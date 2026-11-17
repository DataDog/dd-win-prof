// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal/dd-win-rum-private.h"

typedef struct CallbackContext {
  uint8_t* bytes;
  size_t size;
  int64_t start_unix_time_ms;
  int64_t end_unix_time_ms;
  uint32_t result;
  uint8_t is_last;
  int allocation_failed;
  int calls;
} CallbackContext;

static void DD_WIN_PROF_CALL
CaptureProfile(const ProfilingData* data, void* user_data) {
  CallbackContext* context = (CallbackContext*)user_data;
  uint8_t* copy = NULL;
  context->calls++;
  context->result = data->result;
  context->is_last = data->is_last;
  context->start_unix_time_ms = data->start_unix_time_ms;
  context->end_unix_time_ms = data->end_unix_time_ms;
  if (data->result != 0 || data->pprof_size == 0) {
    return;
  }
  copy = (uint8_t*)malloc(data->pprof_size);
  if (copy == NULL) {
    context->allocation_failed = 1;
    return;
  }
  memcpy(copy, data->pprof_bytes, data->pprof_size);
  context->bytes = copy;
  context->size = data->pprof_size;
}

int main(void) {
  CallbackContext context = {0};
  ProfilerConfig config = {0};
  int success = 0;
  config.size = sizeof(config);
  config.noEnvVars = true;
  config.uploadIntervalSeconds = 3600;

  if (!SetupProfilingDataCallback(CaptureProfile, &context)) {
    fprintf(stderr, "Failed to register C callback\n");
    return 1;
  }
  if (!SetupProfiler(&config) || !StartProfiler()) {
    fprintf(stderr, "Failed to configure or start profiler\n");
    SetupProfilingDataCallback(NULL, NULL);
    return 1;
  }
  StopProfiler();
  if (!SetupProfilingDataCallback(NULL, NULL)) {
    fprintf(stderr, "Failed to unregister C callback\n");
    free(context.bytes);
    return 1;
  }

  success = context.calls == 1 && !context.allocation_failed && context.result == 0 &&
            context.is_last && context.size >= 4 && context.bytes[0] == 0x28 &&
            context.bytes[1] == 0xb5 && context.bytes[2] == 0x2f &&
            context.bytes[3] == 0xfd && context.start_unix_time_ms > 0 &&
            context.end_unix_time_ms >= context.start_unix_time_ms;
  free(context.bytes);
  if (!success) {
    fprintf(stderr, "C callback result validation failed\n");
    return 1;
  }
  return 0;
}
