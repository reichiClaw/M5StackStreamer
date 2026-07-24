#include "CastClient.h"

#include <ArduinoJson.h>
#include <esp_system.h>

#include <memory>
#include <new>
#include <vector>

namespace {

constexpr char kSourceId[] = "sender-0";
constexpr char kPlatformDestinationId[] = "receiver-0";
constexpr char kDefaultMediaReceiverAppId[] = "CC1AD845";
constexpr char kConnectionNamespace[] =
    "urn:x-cast:com.google.cast.tp.connection";
constexpr char kHeartbeatNamespace[] =
    "urn:x-cast:com.google.cast.tp.heartbeat";
constexpr char kReceiverNamespace[] = "urn:x-cast:com.google.cast.receiver";
constexpr char kMediaNamespace[] = "urn:x-cast:com.google.cast.media";

constexpr uint32_t kHeartbeatIntervalMs = 5000;
constexpr uint32_t kFrameCompletionTimeoutMs = 5000;

bool deadlineReached(uint32_t deadline) {
  return static_cast<int32_t>(millis() - deadline) >= 0;
}

String jsonReason(const JsonDocument& document) {
  const char* reason = document["reason"] | "";
  return reason[0] == '\0' ? String("unknown reason") : String(reason);
}

}  // namespace

CastClient::CastClient(StatusCallback statusCallback)
    : statusCallback_(statusCallback),
      requestId_((esp_random() & 0x3fffffffU) + 1),
      lastHeartbeatMs_(0) {}

bool CastClient::play(const IPAddress& address,
                      uint16_t port,
                      const char* url,
                      const char* contentType,
                      const char* title) {
  lastError_ = "";
  ReceiverApplication application;
  bool succeeded = false;

  report("Opening secure Cast connection");
  if (!open(address, port)) {
    return false;
  }

  do {
    if (!sendPayload(kPlatformDestinationId, kConnectionNamespace,
                     F("{\"type\":\"CONNECT\",\"origin\":{}}"))) {
      break;
    }

    const uint32_t statusRequestId = nextRequestId();
    JsonDocument statusRequest;
    statusRequest["type"] = "GET_STATUS";
    statusRequest["requestId"] = statusRequestId;
    String payload;
    if (statusRequest.overflowed() ||
        serializeJson(statusRequest, payload) == 0) {
      setError("Not enough memory for status request");
      break;
    }
    if (!sendPayload(kPlatformDestinationId, kReceiverNamespace, payload)) {
      break;
    }

    report("Checking receiver");
    AppWaitResult appResult =
        waitForReceiverApplication(statusRequestId, 4000, true, application);
    if (appResult == AppWaitResult::kError) {
      break;
    }

    if (appResult != AppWaitResult::kFound) {
      report("Starting media receiver");
      const uint32_t launchRequestId = nextRequestId();
      JsonDocument launchRequest;
      launchRequest["type"] = "LAUNCH";
      launchRequest["appId"] = kDefaultMediaReceiverAppId;
      launchRequest["requestId"] = launchRequestId;
      payload = "";
      if (launchRequest.overflowed() ||
          serializeJson(launchRequest, payload) == 0) {
        setError("Not enough memory for launch request");
        break;
      }
      if (!sendPayload(kPlatformDestinationId, kReceiverNamespace, payload)) {
        break;
      }

      appResult = waitForReceiverApplication(launchRequestId, 20000, false,
                                             application);
      if (appResult != AppWaitResult::kFound) {
        if (appResult != AppWaitResult::kError) {
          setError("Media receiver did not start");
        }
        break;
      }
    }

    report("Connecting to media receiver");
    if (!sendPayload(application.transportId, kConnectionNamespace,
                     F("{\"type\":\"CONNECT\",\"origin\":{}}"))) {
      break;
    }

    const uint32_t loadRequestId = nextRequestId();
    JsonDocument loadRequest;
    loadRequest["type"] = "LOAD";
    loadRequest["requestId"] = loadRequestId;
    if (!application.sessionId.isEmpty()) {
      loadRequest["sessionId"] = application.sessionId;
    }
    loadRequest["autoplay"] = true;
    loadRequest["repeatMode"] = "REPEAT_OFF";
    loadRequest["activeTrackIds"].to<JsonArray>();

    JsonObject media = loadRequest["media"].to<JsonObject>();
    media["contentId"] = url;
    media["contentType"] = contentType;
    media["streamType"] = "LIVE";
    JsonObject metadata = media["metadata"].to<JsonObject>();
    metadata["metadataType"] = 0;
    metadata["title"] = title;

    payload = "";
    if (loadRequest.overflowed() ||
        serializeJson(loadRequest, payload) == 0) {
      setError("Not enough memory for media request");
      break;
    }
    report("Sending OE3 stream");
    if (!sendPayload(application.transportId, kMediaNamespace, payload)) {
      break;
    }

    if (!waitForLoadResult(loadRequestId, url, 25000)) {
      break;
    }

    report("OE3 is playing");
    succeeded = true;
  } while (false);

  close();
  return succeeded;
}

