/*
  Ponte BLE KingSong KS-S16 <-> ESP32-S3 (ES3C28P 2.8") <-> Celular  (v4: 3 telas, buzina no BOOT, logs no SD, midia HID em identidade propria)

  - Conecta na roda, descobre TODOS os servicos/caracteristicas.
  - Recria todos eles no servidor do ESP32 (exceto 0x1800/0x1801, que a pilha ja tem),
    com as mesmas propriedades. Valores legiveis (ex.: Device Information 0x180A)
    sao copiados.
  - Anuncia com o mesmo nome, UUIDs e dados de fabricante da roda.
  - Repassa CRU: notificacoes de qualquer caracteristica -> celular;
    escritas do celular em qualquer caracteristica -> roda.
  - FFE1 e sempre assinada (para a tela); as demais so quando o celular assina.

  Bibliotecas: NimBLE-Arduino 2.x (com o patch do GAP/GATT), LovyanGFX (lovyan03)
  Placa: "ESP32S3 Dev Module"
         USB CDC On Boot: Enabled | USB Mode: Hardware CDC and JTAG
         Flash Size: 16MB | PSRAM: OPI PSRAM | JTAG Adapter: Disabled
         Partition Scheme: "16M Flash (3MB APP/9.9MB FATFS)" (ou qualquer uma de 16MB)
*/

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <SD_MMC.h>          // antes da LovyanGFX
#include <sys/time.h>
#include <esp_mac.h>          // MAC do chip, para gerar o endereco do "S16 Controle"
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <vector>

// ================= Configuracao =================
static const char* NAME_FILTER = "KS";
static const char* TARGET_MAC  = "";       // ex.: "c5:39:32:38:4c:4b"
static const bool DUMP_HEX  = false;       // frames da FFE1 vindos da roda (R>) - muito volume
static const bool DUMP_PHONE = true;       // tudo que o CELULAR faz: leituras, escritas, assinaturas
static const bool DUMP_NOVOS = true;       // frames da roda de tipos que NAO aparecem no fluxo normal (R?)
// Servico "estranho" de teste na GATT (ja provou que o EUC World tolera servicos extras)
static const bool TESTE_SERVICO_EXTRA = false;
// EXPERIMENTO: o display tambem se apresenta como CONTROLE DE MIDIA Bluetooth (HID).
// Os botoes anterior / play-pause / proxima vao direto para o celular (Spotify etc.).
// Exige parear UMA VEZ nas configuracoes de Bluetooth do Android. false = como antes.
static const bool MIDIA_HID = true;
static const bool DUMP_OTHER = true;       // trafego nas OUTRAS caracteristicas (raro, interessante)

static NimBLEUUID MAIN_SVC("FFE0");
static NimBLEUUID MAIN_CHR("FFE1");

// ================= Tela ES3C28P (pinos do LCDWIKI) =================
// ILI9341V via SPI: CS=10 DC=46 SCK=12 MOSI=11 MISO=13, backlight=45, RST compartilhado com o EN
// Se as cores sairem "negativas", troque PANEL_INVERT. Se vermelho/azul trocados, troque PANEL_RGB_ORDER.
static const bool PANEL_INVERT    = true;   // o init do fabricante liga a inversao (cmd 0x21)
static const bool PANEL_RGB_ORDER = false;  // painel BGR (padrao do ILI9341)

// O touch desta placa vem girado 180 graus em relacao a imagem (vimos na galeria)
static const bool INVERTER_TOUCH_X = true;
static const bool INVERTER_TOUCH_Y = true;

