#include <Arduino.h>
#include <ESPmDNS.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <esp_system.h>

#include <algorithm>
#include <vector>

#include "AppConfig.h"
#include "CastClient.h"
#include "Diagnostics.h"
#include "Oe3Logo.h"
#include "TextEncoding.h"

namespace {

struct CastDevice {
  String name;
  String id;
  IPAddress address;
  uint16_t port;
};

enum class UiPlaybackState { kUnknown, kPlaying, kStopped };
enum class StatusVisual { kReady, kBusy, kPlaying, kStopped, kError };

constexpr uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue) {
  return static_cast<uint16_t>(((red & 0xf8) << 8) |
                               ((green & 0xfc) << 3) | (blue >> 3));
}

constexpr uint16_t kBackground = rgb565(4, 9, 18);
constexpr uint16_t kCard = rgb565(10, 20, 35);
constexpr uint16_t kCardBorder = rgb565(25, 45, 68);
constexpr uint16_t kMuted = rgb565(132, 151, 177);
constexpr uint16_t kOe3Blue = rgb565(1, 35, 80);
constexpr uint16_t kOe3Red = rgb565(228, 28, 42);
constexpr uint16_t kGreen = rgb565(36, 201, 123);
constexpr uint16_t kAmber = rgb565(245, 166, 35);

std::vector<CastDevice> devices;
size_t selectedDevice = 0;
String statusText = "Starting";
String accessPointName;
String accessPointPassword;
String hostName;
bool mdnsStarted = false;
uint32_t lastScanMs = 0;
uint32_t wifiLostAtMs = 0;
uint32_t lastEqualizerDrawMs = 0;
UiPlaybackState playbackState = UiPlaybackState::kUnknown;
bool diagnosticsServerStarted = false;
IPAddress displayedLocalIp;

void updateCastStatus(const String& message);

CastClient& castController() {
  static CastClient controller(updateCastStatus);
  return controller;
}

WebServer& diagnosticsServer() {
  static WebServer server(80);
  return server;
}

void sendDiagnosticsLog() {
  WebServer& server = diagnosticsServer();
  server.setContentLength(diagnostics::contentLength());
  server.send(200, "text/plain; charset=utf-8", "");
  WiFiClient client = server.client();
  if (!diagnostics::writeTo(client)) {
    client.stop();
  }
}

void startDiagnosticsServer() {
  if (diagnosticsServerStarted) {
    return;
  }
  WebServer& server = diagnosticsServer();
  server.on("/", HTTP_GET, sendDiagnosticsLog);
  server.on("/log", HTTP_GET, sendDiagnosticsLog);
  server.begin();
  diagnosticsServerStarted = true;
  diagnostics::logf("HTTP diagnostics started url=http://%s/log",
                    WiFi.localIP().toString().c_str());
}

String shortened(const String& value, size_t maximumCharacters) {
  if (value.length() <= maximumCharacters) {
    return value;
  }
  if (maximumCharacters <= 3) {
    return value.substring(0, maximumCharacters);
  }
  return value.substring(0, maximumCharacters - 3) + "...";
}

String textForDisplay(const String& utf8Text) {
  const std::string cp437Text = text_encoding::utf8ToCp437(
      std::string(utf8Text.c_str(), utf8Text.length()));
  return String(cp437Text.c_str());
}

void drawWrappedText(const String& text,
                     int32_t x,
                     int32_t y,
                     size_t charactersPerLine,
                     size_t maximumLines) {
  size_t offset = 0;
  for (size_t line = 0;
       line < maximumLines && offset < static_cast<size_t>(text.length());
       ++line) {
    size_t count = text.length() - offset;
    if (count > charactersPerLine) {
      count = charactersPerLine;
    }
    M5.Display.setCursor(x, y + static_cast<int32_t>(line * 10));
    M5.Display.print(text.substring(offset, offset + count));
    offset += count;
  }
}

void drawLogoRuns(int32_t x,
                  int32_t y,
                  const oe3_logo::Run* runs,
                  size_t runCount,
                  uint16_t color) {
  for (size_t index = 0; index < runCount; ++index) {
    oe3_logo::Run run;
    memcpy_P(&run, &runs[index], sizeof(run));
    M5.Display.drawFastHLine(x + run.x, y + run.y, run.length, color);
  }
}

void drawOe3Logo(int32_t x, int32_t y) {
  drawLogoRuns(x, y, oe3_logo::kBlueRuns, oe3_logo::kBlueRunCount,
               kOe3Blue);
  drawLogoRuns(x, y, oe3_logo::kRedRuns, oe3_logo::kRedRunCount,
               kOe3Red);
}