bool CastClient::stop(const IPAddress& address, uint16_t port) {
  lastError_ = "";
  ReceiverApplication application;
  bool succeeded = false;

  report("Opening secure Cast connection");
  if (!open(address, port)) {
    return false;
  }

  do {
    if (!sendPayload(kPlatformDestinationId, kConnectionNamespace,
                     F("{\"type\":\"CONNECT\",\"origin\":{}}"))) {
      break;
    }

    const uint32_t statusRequestId = nextRequestId();
    JsonDocument statusRequest;
    statusRequest["type"] = "GET_STATUS";
    statusRequest["requestId"] = statusRequestId;
    String payload;
    if (statusRequest.overflowed() ||
        serializeJson(statusRequest, payload) == 0) {
      setError("Not enough memory for status request");
      break;
    }
    if (!sendPayload(kPlatformDestinationId, kReceiverNamespace, payload)) {
      break;
    }

    report("Checking media receiver");
    const AppWaitResult statusResult =
        waitForReceiverApplication(statusRequestId, 4000, true, application);
    if (statusResult == AppWaitResult::kError) {
      break;
    }
    if (statusResult == AppWaitResult::kTimeout) {
      setError("Receiver did not answer status request");
      break;
    }
    if (statusResult == AppWaitResult::kNotFound) {
      report("Stream is already stopped");
      succeeded = true;
      break;
    }
    if (application.sessionId.isEmpty()) {
      setError("Receiver supplied no session ID");
      break;
    }

    const uint32_t stopRequestId = nextRequestId();
    JsonDocument stopRequest;
    stopRequest["type"] = "STOP";
    stopRequest["requestId"] = stopRequestId;
    stopRequest["sessionId"] = application.sessionId;
    payload = "";
    if (stopRequest.overflowed() ||
        serializeJson(stopRequest, payload) == 0) {
      setError("Not enough memory for stop request");
      break;
    }

    report("Stopping stream");
    if (!sendPayload(kPlatformDestinationId, kReceiverNamespace, payload)) {
      break;
    }

    const AppWaitResult stopResult =
        waitForReceiverApplication(stopRequestId, 10000, true, application);
    if (stopResult == AppWaitResult::kNotFound) {
      report("Stream is stopped");
      succeeded = true;
    } else if (stopResult == AppWaitResult::kFound) {
      setError("Receiver kept the media session open");
    } else if (stopResult == AppWaitResult::kTimeout) {
      setError("Receiver did not confirm stop");
    }
  } while (false);

  close();
  return succeeded;
}

CastClient::ToggleResult CastClient::toggle(const IPAddress& address,
                                            uint16_t port,
                                            const char* url,
                                            const char* contentType,
                                            const char* title) {
  const PlaybackState state = queryPlaybackState(address, port);
  if (state == PlaybackState::kError) {
    return ToggleResult::kError;
  }

  if (state == PlaybackState::kActive) {
    return stop(address, port) ? ToggleResult::kStopped
                               : ToggleResult::kError;
  }

  return play(address, port, url, contentType, title)
             ? ToggleResult::kStarted
             : ToggleResult::kError;
}

const String& CastClient::lastError() const {
  return lastError_;
}

bool CastClient::open(const IPAddress& address, uint16_t port) {
  client_.stop();
  client_.setInsecure();
  client_.setTimeout(5);
  client_.setHandshakeTimeout(12);

  if (!client_.connect(address, port)) {
    setError("TLS connection failed");
    return false;
  }

  lastHeartbeatMs_ = millis();
  return true;
}

void CastClient::close() {
  client_.stop();
}

