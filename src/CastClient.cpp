#include "CastClient.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <errno.h>
#include <esp_system.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <new>
#include <vector>

#include "Diagnostics.h"

namespace {

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
constexpr uint32_t kMediaCheckIntervalMs = 15000;
constexpr uint32_t kBufferingMaximumMs = 30000;
constexpr uint32_t kRecoveryBaseDelayMs = 2000;
constexpr uint32_t kRecoveryMaximumDelayMs = 30000;

bool deadlineReached(uint32_t deadline) {
  return static_cast<int32_t>(millis() - deadline) >= 0;
}

String jsonReason(const JsonDocument& document) {
  const char* reason = document["reason"] | "";
  return reason[0] == '\0' ? String("unknown reason") : String(reason);
}

void logCastMessage(const char* direction,
                    const char* source,
                    const char* destination,
                    const char* nameSpace,
                    const char* payload,
                    size_t payloadLength) {
  if (strcmp(nameSpace, kHeartbeatNamespace) == 0) {
    return;
  }

  char type[32] = "unknown";
  const char* typeStart = strstr(payload, "\"type\"");
  if (typeStart != nullptr) {
    typeStart = strchr(typeStart + 6, ':');
    if (typeStart != nullptr) {
      do {
        ++typeStart;
      } while (*typeStart == ' ' || *typeStart == '\t');
      if (*typeStart == '"') {
        ++typeStart;
        const char* typeEnd = strchr(typeStart, '"');
        if (typeEnd != nullptr) {
          size_t typeLength = static_cast<size_t>(typeEnd - typeStart);
          if (typeLength >= sizeof(type)) {
            typeLength = sizeof(type) - 1;
          }
          memcpy(type, typeStart, typeLength);
          type[typeLength] = '\0';
        }
      }
    }
  }

  long requestId = -1;
  const char* requestStart = strstr(payload, "\"requestId\"");
  if (requestStart != nullptr) {
    requestStart = strchr(requestStart + 11, ':');
    if (requestStart != nullptr) {
      requestId = strtol(requestStart + 1, nullptr, 10);
    }
  }
  diagnostics::logf(
      "CAST %s type=%s req=%ld src=%s dst=%s ns=%s bytes=%u",
      direction, type, requestId, source, destination, nameSpace,
      static_cast<unsigned int>(payloadLength));
}

}  // namespace

CastClient::CastClient(StatusCallback statusCallback)
    : statusCallback_(statusCallback),
      sourceId_("sender-0"),
      requestId_((esp_random() & 0x3fffffffU) + 1),
      lastPingMs_(0),
      heartbeatSent_(0),
      heartbeatPongs_(0),
      heartbeatPeerPings_(0),
      maintainPlayback_(false),
      maintainedPort_(0),
      maintainedMediaSessionId_(-1),
      lastMediaCheckMs_(0),
      mediaCheckPending_(false),
      mediaCheckRequestId_(0),
      mediaCheckSentMs_(0),
      resumeAttempts_(0),
      bufferingSinceMs_(0),
      recoveryScheduled_(false),
      recoveryScheduledAtMs_(0),
      recoveryDelayMs_(0),
      recoveryFailures_(0),
      serviceHeaderBytes_(0),
      serviceFrameLength_(0),
      serviceFrameBytes_(0),
      serviceFrameStartedMs_(0) {}

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
      addErrorContext("Platform connect");
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
      addErrorContext("Receiver status request");
      break;
    }

    report("Checking receiver");
    AppWaitResult appResult =
        waitForReceiverApplication(statusRequestId, 4000, true, application);
    if (appResult == AppWaitResult::kError) {
      addErrorContext("Receiver status");
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

  if (succeeded && maintainPlayback_) {
    maintainedApplication_ = application;
    recoveryFailures_ = 0;
    lastMediaCheckMs_ = millis();
    recoveryScheduled_ = false;
    recoveryScheduledAtMs_ = 0;
    recoveryDelayMs_ = 0;
    mediaCheckPending_ = false;
    mediaCheckRequestId_ = 0;
    mediaCheckSentMs_ = 0;
    resumeAttempts_ = 0;
    bufferingSinceMs_ = 0;
  } else {
    close();
  }
  return succeeded;
}

