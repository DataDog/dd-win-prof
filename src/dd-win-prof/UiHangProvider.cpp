// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "UiHangProvider.h"

#include "pch.h"

std::vector<SampleValueType> UiHangProvider::SampleTypeDefinitions(
    {{"wall-time", "nanoseconds"}, {"wait-time", "nanoseconds"}}
);

UiHangProvider::UiHangProvider(SampleValueTypeProvider& valueTypeProvider)
    : CollectorBase(
          "UiHangProvider", valueTypeProvider.GetOrRegister(SampleTypeDefinitions)
      ) {}