bool CastClient::sendPayload(const String& destinationId,
                             const char* nameSpace,
                             const String& payload) {
  std::vector<uint8_t> encoded;
  if (!cast_protocol::encodeStringMessage(
          kSourceId, destinationId.c_str(), nameSpace,
          std::string(payload.c_str(), payload.length()), encoded)) {
    setError("Cast message is too large");
    return false;
  }

  const uint32_t length = static_cast<uint32_t>(encoded.size());
  const uint8_t header[] = {
      static_cast<uint8_t>((length >> 24) & 0xff),
      static_cast<uint8_t>((length >> 16) & 0xff),
      static_cast<uint8_t>((length >> 8) & 0xff),
      static_cast<uint8_t>(length & 0xff),
  };

  if (!writeAll(header, sizeof(header)) ||
      !writeAll(encoded.data(), encoded.size())) {
    if (lastError_.isEmpty()) {
      setError("Cast connection write failed");
    }
    return false;
  }
  return true;
}

bool CastClient::writeAll(const uint8_t* data, size_t length) {
  size_t written = 0;
  const uint32_t deadline = millis() + kFrameCompletionTimeoutMs;

  while (written < length) {
    if (!client_.connected()) {
      return false;
    }

    const size_t count = client_.write(data + written, length - written);
    if (count > 0) {
      written += count;
      continue;
    }

    if (deadlineReached(deadline)) {
      return false;
    }
    delay(1);
  }

  return true;
}

CastClient::ReceiveResult CastClient::receiveMessage(
    cast_protocol::Message& message,
    uint32_t waitMs) {
  uint8_t header[4];
  size_t headerBytes = 0;
  uint32_t deadline = millis() + waitMs;
  bool frameStarted = false;

  while (headerBytes < sizeof(header)) {
    const int available = client_.available();
    if (available > 0) {
      size_t wanted = sizeof(header) - headerBytes;
      if (wanted > static_cast<size_t>(available)) {
        wanted = static_cast<size_t>(available);
      }
      const int count = client_.read(header + headerBytes, wanted);
      if (count > 0) {
        headerBytes += static_cast<size_t>(count);
        if (!frameStarted) {
          frameStarted = true;
          deadline = millis() + kFrameCompletionTimeoutMs;
        }
        continue;
      }
    }

    if (!client_.connected()) {
      setError("Cast receiver closed the connection");
      return ReceiveResult::kError;
    }
    if (deadlineReached(deadline)) {
      if (headerBytes == 0) {
        return ReceiveResult::kNoData;
      }
      setError("Incomplete Cast frame header");
      return ReceiveResult::kError;
    }
    delay(1);
  }

  const uint32_t frameLength =
      (static_cast<uint32_t>(header[0]) << 24) |
      (static_cast<uint32_t>(header[1]) << 16) |
      (static_cast<uint32_t>(header[2]) << 8) |
      static_cast<uint32_t>(header[3]);
  if (frameLength == 0 ||
      frameLength > cast_protocol::kMaximumMessageSize) {
    setError("Invalid Cast frame size");
    return ReceiveResult::kError;
  }

  std::unique_ptr<uint8_t[]> frame(
      new (std::nothrow) uint8_t[frameLength]);
  if (!frame) {
    setError("Not enough memory for Cast message");
    return ReceiveResult::kError;
  }

  size_t frameBytes = 0;
  while (frameBytes < frameLength) {
    const int available = client_.available();
    if (available > 0) {
      size_t wanted = frameLength - frameBytes;
      if (wanted > static_cast<size_t>(available)) {
        wanted = static_cast<size_t>(available);
      }
      const int count = client_.read(frame.get() + frameBytes, wanted);
      if (count > 0) {
        frameBytes += static_cast<size_t>(count);
        continue;
      }
    }

    if (!client_.connected()) {
      setError("Cast receiver closed mid-message");
      return ReceiveResult::kError;
    }
    if (deadlineReached(deadline)) {
      setError("Timed out reading Cast message");
      return ReceiveResult::kError;
    }
    delay(1);
  }

  if (!cast_protocol::decodeMessage(frame.get(), frameLength, message) ||
      message.protocolVersion != 0) {
    setError("Invalid Cast protocol message");
    return ReceiveResult::kError;
  }
  return ReceiveResult::kMessage;
}

