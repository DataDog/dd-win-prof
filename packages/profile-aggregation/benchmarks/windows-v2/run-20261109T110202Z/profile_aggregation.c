/* Copyright 2026 Datadog, Inc. SPDX-License-Identifier: Apache-2.0 */
#include "profile_aggregation.h"

#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "pb_encode.h"
#include "rapidhash.h"
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"
#ifdef PA_BENCH_COMPRESSION_HOOKS
#include "compression_hooks.h"
#endif

enum { STRING = 1, MAPPING, FUNCTION, LOCATION, STACK, LABELS };
typedef struct {
  const void *data;
  size_t size;
  uint32_t kind;
} key;
static uint64_t hash_key(key k) {
#ifdef PA_TEST_COLLISIONS
  (void)k;
  return 1;
#else
  return rapidhash_withSeed(k.data, k.size, k.kind);
#endif
}
static bool equal_key(key a, key b) {
  return a.kind == b.kind && a.size == b.size &&
         (!a.size || !memcmp(a.data, b.data, a.size));
}
static void *allocate(pa_profile *, size_t);
static void release(pa_profile *, void *, size_t);
static void *map_alloc(size_t n, pa_profile **p) { return allocate(*p, n); }
static void map_free(void *v, size_t n, pa_profile **p) { release(*p, v, n); }
#define NAME intern_map
#define KEY_TY key
#define VAL_TY uint32_t
#define HASH_FN hash_key
#define CMPR_FN equal_key
#define CTX_TY pa_profile *
#define MALLOC_FN map_alloc
#define FREE_FN map_free
#include "verstable.h"

typedef struct arena {
  struct arena *next;
  size_t used, capacity;
  uint64_t data[1];
} arena;
typedef struct {
  key k;
  uint32_t wire;
} entry;
typedef struct {
  pa_id stack, labels;
} sample_key;
static uint64_t sample_hash(sample_key k) { return rapidhash(&k, sizeof(k)); }
static bool sample_equal(sample_key a, sample_key b) {
  return a.stack == b.stack && a.labels == b.labels;
}
#define NAME sample_map
#define KEY_TY sample_key
#define VAL_TY size_t
#define HASH_FN sample_hash
#define CMPR_FN sample_equal
#define CTX_TY pa_profile *
#define MALLOC_FN map_alloc
#define FREE_FN map_free
#include "verstable.h"