class LGFX : public lgfx::LGFX_Device {
  lgfx::Touch_FT5x06 _touch;
  lgfx::Panel_ILI9341 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 12;
      cfg.pin_mosi = 11;
      cfg.pin_miso = 13;
      cfg.pin_dc = 46;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = 10;
      cfg.pin_rst = -1;
      cfg.pin_busy = -1;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.readable = true;
      cfg.invert = PANEL_INVERT;
      cfg.rgb_order = PANEL_RGB_ORDER;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = 45;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    {
      auto cfg = _touch.config();
      cfg.i2c_port = 0;
      cfg.i2c_addr = 0x38;
      cfg.pin_sda = 16;
      cfg.pin_scl = 15;
      cfg.pin_int = 17;
      cfg.pin_rst = 18;
      cfg.freq = 400000;
      if (INVERTER_TOUCH_X) { cfg.x_min = 239; cfg.x_max = 0; } else { cfg.x_min = 0; cfg.x_max = 239; }
      if (INVERTER_TOUCH_Y) { cfg.y_min = 319; cfg.y_max = 0; } else { cfg.y_min = 0; cfg.y_max = 319; }
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

static LGFX tft;
static LGFX_Sprite spr(&tft);   // quadro inteiro na PSRAM -> sem piscar

// ---- mapeamento remoto <-> local ----
struct Map {
  NimBLEUUID svcUuid;
  NimBLEUUID chrUuid;
  NimBLERemoteCharacteristic* remote = nullptr;
  NimBLECharacteristic* local = nullptr;
  bool remoteWriteNR = false;
  bool isMain = false;
  bool phoneSub = false;      // algum celular assinou a caracteristica local
  bool remoteSub = false;     // ESP32 assinou a remota
  // Quais conexoes (celulares) assinaram esta caracteristica. Enviamos so para elas,
  // uma a uma: mandar "para todos" deixava a NimBLE tentar notificar um celular que
  // estava no meio da desconexao -> ponteiro nulo -> reinicio.
  uint16_t subs[4] = {BLE_HS_CONN_HANDLE_NONE, BLE_HS_CONN_HANDLE_NONE,
                      BLE_HS_CONN_HANDLE_NONE, BLE_HS_CONN_HANDLE_NONE};
};

static void subAdd(Map& m, uint16_t h) {
  for (auto& s : m.subs) if (s == h) return;
  for (auto& s : m.subs) if (s == BLE_HS_CONN_HANDLE_NONE) { s = h; break; }
  m.phoneSub = true;
}
static void subRemove(Map& m, uint16_t h) {
  bool any = false;
  for (auto& s : m.subs) { if (s == h) s = BLE_HS_CONN_HANDLE_NONE; if (s != BLE_HS_CONN_HANDLE_NONE) any = true; }
  m.phoneSub = any;
}
static std::vector<Map> maps;

// ---- lado roda ----
static const NimBLEAdvertisedDevice* target = nullptr;
static NimBLEClient* client = nullptr;
static NimBLERemoteCharacteristic* mainRemote = nullptr;
static volatile bool doConnect = false;
static volatile bool needScan  = false;
static std::string wheelAdvName;
static std::string wheelMfgData;
static std::vector<NimBLEUUID> wheelAdvUuids;

// ---- lado celular ----
static NimBLEServer* server = nullptr;
static bool serverStarted = false;
// Aceita mais de um celular ao mesmo tempo (e continua anunciando enquanto houver vaga).
// Assim, se o Android "segurar" uma conexao velha, um app novo ainda consegue entrar.
// ================= Duas identidades Bluetooth =================
// O ESP32-S3 anuncia DOIS "aparelhos" ao mesmo tempo (extended advertising, 2 instancias):
//   ADV_RODA: "KSN-S16P--..." com o endereco normal do ESP32, SEM pareamento -> os apps
//             (EUC World, Mono Riders, KingSong), cada um com a sua conexao, como antes.
//   ADV_HID:  "S16 Controle" com um endereco proprio, PAREADO -> so o controle de midia.
// O Android trata cada endereco como um aparelho diferente, entao os apps nao dividem mais
// a conexao com o controle de midia.
// Requer no nimconfig.h:  CONFIG_BT_NIMBLE_EXT_ADV 1, CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES 2
//                         e CONFIG_BT_NIMBLE_MAX_CONNECTIONS 4  (roda + 2 apps + controle)
static const char* NOME_CONTROLE = "S16 Controle";
static const int MAX_APPS = 2;
static const uint8_t ADV_RODA = 0, ADV_HID = 1;
static uint8_t hidAddrVal[6];                       // endereco "random static" do S16 Controle
static volatile int appConns = 0;                   // apps conectados na identidade da roda
static volatile uint16_t hidConnHandle = BLE_HS_CONN_HANDLE_NONE;   // conexao do controle
static volatile bool advDirty = true;               // pede para o loop rever o anuncio
static bool advConfigured = false;
static volatile int phoneCount = 0;
static volatile bool phoneConnected = false;
static volatile uint16_t phoneMtu = 247;  // menor MTU entre os celulares conectados
static uint32_t phoneConnT = 0;

// ---- fila de escritas que precisam de resposta (nao podem ser feitas no callback) ----
struct PendingWrite { uint8_t idx; uint16_t len; uint8_t data[244]; };
static QueueHandle_t writeQ;

// ---- dados para a tela ----
struct WheelData {
  float voltage = 0, speed = 0, current = 0, temp = 0, tripKm = 0, topSpeed = 0;
  float temp2 = 0;        // B9 14-15 (motor? a confirmar com o app da KingSong)
  float speedLimit = 0;   // F6 2-3
  int cooling = -1;       // B9 12 ("Cooling" no app)
  int pwm = -1;
  uint32_t lastUpdate = 0;
} wd;

// BMS: a roda manda sozinha um pacote longo por pack (F1 = pack 1, F2 = pack 2, subtipo D0)
//   [21] numero de celulas | [22..] tensoes (mV, 2 bytes cada)
//   depois: numero de sensores de temperatura, temperaturas (0,1 K), corrente (/100 A),
//   tensao do pack (/100 V) e SoC (/10 %)
struct BmsPack {
  uint16_t cell[24];
  int nCells = 0;
  float temps[8];
  int nTemps = 0;
  float current = 0, voltage = 0, soc = 0;
  uint32_t lastUpdate = 0;
} bms[2];

static char statusL1[32] = "KS-S16 ponte v4";
static char statusL2[32] = "Iniciando...";

// ================= Logs no cartao SD =================
// Pinos do slot (manual do fabricante): CLK=38, CMD=40, D0=39 (modo de 1 linha)
// /logs/eventos.csv  -> sempre: conexoes e desconexoes da roda e dos apps
// /logs/viagem_*.csv -> quando a gravacao e ligada na tela de controles (5 linhas/s)
static bool sdOk = false;
static bool clockSet = false;          // relogio acertado pelo app da KingSong?
static bool rideLogging = false;
static File rideFile;
static char rideFileName[48] = "";
static uint32_t rideLines = 0;

// Eventos podem acontecer dentro dos callbacks do Bluetooth, onde nao e bom
// escrever no cartao (demora). Eles entram numa fila e o loop grava depois.
struct LogEvt { uint32_t ms; char txt[80]; };
static QueueHandle_t logQ;

static void logEvent(const char* fmt, ...) {
  if (!logQ) return;
  LogEvt e;
  e.ms = millis();
  va_list ap; va_start(ap, fmt);
  vsnprintf(e.txt, sizeof(e.txt), fmt, ap);
  va_end(ap);
  xQueueSend(logQ, &e, 0);
}

// "AAAA-MM-DD HH:MM:SS" se o relogio foi acertado; senao, vazio
static void clockStr(char* out, size_t n) {
  if (!clockSet) { out[0] = 0; return; }
  time_t t = time(nullptr);
  struct tm tmv;
  localtime_r(&t, &tmv);
  strftime(out, n, "%Y-%m-%d %H:%M:%S", &tmv);
}

// Comando F9 = relogio da roda. O mesmo formato serve para os dois sentidos:
//   app -> roda (acerta):   AA 55 01 00 [4-5]=ano [6]=mes [7]=dia [8]=hora [9]=min [10]=seg ... F9 01 <crc>
//   roda -> nos (resposta): AA 55 00 00 [4-5]=ano ...                                      F9 01 <crc>
// Ao conectar, o display PERGUNTA a hora para a roda (frame capturado do app, com CRC pronto).
// Se depois o app da KingSong acertar a roda, pegamos carona e acertamos de novo.
static const uint8_t PEDE_HORA[20] = {0xAA, 0x55, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xF9, 0x01, 0xB9, 0x78};

static void setClockFromF9(const uint8_t* d, size_t len, const char* origem) {
  if (len < 20 || d[16] != 0xF9 || d[17] != 0x01) return;
  int year = d[4] | (d[5] << 8);
  if (year < 2024 || year > 2100) return;
  struct tm tmv = {};
  tmv.tm_year = year - 1900; tmv.tm_mon = d[6] - 1; tmv.tm_mday = d[7];
  tmv.tm_hour = d[8]; tmv.tm_min = d[9]; tmv.tm_sec = d[10];
  struct timeval tv = { mktime(&tmv), 0 };
  settimeofday(&tv, nullptr);
  bool primeira = !clockSet;
  clockSet = true;
  if (primeira) {
    char ts[24]; clockStr(ts, sizeof(ts));
    logEvent("relogio acertado pel%s: %s", origem, ts);
  }
}

static void writeEvents() {
  LogEvt e;
  while (logQ && xQueueReceive(logQ, &e, 0) == pdTRUE) {
    Serial.printf("[evento] %s\n", e.txt);
    if (!sdOk) continue;
    File f = SD_MMC.open("/logs/eventos.csv", FILE_APPEND);
    if (!f) continue;
    if (f.size() == 0) f.println("uptime_ms,data_hora,evento");
    char ts[24]; clockStr(ts, sizeof(ts));
    f.printf("%lu,%s,%s\n", (unsigned long)e.ms, ts, e.txt);
    f.close();
  }
}

static void startRideLog() {
  if (!sdOk || rideLogging) return;
  if (clockSet) {
    time_t t = time(nullptr); struct tm tmv; localtime_r(&t, &tmv);
    strftime(rideFileName, sizeof(rideFileName), "/logs/viagem_%Y%m%d_%H%M%S.csv", &tmv);
  } else {
    for (int i = 1; i < 1000; i++) {           // sem relogio: viagem_001, 002...
      snprintf(rideFileName, sizeof(rideFileName), "/logs/viagem_%03d.csv", i);
      if (!SD_MMC.exists(rideFileName)) break;
    }
  }
  rideFile = SD_MMC.open(rideFileName, FILE_WRITE);
  if (!rideFile) { logEvent("erro ao criar %s", rideFileName); return; }
  rideFile.println("uptime_ms,data_hora,velocidade_kmh,pwm_pct,corrente_a,tensao_v");
  rideLines = 0;
  rideLogging = true;
  logEvent("gravacao iniciada: %s", rideFileName);
}

static void stopRideLog() {
  if (!rideLogging) return;
  rideLogging = false;
  rideFile.close();
  logEvent("gravacao parada: %s (%lu linhas)", rideFileName, (unsigned long)rideLines);
}

static void writeRideLine() {
  static uint32_t last = 0, lastFlush = 0;
  if (!rideLogging || millis() - last < 200) return;     // 5 linhas por segundo
  last = millis();
  if (millis() - wd.lastUpdate > 2000) return;           // sem dados novos da roda
  char ts[24]; clockStr(ts, sizeof(ts));
  rideFile.printf("%lu,%s,%.2f,%d,%.2f,%.2f\n", (unsigned long)millis(), ts,
                  wd.speed, wd.pwm, wd.current, wd.voltage);
  rideLines++;
  if (millis() - lastFlush > 5000) {    // grava de fato a cada 5 s (protege contra desligar)
    lastFlush = millis();
    rideFile.flush();
  }
}

// ================= Utilitarios =================
static uint16_t u16(const uint8_t* d, int i) { return d[i] | (d[i + 1] << 8); }
static int16_t  s16(const uint8_t* d, int i) { return (int16_t)u16(d, i); }
static uint32_t u32r(const uint8_t* d, int i) { return ((uint32_t)u16(d, i) << 16) | u16(d, i + 2); }

static void printHex(const char* prefix, const uint8_t* d, size_t len) {
  Serial.print(prefix);
  for (size_t i = 0; i < len; i++) Serial.printf("%02X ", d[i]);
  Serial.println();
}

static void drawStatus();
static void statusMsg(const char* l1, const char* l2) {
  strlcpy(statusL1, l1, sizeof(statusL1));
  strlcpy(statusL2, l2, sizeof(statusL2));
  drawStatus();
}

static Map* findByRemote(NimBLERemoteCharacteristic* rc) {
  for (auto& m : maps) if (m.remote == rc) return &m;
  return nullptr;
}
static int findIdxByLocal(NimBLECharacteristic* lc) {
  for (size_t i = 0; i < maps.size(); i++) if (maps[i].local == lc) return (int)i;
  return -1;
}

// ================= Decodificacao (so para a tela) =================
static void parseBms(const uint8_t* d, size_t len) {
  BmsPack& b = bms[d[16] == 0xF1 ? 0 : 1];
  int n = min((int)d[21], 24);
  size_t base = 22 + 2 * n;                    // onde termina a lista de celulas
  if (len < base + 1) return;
  int nt = min((int)d[base], 8);
  size_t after = base + 1 + 2 * nt;            // depois das temperaturas
  if (len < after + 6) return;
  for (int i = 0; i < n; i++) b.cell[i] = u16(d, 22 + 2 * i);
  b.nCells = n;
  for (int i = 0; i < nt; i++) {
    uint16_t raw = u16(d, base + 1 + 2 * i);
    b.temps[i] = raw ? (raw - 2731) / 10.0f : NAN;   // 0 = sensor ausente
  }
  b.nTemps = nt;
  b.current = s16(d, after) / 100.0f;
  b.voltage = u16(d, after + 2) / 100.0f;
  b.soc     = u16(d, after + 4) / 10.0f;
  b.lastUpdate = millis();
}

static void parseFrame(const uint8_t* d, size_t len) {
  if (len < 20 || d[0] != 0xAA || d[1] != 0x55) return;
  if (len > 20 && (d[16] == 0xF1 || d[16] == 0xF2) && d[17] == 0xD0) { parseBms(d, len); return; }
  if (d[16] == 0xF9 && d[2] == 0x00) { setClockFromF9(d, len, "a roda"); return; }
  switch (d[16]) {
    case 0xA9:
      wd.voltage = u16(d, 2) / 100.0f;
      wd.speed   = u16(d, 4) / 100.0f;
      wd.current = s16(d, 10) / 100.0f;
      wd.temp    = u16(d, 12) / 100.0f;
      wd.lastUpdate = millis();
      break;
    case 0xB9:
      wd.tripKm   = u32r(d, 2) / 1000.0f;
      wd.topSpeed = u16(d, 8) / 100.0f;
      wd.cooling  = d[12];
      wd.temp2    = u16(d, 14) / 100.0f;
      break;
    case 0xF6:
      wd.speedLimit = u16(d, 2) / 100.0f;
      break;
    case 0xF5:
      wd.pwm = d[15];
      break;
  }
}

// ================= Repasse roda -> celular =================
static void forwardToPhone(Map* m, const uint8_t* d, size_t len) {
  if (!phoneConnected || !m->phoneSub || !m->local) return;
  size_t maxLen = phoneMtu > 3 ? phoneMtu - 3 : 20;
  for (uint16_t h : m->subs) {
    if (h == BLE_HS_CONN_HANDLE_NONE) continue;
    for (size_t off = 0; off < len; off += maxLen) {
      size_t n = min(maxLen, len - off);
      m->local->notify(d + off, n, h);   // so para este celular
    }
  }
}

static void remoteNotifyCB(NimBLERemoteCharacteristic* rc, uint8_t* data, size_t len, bool) {
  Map* m = findByRemote(rc);
  if (!m) return;
  if (m->isMain) {
    if (DUMP_HEX) printHex("R> ", data, len);
    else if (DUMP_NOVOS && len >= 20) {
      // o fluxo normal da roda: A9 B9 F5 F6 C9 F1 F2, e BB (nome) / B3 (serie), que sao as
      // respostas ao handshake do proprio display. O resto e resposta a algum comando.
      static const uint8_t comuns[] = {0xA9, 0xB9, 0xF5, 0xF6, 0xC9, 0xF1, 0xF2, 0xBB, 0xB3};
      bool comum = false;
      for (uint8_t t : comuns) if (data[16] == t) comum = true;
      if (!comum) printHex("R? ", data, len);
    }
    parseFrame(data, len);
  } else if (DUMP_OTHER) {
    Serial.printf("R[%s]> ", m->chrUuid.toString().c_str());
    printHex("", data, len);
  }
  forwardToPhone(m, data, len);
}

// ================= Servidor (lado celular) =================
class LocalChrCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    int idx = findIdxByLocal(c);
    if (idx < 0) return;
    Map& m = maps[idx];
    NimBLEAttValue v = c->getValue();
    if (m.isMain) { if (DUMP_PHONE) printHex("C> ", v.data(), v.size()); if (v.size() >= 20 && v.data()[2] == 0x01) setClockFromF9(v.data(), v.size(), "o app"); }
    else if (DUMP_OTHER) {
      Serial.printf("C[%s]> ", m.chrUuid.toString().c_str());
      printHex("", v.data(), v.size());
    }
    if (!m.remote) return;
    if (m.remoteWriteNR) {
      m.remote->writeValue(v.data(), v.size(), false);   // rapido, seguro no callback
    } else {
      static PendingWrite pw;   // static: nao ocupa a pilha (pequena) da tarefa do Bluetooth                                    // com resposta -> faz no loop
      pw.idx = idx;
      pw.len = (uint16_t)std::min((size_t)sizeof(pw.data), (size_t)v.size());
      memcpy(pw.data, v.data(), pw.len);
      xQueueSend(writeQ, &pw, 0);
    }
  }
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    if (!DUMP_PHONE) return;
    int idx = findIdxByLocal(c);
    Serial.printf("Celular leu %s\n", idx >= 0 ? maps[idx].chrUuid.toString().c_str() : "?");
  }
  void onSubscribe(NimBLECharacteristic* c, NimBLEConnInfo& info, uint16_t subValue) override {
    int idx = findIdxByLocal(c);
    if (idx < 0) return;
    if (subValue) subAdd(maps[idx], info.getConnHandle());
    else          subRemove(maps[idx], info.getConnHandle());
    Serial.printf("Celular %s %s\n", subValue ? "assinou" : "cancelou",
                  maps[idx].chrUuid.toString().c_str());
  }
} localChrCB;

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
    if (phoneCount == 0) phoneMtu = 247;
    phoneCount++;
    // Por qual identidade ele entrou? O endereco que NOS usamos nesta conexao diz.
    struct ble_gap_conn_desc d;
    bool viaHid = false;
    if (ble_gap_conn_find(info.getConnHandle(), &d) == 0)
      viaHid = MIDIA_HID && memcmp(d.our_ota_addr.val, hidAddrVal, 6) == 0;
    if (viaHid) hidConnHandle = info.getConnHandle(); else appConns++;
    phoneConnected = true;
    phoneConnT = millis();
    Serial.printf("Celular conectado: %s (handle %u) - %d conectado(s)\n",
                  info.getAddress().toString().c_str(), info.getConnHandle(), phoneCount);
    Serial.printf("  -> pela identidade %s (apps: %d)\n", viaHid ? NOME_CONTROLE : "da roda", appConns);
    logEvent("%s conectou %s", viaHid ? "controle de midia" : "app", info.getAddress().toString().c_str());
    advDirty = true;
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int reason) override {
    if (phoneCount > 0) phoneCount--;
    if (info.getConnHandle() == hidConnHandle) hidConnHandle = BLE_HS_CONN_HANDLE_NONE;
    else if (appConns > 0) appConns--;
    phoneConnected = (phoneCount > 0);
    for (auto& m : maps) subRemove(m, info.getConnHandle());   // esquece as assinaturas dele
    logEvent("app desconectou apos %lus, motivo 0x%X (restam %d)",
             (unsigned long)((millis() - phoneConnT) / 1000), reason, phoneCount);
    Serial.printf("Celular desconectou apos %lu ms, motivo %d (0x%X) - restam %d\n",
                  (unsigned long)(millis() - phoneConnT), reason, reason, phoneCount);
    advDirty = true;
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    Serial.printf("Celular pediu seguranca: cifrado=%d pareado=%d\n", info.isEncrypted(), info.isBonded());
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo& info) override {
    // o MTU da conexao do controle de midia nao importa: ela nao recebe os dados da roda
    if (info.getConnHandle() != hidConnHandle && mtu < phoneMtu) phoneMtu = mtu;
    Serial.printf("MTU do celular: %u\n", mtu);
  }
} serverCB;