bool CastClient::stop(const IPAddress& address, uint16_t port) {
  maintainPlayback_ = false;
  lastError_ = "";
  ReceiverApplication application;
  bool succeeded = false;

  report("Opening secure Cast connection");
  if (!open(address, port)) {
    clearMaintainedPlayback();
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
  clearMaintainedPlayback();
  return succeeded;
}

CastClient::ToggleResult CastClient::toggle(const IPAddress& address,
                                            uint16_t port,
                                            const char* deviceId,
                                            const char* url,
                                            const char* contentType,
                                            const char* title) {
  if (maintainPlayback_) {
    const bool sameTarget =
        address == maintainedAddress_ && port == maintainedPort_;
    const ToggleResult stopResult = stopMaintainedPlayback();
    if (sameTarget) {
      return stopResult;
    }
    if (stopResult == ToggleResult::kError) {
      return stopResult;
    }
  }

  lastError_ = "";
  ReceiverApplication application;
  ToggleResult result = ToggleResult::kError;
  bool applicationChannelConnected = false;

  report("Opening secure Cast connection");
  if (!open(address, port)) {
    return ToggleResult::kError;
  }

  do {
    if (!sendPayload(kPlatformDestinationId, kConnectionNamespace,
                     F("{\"type\":\"CONNECT\",\"origin\":{}}"))) {
      addErrorContext("Platform connect");
      break;
    }

    report("Checking receiver playback");
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
      addErrorContext("Receiver status request");
      break;
    }

    AppWaitResult appResult =
        waitForReceiverApplication(receiverRequestId, 4000, true, application);
    if (appResult == AppWaitResult::kTimeout) {
      setError("Receiver did not answer status request");
      break;
    }
    if (appResult == AppWaitResult::kError) {
      addErrorContext("Receiver status");
      break;
    }

    bool active = false;
    if (appResult == AppWaitResult::kFound) {
      if (!sendPayload(application.transportId, kConnectionNamespace,
                       F("{\"type\":\"CONNECT\",\"origin\":{}}"))) {
        addErrorContext("Application connect");
        break;
      }
      applicationChannelConnected = true;

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
        addErrorContext("Media status request");
        break;
      }
      if (!waitForMediaActivity(mediaRequestId, 5000, active)) {
        addErrorContext("Media status");
        break;
      }
    }

    if (active) {
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
        addErrorContext("Stop request");
        break;
      }
      const AppWaitResult stopResult =
          waitForReceiverApplication(stopRequestId, 10000, true, application);
      if (stopResult == AppWaitResult::kNotFound) {
        report("Stream is stopped");
        result = ToggleResult::kStopped;
      } else if (stopResult == AppWaitResult::kFound) {
        setError("Receiver kept the media session open");
      } else if (stopResult == AppWaitResult::kTimeout) {
        setError("Receiver did not confirm stop");
      } else if (stopResult == AppWaitResult::kError) {
        addErrorContext("Stop request");
      }
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
        addErrorContext("Receiver launch request");
        break;
      }

      appResult = waitForReceiverApplication(launchRequestId, 20000, false,
                                             application);
      if (appResult != AppWaitResult::kFound) {
        if (appResult == AppWaitResult::kError) {
          addErrorContext("Receiver launch");
        } else {
          setError("Media receiver did not start");
        }
        break;
      }
    }

    if (!applicationChannelConnected) {
      report("Connecting to media receiver");
      if (!sendPayload(application.transportId, kConnectionNamespace,
                       F("{\"type\":\"CONNECT\",\"origin\":{}}"))) {
        addErrorContext("Application connect");
        break;
      }
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
      addErrorContext("Stream load request");
      break;
    }
    if (!waitForLoadResult(loadRequestId, url, 25000)) {
      addErrorContext("Stream load");
      break;
    }

    report("OE3 is playing");
    result = ToggleResult::kStarted;
  } while (false);

  if (result == ToggleResult::kStarted) {
    rememberPlayback(address, port, deviceId, url, contentType, title,
                     application);
  } else {
    close();
    if (result == ToggleResult::kStopped) {
      clearMaintainedPlayback();
    }
  }
  return result;
}

CastClient::ToggleResult CastClient::stopMaintainedPlayback() {
  const IPAddress address = maintainedAddress_;
  const uint16_t port = maintainedPort_;
  maintainPlayback_ = false;  // Disable recovery before any network operation.
  lastError_ = "";

  bool stopped = false;
  if (client_.connected() && serviceHeaderBytes_ == 0 &&
      serviceFrameLength_ == 0 &&
      !maintainedApplication_.sessionId.isEmpty()) {
    const uint32_t stopRequestId = nextRequestId();
    JsonDocument stopRequest;
    stopRequest["type"] = "STOP";
    stopRequest["requestId"] = stopRequestId;
    stopRequest["sessionId"] = maintainedApplication_.sessionId;
    String payload;
    if (!stopRequest.overflowed() &&
        serializeJson(stopRequest, payload) > 0) {
      report("Stopping stream");
      if (sendPayload(kPlatformDestinationId, kReceiverNamespace, payload)) {
        const AppWaitResult stopResult = waitForReceiverApplication(
            stopRequestId, 10000, true, maintainedApplication_);
        stopped = stopResult == AppWaitResult::kNotFound;
      }
    }
  }

  close();
  if (!stopped && address != IPAddress() && port != 0) {
    // The retained socket may have gone stale. Make one idempotent STOP
    // attempt over a fresh connection before giving up.
    stopped = stop(address, port);
  }

  clearMaintainedPlayback();
  if (stopped) {
    report("Stream is stopped");
    return ToggleResult::kStopped;
  }
  if (lastError_.isEmpty()) {
    setError("Receiver did not confirm stop");
  }
  return ToggleResult::kError;
}

