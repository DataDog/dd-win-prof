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
      Sample&& sample,
      std::chrono::nanoseconds hangDuration,
      bool hang
  ) {
    auto offsets = GetValueOffsets();
    sample.AddValue(0, offsets[0]);  // no wall time for hang sample
    sample.AddValue(hangDuration.count(), offsets[1]);

    if (hang) {
    // TODO: add "UIHang=true" label to the sample
    }
    else {
    // TODO: add "UIHang=false" label to the sample
    }

    CollectorBase::Add(std::move(sample));
  }

  static std::vector<SampleValueType> SampleTypeDefinitions;
};
