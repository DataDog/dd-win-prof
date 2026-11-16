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
  size_t used, capacity, live;
  uint64_t data[1];
} arena;
/* Cache values never escape map operations: insert/erase may relocate them. */
typedef struct {
  arena *block;
  uint32_t generation, index;
  uint8_t age;
  bool used;
} cached_string;
#define NAME string_map
#define KEY_TY key
#define VAL_TY cached_string
#define HASH_FN hash_key
#define CMPR_FN equal_key
#define CTX_TY pa_profile *
#define MALLOC_FN map_alloc
#define FREE_FN map_free
#include "verstable.h"

enum {
  ENTRY_FLOOR = 1024,
  SUM_FLOOR = 256,
  STRING_FLOOR = 256,
  ARENA_FLOOR = 16384,
  OBS_FLOOR = 8192,
  OBS_BYTES = 384 * 1024
};
typedef struct {
  size_t observations, entries, sums, arena_bytes;
} demand;
typedef struct {
  key k;
  /* STACK caches its packed size; other kinds carry a wire ID. */
  union {
    uint32_t wire;
    size_t packed_stack_size;
  };
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
  arena *arena, *arena_cursor;
  size_t arena_used;
  string_map strings;
  arena *string_blocks, *string_cursor;
  bool successful_output_pending, maintain_strings;
  demand recent[3];
  unsigned recent_slot;
  void *workspace;
  size_t workspace_size;
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
  size_t cap = *capacity ? *capacity : 1;
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
static size_t aligned_size(size_t n) {
  return n > SIZE_MAX - 7 ? 0 : (n + 7) & ~(size_t)7;
}
static void free_blocks(pa_profile *p, arena *a) {
  while (a) {
    arena *next = a->next;
    release(p, a, sizeof(arena) + a->capacity);
    a = next;
  }
}
/* Lists have no allocation-order dependency. A transient cursor skips exhausted
 * retained blocks, including when a large request needs a fresh block. */
static void *block_alloc(
    pa_profile *p, arena **head, arena **cursor, size_t n, arena **owner
) {
  n = aligned_size(n ? n : 1);
  if (!n) return NULL;
  arena *a = cursor && *cursor ? *cursor : *head;
  arena *last = NULL;
  while (a && n > a->capacity - a->used) {
    last = a;
    a = a->next;
  }
  if (!a) {
    size_t cap = n > ARENA_FLOOR ? n : ARENA_FLOOR;
    if (cap > SIZE_MAX - sizeof(arena)) return NULL;
    a = allocate(p, sizeof(arena) + cap);
    if (!a) return NULL;
    a->used = a->live = 0;
    a->capacity = cap;
    a->next = NULL;
    if (last)
      last->next = a;
    else
      *head = a;
  }
  if (cursor) *cursor = a;
  *owner = a;
  void *v = (uint8_t *)a->data + a->used;
  a->used += n;
  a->live += n;
  return v;
}
static void rollback_payload(
    pa_profile *p, arena **head, arena **cursor, arena *owner, size_t n
) {
  n = aligned_size(n ? n : 1);
  owner->used -= n;
  owner->live -= n;
  /* Failed table growth must not retain a succession of larger fitted blocks.
   * Ordinary empty blocks remain useful spare capacity for a later retry. */
  if (!owner->live && !owner->used && owner->capacity > ARENA_FLOOR) {
    arena **link = head;
    while (*link != owner) link = &(*link)->next;
    *link = owner->next;
    if (*cursor == owner) *cursor = owner->next;
    release(p, owner, sizeof(arena) + owner->capacity);
  }
}
/* Optional maintenance runs once after reset, before any generation-local
 * pointers exist. Build both replacement map and payload before committing. */
static void maintain_string_cache(pa_profile *p) {
  if (!p->maintain_strings) return;
  p->maintain_strings = false;
  string_map replacement;
  string_map_init(&replacement, p);
  arena *blocks = NULL, *cursor = NULL;
  size_t count = string_map_size(&p->strings);
  if (!string_map_reserve(&replacement, count > STRING_FLOOR ? count : STRING_FLOOR))
    return;
  for (string_map_itr it = string_map_first(&p->strings); !string_map_is_end(it);
       it = string_map_next(it)) {
    key k = it.data->key;
    cached_string value = it.data->val;
    void *copy = block_alloc(p, &blocks, &cursor, k.size, &value.block);
    if (!copy) goto fail;
    memcpy(copy, k.data, k.size);
    k.data = copy;
    if (string_map_is_end(string_map_insert(&replacement, k, value))) goto fail;
  }
  string_map_cleanup(&p->strings);
  free_blocks(p, p->string_blocks);
  p->strings = replacement;
  p->string_blocks = blocks;
  p->string_cursor = cursor;
  return;
fail:
  string_map_cleanup(&replacement);
  free_blocks(p, blocks);
}
static size_t observation_floor(pa_profile *p) {
  size_t floor = OBS_BYTES / p->stride;
  if (!floor) floor = 1;
  /* grow uses powers of two. Round down so large value counts stay capped. */
  size_t cap = 1;
  while (cap < OBS_FLOOR && cap <= floor / 2) cap *= 2;
  return cap;
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
  maintain_string_cache(p);
  key k = {data, n, kind};
  uint32_t index = 0;
  if (kind == STRING) {
    string_map_itr hit = string_map_get(&p->strings, k);
    if (!string_map_is_end(hit) && hit.data->val.generation == p->generation) {
      *out = make_id(p, hit.data->val.index);
      return PA_OK;
    }
  } else {
    intern_map_itr hit = intern_map_get(&p->interns, k);
    if (!intern_map_is_end(hit)) {
      *out = make_id(p, hit.data->val);
      return PA_OK;
    }
  }
  if (p->entry_count >= UINT32_MAX - 1) return PA_LIMIT;
  if (!grow(
          p,
          (void **)&p->entries,
          &p->entry_capacity,
          p->entry_count ? p->entry_count + 1 : ENTRY_FLOOR,
          sizeof(entry)
      ))
    return PA_NOMEM;
  index = (uint32_t)p->entry_count;
  if (kind == STRING) {
    string_map_itr it = string_map_get(&p->strings, k);
    if (string_map_is_end(it)) {
      if (!string_map_reserve(&p->strings, STRING_FLOOR)) return PA_NOMEM;
      arena *owner;
      void *copy = block_alloc(p, &p->string_blocks, &p->string_cursor, n, &owner);
      if (!copy) return PA_NOMEM;
      memcpy(copy, data, n);
      k.data = copy;
      cached_string value = {owner, p->generation, index, 0, false};
      it = string_map_insert(&p->strings, k, value);
      if (string_map_is_end(it)) {
        rollback_payload(p, &p->string_blocks, &p->string_cursor, owner, n);
        return PA_NOMEM;
      }
    }
    it.data->val.generation = p->generation;
    it.data->val.index = index;
    k = it.data->key;
  } else {
    if (!intern_map_reserve(&p->interns, ENTRY_FLOOR)) return PA_NOMEM;
    arena *owner;
    void *copy = block_alloc(p, &p->arena, &p->arena_cursor, n, &owner);
    if (!copy) return PA_NOMEM;
    if (n) memcpy(copy, data, n);
    k.data = copy;
    if (intern_map_is_end(intern_map_insert(&p->interns, k, index))) {
      rollback_payload(p, &p->arena, &p->arena_cursor, owner, n);
      return PA_NOMEM;
    }
    p->arena_used += aligned_size(n ? n : 1);
  }
  entry *e = &p->entries[p->entry_count];
  e->k = k;
  if (kind == STACK)
    e->packed_stack_size = SIZE_MAX;
  else
    e->wire = kind == STRING ? ++p->string_count : index + 1;
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
static size_t maximum(size_t a, size_t b) { return a > b ? a : b; }
static bool excessive(size_t capacity, size_t need) {
  return need <= SIZE_MAX / 4 && capacity > 4 * need;
}
static void finish_strings(pa_profile *p) {
  for (string_map_itr it = string_map_first(&p->strings); !string_map_is_end(it);) {
    cached_string *v = &it.data->val;
    if (p->successful_output_pending) {
      v->age = v->used ? 0 : (uint8_t)(v->age + 1);
      v->used = false;
    }
    /* Generation-local entries will be discarded before relocation. */
    v->generation = 0;
    if (v->age == 3) {
      v->block->live -= aligned_size(it.data->key.size);
      it = string_map_erase_itr(&p->strings, it);
    } else
      it = string_map_next(it);
  }
  size_t capacity = 0, live = 0;
  arena **link = &p->string_blocks;
  bool kept_empty = false;
  while (*link) {
    arena *a = *link;
    if (!a->live) {
      a->used = 0;
      if (kept_empty || a->capacity > ARENA_FLOOR) {
        *link = a->next;
        release(p, a, sizeof(arena) + a->capacity);
        continue;
      }
      kept_empty = true;
    }
    capacity += a->capacity;
    live += a->live;
    link = &a->next;
  }
  p->string_cursor = p->string_blocks;
  size_t count = maximum(STRING_FLOOR, string_map_size(&p->strings));
  p->maintain_strings =
      (capacity > 65536 && live < capacity / 2 && capacity - live >= 65536) ||
      (count <= SIZE_MAX / 8 && string_map_bucket_count(&p->strings) > count * 8);
}
pa_status pa_reset(pa_profile *p) {
  if (!p) return PA_INVALID;
  /* Never wrap and accidentally validate old IDs. */
  if (p->generation == UINT32_MAX) return PA_LIMIT;
  finish_strings(p);
  if (p->successful_output_pending) {
    p->recent[p->recent_slot] =
        (demand){p->count, p->entry_count, sample_map_size(&p->sums), p->arena_used};
    p->recent_slot = (p->recent_slot + 1) % 3;
  }
  demand keep = {observation_floor(p), ENTRY_FLOOR, SUM_FLOOR, ARENA_FLOOR};
  for (size_t i = 0; i < 3; i++) {
    keep.observations = maximum(keep.observations, p->recent[i].observations);
    keep.entries = maximum(keep.entries, p->recent[i].entries);
    keep.sums = maximum(keep.sums, p->recent[i].sums);
    keep.arena_bytes = maximum(keep.arena_bytes, p->recent[i].arena_bytes);
  }
  intern_map_clear(&p->interns);
  sample_map_clear(&p->sums);
  if (keep.entries <= SIZE_MAX / 8 &&
      intern_map_bucket_count(&p->interns) > keep.entries * 8) {
    intern_map_cleanup(&p->interns);
    intern_map_init(&p->interns, p);
  }
  if (keep.sums <= SIZE_MAX / 8 && sample_map_bucket_count(&p->sums) > keep.sums * 8) {
    sample_map_cleanup(&p->sums);
    sample_map_init(&p->sums, p);
  }
  if (excessive(p->entry_capacity, keep.entries)) {
    release(p, p->entries, p->entry_capacity * sizeof(entry));
    p->entries = NULL;
    p->entry_capacity = 0;
  }
  if (excessive(p->capacity, keep.observations)) {
    release(p, p->observations, p->capacity * p->stride);
    p->observations = NULL;
    p->capacity = 0;
  }
  size_t retained = 0;
  arena **link = &p->arena;
  while (*link) {
    arena *a = *link;
    if (excessive(a->capacity, keep.arena_bytes) ||
        excessive(retained, keep.arena_bytes)) {
      *link = a->next;
      release(p, a, sizeof(arena) + a->capacity);
      continue;
    }
    retained += a->capacity;
    a->used = a->live = 0;
    link = &a->next;
  }
  p->arena_cursor = p->arena;
  p->arena_used = p->entry_count = p->count = p->string_count = 0;
  p->timestamp_key = 0;
  p->successful_output_pending = false;
  ++p->generation;
  return PA_OK;
}
void pa_destroy(pa_profile *p) {
  if (!p) return;
  intern_map_cleanup(&p->interns);
  sample_map_cleanup(&p->sums);
  string_map_cleanup(&p->strings);
  free_blocks(p, p->arena);
  free_blocks(p, p->string_blocks);
  release(p, p->entries, p->entry_capacity * sizeof(entry));
  release(p, p->observations, p->capacity * p->stride);
  release(p, p->workspace, p->workspace_size);
  release(p, p->config, p->config_size);
  release(p, p->types, p->types_size);
  pa_allocator a = p->allocator;
  a.free(a.context, p, sizeof(*p));
}
static pa_status prepare_workspace(pa_profile *p) {
  if (p->workspace) return PA_OK;
  ZSTD_compressionParameters params = ZSTD_getCParams(1, 0, 0);
  params.windowLog = 17;
  size_t size = ZSTD_estimateCStreamSize_usingCParams(params);
  if (ZSTD_isError(size)) return PA_COMPRESSION;
  p->workspace = allocate(p, size);
  if (!p->workspace) return PA_NOMEM;
  p->workspace_size = size;
  return PA_OK;
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
  string_map_init(&p->strings, p);
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
  /* Normal floors are paid at creation, not on the first sampled record. */
  arena *owner;
  if (!grow(p, (void **)&p->entries, &p->entry_capacity, ENTRY_FLOOR, sizeof(entry)) ||
      !grow(
          p, (void **)&p->observations, &p->capacity, observation_floor(p), p->stride
      ) ||
      !intern_map_reserve(&p->interns, ENTRY_FLOOR) ||
      !sample_map_reserve(&p->sums, SUM_FLOOR) ||
      !string_map_reserve(&p->strings, STRING_FLOOR) ||
      !block_alloc(p, &p->arena, &p->arena_cursor, 1, &owner)) {
    pa_destroy(p);
    return PA_NOMEM;
  }
  p->arena->used = p->arena->live = 0;
  if (!block_alloc(p, &p->string_blocks, &p->string_cursor, 1, &owner)) {
    pa_destroy(p);
    return PA_NOMEM;
  }
  p->string_blocks->used = p->string_blocks->live = 0;
  pa_status status = prepare_workspace(p);
  if (status != PA_OK) {
    pa_destroy(p);
    return status;
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
  if (p->count == SIZE_MAX || !grow(
                                  p,
                                  (void **)&p->observations,
                                  &p->capacity,
                                  p->count ? p->count + 1 : observation_floor(p),
                                  p->stride
                              ))
    return PA_NOMEM;
  if (!has_timestamp && (!sample_map_reserve(&p->sums, SUM_FLOOR) ||
                         sample_map_is_end(sample_map_insert(&p->sums, k, p->count))))
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
static bool emit_stack(pb_ostream_t *s, pa_profile *p, entry *stack) {
  const pa_id *ids = stack->k.data;
  size_t count = stack->k.size / sizeof(pa_id);
  if (!count) return true;
  if (stack->packed_stack_size == SIZE_MAX) {
    size_t bytes = 0;
    for (size_t i = 0; i < count; i++) {
      uint64_t id = wire(p, ids[i]);
      do {
        ++bytes;
        id >>= 7;
      } while (id);
    }
    stack->packed_stack_size = bytes;
  }
  if (!pb_encode_tag(s, PB_WT_STRING, 1) ||
      !pb_encode_varint(s, stack->packed_stack_size))
    return false;
  if (!s->callback) return pb_write(s, NULL, stack->packed_stack_size);
  for (size_t i = 0; i < count; i++)
    if (!pb_encode_varint(s, wire(p, ids[i]))) return false;
  return true;
}
static bool emit_sample(pb_ostream_t *s, pa_profile *p, const void *v) {
  const observation *o = v;
  entry *stack = lookup(p, o->stack, STACK), *labels = lookup(p, o->labels, LABELS);
  const int64_t *values = (const int64_t *)(o + 1);
  if (!emit_stack(s, p, stack)) return false;
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
  st = prepare_workspace(p);
  if (st != PA_OK) return st;
  compressor c = {0};
  c.sink = sink;
  c.context = context;
  c.zstd = ZSTD_initStaticCCtx(p->workspace, p->workspace_size);
  if (!c.zstd ||
      ZSTD_isError(ZSTD_CCtx_setParameter(c.zstd, ZSTD_c_compressionLevel, 1)) ||
      ZSTD_isError(ZSTD_CCtx_setParameter(c.zstd, ZSTD_c_windowLog, 17))) {
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
  if (ok) {
    /* Only strings in this completed frame count. A later failed retry must not
     * mark newly interned strings used or consume another export cycle. */
    for (size_t i = 0; i < p->entry_count; i++) {
      entry *e = &p->entries[i];
      if (e->k.kind == STRING) {
        string_map_itr it = string_map_get(&p->strings, e->k);
        it.data->val.used = true;
      }
    }
    p->successful_output_pending = true;
  }
  return ok ? PA_OK : (c.status != PA_OK ? c.status : PA_LIMIT);
}