void CastClient::rememberPlayback(
    const IPAddress& address,
    uint16_t port,
    const char* deviceId,
    const char* url,
    const char* contentType,
    const char* title,
    const ReceiverApplication& application) {
  maintainedAddress_ = address;
  maintainedPort_ = port;
  maintainedDeviceId_ = deviceId == nullptr ? "" : deviceId;
  maintainedUrl_ = url;
  maintainedContentType_ = contentType;
  maintainedTitle_ = title;
  maintainedApplication_ = application;
  maintainPlayback_ = true;
  recoveryFailures_ = 0;
  recoveryScheduled_ = false;
  recoveryScheduledAtMs_ = 0;
  recoveryDelayMs_ = 0;
  lastMediaCheckMs_ = millis();
  mediaCheckPending_ = false;
  mediaCheckRequestId_ = 0;
  mediaCheckSentMs_ = 0;
  resumeAttempts_ = 0;
  bufferingSinceMs_ = 0;
}

void CastClient::clearMaintainedPlayback() {
  maintainPlayback_ = false;
  maintainedAddress_ = IPAddress();
  maintainedPort_ = 0;
  maintainedDeviceId_ = "";
  maintainedUrl_ = "";
  maintainedContentType_ = "";
  maintainedTitle_ = "";
  maintainedApplication_ = ReceiverApplication();
  maintainedMediaSessionId_ = -1;
  lastMediaCheckMs_ = 0;
  mediaCheckPending_ = false;
  mediaCheckRequestId_ = 0;
  mediaCheckSentMs_ = 0;
  resumeAttempts_ = 0;
  bufferingSinceMs_ = 0;
  recoveryScheduled_ = false;
  recoveryScheduledAtMs_ = 0;
  recoveryDelayMs_ = 0;
  recoveryFailures_ = 0;
}

void CastClient::scheduleRecovery(const String& reason) {
  diagnostics::logf(
      "RECOVERY reason=\"%s\" sender=%s heartbeats_sent=%lu pongs=%lu "
      "peer_pings=%lu failures=%u",
      reason.c_str(), sourceId_.c_str(),
      static_cast<unsigned long>(heartbeatSent_),
      static_cast<unsigned long>(heartbeatPongs_),
      static_cast<unsigned long>(heartbeatPeerPings_),
      static_cast<unsigned int>(recoveryFailures_));
  close();
  maintainedApplication_ = ReceiverApplication();
  maintainedMediaSessionId_ = -1;
  mediaCheckPending_ = false;
  mediaCheckRequestId_ = 0;
  mediaCheckSentMs_ = 0;
  resumeAttempts_ = 0;
  bufferingSinceMs_ = 0;

  uint8_t shift = recoveryFailures_;
  if (shift > 4) {
    shift = 4;
  }
  uint32_t delayMs = kRecoveryBaseDelayMs << shift;
  if (delayMs > kRecoveryMaximumDelayMs) {
    delayMs = kRecoveryMaximumDelayMs;
  }
  if (recoveryFailures_ < 10) {
    ++recoveryFailures_;
  }
  recoveryScheduled_ = true;
  recoveryScheduledAtMs_ = millis();
  recoveryDelayMs_ = delayMs;
  report(reason + "; retrying in " + String(delayMs / 1000) + "s");
}

bool CastClient::loadMaintainedStream() {
  if (maintainedApplication_.transportId.isEmpty()) {
    return false;
  }

  const uint32_t loadRequestId = nextRequestId();
  JsonDocument loadRequest;
  loadRequest["type"] = "LOAD";
  loadRequest["requestId"] = loadRequestId;
  if (!maintainedApplication_.sessionId.isEmpty()) {
    loadRequest["sessionId"] = maintainedApplication_.sessionId;
  }
  loadRequest["autoplay"] = true;
  loadRequest["repeatMode"] = "REPEAT_OFF";
  loadRequest["activeTrackIds"].to<JsonArray>();
  JsonObject media = loadRequest["media"].to<JsonObject>();
  media["contentId"] = maintainedUrl_;
  media["contentType"] = maintainedContentType_;
  media["streamType"] = "LIVE";
  JsonObject metadata = media["metadata"].to<JsonObject>();
  metadata["metadataType"] = 0;
  metadata["title"] = maintainedTitle_;

  String payload;
  if (loadRequest.overflowed() ||
      serializeJson(loadRequest, payload) == 0) {
    setError("Not enough memory for recovery request");
    return false;
  }

  report("Restarting OE3 stream");
  maintainedMediaSessionId_ = -1;
  if (!sendPayload(maintainedApplication_.transportId, kMediaNamespace,
                   payload)) {
    return false;
  }

  report("OE3 restart requested");
  recoveryScheduled_ = false;
  recoveryScheduledAtMs_ = 0;
  recoveryDelayMs_ = 0;
  lastMediaCheckMs_ = millis();
  mediaCheckPending_ = false;
  mediaCheckRequestId_ = 0;
  mediaCheckSentMs_ = 0;
  resumeAttempts_ = 0;
  bufferingSinceMs_ = 0;
  return true;
}

