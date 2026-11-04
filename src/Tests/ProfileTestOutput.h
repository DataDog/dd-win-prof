// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <Windows.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <span>
#include <string>

inline void WriteTestProfile(const std::string& name, std::span<const uint8_t> bytes) {
  wchar_t directory[MAX_PATH];
  const auto length =
      GetEnvironmentVariableW(L"DD_WIN_PROF_TEST_PROFILE_DIR", directory, MAX_PATH);
  if (length == 0) return;
  ASSERT_LT(length, MAX_PATH);
  std::ofstream output(std::filesystem::path(directory) / name, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  ASSERT_TRUE(output.good());
}

inline void WriteHttpTestProfile(const std::string& name, const std::string& body) {
  const auto lineEnd = body.find("\r\n");
  ASSERT_NE(lineEnd, std::string::npos);
  const auto part = body.find("filename=\"profile.pprof\"");
  ASSERT_NE(part, std::string::npos);
  const auto headersEnd = body.find("\r\n\r\n", part);
  ASSERT_NE(headersEnd, std::string::npos);
  const auto start = headersEnd + 4;
  const auto end = body.find("\r\n" + body.substr(0, lineEnd), start);
  ASSERT_NE(end, std::string::npos);
  WriteTestProfile(
      name, {reinterpret_cast<const uint8_t*>(body.data() + start), end - start}
  );
}
