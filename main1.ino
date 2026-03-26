#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ── PIN ───────────────────────────────────────────
#define OLED_SDA      8
#define OLED_SCL      9
#define VIBRO_PIN     2   // pin motor vibration (via transistor/MOSFET)

// ── OLED ──────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ── BLE ───────────────────────────────────────────
#define SERVICE_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_UUID_RX  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_UUID_TX  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

BLECharacteristic *pTxChar;
bool bleConnected  = false;
bool lastConnState = false;

// ── State Waktu ───────────────────────────────────
int  baseH = 0, baseM = 0, baseS = 0;
bool timeValid   = false;
unsigned long syncMillis = 0;
int  lastSec     = -1;

// ── State Alarm ───────────────────────────────────
int  alarmH = -1, alarmM = -1;
bool alarmEnabled  = false;
bool alarmRinging  = false;
unsigned long alarmStartMs = 0;

// Pola getar: ON x ms, OFF x ms, ulang N kali
#define VIBRO_ON_MS   400
#define VIBRO_OFF_MS  300
#define VIBRO_REPEAT   10   // 10 pulsa lalu berhenti otomatis

int  vibroPulse    = 0;
bool vibroState    = false;
unsigned long vibroLastMs = 0;

// ─────────────────────────────────────────────────
// Hitung waktu saat ini dari base + millis
// ─────────────────────────────────────────────────
void getCurrentTime(int &h, int &m, int &s) {
  unsigned long elapsed = (millis() - syncMillis) / 1000UL;
  long total = (long)(baseH * 3600 + baseM * 60 + baseS) + elapsed;
  total %= 86400;
  h = total / 3600;
  m = (total % 3600) / 60;
  s = total % 60;
}

// ─────────────────────────────────────────────────
// Kontrol motor vibration (non-blocking)
// ─────────────────────────────────────────────────
void startVibration() {
  alarmRinging  = true;
  alarmStartMs  = millis();
  vibroPulse    = 0;
  vibroState    = false;
  vibroLastMs   = millis();
  digitalWrite(VIBRO_PIN, HIGH);
  vibroState = true;
}

void stopVibration() {
  alarmRinging = false;
  vibroPulse   = 0;
  vibroState   = false;
  digitalWrite(VIBRO_PIN, LOW);
}

void handleVibration() {
  if (!alarmRinging) return;

  unsigned long now = millis();
  unsigned long interval = vibroState ? VIBRO_ON_MS : VIBRO_OFF_MS;

  if (now - vibroLastMs >= interval) {
    vibroLastMs = now;
    vibroState  = !vibroState;
    digitalWrite(VIBRO_PIN, vibroState ? HIGH : LOW);

    if (!vibroState) {            // selesai 1 pulsa ON+OFF
      vibroPulse++;
      if (vibroPulse >= VIBRO_REPEAT) {
        stopVibration();           // berhenti otomatis
      }
    }
  }
}

// ─────────────────────────────────────────────────
// Cek alarm — dipanggil tiap detik
// ─────────────────────────────────────────────────
void checkAlarm(int h, int m, int s) {
  if (!alarmEnabled || alarmRinging) return;
  if (h == alarmH && m == alarmM && s == 0) {
    startVibration();
    // Kirim notif ke HP
    if (bleConnected) {
      char msg[32];
      snprintf(msg, sizeof(msg), "ALARM:%02d:%02d BERBUNYI!", alarmH, alarmM);
      pTxChar->setValue((uint8_t*)msg, strlen(msg));
      pTxChar->notify();
    }
  }
}

// ─────────────────────────────────────────────────
// Parse perintah BLE
//   T:HH:MM:SS  → set waktu
//   A:HH:MM     → set alarm
//   X           → hapus alarm
// ─────────────────────────────────────────────────
void handleCommand(const String &cmd) {
  if (cmd.startsWith("T:")) {
    int h, m, s;
    if (sscanf(cmd.c_str() + 2, "%d:%d:%d", &h, &m, &s) == 3 &&
        h >= 0 && h < 24 && m >= 0 && m < 60 && s >= 0 && s < 60) {
      baseH = h; baseM = m; baseS = s;
      syncMillis = millis();
      timeValid  = true;
      lastSec    = -1;
      drawStaticUI();
    }
  }
  else if (cmd.startsWith("A:")) {
    int h, m;
    if (sscanf(cmd.c_str() + 2, "%d:%d", &h, &m) == 2 &&
        h >= 0 && h < 24 && m >= 0 && m < 60) {
      alarmH = h; alarmM = m;
      alarmEnabled = true;
      stopVibration();
      lastSec = -1;  // paksa redraw
    }
  }
  else if (cmd == "X" || cmd == "x") {
    alarmEnabled = false;
    alarmH = alarmM = -1;
    stopVibration();
    lastSec = -1;
  }
}