CastClient::AppWaitResult CastClient::waitForReceiverApplication(
    uint32_t expectedRequestId,
    uint32_t timeoutMs,
    bool finishOnMissingStatus,
    ReceiverApplication& application) {
  const uint32_t deadline = millis() + timeoutMs;

  while (!deadlineReached(deadline)) {
    if (!sendHeartbeatIfDue()) {
      return AppWaitResult::kError;
    }

    cast_protocol::Message message;
    const ReceiveResult receiveResult = receiveMessage(message, 250);
    if (receiveResult == ReceiveResult::kError) {
      return AppWaitResult::kError;
    }
    if (receiveResult == ReceiveResult::kNoData) {
      continue;
    }

    if (message.nameSpace == kHeartbeatNamespace) {
      if (!handleHeartbeat(message)) {
        return AppWaitResult::kError;
      }
      continue;
    }
    if (message.nameSpace == kConnectionNamespace) {
      JsonDocument connectionMessage;
      if (deserializeJson(connectionMessage, message.payloadUtf8) ==
              DeserializationError::Ok &&
          strcmp(connectionMessage["type"] | "", "CLOSE") == 0) {
        setError("Cast receiver closed the channel");
        return AppWaitResult::kError;
      }
      continue;
    }
    if (message.nameSpace != kReceiverNamespace ||
        message.payloadType != 0) {
      continue;
    }

    JsonDocument document;
    const DeserializationError jsonError =
        deserializeJson(document, message.payloadUtf8);
    if (jsonError) {
      setError("Invalid receiver status JSON");
      return AppWaitResult::kError;
    }

    const char* type = document["type"] | "";
    const uint32_t responseRequestId = document["requestId"] | 0U;
    if (responseRequestId != expectedRequestId) {
      continue;
    }

    if (strcmp(type, "RECEIVER_STATUS") == 0) {
      JsonArrayConst applications =
          document["status"]["applications"].as<JsonArrayConst>();
      for (JsonObjectConst item : applications) {
        if (strcmp(item["appId"] | "", kDefaultMediaReceiverAppId) != 0) {
          continue;
        }

        application.transportId = item["transportId"] | "";
        application.sessionId = item["sessionId"] | "";
        if (application.transportId.isEmpty()) {
          setError("Receiver supplied no transport ID");
          return AppWaitResult::kError;
        }
        return AppWaitResult::kFound;
      }

      if (finishOnMissingStatus) {
        return AppWaitResult::kNotFound;
      }
    } else if (strcmp(type, "LAUNCH_ERROR") == 0 ||
               strcmp(type, "INVALID_REQUEST") == 0) {
      setError(String("Receiver request failed: ") + jsonReason(document));
      return AppWaitResult::kError;
    }
  }

  return AppWaitResult::kTimeout;
}

