/* Benchmark-only hooks. No platform dependencies or production runtime API. */
#ifndef PA_BENCH_COMPRESSION_HOOKS_H
#define PA_BENCH_COMPRESSION_HOOKS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint64_t pa_bench_compression_begin(void);
void pa_bench_compression_end(uint64_t start);
void pa_bench_serialized_bytes(uint64_t bytes);
#ifdef __cplusplus
}
#endif
#endif
