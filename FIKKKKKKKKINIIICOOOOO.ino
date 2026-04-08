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
#define VIBRO_PIN     2
#define BTN_PIN       3    // Push button, sisi lain ke GND

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
int  alarmH = 7, alarmM = 0;
bool alarmEnabled  = false;
bool alarmRinging  = false;

// ── Vibration ─────────────────────────────────────
#define VIBRO_ON_MS   400
#define VIBRO_OFF_MS  300
#define VIBRO_DURATION_MS  60000UL   // ← 1 menit

//int  vibroPulse = 0;
bool vibroState = false;
unsigned long vibroLastMs = 0;
unsigned long vibroStartMs = 0;

// ── Mode UI ───────────────────────────────────────
enum Mode { MODE_NORMAL, MODE_SET_HOUR, MODE_SET_MINUTE };
Mode currentMode = MODE_NORMAL;

// Nilai sementara saat setting
int  editH = 7, editM = 0;

// Timeout otomatis keluar mode setting (10 detik tidak ada aksi)
unsigned long modeEnteredMs = 0;
#define MODE_TIMEOUT_MS  10000

// ── Tombol ────────────────────────────────────────
#define DEBOUNCE_MS     50
#define LONG_PRESS_MS  2000

bool     btnLastRaw    = HIGH;
bool     btnState      = HIGH;
unsigned long btnPressMs   = 0;
unsigned long debounceMs   = 0;
bool     btnWaitRelease = false;   // tunggu lepas setelah long press
bool     longFired      = false;   // sudah trigger long press?

// ─────────────────────────────────────────────────
// Hitung waktu saat ini
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
// Vibration (non-blocking)
// ─────────────────────────────────────────────────
void startVibration() {
  alarmRinging  = true;
  vibroState    = true;
  vibroLastMs   = millis();
  vibroStartMs  = millis();
  digitalWrite(VIBRO_PIN, HIGH);
}

void stopVibration() {
  alarmRinging = false;
  vibroState   = false;
  digitalWrite(VIBRO_PIN, LOW);
}

void handleVibration() {
  if (!alarmRinging) return;
  unsigned long now = millis();

  // Berhenti otomatis setelah 1 menit
  if (now - vibroStartMs >= VIBRO_DURATION_MS) {
    stopVibration();
    return;
  }                // ← kurung tutup yang hilang

  unsigned long interval = vibroState ? VIBRO_ON_MS : VIBRO_OFF_MS;
  if (now - vibroLastMs < interval) return;

  vibroLastMs = now;
  vibroState  = !vibroState;
  digitalWrite(VIBRO_PIN, vibroState ? HIGH : LOW);
}

// ─────────────────────────────────────────────────
// Cek alarm
// ─────────────────────────────────────────────────
void checkAlarm(int h, int m, int s) {
  if (!alarmEnabled || alarmRinging) return;
  if (h == alarmH && m == alarmM && s == 0) {
    startVibration();
    if (bleConnected) {
      char msg[32];
      snprintf(msg, sizeof(msg), "ALARM:%02d:%02d BUNYI!", alarmH, alarmM);
      pTxChar->setValue((uint8_t*)msg, strlen(msg));
      pTxChar->notify();
    }
  }
}

// ─────────────────────────────────────────────────
// OLED — gambar semua
// ─────────────────────────────────────────────────
void drawStaticUI() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(22, 1);
  display.print("VIBRACLOCK");
  display.drawLine(0, 11, 128, 11, WHITE);
  display.drawLine(0, 52, 128, 52, WHITE);
  display.display();
  lastSec = -1;
}

