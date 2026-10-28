// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <algorithm>
#include <cstring>
#include <string_view>

#include "dd-win-prof-diagnostics.h"

namespace dd_win_prof {
inline size_t Utf8SequenceLength(std::string_view text, size_t offset) noexcept {
  const auto first = static_cast<unsigned char>(text[offset]);
  if (first > 0 && first < 0x80) {
    return 1;
  }
  size_t length = first >= 0xc2 && first <= 0xdf   ? 2
                  : first >= 0xe0 && first <= 0xef ? 3
                  : first >= 0xf0 && first <= 0xf4 ? 4
                                                   : 0;
  if (length == 0 || text.size() - offset < length) {
    return 0;
  }
  for (size_t i = 1; i < length; ++i) {
    if ((static_cast<unsigned char>(text[offset + i]) & 0xc0) != 0x80) {
      return 0;
    }
  }
  const auto second = static_cast<unsigned char>(text[offset + 1]);
  if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0) ||
      (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90)) {
    return 0;
  }
  return length;
}

inline size_t Utf8Prefix(std::string_view text, size_t capacity) noexcept {
  size_t length = (std::min)(text.size(), capacity);
  while (length != 0 && length < text.size() &&
         (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80) {
    --length;
  }
  return length;
}

class LifecycleDiagnostic {
 public:
  void Clear() noexcept {
    _severity = DD_WIN_PROF_SUCCESS;
    _code = DD_WIN_PROF_CODE_NONE;
    _message[0] = '\0';
    _length = 0;
  }

  int32_t Set(int32_t severity, uint32_t code, std::string_view message) noexcept {
    _severity = severity;
    _code = code;
    _length = 0;
    size_t offset = 0;
    while (offset < message.size() && _length < sizeof(_message) - 1) {
      const size_t length = Utf8SequenceLength(message, offset);
      if (length == 0) {
        _message[_length++] = '?';
        ++offset;
      } else {
        if (length > sizeof(_message) - 1 - _length) {
          break;
        }
        std::memcpy(_message + _length, message.data() + offset, length);
        _length += length;
        offset += length;
      }
    }
    _message[_length] = '\0';
    return severity;
  }

  uint32_t Copy(int32_t* severity, uint32_t* code, char* message, uint32_t capacity)
      const noexcept {
    if (severity != nullptr) {
      *severity = _severity;
    }
    if (code != nullptr) {
      *code = _code;
    }
    if (message != nullptr && capacity != 0) {
      const size_t length = Utf8Prefix({_message, _length}, capacity - 1);
      if (length != 0) {
        std::memcpy(message, _message, length);
      }
      message[length] = '\0';
    }
    return _severity == DD_WIN_PROF_SUCCESS ? 0 : static_cast<uint32_t>(_length + 1);
  }

 private:
  int32_t _severity = DD_WIN_PROF_SUCCESS;
  uint32_t _code = DD_WIN_PROF_CODE_NONE;
  char _message[1024]{};
  size_t _length = 0;
};
}  // namespace dd_win_prof