bool CastClient::resumeMaintainedStream() {
  if (maintainedMediaSessionId_ < 0 ||
      maintainedApplication_.transportId.isEmpty()) {
    return loadMaintainedStream();
  }

  const uint32_t playRequestId = nextRequestId();
  JsonDocument playRequest;
  playRequest["type"] = "PLAY";
  playRequest["requestId"] = playRequestId;
  playRequest["mediaSessionId"] = maintainedMediaSessionId_;
  if (!maintainedApplication_.sessionId.isEmpty()) {
    playRequest["sessionId"] = maintainedApplication_.sessionId;
  }
  String payload;
  if (playRequest.overflowed() ||
      serializeJson(playRequest, payload) == 0) {
    setError("Not enough memory for resume request");
    return false;
  }

  report("Resuming OE3 stream");
  if (!sendPayload(maintainedApplication_.transportId, kMediaNamespace,
                   payload)) {
    return false;
  }
  if (resumeAttempts_ < 255) {
    ++resumeAttempts_;
  }
  recoveryScheduled_ = false;
  recoveryScheduledAtMs_ = 0;
  recoveryDelayMs_ = 0;
  lastMediaCheckMs_ = millis();
  mediaCheckPending_ = true;
  mediaCheckRequestId_ = playRequestId;
  mediaCheckSentMs_ = millis();
  return true;
}

bool CastClient::recoverMaintainedPlayback() {
  lastError_ = "";
  ReceiverApplication application;

  if (!open(maintainedAddress_, maintainedPort_)) {
    return false;
  }

  bool succeeded = false;
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
        serializeJson(receiverRequest, payload) == 0 ||
        !sendPayload(kPlatformDestinationId, kReceiverNamespace, payload)) {
      break;
    }

    AppWaitResult appResult =
        waitForReceiverApplication(receiverRequestId, 5000, true, application);
    if (appResult == AppWaitResult::kError ||
        appResult == AppWaitResult::kTimeout) {
      break;
    }

    bool launched = false;
    if (appResult == AppWaitResult::kNotFound) {
      const uint32_t launchRequestId = nextRequestId();
      JsonDocument launchRequest;
      launchRequest["type"] = "LAUNCH";
      launchRequest["appId"] = kDefaultMediaReceiverAppId;
      launchRequest["requestId"] = launchRequestId;
      payload = "";
      if (launchRequest.overflowed() ||
          serializeJson(launchRequest, payload) == 0 ||
          !sendPayload(kPlatformDestinationId, kReceiverNamespace, payload)) {
        break;
      }

      appResult = waitForReceiverApplication(launchRequestId, 20000, false,
                                             application);
      if (appResult != AppWaitResult::kFound) {
        break;
      }
      launched = true;
    }

    if (!sendPayload(application.transportId, kConnectionNamespace,
                     F("{\"type\":\"CONNECT\",\"origin\":{}}"))) {
      break;
    }
    maintainedApplication_ = application;

    if (!launched) {
      const uint32_t mediaRequestId = nextRequestId();
      JsonDocument mediaRequest;
      mediaRequest["type"] = "GET_STATUS";
      mediaRequest["requestId"] = mediaRequestId;
      if (!application.sessionId.isEmpty()) {
        mediaRequest["sessionId"] = application.sessionId;
      }
      payload = "";
      if (mediaRequest.overflowed() ||
          serializeJson(mediaRequest, payload) == 0 ||
          !sendPayload(application.transportId, kMediaNamespace, payload)) {
        break;
      }

      bool active = false;
      bool paused = false;
      bool configuredContent = false;
      if (!waitForMediaActivity(mediaRequestId, 5000, active, &paused,
                                &configuredContent)) {
        if (client_.connected()) {
          report("Media state unavailable; requesting OE3");
          succeeded = loadMaintainedStream();
        }
        break;
      }
      if (paused) {
        succeeded = configuredContent ? resumeMaintainedStream()
                                      : loadMaintainedStream();
        break;
      }
      if (active) {
        recoveryScheduled_ = false;
        recoveryScheduledAtMs_ = 0;
        recoveryDelayMs_ = 0;
        lastMediaCheckMs_ = millis();
        mediaCheckPending_ = false;
        mediaCheckRequestId_ = 0;
        mediaCheckSentMs_ = 0;
        report("Cast connection restored");
        succeeded = true;
        break;
      }
    }

    succeeded = loadMaintainedStream();
  } while (false);

  if (!succeeded) {
    close();
  }
  return succeeded;
}