StatusVisual statusVisual() {
  String normalized = statusText;
  normalized.toLowerCase();
  if (normalized.indexOf("error") >= 0 ||
      normalized.indexOf("failed") >= 0 ||
      normalized.indexOf("could not") >= 0 ||
      normalized.indexOf("did not") >= 0 ||
      normalized.indexOf("unavailable") >= 0 ||
      normalized.indexOf("not connected") >= 0 ||
      normalized.indexOf("no cast receiver") >= 0 ||
      normalized.indexOf("no receiver") >= 0) {
    return StatusVisual::kError;
  }
  if (normalized.indexOf("playing") >= 0) {
    return StatusVisual::kPlaying;
  }
  if (normalized.indexOf("stopped") >= 0) {
    return StatusVisual::kStopped;
  }
  if (normalized.indexOf("connecting") >= 0 ||
      normalized.indexOf("checking") >= 0 ||
      normalized.indexOf("scanning") >= 0 ||
      normalized.indexOf("starting") >= 0 ||
      normalized.indexOf("opening") >= 0 ||
      normalized.indexOf("sending") >= 0 ||
      normalized.indexOf("buffering") >= 0 ||
      normalized.indexOf("stopping") >= 0) {
    return StatusVisual::kBusy;
  }
  return StatusVisual::kReady;
}

void drawStatusIcon(int32_t x, int32_t y, StatusVisual visual) {
  uint16_t color = kOe3Blue;
  if (visual == StatusVisual::kPlaying) {
    color = kGreen;
  } else if (visual == StatusVisual::kBusy) {
    color = kAmber;
  } else if (visual == StatusVisual::kError) {
    color = kOe3Red;
  } else if (visual == StatusVisual::kStopped) {
    color = kMuted;
  }

  M5.Display.fillCircle(x, y, 18, color);
  if (visual == StatusVisual::kPlaying) {
    M5.Display.fillTriangle(x - 4, y - 8, x - 4, y + 8, x + 9, y,
                            TFT_WHITE);
  } else if (visual == StatusVisual::kStopped) {
    M5.Display.fillRoundRect(x - 6, y - 6, 12, 12, 2, TFT_WHITE);
  } else if (visual == StatusVisual::kError) {
    M5.Display.fillRect(x - 2, y - 9, 4, 12, TFT_WHITE);
    M5.Display.fillCircle(x, y + 8, 2, TFT_WHITE);
  } else if (visual == StatusVisual::kBusy) {
    M5.Display.drawCircle(x, y, 9, TFT_WHITE);
    M5.Display.fillCircle(x + 7, y - 6, 3, TFT_WHITE);
  } else {
    M5.Display.drawLine(x - 8, y, x - 2, y + 6, TFT_WHITE);
    M5.Display.drawLine(x - 2, y + 6, x + 9, y - 7, TFT_WHITE);
  }
}

void drawEqualizer() {
  static const uint8_t levels[4][7] = {
      {9, 23, 15, 31, 18, 26, 11},
      {18, 11, 29, 17, 32, 14, 24},
      {27, 17, 10, 26, 15, 31, 18},
      {13, 30, 20, 11, 25, 17, 29},
  };

  M5.Display.fillRect(12, 134, 74, 46, kCard);
  const uint8_t frame = (millis() / 180) % 4;
  for (uint8_t index = 0; index < 7; ++index) {
    const uint8_t height =
        playbackState == UiPlaybackState::kPlaying ? levels[frame][index] : 4;
    const uint16_t color =
        playbackState == UiPlaybackState::kPlaying
            ? (index == 2 || index == 4 ? kOe3Red : kOe3Blue)
            : kCardBorder;
    M5.Display.fillRoundRect(15 + index * 10, 178 - height, 6, height, 2,
                             color);
  }
}

void drawButton(int32_t x,
                uint16_t fillColor,
                const char* key,
                const char* label) {
  constexpr int32_t y = 198;
  constexpr int32_t width = 100;
  constexpr int32_t height = 36;
  M5.Display.fillRoundRect(x, y, width, height, 7, fillColor);
  M5.Display.drawRoundRect(x, y, width, height, 7, kCardBorder);
  M5.Display.fillCircle(x + 17, y + 18, 10, kBackground);
  M5.Display.setTextColor(TFT_WHITE, kBackground);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(x + 14, y + 14);
  M5.Display.print(key);
  M5.Display.setTextColor(TFT_WHITE, fillColor);
  M5.Display.setCursor(x + 33, y + 14);
  M5.Display.print(label);
}

