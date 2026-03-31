#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

// Breadboard wiring for Seeed Studio XIAO ESP32-C3:
// MOSI -> D5 -> GPIO7
// MISO -> D6 -> GPIO21
// SCK  -> D7 -> GPIO20
// CS   -> D8 -> GPIO8
// VCC  -> 3V3
// GND  -> GND
static constexpr int PIN_SD_CS   = 8;
static constexpr int PIN_SD_MISO = 21;
static constexpr int PIN_SD_MOSI = 7;
static constexpr int PIN_SD_SCK  = 20;

SPIClass sdSpi(FSPI);

static const char* cardTypeToString(uint8_t type) {
  switch (type) {
    case CARD_MMC:
      return "MMC";
    case CARD_SD:
      return "SDSC";
    case CARD_SDHC:
      return "SDHC/SDXC";
    default:
      return "UNKNOWN";
  }
}

static void printDirectory(fs::FS& fs, const char* dirname, uint8_t levels) {
  File root = fs.open(dirname);
  if (!root || !root.isDirectory()) {
    Serial.println("Failed to open root directory");
    return;
  }

  File file = root.openNextFile();
  while (file) {
    if (file.isDirectory()) {
      Serial.print("DIR : ");
      Serial.println(file.name());
      if (levels > 0) {
        printDirectory(fs, file.path(), levels - 1);
      }
    } else {
      Serial.print("FILE: ");
      Serial.print(file.name());
      Serial.print("  SIZE: ");
      Serial.println(file.size());
    }
    file = root.openNextFile();
  }
}

static void mountAndReport() {
  Serial.println();
  Serial.println("Starting SD card test...");
  Serial.printf("CS=%d MISO=%d MOSI=%d SCK=%d\n",
                PIN_SD_CS, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_SCK);

  sdSpi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);

  if (!SD.begin(PIN_SD_CS, sdSpi, 1000000)) {
    Serial.println("SD.begin() failed.");
    Serial.println("Check wiring, card format, and 3.3V power.");
    return;
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("No SD card detected.");
    return;
  }

  uint64_t totalBytes = SD.cardSize();
  uint64_t usedBytes = SD.usedBytes();

  Serial.println("SD card detected successfully.");
  Serial.print("Card type: ");
  Serial.println(cardTypeToString(cardType));
  Serial.printf("Card size: %.2f MB\n", totalBytes / (1024.0 * 1024.0));
  Serial.printf("Used space: %.2f MB\n", usedBytes / (1024.0 * 1024.0));
  Serial.printf("Free space: %.2f MB\n", (totalBytes - usedBytes) / (1024.0 * 1024.0));
  Serial.println("Root directory:");
  printDirectory(SD, "/", 1);

  File testFile = SD.open("/sd_test.txt", FILE_WRITE);
  if (!testFile) {
    Serial.println("Write test failed: could not create /sd_test.txt");
    return;
  }

  testFile.printf("SD test OK. Uptime: %lu ms\n", millis());
  testFile.close();
  Serial.println("Write test OK: /sd_test.txt updated.");
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("XIAO ESP32C3 SD card checker");
  mountAndReport();
}

void loop() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck >= 5000) {
    lastCheck = millis();
    Serial.println("Rechecking card presence...");
    mountAndReport();
  }
}