static uint32_t propsOf(NimBLERemoteCharacteristic* rc) {
  uint32_t p = 0;
  if (rc->canRead())            p |= NIMBLE_PROPERTY::READ;
  if (rc->canWrite())           p |= NIMBLE_PROPERTY::WRITE;
  if (rc->canWriteNoResponse()) p |= NIMBLE_PROPERTY::WRITE_NR;
  if (rc->canNotify())          p |= NIMBLE_PROPERTY::NOTIFY;
  if (rc->canIndicate())        p |= NIMBLE_PROPERTY::INDICATE;
  return p;
}

static bool isGenericService(const NimBLEUUID& u) {
  return u == NimBLEUUID((uint16_t)0x1800) || u == NimBLEUUID((uint16_t)0x1801);
}

// ---------------------------------------------------------------------------
// A NimBLE NAO permite montar/alterar o servidor GATT enquanto existe QUALQUER
// conexao aberta (inclusive a nossa com a roda) -> assert em ble_svc_gap_init.
// Por isso o clone acontece em 3 etapas:
//   1. Na primeira conexao: grava um "modelo" da GATT da roda (UUIDs, props, valores).
//   2. Desconecta da roda e monta o servidor local a partir do modelo.
//   3. Reconecta e mapeia remoto <-> local pelos UUIDs.
// Nas reconexoes seguintes so a etapa 3 e feita.
// ---------------------------------------------------------------------------
struct DescInfo { NimBLEUUID uuid; std::string value; };
struct ChrInfo { NimBLEUUID uuid; uint32_t props; std::string value; std::vector<DescInfo> descs; };
struct SvcInfo { NimBLEUUID uuid; std::vector<ChrInfo> chrs; };
static std::vector<SvcInfo> gattTemplate;

static void recordTemplate() {
  gattTemplate.clear();
  for (auto* rs : client->getServices(false)) {
    if (isGenericService(rs->getUUID())) continue;
    SvcInfo si;
    si.uuid = rs->getUUID();
    for (auto* rc : rs->getCharacteristics(false)) {
      ChrInfo ci;
      ci.uuid = rc->getUUID();
      ci.props = propsOf(rc);
      if (rc->canRead()) {
        NimBLEAttValue v = rc->readValue();
        ci.value.assign((const char*)v.data(), v.size());
      }
      Serial.printf("  %s props=0x%02X val=\"", ci.uuid.toString().c_str(), (unsigned)ci.props);
      for (char ch : ci.value) Serial.print(isprint((uint8_t)ch) ? ch : '.');
      Serial.println("\"");
      // Descritores: so o 0x2901 (User Description).
      // O 0x2902 a NimBLE cria sozinha. E a descoberta de descritores devolve, por engano,
      // a DECLARACAO da proxima caracteristica (0x2803) - clonar isso corrompia a GATT.
      for (auto* rd : rc->getDescriptors(true)) {
        if (rd->getUUID() != NimBLEUUID((uint16_t)0x2901)) continue;
        DescInfo di;
        di.uuid = rd->getUUID();
        NimBLEAttValue dv = rd->readValue();
        di.value.assign((const char*)dv.data(), dv.size());
        Serial.printf("      desc %s = \"", di.uuid.toString().c_str());
        for (char ch : di.value) Serial.print(isprint((uint8_t)ch) ? ch : '.');
        Serial.println("\"");
        ci.descs.push_back(di);
      }
      si.chrs.push_back(ci);
    }
    gattTemplate.push_back(si);
  }
}