void drawClock() {
  int h, m, s;
  getCurrentTime(h, m, s);
  if (s == lastSec && currentMode == MODE_NORMAL) return;
  lastSec = s;
  checkAlarm(h, m, s);

  // ── Area jam ──
  display.fillRect(0, 13, 128, 27, BLACK);
  char buf[9];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
  display.setTextSize(2);
  display.setCursor(4, 18);
  if (!alarmRinging || (millis() / 500) % 2 == 0) display.print(buf);

  // ── Area alarm / mode setting ──
  display.fillRect(0, 40, 128, 12, BLACK);
  display.setTextSize(1);
  display.setCursor(2, 41);

  if (currentMode == MODE_SET_HOUR) {
    char line[24];
    snprintf(line, sizeof(line), "SET JAM : [%02d]:??", editH);
    // Kedip angka yang sedang diedit
    if ((millis() / 400) % 2 == 0) display.print(line);
    else {
      display.print("SET JAM : [  ]:??");
    }
  }
  else if (currentMode == MODE_SET_MINUTE) {
    char line[24];
    snprintf(line, sizeof(line), "SET MENIT: %02d:[%02d]", editH, editM);
    if ((millis() / 400) % 2 == 0) display.print(line);
    else display.print("SET MENIT: ??:[  ]");
  }
  else {
    if (alarmEnabled) {
      char line[24];
      if (alarmRinging)
        snprintf(line, sizeof(line), "ALARM %02d:%02d [BUNYI!]", alarmH, alarmM);
      else
        snprintf(line, sizeof(line), "Alarm: %02d:%02d  [ON] ", alarmH, alarmM);
      display.print(line);
    } else {
      display.print("Alarm: --:--  [OFF]");
    }
  }

  // ── Hint tombol (row 41 bawah) ──
  display.fillRect(0, 40, 128, 0, BLACK);  // tidak hapus, hanya hint
  // Tampilkan hint kecil di pojok kanan bawah area alarm
  display.setTextSize(1);
  if (currentMode != MODE_NORMAL) {
    // Tampilkan hint di baris status
    display.fillRect(0, 54, 128, 10, BLACK);
    display.setCursor(2, 55);
    display.print(currentMode == MODE_SET_HOUR
      ? "Pendek:+Jam Tahan:OK"
      : "Pendek:+Mnt Tahan:OK");
  } else if (bleConnected != lastConnState) {
    lastConnState = bleConnected;
    display.fillRect(0, 54, 128, 10, BLACK);
    display.setCursor(2, 55);
    display.print(bleConnected ? "BLE: Terhubung      " : "BLE: Mencari...     ");
  }

  display.display();
}

// ─────────────────────────────────────────────────
// Aksi tombol
// ─────────────────────────────────────────────────
void onShortPress() {
  // Saat alarm bunyi → matikan
  if (alarmRinging) {
    stopVibration();
    lastSec = -1;
    return;
  }

  if (currentMode == MODE_SET_HOUR) {
    editH = (editH + 1) % 24;
    modeEnteredMs = millis();   // reset timeout
    lastSec = -1;
  }
  else if (currentMode == MODE_SET_MINUTE) {
    editM = (editM + 1) % 60;   // +5 menit per tekan, lebih praktis
    modeEnteredMs = millis();
    lastSec = -1;
  }
  // MODE_NORMAL: short press tidak melakukan apa-apa
  // (bisa kamu ubah misal untuk toggle alarm on/off)
}

void onLongPress() {
  if (alarmRinging) {
    stopVibration();
    lastSec = -1;
    return;
  }

  if (currentMode == MODE_NORMAL) {
    // Masuk mode setting jam
    int h, m, s;
    getCurrentTime(h, m, s);
    editH = alarmEnabled ? alarmH : h;
    editM = alarmEnabled ? alarmM : m;
    currentMode   = MODE_SET_HOUR;
    modeEnteredMs = millis();
    lastSec       = -1;
    drawStaticUI();
  }
  else if (currentMode == MODE_SET_HOUR) {
    // Jam dikonfirmasi, lanjut ke set menit
    currentMode   = MODE_SET_MINUTE;
    modeEnteredMs = millis();
    lastSec       = -1;
  }
  else if (currentMode == MODE_SET_MINUTE) {
    // Simpan alarm
    alarmH       = editH;
    alarmM       = editM;
    alarmEnabled = true;
    currentMode  = MODE_NORMAL;
    lastSec      = -1;
    drawStaticUI();

    // Kirim ke HP via BLE
    if (bleConnected) {
      char msg[24];
      snprintf(msg, sizeof(msg), "ALARM_SET:%02d:%02d", alarmH, alarmM);
      pTxChar->setValue((uint8_t*)msg, strlen(msg));
      pTxChar->notify();
    }
  }
}