typedef struct {
  pa_id type, unit;
} value_type;
/* All members have eight-byte alignment; values immediately follow this header. */
typedef struct {
  pa_id stack, labels;
  int64_t timestamp;
  uint64_t has_timestamp;
} observation;
struct pa_profile {
  pa_allocator allocator;
  intern_map interns;
  sample_map sums;
  arena *arena;
  entry *entries;
  size_t entry_count, entry_capacity;
  uint32_t generation, string_count;
  uint8_t *observations;
  size_t count, capacity, stride, type_count;
  /* Config is kept independently of generation-scoped interned strings. */
  char *config;
  size_t config_size;
  value_type *types;
  size_t types_size;
  int64_t period;
  pa_id timestamp_key;
};
static void *default_alloc(void *c, size_t n) {
  (void)c;
  return malloc(n);
}
static void default_free(void *c, void *p, size_t n) {
  (void)c;
  (void)n;
  free(p);
}
static void *allocate(pa_profile *p, size_t n) {
  return p->allocator.alloc(p->allocator.context, n);
}
static void release(pa_profile *p, void *v, size_t n) {
  if (v) p->allocator.free(p->allocator.context, v, n);
}
static bool grow(
    pa_profile *p, void **data, size_t *capacity, size_t need, size_t element
) {
  size_t cap = *capacity ? *capacity : 16;
  if (need <= *capacity) return true;
  while (cap < need) {
    if (cap > SIZE_MAX / 2) return false;
    cap *= 2;
  }
  if (cap > SIZE_MAX / element) return false;
  void *v = allocate(p, cap * element);
  if (!v) return false;
  if (*data) memcpy(v, *data, *capacity * element);
  release(p, *data, *capacity * element);
  *data = v;
  *capacity = cap;
  return true;
}
static void *arena_alloc(pa_profile *p, size_t n) {
  if (n > SIZE_MAX - 7) return NULL;
  n = (n + 7) & ~(size_t)7;
  arena *a = p->arena;
  if (!a || n > a->capacity - a->used) {
    size_t cap = n > 16384 ? n : 16384;
    if (cap > SIZE_MAX - sizeof(arena)) return NULL;
    a = allocate(p, sizeof(arena) + cap);
    if (!a) return NULL;
    a->next = p->arena;
    a->used = 0;
    a->capacity = cap;
    p->arena = a;
  }
  void *v = (uint8_t *)a->data + a->used;
  a->used += n;
  return v;
}
static pa_id make_id(pa_profile *p, uint32_t i) {
  return ((uint64_t)p->generation << 32) | (i + 1);
}
static entry *lookup(pa_profile *p, pa_id id, uint32_t kind) {
  uint32_t i = (uint32_t)id;
  if (!id || (id >> 32) != p->generation || !i || i > p->entry_count) return NULL;
  entry *e = &p->entries[i - 1];
  return e->k.kind == kind ? e : NULL;
}
static bool valid(pa_profile *p, pa_id id, uint32_t kind) {
  return !id || lookup(p, id, kind) != NULL;
}
static uint64_t wire(pa_profile *p, pa_id id) {
  return id ? p->entries[(uint32_t)id - 1].wire : 0;
}
static pa_status intern(
    pa_profile *p, uint32_t kind, const void *data, size_t n, pa_id *out
) {
  if (!p || !out || (!data && n)) return PA_INVALID;
  key k = {data, n, kind};
  intern_map_itr it = intern_map_get(&p->interns, k);
  if (!intern_map_is_end(it)) {
    *out = make_id(p, it.data->val);
    return PA_OK;
  }
  if (p->entry_count >= UINT32_MAX - 1) return PA_LIMIT;
  if (!grow(
          p, (void **)&p->entries, &p->entry_capacity, p->entry_count + 1, sizeof(entry)
      ))
    return PA_NOMEM;
  arena *previous_arena = p->arena;
  size_t previous_used = previous_arena ? previous_arena->used : 0;
  void *copy = arena_alloc(p, n ? n : 1);
  if (!copy) return PA_NOMEM;
  if (n) memcpy(copy, data, n);
  k.data = copy;
  it = intern_map_insert(&p->interns, k, (uint32_t)p->entry_count);
  if (intern_map_is_end(it)) {
    if (p->arena != previous_arena) {
      arena *discard = p->arena;
      p->arena = previous_arena;
      release(p, discard, sizeof(arena) + discard->capacity);
    } else if (previous_arena)
      previous_arena->used = previous_used;
    return PA_NOMEM;
  }
  entry *e = &p->entries[p->entry_count];
  e->k = k;
  e->wire = kind == STRING ? ++p->string_count : (uint32_t)p->entry_count + 1;
  *out = make_id(p, (uint32_t)p->entry_count++);
  return PA_OK;
}
pa_status pa_intern_string(pa_profile *p, const char *s, size_t n, pa_id *out) {
  if (!p || !out || (!s && n)) return PA_INVALID;
  if (!n) {
    *out = 0;
    return PA_OK;
  }
  return intern(p, STRING, s, n, out);
}
pa_status pa_intern_mapping(pa_profile *p, const pa_mapping *m, pa_id *out) {
  if (!p || !m || !valid(p, m->filename, STRING) || !valid(p, m->build_id, STRING) ||
      m->memory_limit < m->memory_start)
    return PA_INVALID;
  return intern(p, MAPPING, m, sizeof(*m), out);
}
pa_status pa_intern_function(pa_profile *p, const pa_function *f, pa_id *out) {
  if (!p || !f || !valid(p, f->name, STRING) || !valid(p, f->system_name, STRING) ||
      !valid(p, f->filename, STRING))
    return PA_INVALID;
  return intern(p, FUNCTION, f, sizeof(*f), out);
}
pa_status pa_intern_location(pa_profile *p, const pa_location *l, pa_id *out) {
  if (!p || !l || !valid(p, l->mapping, MAPPING) || !valid(p, l->function, FUNCTION))
    return PA_INVALID;
  return intern(p, LOCATION, l, sizeof(*l), out);
}
pa_status pa_intern_stack(pa_profile *p, const pa_id *ids, size_t n, pa_id *out) {
  if (!p || (!ids && n) || n > SIZE_MAX / sizeof(pa_id)) return PA_INVALID;
  for (size_t i = 0; i < n; i++)
    if (!lookup(p, ids[i], LOCATION)) return PA_INVALID;
  return intern(p, STACK, ids, n * sizeof(pa_id), out);
}
pa_status pa_intern_labels(
    pa_profile *p, const pa_label *labels, size_t n, pa_id *out
) {
  if (!p || !out || (!labels && n) || n > SIZE_MAX / sizeof(pa_label))
    return PA_INVALID;
  for (size_t i = 0; i < n; i++) {
    const pa_label *l = &labels[i];
    if (!valid(p, l->key, STRING) || !valid(p, l->str, STRING) ||
        !valid(p, l->num_unit, STRING) || l->is_numeric > 1 ||
        (l->is_numeric ? l->str != 0 : (l->num != 0 || l->num_unit != 0)))
      return PA_INVALID;
  }
  /* Do not hash caller padding. Canonical labels are arrays of five uint64s. */
  size_t bytes = n * 5 * sizeof(uint64_t);
  uint64_t local[40];
  uint64_t *v = bytes > sizeof(local) ? allocate(p, bytes) : local;
  if (!v) return PA_NOMEM;
  for (size_t i = 0; i < n; i++) {
    v[5 * i] = labels[i].key;
    v[5 * i + 1] = labels[i].str;
    v[5 * i + 2] = labels[i].num_unit;
    v[5 * i + 3] = (uint64_t)labels[i].num;
    v[5 * i + 4] = labels[i].is_numeric;
  }
  pa_status s = intern(p, LABELS, v, bytes, out);
  if (v != local) release(p, v, bytes);
  return s;
}
static void clear(pa_profile *p) {
  intern_map_cleanup(&p->interns);
  intern_map_init(&p->interns, p);
  sample_map_cleanup(&p->sums);
  sample_map_init(&p->sums, p);
  while (p->arena) {
    arena *a = p->arena;
    p->arena = a->next;
    release(p, a, sizeof(arena) + a->capacity);
  }
  release(p, p->entries, p->entry_capacity * sizeof(entry));
  p->entries = NULL;
  p->entry_count = p->entry_capacity = 0;
  release(p, p->observations, p->capacity * p->stride);
  p->observations = NULL;
  p->count = p->capacity = 0;
  p->string_count = 0;
  p->timestamp_key = 0;
}
pa_status pa_reset(pa_profile *p) {
  if (!p) return PA_INVALID;
  /* Never wrap and accidentally validate old IDs. */
  if (p->generation == UINT32_MAX) return PA_LIMIT;
  clear(p);
  ++p->generation;
  return PA_OK;
}
void pa_destroy(pa_profile *p) {
  if (!p) return;
  clear(p);
  release(p, p->config, p->config_size);
  release(p, p->types, p->types_size);
  pa_allocator a = p->allocator;
  a.free(a.context, p, sizeof(*p));
}
pa_status pa_create(
    const pa_value_type *types,
    size_t n,
    int64_t period,
    const pa_allocator *a,
    pa_profile **out
) {
  if (!out) return PA_INVALID;
  *out = NULL;
  if (!types || !n || n > (SIZE_MAX - sizeof(observation)) / sizeof(int64_t) ||
      n > SIZE_MAX / sizeof(value_type) || period < 0 || (a && (!a->alloc || !a->free)))
    return PA_INVALID;
  size_t bytes = 0;
  for (size_t i = 0; i < n; i++) {
    if (!types[i].type || !types[i].unit) return PA_INVALID;
    size_t t = strlen(types[i].type), u = strlen(types[i].unit);
    if (t > SIZE_MAX - u - 2 || bytes > SIZE_MAX - t - u - 2) return PA_LIMIT;
    bytes += t + u + 2;
  }
  pa_allocator alloc = a ? *a : (pa_allocator){NULL, default_alloc, default_free};
  pa_profile *p = alloc.alloc(alloc.context, sizeof(*p));
  if (!p) return PA_NOMEM;
  memset(p, 0, sizeof(*p));
  p->allocator = alloc;
  p->generation = 1;
  intern_map_init(&p->interns, p);
  sample_map_init(&p->sums, p);
  p->stride = sizeof(observation) + n * sizeof(int64_t);
  p->type_count = n;
  p->period = period;
  p->config_size = bytes;
  p->types_size = n * sizeof(value_type);
  p->config = allocate(p, bytes);
  p->types = allocate(p, p->types_size);
  if (!p->config || !p->types) {
    pa_destroy(p);
    return PA_NOMEM;
  }
  char *s = p->config;
  for (size_t i = 0; i < n; i++) {
    size_t t = strlen(types[i].type) + 1, u = strlen(types[i].unit) + 1;
    memcpy(s, types[i].type, t);
    s += t;
    memcpy(s, types[i].unit, u);
    s += u;
  }
  *out = p;
  return PA_OK;
}
static int64_t add_saturating(int64_t a, int64_t b) {
  if (b > 0 && a > INT64_MAX - b) return INT64_MAX;
  if (b < 0 && a < INT64_MIN - b) return INT64_MIN;
  return a + b;
}
pa_status pa_record(
    pa_profile *p,
    pa_id stack,
    pa_id labels,
    const int64_t *values,
    size_t n,
    int has_timestamp,
    int64_t timestamp
) {
  if (!p || !values || n != p->type_count ||
      (has_timestamp != 0 && has_timestamp != 1) || !lookup(p, stack, STACK) ||
      !lookup(p, labels, LABELS))
    return PA_INVALID;
  sample_key k = {stack, labels};
  if (!has_timestamp) {
    sample_map_itr it = sample_map_get(&p->sums, k);
    if (!sample_map_is_end(it)) {
      int64_t *v =
          (int64_t *)(p->observations + it.data->val * p->stride + sizeof(observation));
      for (size_t i = 0; i < n; i++) v[i] = add_saturating(v[i], values[i]);
      return PA_OK;
    }
  }
  if (p->count == SIZE_MAX ||
      !grow(p, (void **)&p->observations, &p->capacity, p->count + 1, p->stride))
    return PA_NOMEM;
  if (!has_timestamp && sample_map_is_end(sample_map_insert(&p->sums, k, p->count)))
    return PA_NOMEM;
  observation *o = (observation *)(p->observations + p->count * p->stride);
  o->stack = stack;
  o->labels = labels;
  o->timestamp = timestamp;
  o->has_timestamp = (uint64_t)has_timestamp;
  memcpy(o + 1, values, n * sizeof(int64_t));
  ++p->count;
  return PA_OK;
}

