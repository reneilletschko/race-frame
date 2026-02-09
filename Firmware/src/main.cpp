#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Adafruit_NeoPixel.h>
#include <WiFiManager.h>
#include <Preferences.h>

// ===================== Hardware =====================
constexpr uint8_t PIN_NEO_PIXEL = 21;
constexpr uint8_t NUM_PIXELS    = 1;
constexpr uint8_t RESET_BUTTON_PIN = 0;   // Boot-Taste (GPIO0) für Factory Reset verwenden

// ===================== Timing =====================
constexpr unsigned long RESET_HOLD_MS = 5000; // Zeit bis Reset ausgelöst wird (5 Sekunden)
constexpr unsigned long BOOT_IGNORE_MS = 3000; // GPIO0 beim Boot ignorieren

// ===================== Firmware =====================
const char* currentFirmwareVersion = "1.11";
const char* firmwareUrl = "https://github.com/reneilletschko/race-frame/releases/download/release/firmware.bin"; //https://github.com/ittipu/esp32_firmware/releases/download/esp32_firmware/firmware.ino.bin
const char* versionUrl = "https://raw.githubusercontent.com/reneilletschko/race-frame/refs/heads/main/Firmware/version.txt"; //https://raw.githubusercontent.com/ittipu/esp32_firmware/refs/heads/main/version.txt
const unsigned long updateCheckInterval = 5 * 60 * 1000;  // 5 minutes in milliseconds
unsigned long lastUpdateCheck = 0;

// ===================== Globals =====================
Adafruit_NeoPixel neoPixel(NUM_PIXELS, PIN_NEO_PIXEL, NEO_GRB + NEO_KHZ800);
WiFiManager wm;
Preferences prefs;

// ===================== Custom Config =====================
char cfgUser[32] = "";  //Platzhalter für Benutzername RaceBox Website
char cfgPass[32] = ""; //Platzhalter für Passwort RaceBox Website

// ===================== Reset Button =====================
unsigned long resetPressStart = 0;
bool resetTriggered = false;
unsigned long bootTime = 0;

// ===================== WiFiManager Parameters =====================
WiFiManagerParameter paramUser(
  "user", "User", cfgUser, sizeof(cfgUser)
);

WiFiManagerParameter paramPass(
  "pass",
  "Password",
  cfgPass,
  sizeof(cfgPass),
  "type=\"password\""
);

// ===================== OTA Updater =====================
String fetchLatestVersion() {
  HTTPClient http;
  http.begin(versionUrl);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String latestVersion = http.getString();
    latestVersion.trim();  // Remove any extra whitespace
    http.end();
    return latestVersion;
  } else {
    Serial.printf("Failed to fetch version. HTTP code: %d\n", httpCode);
    http.end();
    return "";
  }
}

bool startOTAUpdate(WiFiClient* client, int contentLength) {
  Serial.println("Initializing update...");
  if (!Update.begin(contentLength)) {
    Serial.printf("Update begin failed: %s\n", Update.errorString());
    return false;
  }

  Serial.println("Writing firmware...");
  size_t written = 0;
  int progress = 0;
  int lastProgress = 0;

  // Timeout variables
  const unsigned long timeoutDuration = 120*1000;  // 10 seconds timeout
  unsigned long lastDataTime = millis();

  while (written < contentLength) {
    if (client->available()) {
      uint8_t buffer[128];
      size_t len = client->read(buffer, sizeof(buffer));
      if (len > 0) {
        Update.write(buffer, len);
        written += len;

        // Calculate and print progress
        progress = (written * 100) / contentLength;
        if (progress != lastProgress) {
          Serial.printf("Writing Progress: %d%%\n", progress);
          lastProgress = progress;
        }
      }
    }
    // Check for timeout
    if (millis() - lastDataTime > timeoutDuration) {
      Serial.println("Timeout: No data received for too long. Aborting update...");
      Update.abort();
      return false;
    }
    yield();
  }
  Serial.println("\nWriting complete");

  if (written != contentLength) {
    Serial.printf("Error: Write incomplete. Expected %d but got %d bytes\n", contentLength, written);
    Update.abort();
    return false;
  }

  if (!Update.end()) {
    Serial.printf("Error: Update end failed: %s\n", Update.errorString());
    return false;
  }

  Serial.println("Update successfully completed");
  return true;
}

void downloadAndApplyFirmware() {
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(firmwareUrl);

  int httpCode = http.GET();
  Serial.printf("HTTP GET code: %d\n", httpCode);

  if (httpCode == HTTP_CODE_OK) {
    int contentLength = http.getSize();
    Serial.printf("Firmware size: %d bytes\n", contentLength);

    if (contentLength > 0) {
      WiFiClient* stream = http.getStreamPtr();
      if (startOTAUpdate(stream, contentLength)) {
        Serial.println("OTA update successful, restarting...");
        delay(2000);
        ESP.restart();
      } else {
        Serial.println("OTA update failed");
      }
    } else {
      Serial.println("Invalid firmware size");
    }
  } else {
    Serial.printf("Failed to fetch firmware. HTTP code: %d\n", httpCode);
  }
  http.end();
}