// ================= Controle de midia (HID over GATT) =================
// Um "teclado" que so tem as teclas de midia. O Android le o Report Map, entende que
// e um controle de midia e repassa as teclas para o app que estiver tocando.
// Cada aperto = 1 byte com um bit ligado (tecla pressionada) e, logo depois, 0 (solta).
static const uint8_t HID_REPORT_MAP[] = {
  0x05, 0x0C,   // Usage Page (Consumer)          - "teclas de consumo" (midia)
  0x09, 0x01,   // Usage (Consumer Control)
  0xA1, 0x01,   // Collection (Application)
  0x85, 0x01,   //   Report ID (1)
  0x15, 0x00,   //   Logical Minimum (0)
  0x25, 0x01,   //   Logical Maximum (1)
  0x75, 0x01,   //   Report Size (1 bit por tecla)
  0x95, 0x08,   //   Report Count (8 teclas)
  0x09, 0xB5,   //   bit 0: Scan Next Track      (proxima)
  0x09, 0xB6,   //   bit 1: Scan Previous Track  (anterior)
  0x09, 0xCD,   //   bit 2: Play/Pause
  0x09, 0xE9,   //   bit 3: Volume Up    (do celular)
  0x09, 0xEA,   //   bit 4: Volume Down  (do celular)
  0x09, 0xE2,   //   bit 5: Mute
  0x09, 0xB7,   //   bit 6: Stop
  0x09, 0xCD,   //   bit 7: (repete play/pause, so para completar 8 bits)
  0x81, 0x02,   //   Input (Data, Variable, Absolute)
  0xC0          // End Collection
};
static const uint8_t HID_PROXIMA = 0x01, HID_ANTERIOR = 0x02, HID_PLAY = 0x04,
                     HID_VOL_MAIS = 0x08, HID_VOL_MENOS = 0x10;

static NimBLECharacteristic* hidInput = nullptr;
static uint32_t hidSoltarEm = 0;
static bool hidAtivo = false;      // o celular (o "host HID" do Android) assinou as teclas
static uint16_t hidHandle = BLE_HS_CONN_HANDLE_NONE;   // conexao que assinou

class HidCB : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& info, uint16_t subValue) override {
    hidAtivo = subValue != 0;
    hidHandle = info.getConnHandle();
    Serial.printf("Controle de midia %s (handle %u, cifrado=%d)\n",
                  hidAtivo ? "ATIVO no celular" : "desativado", info.getConnHandle(), info.isEncrypted());
    logEvent("controle de midia %s", hidAtivo ? "ativo" : "desativado");
  }
} hidCB;

static void buildHid() {
  NimBLEService* hid = server->createService(NimBLEUUID((uint16_t)0x1812));
  const uint8_t info[4] = {0x11, 0x01, 0x00, 0x02};          // HID 1.11, sem pais, "normally connectable"
  hid->createCharacteristic(NimBLEUUID((uint16_t)0x2A4A), NIMBLE_PROPERTY::READ)->setValue(info, sizeof(info));
  hid->createCharacteristic(NimBLEUUID((uint16_t)0x2A4B), NIMBLE_PROPERTY::READ)
     ->setValue(HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
  hid->createCharacteristic(NimBLEUUID((uint16_t)0x2A4C), NIMBLE_PROPERTY::WRITE_NR);   // HID Control Point
  const uint8_t modo = 0x01;                                                            // "report mode"
  hid->createCharacteristic(NimBLEUUID((uint16_t)0x2A4E), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE_NR)
     ->setValue(&modo, 1);
  // A caracteristica das teclas: so pode ser lida com a conexao cifrada (pareada), como o Android exige
  hidInput = hid->createCharacteristic(NimBLEUUID((uint16_t)0x2A4D),
                                       NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC);
  const uint8_t ref[2] = {0x01, 0x01};                       // Report ID 1, tipo "entrada"
  hidInput->createDescriptor(NimBLEUUID((uint16_t)0x2908), NIMBLE_PROPERTY::READ, 2)->setValue(ref, 2);
  const uint8_t zero = 0;
  hidInput->setValue(&zero, 1);
  hidInput->setCallbacks(&hidCB);
  hid->start();

  // Servico de bateria: o Android espera encontrar um junto com o HID
  NimBLEService* bat = server->createService(NimBLEUUID((uint16_t)0x180F));
  const uint8_t nivel = 100;
  bat->createCharacteristic(NimBLEUUID((uint16_t)0x2A19), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY)
     ->setValue(&nivel, 1);
  bat->start();
  Serial.println("Controle de midia (HID) incluido na GATT");
}

// "Aperta" uma tecla de midia; o loop "solta" 40 ms depois
static void hidTecla(uint8_t bits) {
  if (!hidInput) return;
  if (!hidAtivo) Serial.println("Controle de midia ainda nao ativo: pareie o display no Bluetooth do celular");
  hidInput->setValue(&bits, 1);
  hidInput->notify();
  hidSoltarEm = millis() + 40;
}
static void hidSoltar() {
  if (!hidSoltarEm || millis() < hidSoltarEm) return;
  hidSoltarEm = 0;
  const uint8_t zero = 0;
  hidInput->setValue(&zero, 1);
  hidInput->notify();
}

static void buildServer() {
  for (auto& si : gattTemplate) {
    NimBLEService* ls = server->createService(si.uuid);
    for (auto& ci : si.chrs) {
      NimBLECharacteristic* lc = ls->createCharacteristic(ci.uuid, ci.props);
      lc->setCallbacks(&localChrCB);
      if (!ci.value.empty()) lc->setValue((const uint8_t*)ci.value.data(), ci.value.size());
      for (auto& di : ci.descs) {
        NimBLEDescriptor* ld = lc->createDescriptor(di.uuid, NIMBLE_PROPERTY::READ,
                                                    di.value.size() ? di.value.size() : 1);
        ld->setValue((const uint8_t*)di.value.data(), di.value.size());
      }
    }
    ls->start();
  }
  if (MIDIA_HID) buildHid();
  if (TESTE_SERVICO_EXTRA) {
    NimBLEService* extra = server->createService("8a1b0000-0000-4000-8000-00000000abcd");
    extra->createCharacteristic("8a1b0001-0000-4000-8000-00000000abcd", NIMBLE_PROPERTY::READ)->setValue("teste");
    extra->start();
    Serial.println("Servico extra de teste incluido na GATT");
  }
  server->start();
  serverStarted = true;
  Serial.println("Servidor local montado");
}

static void mapGatt() {
  maps.clear();
  maps.reserve(48);
  mainRemote = nullptr;
  for (auto* rs : client->getServices(false)) {
    if (isGenericService(rs->getUUID())) continue;
    NimBLEService* ls = server->getServiceByUUID(rs->getUUID());
    if (!ls) continue;
    for (auto* rc : rs->getCharacteristics(false)) {
      NimBLECharacteristic* lc = ls->getCharacteristic(rc->getUUID());
      if (!lc) continue;
      Map m;
      m.svcUuid = rs->getUUID();
      m.chrUuid = rc->getUUID();
      m.remote = rc;
      m.local = lc;
      m.remoteWriteNR = rc->canWriteNoResponse();
      m.isMain = (rs->getUUID() == MAIN_SVC && rc->getUUID() == MAIN_CHR);
      if (m.isMain) mainRemote = rc;
      maps.push_back(m);
    }
  }
  Serial.printf("Mapeadas %u caracteristicas\n", (unsigned)maps.size());
}

static void startAdvertisingAsWheel() {
  NimBLEDevice::setDeviceName(wheelAdvName);   // nome GAP (0x2A00) igual ao da roda
  NimBLEExtAdvertising* adv = NimBLEDevice::getAdvertising();

  // Identidade 1: a roda (anuncio "legado", que todo celular entende)
  NimBLEExtAdvertisement roda;
  roda.setLegacyAdvertising(true);
  roda.setConnectable(true);
  roda.setScannable(true);
  roda.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  roda.setName(wheelAdvName);
  for (auto& u : wheelAdvUuids) roda.addServiceUUID(u);
  if (wheelAdvUuids.empty()) roda.addServiceUUID(MAIN_SVC);
  adv->setInstanceData(ADV_RODA, roda);
  NimBLEExtAdvertisement respRoda;                // resposta ao "scan ativo"
  respRoda.setLegacyAdvertising(true);
  if (!wheelMfgData.empty()) respRoda.setManufacturerData(wheelMfgData);
  adv->setScanResponseData(ADV_RODA, respRoda);

  // Identidade 2: o controle de midia, com endereco proprio
  if (MIDIA_HID) {
    NimBLEExtAdvertisement hid;
    hid.setLegacyAdvertising(true);
    hid.setConnectable(true);
    hid.setScannable(true);
    hid.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    hid.setName(NOME_CONTROLE);
    hid.setAppearance(0x0180);                    // "controle remoto generico"
    hid.addServiceUUID(NimBLEUUID((uint16_t)0x1812));
    ble_addr_t a;
    a.type = BLE_ADDR_RANDOM;
    memcpy(a.val, hidAddrVal, 6);
    hid.setAddress(NimBLEAddress(a));
    adv->setInstanceData(ADV_HID, hid);
    NimBLEExtAdvertisement respHid;
    respHid.setLegacyAdvertising(true);
    adv->setScanResponseData(ADV_HID, respHid);
  }
  advConfigured = true;
  advDirty = true;
  Serial.printf("Anunciando como \"%s\"%s\n", wheelAdvName.c_str(),
                MIDIA_HID ? " e como \"S16 Controle\"" : "");
}

// Liga/desliga cada anuncio conforme as vagas. Chamado pelo loop.
static void updateAdvertising() {
  static uint32_t last = 0;
  if (!advConfigured || (!advDirty && millis() - last < 1000)) return;
  advDirty = false;
  last = millis();
  NimBLEExtAdvertising* adv = NimBLEDevice::getAdvertising();
  bool queroRoda = mainRemote && appConns < MAX_APPS;
  bool queroHid  = MIDIA_HID && hidConnHandle == BLE_HS_CONN_HANDLE_NONE;
  if (queroRoda != adv->isActive(ADV_RODA)) { if (queroRoda) adv->start(ADV_RODA); else adv->stop(ADV_RODA); }
  if (MIDIA_HID && queroHid != adv->isActive(ADV_HID)) { if (queroHid) adv->start(ADV_HID); else adv->stop(ADV_HID); }
}

// ================= Cliente (lado roda) =================
class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (target) return;
    std::string name = dev->getName();
    bool match = strlen(TARGET_MAC)
                   ? dev->getAddress().toString() == TARGET_MAC
                   : (!name.empty() && name.find(NAME_FILTER) != std::string::npos);
    if (!match) return;
    target = dev;
    NimBLEDevice::getScan()->stop();

    wheelAdvName = name;
    wheelMfgData = dev->getManufacturerData();
    wheelAdvUuids.clear();
    for (int i = 0; i < dev->getServiceUUIDCount(); i++) wheelAdvUuids.push_back(dev->getServiceUUID(i));

    Serial.printf("Roda encontrada: %s \"%s\"\n", dev->getAddress().toString().c_str(), name.c_str());
    for (auto& u : wheelAdvUuids) Serial.printf("  anuncia UUID %s\n", u.toString().c_str());
    if (!wheelMfgData.empty())
      printHex("  dados fabricante: ", (const uint8_t*)wheelMfgData.data(), wheelMfgData.size());
    doConnect = true;
  }
} scanCB;

