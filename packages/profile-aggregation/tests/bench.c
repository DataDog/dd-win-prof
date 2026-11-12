/* Copyright 2026 Datadog, Inc. SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "profile_aggregation.h"
#ifdef _WIN32
#include <windows.h>

#include <psapi.h>
static double now(void) {
  LARGE_INTEGER t, f;
  QueryPerformanceCounter(&t);
  QueryPerformanceFrequency(&f);
  return (double)t.QuadPart / f.QuadPart;
}
static size_t resident(void) {
  PROCESS_MEMORY_COUNTERS c;
  GetProcessMemoryInfo(GetCurrentProcess(), &c, sizeof(c));
  return c.WorkingSetSize;
}
#else
#include <sys/resource.h>
static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}
static size_t resident(void) {
  struct rusage r;
  getrusage(RUSAGE_SELF, &r);
#ifdef __APPLE__
  return (size_t)r.ru_maxrss;
#else
  return (size_t)r.ru_maxrss * 1024;
#endif
}
#endif
#define OK(x)                                                 \
  do {                                                        \
    pa_status s = (x);                                        \
    if (s != PA_OK) {                                         \
      fprintf(stderr, "status %d at line %d\n", s, __LINE__); \
      exit(1);                                                \
    }                                                         \
  } while (0)
typedef struct {
  size_t allocations, bytes, live, peak;
} stats;
static void *alloc(void *ctx, size_t n) {
  stats *s = ctx;
  void *p = malloc(n);
  if (p) {
    s->allocations++;
    s->bytes += n;
    s->live += n;
    if (s->live > s->peak) s->peak = s->live;
  }
  return p;
}
static void release(void *ctx, void *p, size_t n) {
  ((stats *)ctx)->live -= n;
  free(p);
}
static pa_status output(void *ctx, const uint8_t *p, size_t n) {
  (void)p;
  *(size_t *)ctx += n;
  return PA_OK;
}
int main(int argc, char **argv) {
  size_t count = argc > 1 ? (size_t)strtoull(argv[1], NULL, 10) : 100000;
  printf(
      "mode,samples,ingest_ms,max_record_us,serialize_ms,allocations,allocated_bytes,"
      "core_peak_bytes,core_live_before_flush,core_after_reset,compressed_bytes,"
      "process_peak_rss_or_working_set\n"
  );
  const char *names[] = {"repeated", "unique", "deep", "labels", "churn"};
  for (int mode = 0; mode < 5; mode++) {
    stats s = {0};
    pa_allocator allocator = {&s, alloc, release};
    pa_value_type types[] = {{"cpu-time", "nanoseconds"}, {"wall-time", "nanoseconds"}};
    pa_profile *p = NULL;
    OK(pa_create(types, 2, 10000000, &allocator, &p));
    pa_location location = {0};
    pa_function function = {0};
    pa_id locs[256], stack = 0, labels = 0, key = 0;
    OK(pa_intern_function(p, &function, &location.function));
    OK(pa_intern_string(p, "thread_name", 11, &key));
    for (size_t j = 0; j < 256; j++) {
      location.address = j;
      OK(pa_intern_location(p, &location, &locs[j]));
    }
    size_t depth = mode == 2 ? 256 : 16;
    OK(pa_intern_stack(p, locs, depth, &stack));
    OK(pa_intern_labels(p, NULL, 0, &labels));
    size_t warm_allocs = s.allocations;
    double start = now(), max_record = 0, serialize_ms = 0;
    size_t compressed = 0;
    for (size_t i = 0; i < count; i++) {
      double t = now();
      if (mode == 1 || mode == 4) {
        OK(pa_intern_function(p, &function, &location.function));
        location.address = i + 256;
        OK(pa_intern_location(p, &location, &locs[0]));
      }
      OK(pa_intern_stack(p, locs, depth, &stack));
      if (mode != 3) OK(pa_intern_labels(p, NULL, 0, &labels));
      if (mode == 3) {
        char name[64];
        snprintf(name, sizeof(name), "worker-%zu", i);
        pa_label l = {0};
        OK(pa_intern_string(p, "thread_name", 11, &key));
        l.key = key;
        OK(pa_intern_string(p, name, strlen(name), &l.str));
        OK(pa_intern_labels(p, &l, 1, &labels));
      }
      int64_t values[] = {10000000, 20000000};
      OK(pa_record(p, stack, labels, values, 2, 1, (int64_t)i + 1));
      double elapsed = now() - t;
      if (elapsed > max_record) max_record = elapsed;
      if (mode == 4 && i % 1000 == 999) {
        double encode = now();
        OK(pa_serialize(p, 0, (int64_t)i + 1, output, &compressed));
        serialize_ms += (now() - encode) * 1000;
        OK(pa_reset(p));
        OK(pa_intern_function(p, &function, &location.function));
        for (size_t j = 0; j < 256; j++) {
          location.address = j;
          OK(pa_intern_location(p, &location, &locs[j]));
        }
        OK(pa_intern_stack(p, locs, depth, &stack));
        OK(pa_intern_labels(p, NULL, 0, &labels));
      }
    }
    double ingest_ms = (now() - start) * 1000 - serialize_ms;
    size_t live = s.live;
    double encode = now();
    OK(pa_serialize(p, 0, (int64_t)count, output, &compressed));
    serialize_ms += (now() - encode) * 1000;
    OK(pa_reset(p));
    printf(
        "%s,%zu,%.3f,%.3f,%.3f,%zu,%zu,%zu,%zu,%zu,%zu,%zu\n",
        names[mode],
        count,
        ingest_ms,
        max_record * 1e6,
        serialize_ms,
        s.allocations - warm_allocs,
        s.bytes,
        s.peak,
        live,
        s.live,
        compressed,
        resident()
    );
    pa_destroy(p);
    if (s.live) return 1;
  }
  return 0;
}