void drawMainScreen() {
  M5.Display.startWrite();
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setAttribute(lgfx::UTF8_SWITCH, true);
  M5.Display.cp437(false);
  M5.Display.fillScreen(kBackground);

  M5.Display.fillRoundRect(6, 6, 86, 184, 10, kCard);
  M5.Display.drawRoundRect(6, 6, 86, 184, 10, kCardBorder);
  drawOe3Logo(10, 11);

  M5.Display.fillCircle(15, 91, 4,
                        WiFi.status() == WL_CONNECTED ? kGreen : kOe3Red);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(kMuted, kCard);
  M5.Display.setCursor(24, 88);
  if (WiFi.status() == WL_CONNECTED) {
    M5.Display.setAttribute(lgfx::UTF8_SWITCH, false);
    M5.Display.cp437(true);
    M5.Display.print(shortened(textForDisplay(WiFi.SSID()), 10));
    M5.Display.setAttribute(lgfx::UTF8_SWITCH, true);
    M5.Display.cp437(false);
  } else {
    M5.Display.print("OFFLINE");
  }
  M5.Display.drawFastHLine(13, 106, 72, kCardBorder);

  const bool isPlaying = playbackState == UiPlaybackState::kPlaying;
  M5.Display.fillRoundRect(14, 113, 70, 17, 8,
                           isPlaying ? kOe3Red : kCardBorder);
  M5.Display.setTextColor(TFT_WHITE, isPlaying ? kOe3Red : kCardBorder);
  M5.Display.setCursor(isPlaying ? 29 : 24, 118);
  M5.Display.print(isPlaying ? "ON AIR" : "LIVE RADIO");
  drawEqualizer();

  M5.Display.fillRoundRect(98, 6, 216, 84, 10, kCard);
  M5.Display.drawRoundRect(98, 6, 216, 84, 10, kCardBorder);
  M5.Display.fillRoundRect(109, 16, 15, 11, 2, kOe3Blue);
  M5.Display.fillCircle(112, 24, 2, TFT_WHITE);
  M5.Display.drawLine(112, 24, 119, 17, TFT_WHITE);
  M5.Display.setTextColor(kMuted, kCard);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(131, 18);
  M5.Display.print("CAST RECEIVER");
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);

  if (devices.empty()) {
    M5.Display.setTextColor(TFT_WHITE, kCard);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(109, 39);
    M5.Display.print("No receiver");
  } else {
    const String displayName = textForDisplay(devices[selectedDevice].name);
    M5.Display.setAttribute(lgfx::UTF8_SWITCH, false);
    M5.Display.cp437(true);
    M5.Display.setTextColor(TFT_WHITE, kCard);
    if (displayName.length() <= 16) {
      M5.Display.setTextSize(2);
      M5.Display.setCursor(109, 37);
      M5.Display.print(displayName);
    } else {
      M5.Display.setTextSize(1);
      M5.Display.setCursor(109, 42);
      M5.Display.print(shortened(displayName, 31));
    }
    M5.Display.setAttribute(lgfx::UTF8_SWITCH, true);
    M5.Display.cp437(false);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(kMuted, kCard);
    M5.Display.setCursor(109, 69);
    M5.Display.printf("%s:%u",
                      devices[selectedDevice].address.toString().c_str(),
                      static_cast<unsigned int>(
                          devices[selectedDevice].port));
    M5.Display.fillRoundRect(269, 64, 36, 16, 8, kOe3Blue);
    M5.Display.setTextColor(TFT_WHITE, kOe3Blue);
    M5.Display.setCursor(272, 69);
    M5.Display.printf("%u/%u", static_cast<unsigned int>(selectedDevice + 1),
                      static_cast<unsigned int>(devices.size()));
  }

  M5.Display.fillRoundRect(98, 96, 216, 94, 10, kCard);
  M5.Display.drawRoundRect(98, 96, 216, 94, 10, kCardBorder);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(kMuted, kCard);
  M5.Display.setCursor(109, 106);
  M5.Display.print("STATUS");
  M5.Display.setCursor(198, 106);
  if (WiFi.status() == WL_CONNECTED) {
    M5.Display.printf("M5 %s", WiFi.localIP().toString().c_str());
  } else {
    M5.Display.print("M5 OFFLINE");
  }
  drawStatusIcon(120, 146, statusVisual());

  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setAttribute(lgfx::UTF8_SWITCH, false);
  M5.Display.cp437(true);
  M5.Display.setTextColor(TFT_WHITE, kCard);
  drawWrappedText(textForDisplay(statusText), 145, 126, 26, 5);
  M5.Display.setAttribute(lgfx::UTF8_SWITCH, true);
  M5.Display.cp437(false);

  drawButton(4, kCard, "A", "NEXT");
  drawButton(110, kOe3Red, "B", "START/STOP");
  drawButton(216, kOe3Blue, "C", "SCAN");
  M5.Display.endWrite();
  lastEqualizerDrawMs = millis();
  displayedLocalIp =
      WiFi.status() == WL_CONNECTED ? WiFi.localIP() : IPAddress();
}