static volatile bool intentionalDisconnect = false;

class ClientCB : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient*, int reason) override {
    mainRemote = nullptr;
    for (auto& m : maps) { m.remote = nullptr; m.remoteSub = false; }
    if (intentionalDisconnect) {           // desconexao planejada para montar o servidor
      intentionalDisconnect = false;
      return;
    }
    Serial.printf("Roda desconectou (%d)\n", reason);
    logEvent("roda desconectou (motivo 0x%X)", reason);
    NimBLEDevice::getAdvertising()->stop(ADV_RODA);
    advDirty = true;
    // derruba os apps (ficariam sem dados), mas mantem o controle de midia conectado
    if (server) for (uint16_t h : server->getPeerDevices()) if (h != hidConnHandle) server->disconnect(h);
    needScan = true;
  }
} clientCB;

// Monta e envia um comando no formato padrao da KingSong:
//   AA 55 | b2 b3 b4 b5 (parametros) 00.. | [16]=comando | 14 5A 5A
static void sendKs(uint8_t cmd, uint8_t b2, uint8_t b3, uint8_t b4, uint8_t b5) {
  if (!mainRemote) return;
  uint8_t p[20] = {0xAA, 0x55, b2, b3, b4, b5};
  p[16] = cmd; p[17] = 0x14; p[18] = 0x5A; p[19] = 0x5A;
  mainRemote->writeValue(p, sizeof(p), false);
  if (DUMP_PHONE) printHex("D> ", p, sizeof(p));   // D> = enviado pelo display
}
static void sendCmd(uint8_t cmd) { sendKs(cmd, 0, 0, 0, 0); }

// ---------- Console de experimentos (digite no Serial Monitor, "Nova linha" ligado) ----------
//   t 95 6 FF                 -> comando 95 com o byte 6 = FF (resto zero, final 14 5A 5A)
//   raw AA 55 00 ... 5A 5A    -> envia exatamente esses 20 bytes (ex.: um frame capturado)
//   esquecer                  -> apaga os pareamentos (controle de midia) guardados no display
// As respostas "diferentes" da roda aparecem como R? (ver DUMP_NOVOS).
static void handleSerialConsole() {
  static char line[160];
  static int n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') { if (n < (int)sizeof(line) - 1) line[n++] = c; continue; }
    line[n] = 0; n = 0;

    char* save;
    char* tok = strtok_r(line, " ", &save);
    if (!tok) continue;
    String nome = tok;
    uint8_t v[24]; int cnt = 0;
    while ((tok = strtok_r(nullptr, " ", &save)) && cnt < 24) v[cnt++] = strtoul(tok, nullptr, 16);

    if (nome == "esquecer") {          // apaga os pareamentos guardados no display
      NimBLEDevice::deleteAllBonds();
      Serial.println("Pareamentos apagados (esqueca o display no Bluetooth do celular tambem)");
      continue;
    }
    if (!mainRemote) { Serial.println("Roda nao conectada"); continue; }
    uint8_t p[20] = {0xAA, 0x55};
    if (nome == "t" && cnt == 3 && v[1] >= 2 && v[1] <= 15) {
      p[v[1]] = v[2];
      p[16] = v[0]; p[17] = 0x14; p[18] = 0x5A; p[19] = 0x5A;
    } else if (nome == "raw" && cnt == 20) {
      memcpy(p, v, 20);
    } else {
      Serial.println("Uso:  t <cmd> <posicao 2-15> <valor>   (hex)   ex.: t 95 6 FF");
      Serial.println("      raw <20 bytes hex>");
      continue;
    }
    mainRemote->writeValue(p, sizeof(p), false);
    printHex("D> ", p, sizeof(p));
  }
}

static bool connectToWheel() {
  if (!client) {
    client = NimBLEDevice::createClient();
    client->setClientCallbacks(&clientCB, false);
  }
  if (!client->connect(target)) return false;
  Serial.println("Conectado na roda, descobrindo servicos...");

  if (!client->discoverAttributes()) {
    Serial.println("Falha na descoberta");
    client->disconnect();
    return false;
  }

  if (!serverStarted) {
    // Etapa 1: grava o modelo
    recordTemplate();

    // Etapa 2: desconecta e monta o servidor sem nenhuma conexao aberta
    intentionalDisconnect = true;
    client->disconnect();
    uint32_t t0 = millis();
    while (client->isConnected() && millis() - t0 < 3000) delay(10);
    delay(300);
    buildServer();

    // Etapa 3: reconecta (algumas tentativas, a roda leva um instante para voltar a anunciar)
    bool ok = false;
    for (int i = 0; i < 5 && !ok; i++) {
      delay(500);
      Serial.printf("Reconectando na roda (tentativa %d)...\n", i + 1);
      ok = client->connect(target) && client->discoverAttributes();
    }
    if (!ok) { Serial.println("Falha ao reconectar"); return false; }
  }

  mapGatt();

  if (!mainRemote || !mainRemote->subscribe(true, remoteNotifyCB)) {
    Serial.println("Falha ao assinar FFE1");
    client->disconnect();
    return false;
  }
  for (auto& m : maps) if (m.isMain) m.remoteSub = true;
  Serial.println("Inscrito em FFE1");

  sendCmd(0x9B);
  delay(100);
  sendCmd(0x63);
  delay(100);
  mainRemote->writeValue(PEDE_HORA, sizeof(PEDE_HORA), false);   // resposta chega como F9
  return true;
}

static void startScan() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->clearResults();
  target = nullptr;
  scan->start(0, false, true);
}

// Sincroniza assinaturas das caracteristicas secundarias com o que o celular pediu
static void syncSubscriptions() {
  for (auto& m : maps) {
    if (m.isMain || !m.remote) continue;
    bool want = phoneConnected && m.phoneSub;
    if (want && !m.remoteSub) {
      bool useNotify = m.remote->canNotify();
      if (m.remote->subscribe(useNotify, remoteNotifyCB)) {
        m.remoteSub = true;
        Serial.printf("Assinada na roda: %s\n", m.chrUuid.toString().c_str());
      }
    } else if (!want && m.remoteSub) {
      m.remote->unsubscribe();
      m.remoteSub = false;
    }
  }
}

static void processWrites() {
  PendingWrite pw;
  while (xQueueReceive(writeQ, &pw, 0) == pdTRUE) {
    if (pw.idx < maps.size() && maps[pw.idx].remote)
      maps[pw.idx].remote->writeValue(pw.data, pw.len, true);
  }
}

// ================= Tela =================
static const uint16_t COL_DIM   = 0x7BEF;  // cinza
static const uint16_t COL_FRAME = 0x39E7;  // cinza escuro