bool CastClient::waitForLoadResult(uint32_t requestId,
                                   const char* url,
                                   uint32_t timeoutMs) {
  const uint32_t deadline = millis() + timeoutMs;
  bool loadAcknowledged = false;
  int32_t mediaSessionId = -1;

  while (!deadlineReached(deadline)) {
    if (!sendHeartbeatIfDue()) {
      return false;
    }

    cast_protocol::Message message;
    const ReceiveResult receiveResult = receiveMessage(message, 250);
    if (receiveResult == ReceiveResult::kError) {
      return false;
    }
    if (receiveResult == ReceiveResult::kNoData) {
      continue;
    }

    if (message.nameSpace == kHeartbeatNamespace) {
      if (!handleHeartbeat(message)) {
        return false;
      }
      continue;
    }
    if (message.nameSpace == kConnectionNamespace) {
      JsonDocument connectionMessage;
      if (deserializeJson(connectionMessage, message.payloadUtf8) ==
              DeserializationError::Ok &&
          strcmp(connectionMessage["type"] | "", "CLOSE") == 0) {
        setError("Media receiver closed the channel");
        return false;
      }
      continue;
    }
    if (message.nameSpace != kMediaNamespace || message.payloadType != 0) {
      continue;
    }

    JsonDocument document;
    if (deserializeJson(document, message.payloadUtf8)) {
      setError("Invalid media status JSON");
      return false;
    }

    const char* type = document["type"] | "";
    const uint32_t responseRequestId = document["requestId"] | 0U;
    const bool matchesRequest = responseRequestId == requestId;

    if (strcmp(type, "MEDIA_STATUS") == 0) {
      JsonArrayConst statuses = document["status"].as<JsonArrayConst>();
      if (matchesRequest && statuses.isNull()) {
        setError("Receiver returned an empty media status");
        return false;
      }

      for (JsonObjectConst status : statuses) {
        const int32_t candidateSessionId = status["mediaSessionId"] | -1;
        const char* responseUrl = status["media"]["contentId"] | "";
        const bool sameSession =
            loadAcknowledged && mediaSessionId >= 0 &&
            candidateSessionId == mediaSessionId;
        const bool sameUrl = loadAcknowledged && responseUrl[0] != '\0' &&
                             strcmp(responseUrl, url) == 0;
        if (!matchesRequest && !sameSession && !sameUrl) {
          continue;
        }

        if (matchesRequest) {
          loadAcknowledged = true;
          if (candidateSessionId >= 0) {
            mediaSessionId = candidateSessionId;
          }
        }

        const char* state = status["playerState"] | "";
        const char* extendedState =
            status["extendedStatus"]["playerState"] | "";
        const char* idleReason = status["idleReason"] | "";
        if (strcmp(state, "PLAYING") == 0) {
          return true;
        }
        if (strcmp(state, "BUFFERING") == 0 ||
            (strcmp(state, "IDLE") == 0 &&
             strcmp(extendedState, "LOADING") == 0)) {
          report("Receiver is buffering OE3");
          continue;
        }
        if (strcmp(state, "IDLE") == 0) {
          const String reason =
              idleReason[0] == '\0' ? String("IDLE") : String(idleReason);
          setError(String("Receiver could not play stream: ") + reason);
          return false;
        }
      }

      if (matchesRequest && statuses.size() == 0) {
        setError("Receiver returned an empty media status");
        return false;
      }
    }

    if (matchesRequest &&
        (strcmp(type, "LOAD_FAILED") == 0 ||
         strcmp(type, "LOAD_CANCELLED") == 0 ||
         strcmp(type, "INVALID_REQUEST") == 0 ||
         strcmp(type, "INVALID_PLAYER_STATE") == 0 ||
         strcmp(type, "ERROR") == 0)) {
      setError(String("Stream load failed: ") + jsonReason(document));
      return false;
    }
  }

  setError(loadAcknowledged ? "Receiver did not start playing"
                            : "Receiver did not accept the stream");
  return false;
}

CastClient::PlaybackState CastClient::queryPlaybackState(
    const IPAddress& address,
    uint16_t port) {
  lastError_ = "";
  ReceiverApplication application;
  PlaybackState playbackState = PlaybackState::kError;

  report("Checking receiver playback");
  if (!open(address, port)) {
    return PlaybackState::kError;
  }

  do {
    if (!sendPayload(kPlatformDestinationId, kConnectionNamespace,
                     F("{\"type\":\"CONNECT\",\"origin\":{}}"))) {
      break;
    }

    const uint32_t receiverRequestId = nextRequestId();
    JsonDocument receiverRequest;
    receiverRequest["type"] = "GET_STATUS";
    receiverRequest["requestId"] = receiverRequestId;
    String payload;
    if (receiverRequest.overflowed() ||
        serializeJson(receiverRequest, payload) == 0) {
      setError("Not enough memory for status request");
      break;
    }
    if (!sendPayload(kPlatformDestinationId, kReceiverNamespace, payload)) {
      break;
    }

    const AppWaitResult appResult =
        waitForReceiverApplication(receiverRequestId, 4000, true, application);
    if (appResult == AppWaitResult::kNotFound) {
      playbackState = PlaybackState::kInactive;
      break;
    }
    if (appResult == AppWaitResult::kTimeout) {
      setError("Receiver did not answer status request");
      break;
    }
    if (appResult != AppWaitResult::kFound) {
      break;
    }

    if (!sendPayload(application.transportId, kConnectionNamespace,
                     F("{\"type\":\"CONNECT\",\"origin\":{}}"))) {
      break;
    }

    const uint32_t mediaRequestId = nextRequestId();
    JsonDocument mediaRequest;
    mediaRequest["type"] = "GET_STATUS";
    mediaRequest["requestId"] = mediaRequestId;
    if (!application.sessionId.isEmpty()) {
      mediaRequest["sessionId"] = application.sessionId;
    }
    payload = "";
    if (mediaRequest.overflowed() ||
        serializeJson(mediaRequest, payload) == 0) {
      setError("Not enough memory for media status request");
      break;
    }
    if (!sendPayload(application.transportId, kMediaNamespace, payload)) {
      break;
    }

    bool active = false;
    if (!waitForMediaActivity(mediaRequestId, 5000, active)) {
      break;
    }
    playbackState =
        active ? PlaybackState::kActive : PlaybackState::kInactive;
  } while (false);

  close();
  return playbackState;
}