void updateCastStatus(const String& message) {
  statusText = message;
  String normalized = message;
  normalized.toLowerCase();
  if (normalized.indexOf("is playing") >= 0) {
    playbackState = UiPlaybackState::kPlaying;
  } else if (normalized.indexOf("stream is stopped") >= 0 ||
             normalized.indexOf("already stopped") >= 0) {
    playbackState = UiPlaybackState::kStopped;
  }
  drawMainScreen();
}

void drawConfigPortal(WiFiManager*) {
  M5.Display.startWrite();
  M5.Display.setFont(&fonts::Font0);
  M5.Display.fillScreen(kBackground);
  M5.Display.fillRoundRect(6, 6, 86, 80, 10, kCard);
  M5.Display.drawRoundRect(6, 6, 86, 80, 10, kCardBorder);
  drawOe3Logo(10, 10);

  M5.Display.fillRoundRect(98, 6, 216, 80, 10, kCard);
  M5.Display.drawRoundRect(98, 6, 216, 80, 10, kCardBorder);
  M5.Display.setTextColor(TFT_WHITE, kCard);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(109, 15);
  M5.Display.print("WIFI SETUP");
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(kMuted, kCard);
  M5.Display.setCursor(109, 39);
  M5.Display.print(accessPointName);
  M5.Display.setCursor(109, 58);
  M5.Display.printf("Password: %s", accessPointPassword.c_str());

  const char* steps[] = {
      "Connect your phone to the hotspot",
      "Open http://192.168.4.1",
      "Choose your 2.4 GHz WiFi",
  };
  for (uint8_t index = 0; index < 3; ++index) {
    const int32_t y = 96 + index * 35;
    M5.Display.fillRoundRect(6, y, 308, 29, 7, kCard);
    M5.Display.drawRoundRect(6, y, 308, 29, 7, kCardBorder);
    M5.Display.fillCircle(22, y + 14, 9,
                          index == 0 ? kOe3Red : kOe3Blue);
    M5.Display.setTextColor(TFT_WHITE,
                            index == 0 ? kOe3Red : kOe3Blue);
    M5.Display.setCursor(19, y + 11);
    M5.Display.print(index + 1);
    M5.Display.setTextColor(TFT_WHITE, kCard);
    M5.Display.setCursor(39, y + 11);
    M5.Display.print(steps[index]);
  }
  M5.Display.setTextColor(kMuted, kBackground);
  M5.Display.setCursor(34, 210);
  M5.Display.print("The setup hotspot closes after connection.");
  M5.Display.endWrite();
}

void createDeviceIdentity() {
  const uint32_t chipId =
      static_cast<uint32_t>(ESP.getEfuseMac() & 0x00ffffffULL);
  char buffer[24];
  snprintf(buffer, sizeof(buffer), "M5Cast-%06X", chipId);
  accessPointName = buffer;
  snprintf(buffer, sizeof(buffer), "cast-%08X",
           static_cast<unsigned int>(esp_random()));
  accessPointPassword = buffer;
  snprintf(buffer, sizeof(buffer), "m5cast-%06x", chipId);
  hostName = buffer;
}

bool connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostName.c_str());

  WiFiManager manager;
  manager.setConnectTimeout(20);
  manager.setAPCallback(drawConfigPortal);
  manager.setDebugOutput(false);

  statusText = "Connecting to WiFi";
  drawMainScreen();
  return manager.autoConnect(accessPointName.c_str(),
                             accessPointPassword.c_str());
}

bool startMdns() {
  if (mdnsStarted) {
    MDNS.end();
    mdnsStarted = false;
  }

  mdnsStarted = MDNS.begin(hostName.c_str());
  if (!mdnsStarted) {
    statusText = "Could not start mDNS";
    drawMainScreen();
  }
  return mdnsStarted;
}

