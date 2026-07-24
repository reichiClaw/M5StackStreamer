#pragma once

#include <string>

namespace text_encoding {

// Converts a UTF-8 receiver name to the single-byte IBM CP437 glyph indices
// used by M5GFX's built-in Font0. Unsupported code points become '?'.
std::string utf8ToCp437(const std::string& input);

}  // namespace text_encoding
