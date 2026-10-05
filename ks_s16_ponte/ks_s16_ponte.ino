/*
  Ponte BLE KingSong KS-S16 <-> ESP32-C3 <-> Celular  (v3: clone completo da GATT)

  - Conecta na roda, descobre TODOS os servicos/caracteristicas.
  - Recria todos eles no servidor do ESP32 (exceto 0x1800/0x1801, que a pilha ja tem),
    com as mesmas propriedades. Valores legiveis (ex.: Device Information 0x180A)
    sao copiados.
  - Anuncia com o mesmo nome, UUIDs e dados de fabricante da roda.
  - Repassa CRU: notificacoes de qualquer caracteristica -> celular;
    escritas do celular em qualquer caracteristica -> roda.
  - FFE1 e sempre assinada (para o LCD); as demais so quando o celular assina.

  Bibliotecas: NimBLE-Arduino 2.x, LiquidCrystal_I2C (Frank de Brabander)
  Placa: "ESP32C3 Dev Module", USB CDC On Boot: Enabled
*/

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <vector>

// ================= Configuracao =================
static const char* NAME_FILTER = "KS";
static const char* TARGET_MAC  = "";       // ex.: "c5:39:32:38:4c:4b"
static const uint8_t LCD_ADDR  = 0x27;
static const int I2C_SDA = 8;
static const int I2C_SCL = 9;
static const bool DUMP_HEX  = false;       // frames da FFE1 vindos da roda (R>) - muito volume
static const bool DUMP_PHONE = true;       // tudo que o CELULAR faz: leituras, escritas, assinaturas
static const bool DUMP_OTHER = true;       // trafego nas OUTRAS caracteristicas (raro, interessante)

static NimBLEUUID MAIN_SVC("FFE0");
static NimBLEUUID MAIN_CHR("FFE1");

LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

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
static const int MAX_PHONES = 2;          // NimBLE padrao = 3 conexoes, uma e a roda
static volatile int phoneCount = 0;
static volatile bool phoneConnected = false;
static volatile uint16_t phoneMtu = 247;  // menor MTU entre os celulares conectados
static uint32_t phoneConnT = 0;

// ---- fila de escritas que precisam de resposta (nao podem ser feitas no callback) ----
struct PendingWrite { uint8_t idx; uint16_t len; uint8_t data[244]; };
static QueueHandle_t writeQ;

// ---- dados para o LCD ----
struct WheelData {
  float voltage = 0, speed = 0, current = 0, temp = 0, tripKm = 0, topSpeed = 0;
  int pwm = -1;
  uint32_t lastUpdate = 0;
} wd;

// ================= Utilitarios =================
static uint16_t u16(const uint8_t* d, int i) { return d[i] | (d[i + 1] << 8); }
static int16_t  s16(const uint8_t* d, int i) { return (int16_t)u16(d, i); }
static uint32_t u32r(const uint8_t* d, int i) { return ((uint32_t)u16(d, i) << 16) | u16(d, i + 2); }

static void printHex(const char* prefix, const uint8_t* d, size_t len) {
  Serial.print(prefix);
  for (size_t i = 0; i < len; i++) Serial.printf("%02X ", d[i]);
  Serial.println();
}

static void lcdLine(uint8_t row, const char* txt) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16s", txt);
  lcd.setCursor(0, row);
  lcd.print(buf);
}
static void lcdMsg(const char* l1, const char* l2) { lcdLine(0, l1); lcdLine(1, l2); }

static Map* findByRemote(NimBLERemoteCharacteristic* rc) {
  for (auto& m : maps) if (m.remote == rc) return &m;
  return nullptr;
}
static int findIdxByLocal(NimBLECharacteristic* lc) {
  for (size_t i = 0; i < maps.size(); i++) if (maps[i].local == lc) return (int)i;
  return -1;
}

