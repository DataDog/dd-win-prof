// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include "CollectorBase.h"
#include "SampleValueTypeProvider.h"
#include "pch.h"

class UiHangProvider : public CollectorBase {
 public:
  UiHangProvider(SampleValueTypeProvider& valueTypeProvider);

  inline void Add(
      Sample&& sample, std::chrono::nanoseconds hangDuration, UiHangSampleKind kind
  ) {
    auto offsets = GetValueOffsets();
    sample.AddValue(hangDuration.count(), offsets[0]);

    // the "UIHang" labels are attached at export time in ProfileExporter
    sample.SetUiHangSampleKind(kind);

    CollectorBase::Add(std::move(sample));
  }

  static std::vector<SampleValueType> SampleTypeDefinitions;
};