bool CastClient::isMaintainingPlayback() const {
  return maintainPlayback_;
}

bool CastClient::cancelMaintenance() {
  if (!maintainPlayback_) {
    return false;
  }
  if (WiFi.status() == WL_CONNECTED) {
    stopMaintainedPlayback();
    return true;
  }
  maintainPlayback_ = false;
  close();
  clearMaintainedPlayback();
  report("Playback recovery cancelled");
  return true;
}

void CastClient::updateDiscoveredEndpoint(const char* deviceId,
                                          const IPAddress& address,
                                          uint16_t port) {
  if (!maintainPlayback_ || deviceId == nullptr || deviceId[0] == '\0' ||
      maintainedDeviceId_ != deviceId) {
    return;
  }
  maintainedAddress_ = address;
  maintainedPort_ = port;
}

void CastClient::service() {
  if (!maintainPlayback_ || WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (!client_.connected()) {
    if (!recoveryScheduled_) {
      diagnostics::logf(
          "TLS disconnected sender=%s heartbeats_sent=%lu pongs=%lu "
          "peer_pings=%lu",
          sourceId_.c_str(), static_cast<unsigned long>(heartbeatSent_),
          static_cast<unsigned long>(heartbeatPongs_),
          static_cast<unsigned long>(heartbeatPeerPings_));
      scheduleRecovery("Cast socket disconnected");
      return;
    }
    if (recoveryScheduled_ &&
        millis() - recoveryScheduledAtMs_ < recoveryDelayMs_) {
      return;
    }

    report("Reconnecting Cast stream");
    if (recoverMaintainedPlayback()) {
      return;
    }
    scheduleRecovery("Cast recovery failed");
    return;
  }

  if (!maintainHeartbeat()) {
    scheduleRecovery("Cast heartbeat send failed");
    return;
  }

  for (uint8_t count = 0; count < 6; ++count) {
    cast_protocol::Message message;
    const ReceiveResult receiveResult = receiveMessageNonBlocking(message);
    if (receiveResult == ReceiveResult::kNoData) {
      break;
    }
    if (receiveResult == ReceiveResult::kError) {
      scheduleRecovery("Cast connection lost");
      return;
    }

    if (message.nameSpace == kHeartbeatNamespace) {
      if (!handleHeartbeat(message)) {
        scheduleRecovery("Cast heartbeat failed");
        return;
      }
      continue;
    }

    if (message.nameSpace == kConnectionNamespace) {
      JsonDocument connectionMessage;
      if (deserializeJson(connectionMessage, message.payloadUtf8) ==
              DeserializationError::Ok &&
          strcmp(connectionMessage["type"] | "", "CLOSE") == 0) {
        scheduleRecovery("Cast channel closed");
        return;
      }
      continue;
    }

    if (message.nameSpace == kMediaNamespace && message.payloadType == 0 &&
        mediaCheckPending_) {
      JsonDocument mediaStatus;
      if (deserializeJson(mediaStatus, message.payloadUtf8)) {
        mediaCheckPending_ = false;
        mediaCheckRequestId_ = 0;
        mediaCheckSentMs_ = 0;
        lastMediaCheckMs_ = millis();
        report("Invalid media status; keeping Cast session");
        continue;
      }
      const uint32_t responseRequestId = mediaStatus["requestId"] | 0U;
      if (responseRequestId != 0 &&
          responseRequestId != mediaCheckRequestId_) {
        continue;
      }

      const char* type = mediaStatus["type"] | "";
      if (strcmp(type, "MEDIA_STATUS") != 0 ||
          !mediaStatus["status"].is<JsonArrayConst>()) {
        const bool resumeWasPending = resumeAttempts_ > 0;
        mediaCheckPending_ = false;
        mediaCheckRequestId_ = 0;
        mediaCheckSentMs_ = 0;
        lastMediaCheckMs_ = millis();
        if (resumeWasPending) {
          if (!loadMaintainedStream()) {
            scheduleRecovery("Resume rejected");
            return;
          }
          continue;
        }
        report("Media status rejected; keeping Cast session");
        continue;
      }

      bool active = false;
      bool paused = false;
      bool configuredContent = false;
      JsonArrayConst statuses = mediaStatus["status"].as<JsonArrayConst>();
      for (JsonObjectConst status : statuses) {
        const char* state = status["playerState"] | "";
        const char* extendedState =
            status["extendedStatus"]["playerState"] | "";
        const char* idleReason = status["idleReason"] | "";
        const char* contentId = status["media"]["contentId"] | "";
        configuredContent =
            contentId[0] != '\0' && maintainedUrl_ == contentId;
        const int32_t mediaSessionId = status["mediaSessionId"] | -1;
        if (mediaSessionId >= 0) {
          maintainedMediaSessionId_ = mediaSessionId;
        }
        diagnostics::logf(
            "MEDIA state=%s extended=%s idle=%s media_session=%ld "
            "content=\"%s\"",
            state, extendedState, idleReason,
            static_cast<long>(mediaSessionId), contentId);
        if (strcmp(state, "PLAYING") == 0) {
          bufferingSinceMs_ = 0;
          recoveryFailures_ = 0;
          resumeAttempts_ = 0;
          active = true;
          break;
        }
        if (strcmp(state, "PAUSED") == 0) {
          paused = true;
          break;
        }
        if (strcmp(state, "BUFFERING") == 0 ||
            (strcmp(state, "IDLE") == 0 &&
             strcmp(extendedState, "LOADING") == 0)) {
          if (bufferingSinceMs_ == 0) {
            bufferingSinceMs_ = millis() == 0 ? 1 : millis();
          }
          active =
              millis() - bufferingSinceMs_ < kBufferingMaximumMs;
          break;
        }
      }

      mediaCheckPending_ = false;
      mediaCheckRequestId_ = 0;
      mediaCheckSentMs_ = 0;
      lastMediaCheckMs_ = millis();
      if (paused) {
        const bool requested =
            configuredContent && resumeAttempts_ < 2
                ? resumeMaintainedStream()
                : loadMaintainedStream();
        if (!requested) {
          scheduleRecovery("Stream resume failed");
          return;
        }
      }
      if (!paused && !active && !loadMaintainedStream()) {
        scheduleRecovery("Stream restart failed");
        return;
      }
    }
  }

  if (mediaCheckPending_) {
    if (millis() - mediaCheckSentMs_ >= 5000) {
      const bool resumeWasPending = resumeAttempts_ > 0;
      mediaCheckPending_ = false;
      mediaCheckRequestId_ = 0;
      mediaCheckSentMs_ = 0;
      lastMediaCheckMs_ = millis();
      if (resumeWasPending) {
        if (!loadMaintainedStream()) {
          scheduleRecovery("Resume status timed out");
        }
      } else {
        report("Media status delayed; keeping Cast session");
      }
    }
    return;
  }

  if (millis() - lastMediaCheckMs_ < kMediaCheckIntervalMs) {
    return;
  }
  lastMediaCheckMs_ = millis();

  if (maintainedApplication_.transportId.isEmpty()) {
    scheduleRecovery("Media session disappeared");
    return;
  }

  const uint32_t mediaRequestId = nextRequestId();
  JsonDocument mediaRequest;
  mediaRequest["type"] = "GET_STATUS";
  mediaRequest["requestId"] = mediaRequestId;
  if (!maintainedApplication_.sessionId.isEmpty()) {
    mediaRequest["sessionId"] = maintainedApplication_.sessionId;
  }
  String payload;
  if (mediaRequest.overflowed() ||
      serializeJson(mediaRequest, payload) == 0) {
    report("Could not build media health check");
    return;
  }
  if (!sendPayload(maintainedApplication_.transportId, kMediaNamespace,
                   payload)) {
    scheduleRecovery("Media health check failed");
    return;
  }
  mediaCheckPending_ = true;
  mediaCheckRequestId_ = mediaRequestId;
  mediaCheckSentMs_ = millis();
}

const String& CastClient::lastError() const {
  return lastError_;
}

bool CastClient::open(const IPAddress& address, uint16_t port) {
  String failure = "TLS connection failed";
  resetServiceReceiver();
  constexpr uint32_t retryDelaysMs[] = {1000, 3000};
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    client_.stop();
    client_.setInsecure();
    client_.setHandshakeTimeout(15);

    diagnostics::logf(
        "TLS attempt=%u target=%s:%u wifi=%d rssi=%d heap=%u "
        "largest=%u min=%u psram=%u",
        static_cast<unsigned int>(attempt + 1),
        address.toString().c_str(), static_cast<unsigned int>(port),
        static_cast<int>(WiFi.status()),
        WiFi.RSSI(), ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
        ESP.getMinFreeHeap(), ESP.getFreePsram());
    const uint32_t startedAt = millis();
    errno = 0;
    if (client_.connect(address, port, 10000)) {
      client_.setTimeout(10);
      char senderId[24];
      snprintf(senderId, sizeof(senderId), "sender-%08X",
               static_cast<unsigned int>(esp_random()));
      sourceId_ = senderId;
      lastPingMs_ = millis();
      heartbeatSent_ = 0;
      heartbeatPongs_ = 0;
      heartbeatPeerPings_ = 0;
      diagnostics::logf(
          "TLS connected attempt=%u elapsed=%lums fd=%d sender=%s",
                        static_cast<unsigned int>(attempt + 1),
                        static_cast<unsigned long>(millis() - startedAt),
                        client_.fd(), sourceId_.c_str());
      return true;
    }

    const int socketErrno = errno;
    char errorText[96] = {};
    const int errorCode = client_.lastError(errorText, sizeof(errorText));
    if (errorCode != 0) {
      failure = String("TLS failed ") + errorCode;
      if (errorText[0] != '\0') {
        failure += ": ";
        failure += errorText;
      }
    }

    diagnostics::logf(
        "TLS failed attempt=%u elapsed=%lums mbed=%d text=\"%s\" "
        "errno_snapshot=%d \"%s\"",
        static_cast<unsigned int>(attempt + 1),
        static_cast<unsigned long>(millis() - startedAt), errorCode,
        errorText, socketErrno,
        socketErrno == 0 ? "" : strerror(socketErrno));
    if (attempt < 2) {
      report("Retrying secure Cast connection");
      delay(retryDelaysMs[attempt]);
    }
  }

  WiFiClient infoProbe;
  const uint32_t probeStartedAt = millis();
  const bool infoPortOpen = infoProbe.connect(address, 8008, 2000);
  diagnostics::logf("Probe target=%s:8008 result=%s elapsed=%lums",
                    address.toString().c_str(),
                    infoPortOpen ? "open" : "closed",
                    static_cast<unsigned long>(millis() - probeStartedAt));
  infoProbe.stop();
  setError(failure);
  return false;
}