void checkForFirmwareUpdate() {
  Serial.println("Checking for firmware update...");
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected");
    return;
  }

  // Step 1: Fetch the latest version from GitHub
  String latestVersion = fetchLatestVersion();
  if (latestVersion == "") {
    Serial.println("Failed to fetch latest version");
    return;
  }
  Serial.println("Current Firmware Version: " + String(currentFirmwareVersion));
  Serial.println("Latest Firmware Version: " + latestVersion);

  // Step 2: Compare versions
  if (latestVersion != currentFirmwareVersion) {
    Serial.println("New firmware available. Starting OTA update...");
    downloadAndApplyFirmware();
  } else {
    Serial.println("Device is up to date.");
  }
}

// ===================== LED Helper =====================
void setLed(uint8_t r, uint8_t g, uint8_t b) {
  neoPixel.setPixelColor(0, neoPixel.Color(r, g, b));
  neoPixel.show();
}

// ===================== Preferences =====================
void loadCustomParams() {
  prefs.begin("config", true);
  prefs.getString("user", cfgUser, sizeof(cfgUser));
  prefs.getString("pass", cfgPass, sizeof(cfgPass));
  prefs.end();

  Serial.printf("User geladen: %s\n", cfgUser);
}

void saveCustomParams() {
  prefs.begin("config", false);
  prefs.putString("user", cfgUser);
  prefs.putString("pass", cfgPass);
  prefs.end();

  Serial.println("Custom Config gespeichert");
}

// ===================== Factory Reset =====================
void factoryReset() {
  Serial.println("FACTORY RESET");

  setLed(255, 0, 0);

  prefs.begin("config", false);
  prefs.clear();
  prefs.end();

  wm.resetSettings();

  delay(1000);
  ESP.restart();
}

// ===================== Validation =====================
bool validateCustomParams() {
  if (strlen(cfgUser) == 0 || strlen(cfgPass) == 0) {
    Serial.println("Validierung fehlgeschlagen: Felder dürfen nicht leer sein");
    factoryReset();
    return false;
  }
  return true;
}

// ===================== WiFiManager Callback =====================
void saveConfigCallback() {
  Serial.println("Config Portal gespeichert");

  strncpy(cfgUser, paramUser.getValue(), sizeof(cfgUser));
  strncpy(cfgPass, paramPass.getValue(), sizeof(cfgPass));

  if (!validateCustomParams()) {
    Serial.println("Ungültige Eingabe → Portal erneut");
    delay(1000);
    wm.startConfigPortal("RaceFrame-Setup");
    return;
  }

  saveCustomParams();
}

// ===================== WiFi Init =====================
void initWiFi() {
  loadCustomParams();

  wm.setShowInfoErase(false);
  wm.setShowInfoUpdate(false);
  wm.setBreakAfterConfig(true);
  wm.setConfigPortalBlocking(true);
  wm.setSaveConfigCallback(saveConfigCallback);

  wm.addParameter(&paramUser);
  wm.addParameter(&paramPass);

  Serial.println("Starte WiFi / Config Portal");
  setLed(0, 0, 255); // Blau

  if (!wm.autoConnect("RaceFrame-Setup")) {
    Serial.println("WLAN Verbindung fehlgeschlagen");
    ESP.restart();
  }

  Serial.print("WLAN verbunden, IP: ");
  Serial.println(WiFi.localIP());
  setLed(0, 255, 0); // Grün
}

// ===================== Reset Button Handler =====================
void handleResetButton() {
  // GPIO0 in den ersten Sekunden ignorieren
  if (millis() - bootTime < BOOT_IGNORE_MS) return;

  bool pressed = digitalRead(RESET_BUTTON_PIN) == LOW;

  if (pressed && resetPressStart == 0) {
    resetPressStart = millis();
    Serial.println("Reset-Taste (GPIO0) gedrückt...");
  }

  if (pressed && !resetTriggered) {
    unsigned long held = millis() - resetPressStart;

    if (held >= RESET_HOLD_MS) {
      resetTriggered = true;
      factoryReset();
    } else {
      // LED blinkt rot während gedrückt halten
      if ((millis() / 300) % 2 == 0) { // Blinken alle 300ms
        setLed(255, 0, 0);
      } else {
        setLed(0, 0, 0);
      }

      Serial.printf("Reset in %lu ms\n", RESET_HOLD_MS - held);
    }
  }

  if (!pressed) {
    resetPressStart = 0;
    resetTriggered = false;
  }
}

// ===================== Setup & Loop =====================
void setup() {
  Serial.begin(115200);

  delay(10000); // Zeit für seriellen Monitor, um sich zu verbinden und Boot-Logs zu sehen
  neoPixel.begin();
  setLed(0, 0, 0);

  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  bootTime = millis();

  Serial.println("\n=== RaceFrame Boot ===");
  Serial.println("Current Firmware Version: " + String(currentFirmwareVersion));
  Serial.println("GPIO0 wird als Reset-Taste verwendet");

  initWiFi();

  checkForFirmwareUpdate();
}

void loop() {
  handleResetButton();
  unsigned long currentMillis = millis();
  if (currentMillis - lastUpdateCheck >= updateCheckInterval) {
    lastUpdateCheck = currentMillis;
    checkForFirmwareUpdate();}
  setLed(255, 0, 0);
  delay(100);
  setLed(0, 255, 0);
  delay(100);
  setLed(0, 0, 255);
  delay(100);

}
