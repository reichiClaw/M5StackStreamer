#include "CastProtocol.h"

#include <limits>

namespace cast_protocol {
namespace {

bool appendVarint(uint64_t value, std::vector<uint8_t>& output) {
  do {
    if (output.size() >= kMaximumMessageSize) {
      return false;
    }

    uint8_t byte = static_cast<uint8_t>(value & 0x7f);
    value >>= 7;
    if (value != 0) {
      byte |= 0x80;
    }
    output.push_back(byte);
  } while (value != 0);

  return true;
}

bool appendLengthDelimited(uint32_t fieldNumber,
                           const std::string& value,
                           std::vector<uint8_t>& output) {
  if (value.size() > kMaximumMessageSize ||
      !appendVarint((static_cast<uint64_t>(fieldNumber) << 3) | 2, output) ||
      !appendVarint(value.size(), output) ||
      output.size() > kMaximumMessageSize - value.size()) {
    return false;
  }

  output.insert(output.end(), value.begin(), value.end());
  return true;
}

bool readVarint(const uint8_t* data,
                size_t length,
                size_t& offset,
                uint64_t& value) {
  value = 0;
  for (unsigned int shift = 0; shift < 64; shift += 7) {
    if (offset >= length) {
      return false;
    }

    const uint8_t byte = data[offset++];
    if (shift == 63 && (byte & 0xfe) != 0) {
      return false;
    }
    value |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) {
      return true;
    }
  }
  return false;
}

bool readString(const uint8_t* data,
                size_t length,
                size_t& offset,
                std::string& value) {
  uint64_t encodedLength = 0;
  if (!readVarint(data, length, offset, encodedLength) ||
      encodedLength > length - offset ||
      encodedLength > kMaximumMessageSize) {
    return false;
  }

  const size_t stringLength = static_cast<size_t>(encodedLength);
  value.assign(reinterpret_cast<const char*>(data + offset), stringLength);
  offset += stringLength;
  return true;
}

bool skipField(uint32_t wireType,
               const uint8_t* data,
               size_t length,
               size_t& offset) {
  uint64_t ignored = 0;
  switch (wireType) {
    case 0:
      return readVarint(data, length, offset, ignored);
    case 1:
      if (length - offset < 8) {
        return false;
      }
      offset += 8;
      return true;
    case 2:
      if (!readVarint(data, length, offset, ignored) ||
          ignored > length - offset) {
        return false;
      }
      offset += static_cast<size_t>(ignored);
      return true;
    case 5:
      if (length - offset < 4) {
        return false;
      }
      offset += 4;
      return true;
    default:
      return false;
  }
}

}  // namespace

bool encodeStringMessage(const std::string& sourceId,
                         const std::string& destinationId,
                         const std::string& nameSpace,
                         const std::string& payloadUtf8,
                         std::vector<uint8_t>& output) {
  output.clear();
  size_t estimatedSize = 32;
  const size_t fieldSizes[] = {sourceId.size(), destinationId.size(),
                               nameSpace.size(), payloadUtf8.size()};
  for (size_t fieldSize : fieldSizes) {
    if (fieldSize > kMaximumMessageSize - estimatedSize) {
      return false;
    }
    estimatedSize += fieldSize;
  }
  output.reserve(estimatedSize);

  // CastMessage is a proto2 message, so required enum fields must be emitted
  // even though both values are zero.
  if (!appendVarint(1 << 3, output) || !appendVarint(0, output) ||
      !appendLengthDelimited(2, sourceId, output) ||
      !appendLengthDelimited(3, destinationId, output) ||
      !appendLengthDelimited(4, nameSpace, output) ||
      !appendVarint(5 << 3, output) || !appendVarint(0, output) ||
      !appendLengthDelimited(6, payloadUtf8, output)) {
    output.clear();
    return false;
  }

  return output.size() <= kMaximumMessageSize;
}

bool decodeMessage(const uint8_t* data, size_t length, Message& output) {
  if (data == nullptr || length == 0 || length > kMaximumMessageSize) {
    return false;
  }

  output = Message();
  bool hasProtocolVersion = false;
  bool hasSourceId = false;
  bool hasDestinationId = false;
  bool hasNamespace = false;
  bool hasPayloadType = false;
  size_t offset = 0;

  while (offset < length) {
    uint64_t tag = 0;
    if (!readVarint(data, length, offset, tag) || tag == 0) {
      return false;
    }

    const uint32_t fieldNumber = static_cast<uint32_t>(tag >> 3);
    const uint32_t wireType = static_cast<uint32_t>(tag & 0x07);
    uint64_t enumValue = 0;

    switch (fieldNumber) {
      case 1:
        if (wireType != 0 ||
            !readVarint(data, length, offset, enumValue) ||
            enumValue > std::numeric_limits<uint32_t>::max()) {
          return false;
        }
        output.protocolVersion = static_cast<uint32_t>(enumValue);
        hasProtocolVersion = true;
        break;
      case 2:
        if (wireType != 2 ||
            !readString(data, length, offset, output.sourceId)) {
          return false;
        }
        hasSourceId = true;
        break;
      case 3:
        if (wireType != 2 ||
            !readString(data, length, offset, output.destinationId)) {
          return false;
        }
        hasDestinationId = true;
        break;
      case 4:
        if (wireType != 2 ||
            !readString(data, length, offset, output.nameSpace)) {
          return false;
        }
        hasNamespace = true;
        break;
      case 5:
        if (wireType != 0 ||
            !readVarint(data, length, offset, enumValue) ||
            enumValue > std::numeric_limits<uint32_t>::max()) {
          return false;
        }
        output.payloadType = static_cast<uint32_t>(enumValue);
        hasPayloadType = true;
        break;
      case 6:
        if (wireType != 2 ||
            !readString(data, length, offset, output.payloadUtf8)) {
          return false;
        }
        break;
      default:
        if (!skipField(wireType, data, length, offset)) {
          return false;
        }
        break;
    }
  }

  return hasProtocolVersion && hasSourceId && hasDestinationId &&
         hasNamespace && hasPayloadType;
}

}  // namespace cast_protocol
