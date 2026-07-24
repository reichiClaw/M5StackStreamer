#include <unity.h>

#include <string>
#include <vector>

#include "CastProtocol.h"
#include "TextEncoding.h"

void setUp() {}
void tearDown() {}

void test_round_trip_string_message() {
  std::vector<uint8_t> encoded;
  TEST_ASSERT_TRUE(cast_protocol::encodeStringMessage(
      "sender-0", "receiver-0", "urn:x-cast:test", "{\"type\":\"PING\"}",
      encoded));

  cast_protocol::Message decoded;
  TEST_ASSERT_TRUE(
      cast_protocol::decodeMessage(encoded.data(), encoded.size(), decoded));
  TEST_ASSERT_EQUAL_UINT32(0, decoded.protocolVersion);
  TEST_ASSERT_EQUAL_STRING("sender-0", decoded.sourceId.c_str());
  TEST_ASSERT_EQUAL_STRING("receiver-0", decoded.destinationId.c_str());
  TEST_ASSERT_EQUAL_STRING("urn:x-cast:test", decoded.nameSpace.c_str());
  TEST_ASSERT_EQUAL_UINT32(0, decoded.payloadType);
  TEST_ASSERT_EQUAL_STRING("{\"type\":\"PING\"}",
                           decoded.payloadUtf8.c_str());
}

void test_encoder_emits_required_zero_enum_fields() {
  std::vector<uint8_t> encoded;
  TEST_ASSERT_TRUE(cast_protocol::encodeStringMessage(
      "sender-0", "receiver-0", "ns", "{}", encoded));

  TEST_ASSERT_TRUE(encoded.size() >= 4);
  TEST_ASSERT_EQUAL_HEX8(0x08, encoded[0]);
  TEST_ASSERT_EQUAL_HEX8(0x00, encoded[1]);

  bool foundPayloadType = false;
  for (size_t index = 2; index + 1 < encoded.size(); ++index) {
    if (encoded[index] == 0x28 && encoded[index + 1] == 0x00) {
      foundPayloadType = true;
      break;
    }
  }
  TEST_ASSERT_TRUE(foundPayloadType);
}

void test_decoder_skips_unknown_fields() {
  std::vector<uint8_t> encoded;
  TEST_ASSERT_TRUE(cast_protocol::encodeStringMessage(
      "a", "b", "namespace", "payload", encoded));

  // Unknown field 10, varint wire type, value 150.
  encoded.push_back(0x50);
  encoded.push_back(0x96);
  encoded.push_back(0x01);

  cast_protocol::Message decoded;
  TEST_ASSERT_TRUE(
      cast_protocol::decodeMessage(encoded.data(), encoded.size(), decoded));
  TEST_ASSERT_EQUAL_STRING("payload", decoded.payloadUtf8.c_str());
}

void test_decoder_rejects_truncated_length_delimited_field() {
  const uint8_t malformed[] = {
      0x08, 0x00,  // protocol version
      0x12, 0x05, 'a',  // source says five bytes, only one follows
  };
  cast_protocol::Message decoded;
  TEST_ASSERT_FALSE(cast_protocol::decodeMessage(
      malformed, sizeof(malformed), decoded));
}

void test_encoder_rejects_oversized_payload() {
  const std::string oversized(cast_protocol::kMaximumMessageSize + 1, 'x');
  std::vector<uint8_t> encoded;
  TEST_ASSERT_FALSE(cast_protocol::encodeStringMessage(
      "sender", "receiver", "namespace", oversized, encoded));
  TEST_ASSERT_TRUE(encoded.empty());
}

void test_receiver_name_converts_german_characters_to_cp437() {
  const std::string converted =
      text_encoding::utf8ToCp437(u8"ÄÖÜ äöü ß ẞ");
  const uint8_t expected[] = {
      0x8e, 0x99, 0x9a, ' ', 0x84, 0x94, 0x81,
      ' ',  0xe1, ' ',  'S', 'S',
  };

  TEST_ASSERT_EQUAL_size_t(sizeof(expected), converted.size());
  TEST_ASSERT_EQUAL_MEMORY(expected, converted.data(), sizeof(expected));
}

void test_receiver_name_preserves_ascii_and_replaces_unknown_unicode() {
  const std::string converted =
      text_encoding::utf8ToCp437(u8"Living Room 🏠");
  TEST_ASSERT_EQUAL_STRING("Living Room ?", converted.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_round_trip_string_message);
  RUN_TEST(test_encoder_emits_required_zero_enum_fields);
  RUN_TEST(test_decoder_skips_unknown_fields);
  RUN_TEST(test_decoder_rejects_truncated_length_delimited_field);
  RUN_TEST(test_encoder_rejects_oversized_payload);
  RUN_TEST(test_receiver_name_converts_german_characters_to_cp437);
  RUN_TEST(test_receiver_name_preserves_ascii_and_replaces_unknown_unicode);
  return UNITY_END();
}