bool CastClient::waitForMediaActivity(uint32_t expectedRequestId,
                                      uint32_t timeoutMs,
                                      bool& active) {
  const uint32_t deadline = millis() + timeoutMs;
  active = false;

  while (!deadlineReached(deadline)) {
    if (!sendHeartbeatIfDue()) {
      return false;
    }

    cast_protocol::Message message;
    const ReceiveResult receiveResult = receiveMessage(message, 250);
    if (receiveResult == ReceiveResult::kError) {
      return false;
    }
    if (receiveResult == ReceiveResult::kNoData) {
      continue;
    }

    if (message.nameSpace == kHeartbeatNamespace) {
      if (!handleHeartbeat(message)) {
        return false;
      }
      continue;
    }
    if (message.nameSpace == kConnectionNamespace) {
      JsonDocument connectionMessage;
      if (deserializeJson(connectionMessage, message.payloadUtf8) ==
              DeserializationError::Ok &&
          strcmp(connectionMessage["type"] | "", "CLOSE") == 0) {
        setError("Media receiver closed the channel");
        return false;
      }
      continue;
    }
    if (message.nameSpace != kMediaNamespace || message.payloadType != 0) {
      continue;
    }

    JsonDocument document;
    if (deserializeJson(document, message.payloadUtf8)) {
      setError("Invalid media status JSON");
      return false;
    }

    const uint32_t responseRequestId = document["requestId"] | 0U;
    if (responseRequestId != expectedRequestId) {
      continue;
    }

    const char* type = document["type"] | "";
    if (strcmp(type, "MEDIA_STATUS") == 0) {
      if (!document["status"].is<JsonArrayConst>()) {
        setError("Receiver returned malformed media status");
        return false;
      }
      JsonArrayConst statuses = document["status"].as<JsonArrayConst>();
      for (JsonObjectConst status : statuses) {
        const char* state = status["playerState"] | "";
        const char* extendedState =
            status["extendedStatus"]["playerState"] | "";
        if (strcmp(state, "PLAYING") == 0 ||
            strcmp(state, "BUFFERING") == 0 ||
            strcmp(state, "PAUSED") == 0 ||
            (strcmp(state, "IDLE") == 0 &&
             strcmp(extendedState, "LOADING") == 0)) {
          active = true;
          break;
        }
      }
      return true;
    }

    if (strcmp(type, "INVALID_REQUEST") == 0 ||
        strcmp(type, "INVALID_PLAYER_STATE") == 0 ||
        strcmp(type, "ERROR") == 0) {
      setError(String("Media status failed: ") + jsonReason(document));
      return false;
    }
  }

  setError("Receiver did not report media status");
  return false;
}

bool CastClient::handleHeartbeat(const cast_protocol::Message& message) {
  if (message.payloadType != 0) {
    return true;
  }

  JsonDocument document;
  if (deserializeJson(document, message.payloadUtf8)) {
    setError("Invalid Cast heartbeat");
    return false;
  }

  const char* type = document["type"] | "";
  if (strcmp(type, "PING") == 0) {
    const String destination =
        message.sourceId.empty() ? String(kPlatformDestinationId)
                                 : String(message.sourceId.c_str());
    if (!sendPayload(destination, kHeartbeatNamespace,
                     F("{\"type\":\"PONG\"}"))) {
      return false;
    }
    lastHeartbeatMs_ = millis();
  }
  return true;
}

bool CastClient::sendHeartbeatIfDue() {
  if (millis() - lastHeartbeatMs_ < kHeartbeatIntervalMs) {
    return true;
  }

  if (!sendPayload(kPlatformDestinationId, kHeartbeatNamespace,
                   F("{\"type\":\"PING\"}"))) {
    return false;
  }
  lastHeartbeatMs_ = millis();
  return true;
}

uint32_t CastClient::nextRequestId() {
  requestId_ = (requestId_ + 1) & 0x7fffffffU;
  if (requestId_ == 0) {
    requestId_ = 1;
  }
  return requestId_;
}

void CastClient::setError(const String& message) {
  lastError_ = message;
  report(String("Error: ") + message);
}

void CastClient::report(const String& status) const {
  Serial.println(status);
  if (statusCallback_ != nullptr) {
    statusCallback_(status);
  }
}
