#include "Diagnostics.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace diagnostics {
namespace {

constexpr size_t kLineCount = 32;
constexpr size_t kLineLength = 256;

char lines[kLineCount][kLineLength];
size_t nextLine = 0;
size_t storedLines = 0;

}  // namespace

void logf(const char* format, ...) {
  char message[kLineLength - 16];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(message, sizeof(message), format, arguments);
  va_end(arguments);

  char line[kLineLength];
  snprintf(line, sizeof(line), "%10lu | %s",
           static_cast<unsigned long>(millis()), message);
  Serial.println(line);

  strncpy(lines[nextLine], line, kLineLength - 1);
  lines[nextLine][kLineLength - 1] = '\0';
  nextLine = (nextLine + 1) % kLineCount;
  if (storedLines < kLineCount) {
    ++storedLines;
  }
}

String snapshot() {
  String output;
  output.reserve(storedLines * kLineLength);
  const size_t oldest =
      (nextLine + kLineCount - storedLines) % kLineCount;
  for (size_t index = 0; index < storedLines; ++index) {
    output += lines[(oldest + index) % kLineCount];
    output += '\n';
  }
  return output;
}

}  // namespace diagnostics
