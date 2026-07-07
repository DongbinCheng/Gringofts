/************************************************************************
Copyright 2019-2020 eBay Inc.
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
**************************************************************************/

#ifndef SRC_INFRA_UTIL_PROTOUTIL_H_
#define SRC_INFRA_UTIL_PROTOUTIL_H_

#include <execinfo.h>
#include <stdlib.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <spdlog/spdlog.h>

#include "StrUtil.h"

namespace gringofts {

/**
 * Diagnostics around protobuf serialization.
 *
 * Historically a payload could be silently corrupted while being persisted into
 * the Raft log because:
 *   1. `RaftLogStore` ignored the boolean returned by `SerializeToString`;
 *   2. proto3 `string` fields (e.g. the json `trackingContext`) carried invalid
 *      UTF-8, which protobuf reports via a log line but, depending on the
 *      version, may not surface as a serialization failure.
 *
 * This helper centralises the "catch the evidence" logic: it validates the
 * message, checks every `string` field for well-formed UTF-8 and, on any
 * failure, logs the message type, the offending value in hex and a stack trace
 * so the next reproduction can be diagnosed directly from the logs.
 */
class ProtoUtil final {
 public:
  /// Upper bound on the number of bytes dumped as hex when logging evidence,
  /// so that a large payload does not flood the logs.
  static constexpr std::size_t kMaxHexDumpBytes = 256;

  /**
   * Check whether @p str is a well-formed UTF-8 byte sequence, following the
   * ranges defined by RFC 3629 (the same requirement proto3 imposes on `string`
   * fields).
   */
  static bool isValidUtf8(const std::string &str) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(str.data());
    const std::size_t len = str.size();

    std::size_t i = 0;
    while (i < len) {
      const unsigned char c = bytes[i];
      std::size_t extra = 0;
      unsigned char lower = 0x80;
      unsigned char upper = 0xBF;

      if (c <= 0x7F) {
        extra = 0;
      } else if (c >= 0xC2 && c <= 0xDF) {
        extra = 1;
      } else if (c == 0xE0) {
        extra = 2;
        lower = 0xA0;
      } else if (c >= 0xE1 && c <= 0xEC) {
        extra = 2;
      } else if (c == 0xED) {
        extra = 2;
        upper = 0x9F;
      } else if (c >= 0xEE && c <= 0xEF) {
        extra = 2;
      } else if (c == 0xF0) {
        extra = 3;
        lower = 0x90;
      } else if (c >= 0xF1 && c <= 0xF3) {
        extra = 3;
      } else if (c == 0xF4) {
        extra = 3;
        upper = 0x8F;
      } else {
        /// 0xC0, 0xC1, 0xF5..0xFF and stray continuation bytes are illegal leads
        return false;
      }

      if (i + extra >= len) {
        return false;  /// truncated multi-byte sequence
      }
      for (std::size_t j = 1; j <= extra; ++j) {
        const unsigned char cont = bytes[i + j];
        const unsigned char lo = (j == 1) ? lower : 0x80;
        const unsigned char hi = (j == 1) ? upper : 0xBF;
        if (cont < lo || cont > hi) {
          return false;
        }
      }
      i += extra + 1;
    }
    return true;
  }

  /**
   * Capture the current stack trace as a printable string, used to pinpoint the
   * caller that produced a bad payload.
   */
  static std::string stackTrace(int maxFrames = 32) {
    std::vector<void *> frames(maxFrames);
    const int size = ::backtrace(frames.data(), maxFrames);

    std::string result;
    char **symbols = ::backtrace_symbols(frames.data(), size);
    if (symbols != nullptr) {
      for (int i = 0; i < size; ++i) {
        result += symbols[i];
        result += '\n';
      }
      ::free(symbols);
    }
    return result;
  }

