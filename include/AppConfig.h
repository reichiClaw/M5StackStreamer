#pragma once

#include <stddef.h>
#include <stdint.h>

namespace app_config {

constexpr char kStreamUrl[] = "http://orf-live.ors-shoutcast.at/oe3-q2a";
constexpr char kStreamContentType[] = "audio/mpeg";
constexpr char kStreamTitle[] = "Hitradio OE3";

constexpr size_t kMaxCastDevices = 12;
constexpr uint16_t kDefaultCastPort = 8009;
constexpr uint32_t kEmptyScanIntervalMs = 30000;
constexpr uint32_t kPeriodicScanIntervalMs = 5 * 60 * 1000;
constexpr uint32_t kWifiReconnectTimeoutMs = 30000;

// Internet monitoring probes the stream host itself, so "online" means
// "OE3 can actually be streamed", not merely "some gateway responds".
constexpr char kInternetProbeHost[] = "orf-live.ors-shoutcast.at";
constexpr uint16_t kInternetProbePort = 80;
constexpr uint32_t kInternetProbeTimeoutMs = 2000;
constexpr uint32_t kInternetCheckOnlineIntervalMs = 60000;
constexpr uint32_t kInternetCheckOfflineIntervalMs = 15000;
constexpr uint32_t kInternetDnsRefreshMs = 10 * 60 * 1000;

}  // namespace app_config