/* Low-level nanopb encoding keeps arbitrary strings/stacks out of fixed-size
 * generated structs. Submessage lengths use nanopb's sizing stream, not a buffer. */
static bool field(pb_ostream_t *s, uint32_t tag, uint64_t v) {
  return pb_encode_tag(s, PB_WT_VARINT, tag) && pb_encode_varint(s, v);
}
typedef bool (*emit_fn)(pb_ostream_t *, pa_profile *, const void *);
static bool message(
    pb_ostream_t *s, uint32_t tag, emit_fn f, pa_profile *p, const void *v
) {
  pb_ostream_t size = PB_OSTREAM_SIZING;
  return f(&size, p, v) && pb_encode_tag(s, PB_WT_STRING, tag) &&
         pb_encode_varint(s, size.bytes_written) && f(s, p, v);
}
static bool emit_type(pb_ostream_t *s, pa_profile *p, const void *v) {
  const value_type *t = v;
  return field(s, 1, wire(p, t->type)) && field(s, 2, wire(p, t->unit));
}
static bool emit_mapping(pb_ostream_t *s, pa_profile *p, const void *v) {
  const entry *e = v;
  const pa_mapping *m = e->k.data;
  return field(s, 1, e->wire) && field(s, 2, m->memory_start) &&
         field(s, 3, m->memory_limit) && field(s, 4, m->file_offset) &&
         field(s, 5, wire(p, m->filename)) && field(s, 6, wire(p, m->build_id));
}
static bool emit_function(pb_ostream_t *s, pa_profile *p, const void *v) {
  const entry *e = v;
  const pa_function *f = e->k.data;
  return field(s, 1, e->wire) && field(s, 2, wire(p, f->name)) &&
         field(s, 3, wire(p, f->system_name)) && field(s, 4, wire(p, f->filename)) &&
         field(s, 5, (uint64_t)f->start_line);
}
static bool emit_line(pb_ostream_t *s, pa_profile *p, const void *v) {
  const pa_location *l = v;
  return field(s, 1, wire(p, l->function)) && field(s, 2, (uint64_t)l->line);
}
static bool emit_location(pb_ostream_t *s, pa_profile *p, const void *v) {
  const entry *e = v;
  const pa_location *l = e->k.data;
  return field(s, 1, e->wire) && field(s, 2, wire(p, l->mapping)) &&
         field(s, 3, l->address) && (!l->function || message(s, 4, emit_line, p, l));
}
static bool emit_label(pb_ostream_t *s, pa_profile *p, const void *v) {
  const uint64_t *l = v;
  return field(s, 1, wire(p, l[0])) &&
         (l[4] ? (field(s, 3, l[3]) && field(s, 4, wire(p, l[2])))
               : field(s, 2, wire(p, l[1])));
}
static bool emit_sample(pb_ostream_t *s, pa_profile *p, const void *v) {
  const observation *o = v;
  entry *stack = lookup(p, o->stack, STACK), *labels = lookup(p, o->labels, LABELS);
  const pa_id *ids = stack->k.data;
  const int64_t *values = (const int64_t *)(o + 1);
  /* Unpacked scalar repetitions are wire-compatible with packed pprof fields. */
  for (size_t i = 0; i < stack->k.size / sizeof(pa_id); i++)
    if (!field(s, 1, wire(p, ids[i]))) return false;
  for (size_t i = 0; i < p->type_count; i++)
    if (!field(s, 2, (uint64_t)values[i])) return false;
  const uint64_t *ls = labels->k.data;
  for (size_t i = 0; i < labels->k.size / (5 * sizeof(uint64_t)); i++)
    if (!message(s, 3, emit_label, p, ls + i * 5)) return false;
  if (o->has_timestamp) {
    uint64_t ts[5] = {p->timestamp_key, 0, 0, (uint64_t)o->timestamp, 1};
    if (!message(s, 3, emit_label, p, ts)) return false;
  }
  return true;
}
typedef struct {
  ZSTD_CCtx *zstd;
  pa_sink sink;
  void *context;
  pa_status status;
  size_t used;
  uint8_t input[16384], output[16384];
} compressor;
static bool compress_chunk(compressor *c, ZSTD_EndDirective mode) {
  ZSTD_inBuffer in = {c->input, c->used, 0};
  size_t remaining;
  do {
    ZSTD_outBuffer out = {c->output, sizeof(c->output), 0};
#ifdef PA_BENCH_COMPRESSION_HOOKS
    uint64_t compression_start = pa_bench_compression_begin();
#endif
    remaining = ZSTD_compressStream2(c->zstd, &out, &in, mode);
#ifdef PA_BENCH_COMPRESSION_HOOKS
    pa_bench_compression_end(compression_start);
#endif
    if (ZSTD_isError(remaining)) {
      c->status = PA_COMPRESSION;
      return false;
    }
    if (out.pos && c->sink(c->context, c->output, out.pos) != PA_OK) {
      c->status = PA_OUTPUT;
      return false;
    }
  } while (in.pos < in.size || (mode == ZSTD_e_end && remaining));
  c->used = 0;
  return true;
}
static bool compressed_write(pb_ostream_t *s, const pb_byte_t *bytes, size_t n) {
  compressor *c = s->state;
  while (n) {
    size_t amount = sizeof(c->input) - c->used;
    if (amount > n) amount = n;
    memcpy(c->input + c->used, bytes, amount);
    c->used += amount;
    bytes += amount;
    n -= amount;
    if (c->used == sizeof(c->input) && !compress_chunk(c, ZSTD_e_continue))
      return false;
  }
  return true;
}
pa_status pa_serialize(
    pa_profile *p, int64_t start, int64_t end, pa_sink sink, void *context
) {
  if (!p || !sink || end < start || (start < 0 && end > INT64_MAX + start))
    return PA_INVALID;
  const char *config = p->config;
  for (size_t i = 0; i < p->type_count; i++) {
    pa_status st = pa_intern_string(p, config, strlen(config), &p->types[i].type);
    if (st != PA_OK) return st;
    config += strlen(config) + 1;
    st = pa_intern_string(p, config, strlen(config), &p->types[i].unit);
    if (st != PA_OK) return st;
    config += strlen(config) + 1;
  }
  pa_status st = pa_intern_string(p, "end_timestamp_ns", 16, &p->timestamp_key);
  if (st != PA_OK) return st;
  ZSTD_compressionParameters params = ZSTD_getCParams(1, 0, 0);
  params.windowLog = 17;
  size_t workspace_size = ZSTD_estimateCStreamSize_usingCParams(params);
  if (ZSTD_isError(workspace_size)) return PA_COMPRESSION;
  void *workspace = allocate(p, workspace_size);
  if (!workspace) return PA_NOMEM;
  compressor c = {0};
  c.sink = sink;
  c.context = context;
  c.zstd = ZSTD_initStaticCCtx(workspace, workspace_size);
  if (!c.zstd ||
      ZSTD_isError(ZSTD_CCtx_setParameter(c.zstd, ZSTD_c_compressionLevel, 1)) ||
      ZSTD_isError(ZSTD_CCtx_setParameter(c.zstd, ZSTD_c_windowLog, 17))) {
    release(p, workspace, workspace_size);
    return PA_COMPRESSION;
  }
  pb_ostream_t s = {compressed_write, &c, SIZE_MAX, 0, NULL};
  bool ok = true;
  for (size_t i = 0; i < p->type_count && ok; i++)
    ok = message(&s, 1, emit_type, p, &p->types[i]);
  for (size_t i = 0; i < p->entry_count && ok; i++) {
    entry *e = &p->entries[i];
    switch (e->k.kind) {
      case MAPPING:
        ok = message(&s, 3, emit_mapping, p, e);
        break;
      case FUNCTION:
        ok = message(&s, 5, emit_function, p, e);
        break;
      case LOCATION:
        ok = message(&s, 4, emit_location, p, e);
        break;
      default:
        break;
    }
  }
  /* The string table starts with the mandatory empty string at index zero. */
  ok = ok && pb_encode_tag(&s, PB_WT_STRING, 6) &&
       pb_encode_string(&s, (const pb_byte_t *)"", 0);
  for (size_t i = 0; i < p->entry_count && ok; i++) {
    entry *e = &p->entries[i];
    if (e->k.kind == STRING)
      ok = pb_encode_tag(&s, PB_WT_STRING, 6) &&
           pb_encode_string(&s, e->k.data, e->k.size);
  }
  ok = ok && field(&s, 9, (uint64_t)start) && field(&s, 10, (uint64_t)(end - start)) &&
       message(&s, 11, emit_type, p, &p->types[0]) &&
       field(&s, 12, (uint64_t)p->period);
  for (size_t i = 0; i < p->count && ok; i++)
    ok = message(&s, 2, emit_sample, p, p->observations + i * p->stride);
  if (ok) ok = compress_chunk(&c, ZSTD_e_end);
#ifdef PA_BENCH_COMPRESSION_HOOKS
  if (ok) pa_bench_serialized_bytes(s.bytes_written);
#endif
  release(p, workspace, workspace_size);
  return ok ? PA_OK : (c.status != PA_OK ? c.status : PA_LIMIT);
}
