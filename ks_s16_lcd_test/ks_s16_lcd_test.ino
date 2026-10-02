/*
  Teste KingSong KS-S16 -> ESP32-C3 Supermini -> LCD 16x2 I2C (PCF8574)

  Bibliotecas:
    - NimBLE-Arduino (h2zero) versao 2.x
    - LiquidCrystal_I2C (Frank de Brabander)
  Placa na Arduino IDE: "ESP32C3 Dev Module", com "USB CDC On Boot: Enabled"
  (sem isso o Serial pela USB-C nao aparece).

  Protocolo baseado no KingsongAdapter do WheelLog. Os offsets abaixo sao
  de memoria: valide com o dump hex no Serial Monitor.
*/

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ================= Configuracao =================
static const char* NAME_FILTER = "KS";   // trecho do nome anunciado pela roda
static const char* TARGET_MAC  = "";     // opcional: "aa:bb:cc:dd:ee:ff" trava numa roda especifica
static const uint8_t LCD_ADDR  = 0x27;   // alguns modulos usam 0x3F
static const int I2C_SDA = 8;            // padrao da C3 Supermini
static const int I2C_SCL = 9;
static const bool DUMP_HEX = true;       // imprime os frames crus no Serial

static NimBLEUUID SVC_UUID("FFE0");
static NimBLEUUID CHR_UUID("FFE1");

LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

static const NimBLEAdvertisedDevice* target = nullptr;
static NimBLEClient* client = nullptr;
static NimBLERemoteCharacteristic* chr = nullptr;
static volatile bool doConnect = false;
static volatile bool needScan  = false;

struct WheelData {
  float voltage = 0, speed = 0, current = 0, temp = 0;
  int pwm = -1;                 // -1 = ainda nao recebeu pacote F5
  uint32_t lastUpdate = 0;
} wd;

// ================= Utilitarios =================
static uint16_t u16(const uint8_t* d, int i) { return d[i] | (d[i + 1] << 8); }  // little-endian
static int16_t  s16(const uint8_t* d, int i) { return (int16_t)u16(d, i); }

static void lcdLine(uint8_t row, const char* txt) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16s", txt);  // completa com espacos, evita lcd.clear() e flicker
  lcd.setCursor(0, row);
  lcd.print(buf);
}

static void lcdMsg(const char* l1, const char* l2) { lcdLine(0, l1); lcdLine(1, l2); }

// ================= Protocolo KingSong =================
// Frame de 20 bytes: AA 55 [dados...] [16]=tipo [17]=0x14 [18]=5A [19]=5A
static void parseFrame(const uint8_t* d, size_t len) {
  if (len < 20 || d[0] != 0xAA || d[1] != 0x55) return;

  switch (d[16]) {
    case 0xA9:  // dados ao vivo
      wd.voltage = u16(d, 2) / 100.0f;   // V
      wd.speed   = u16(d, 4) / 100.0f;   // km/h
      wd.current = s16(d, 10) / 100.0f;  // A
      wd.temp    = u16(d, 12) / 100.0f;  // C
      wd.lastUpdate = millis();
      break;

    case 0xF5:  // carga de CPU + saida (PWM) nos modelos novos
      wd.pwm = d[15];  // hipotese: confirmar no dump / WheelLog
      break;
  }
}

static void sendCmd(uint8_t cmd) {
  if (!chr) return;
  uint8_t p[20] = {0xAA, 0x55};
  p[16] = cmd; p[17] = 0x14; p[18] = 0x5A; p[19] = 0x5A;
  chr->writeValue(p, sizeof(p), false);
}

static void notifyCB(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (DUMP_HEX) {
    for (size_t i = 0; i < len; i++) Serial.printf("%02X ", data[i]);
    Serial.println();
  }
  parseFrame(data, len);
}

// ================= BLE: scan e conexao =================
class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (target) return;
    std::string name = dev->getName();
    if (!name.empty())
      Serial.printf("Visto: %s  \"%s\"  RSSI %d\n",
                    dev->getAddress().toString().c_str(), name.c_str(), dev->getRSSI());

    bool match = strlen(TARGET_MAC)
                   ? dev->getAddress().toString() == TARGET_MAC
                   : (!name.empty() && name.find(NAME_FILTER) != std::string::npos);
    if (match) {
      NimBLEDevice::getScan()->stop();
      target = dev;
      doConnect = true;
    }
  }
} scanCB;

class ClientCB : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient*, int reason) override {
    Serial.printf("Desconectado (motivo %d)\n", reason);
    chr = nullptr;
    needScan = true;
  }
} clientCB;

static bool connectToWheel() {
  if (!client) {
    client = NimBLEDevice::createClient();
    client->setClientCallbacks(&clientCB, false);
  }
  if (!client->connect(target)) return false;

  NimBLERemoteService* svc = client->getService(SVC_UUID);
  if (!svc) { Serial.println("Servico FFE0 nao encontrado"); client->disconnect(); return false; }

  chr = svc->getCharacteristic(CHR_UUID);
  if (!chr || !chr->canNotify() || !chr->subscribe(true, notifyCB)) {
    Serial.println("Falha ao assinar FFE1");
    client->disconnect();
    return false;
  }

  // Alguns KingSong so comecam a transmitir depois de pedidos iniciais
  sendCmd(0x9B);  // pede nome/modelo
  delay(100);
  sendCmd(0x63);  // pede serial
  return true;
}

static void startScan() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->clearResults();
  target = nullptr;
  scan->start(0, false, true);  // 0 = escaneia ate achar
}

// ================= Tela =================
static void drawData() {
  if (millis() - wd.lastUpdate > 2000) { lcdMsg("Conectado", "Sem dados..."); return; }

  char pwm[5], l1[24], l2[24];
  if (wd.pwm >= 0) snprintf(pwm, sizeof(pwm), "%3d", wd.pwm);
  else strcpy(pwm, " --");

  snprintf(l1, sizeof(l1), "%5.1fkm/h P%s%%", wd.speed, pwm);             // " 25.3km/h P 45%"
  snprintf(l2, sizeof(l2), "%4.1fV %5.1fA %2.0fC", wd.voltage, wd.current, wd.temp);  // "67.2V  12.3A 38C"
  lcdLine(0, l1);
  lcdLine(1, l2);
}

// ================= Setup / Loop =================
void setup() {
  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.init();
  lcd.backlight();
  lcdMsg("KS-S16 teste", "Escaneando...");

  NimBLEDevice::init("");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCB, false);
  scan->setActiveScan(true);   // necessario para receber o nome no scan response
  scan->setInterval(100);
  scan->setWindow(99);
  startScan();
}

void loop() {
  if (doConnect) {
    doConnect = false;
    lcdMsg("Conectando...", target->getName().c_str());
    if (connectToWheel()) {
      Serial.println("Conectado e inscrito em FFE1");
      lcdMsg("Conectado!", "Aguardando...");
    } else {
      lcdMsg("Falha conexao", "Reescaneando...");
      needScan = true;
    }
  }

  if (needScan) {
    needScan = false;
    lcdMsg("KS-S16 teste", "Escaneando...");
    startScan();
  }

  static uint32_t lastDraw = 0;
  if (client && client->isConnected() && millis() - lastDraw > 250) {
    lastDraw = millis();
    drawData();
  }
}
