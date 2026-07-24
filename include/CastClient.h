#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>

#include "CastProtocol.h"

class CastClient {
 public:
  using StatusCallback = void (*)(const String& status);

  enum class ToggleResult { kStarted, kStopped, kError };

  explicit CastClient(StatusCallback statusCallback = nullptr);

  // Launches (or joins) the Default Media Receiver and asks it to load the
  // supplied URL. Returns after the receiver accepts or rejects the LOAD.
  bool play(const IPAddress& address,
            uint16_t port,
            const char* url,
            const char* contentType,
            const char* title);

  // Stops the Default Media Receiver on the target. This is idempotent: it
  // succeeds when no Default Media Receiver session is currently running.
  bool stop(const IPAddress& address, uint16_t port);

  // Queries the receiver's real media state, then stops active media or starts
  // the supplied stream when the receiver is idle.
  ToggleResult toggle(const IPAddress& address,
                      uint16_t port,
                      const char* deviceId,
                      const char* url,
                      const char* contentType,
                      const char* title);

  // Services heartbeats and health checks for a stream started by toggle().
  // Call frequently from the Arduino loop.
  void service();
  bool isMaintainingPlayback() const;
  bool cancelMaintenance();
  void updateDiscoveredEndpoint(const char* deviceId,
                                const IPAddress& address,
                                uint16_t port);

  const String& lastError() const;

 private:
  struct ReceiverApplication {
    String transportId;
    String sessionId;
  };

  enum class ReceiveResult { kMessage, kNoData, kError };
  enum class AppWaitResult { kFound, kNotFound, kTimeout, kError };

  bool open(const IPAddress& address, uint16_t port);
  void close();
  bool sendPayload(const String& destinationId,
                   const char* nameSpace,
                   const String& payload);
  bool writeAll(const uint8_t* data, size_t length);
  ReceiveResult receiveMessage(cast_protocol::Message& message,
                               uint32_t waitMs);
  ReceiveResult receiveMessageNonBlocking(cast_protocol::Message& message);
  void resetServiceReceiver();
  AppWaitResult waitForReceiverApplication(uint32_t expectedRequestId,
                                           uint32_t timeoutMs,
                                           bool finishOnMissingStatus,
                                           ReceiverApplication& application);
  bool waitForLoadResult(uint32_t requestId,
                         const char* url,
                         uint32_t timeoutMs);
  bool waitForMediaActivity(uint32_t expectedRequestId,
                            uint32_t timeoutMs,
                            bool& active);
  bool handleHeartbeat(const cast_protocol::Message& message);
  bool sendHeartbeatIfDue();
  bool maintainHeartbeat();
  ToggleResult stopMaintainedPlayback();
  void rememberPlayback(const IPAddress& address,
                        uint16_t port,
                        const char* deviceId,
                        const char* url,
                        const char* contentType,
                        const char* title,
                        const ReceiverApplication& application);
  void clearMaintainedPlayback();
  void scheduleRecovery(const String& reason);
  bool loadMaintainedStream();
  bool recoverMaintainedPlayback();
  uint32_t nextRequestId();
  void setError(const String& message);
  void addErrorContext(const char* operation);
  void report(const String& status) const;

  WiFiClientSecure client_;
  StatusCallback statusCallback_;
  String lastError_;
  uint32_t requestId_;
  uint32_t lastPingMs_;

  bool maintainPlayback_;
  IPAddress maintainedAddress_;
  uint16_t maintainedPort_;
  String maintainedDeviceId_;
  String maintainedUrl_;
  String maintainedContentType_;
  String maintainedTitle_;
  ReceiverApplication maintainedApplication_;
  uint32_t lastMediaCheckMs_;
  bool mediaCheckPending_;
  uint32_t mediaCheckRequestId_;
  uint32_t mediaCheckSentMs_;
  uint32_t bufferingSinceMs_;
  bool recoveryScheduled_;
  uint32_t recoveryScheduledAtMs_;
  uint32_t recoveryDelayMs_;
  uint8_t recoveryFailures_;

  uint8_t serviceHeader_[4];
  size_t serviceHeaderBytes_;
  uint32_t serviceFrameLength_;
  size_t serviceFrameBytes_;
  uint32_t serviceFrameStartedMs_;
  uint8_t serviceFrame_[cast_protocol::kMaximumMessageSize];
};