static uint16_t pwmColor(int p) {
  if (p < 70) return TFT_GREEN;
  if (p < 85) return TFT_YELLOW;
  return TFT_RED;
}

// Telas: 0 = BMS (esquerda), 1 = principal, 2 = controles (direita)
static const int NUM_PAGES = 3;
static int page = 1;
static uint32_t hornFlashUntil = 0;     // mostra "BUZINA" na barra por um instante

static void drawTopBar() {
  bool wheelOk = client && client->isConnected();
  spr.fillRect(0, 0, 240, 22, 0x18E3);
  spr.setFont(&fonts::Font2);
  spr.setTextSize(1);
  spr.setTextDatum(middle_left);
  spr.setTextColor(wheelOk ? TFT_GREEN : TFT_RED);
  spr.drawString("RODA", 4, 11);
  // APP n: quantos apps estao recebendo os dados da roda (assinaram a FFE1)
  int apps = appConns;
  char ap[8];
  if (apps > 0) snprintf(ap, sizeof(ap), "APP %d", apps); else strcpy(ap, "APP");
  spr.setTextColor(apps > 0 ? TFT_CYAN : COL_FRAME);
  spr.drawString(ap, 44, 11);
  // nota musical: controle de midia (HID) ativo no celular
  if (MIDIA_HID) {
    bool hidOk = hidAtivo && hidConnHandle != BLE_HS_CONN_HANDLE_NONE;
    uint16_t cn = hidOk ? TFT_MAGENTA : COL_FRAME;
    spr.fillCircle(92, 15, 3, cn);
    spr.drawFastVLine(95, 4, 12, cn);
    spr.drawLine(95, 4, 99, 7, cn);
  }
  if (millis() < hornFlashUntil) spr.fillCircle(110, 11, 5, TFT_YELLOW);   // buzina
  if (wd.cooling > 0) { spr.setTextColor(TFT_ORANGE); spr.drawString("FAN", 118, 11); }
  // bolinhas indicando a tela atual
  for (int i = 0; i < NUM_PAGES; i++) {
    int x = 148 + i * 12;
    if (i == page) spr.fillCircle(x, 11, 3, TFT_WHITE); else spr.drawCircle(x, 11, 3, COL_DIM);
  }
  spr.setTextDatum(middle_right);
  spr.setTextColor(COL_DIM);
  char buf[16];
  if (clockSet) {                       // hora (acertada pela roda ou pelo app)
    time_t t = time(nullptr); struct tm tmv; localtime_r(&t, &tmv);
    strftime(buf, sizeof(buf), "%H:%M", &tmv);
    spr.setTextColor(TFT_WHITE);
    spr.drawString(buf, 236, 11);
  }
  if (rideLogging) spr.fillCircle(184, 11, 5, TFT_RED);   // gravando
}

static void drawCell(int x, int y, int w, int h, const char* label, const char* value) {
  spr.drawRoundRect(x, y, w, h, 6, COL_FRAME);
  spr.setFont(&fonts::Font2);
  spr.setTextSize(1);
  spr.setTextColor(COL_DIM);
  spr.setTextDatum(top_left);
  spr.drawString(label, x + 6, y + 3);
  spr.setFont(&fonts::FreeSansBold12pt7b);
  spr.setTextColor(TFT_WHITE);
  spr.setTextDatum(bottom_right);
  spr.drawString(value, x + w - 6, y + h - 3);
}

static void drawStatus() {
  spr.fillScreen(TFT_BLACK);
  drawTopBar();
  spr.setTextDatum(middle_center);
  spr.setFont(&fonts::FreeSansBold12pt7b);
  spr.setTextColor(TFT_WHITE);
  spr.drawString(statusL1, 120, 140);
  spr.setFont(&fonts::FreeSans9pt7b);
  spr.setTextColor(COL_DIM);
  spr.drawString(statusL2, 120, 175);
  spr.pushSprite(0, 0);
}

static void drawData() {
  if (millis() - wd.lastUpdate > 2000) { statusMsg("Conectado", "Sem dados..."); return; }
  char buf[24];
  spr.fillScreen(TFT_BLACK);
  drawTopBar();

  // ---- velocidade ----
  spr.setFont(&fonts::Font7);          // fonte de 7 segmentos (so digitos)
  spr.setTextSize(2);
  spr.setTextDatum(middle_center);
  spr.setTextColor(wd.pwm >= 0 ? pwmColor(wd.pwm) : TFT_WHITE);
  snprintf(buf, sizeof(buf), "%d", (int)(wd.speed + 0.5f));
  spr.drawString(buf, 120, 78);
  spr.setTextSize(1);
  spr.setFont(&fonts::Font2);
  spr.setTextColor(COL_DIM);
  if (wd.speedLimit > 0) {
    snprintf(buf, sizeof(buf), "km/h   lim %.0f", wd.speedLimit);
    spr.drawString(buf, 120, 135);
  } else {
    spr.drawString("km/h", 120, 135);
  }

  // ---- barra de PWM ----
  const int bx = 8, by = 150, bw = 224, bh = 34;
  spr.drawRoundRect(bx, by, bw, bh, 6, COL_FRAME);
  if (wd.pwm > 0) {
    int fill = (bw - 4) * min(wd.pwm, 100) / 100;
    spr.fillRoundRect(bx + 2, by + 2, fill, bh - 4, 4, pwmColor(wd.pwm));
  }
  spr.setFont(&fonts::FreeSansBold12pt7b);
  spr.setTextDatum(middle_center);
  spr.setTextColor(TFT_WHITE);
  if (wd.pwm >= 0) snprintf(buf, sizeof(buf), "PWM %d%%", wd.pwm); else strcpy(buf, "PWM --");
  spr.drawString(buf, 120, by + bh / 2);

  // ---- grade de valores (3 linhas x 2 colunas) ----
  const int gy = 194, cw = 114, ch = 40, gap = 4, x1 = 4, x2 = 4 + cw + gap + 4;
  snprintf(buf, sizeof(buf), "%.1f V", wd.voltage);   drawCell(x1, gy,               cw, ch, "TENSAO", buf);
  snprintf(buf, sizeof(buf), "%.1f A", wd.current);   drawCell(x2, gy,               cw, ch, "CORRENTE", buf);
  snprintf(buf, sizeof(buf), "%.0f C", wd.temp);      drawCell(x1, gy + ch + gap,     cw, ch, "TEMP", buf);
  snprintf(buf, sizeof(buf), "%.0f C", wd.temp2);     drawCell(x2, gy + ch + gap,     cw, ch, "TEMP 2", buf);
  snprintf(buf, sizeof(buf), "%.2f km", wd.tripKm);   drawCell(x1, gy + 2 * (ch + gap), cw, ch, "TRIP", buf);
  snprintf(buf, sizeof(buf), "%.1f", wd.topSpeed);    drawCell(x2, gy + 2 * (ch + gap), cw, ch, "MAX km/h", buf);

  spr.pushSprite(0, 0);
}

// ---------- Tela BMS (tensoes das celulas) ----------
// Para cada pack: um cabecalho (tensao, SoC, temperatura maxima), a linha min/max/diferenca
// e um grafico de barras com as 20 celulas. A escala das barras e "ampliada" em volta
// das tensoes reais, senao diferencas de poucos mV ficariam invisiveis.
static void drawBmsPack(int idx, int y0, uint16_t lo, uint16_t hi) {
  const BmsPack& b = bms[idx];
  char buf[48];
  spr.setFont(&fonts::Font2);
  spr.setTextSize(1);
  spr.setTextDatum(top_left);
  spr.setTextColor(TFT_WHITE);

  if (b.nCells == 0 || millis() - b.lastUpdate > 10000) {
    snprintf(buf, sizeof(buf), "PACK %d: aguardando dados", idx + 1);
    spr.setTextColor(COL_DIM);
    spr.drawString(buf, 6, y0 + 4);
    return;
  }

  float tmax = NAN;
  for (int i = 0; i < b.nTemps; i++) if (!isnan(b.temps[i]) && (isnan(tmax) || b.temps[i] > tmax)) tmax = b.temps[i];
  snprintf(buf, sizeof(buf), "PACK %d  %.1fV  %.0f%%  %.0fC", idx + 1, b.voltage, b.soc, isnan(tmax) ? 0.0f : tmax);
  spr.drawString(buf, 6, y0);

  int iMin = 0, iMax = 0;
  for (int i = 1; i < b.nCells; i++) {
    if (b.cell[i] < b.cell[iMin]) iMin = i;
    if (b.cell[i] > b.cell[iMax]) iMax = i;
  }
  snprintf(buf, sizeof(buf), "min %.3f(%d) max %.3f(%d) d%dmV",
           b.cell[iMin] / 1000.0f, iMin + 1, b.cell[iMax] / 1000.0f, iMax + 1, b.cell[iMax] - b.cell[iMin]);
  spr.setTextColor(COL_DIM);
  spr.drawString(buf, 6, y0 + 17);

  // barras
  const int gx = 6, gy = y0 + 36, gw = 228, gh = 96;
  spr.drawRect(gx, gy, gw, gh, COL_FRAME);
  int bw = (gw - 4) / b.nCells;
  for (int i = 0; i < b.nCells; i++) {
    int h = (int)((long)(constrain(b.cell[i], lo, hi) - lo) * (gh - 4) / (hi - lo));
    uint16_t col = (i == iMin) ? TFT_ORANGE : (i == iMax) ? TFT_GREEN : TFT_CYAN;
    spr.fillRect(gx + 2 + i * bw + 1, gy + gh - 2 - h, bw - 2, h, col);
  }
}

