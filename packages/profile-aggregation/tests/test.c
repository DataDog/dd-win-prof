/* Copyright 2026 Datadog, Inc. SPDX-License-Identifier: Apache-2.0 */
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "profile_aggregation.h"
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); \
      exit(1);                                                \
    }                                                         \
  } while (0)
#define OK(x) CHECK((x) == PA_OK)
typedef struct {
  size_t calls, live, peak, fail, frees, reject_below;
} allocation;
static void *test_alloc(void *ctx, size_t n) {
  allocation *a = ctx;
  ++a->calls;
  if ((a->fail && a->calls == a->fail) || n < a->reject_below) return NULL;
  void *p = malloc(n);
  if (p) {
    a->live += n;
    if (a->live > a->peak) a->peak = a->live;
  }
  return p;
}
static void test_free(void *ctx, void *p, size_t n) {
  allocation *a = ctx;
  ++a->frees;
  CHECK(a->live >= n);
  a->live -= n;
  free(p);
}
typedef struct {
  uint8_t *bytes;
  size_t n, cap, calls;
  int fail;
} buffer;
static pa_status sink(void *ctx, const uint8_t *v, size_t n) {
  buffer *b = ctx;
  ++b->calls;
  CHECK(n <= 16384);
  if (b->fail) return PA_OUTPUT;
  if (b->n + n > b->cap) {
    b->cap = (b->n + n) * 2;
    b->bytes = realloc(b->bytes, b->cap);
    CHECK(b->bytes);
  }
  memcpy(b->bytes + b->n, v, n);
  b->n += n;
  return PA_OK;
}
/* Independent, minimal protobuf wire decoder, not the nanopb encoder under test. */
typedef struct {
  const uint8_t *p, *end;
} view;
typedef struct {
  unsigned tag;
  uint64_t number;
  view bytes;
} field;
static uint64_t varint(view *v) {
  uint64_t n = 0;
  unsigned shift = 0;
  for (;;) {
    CHECK(v->p < v->end && shift < 70);
    uint8_t b = *v->p++;
    n |= (uint64_t)(b & 127) << shift;
    if (!(b & 128)) return n;
    shift += 7;
  }
}
static int next(view *v, field *f) {
  if (v->p == v->end) return 0;
  uint64_t t = varint(v);
  f->tag = (unsigned)(t >> 3);
  f->number = 0;
  f->bytes = (view){NULL, NULL};
  switch (t & 7) {
    case 0:
      f->number = varint(v);
      break;
    case 2: {
      uint64_t n = varint(v);
      CHECK(n <= (uint64_t)(v->end - v->p));
      f->bytes = (view){v->p, v->p + (size_t)n};
      v->p += (size_t)n;
      break;
    }
    default:
      CHECK(0);
  }
  return 1;
}
static uint64_t number(view v, unsigned tag) {
  field f;
  while (next(&v, &f))
    if (f.tag == tag) return f.number;
  return 0;
}
static view sub(view v, unsigned tag) {
  field f;
  while (next(&v, &f))
    if (f.tag == tag) return f.bytes;
  return (view){NULL, NULL};
}
static size_t fields(view v, unsigned tag) {
  size_t n = 0;
  field f;
  while (next(&v, &f)) n += f.tag == tag;
  return n;
}
static int text(view v, const char *s) {
  return (size_t)(v.end - v.p) == strlen(s) && !memcmp(v.p, s, strlen(s));
}
static view string_at(view profile, uint64_t index) {
  field f;
  while (next(&profile, &f))
    if (f.tag == 6 && index-- == 0) return f.bytes;
  CHECK(0);
  return profile;
}
static view decode(buffer *b, uint8_t **storage) {
  CHECK(b->n >= 4 && !memcmp(b->bytes, "\x28\xb5\x2f\xfd", 4));
  size_t cap = 32 * 1024 * 1024;
  *storage = malloc(cap);
  CHECK(*storage);
  size_t ws = ZSTD_estimateDCtxSize();
  void *workspace = malloc(ws);
  CHECK(workspace);
  ZSTD_DCtx *ctx = ZSTD_initStaticDCtx(workspace, ws);
  CHECK(ctx);
  size_t n = ZSTD_decompressDCtx(ctx, *storage, cap, b->bytes, b->n);
  if (ZSTD_isError(n)) fprintf(stderr, "decompress: %s\n", ZSTD_getErrorName(n));
  CHECK(!ZSTD_isError(n));
  free(workspace);
  return (view){*storage, *storage + n};
}
static pa_id str(pa_profile *p, const char *s) {
  pa_id id = 0;
  OK(pa_intern_string(p, s, strlen(s), &id));
  return id;
}
static void semantics(const char *output) {
  allocation a = {0};
  pa_allocator allocator = {&a, test_alloc, test_free};
  pa_value_type types[] = {{"cpu-time", "nanoseconds"}, {"wall-time", "nanoseconds"}};
  pa_profile *p = NULL;
  OK(pa_create(types, 2, 10000000, &allocator, &p));
  pa_id filename = str(p, "native.dll"), build = str(p, "ABCDEF01234567891"),
        name = str(p, "Worker"), file = str(p, "worker.cpp");
  pa_mapping m = {0x140000000ULL, 0x140100000ULL, 4096, filename, build};
  pa_id mapping = 0;
  OK(pa_intern_mapping(p, &m, &mapping));
  pa_mapping m2 = m;
  ++m2.memory_start;
  ++m2.memory_limit;
  pa_id other = 0;
  OK(pa_intern_mapping(p, &m2, &other));
  CHECK(other != mapping);
  pa_function f = {name, name, file, 42};
  pa_id function = 0;
  OK(pa_intern_function(p, &f, &function));
  pa_location l = {mapping, function, 0x123456789ULL, 43};
  pa_id location = 0;
  OK(pa_intern_location(p, &l, &location));
  pa_id locs[] = {location, location};
  pa_id stack = 0, stack2 = 0, labels = 0;
  OK(pa_intern_stack(p, locs, 2, &stack));
  OK(pa_intern_stack(p, locs, 1, &stack2));
  CHECK(stack != stack2);
  pa_label ls[3] = {{0}};
  ls[0].key = str(p, "process_id");
  ls[0].str = str(p, "123");
  ls[1].key = str(p, "thread id");
  ls[1].num = 0;
  ls[1].is_numeric = 1;
  ls[1].num_unit = str(p, "count");
  ls[2].key = str(p, "rum.view_id");
  ls[2].str = str(p, "view-1");
  OK(pa_intern_labels(p, ls, 3, &labels));
  pa_id again = 0;
  OK(pa_intern_labels(p, ls, 3, &again));
  CHECK(again == labels);
  int64_t values[] = {7, 11};
  OK(pa_record(p, stack, labels, values, 2, 1, 0));
  OK(pa_record(p, stack, labels, values, 2, 1, 1234567890123456789LL));
  OK(pa_record(p, stack, labels, values, 2, 1, 1234567890123456789LL));
  OK(pa_record(p, stack, labels, values, 2, 0, 0));
  OK(pa_record(p, stack, labels, values, 2, 0, 0));
  size_t calls = a.calls;
  OK(pa_record(p, stack, labels, values, 2, 0, 0));
  CHECK(calls == a.calls);
  CHECK(pa_record(p, stack, labels, values, 1, 0, 0) == PA_INVALID);
  buffer b = {0};
  OK(pa_serialize(p, 1000000000, 2000000000, sink, &b));
  uint8_t *decoded = NULL;
  view profile = decode(&b, &decoded);
  CHECK(
      number(profile, 9) == 1000000000 && number(profile, 10) == 1000000000 &&
      number(profile, 12) == 10000000
  );
  CHECK(
      fields(profile, 1) == 2 && fields(profile, 2) == 4 && fields(profile, 3) == 2 &&
      fields(profile, 4) == 1 && fields(profile, 5) == 1
  );
  view mt = sub(profile, 3);
  CHECK(
      number(mt, 2) == m.memory_start && number(mt, 3) == m.memory_limit &&
      number(mt, 4) == 4096
  );
  CHECK(text(string_at(profile, number(mt, 5)), "native.dll"));
  CHECK(text(string_at(profile, number(mt, 6)), "ABCDEF01234567891"));
  view fn = sub(profile, 5);
  CHECK(text(string_at(profile, number(fn, 2)), "Worker"));
  CHECK(number(fn, 2) == number(fn, 3));
  CHECK(text(string_at(profile, number(fn, 4)), "worker.cpp"));
  CHECK(number(fn, 5) == 42);
  view lc = sub(profile, 4);
  CHECK(number(lc, 2) == number(mt, 1) && number(lc, 3) == l.address);
  CHECK(number(sub(lc, 4), 2) == 43);
  CHECK(text(string_at(profile, number(sub(profile, 11), 1)), "cpu-time"));
  view iter = profile;
  field sf;
  size_t sample = 0;
  while (next(&iter, &sf))
    if (sf.tag == 2) {
      CHECK(fields(sf.bytes, 1) == 1 && fields(sf.bytes, 2) == 2);
      view packed = sub(sf.bytes, 1);
      CHECK(packed.p != NULL);
      CHECK(varint(&packed) == number(lc, 1));
      CHECK(varint(&packed) == number(lc, 1) && packed.p == packed.end);
      CHECK(number(sf.bytes, 2) == (sample < 3 ? 7 : 21));
      CHECK(fields(sf.bytes, 3) == (sample < 3 ? 4 : 3));
      field label;
      view sit = sf.bytes;
      size_t timestamp_count = 0;
      while (next(&sit, &label))
        if (label.tag == 3) {
          view key = string_at(profile, number(label.bytes, 1));
          if (text(key, "end_timestamp_ns")) {
            CHECK(number(label.bytes, 3) == (sample == 0 ? 0 : 1234567890123456789ULL));
            timestamp_count++;
          }
          if (text(key, "thread id")) {
            CHECK(fields(label.bytes, 3) == 1 && number(label.bytes, 3) == 0);
            CHECK(text(string_at(profile, number(label.bytes, 4)), "count"));
          }
          if (text(key, "rum.view_id"))
            CHECK(text(string_at(profile, number(label.bytes, 2)), "view-1"));
        }
      CHECK(timestamp_count == (sample < 3 ? 1 : 0));
      sample++;
    }
  if (output) {
    FILE *fp = fopen(output, "wb");
    CHECK(fp);
    CHECK(fwrite(b.bytes, 1, b.n, fp) == b.n);
    fclose(fp);
  }
  free(decoded);
  // Failed output must not consume samples. Retry yields a complete frame.
  buffer failed = {0};
  failed.fail = 1;
  CHECK(pa_serialize(p, 0, 1, sink, &failed) == PA_OUTPUT);
  b.n = 0;
  OK(pa_serialize(p, 0, 1, sink, &b));
  profile = decode(&b, &decoded);
  CHECK(fields(profile, 2) == 4);
  free(decoded);
  size_t before = a.live;
  OK(pa_reset(p));
  CHECK(a.live == before);
  CHECK(pa_record(p, stack, labels, values, 2, 1, 0) == PA_INVALID);
  CHECK(pa_intern_stack(p, &location, 1, &again) == PA_INVALID);
  b.n = 0;
  OK(pa_serialize(p, 0, 0, sink, &b));
  profile = decode(&b, &decoded);
  CHECK(fields(profile, 2) == 0 && fields(profile, 4) == 0);
  free(decoded);
  pa_destroy(p);
  CHECK(a.live == 0);
  free(b.bytes);
}
static void boundaries(void) {
  pa_value_type type = {"cpu", "ns"};
  pa_profile *p = NULL;
  OK(pa_create(&type, 1, 1, NULL, &p));
  pa_id stack = 0, labels = 0;
  OK(pa_intern_stack(p, NULL, 0, &stack));
  OK(pa_intern_labels(p, NULL, 0, &labels));
  int64_t value = INT64_MAX;
  OK(pa_record(p, stack, labels, &value, 1, 0, 0));
  value = 1;
  OK(pa_record(p, stack, labels, &value, 1, 0, 0));
  for (int i = 0; i < 10000; i++) OK(pa_record(p, stack, labels, &value, 1, 1, i));
  buffer b = {0};
  OK(pa_serialize(p, 0, 10000, sink, &b));
  uint8_t *bytes;
  view v = decode(&b, &bytes);
  CHECK(fields(v, 2) == 10001 && number(sub(v, 2), 2) == INT64_MAX);
  free(bytes);
  free(b.bytes);
  // Metadata much larger than the encoder/compressor staging block.
  char *long_string = malloc(100000);
  CHECK(long_string);
  for (size_t i = 0; i < 100000; i++) long_string[i] = (char)('a' + i % 26);
  pa_id id = 0;
  OK(pa_intern_string(p, long_string, 100000, &id));
  free(long_string);
  b = (buffer){0};
  OK(pa_serialize(p, 0, 10000, sink, &b));
  v = decode(&b, &bytes);
  CHECK(fields(v, 2) == 10001);
  free(bytes);
  free(b.bytes);
  pa_destroy(p);
}
static void warmed_record_and_negative_saturation(void) {
  allocation a = {0};
  pa_allocator allocator = {&a, test_alloc, test_free};
  pa_value_type type = {"cpu", "ns"};
  pa_profile *p = NULL;
  OK(pa_create(&type, 1, 1, &allocator, &p));
  pa_id stack = 0, labels = 0;
  OK(pa_intern_stack(p, NULL, 0, &stack));
  OK(pa_intern_labels(p, NULL, 0, &labels));
  int64_t value = 1;
  OK(pa_record(p, stack, labels, &value, 1, 1, 0));
  size_t calls = a.calls;
  for (int i = 1; i < 16; ++i) {
    OK(pa_intern_stack(p, NULL, 0, &stack));
    OK(pa_intern_labels(p, NULL, 0, &labels));
    OK(pa_record(p, stack, labels, &value, 1, 1, i));
  }
  CHECK(a.calls == calls);
  OK(pa_reset(p));
  OK(pa_intern_stack(p, NULL, 0, &stack));
  OK(pa_intern_labels(p, NULL, 0, &labels));
  value = INT64_MIN;
  OK(pa_record(p, stack, labels, &value, 1, 0, 0));
  value = -1;
  OK(pa_record(p, stack, labels, &value, 1, 0, 0));
  buffer b = {0};
  OK(pa_serialize(p, 0, 1, sink, &b));
  uint8_t *bytes = NULL;
  view profile = decode(&b, &bytes);
  CHECK(number(sub(profile, 2), 2) == (uint64_t)INT64_MIN);
  free(bytes);
  free(b.bytes);
  pa_destroy(p);
  CHECK(a.live == 0);
}
static void failed_insert_retries(void) {
  allocation a = {0};
  pa_allocator allocator = {&a, test_alloc, test_free};
  pa_value_type type = {"cpu", "ns"};
  pa_profile *p = NULL;
  OK(pa_create(&type, 1, 1, &allocator, &p));
  char large[8000];
  pa_id id, again;
  for (int i = 0; i < 2; i++) {
    memset(large, 'a' + i, sizeof(large));
    OK(pa_intern_string(p, large, sizeof(large), &id));
  }
  memset(large, 'x', sizeof(large));
  size_t live = a.live;
  /* A payload allocation that repeatedly fails must not leak or alter IDs. */
  for (int i = 0; i < 100; i++) {
    a.fail = a.calls + 1;
    CHECK(pa_intern_string(p, large, sizeof(large), &again) == PA_NOMEM);
    CHECK(a.live == live);
  }
  a.fail = 0;
  OK(pa_intern_string(p, large, sizeof(large), &again));
  pa_function f = {id, id, again, 0};
  pa_id function;
  OK(pa_intern_function(p, &f, &function));
  pa_destroy(p);
  CHECK(a.live == 0);
}
static void rejected_table_growth(void) {
  allocation a = {0};
  pa_allocator allocator = {&a, test_alloc, test_free};
  pa_value_type type = {"cpu", "ns"};
  pa_profile *p = NULL;
  OK(pa_create(&type, 1, 1, &allocator, &p));
  pa_id stable = str(p, "stable"), id;
  /* Allow fitted payloads, but reject small allocations such as table growth.
   * Fill until an insertion needs a larger map, while entry capacity is spare. */
  a.reject_below = 256 * 1024;
  for (int i = 0;; i++) {
    char value[32];
    snprintf(value, sizeof(value), "value-%d", i);
    pa_status status = pa_intern_string(p, value, strlen(value), &id);
    if (status == PA_NOMEM) break;
    CHECK(status == PA_OK && i < 1024);
  }
  size_t live = a.live;
  char *large = malloc(2 * 1024 * 1024);
  CHECK(large);
  memset(large, 'x', 2 * 1024 * 1024);
  for (size_t i = 0; i < 64; i++) {
    size_t n = 512 * 1024 + i * 16384;
    CHECK(pa_intern_string(p, large, n, &id) == PA_NOMEM);
    CHECK(a.live == live); /* every failed fitted block must be released */
  }
  a.reject_below = 0;
  OK(pa_intern_string(p, large, 512 * 1024, &id));
  pa_function f = {stable, id, 0, 0};
  pa_id function;
  OK(pa_intern_function(p, &f, &function));
  pa_destroy(p);
  CHECK(a.live == 0);
  free(large);
}
static void failures(void) {
  // Fail each allocation in a cold fixture; all successful IDs remain usable,
  // failed calls can retry, no allocations escape destroy (including growth).
  for (size_t fail = 1; fail < 35; fail++) {
    allocation a = {0};
    a.fail = fail;
    pa_allocator allocator = {&a, test_alloc, test_free};
    pa_value_type type = {"cpu", "ns"};
    pa_profile *p = NULL;
    pa_status status = pa_create(&type, 1, 1, &allocator, &p);
    if (status == PA_OK) {
      pa_id stack = 0, labels = 0, id = 0;
      for (int i = 0; i < 100; i++) {
        char text[32];
        snprintf(text, sizeof(text), "string-%d", i);
        pa_status st = pa_intern_string(p, text, strlen(text), &id);
        CHECK(st == PA_OK || st == PA_NOMEM);
      }
      status = pa_intern_stack(p, NULL, 0, &stack);
      CHECK(status == PA_OK || status == PA_NOMEM);
      if (status != PA_OK) OK(pa_intern_stack(p, NULL, 0, &stack));
      status = pa_intern_labels(p, NULL, 0, &labels);
      CHECK(status == PA_OK || status == PA_NOMEM);
      if (status != PA_OK) OK(pa_intern_labels(p, NULL, 0, &labels));
      int64_t value = 1;
      for (int i = 0; i < 100; i++) {
        status = pa_record(p, stack, labels, &value, 1, 1, i);
        CHECK(status == PA_OK || status == PA_NOMEM);
      }
      buffer b = {0};
      status = pa_serialize(p, 0, 1, sink, &b);
      CHECK(status == PA_OK || status == PA_NOMEM);
      a.fail = 0;
      b.n = 0;
      OK(pa_serialize(p, 0, 1, sink, &b));
      free(b.bytes);
      pa_destroy(p);
    } else
      CHECK(status == PA_NOMEM && p == NULL);
    CHECK(a.live == 0);
  }
}
static void packed_stacks(void) {
  allocation a = {0};
  pa_allocator allocator = {&a, test_alloc, test_free};
  pa_value_type type = {"cpu-time", "nanoseconds"};
  pa_profile *p = NULL;
  OK(pa_create(&type, 1, 1, &allocator, &p));
  pa_id locations[256], ids[16384], stack = 0, labels = 0;
  for (size_t i = 0; i < 256; i++) {
    pa_location location = {0};
    location.address = 1000 + i;
    OK(pa_intern_location(p, &location, &locations[i]));
  }
  for (size_t i = 0; i < 16384; i++) ids[i] = locations[i % 256];
  OK(pa_intern_stack(p, ids, 16384, &stack));
  OK(pa_intern_labels(p, NULL, 0, &labels));
  int64_t value = -1;
  for (size_t pass = 0; pass < 2; pass++) {
    OK(pa_record(p, stack, labels, &value, 1, 1, (int64_t)pass));
    buffer b = {0};
    OK(pa_serialize(p, 0, 2, sink, &b));
    uint8_t *storage = NULL;
    view profile = decode(&b, &storage), iter = profile;
    uint64_t wire_ids[256] = {0};
    field f;
    while (next(&iter, &f)) {
      if (f.tag != 4) continue;
      uint64_t address = number(f.bytes, 3);
      if (address >= 1000 && address < 1256)
        wire_ids[address - 1000] = number(f.bytes, 1);
    }
    CHECK(wire_ids[255] >= 128);
    iter = profile;
    size_t samples = 0;
    while (next(&iter, &f)) {
      if (f.tag != 2) continue;
      CHECK(fields(f.bytes, 1) == 1 && number(f.bytes, 2) == UINT64_MAX);
      view packed = sub(f.bytes, 1);
      CHECK(packed.p != NULL && (size_t)(packed.end - packed.p) > 16384);
      for (size_t i = 0; i < 16384; i++)
        CHECK(wire_ids[i % 256] && varint(&packed) == wire_ids[i % 256]);
      CHECK(packed.p == packed.end);
      ++samples;
    }
    CHECK(samples == pass + 1);
    free(storage);
    free(b.bytes);
    for (size_t i = 0; i < 700; i++) {
      pa_location location = {0};
      location.address = 100000 + i;
      pa_id unused = 0;
      OK(pa_intern_location(p, &location, &unused));
    }
  }
  pa_destroy(p);
  CHECK(a.live == 0);
}
static void export_reset(pa_profile *p, buffer *b) {
  b->n = 0;
  OK(pa_serialize(p, 0, 1, sink, b));
  OK(pa_reset(p));
}
static void normal_profile(pa_profile *p) {
  pa_id filename = str(p, "module.dll"), name = str(p, "stable-symbol");
  pa_mapping m = {0, 1000, 0, filename, filename};
  pa_function f = {name, name, filename, 42};
  pa_id mapping, function, location, stack, labels;
  OK(pa_intern_mapping(p, &m, &mapping));
  OK(pa_intern_function(p, &f, &function));
  pa_location l = {mapping, function, 100, 43};
  OK(pa_intern_location(p, &l, &location));
  OK(pa_intern_stack(p, &location, 1, &stack));
  pa_label ls = {name, filename, 0, 0, 0};
  OK(pa_intern_labels(p, &ls, 1, &labels));
  int64_t values[] = {7, 11};
  for (int i = 0; i < 200; i++) {
    OK(pa_record(p, stack, labels, values, 2, 1, i));
    OK(pa_record(p, stack, labels, values, 2, 0, 0));
  }
}
static void warmed_cycles_and_spike(void) {
  allocation a = {0};
  pa_allocator allocator = {&a, test_alloc, test_free};
  pa_value_type types[] = {{"cpu", "ns"}, {"wall", "ns"}};
  pa_profile *p = NULL;
  OK(pa_create(types, 2, 1, &allocator, &p));
  buffer b = {0};
  normal_profile(p);
  export_reset(p, &b);
  size_t baseline = a.live, calls = a.calls, frees = a.frees;
  for (int i = 0; i < 8; i++) {
    normal_profile(p);
    CHECK(a.calls == calls && a.frees == frees); /* ingestion */
    export_reset(p, &b);
    CHECK(a.calls == calls && a.frees == frees && a.live == baseline);
  }
  pa_id stack, labels;
  OK(pa_intern_stack(p, NULL, 0, &stack));
  OK(pa_intern_labels(p, NULL, 0, &labels));
  int64_t values[] = {1, 2};
  for (int i = 0; i < 100000; i++) OK(pa_record(p, stack, labels, values, 2, 1, i));
#ifndef PA_TEST_COLLISIONS
  /* The all-hashes-identical table deliberately cannot model huge cardinality. */
  for (int i = 0; i < 10000; i++) {
    pa_location l = {0};
    l.address = (uint64_t)i;
    pa_id location;
    OK(pa_intern_location(p, &l, &location));
  }
#endif
  export_reset(p, &b);
  size_t spike = a.live;
  CHECK(spike > baseline + 4000000);
  for (int i = 0; i < 2; i++) {
    normal_profile(p);
    export_reset(p, &b);
    CHECK(a.live == spike);
  }
  normal_profile(p);
  export_reset(p, &b); /* third normal completed cycle retires spike demand */
  CHECK(a.live < baseline + 200000);
  normal_profile(p);
  export_reset(p, &b); /* reacquire modest floors, then no further churn */
  calls = a.calls;
  frees = a.frees;
  for (int i = 0; i < 5; i++) {
    normal_profile(p);
    export_reset(p, &b);
    CHECK(a.calls == calls && a.frees == frees);
  }
  pa_destroy(p);
  CHECK(a.live == 0);
  free(b.bytes);
}
static void string_cycles(void) {
  allocation a = {0};
  pa_allocator allocator = {&a, test_alloc, test_free};
  pa_value_type type = {"cpu", "ns"};
  pa_profile *p = NULL;
  OK(pa_create(&type, 1, 1, &allocator, &p));
  char payload[70000];
  memset(payload, 'x', sizeof(payload));
  pa_id old, id;
  OK(pa_intern_string(p, payload, sizeof(payload), &old));
  buffer b = {0};
  export_reset(p, &b);
  size_t retained = a.live;
  /* No completed output: even many failed serializations/resets cannot age. */
  for (int i = 0; i < 5; i++) {
    b.fail = 1;
    CHECK(pa_serialize(p, 0, 1, sink, &b) == PA_OUTPUT);
    size_t calls = a.calls;
    OK(pa_reset(p));
    CHECK(a.calls == calls && a.live == retained);
  }
  b.fail = 0;
  /* An unused cached string is NOT emitted by an empty current profile. */
  b.n = 0;
  OK(pa_serialize(p, 0, 1, sink, &b));
  uint8_t *storage;
  view v = decode(&b, &storage);
  CHECK(fields(v, 6) == 4 && fields(v, 2) == 0);
  free(storage);
  for (int i = 0; i < 5; i++) {
    b.n = 0;
    OK(pa_serialize(p, 0, 1, sink, &b));
  }
  OK(pa_reset(p)); /* only one unused cycle */
  CHECK(a.live == retained);
  export_reset(p, &b); /* two unused cycles */
  CHECK(a.live == retained);
  size_t calls = a.calls;
  OK(pa_intern_string(p, payload, sizeof(payload), &id));
  CHECK(a.calls == calls && id != old);
  pa_function stale = {old, 0, 0, 0};
  CHECK(pa_intern_function(p, &stale, &old) == PA_INVALID);
  export_reset(p, &b); /* successful reuse restarts age */
  for (int i = 0; i < 2; i++) {
    export_reset(p, &b);
    CHECK(a.live == retained);
  }
  export_reset(p, &b);
  CHECK(a.live + sizeof(payload) < retained); /* REAL payload was freed */
  calls = a.calls;
  OK(pa_intern_string(p, payload, sizeof(payload), &id));
  CHECK(a.calls > calls);
  export_reset(p, &b);
  /* Success then failed output: only previously emitted strings are used. */
  b.n = 0;
  OK(pa_serialize(p, 0, 1, sink, &b));
  OK(pa_intern_string(p, payload, sizeof(payload), &id));
  b.fail = 1;
  CHECK(pa_serialize(p, 0, 1, sink, &b) == PA_OUTPUT);
  OK(pa_reset(p));
  b.fail = 0;
  retained = a.live;
  export_reset(p, &b);
  CHECK(a.live == retained);
  export_reset(p, &b); /* failed-only reference did not restart age */
  CHECK(a.live + sizeof(payload) < retained);
  pa_destroy(p);
  CHECK(a.live == 0);
  free(b.bytes);
}
static void churn_and_compaction(void) {
  /* Initial 160 KiB of mixed strings leaves ten sparse retained blocks.
   * Sweep every allocation in maintenance and its successful path. */
  for (size_t failure = 0; failure < 5; failure++) {
    allocation a = {0};
    pa_allocator allocator = {&a, test_alloc, test_free};
    pa_value_type type = {"cpu", "ns"};
    pa_profile *p = NULL;
    OK(pa_create(&type, 1, 1, &allocator, &p));
    char payload[1000];
    pa_id id;
    for (int i = 0; i < 160; i++) {
      memset(payload, 'a' + i % 26, sizeof(payload));
      memcpy(payload, &i, sizeof(i));
      OK(pa_intern_string(p, payload, sizeof(payload), &id));
    }
    buffer b = {0};
    export_reset(p, &b);
    for (int cycle = 0; cycle < 3; cycle++) {
      for (int i = 0; i < 160; i += 16) {
        memset(payload, 'a' + i % 26, sizeof(payload));
        memcpy(payload, &i, sizeof(i));
        OK(pa_intern_string(p, payload, sizeof(payload), &id));
      }
      export_reset(p, &b);
    }
    size_t sparse = a.live;
    a.fail = failure ? a.calls + failure : 0;
    /* A retained hit works even if optional maintenance cannot allocate. */
    memset(payload, 'a', sizeof(payload));
    memset(payload, 0, sizeof(int));
    OK(pa_intern_string(p, payload, sizeof(payload), &id));
    a.fail = 0;
    for (int cycle = 0; cycle < 2; cycle++) {
      for (int i = 0; i < 160; i += 16) {
        memset(payload, 'a' + i % 26, sizeof(payload));
        memcpy(payload, &i, sizeof(i));
        OK(pa_intern_string(p, payload, sizeof(payload), &id));
      }
      export_reset(p, &b);
    }
    CHECK(a.live + 100000 < sparse); /* successful retry reclaimed holes */
    size_t compact = a.live;
    for (int cycle = 0; cycle < 20; cycle++) {
      str(p, "stable");
      for (int i = 0; i < 64; i++) {
        int unique = 10000 + 64 * cycle + i;
        memset(payload, 'z', sizeof(payload));
        memcpy(payload, &unique, sizeof(unique));
        OK(pa_intern_string(p, payload, sizeof(payload), &id));
      }
      export_reset(p, &b);
      CHECK(a.live < compact + 400000);
    }
    pa_destroy(p);
    CHECK(a.live == 0);
    free(b.bytes);
  }
}
#ifndef PA_TEST_COLLISIONS
static void check_numeric_sums(buffer *b, size_t expected) {
  uint8_t *storage;
  view profile = decode(b, &storage), iter = profile;
  CHECK(fields(profile, 2) == expected);
  field sample;
  size_t count = 0;
  while (next(&iter, &sample)) {
    if (sample.tag != 2) continue;
    CHECK(fields(sample.bytes, 2) == 2 && fields(sample.bytes, 3) == 1);
    view values = sample.bytes;
    field value;
    size_t value_count = 0;
    while (next(&values, &value)) {
      if (value.tag != 2) continue;
      CHECK(value.number == (value_count++ ? 16 : 10));
    }
    view label = sub(sample.bytes, 3);
    CHECK(text(string_at(profile, number(label, 1)), "bucket"));
    CHECK(fields(label, 3) == 1 && number(label, 3) == ++count);
  }
  CHECK(count == expected);
  free(storage);
}
static void untimed_sum_growth_and_trim(void) {
  allocation a = {0};
  pa_allocator allocator = {&a, test_alloc, test_free};
  pa_value_type types[] = {{"cpu", "ns"}, {"wall", "ns"}};
  pa_profile *p = NULL;
  OK(pa_create(types, 2, 1, &allocator, &p));
  buffer b = {0};
  int64_t first[] = {3, 5}, repeat[] = {7, 11};
  pa_id stack, labels;
  pa_label label = {0};
  label.is_numeric = 1;
  label.num = 1;
  /* Warm the same strings and normal untimed path used after the spike. */
  label.key = str(p, "bucket");
  OK(pa_intern_stack(p, NULL, 0, &stack));
  OK(pa_intern_labels(p, &label, 1, &labels));
  OK(pa_record(p, stack, labels, first, 2, 0, 0));
  OK(pa_record(p, stack, labels, repeat, 2, 0, 0));
  export_reset(p, &b);
  size_t normal = a.live;

  label.key = str(p, "bucket");
  OK(pa_intern_stack(p, NULL, 0, &stack));
  bool failed_growth = false;
  /* Stay within the normal observation floor: allocations during record here
   * belong to the sum map, not the observation vector or label metadata. */
  for (size_t i = 0; i < 4096; i++) {
    label.num = (int64_t)i + 1;
    OK(pa_intern_labels(p, &label, 1, &labels));
    size_t live = a.live;
    if (!failed_growth) a.fail = a.calls + 1;
    pa_status status = pa_record(p, stack, labels, first, 2, 0, 0);
    a.fail = 0; /* Never inject failure into interning or serialization. */
    if (status == PA_NOMEM) {
      CHECK(!failed_growth && a.live == live);
      failed_growth = true;
      b.n = 0;
      OK(pa_serialize(p, 0, 1, sink, &b));
      check_numeric_sums(&b, i); /* no appended sample or altered old sums */
      OK(pa_record(p, stack, labels, first, 2, 0, 0));
    } else {
      CHECK(status == PA_OK);
    }
    OK(pa_record(p, stack, labels, repeat, 2, 0, 0));
  }
  CHECK(failed_growth);
  b.n = 0;
  OK(pa_serialize(p, 0, 1, sink, &b));
  check_numeric_sums(&b, 4096); /* includes the retried key, exactly once */
  OK(pa_reset(p));
  size_t spike = a.live;
  CHECK(spike > normal);

  for (size_t cycle = 0; cycle < 3; cycle++) {
    label.key = str(p, "bucket");
    label.num = 1;
    OK(pa_intern_stack(p, NULL, 0, &stack));
    OK(pa_intern_labels(p, &label, 1, &labels));
    size_t calls = a.calls;
    OK(pa_record(p, stack, labels, first, 2, 0, 0));
    OK(pa_record(p, stack, labels, repeat, 2, 0, 0));
    CHECK(a.calls == calls); /* spike sum-map capacity remains reusable */
    b.n = 0;
    OK(pa_serialize(p, 0, 1, sink, &b));
    check_numeric_sums(&b, 1);
    calls = a.calls;
    OK(pa_reset(p));
    CHECK(a.calls == calls);
    if (cycle < 2) CHECK(a.live == spike);
  }
  CHECK(a.live < spike);
  /* Isolate sum-map trimming from metadata trimming: intern first, then the
   * next untimed record must reacquire its map while observations still fit. */
  label.key = str(p, "bucket");
  OK(pa_intern_stack(p, NULL, 0, &stack));
  OK(pa_intern_labels(p, &label, 1, &labels));
  size_t calls = a.calls;
  OK(pa_record(p, stack, labels, first, 2, 0, 0));
  CHECK(a.calls > calls);
  OK(pa_record(p, stack, labels, repeat, 2, 0, 0));
  b.n = 0;
  OK(pa_serialize(p, 0, 1, sink, &b));
  check_numeric_sums(&b, 1);
  OK(pa_reset(p));
  CHECK(a.live < spike);
  pa_destroy(p);
  CHECK(a.live == 0);
  free(b.bytes);
}
#endif
#ifndef PA_TEST_COLLISIONS
static void growth_failures(void) {
  size_t sites = 0;
  /* First measure the successful fixture, then fail each allocation exactly
   * once. Includes map/vector growth, payload blocks and observation growth. */
  for (size_t failure = 0; failure <= sites; failure++) {
    allocation a = {0};
    pa_allocator allocator = {&a, test_alloc, test_free};
    pa_value_type type = {"cpu", "ns"};
    pa_profile *p = NULL;
    OK(pa_create(&type, 1, 1, &allocator, &p));
    pa_id stable = str(p, "stable"), stack, labels;
    OK(pa_intern_stack(p, NULL, 0, &stack));
    OK(pa_intern_labels(p, NULL, 0, &labels));
    size_t start = a.calls;
    if (failure) a.fail = start + failure;
    char payload[1000];
    for (int i = 0; i < 1400; i++) {
      memset(payload, 'x', sizeof(payload));
      memcpy(payload, &i, sizeof(i));
      pa_id id;
      pa_status status = pa_intern_string(p, payload, sizeof(payload), &id);
      CHECK(status == PA_OK || status == PA_NOMEM);
      if (status != PA_OK) {
        OK(pa_intern_string(p, payload, sizeof(payload), &id));
      }
      pa_location l = {0};
      l.address = (uint64_t)i;
      status = pa_intern_location(p, &l, &id);
      CHECK(status == PA_OK || status == PA_NOMEM);
      if (status != PA_OK) OK(pa_intern_location(p, &l, &id));
    }
    int64_t value = 1;
    for (int i = 0; i < 10000; i++) {
      pa_status status = pa_record(p, stack, labels, &value, 1, 1, i);
      CHECK(status == PA_OK || status == PA_NOMEM);
      if (status != PA_OK) OK(pa_record(p, stack, labels, &value, 1, 1, i));
    }
    if (!failure) sites = a.calls - start;
    a.fail = 0;
    pa_function f = {stable, stable, stable, 0};
    pa_id function;
    OK(pa_intern_function(p, &f, &function)); /* old IDs survived every failure */
    buffer b = {0};
    OK(pa_serialize(p, 0, 1, sink, &b));
    uint8_t *storage;
    view v = decode(&b, &storage);
    CHECK(fields(v, 2) == 10000);
    free(storage);
    free(b.bytes);
    pa_destroy(p);
    CHECK(a.live == 0);
  }
}
#endif
int main(int argc, char **argv) {
#ifndef PA_TEST_COLLISIONS
  growth_failures();
  untimed_sum_growth_and_trim();
#endif
  warmed_cycles_and_spike();
  string_cycles();
  churn_and_compaction();
  semantics(argc > 1 ? argv[1] : NULL);
  packed_stacks();
  boundaries();
  failures();
  failed_insert_retries();
  rejected_table_growth();
  warmed_record_and_negative_saturation();
  puts(
      "aggregation: semantics, timestamps, collisions, packed streaming, warmed reuse, "
      "three-cycle eviction, spike recovery, churn/compaction and OOM/output failures "
      "passed"
  );
  return 0;
}
