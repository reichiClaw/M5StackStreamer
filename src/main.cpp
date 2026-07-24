#include <Arduino.h>
#include <ESPmDNS.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiManager.h>

#include <algorithm>
#include <vector>

#include "AppConfig.h"
#include "CastClient.h"

namespace {

struct CastDevice {
  String name;
  IPAddress address;
  uint16_t port;
};

std::vector<CastDevice> devices;
size_t selectedDevice = 0;
String statusText = "Starting";
String accessPointName;
String accessPointPassword;
String hostName;
bool mdnsStarted = false;
uint32_t lastScanMs = 0;
uint32_t wifiLostAtMs = 0;

String shortened(const String& value, size_t maximumCharacters) {
  if (value.length() <= maximumCharacters) {
    return value;
  }
  if (maximumCharacters <= 3) {
    return value.substring(0, maximumCharacters);
  }
  return value.substring(0, maximumCharacters - 3) + "...";
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

void drawMainScreen() {
  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.fillRect(0, 0, M5.Display.width(), 31, TFT_NAVY);
  M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(8, 8);
  M5.Display.print("M5 OE3 CAST");

  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  M5.Display.setCursor(8, 39);
  if (WiFi.status() == WL_CONNECTED) {
    M5.Display.printf("WiFi: %s  %s",
                      shortened(WiFi.SSID(), 24).c_str(),
                      WiFi.localIP().toString().c_str());
  } else {
    M5.Display.print("WiFi: disconnected");
  }

  M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Display.setCursor(8, 62);
  M5.Display.print("CAST RECEIVER");
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(8, 77);
  if (devices.empty()) {
    M5.Display.print("No receiver found");
  } else {
    M5.Display.print(shortened(devices[selectedDevice].name, 25));
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    M5.Display.setCursor(8, 101);
    M5.Display.printf("%s:%u  (%u/%u)",
                      devices[selectedDevice].address.toString().c_str(),
                      devices[selectedDevice].port,
                      static_cast<unsigned int>(selectedDevice + 1),
                      static_cast<unsigned int>(devices.size()));
  }

  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Display.setCursor(8, 126);
  M5.Display.print("STATUS");
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  drawWrappedText(statusText, 8, 142, 50, 5);

  const int32_t buttonTop = M5.Display.height() - 30;
  const int32_t third = M5.Display.width() / 3;
  M5.Display.fillRect(0, buttonTop, M5.Display.width(), 30, TFT_DARKGREY);
  M5.Display.drawFastVLine(third, buttonTop, 30, TFT_BLACK);
  M5.Display.drawFastVLine(third * 2, buttonTop, 30, TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_DARKGREY);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(37, buttonTop + 11);
  M5.Display.print("NEXT");
  M5.Display.setCursor(third + 17, buttonTop + 11);
  M5.Display.print("START / STOP");
  M5.Display.setCursor(third * 2 + 38, buttonTop + 11);
  M5.Display.print("SCAN");
  M5.Display.endWrite();
}

void updateCastStatus(const String& message) {
  statusText = message;
  drawMainScreen();
}

void drawConfigPortal(WiFiManager*) {
  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.fillRect(0, 0, M5.Display.width(), 31, TFT_ORANGE);
  M5.Display.setTextColor(TFT_BLACK, TFT_ORANGE);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(8, 8);
  M5.Display.print("WIFI SETUP");

  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 48);
  M5.Display.print("1. Connect your phone to:");
  M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(8, 67);
  M5.Display.print(accessPointName);

  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 98);
  M5.Display.printf("Password: %s", accessPointPassword.c_str());
  M5.Display.setCursor(8, 122);
  M5.Display.print("2. Open http://192.168.4.1");
  M5.Display.setCursor(8, 146);
  M5.Display.print("3. Choose your 2.4 GHz WiFi");
  M5.Display.setCursor(8, 170);
  M5.Display.print("The setup hotspot closes after connection.");
  M5.Display.endWrite();
}

void createDeviceIdentity() {
  const uint32_t chipId =
      static_cast<uint32_t>(ESP.getEfuseMac() & 0x00ffffffULL);
  char buffer[24];
  snprintf(buffer, sizeof(buffer), "M5Cast-%06X", chipId);
  accessPointName = buffer;
  snprintf(buffer, sizeof(buffer), "cast-%06X", chipId);
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
  manager.setDebugOutput(true);

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
  bool hadSelection = !devices.empty();
  if (hadSelection) {
    previousAddress = devices[selectedDevice].address;
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

    bool duplicate = false;
    for (const CastDevice& item : discovered) {
      if (item.address == address && item.port == port) {
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
    discovered.push_back({friendlyName, address, port});
  }

  std::sort(discovered.begin(), discovered.end(),
            [](const CastDevice& left, const CastDevice& right) {
              return left.name.compareTo(right.name) < 0;
            });
  devices.swap(discovered);
  selectedDevice = 0;
  if (hadSelection) {
    for (size_t index = 0; index < devices.size(); ++index) {
      if (devices[index].address == previousAddress) {
        selectedDevice = index;
        break;
      }
    }
  }

  lastScanMs = millis();
  if (devices.empty()) {
    statusText = "No Cast receiver found. Press SCAN.";
  } else {
    statusText = String(devices.size()) + " receiver(s) found";
  }
  drawMainScreen();

  Serial.printf("Cast discovery returned %d service(s), kept %u\n",
                resultCount, static_cast<unsigned int>(devices.size()));
  for (const CastDevice& item : devices) {
    Serial.printf("  %s at %s:%u\n", item.name.c_str(),
                  item.address.toString().c_str(), item.port);
  }
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
  if (devices.empty()) {
    statusText = "No receiver selected. Press SCAN.";
    drawMainScreen();
    return;
  }

  CastClient castClient(updateCastStatus);
  const CastDevice device = devices[selectedDevice];
  const CastClient::ToggleResult result =
      castClient.toggle(device.address, device.port, app_config::kStreamUrl,
                        app_config::kStreamContentType,
                        app_config::kStreamTitle);
  if (result == CastClient::ToggleResult::kStarted) {
    statusText = String("Playing OE3 on ") + device.name;
  } else if (result == CastClient::ToggleResult::kStopped) {
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
      drawMainScreen();
      WiFi.reconnect();
    } else if (millis() - wifiLostAtMs >=
               app_config::kWifiReconnectTimeoutMs) {
      statusText = "WiFi unavailable; restarting";
      drawMainScreen();
      delay(1000);
      ESP.restart();
    }
    return;
  }

  if (wifiLostAtMs != 0) {
    wifiLostAtMs = 0;
    statusText = "WiFi reconnected";
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

  createDeviceIdentity();
  resetWifiIfRequested();
  drawMainScreen();

  if (!connectWifi()) {
    statusText = "WiFi setup failed; restarting";
    drawMainScreen();
    delay(3000);
    ESP.restart();
  }

  statusText = String("Connected to ") + WiFi.SSID();
  drawMainScreen();
  startMdns();
  scanCastDevices();
}

void loop() {
  M5.update();
  handleWifiConnection();

  if (WiFi.status() == WL_CONNECTED) {
    if (M5.BtnA.wasPressed() && !devices.empty()) {
      selectedDevice = (selectedDevice + 1) % devices.size();
      statusText = String("Selected ") + devices[selectedDevice].name;
      drawMainScreen();
    }
    if (M5.BtnB.wasClicked() || M5.BtnB.wasHold()) {
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

  delay(10);
}
