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

#include <gtest/gtest.h>

#include "../../../src/infra/es/store/generated/store.pb.h"
#include "../../../src/infra/util/ProtoUtil.h"

namespace gringofts::test {

/// A byte sequence that is a valid protobuf `string` payload as far as the wire
/// format is concerned, but is NOT well-formed UTF-8 (0xFF is never legal).
/// This mirrors the "line online" scenario where the json trackingContext ends
/// up carrying raw/binary bytes.
static std::string invalidUtf8() {
  return std::string("json:\xff\xfe{corrupt}", 15);
}

TEST(ProtoUtilTest, IsValidUtf8AcceptsWellFormedSequences) {
  EXPECT_TRUE(ProtoUtil::isValidUtf8(""));
  EXPECT_TRUE(ProtoUtil::isValidUtf8("plain ascii"));
  EXPECT_TRUE(ProtoUtil::isValidUtf8("\xC3\xA9"));          /// é
  EXPECT_TRUE(ProtoUtil::isValidUtf8("\xE2\x82\xAC"));      /// €
  EXPECT_TRUE(ProtoUtil::isValidUtf8("\xF0\x9F\x98\x80"));  /// 😀
}

TEST(ProtoUtilTest, IsValidUtf8RejectsMalformedSequences) {
  EXPECT_FALSE(ProtoUtil::isValidUtf8("\xFF"));              /// illegal lead byte
  EXPECT_FALSE(ProtoUtil::isValidUtf8("\x80"));              /// stray continuation byte
  EXPECT_FALSE(ProtoUtil::isValidUtf8("\xE2\x82"));          /// truncated 3-byte seq
  EXPECT_FALSE(ProtoUtil::isValidUtf8("\xC0\xAF"));          /// overlong encoding
  EXPECT_FALSE(ProtoUtil::isValidUtf8(invalidUtf8()));
}

TEST(ProtoUtilTest, CheckedSerializeSucceedsForValidPayload) {
  es::RaftPayload payload;
  auto *command = payload.mutable_command();
  command->set_type(1);
  command->set_id(42);
  command->set_trackingcontext(R"({"traceId":"abc-123"})");
  command->set_entry(std::string("\x00\x01\x02\xff", 4));  /// bytes may hold anything

  auto *event = payload.add_events();
  event->set_type(2);
  event->set_trackingcontext(R"({"span":"ok"})");

  std::string out;
  EXPECT_TRUE(ProtoUtil::checkedSerializeToString(payload, &out));
  EXPECT_FALSE(out.empty());

  /// round-trip to prove the produced payload is well-formed.
  es::RaftPayload parsed;
  EXPECT_TRUE(parsed.ParseFromString(out));
  EXPECT_EQ(parsed.command().id(), 42u);
}

TEST(ProtoUtilTest, BytesFieldWithNonUtf8DoesNotTriggerFailure) {
  es::CommandEntry command;
  command.set_id(7);
  command.set_entry(invalidUtf8());  /// `entry` is a `bytes` field, must be tolerated

  std::string out;
  EXPECT_TRUE(ProtoUtil::checkedSerializeToString(command, &out));
}

/// Reproduces the online issue: a `string` field carrying invalid UTF-8. The
/// checked serializer must detect it and report false so the caller can capture
/// the evidence instead of silently persisting a corrupt payload.
TEST(ProtoUtilTest, InvalidUtf8InTopLevelStringFieldIsDetected) {
  es::CommandEntry command;
  command.set_id(7);
  command.set_trackingcontext(invalidUtf8());

  std::string out;
  EXPECT_FALSE(ProtoUtil::checkedSerializeToString(command, &out));
  EXPECT_FALSE(ProtoUtil::validateUtf8Fields(command));
}

TEST(ProtoUtilTest, InvalidUtf8InNestedMessageIsDetected) {
  es::RaftPayload payload;
  payload.mutable_command()->set_id(7);
  payload.mutable_command()->set_trackingcontext(invalidUtf8());

  std::string out;
  EXPECT_FALSE(ProtoUtil::checkedSerializeToString(payload, &out));
}

TEST(ProtoUtilTest, InvalidUtf8InRepeatedNestedMessageIsDetected) {
  es::RaftPayload payload;
  payload.mutable_command()->set_id(7);
  payload.mutable_command()->set_trackingcontext(R"({"ok":true})");

  auto *event = payload.add_events();
  event->set_id(1);
  event->set_trackingcontext(invalidUtf8());

  std::string out;
  EXPECT_FALSE(ProtoUtil::checkedSerializeToString(payload, &out));
}

TEST(ProtoUtilTest, NullOutputPointerIsRejected) {
  es::CommandEntry command;
  command.set_id(1);
  EXPECT_FALSE(ProtoUtil::checkedSerializeToString(command, nullptr));
}

}  /// namespace gringofts::test
