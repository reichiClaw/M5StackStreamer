#include "TextEncoding.h"

#include <stddef.h>
#include <stdint.h>

namespace text_encoding {
namespace {

constexpr uint32_t kReplacementCodePoint = 0xfffd;

uint32_t decodeNext(const std::string& input, size_t& offset) {
  const uint8_t first = static_cast<uint8_t>(input[offset]);
  if (first < 0x80) {
    ++offset;
    return first;
  }

  size_t continuationCount = 0;
  uint32_t codePoint = 0;
  uint32_t minimumCodePoint = 0;
  if (first >= 0xc2 && first <= 0xdf) {
    continuationCount = 1;
    codePoint = first & 0x1f;
    minimumCodePoint = 0x80;
  } else if (first >= 0xe0 && first <= 0xef) {
    continuationCount = 2;
    codePoint = first & 0x0f;
    minimumCodePoint = 0x800;
  } else if (first >= 0xf0 && first <= 0xf4) {
    continuationCount = 3;
    codePoint = first & 0x07;
    minimumCodePoint = 0x10000;
  } else {
    ++offset;
    return kReplacementCodePoint;
  }

  if (offset + continuationCount >= input.size()) {
    ++offset;
    return kReplacementCodePoint;
  }

  for (size_t index = 1; index <= continuationCount; ++index) {
    const uint8_t next = static_cast<uint8_t>(input[offset + index]);
    if ((next & 0xc0) != 0x80) {
      ++offset;
      return kReplacementCodePoint;
    }
    codePoint = (codePoint << 6) | (next & 0x3f);
  }
  offset += continuationCount + 1;

  if (codePoint < minimumCodePoint || codePoint > 0x10ffff ||
      (codePoint >= 0xd800 && codePoint <= 0xdfff)) {
    return kReplacementCodePoint;
  }
  return codePoint;
}

void appendCp437(uint32_t codePoint, std::string& output) {
  if (codePoint <= 0x7f) {
    output.push_back(static_cast<char>(codePoint));
    return;
  }

  uint8_t encoded = 0;
  switch (codePoint) {
    case 0x00c4:  // Ä
      encoded = 0x8e;
      break;
    case 0x00d6:  // Ö
      encoded = 0x99;
      break;
    case 0x00dc:  // Ü
      encoded = 0x9a;
      break;
    case 0x00e4:  // ä
      encoded = 0x84;
      break;
    case 0x00f6:  // ö
      encoded = 0x94;
      break;
    case 0x00fc:  // ü
      encoded = 0x81;
      break;
    case 0x00df:  // ß
      encoded = 0xe1;
      break;
    case 0x1e9e:  // ẞ is not present in CP437.
      output.append("SS");
      return;
    default:
      output.push_back('?');
      return;
  }
  output.push_back(static_cast<char>(encoded));
}

}  // namespace

std::string utf8ToCp437(const std::string& input) {
  std::string output;
  output.reserve(input.size());

  size_t offset = 0;
  while (offset < input.size()) {
    appendCp437(decodeNext(input, offset), output);
  }
  return output;
}

}  // namespace text_encoding