  /**
   * Recursively scan every populated `string` field (including nested and
   * repeated messages) and verify it holds valid UTF-8. On the first offending
   * value it logs the field path, the value in hex and a stack trace, then
   * returns false. `bytes` fields are intentionally skipped since they are
   * allowed to hold arbitrary octets.
   */
  static bool validateUtf8Fields(const google::protobuf::Message &message,
                                 const std::string &path = "") {
    const auto *reflection = message.GetReflection();
    std::vector<const google::protobuf::FieldDescriptor *> fields;
    reflection->ListFields(message, &fields);

    for (const auto *field : fields) {
      const std::string fieldPath = path.empty() ? field->name() : path + "." + field->name();

      if (field->type() == google::protobuf::FieldDescriptor::TYPE_STRING) {
        if (field->is_repeated()) {
          const int count = reflection->FieldSize(message, field);
          for (int i = 0; i < count; ++i) {
            std::string scratch;
            const std::string &value =
                reflection->GetRepeatedStringReference(message, field, i, &scratch);
            if (!isValidUtf8(value)) {
              logInvalidUtf8(fieldPath + "[" + std::to_string(i) + "]", value);
              return false;
            }
          }
        } else {
          std::string scratch;
          const std::string &value = reflection->GetStringReference(message, field, &scratch);
          if (!isValidUtf8(value)) {
            logInvalidUtf8(fieldPath, value);
            return false;
          }
        }
      } else if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
        if (field->is_repeated()) {
          const int count = reflection->FieldSize(message, field);
          for (int i = 0; i < count; ++i) {
            if (!validateUtf8Fields(reflection->GetRepeatedMessage(message, field, i),
                                    fieldPath + "[" + std::to_string(i) + "]")) {
              return false;
            }
          }
        } else {
          if (!validateUtf8Fields(reflection->GetMessage(message, field), fieldPath)) {
            return false;
          }
        }
      }
    }
    return true;
  }

  /**
   * Serialize @p message into @p out while guarding against the failure modes
   * that used to corrupt the Raft payload silently. Logs full evidence (type,
   * initialization error, hex dump, stack trace) on any problem and returns
   * true only when serialization is safe.
   */
  static bool checkedSerializeToString(const google::protobuf::Message &message, std::string *out) {
    if (out == nullptr) {
      return false;
    }
    const std::string &typeName = message.GetTypeName();

    if (!message.IsInitialized()) {
      SPDLOG_ERROR("Serialize {} failed: message not initialized, missing fields=[{}]\nstacktrace:\n{}",
                   typeName, message.InitializationErrorString(), stackTrace());
      return false;
    }

    /// Validate before serializing so that the offending field is reported even
    /// when protobuf itself does not fail the serialization.
    const bool utf8Ok = validateUtf8Fields(message);

    if (!message.SerializeToString(out)) {
      const std::size_t dumpLen = std::min(out->size(), kMaxHexDumpBytes);
      SPDLOG_ERROR("Serialize {} failed: SerializeToString returned false, byteSize={}, produced={}B, "
                   "hex(first {}B)={}\nstacktrace:\n{}",
                   typeName, message.ByteSizeLong(), out->size(), dumpLen,
                   StrUtil::hexStr(reinterpret_cast<const unsigned char *>(out->data()), dumpLen),
                   stackTrace());
      return false;
    }

    if (!utf8Ok) {
      SPDLOG_ERROR("Serialize {} produced a payload with invalid UTF-8 string field(s); "
                   "payload may be rejected by strict parsers, byteSize={}", typeName, out->size());
      return false;
    }
    return true;
  }

 private:
  static void logInvalidUtf8(const std::string &fieldPath, const std::string &value) {
    const std::size_t dumpLen = std::min(value.size(), kMaxHexDumpBytes);
    SPDLOG_ERROR("Invalid UTF-8 detected in proto string field '{}', size={}, hex(first {}B)={}{}\nstacktrace:\n{}",
                 fieldPath, value.size(), dumpLen,
                 StrUtil::hexStr(reinterpret_cast<const unsigned char *>(value.data()), dumpLen),
                 value.size() > dumpLen ? "..." : "",
                 stackTrace());
  }
};

}  /// namespace gringofts

#endif  // SRC_INFRA_UTIL_PROTOUTIL_H_