void CastClient::close() {
  if (client_.fd() >= 0) {
    diagnostics::logf("TLS close fd=%d maintaining=%s", client_.fd(),
                      maintainPlayback_ ? "yes" : "no");
  }
  client_.stop();
  resetServiceReceiver();
}

bool CastClient::sendPayload(const String& destinationId,
                             const char* nameSpace,
                             const String& payload) {
  logCastMessage("TX", sourceId_.c_str(), destinationId.c_str(), nameSpace,
                 payload.c_str(), payload.length());
  std::vector<uint8_t> encoded;
  if (!cast_protocol::encodeStringMessage(
          sourceId_.c_str(), destinationId.c_str(), nameSpace,
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
  logCastMessage("RX", message.sourceId.c_str(),
                 message.destinationId.c_str(), message.nameSpace.c_str(),
                 message.payloadUtf8.c_str(), message.payloadUtf8.size());
  return ReceiveResult::kMessage;
}

void CastClient::resetServiceReceiver() {
  serviceHeaderBytes_ = 0;
  serviceFrameLength_ = 0;
  serviceFrameBytes_ = 0;
  serviceFrameStartedMs_ = 0;
}

CastClient::ReceiveResult CastClient::receiveMessageNonBlocking(
    cast_protocol::Message& message) {
  while (client_.available() > 0) {
    if (serviceHeaderBytes_ < sizeof(serviceHeader_)) {
      size_t wanted = sizeof(serviceHeader_) - serviceHeaderBytes_;
      const int available = client_.available();
      if (wanted > static_cast<size_t>(available)) {
        wanted = static_cast<size_t>(available);
      }
      const int count =
          client_.read(serviceHeader_ + serviceHeaderBytes_, wanted);
      if (count <= 0) {
        break;
      }
      if (serviceHeaderBytes_ == 0) {
        serviceFrameStartedMs_ = millis();
      }
      serviceHeaderBytes_ += static_cast<size_t>(count);
      if (serviceHeaderBytes_ < sizeof(serviceHeader_)) {
        continue;
      }

      serviceFrameLength_ =
          (static_cast<uint32_t>(serviceHeader_[0]) << 24) |
          (static_cast<uint32_t>(serviceHeader_[1]) << 16) |
          (static_cast<uint32_t>(serviceHeader_[2]) << 8) |
          static_cast<uint32_t>(serviceHeader_[3]);
      if (serviceFrameLength_ == 0 ||
          serviceFrameLength_ > cast_protocol::kMaximumMessageSize) {
        resetServiceReceiver();
        setError("Invalid Cast frame size");
        return ReceiveResult::kError;
      }
    }

    if (serviceFrameBytes_ < serviceFrameLength_) {
      size_t wanted = serviceFrameLength_ - serviceFrameBytes_;
      const int available = client_.available();
      if (available <= 0) {
        break;
      }
      if (wanted > static_cast<size_t>(available)) {
        wanted = static_cast<size_t>(available);
      }
      const int count =
          client_.read(serviceFrame_ + serviceFrameBytes_, wanted);
      if (count <= 0) {
        break;
      }
      serviceFrameBytes_ += static_cast<size_t>(count);
    }

    if (serviceFrameBytes_ == serviceFrameLength_) {
      const bool valid = cast_protocol::decodeMessage(
                             serviceFrame_, serviceFrameLength_, message) &&
                         message.protocolVersion == 0;
      resetServiceReceiver();
      if (!valid) {
        setError("Invalid Cast protocol message");
        return ReceiveResult::kError;
      }
      logCastMessage("RX", message.sourceId.c_str(),
                     message.destinationId.c_str(),
                     message.nameSpace.c_str(),
                     message.payloadUtf8.c_str(),
                     message.payloadUtf8.size());
      return ReceiveResult::kMessage;
    }
  }

  if ((serviceHeaderBytes_ != 0 || serviceFrameLength_ != 0) &&
      millis() - serviceFrameStartedMs_ >= kFrameCompletionTimeoutMs) {
    resetServiceReceiver();
    setError("Timed out reading Cast message");
    return ReceiveResult::kError;
  }
  if (!client_.connected()) {
    resetServiceReceiver();
    setError("Cast receiver closed the connection");
    return ReceiveResult::kError;
  }
  return ReceiveResult::kNoData;
}

CastClient::AppWaitResult CastClient::waitForReceiverApplication(
    uint32_t expectedRequestId,
    uint32_t timeoutMs,
    bool finishOnMissingStatus,
    ReceiverApplication& application) {
  const uint32_t deadline = millis() + timeoutMs;

  while (!deadlineReached(deadline)) {
    if (!maintainHeartbeat()) {
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
        if (!application.transportId.isEmpty() &&
            message.sourceId == application.transportId.c_str()) {
          // STOP closes the application channel before receiver-0 confirms
          // the updated application list. The platform TLS channel is alive.
          continue;
        }
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
    if (!maintainHeartbeat()) {
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
            maintainedMediaSessionId_ = candidateSessionId;
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

bool CastClient::waitForMediaActivity(uint32_t expectedRequestId,
                                      uint32_t timeoutMs,
                                      bool& active,
                                      bool* paused,
                                      bool* configuredContent) {
  const uint32_t deadline = millis() + timeoutMs;
  active = false;
  if (paused != nullptr) {
    *paused = false;
  }
  if (configuredContent != nullptr) {
    *configuredContent = false;
  }

  while (!deadlineReached(deadline)) {
    if (!maintainHeartbeat()) {
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
    if (responseRequestId != 0 &&
        responseRequestId != expectedRequestId) {
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
        const char* contentId = status["media"]["contentId"] | "";
        if (configuredContent != nullptr) {
          *configuredContent =
              contentId[0] != '\0' && maintainedUrl_ == contentId;
        }
        const int32_t mediaSessionId = status["mediaSessionId"] | -1;
        if (mediaSessionId >= 0) {
          maintainedMediaSessionId_ = mediaSessionId;
        }
        diagnostics::logf(
            "MEDIA query state=%s extended=%s media_session=%ld",
            state, extendedState, static_cast<long>(mediaSessionId));
        if (strcmp(state, "PLAYING") == 0) {
          bufferingSinceMs_ = 0;
          active = true;
          break;
        }
        if (strcmp(state, "PAUSED") == 0) {
          if (paused != nullptr) {
            *paused = true;
          }
          break;
        }
        if (strcmp(state, "BUFFERING") == 0 ||
            (strcmp(state, "IDLE") == 0 &&
             strcmp(extendedState, "LOADING") == 0)) {
          if (bufferingSinceMs_ == 0) {
            bufferingSinceMs_ = millis() == 0 ? 1 : millis();
          }
          active =
              millis() - bufferingSinceMs_ < kBufferingMaximumMs;
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
  if (strcmp(type, "PONG") == 0) {
    ++heartbeatPongs_;
  }
  if (strcmp(type, "PING") == 0) {
    ++heartbeatPeerPings_;
    const String destination =
        message.sourceId.empty() ? String(kPlatformDestinationId)
                                 : String(message.sourceId.c_str());
    if (!sendPayload(destination, kHeartbeatNamespace,
                     F("{\"type\":\"PONG\"}"))) {
      return false;
    }
  }
  return true;
}

bool CastClient::maintainHeartbeat() {
  return sendHeartbeatIfDue();
}

bool CastClient::sendHeartbeatIfDue() {
  if (millis() - lastPingMs_ < kHeartbeatIntervalMs) {
    return true;
  }

  if (!sendPayload(kPlatformDestinationId, kHeartbeatNamespace,
                   F("{\"type\":\"PING\"}"))) {
    return false;
  }
  lastPingMs_ = millis();
  ++heartbeatSent_;
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

void CastClient::addErrorContext(const char* operation) {
  const String previousError = lastError_;
  String contextualError = operation;
  if (!previousError.isEmpty()) {
    contextualError += ": ";
    contextualError += previousError;
  }
  setError(contextualError);
}

void CastClient::report(const String& status) const {
  diagnostics::logf("STATE %s", status.c_str());
  if (statusCallback_ != nullptr) {
    statusCallback_(status);
  }
}
