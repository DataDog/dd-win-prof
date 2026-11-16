/* Copyright 2026 Datadog, Inc. SPDX-License-Identifier: Apache-2.0 */
#ifndef DD_PROFILE_AGGREGATION_H
#define DD_PROFILE_AGGREGATION_H
#include <stddef.h>
#include <stdint.h>
#if defined(_WIN32) && defined(PA_SHARED)
#ifdef PA_BUILD
#define PA_API __declspec(dllexport)
#else
#define PA_API __declspec(dllimport)
#endif
#else
#define PA_API
#endif
#ifdef __cplusplus
extern "C" {
#endif

typedef struct pa_profile pa_profile;
typedef uint64_t pa_id;
typedef enum {
  PA_OK,
  PA_INVALID,
  PA_NOMEM,
  PA_OUTPUT,
  PA_COMPRESSION,
  PA_LIMIT
} pa_status;
typedef struct {
  const char *type;
  const char *unit;
} pa_value_type;
typedef struct {
  const char *ptr;
  size_t len;
} pa_string_view;
typedef struct {
  pa_string_view type, unit;
} pa_value_type_view;
/* Pass NULL for the default allocator; otherwise both callbacks are required.
 * alloc returns malloc-aligned memory; free receives the original size.
 * The allocator/context must outlive the profile. */
typedef struct {
  void *context;
  void *(*alloc)(void *context, size_t size);
  void (*free)(void *context, void *ptr, size_t size);
} pa_allocator;
typedef struct {
  uint64_t memory_start, memory_limit, file_offset;
  pa_id filename, build_id;
} pa_mapping;
typedef struct {
  pa_id name, system_name, filename;
  int64_t start_line;
} pa_function;
typedef struct {
  pa_id mapping, function;
  uint64_t address;
  int64_t line;
} pa_location;
/* is_numeric is 0 or 1. Numeric zero and string zero are distinct labels. */
typedef struct {
  pa_id key, str, num_unit;
  int64_t num;
  uint32_t is_numeric;
} pa_label;
/* Sink borrows each chunk only for this call. Non-OK aborts serialization.
 * A failed sink may have received a partial frame: discard the entire output. */
typedef pa_status (*pa_sink)(void *context, const uint8_t *bytes, size_t size);

/* Single-threaded instances, no global state. All input is borrowed for the call
 * and copied on success. Output instance is NULL on failure. Period uses type[0]. */
PA_API pa_status pa_create(
    const pa_value_type *types,
    size_t count,
    int64_t period,
    const pa_allocator *allocator,
    pa_profile **out
);
/* Length-aware UTF-8 definitions, including embedded NUL bytes. */
PA_API pa_status pa_create_with_types(
    const pa_value_type_view *types,
    size_t count,
    int64_t period,
    const pa_allocator *allocator,
    pa_profile **out
);
PA_API void pa_destroy(pa_profile *profile);
/* IDs belong to one instance and one reset generation. Zero denotes absent/empty
 * metadata, but is not a valid stack or label-set. Never mix IDs across instances. */
PA_API pa_status pa_intern_string(pa_profile *, const char *, size_t, pa_id *);
PA_API pa_status pa_intern_mapping(pa_profile *, const pa_mapping *, pa_id *);
PA_API pa_status pa_intern_function(pa_profile *, const pa_function *, pa_id *);
PA_API pa_status pa_intern_location(pa_profile *, const pa_location *, pa_id *);
PA_API pa_status pa_intern_stack(pa_profile *, const pa_id *, size_t, pa_id *);
/* Label order is significant for interning/untimestamped aggregation. */
PA_API pa_status pa_intern_labels(pa_profile *, const pa_label *, size_t, pa_id *);
/* has_timestamp == 0 sums matching stack/labels with saturating int64 arithmetic.
 * Present timestamps (including zero) always retain distinct observations, even when
 * identical. record is atomic on failure. No compression or per-observation allocation
 * on a warmed path with sufficient observation capacity. */
PA_API pa_status pa_record(
    pa_profile *,
    pa_id stack,
    pa_id labels,
    const int64_t *values,
    size_t count,
    int has_timestamp,
    int64_t timestamp_ns
);
/* Emits one Zstandard-compressed pprof frame incrementally. Does not reset or
 * consume data, including on failure; caller may retry. Times are Unix ns.
 * Successful sink completion is finalized as one export cycle by reset. */
PA_API pa_status pa_serialize(
    pa_profile *, int64_t start_ns, int64_t end_ns, pa_sink sink, void *context
);
/* Invalidates ALL IDs and empties the profile. Retains normal working storage
 * and privately caches string contents, evicting after three consecutive completed
 * exports without use. Reset finalizes at most one cycle after successful sink
 * output; failed-only output, retries and repeated serialization do not add cycles.
 * Exceptional spare capacity is eventually released. Preserves value types/period.
 * Allocation-free; only generation exhaustion can fail for a valid instance. */
PA_API pa_status pa_reset(pa_profile *);
#ifdef __cplusplus
}
#endif
#endif
