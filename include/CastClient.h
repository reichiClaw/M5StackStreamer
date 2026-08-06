#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>

#include "CastProtocol.h"

class CastClient {
 public:
  // Machine-readable classification of a status message. The UI must not
  // parse status strings; it derives icons and playback state from this kind.
  enum class StatusKind : uint8_t { kInfo, kBusy, kPlaying, kStopped, kError };

  using StatusCallback = void (*)(const String& status, StatusKind kind);

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

  // Adopts (or starts) OE3 playback on the target without toggling: an
  // already playing configured stream is joined, anything else is resumed or
  // loaded. Enables playback maintenance even when the first attempt fails,
  // so service() keeps retrying. Intended for auto-resume after a reboot.
  bool resumePlayback(const IPAddress& address,
                      uint16_t port,
                      const char* deviceId,
                      const char* url,
                      const char* contentType,
                      const char* title);

  // Services heartbeats and health checks for a stream started by toggle().
  // Call frequently from the Arduino loop.
  void service();
  bool isMaintainingPlayback() const;
  // True while a maintained session has an established, non-recovering TLS
  // connection. Used to skip disruptive periodic work (e.g. mDNS scans).
  bool hasHealthyConnection();
  bool cancelMaintenance();
  void updateDiscoveredEndpoint(const char* deviceId,
                                const IPAddress& address,
                                uint16_t port);

  const String& lastError() const;

  // Receiver volume as reported by the most recent RECEIVER_STATUS on the
  // current connection. Lets the UI distinguish "playing but muted" from a
  // receiver whose audio pipeline is broken.
  bool receiverVolumeKnown() const;
  bool receiverMuted() const;
  uint8_t receiverVolumePercent() const;

 private:
  struct ReceiverApplication {
    String transportId;
    String sessionId;
  };

  enum class ReceiveResult { kMessage, kNoData, kError };
  enum class AppWaitResult { kFound, kNotFound, kTimeout, kError };

  bool open(const IPAddress& address, uint16_t port, uint8_t attempts = 3);
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
                            bool& active,
                            bool* paused = nullptr,
                            bool* configuredContent = nullptr,
                            bool* playing = nullptr);
  bool handleHeartbeat(const cast_protocol::Message& message);
  bool sendHeartbeatIfDue();
  bool maintainHeartbeat();
  // Reacts to receiver-initiated MEDIA_STATUS broadcasts for the maintained
  // stream. Returns false when recovery was scheduled.
  bool handleBroadcastMediaStatus(const JsonDocument& mediaStatus);
  void updateReceiverVolume(const JsonDocument& receiverStatus);
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
  bool resumeMaintainedStream(bool trackResponse = true);
  bool recoverMaintainedPlayback();
  uint32_t nextRequestId();
  void setError(const String& message);
  void addErrorContext(const char* operation);
  void report(const String& status, StatusKind kind = StatusKind::kBusy) const;

  WiFiClientSecure client_;
  StatusCallback statusCallback_;
  String lastError_;
  String sourceId_;
  uint32_t requestId_;
  uint32_t lastPingMs_;
  uint32_t lastInboundMs_;
  uint32_t heartbeatSent_;
  uint32_t heartbeatPongs_;
  uint32_t heartbeatPeerPings_;

  bool maintainPlayback_;
  IPAddress maintainedAddress_;
  uint16_t maintainedPort_;
  String maintainedDeviceId_;
  String maintainedUrl_;
  String maintainedContentType_;
  String maintainedTitle_;
  ReceiverApplication maintainedApplication_;
  int32_t maintainedMediaSessionId_;
  String maintainedMediaApplicationSessionId_;
  uint32_t lastMediaCheckMs_;
  bool mediaCheckPending_;
  uint32_t mediaCheckRequestId_;
  uint32_t mediaCheckSentMs_;
  uint8_t resumeAttempts_;
  uint32_t bufferingSinceMs_;
  bool recoveryScheduled_;
  uint32_t recoveryScheduledAtMs_;
  uint32_t recoveryDelayMs_;
  uint8_t recoveryFailures_;

  bool volumeKnown_;
  bool volumeMuted_;
  float volumeLevel_;

  uint8_t serviceHeader_[4];
  size_t serviceHeaderBytes_;
  uint32_t serviceFrameLength_;
  size_t serviceFrameBytes_;
  uint32_t serviceFrameStartedMs_;
  uint8_t serviceFrame_[cast_protocol::kMaximumMessageSize];
};
