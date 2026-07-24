#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace cast_protocol {

// Receiver status messages are normally only a few kilobytes. Keeping a hard
// ceiling protects the ESP32 heap from a malformed or hostile LAN peer.
constexpr size_t kMaximumMessageSize = 16 * 1024;

struct Message {
  uint32_t protocolVersion = 0;
  std::string sourceId;
  std::string destinationId;
  std::string nameSpace;
  uint32_t payloadType = 0;
  std::string payloadUtf8;
};

// Encodes the protobuf CastMessage body. The four-byte transport length is
// deliberately not included.
bool encodeStringMessage(const std::string& sourceId,
                         const std::string& destinationId,
                         const std::string& nameSpace,
                         const std::string& payloadUtf8,
                         std::vector<uint8_t>& output);

// Decodes the fields used by a Cast V2 JSON channel. Unknown protobuf fields
// are skipped so messages remain compatible with newer receivers.
bool decodeMessage(const uint8_t* data, size_t length, Message& output);

}  // namespace cast_protocol
