// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <cstdint>
#include <future>
#include <string>

#include "internal/error.h"
#include "pch.h"

TEST(ProfilingErrorMessageTests, KnownCallbackCodesHaveStaticMessages) {
  for (const auto code :
       {ProfilingErrorCode::None,
        ProfilingErrorCode::SerializationFailed,
        ProfilingErrorCode::ProfileBytesUnavailable,
        ProfilingErrorCode::ProfileResetFailed,
        ProfilingErrorCode::SampleLabelsUnavailable}) {
    SCOPED_TRACE(static_cast<uint32_t>(code));
    const auto result = static_cast<uint32_t>(code);
    const char* message = GetProfilingErrorMessage(result);
    ASSERT_NE(message, nullptr);
    EXPECT_STRNE(message, "");
    EXPECT_STRNE(message, "Unknown profiling error");
    EXPECT_EQ(message, GetProfilingErrorMessage(result));
  }
}

TEST(ProfilingErrorMessageTests, UnknownCodesHaveFallbackMessage) {
  EXPECT_STREQ(GetProfilingErrorMessage(5), "Unknown profiling error");
  EXPECT_STREQ(GetProfilingErrorMessage(UINT32_MAX), "Unknown profiling error");
}

TEST(ProfilingErrorMessageTests, LookupDoesNotInvalidateEarlierMessages) {
  const char* message = GetProfilingErrorMessage(1);
  const std::string saved = message;
  GetProfilingErrorMessage(3);
  GetProfilingErrorMessage(UINT32_MAX);
  std::async(std::launch::async, [] { GetProfilingErrorMessage(2); }).get();
  EXPECT_STREQ(message, saved.c_str());
  EXPECT_EQ(message, GetProfilingErrorMessage(1));
}
