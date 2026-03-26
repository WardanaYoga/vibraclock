#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ── OLED ──────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_SDA        8
#define OLED_SCL        9
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ── BLE Nordic UART Service ───────────────────────
#define SERVICE_UUID     "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_UUID_RX     "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_UUID_TX     "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

BLECharacteristic *pTxChar;
bool bleConnected  = false;
bool lastConnState = false;

// ── State Jam ─────────────────────────────────────
int  baseHour = 0, baseMin = 0, baseSec = 0;
bool timeValid      = false;
unsigned long syncMillis = 0;
int  lastDrawnSec   = -1;

// ─────────────────────────────────────────────────
// Parse "HH:MM:SS" dari BLE
// ─────────────────────────────────────────────────
bool parseTime(const String &s) {
  int h, m, sec;
  if (sscanf(s.c_str(), "%d:%d:%d", &h, &m, &sec) == 3 &&
      h >= 0 && h < 24 && m >= 0 && m < 60 && sec >= 0 && sec < 60) {
    baseHour  = h; baseMin = m; baseSec = sec;
    syncMillis = millis();
    timeValid  = true;
    return true;
  }
  return false;
}

// ─────────────────────────────────────────────────
// BLE Callbacks
// ─────────────────────────────────────────────────
class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer *s)    override { bleConnected = true; }
  void onDisconnect(BLEServer *s) override {
    bleConnected = false;
    BLEDevice::startAdvertising();
  }
};

class RxCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    parseTime(String(c->getValue().c_str()));
  }
};

// ─────────────────────────────────────────────────
// Gambar jam — hanya update area yang berubah
// ─────────────────────────────────────────────────
void drawClock() {
  // Hitung waktu saat ini
  unsigned long elapsed = (millis() - syncMillis) / 1000UL;
  long total = (long)(baseHour * 3600 + baseMin * 60 + baseSec) + elapsed;
  total %= 86400;
  if (total < 0) total += 86400;

  int h = total / 3600;
  int m = (total % 3600) / 60;
  int s = total % 60;

  if (s == lastDrawnSec) return;   // ← kunci anti-flicker: skip jika belum berubah
  lastDrawnSec = s;

  // Hapus HANYA area jam (bukan seluruh layar)
  display.fillRect(0, 16, 128, 34, BLACK);

  // Jam besar HH:MM:SS
  char buf[9];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
  display.setTextSize(2);
  display.setTextColor(WHITE);
  display.setCursor(4, 22);
  display.print(buf);

  // Status bar bawah — update hanya jika koneksi berubah
  if (bleConnected != lastConnState) {
    lastConnState = bleConnected;
    display.fillRect(0, 54, 128, 10, BLACK);
    display.setTextSize(1);
    display.setCursor(2, 55);
    display.print(bleConnected ? "BLE: Terhubung     " : "BLE: Mencari...    ");
  }

  display.display();   // kirim buffer ke OLED sekali saja per detik
}

// ─────────────────────────────────────────────────
// Tampilan awal (header statis, hanya sekali)
// ─────────────────────────────────────────────────
void drawStaticUI() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(22, 2);
  display.print("ESP32-C3 BLE JAM");
  display.drawLine(0, 13, 128, 13, WHITE);
  display.drawLine(0, 52, 128, 52, WHITE);
  display.setCursor(2, 55);
  display.print("BLE: Mencari...");
  display.display();
}

// ─────────────────────────────────────────────────
void setup() {
  // OLED
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { for(;;); }
  drawStaticUI();

  // BLE init
  BLEDevice::init("ESP32C3-Jam");
  BLEServer  *srv = BLEDevice::createServer();
  srv->setCallbacks(new ServerCB());

  BLEService *svc = srv->createService(SERVICE_UUID);

  // TX (notify ke HP)
  pTxChar = svc->createCharacteristic(CHAR_UUID_TX,
              BLECharacteristic::PROPERTY_NOTIFY);
  pTxChar->addDescriptor(new BLE2902());

  // RX (terima waktu dari HP)
  BLECharacteristic *rxChar = svc->createCharacteristic(CHAR_UUID_RX,
              BLECharacteristic::PROPERTY_WRITE |
              BLECharacteristic::PROPERTY_WRITE_NR);
  rxChar->setCallbacks(new RxCB());

  svc->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  BLEDevice::startAdvertising();

  // Tampil "Kirim waktu via BLE"
  display.setTextSize(1);
  display.setCursor(4, 30);
  display.print("Kirim: HH:MM:SS");
  display.display();
}

void loop() {
  if (timeValid) {
    drawClock();
  }
  delay(100);  // 100ms cukup, update visual hanya 1x/detik
}