static void drawBms() {
  spr.fillScreen(TFT_BLACK);
  drawTopBar();
  // escala comum aos dois packs, com pelo menos 50 mV de janela
  uint16_t lo = 65535, hi = 0;
  for (auto& b : bms) for (int i = 0; i < b.nCells; i++) { lo = min(lo, b.cell[i]); hi = max(hi, b.cell[i]); }
  if (hi < lo) { lo = 3500; hi = 4200; }
  lo -= 20; hi += 5;
  if (hi - lo < 50) lo = hi - 50;
  drawBmsPack(0, 28, lo, hi);
  drawBmsPack(1, 176, lo, hi);
  spr.pushSprite(0, 0);
}

// ---------- Tela de controles (icones desenhados com formas simples) ----------
// Grade de 4 linhas x 3 colunas. Cada botao tem um tipo de icone e uma acao.
enum Icone { IC_CEL_MENOS, IC_CEL_MAIS, IC_VAZIO,
             IC_PREV, IC_PLAY, IC_NEXT,
             IC_VOL_MENOS, IC_VOL_MAIS, IC_LOG,
             IC_FAROL, IC_LEDS, IC_BUZINA };
// Layout (4 linhas x 3 colunas):
//   [cel -] [cel +] [     ]      <- CELULAR: volume (teclas de midia HID)
//   [ |<< ] [ >|| ] [ >>| ]      <-          musica (HID)
//   [roda-] [roda+] [ log ]      <- RODA: volume dos falantes (comando 95) e gravacao
//   [farol] [ LEDs] [buzina]     <-       farol e LEDs alternam a cada toque
static const Icone layout[12] = {
  IC_CEL_MENOS, IC_CEL_MAIS, IC_VAZIO,
  IC_PREV, IC_PLAY, IC_NEXT,
  IC_VOL_MENOS, IC_VOL_MAIS, IC_LOG,
  IC_FAROL, IC_LEDS, IC_BUZINA,
};
static const int BT_W = 72, BT_H = 62;
static int botaoX(int i) { return 6 + (i % 3) * 78; }
static int botaoY(int i) { return 32 + (i / 3) * 72; }
static int botaoAceso = -1;              // destaque rapido no botao tocado
static uint32_t botaoAcesoAte = 0;

// Estado dos botoes que alternam. A roda nao conta o estado atual, entao comecamos
// em "desconhecido" (-1) e passamos a saber a partir do primeiro toque.
//   farol: 0 liga -> 1 desliga -> 2 automatico -> 0 ...   (comando 73: 12 / 13 / 14)
//   LEDs:  1 ligados <-> 0 desligados                      (comando 6C: 00 liga / 01 desliga)
static int farolModo = -1;
static int ledsLigados = -1;
static const uint8_t FAROL_CMD[3] = {0x12, 0x13, 0x14};
static const char* FAROL_NOME[3] = {"liga", "desliga", "auto"};

// --- pecas de icones (cx, cy = centro do botao) ---
static void icFarol(int cx, int cy, uint16_t c, bool raios) {
  spr.fillCircle(cx, cy, 9, c);
  if (!raios) return;
  for (int a = 0; a < 360; a += 45) {          // 8 raios em volta
    float r = a * DEG_TO_RAD;
    spr.drawLine(cx + cosf(r) * 13, cy + sinf(r) * 13, cx + cosf(r) * 19, cy + sinf(r) * 19, c);
  }
}
static void icTriangulo(int x, int cy, int dir, uint16_t c) {   // dir = +1 direita, -1 esquerda
  spr.fillTriangle(x, cy - 9, x, cy + 9, x + dir * 13, cy, c);
}
static void icAltoFalante(int cx, int cy, uint16_t c) {
  spr.fillRect(cx - 16, cy - 5, 7, 10, c);
  spr.fillTriangle(cx - 9, cy, cx - 1, cy - 11, cx - 1, cy + 11, c);
}
static void icCelular(int cx, int cy, uint16_t c) {        // celular a esquerda do centro
  spr.drawRoundRect(cx - 18, cy - 14, 15, 28, 3, c);
  spr.drawFastHLine(cx - 14, cy - 10, 7, c);              // "alto-falante" do celular
  spr.fillCircle(cx - 10, cy + 9, 1, c);                  // "botao"
}
static void icMenos(int cx, int cy, uint16_t c) { spr.fillRect(cx + 4, cy - 2, 12, 4, c); }
static void icMais(int cx, int cy, uint16_t c)  { icMenos(cx, cy, c); spr.fillRect(cx + 8, cy - 6, 4, 12, c); }
static void icArquivo(int cx, int cy, uint16_t c) {
  const int x = cx - 11, y = cy - 15, w = 22, h = 30, dobra = 7;
  spr.drawLine(x, y, x + w - dobra, y, c);                 // topo
  spr.drawLine(x + w - dobra, y, x + w, y + dobra, c);     // canto dobrado
  spr.drawLine(x + w, y + dobra, x + w, y + h, c);
  spr.drawLine(x, y + h, x + w, y + h, c);
  spr.drawLine(x, y, x, y + h, c);
  for (int k = 0; k < 3; k++) spr.drawFastHLine(x + 4, y + 12 + k * 6, w - 8, c);   // "linhas de texto"
}
static void icInterrogacao(int cx, int cy, uint16_t c) {
  spr.setFont(&fonts::Font2); spr.setTextDatum(middle_center);
  spr.setTextColor(c); spr.drawString("?", cx, cy + 1);
}

// (recebe int em vez de Icone: o Arduino gera "prototipos" das funcoes no topo do
//  arquivo, antes do enum existir, e um parametro do tipo Icone daria erro)
static void drawIcone(int icInt, int cx, int cy, uint16_t c) {
  Icone ic = (Icone)icInt;
  switch (ic) {
    case IC_CEL_MENOS:  icCelular(cx, cy, c); icMenos(cx, cy, c); break;
    case IC_CEL_MAIS:   icCelular(cx, cy, c); icMais(cx, cy, c); break;
    case IC_LOG:        icArquivo(cx, cy, rideLogging ? TFT_RED : (sdOk ? c : COL_FRAME)); break;
    case IC_PREV:       spr.fillRect(cx - 16, cy - 9, 3, 18, c);
                        icTriangulo(cx - 1, cy, -1, c); icTriangulo(cx + 12, cy, -1, c); break;
    case IC_NEXT:       spr.fillRect(cx + 13, cy - 9, 3, 18, c);
                        icTriangulo(cx + 1, cy, +1, c); icTriangulo(cx - 12, cy, +1, c); break;
    case IC_PLAY:       icTriangulo(cx - 14, cy, +1, c);
                        spr.fillRect(cx + 4, cy - 9, 4, 18, c); spr.fillRect(cx + 11, cy - 9, 4, 18, c); break;
    case IC_VOL_MENOS:  icAltoFalante(cx, cy, c); icMenos(cx, cy, c); break;
    case IC_VOL_MAIS:   icAltoFalante(cx, cy, c); icMais(cx, cy, c); break;
    case IC_FAROL:
      if (farolModo == 0)      icFarol(cx, cy, TFT_YELLOW, true);
      else if (farolModo == 1) { icFarol(cx, cy, COL_DIM, false);
                                 spr.drawLine(cx - 14, cy + 14, cx + 14, cy - 14, TFT_RED); }
      else if (farolModo == 2) { icFarol(cx, cy, TFT_YELLOW, false);
                                 spr.setFont(&fonts::Font2); spr.setTextDatum(middle_center);
                                 spr.setTextColor(TFT_BLACK); spr.drawString("A", cx, cy + 1); }
      else                     { icFarol(cx, cy, COL_FRAME, false); icInterrogacao(cx, cy, TFT_WHITE); }
      break;
    case IC_LEDS:
      if (ledsLigados == 1) {
        spr.fillCircle(cx - 13, cy, 6, TFT_RED); spr.fillCircle(cx, cy, 6, TFT_GREEN); spr.fillCircle(cx + 13, cy, 6, TFT_BLUE);
      } else {
        spr.drawCircle(cx - 13, cy, 6, COL_DIM); spr.drawCircle(cx, cy, 6, COL_DIM); spr.drawCircle(cx + 13, cy, 6, COL_DIM);
        if (ledsLigados == 0) spr.drawLine(cx - 20, cy + 10, cx + 20, cy - 10, TFT_RED);
        else                  icInterrogacao(cx, cy, TFT_WHITE);
      }
      break;
    case IC_BUZINA:     spr.fillRect(cx - 14, cy - 5, 6, 10, c);                  // corneta
                        spr.fillTriangle(cx - 8, cy - 5, cx - 8, cy + 5, cx + 6, cy - 12, c);
                        spr.fillTriangle(cx - 8, cy + 5, cx + 6, cy + 12, cx + 6, cy - 12, c);
                        spr.drawArc(cx + 6, cy, 12, 10, 300, 360, c);
                        spr.drawArc(cx + 6, cy, 12, 10, 0, 60, c); break;
    case IC_VAZIO:      break;
  }
}

