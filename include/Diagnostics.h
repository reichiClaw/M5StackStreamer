#pragma once

#include <Arduino.h>

namespace diagnostics {

// Allocates the large ring in PSRAM when available.
void begin();

// Writes a timestamped line to Serial and a fixed-size in-memory ring buffer.
// Wi-Fi credentials and stream audio are never recorded.
void logf(const char* format, ...)
    __attribute__((format(printf, 1, 2)));

// Returns the current ring buffer in chronological order.
String snapshot();

// Streams the full retained ring without constructing a large temporary
// String. contentLength() is the exact byte count emitted by writeTo().
size_t contentLength();
bool writeTo(Print& output);

}  // namespace diagnostics