// ─────────────────────────────────────────────────
// Baca tombol (debounce + short/long press)
// ─────────────────────────────────────────────────
void handleButton() {
  bool raw = digitalRead(BTN_PIN);
  unsigned long now = millis();

  // Debounce
  if (raw != btnLastRaw) {
    debounceMs = now;
    btnLastRaw = raw;
  }
  if (now - debounceMs < DEBOUNCE_MS) return;

  if (raw == LOW && btnState == HIGH) {
    // Tombol baru ditekan
    btnState    = LOW;
    btnPressMs  = now;
    longFired   = false;
    btnWaitRelease = false;
  }

  if (btnState == LOW) {
    // Cek long press (trigger saat masih ditekan)
    if (!longFired && (now - btnPressMs >= LONG_PRESS_MS)) {
      longFired      = true;
      btnWaitRelease = true;
      onLongPress();
    }
  }

  if (raw == HIGH && btnState == LOW) {
    // Tombol dilepas
    btnState = HIGH;
    if (!btnWaitRelease) {
      // Lepas sebelum long press → short press
      onShortPress();
    }
    btnWaitRelease = false;
    longFired      = false;
  }
}

// ─────────────────────────────────────────────────
// Timeout mode setting
// ─────────────────────────────────────────────────
void checkModeTimeout() {
  if (currentMode == MODE_NORMAL) return;
  if (millis() - modeEnteredMs >= MODE_TIMEOUT_MS) {
    currentMode = MODE_NORMAL;
    lastSec     = -1;
    drawStaticUI();
  }
}

// ─────────────────────────────────────────────────
// BLE Command Handler
// ─────────────────────────────────────────────────
void handleCommand(const String &cmd) {
  if (cmd.startsWith("T:")) {
    int h, m, s;
    if (sscanf(cmd.c_str() + 2, "%d:%d:%d", &h, &m, &s) == 3) {
      baseH = h; baseM = m; baseS = s;
      syncMillis = millis(); timeValid = true; lastSec = -1;
      drawStaticUI();
    }
  }
  else if (cmd.startsWith("A:")) {
    int h, m;
    if (sscanf(cmd.c_str() + 2, "%d:%d", &h, &m) == 2) {
      alarmH = h; alarmM = m; alarmEnabled = true;
      stopVibration(); lastSec = -1;
    }
  }
  else if (cmd == "X" || cmd == "x") {
    alarmEnabled = false; stopVibration(); lastSec = -1;
  }
}

// ── BLE Callbacks ─────────────────────────────────
class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer*)    override { bleConnected = true; }
  void onDisconnect(BLEServer*) override {
    bleConnected = false; BLEDevice::startAdvertising();
  }
};
class RxCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    handleCommand(String(c->getValue().c_str()));
  }
};

// ─────────────────────────────────────────────────
void setup() {
  pinMode(VIBRO_PIN, OUTPUT);
  digitalWrite(VIBRO_PIN, LOW);
  pinMode(BTN_PIN, INPUT_PULLUP);   // pakai pull-up internal

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { for (;;); }
  drawStaticUI();
  display.setTextSize(1);
  display.setCursor(10, 28);
  display.print("Kirim T:HH:MM:SS");
  display.setCursor(14, 40);
  display.print("via BLE / Tombol");
  display.display();

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
  handleButton();
  handleVibration();
  checkModeTimeout();
  if (timeValid) drawClock();
  delay(50);
}