static void drawControls() {
  spr.fillScreen(TFT_BLACK);
  drawTopBar();
  for (int i = 0; i < 12; i++) {
    if (layout[i] == IC_VAZIO) continue;                  // posicao livre: sem botao
    int x = botaoX(i), y = botaoY(i);
    bool aceso = (i == botaoAceso && millis() < botaoAcesoAte);
    spr.fillRoundRect(x, y, BT_W, BT_H, 10, aceso ? 0x6B4D : 0x2124);
    spr.drawRoundRect(x, y, BT_W, BT_H, 10, (layout[i] == IC_LOG && rideLogging) ? TFT_RED : COL_FRAME);
    drawIcone(layout[i], x + BT_W / 2, y + BT_H / 2, TFT_WHITE);
  }
  spr.pushSprite(0, 0);
}

static void hidOuAviso(uint8_t tecla) {
  if (MIDIA_HID) hidTecla(tecla);
  else Serial.println("Controle de midia desligado (MIDIA_HID = false)");
}

static void tocarControles(int x, int y) {
  for (int i = 0; i < 12; i++) {
    int bx = botaoX(i), by = botaoY(i);
    if (x < bx || x >= bx + BT_W || y < by || y >= by + BT_H) continue;
    Icone ic = layout[i];
    if (ic == IC_VAZIO) return;
    botaoAceso = i;
    botaoAcesoAte = millis() + 250;

    switch (ic) {
      case IC_CEL_MENOS: logEvent("toque: volume celular -"); hidOuAviso(HID_VOL_MENOS); break;
      case IC_CEL_MAIS:  logEvent("toque: volume celular +"); hidOuAviso(HID_VOL_MAIS); break;
      case IC_PREV:      logEvent("toque: musica anterior");  hidOuAviso(HID_ANTERIOR); break;
      case IC_PLAY:      logEvent("toque: play/pause");       hidOuAviso(HID_PLAY); break;
      case IC_NEXT:      logEvent("toque: musica proxima");   hidOuAviso(HID_PROXIMA); break;
      case IC_VOL_MENOS: logEvent("toque: volume roda -");    sendKs(0x95, 0, 0xFF, 0, 0); break;
      case IC_VOL_MAIS:  logEvent("toque: volume roda +");    sendKs(0x95, 0xFF, 0, 0, 0); break;
      case IC_LOG:
        logEvent("toque: log");
        if (!sdOk) { Serial.println("Sem cartao SD"); break; }
        if (rideLogging) stopRideLog(); else startRideLog();
        break;
      case IC_FAROL:
        farolModo = (farolModo + 1) % 3;          // -1 (desconhecido) vira 0 = liga
        sendKs(0x73, FAROL_CMD[farolModo], 0, 0, 0);
        logEvent("toque: farol %s", FAROL_NOME[farolModo]);
        break;
      case IC_LEDS:
        ledsLigados = (ledsLigados == 1) ? 0 : 1; // desconhecido vira "liga"
        sendKs(0x6C, ledsLigados ? 0x00 : 0x01, 0, 0, 0);
        logEvent("toque: leds %s", ledsLigados ? "liga" : "desliga");
        break;
      case IC_BUZINA:
        logEvent("toque: buzina");
        sendKs(0x88, 0, 0, 0, 0);
        hornFlashUntil = millis() + 600;
        break;
      default: break;
    }
    return;
  }
}

static void drawPage() {
  if (page == 0) drawBms();
  else if (page == 2) drawControls();
  else drawData();
}

// ---------- Touch: deslizar troca de tela, toque aperta botao ----------
static void handleTouch() {
  static bool dedo = false;
  static int x0, y0, xu, yu;
  static uint32_t t0;
  int32_t x, y;
  if (tft.getTouch(&x, &y)) {
    if (!dedo) { dedo = true; x0 = x; y0 = y; t0 = millis(); }
    xu = x; yu = y;
    return;
  }
  if (!dedo) return;
  dedo = false;                                   // o dedo acabou de soltar
  int dx = xu - x0, dy = yu - y0;
  if (abs(dx) > 60 && abs(dx) > abs(dy)) {        // deslizou na horizontal
    if (dx < 0 && page < NUM_PAGES - 1) page++;   // para a esquerda: tela da direita
    if (dx > 0 && page > 0) page--;
    Serial.printf("Tela %d\n", page);
  } else if (abs(dx) < 15 && abs(dy) < 15 && millis() - t0 < 600) {   // foi um toque
    if (page == 2) tocarControles(x0, y0);
  }
}

// ---------- Botao BOOT = buzina ----------
static const int PIN_BUZINA = 0;           // botao BOOT da placa
static void handleHornButton() {
  static bool estavaApertado = false;
  static uint32_t ultimaMudanca = 0;
  bool apertado = digitalRead(PIN_BUZINA) == LOW;   // INPUT_PULLUP: apertado = LOW
  if (apertado != estavaApertado && millis() - ultimaMudanca > 30) {   // filtra o "repique"
    ultimaMudanca = millis();
    estavaApertado = apertado;
    if (apertado) {                               // so na hora de apertar
      sendKs(0x88, 0, 0, 0, 0);
      hornFlashUntil = millis() + 600;
      logEvent("botao BOOT: buzina");
    }
  }
}

// ================= Setup / Loop =================
void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(0);          // retrato, 240x320 (USB-C embaixo; mude para 2 para inverter)
  tft.setBrightness(200);
  spr.setPsram(true);
  spr.setColorDepth(16);
  if (!spr.createSprite(240, 320)) Serial.println("ERRO: sem memoria para o sprite (PSRAM ligada?)");
  Serial.printf("PSRAM: %u bytes\n", ESP.getPsramSize());
  statusMsg("KS-S16 ponte v4", "Escaneando...");

  pinMode(PIN_BUZINA, INPUT_PULLUP);
  logQ = xQueueCreate(16, sizeof(LogEvt));

  // Cartao SD
  SD_MMC.setPins(38, 40, 39);
  if (SD_MMC.begin("/sdcard", true)) {
    sdOk = true;
    SD_MMC.mkdir("/logs");
    Serial.printf("Cartao SD: %llu MB\n", SD_MMC.cardSize() / (1024 * 1024));
    logEvent("display ligado");
  } else {
    Serial.println("Sem cartao SD (logs desligados)");
  }
  writeQ = xQueueCreate(16, sizeof(PendingWrite));

  NimBLEDevice::init("");
  NimBLEDevice::setMTU(247);
  {
    // Endereco do "S16 Controle": o MAC Bluetooth do chip, com os 2 bits mais altos ligados
    // (e o que caracteriza um endereco "random static"). Fixo: o celular reconhece sempre.
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    for (int k = 0; k < 6; k++) hidAddrVal[k] = mac[5 - k];   // ordem invertida (little-endian)
    hidAddrVal[0] ^= 0x01;                                    // diferente do endereco da roda
    hidAddrVal[5] |= 0xC0;
  }
  if (MIDIA_HID) {
    // Pareamento "so confirmar" (sem PIN), com bonding (o celular lembra do display)
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  }

  server = NimBLEDevice::createServer();
  server->setCallbacks(&serverCB, false);
  server->advertiseOnDisconnect(false);

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCB, false);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  startScan();
}

void loop() {
  if (doConnect) {
    doConnect = false;
    statusMsg("Conectando...", wheelAdvName.c_str());
    if (connectToWheel()) {
      startAdvertisingAsWheel();
      logEvent("roda conectada: %s", wheelAdvName.c_str());
      statusMsg("Roda OK", "Aguardando dados...");
    } else {
      statusMsg("Falha na conexao", "Reescaneando...");
      needScan = true;
    }
  }

  if (needScan) {
    needScan = false;
    statusMsg("KS-S16 ponte v4", "Escaneando...");
    startScan();
  }

  processWrites();
  syncSubscriptions();
  writeEvents();
  writeRideLine();
  handleSerialConsole();
  if (MIDIA_HID) hidSoltar();
  updateAdvertising();

  // Diagnostico: quanto sobrou, no pior momento, da pilha da tarefa do Bluetooth.
  // Os callbacks (repasse, logs, Serial) rodam nela. Se chegar perto de 0, ela "transborda"
  // e corrompe a memoria vizinha - suspeita dos reinicios dentro da NimBLE.
  static uint32_t lastStack = 0;
  if (millis() - lastStack > 10000) {
    lastStack = millis();
    TaskHandle_t h = xTaskGetHandle("nimble_host");
    if (h) Serial.printf("[pilha] nimble_host: %u bytes livres (minimo ate agora)\n",
                         (unsigned)uxTaskGetStackHighWaterMark(h));
  }

  bool wheelOk = client && client->isConnected();
  if (wheelOk) {
    handleHornButton();
    handleTouch();
  }

  static uint32_t lastDraw = 0;
  static int lastPage = -1;
  if (wheelOk && (millis() - lastDraw > 100 || page != lastPage)) {   // ~10 quadros/s
    lastDraw = millis();
    lastPage = page;
    drawPage();
  }
}
