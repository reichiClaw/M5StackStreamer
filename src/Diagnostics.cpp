#include "Diagnostics.h"

#include <esp_heap_caps.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace diagnostics {
namespace {

constexpr size_t kFallbackLineCount = 32;
constexpr size_t kPsramLineCount = 256;
constexpr size_t kLineLength = 256;

char fallbackLines[kFallbackLineCount][kLineLength];
char* lineStorage = &fallbackLines[0][0];
size_t lineCapacity = kFallbackLineCount;
size_t nextLine = 0;
size_t storedLines = 0;
uint64_t totalLines = 0;
bool initialized = false;

char* lineAt(size_t index) {
  return lineStorage + index * kLineLength;
}

void ensureInitialized() {
  if (!initialized) {
    begin();
  }
}

size_t writeHeader(char* buffer, size_t bufferLength) {
  const uint64_t dropped =
      totalLines > storedLines ? totalLines - storedLines : 0;
  const int length = snprintf(
      buffer, bufferLength,
      "# retained=%u capacity=%u total=%llu dropped=%llu\n",
      static_cast<unsigned int>(storedLines),
      static_cast<unsigned int>(lineCapacity),
      static_cast<unsigned long long>(totalLines),
      static_cast<unsigned long long>(dropped));
  return length > 0 ? static_cast<size_t>(length) : 0;
}

// The log is also served while a Cast session is maintained; the stall
// budget must stay well below the 10-second heartbeat interval so a slow
// HTTP client cannot starve the Cast connection.
constexpr uint32_t kWriteStallTimeoutMs = 2000;

bool writeAll(Print& output, const uint8_t* data, size_t length) {
  size_t offset = 0;
  uint32_t lastProgressAt = millis();
  while (offset < length) {
    const size_t written = output.write(data + offset, length - offset);
    if (written > 0) {
      offset += written;
      lastProgressAt = millis();
      continue;
    }
    if (millis() - lastProgressAt >= kWriteStallTimeoutMs) {
      return false;
    }
    delay(1);
  }
  return true;
}

}  // namespace

void begin() {
  if (initialized) {
    return;
  }
  const size_t requestedBytes = kPsramLineCount * kLineLength;
  void* psram =
      heap_caps_malloc(requestedBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (psram != nullptr) {
    lineStorage = static_cast<char*>(psram);
    lineCapacity = kPsramLineCount;
  }
  initialized = true;
  Serial.printf("Diagnostics: %u lines (%u bytes), storage=%s\n",
                static_cast<unsigned int>(lineCapacity),
                static_cast<unsigned int>(lineCapacity * kLineLength),
                psram == nullptr ? "internal fallback" : "PSRAM");
}

void logf(const char* format, ...) {
  ensureInitialized();
  char message[kLineLength - 16];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(message, sizeof(message), format, arguments);
  va_end(arguments);

  char line[kLineLength];
  snprintf(line, sizeof(line), "%10lu | %s",
           static_cast<unsigned long>(millis()), message);
  Serial.println(line);

  char* destination = lineAt(nextLine);
  strncpy(destination, line, kLineLength - 1);
  destination[kLineLength - 1] = '\0';
  nextLine = (nextLine + 1) % lineCapacity;
  if (storedLines < lineCapacity) {
    ++storedLines;
  }
  ++totalLines;
}

String snapshot() {
  ensureInitialized();
  String output;
  output.reserve(contentLength());
  char header[96];
  const size_t headerLength = writeHeader(header, sizeof(header));
  output.concat(header, headerLength);
  const size_t oldest =
      (nextLine + lineCapacity - storedLines) % lineCapacity;
  for (size_t index = 0; index < storedLines; ++index) {
    output += lineAt((oldest + index) % lineCapacity);
    output += '\n';
  }
  return output;
}

size_t contentLength() {
  ensureInitialized();
  char header[96];
  size_t length = writeHeader(header, sizeof(header));
  const size_t oldest =
      (nextLine + lineCapacity - storedLines) % lineCapacity;
  for (size_t index = 0; index < storedLines; ++index) {
    length += strlen(lineAt((oldest + index) % lineCapacity)) + 1;
  }
  return length;
}

bool writeTo(Print& output) {
  ensureInitialized();
  char header[96];
  const size_t headerLength = writeHeader(header, sizeof(header));
  if (!writeAll(output, reinterpret_cast<const uint8_t*>(header),
                headerLength)) {
    return false;
  }
  const size_t oldest =
      (nextLine + lineCapacity - storedLines) % lineCapacity;
  for (size_t index = 0; index < storedLines; ++index) {
    const char* line = lineAt((oldest + index) % lineCapacity);
    if (!writeAll(output, reinterpret_cast<const uint8_t*>(line),
                  strlen(line)) ||
        !writeAll(output, reinterpret_cast<const uint8_t*>("\n"), 1)) {
      return false;
    }
  }
  return true;
}

}  // namespace diagnostics
