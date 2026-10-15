// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

// Linking dd-win-prof must expose dd-win-prof.h and nothing else from its source
// directory: these names would shadow a consuming project's own headers.
#if __has_include("Log.h") || __has_include("Uuid.h") || __has_include("version.h") || \
    __has_include("pch.h") || __has_include("framework.h")
#error "dd-win-prof exposes internal headers to consumers"
#endif

// Deliberately the first include: the public header must be self-contained.
#include "dd-win-prof.h"

int main() {
  StopProfiler();
  return 0;
}