void scanCastDevices() {
  if (WiFi.status() != WL_CONNECTED) {
    statusText = "WiFi is not connected";
    drawMainScreen();
    return;
  }
  if (!mdnsStarted && !startMdns()) {
    return;
  }

  IPAddress previousAddress;
  String previousId;
  bool hadSelection = !devices.empty();
  if (hadSelection) {
    previousAddress = devices[selectedDevice].address;
    previousId = devices[selectedDevice].id;
  }

  statusText = "Scanning for Cast receivers...";
  drawMainScreen();
  const int resultCount = MDNS.queryService("googlecast", "tcp");

  std::vector<CastDevice> discovered;
  for (int index = 0;
       index < resultCount &&
       discovered.size() < app_config::kMaxCastDevices;
       ++index) {
    const IPAddress address = MDNS.IP(index);
    if (address == IPAddress()) {
      continue;
    }

    uint16_t port = MDNS.port(index);
    if (port == 0) {
      port = app_config::kDefaultCastPort;
    }
    const String deviceId = MDNS.txt(index, "id");

    bool duplicate = false;
    for (const CastDevice& item : discovered) {
      if ((!deviceId.isEmpty() && item.id == deviceId) ||
          (item.address == address && item.port == port)) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }

    String friendlyName = MDNS.txt(index, "fn");
    if (friendlyName.isEmpty()) {
      friendlyName = MDNS.hostname(index);
    }
    if (friendlyName.isEmpty()) {
      friendlyName = address.toString();
    }
    diagnostics::logf(
        "MDNS cast name=\"%s\" id=%s model=\"%s\" addr=%s:%u ca=%s "
        "ve=%s",
        friendlyName.c_str(), deviceId.c_str(),
        MDNS.txt(index, "md").c_str(), address.toString().c_str(),
        static_cast<unsigned int>(port),
        MDNS.txt(index, "ca").c_str(), MDNS.txt(index, "ve").c_str());
    discovered.push_back({friendlyName, deviceId, address, port});
  }

  std::sort(discovered.begin(), discovered.end(),
            [](const CastDevice& left, const CastDevice& right) {
              return left.name.compareTo(right.name) < 0;
            });
  devices.swap(discovered);
  selectedDevice = 0;
  if (hadSelection) {
    for (size_t index = 0; index < devices.size(); ++index) {
      if ((!previousId.isEmpty() && devices[index].id == previousId) ||
          devices[index].address == previousAddress) {
        selectedDevice = index;
        break;
      }
    }
  }
  for (const CastDevice& item : devices) {
    castController().updateDiscoveredEndpoint(item.id.c_str(), item.address,
                                              item.port);
  }

  lastScanMs = millis();
  if (devices.empty()) {
    statusText = "No Cast receiver found. Press SCAN.";
  } else {
    statusText = String(devices.size()) + " receiver(s) found";
  }
  drawMainScreen();

  diagnostics::logf("MDNS scan results=%d retained=%u", resultCount,
                    static_cast<unsigned int>(devices.size()));
}

void resetWifiIfRequested() {
  M5.update();
  delay(80);
  M5.update();
  if (!M5.BtnA.isPressed() || !M5.BtnC.isPressed()) {
    return;
  }

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(18, 90);
  M5.Display.print("Clearing WiFi settings");
  WiFi.mode(WIFI_STA);
  WiFiManager manager;
  manager.resetSettings();
  delay(1500);
  ESP.restart();
}

void toggleSelectedStream() {
  CastClient& castClient = castController();
  if (devices.empty()) {
    if (castClient.cancelMaintenance()) {
      playbackState = UiPlaybackState::kStopped;
      statusText = "Playback recovery stopped";
      drawMainScreen();
      return;
    }
    statusText = "No receiver selected. Press SCAN.";
    drawMainScreen();
    return;
  }

  const CastDevice device = devices[selectedDevice];
  diagnostics::logf("BUTTON toggle name=\"%s\" id=%s target=%s:%u",
                    device.name.c_str(), device.id.c_str(),
                    device.address.toString().c_str(),
                    static_cast<unsigned int>(device.port));
  const CastClient::ToggleResult result =
      castClient.toggle(device.address, device.port, device.id.c_str(),
                        app_config::kStreamUrl,
                        app_config::kStreamContentType,
                        app_config::kStreamTitle);
  if (result == CastClient::ToggleResult::kStarted) {
    playbackState = UiPlaybackState::kPlaying;
    statusText = String("Playing OE3 on ") + device.name;
  } else if (result == CastClient::ToggleResult::kStopped) {
    playbackState = UiPlaybackState::kStopped;
    statusText = String("Stopped stream on ") + device.name;
  } else {
    statusText = String("Toggle failed: ") + castClient.lastError();
  }
  drawMainScreen();
}

void handleWifiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiLostAtMs == 0) {
      wifiLostAtMs = millis() == 0 ? 1 : millis();
      statusText = "WiFi lost; reconnecting...";
      diagnostics::logf("WIFI lost status=%d", static_cast<int>(WiFi.status()));
      drawMainScreen();
      WiFi.reconnect();
    } else if (millis() - wifiLostAtMs >=
               app_config::kWifiReconnectTimeoutMs) {
      if (castController().isMaintainingPlayback()) {
        wifiLostAtMs = millis();
        statusText = "WiFi unavailable; still retrying";
        diagnostics::logf("WIFI still unavailable; playback recovery retained");
        drawMainScreen();
        WiFi.reconnect();
        return;
      }
      statusText = "WiFi unavailable; restarting";
      diagnostics::logf("WIFI unavailable; restarting M5Stack");
      drawMainScreen();
      delay(1000);
      ESP.restart();
    }
    return;
  }

  if (wifiLostAtMs != 0) {
    wifiLostAtMs = 0;
    statusText = "WiFi reconnected";
    diagnostics::logf("WIFI reconnected ip=%s rssi=%d",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
    drawMainScreen();
    startMdns();
    scanCastDevices();
  }
}

}  // namespace

void setup() {
  auto m5Config = M5.config();
  m5Config.serial_baudrate = 115200;
  m5Config.internal_imu = false;
  m5Config.internal_rtc = false;
  m5Config.internal_spk = false;
  m5Config.internal_mic = false;
  M5.begin(m5Config);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(128);
  M5.Display.setTextWrap(false);
  diagnostics::begin();

  createDeviceIdentity();
  diagnostics::logf(
      "BOOT build=%s %s heap=%u psram=%u reset=%d", __DATE__, __TIME__,
      ESP.getFreeHeap(), ESP.getFreePsram(),
      static_cast<int>(esp_reset_reason()));
  resetWifiIfRequested();
  drawMainScreen();

  if (!connectWifi()) {
    statusText = "WiFi setup failed; restarting";
    drawMainScreen();
    delay(3000);
    ESP.restart();
  }

  statusText = String("Connected to ") + WiFi.SSID();
  diagnostics::logf("WIFI connected ip=%s rssi=%d channel=%d",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                    WiFi.channel());
  drawMainScreen();
  startDiagnosticsServer();
  startMdns();
  scanCastDevices();
}

void loop() {
  M5.update();
  handleWifiConnection();
  if (WiFi.status() == WL_CONNECTED &&
      WiFi.localIP() != displayedLocalIp) {
    drawMainScreen();
  }
  const bool middleButton =
      M5.BtnB.wasClicked() || M5.BtnB.wasHold();
  bool middleButtonHandled = false;

  if (middleButton && WiFi.status() != WL_CONNECTED &&
      castController().cancelMaintenance()) {
    middleButtonHandled = true;
    playbackState = UiPlaybackState::kStopped;
    statusText = "Playback recovery stopped";
    drawMainScreen();
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (M5.BtnA.wasPressed() && !devices.empty()) {
      selectedDevice = (selectedDevice + 1) % devices.size();
      statusText = String("Selected ") + devices[selectedDevice].name;
      drawMainScreen();
    }
    if (middleButton && !middleButtonHandled) {
      toggleSelectedStream();
    }

    if (M5.BtnC.wasPressed()) {
      scanCastDevices();
    }

    const uint32_t scanInterval =
        devices.empty() ? app_config::kEmptyScanIntervalMs
                        : app_config::kPeriodicScanIntervalMs;
    if (millis() - lastScanMs >= scanInterval) {
      scanCastDevices();
    }
  }
  castController().service();
  if (diagnosticsServerStarted && WiFi.status() == WL_CONNECTED &&
      !castController().isMaintainingPlayback()) {
    diagnosticsServer().handleClient();
  }

  if (playbackState == UiPlaybackState::kPlaying &&
      millis() - lastEqualizerDrawMs >= 180) {
    M5.Display.startWrite();
    drawEqualizer();
    M5.Display.endWrite();
    lastEqualizerDrawMs = millis();
  }

  delay(10);
}