// ─────────────────────────────────────────────────
// BLE Callbacks
// ─────────────────────────────────────────────────
class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer*)    override { bleConnected = true; }
  void onDisconnect(BLEServer*) override {
    bleConnected = false;
    BLEDevice::startAdvertising();
  }
};

class RxCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    handleCommand(String(c->getValue().c_str()));
  }
};

// ─────────────────────────────────────────────────
// Gambar UI statis (header + garis)
// ─────────────────────────────────────────────────
void drawStaticUI() {
  display.clearDisplay();
  display.setTextColor(WHITE);

  // Header
  display.setTextSize(1);
  display.setCursor(22, 1);
  display.print("ESP32-C3 BLE JAM");
  display.drawLine(0, 11, 128, 11, WHITE);

  // Garis pemisah bawah
  display.drawLine(0, 52, 128, 52, WHITE);

  display.display();
  lastSec = -1;
}

// ─────────────────────────────────────────────────
// Update OLED — hanya area yang berubah
// ─────────────────────────────────────────────────
void drawClock() {
  int h, m, s;
  getCurrentTime(h, m, s);

  if (s == lastSec) return;
  lastSec = s;

  checkAlarm(h, m, s);

  // ── Area jam (row 14–38) ──
  display.fillRect(0, 13, 128, 27, BLACK);
  char timeBuf[9];
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", h, m, s);
  display.setTextSize(2);
  display.setCursor(4, 18);

  // Jam berkedip saat alarm berbunyi
  if (!alarmRinging || (millis() / 500) % 2 == 0) {
    display.print(timeBuf);
  }

  // ── Area alarm (row 40–51) ──
  display.fillRect(0, 40, 128, 12, BLACK);
  display.setTextSize(1);
  display.setCursor(2, 41);
  if (alarmEnabled) {
    char alarmBuf[24];
    if (alarmRinging) {
      snprintf(alarmBuf, sizeof(alarmBuf), "ALARM %02d:%02d  [BUNYI!]", alarmH, alarmM);
    } else {
      snprintf(alarmBuf, sizeof(alarmBuf), "Alarm: %02d:%02d  [ON] ", alarmH, alarmM);
    }
    display.print(alarmBuf);
  } else {
    display.print("Alarm: --:--  [OFF]");
  }

  // ── Status bar (row 53–63) ──
  if (bleConnected != lastConnState) {
    lastConnState = bleConnected;
    display.fillRect(0, 54, 128, 10, BLACK);
    display.setTextSize(1);
    display.setCursor(2, 55);
    display.print(bleConnected ? "BLE: Terhubung      " : "BLE: Mencari...     ");
  }

  display.display();
}

// ─────────────────────────────────────────────────
void setup() {
  pinMode(VIBRO_PIN, OUTPUT);
  digitalWrite(VIBRO_PIN, LOW);

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { for (;;); }

  drawStaticUI();
  display.setTextSize(1);
  display.setCursor(10, 28);
  display.print("Kirim T:HH:MM:SS");
  display.setCursor(14, 40);
  display.print("via BLE Chrome");
  display.display();

  // BLE
  BLEDevice::init("ESP32C3-Jam");
  BLEServer  *srv = BLEDevice::createServer();
  srv->setCallbacks(new ServerCB());
  BLEService *svc = srv->createService(SERVICE_UUID);

  pTxChar = svc->createCharacteristic(CHAR_UUID_TX,
              BLECharacteristic::PROPERTY_NOTIFY);
  pTxChar->addDescriptor(new BLE2902());

  BLECharacteristic *rx = svc->createCharacteristic(CHAR_UUID_RX,
              BLECharacteristic::PROPERTY_WRITE |
              BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RxCB());

  svc->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
}

void loop() {
  handleVibration();   // non-blocking, harus dipanggil sesering mungkin
  if (timeValid) drawClock();
  delay(50);
}