// ================= Decodificacao (so para o LCD) =================
static void parseFrame(const uint8_t* d, size_t len) {
  if (len < 20 || d[0] != 0xAA || d[1] != 0x55) return;
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
    if (m.isMain) { if (DUMP_PHONE) printHex("C> ", v.data(), v.size()); }
    else if (DUMP_OTHER) {
      Serial.printf("C[%s]> ", m.chrUuid.toString().c_str());
      printHex("", v.data(), v.size());
    }
    if (!m.remote) return;
    if (m.remoteWriteNR) {
      m.remote->writeValue(v.data(), v.size(), false);   // rapido, seguro no callback
    } else {
      PendingWrite pw;                                    // com resposta -> faz no loop
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
    phoneConnected = true;
    phoneConnT = millis();
    Serial.printf("Celular conectado: %s (handle %u) - %d conectado(s)\n",
                  info.getAddress().toString().c_str(), info.getConnHandle(), phoneCount);
    if (phoneCount < MAX_PHONES) NimBLEDevice::getAdvertising()->start();
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int reason) override {
    if (phoneCount > 0) phoneCount--;
    phoneConnected = (phoneCount > 0);
    for (auto& m : maps) subRemove(m, info.getConnHandle());   // esquece as assinaturas dele
    Serial.printf("Celular desconectou apos %lu ms, motivo %d (0x%X) - restam %d\n",
                  (unsigned long)(millis() - phoneConnT), reason, reason, phoneCount);
    if (mainRemote) NimBLEDevice::getAdvertising()->start();
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    Serial.printf("Celular pediu seguranca: cifrado=%d pareado=%d\n", info.isEncrypted(), info.isBonded());
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo&) override {
    if (mtu < phoneMtu) phoneMtu = mtu;
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
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->stop();
  adv->setName(wheelAdvName);
  for (auto& u : wheelAdvUuids) adv->addServiceUUID(u);
  if (wheelAdvUuids.empty()) adv->addServiceUUID(MAIN_SVC);
  if (!wheelMfgData.empty()) adv->setManufacturerData(wheelMfgData);
  adv->enableScanResponse(true);
  adv->start();
  Serial.printf("Anunciando como \"%s\"\n", wheelAdvName.c_str());
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
    NimBLEDevice::getAdvertising()->stop();
    if (server) for (uint16_t h : server->getPeerDevices()) server->disconnect(h);
    needScan = true;
  }
} clientCB;

static void sendCmd(uint8_t cmd) {
  if (!mainRemote) return;
  uint8_t p[20] = {0xAA, 0x55};
  p[16] = cmd; p[17] = 0x14; p[18] = 0x5A; p[19] = 0x5A;
  mainRemote->writeValue(p, sizeof(p), false);
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
static void drawData() {
  if (millis() - wd.lastUpdate > 2000) { lcdMsg("Conectado", "Sem dados..."); return; }
  char l1[24], l2[24], pwm[5];
  if (wd.pwm >= 0) snprintf(pwm, sizeof(pwm), "%3d", wd.pwm); else strcpy(pwm, " --");
  snprintf(l1, sizeof(l1), "%5.1fkm/h P%s%%%c", wd.speed, pwm, phoneConnected ? '*' : ' ');
  if ((millis() / 3000) % 2 == 0)
    snprintf(l2, sizeof(l2), "%4.1fV %5.1fA %2.0fC", wd.voltage, wd.current, wd.temp);
  else
    snprintf(l2, sizeof(l2), "T%5.2fkm M%4.1f", wd.tripKm, wd.topSpeed);
  lcdLine(0, l1);
  lcdLine(1, l2);
}

// ================= Setup / Loop =================
void setup() {
  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.init();
  lcd.backlight();
  lcdMsg("KS-S16 ponte v3", "Escaneando...");

  writeQ = xQueueCreate(16, sizeof(PendingWrite));

  NimBLEDevice::init("");
  NimBLEDevice::setMTU(247);

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
    lcdMsg("Conectando...", wheelAdvName.c_str());
    if (connectToWheel()) {
      startAdvertisingAsWheel();
      lcdMsg("Roda OK", "Aguard. celular");
    } else {
      lcdMsg("Falha conexao", "Reescaneando...");
      needScan = true;
    }
  }

  if (needScan) {
    needScan = false;
    lcdMsg("KS-S16 ponte v3", "Escaneando...");
    startScan();
  }

  processWrites();
  syncSubscriptions();

  static uint32_t lastDraw = 0;
  if (client && client->isConnected() && millis() - lastDraw > 250) {
    lastDraw = millis();
    drawData();
  }
}
