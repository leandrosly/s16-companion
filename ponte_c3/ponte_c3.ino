// VERSAO: c3-202610262023
/*
  =====================================================================
   PONTE C3 - S16 Companion, ETAPA 2: ponte BLE + radio 433 + luz de freio
   Placa: ESP32-C3 Supermini, fixa no monociclo, alimentada pela USB da roda
  =====================================================================

  O QUE FAZ (igual a ponte da v5 2APPS do display, sem tela e sem cartao):
  - Procura a RODA pelo MAC (TARGET_MAC) e conecta.
  - Copia toda a GATT dela e anuncia uma COPIA EXATA (mesmo nome, servicos e
    dados de fabricante). Sem servicos extras -> o WheelLog tambem funciona.
  - Repassa tudo: roda -> quem assinou (por conexao) / celular -> roda.
  - Ate MAX_APPS conexoes ao mesmo tempo (apps do celular e, na etapa 3, o display).

  ATENCAO NO TESTE: a roda aceita UMA conexao. Com o C3 ligado, o display v5
  NAO consegue conectar na roda (e vice-versa). Para testar o C3, deixe o display
  desligado. E o TARGET_MAC evita que o C3 conecte por engano na COPIA anunciada
  pelo display v5 (que tambem se chama KSN-S16P...).

  LED da placa (GPIO8, aceso em nivel BAIXO):
    piscando rapido = procurando a roda | piscando devagar = roda conectada, sem app
    aceso direto    = roda + app(s) conectado(s)

  433 MHz (etapa 2) - tudo o que o display v5 fazia com o radio, agora aqui:
    TX STX882: DATA no GPIO3 (VCC no 5V da USB = mais potencia; ou 3V3)
    RX SRX882S: DATA no GPIO4, VCC e CS no 3V3
    Antenas: 17,3 cm de fio reto (1/4 de onda), longe de metal.
    Controle da buzina (00 9F 1C) -> AA buzina da RODA | 99 pisca esq | CC pisca dir
                                     33 buzina externa | 55 desliga o alarme dela
    Luz de freio pela corrente da roda (FREIO_AUTO) -> lanternas vermelhas.
    !! Durante a transicao, DESLIGUE o TX e o RX 433 do DISPLAY (desconecte os
       modulos): senao os dois respondem ao controle e transmitem ao mesmo tempo.

  PROXIMAS ETAPAS (ver README, secao "C3 na roda"):
    3. Display v6 conecta no C3 como se fosse a roda; canal proprio dentro da
       FFE1 (quadros "AA 55 ... E0" so entre display e C3) -> sem servico extra.

  Bibliotecas: NimBLE-Arduino 2.5.1 COM O PATCH (a mesma pasta do display:
               a biblioteca ja instalada serve para as duas placas)
  Placa: "ESP32C3 Dev Module" | USB CDC On Boot: Enabled
         Partition Scheme: "Huge APP (3MB No OTA/1MB SPIFFS)"
*/

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <vector>
#include <Preferences.h>

static const char* VERSAO = "c3-202610262023";

// ================= Configuracao (muda pelo Serial: "NOME = valor"; salva na flash) =================
static String NAME_FILTER = "KS";
static String TARGET_MAC  = "c5:39:32:38:4c:4b";   // a roda (S16 do Leandro). "-" = usar o NAME_FILTER
static bool DUMP_HEX   = false;            // frames da FFE1 vindos da roda (R>) - muito volume
static bool DUMP_PHONE = true;             // o que os apps mandam (C>)
static bool DUMP_NOVOS = true;             // respostas "incomuns" da roda (R?)
static bool DUMP_OTHER = true;             // trafego nas outras caracteristicas
static bool IGNORA_CANCELAR = true;        // ver a v5: o Android poe 2 apps na mesma conexao
static bool FREIO_AUTO = true;             // luz de freio automatica (lanternas vermelhas ao frear)

struct OpcaoBool  { const char* nome; bool* valor; bool padrao; };
struct OpcaoTexto { const char* nome; String* valor; const char* padrao; };
static OpcaoBool OPCOES_BOOL[] = {
  { "DUMP_HEX",   &DUMP_HEX,   false },
  { "DUMP_PHONE", &DUMP_PHONE, true  },
  { "DUMP_NOVOS", &DUMP_NOVOS, true  },
  { "DUMP_OTHER", &DUMP_OTHER, true  },
  { "IGNORA_CANCELAR", &IGNORA_CANCELAR, true },
  { "FREIO_AUTO", &FREIO_AUTO, true },
};
static OpcaoTexto OPCOES_TEXTO[] = {
  { "NAME_FILTER", &NAME_FILTER, "KS" },
  { "TARGET_MAC",  &TARGET_MAC,  "c5:39:32:38:4c:4b" },
};
static Preferences prefs;

static NimBLEUUID MAIN_SVC("FFE0");
static NimBLEUUID MAIN_CHR("FFE1");

static const int PIN_LED = 8;              // LED azul da Supermini (aceso em LOW)

// ---- mapeamento remoto <-> local ----
static const int MAX_SUBS = 6;
struct Map {
  NimBLEUUID svcUuid;
  NimBLEUUID chrUuid;
  NimBLERemoteCharacteristic* remote = nullptr;
  NimBLECharacteristic* local = nullptr;
  bool remoteWriteNR = false;
  bool isMain = false;
  bool phoneSub = false;
  bool remoteSub = false;
  uint16_t subs[MAX_SUBS] = {BLE_HS_CONN_HANDLE_NONE, BLE_HS_CONN_HANDLE_NONE, BLE_HS_CONN_HANDLE_NONE,
                             BLE_HS_CONN_HANDLE_NONE, BLE_HS_CONN_HANDLE_NONE, BLE_HS_CONN_HANDLE_NONE};
};
static std::vector<Map> maps;

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

// ---- lado roda ----
static const NimBLEAdvertisedDevice* target = nullptr;
static NimBLEClient* client = nullptr;
static NimBLERemoteCharacteristic* mainRemote = nullptr;
static volatile bool doConnect = false;
static volatile bool needScan  = false;
static std::string wheelAdvName;
static std::string wheelMfgData;
static std::vector<NimBLEUUID> wheelAdvUuids;

// ---- lado apps ----
// Orcamento do radio (patch: MAX_CONNECTIONS 6 -> conexoes + anuncios <= 5):
// roda + 3 apps + o anuncio. O anuncio para quando os 3 entram.
static const int MAX_APPS = 3;
static const uint8_t ADV_RODA = 0;
static NimBLEServer* server = nullptr;
static bool serverStarted = false;
static volatile int appConns = 0;
static volatile bool advDirty = true;
static bool advConfigured = false;
static volatile uint16_t phoneMtu = 247;
static uint32_t phoneConnT = 0;

struct PendingWrite { uint8_t idx; uint16_t len; uint8_t data[244]; };
static QueueHandle_t writeQ;

// ---- dados da roda (o C3 usa na etapa 2: luz de freio) ----
struct WheelData {
  float voltage = 0, speed = 0, current = 0;
  int pwm = -1;
  uint32_t lastUpdate = 0;
} wd;

// Ha quanto tempo nao chega dado da roda (copia o lastUpdate ANTES do millis(): o
// callback da NimBLE escreve no lastUpdate a qualquer momento - ver README).
static uint32_t msSemDados() {
  uint32_t t = wd.lastUpdate;
  if (t == 0) return 0xFFFFFFFFUL;
  int32_t d = (int32_t)(millis() - t);
  return d < 0 ? 0 : (uint32_t)d;
}

static const uint8_t PEDE_HORA[20] = {0xAA, 0x55, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xF9, 0x01, 0xB9, 0x78};

// ================= Utilitarios =================
static uint16_t u16(const uint8_t* d, int i) { return d[i] | (d[i + 1] << 8); }
static int16_t  s16(const uint8_t* d, int i) { return (int16_t)u16(d, i); }

static void printHex(const char* prefix, const uint8_t* d, size_t len) {
  Serial.print(prefix);
  for (size_t i = 0; i < len; i++) Serial.printf("%02X ", d[i]);
  Serial.println();
}

static Map* findByRemote(NimBLERemoteCharacteristic* rc) {
  for (auto& m : maps) if (m.remote == rc) return &m;
  return nullptr;
}
static int findIdxByLocal(NimBLECharacteristic* lc) {
  for (size_t i = 0; i < maps.size(); i++) if (maps[i].local == lc) return (int)i;
  return -1;
}

static void parseFrame(const uint8_t* d, size_t len) {
  if (len < 20 || d[0] != 0xAA || d[1] != 0x55) return;
  switch (d[16]) {
    case 0xA9:
      wd.voltage = u16(d, 2) / 100.0f;
      wd.speed   = u16(d, 4) / 100.0f;
      wd.current = s16(d, 10) / 100.0f;
      wd.lastUpdate = millis();
      break;
    case 0xF5:
      wd.pwm = d[15];
      break;
  }
}

// ================= Repasse roda -> apps =================
static void forwardToPhone(Map* m, const uint8_t* d, size_t len) {
  if (appConns <= 0 || !m->phoneSub || !m->local) return;
  size_t maxLen = phoneMtu > 3 ? phoneMtu - 3 : 20;
  for (uint16_t h : m->subs) {
    if (h == BLE_HS_CONN_HANDLE_NONE) continue;
    for (size_t off = 0; off < len; off += maxLen) {
      size_t n = min(maxLen, len - off);
      m->local->notify(d + off, n, h);       // so para quem assinou
    }
  }
}

static void remoteNotifyCB(NimBLERemoteCharacteristic* rc, uint8_t* data, size_t len, bool) {
  Map* m = findByRemote(rc);
  if (!m) return;
  if (m->isMain) {
    if (DUMP_HEX) printHex("R> ", data, len);
    else if (DUMP_NOVOS && len >= 20) {
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

// ================= Servidor (lado apps) =================
class LocalChrCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    int idx = findIdxByLocal(c);
    if (idx < 0) return;
    Map& m = maps[idx];
    NimBLEAttValue v = c->getValue();
    if (m.isMain) { if (DUMP_PHONE) printHex("C> ", v.data(), v.size()); }
    else if (DUMP_OTHER) { Serial.printf("C[%s]> ", m.chrUuid.toString().c_str()); printHex("", v.data(), v.size()); }
    NimBLERemoteCharacteristic* r = m.remote;     // copia local (o ponteiro pode zerar no outro lado)
    if (!r) return;
    if (m.remoteWriteNR) {
      r->writeValue(v.data(), v.size(), false);
    } else {
      static PendingWrite pw;
      pw.idx = idx;
      pw.len = (uint16_t)std::min((size_t)sizeof(pw.data), (size_t)v.size());
      memcpy(pw.data, v.data(), pw.len);
      xQueueSend(writeQ, &pw, 0);
    }
  }
  void onSubscribe(NimBLECharacteristic* c, NimBLEConnInfo& info, uint16_t subValue) override {
    int idx = findIdxByLocal(c);
    if (idx < 0) return;
    if (subValue) subAdd(maps[idx], info.getConnHandle());
    else if (maps[idx].isMain && IGNORA_CANCELAR) {
      Serial.printf("App cancelou %s (handle %u) - ignorado\n", maps[idx].chrUuid.toString().c_str(), info.getConnHandle());
      return;
    } else subRemove(maps[idx], info.getConnHandle());
    Serial.printf("App %s %s\n", subValue ? "assinou" : "cancelou", maps[idx].chrUuid.toString().c_str());
  }
} localChrCB;

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
    if (appConns == 0) phoneMtu = 247;
    appConns++;
    phoneConnT = millis();
    Serial.printf("App conectou: %s (handle %u) - %d conectado(s)\n",
                  info.getAddress().toString().c_str(), info.getConnHandle(), appConns);
    advDirty = true;
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int reason) override {
    if (appConns > 0) appConns--;
    for (auto& m : maps) subRemove(m, info.getConnHandle());
    Serial.printf("App desconectou apos %lu ms, motivo 0x%X - restam %d\n",
                  (unsigned long)(millis() - phoneConnT), reason, appConns);
    advDirty = true;
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo&) override {
    if (mtu < phoneMtu) phoneMtu = mtu;
    Serial.printf("MTU: %u\n", mtu);
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

// Clone em 3 etapas (a NimBLE nao monta o servidor com conexao aberta): grava o modelo,
// desconecta, monta, reconecta e mapeia. Igual ao display.
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
      if (rc->canRead()) { NimBLEAttValue v = rc->readValue(); ci.value.assign((const char*)v.data(), v.size()); }
      Serial.printf("  %s props=0x%02X\n", ci.uuid.toString().c_str(), (unsigned)ci.props);
      for (auto* rd : rc->getDescriptors(true)) {          // so o 0x2901 (ver README)
        if (rd->getUUID() != NimBLEUUID((uint16_t)0x2901)) continue;
        DescInfo di; di.uuid = rd->getUUID();
        NimBLEAttValue dv = rd->readValue();
        di.value.assign((const char*)dv.data(), dv.size());
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
        NimBLEDescriptor* ld = lc->createDescriptor(di.uuid, NIMBLE_PROPERTY::READ, di.value.size() ? di.value.size() : 1);
        ld->setValue((const uint8_t*)di.value.data(), di.value.size());
      }
    }
  }
  server->start();
  serverStarted = true;
  Serial.println("Servidor local montado (copia da roda)");
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
      m.svcUuid = rs->getUUID(); m.chrUuid = rc->getUUID();
      m.remote = rc; m.local = lc;
      m.remoteWriteNR = rc->canWriteNoResponse();
      m.isMain = (rs->getUUID() == MAIN_SVC && rc->getUUID() == MAIN_CHR);
      if (m.isMain) mainRemote = rc;
      maps.push_back(m);
    }
  }
  Serial.printf("Mapeadas %u caracteristicas\n", (unsigned)maps.size());
}

static void startAdvertisingAsWheel() {
  NimBLEDevice::setDeviceName(wheelAdvName);
  NimBLEExtAdvertising* adv = NimBLEDevice::getAdvertising();
  NimBLEExtAdvertisement roda;
  roda.setLegacyAdvertising(true);
  roda.setConnectable(true);
  roda.setScannable(true);
  roda.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  roda.setName(wheelAdvName);
  for (auto& u : wheelAdvUuids) roda.addServiceUUID(u);
  if (wheelAdvUuids.empty()) roda.addServiceUUID(MAIN_SVC);
  adv->setInstanceData(ADV_RODA, roda);
  NimBLEExtAdvertisement resp;
  resp.setLegacyAdvertising(true);
  if (!wheelMfgData.empty()) resp.setManufacturerData(wheelMfgData);
  adv->setScanResponseData(ADV_RODA, resp);
  advConfigured = true;
  advDirty = true;
  Serial.printf("Anunciando como \"%s\"\n", wheelAdvName.c_str());
}

static void updateAdvertising() {
  static uint32_t last = 0;
  if (!advConfigured || (!advDirty && millis() - last < 1000)) return;
  advDirty = false;
  last = millis();
  NimBLEExtAdvertising* adv = NimBLEDevice::getAdvertising();
  bool quero = mainRemote && appConns < MAX_APPS;
  if (quero == adv->isActive(ADV_RODA)) return;
  if (!quero) { adv->stop(ADV_RODA); Serial.println("[anuncio] parado"); return; }
  if (adv->start(ADV_RODA)) Serial.println("[anuncio] ligado");
  else Serial.printf("[anuncio] o controlador RECUSOU ligar (%d apps conectados)\n", appConns);
}

// ================= Cliente (lado roda) =================
class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (target) return;
    std::string name = dev->getName();
    bool porMac = TARGET_MAC.length() && TARGET_MAC != "-";
    bool match = porMac ? dev->getAddress().toString() == std::string(TARGET_MAC.c_str())
                        : (!name.empty() && name.find(NAME_FILTER.c_str()) != std::string::npos);
    if (!match) return;
    target = dev;
    NimBLEDevice::getScan()->stop();
    wheelAdvName = name;
    wheelMfgData = dev->getManufacturerData();
    wheelAdvUuids.clear();
    for (int i = 0; i < dev->getServiceUUIDCount(); i++) wheelAdvUuids.push_back(dev->getServiceUUID(i));
    Serial.printf("Roda encontrada: %s \"%s\"\n", dev->getAddress().toString().c_str(), name.c_str());
    doConnect = true;
  }
} scanCB;

static volatile bool intentionalDisconnect = false;

class ClientCB : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient*, int reason) override {
    mainRemote = nullptr;
    for (auto& m : maps) { m.remote = nullptr; m.remoteSub = false; }
    if (intentionalDisconnect) { intentionalDisconnect = false; return; }
    Serial.printf("Roda desconectou (0x%X)\n", reason);
    NimBLEDevice::getAdvertising()->stop(ADV_RODA);
    advDirty = true;
    if (server) for (uint16_t h : server->getPeerDevices()) server->disconnect(h);   // apps sem dados: derruba
    needScan = true;
  }
} clientCB;

static void sendKs(uint8_t cmd, uint8_t b2, uint8_t b3, uint8_t b4, uint8_t b5) {
  NimBLERemoteCharacteristic* r = mainRemote;
  if (!r) return;
  uint8_t p[20] = {0xAA, 0x55, b2, b3, b4, b5};
  p[16] = cmd; p[17] = 0x14; p[18] = 0x5A; p[19] = 0x5A;
  r->writeValue(p, sizeof(p), false);
  if (DUMP_PHONE) printHex("D> ", p, sizeof(p));     // D> = enviado pelo proprio C3
}
static void sendCmd(uint8_t cmd) { sendKs(cmd, 0, 0, 0, 0); }

static bool connectToWheel() {
  if (!client) { client = NimBLEDevice::createClient(); client->setClientCallbacks(&clientCB, false); }
  if (!client->connect(target)) return false;
  Serial.println("Conectado na roda, descobrindo servicos...");
  if (!client->discoverAttributes()) { Serial.println("Falha na descoberta"); client->disconnect(); return false; }
  if (!serverStarted) {
    recordTemplate();
    intentionalDisconnect = true;
    client->disconnect();
    uint32_t t0 = millis();
    while (client->isConnected() && millis() - t0 < 3000) delay(10);
    delay(300);
    buildServer();
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
  sendCmd(0x9B); delay(100);
  sendCmd(0x63); delay(100);
  mainRemote->writeValue(PEDE_HORA, sizeof(PEDE_HORA), false);
  return true;
}

static void startScan() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->clearResults();
  target = nullptr;
  scan->start(0, false, true);
}

static void syncSubscriptions() {
  for (auto& m : maps) {
    NimBLERemoteCharacteristic* r = m.remote;
    if (m.isMain || !r) continue;
    bool want = appConns > 0 && m.phoneSub;
    if (want && !m.remoteSub) {
      if (r->subscribe(r->canNotify(), remoteNotifyCB)) { m.remoteSub = true; Serial.printf("Assinada na roda: %s\n", m.chrUuid.toString().c_str()); }
    } else if (!want && m.remoteSub) {
      r->unsubscribe();
      m.remoteSub = false;
    }
  }
}

static void processWrites() {
  PendingWrite pw;
  while (xQueueReceive(writeQ, &pw, 0) == pdTRUE) {
    if (pw.idx >= maps.size()) continue;
    NimBLERemoteCharacteristic* r = maps[pw.idx].remote;
    if (r) r->writeValue(pw.data, pw.len, true);
  }
}

// ================= 433 MHz (etapa 2) =================
// Mesmo codigo do display v5 (testado), com os pinos do C3.
static const int      TX433_PIN = 3;
static const int      RX433_PIN = 4;
// Lanternas/piscas: 40 bits [3C 24 06 cmd cmd^05], bit 1 = 990 alto + 330 baixo, 0 = o contrario
static const uint16_t TX433_CURTO = 330, TX433_LONGO = 990, TX433_SILENCIO = 9000;
static const int      TX433_REPET = 12;
static const uint8_t  TX433_IDENT[3] = { 0x3C, 0x24, 0x06 };
static const uint8_t  CMD_PISCA_ESQ = 0x11, CMD_PISCA_DIR = 0x12, CMD_LUZ_OFF = 0x03;
static const uint8_t  CMD_LUZ_LARANJA = 0x13, CMD_LUZ_VERMELHO = 0x23, CMD_LUZ_PISCANDO = 0x33;
static const int      TX433_SIMB = 41;
static rmt_data_t     tx433Simb[TX433_SIMB * TX433_REPET];
// Buzina: 32 bits [00 9F 1x cmd], curto 352 / longo 920 us, 1224 us entre repeticoes
static const uint16_t BUZ_CURTO = 352, BUZ_LONGO = 920, BUZ_INTERVALO = 1224;
static const uint32_t BUZ_EXT_ID  = 0x009F1D;     // buzina externa pareada com esta identidade
static const uint32_t CONTROLE_ID = 0x009F1C;     // o controle (entrada)
static const uint32_t TESTE433_ID = 0x009F1E;     // rajadas de teste (medidor_433)
static bool     tx433Ok = false;
static uint32_t tx433LivreEm = 0;                 // radio livre a partir de (espacamento por tempo)
static volatile bool    tx433Pend = false;  static volatile uint8_t tx433Cmd = 0;
static volatile bool    buzExtPend = false; static volatile uint8_t buzExtCmd = 0;
static bool     tx433Teste = false;
static uint8_t  tx433TesteSeq = 0;

enum { LUZ_OFF = 0, LUZ_LARANJA = 1, LUZ_VERMELHO = 2, LUZ_PISCANDO = 3 };
static const uint8_t LUZ_CMD[4] = { CMD_LUZ_OFF, CMD_LUZ_LARANJA, CMD_LUZ_VERMELHO, CMD_LUZ_PISCANDO };
static const char*   LUZ_NOME[4] = { "apagado", "laranja", "vermelho", "piscando" };
static int  luzBase = LUZ_OFF;            // modo "parado" das lanternas (salvo)
static int  piscaAtivo = 0;               // 0 nenhum, 1 esquerda, 2 direita

static const float    FREIO_LIGA_A = 2.0f, FREIO_DESLIGA_A = 0.8f, FREIO_VEL_MIN = 3.0f;
static const uint32_t FREIO_MIN_MS = 1000;
static bool     freioAtivo = false;
static uint32_t freioTesteAte = 0;

static void tx433Montar(rmt_data_t* s, uint8_t cmd) {
  uint8_t b[5] = { TX433_IDENT[0], TX433_IDENT[1], TX433_IDENT[2], cmd, (uint8_t)(cmd ^ 0x05) };
  int k = 0;
  for (int by = 0; by < 5; by++)
    for (int i = 7; i >= 0; i--) {
      bool um = (b[by] >> i) & 1;
      s[k].level0 = 1; s[k].duration0 = um ? TX433_LONGO : TX433_CURTO;
      s[k].level1 = 0; s[k].duration1 = um ? TX433_CURTO : TX433_LONGO;
      k++;
    }
  s[k].level0 = 1; s[k].duration0 = TX433_CURTO;
  s[k].level1 = 0; s[k].duration1 = TX433_SILENCIO;
}
static void tx433Enviar(uint8_t cmd) { tx433Cmd = cmd; tx433Pend = true; }   // fila de 1 (o ultimo ganha)
static void tx433Loop() {
  if (!tx433Pend || !tx433Ok || millis() < tx433LivreEm) return;
  tx433Pend = false;
  uint8_t cmd = tx433Cmd;
  for (int r = 0; r < TX433_REPET; r++) tx433Montar(&tx433Simb[r * TX433_SIMB], cmd);
  rmtWriteAsync(TX433_PIN, tx433Simb, TX433_SIMB * TX433_REPET);
  tx433LivreEm = millis() + (uint32_t)TX433_REPET * 63 + 60;
  Serial.printf("[433] 3C 24 06 %02X %02X x%d\n", cmd, cmd ^ 0x05, TX433_REPET);
}

// Bits crus no formato da buzina (1 a 10 digitos hexa)
static void tx433Raw(const char* hex, uint16_t curto, uint16_t longo, bool falar) {
  if (curto < 100 || curto > 2000 || longo < 100 || longo > 5000) { Serial.println("433 raw: tempos fora da faixa"); return; }
  int nd = strlen(hex);
  if (nd < 1 || nd > 10) { Serial.println("433 raw: use de 1 a 10 digitos hexa"); return; }
  if (!tx433Ok) { Serial.println("433: sem RMT"); return; }
  if (millis() < tx433LivreEm) { Serial.println("433: radio ocupado, tente de novo"); return; }
  int k = 0;
  for (int r = 0; r < TX433_REPET; r++) {
    for (int d = 0; d < nd; d++) {
      char c = hex[d];
      int val = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
      if (val < 0) { Serial.printf("433 raw: '%c' nao e hexa\n", c); return; }
      for (int i = 3; i >= 0; i--) {
        bool um = (val >> i) & 1;
        tx433Simb[k].level0 = 1; tx433Simb[k].duration0 = um ? longo : curto;
        tx433Simb[k].level1 = 0; tx433Simb[k].duration1 = um ? curto : longo;
        k++;
      }
    }
    tx433Simb[k].level0 = 1; tx433Simb[k].duration0 = curto;
    tx433Simb[k].level1 = 0; tx433Simb[k].duration1 = BUZ_INTERVALO;
    k++;
  }
  rmtWriteAsync(TX433_PIN, tx433Simb, k);
  uint32_t quadroUs = (uint32_t)nd * 4 * (curto + longo) + curto + BUZ_INTERVALO;
  tx433LivreEm = millis() + quadroUs * TX433_REPET / 1000 + 60;
  if (falar) Serial.printf("[433] raw %s x%d\n", hex, TX433_REPET);
}

static void buzExtEnviar(uint8_t cmd) { buzExtCmd = cmd; buzExtPend = true; }
static void buzExtLoop() {
  if (!buzExtPend || !tx433Ok || millis() < tx433LivreEm) return;
  buzExtPend = false;
  char hx[12];
  snprintf(hx, sizeof(hx), "%06lX%02X", (unsigned long)BUZ_EXT_ID, buzExtCmd);
  tx433Raw(hx, BUZ_CURTO, BUZ_LONGO, true);
}
static void tx433TesteLoop() {           // '433 teste': rajadas para o medidor_433
  static uint32_t ultimo = 0;
  if (!tx433Teste || !tx433Ok || tx433Pend || buzExtPend) return;
  if (millis() < tx433LivreEm || millis() - ultimo < 800) return;
  ultimo = millis();
  char hx[12];
  snprintf(hx, sizeof(hx), "%06lX%02X", (unsigned long)TESTE433_ID, tx433TesteSeq++);
  tx433Raw(hx, BUZ_CURTO, BUZ_LONGO, false);
}

// ---- receptor: decodifica o controle da buzina borda a borda ----
static volatile uint32_t rxCodigo = 0, rxContador = 0;
static const uint16_t RX_CURTO_MIN = 200, RX_CURTO_MAX = 600, RX_LONGO_MIN = 700, RX_LONGO_MAX = 1150;
static const uint32_t RX_SINC_MIN = 1000, RX_SINC_MAX = 20000;
void IRAM_ATTR rx433ISR() {
  static uint32_t tAnt = 0, bits = 0, altoAnt = 0;
  static int nb = -1;
  uint32_t agora = micros();
  uint32_t d = agora - tAnt;
  tAnt = agora;
  bool eraAlto = !digitalRead(RX433_PIN);
  if (eraAlto) {
    altoAnt = d;
    bool ok = (d >= RX_CURTO_MIN && d <= RX_CURTO_MAX) || (d >= RX_LONGO_MIN && d <= RX_LONGO_MAX);
    if (!ok) nb = -1;
    return;
  }
  if (d >= RX_SINC_MIN && d <= RX_SINC_MAX) { nb = 0; bits = 0; return; }
  if (nb < 0) return;
  bool hCurto = altoAnt <= RX_CURTO_MAX, lCurto = d >= RX_CURTO_MIN && d <= RX_CURTO_MAX;
  bool lLongo = d >= RX_LONGO_MIN && d <= RX_LONGO_MAX;
  if (hCurto && lLongo)       bits = bits << 1;
  else if (!hCurto && lCurto) bits = (bits << 1) | 1;
  else { nb = -1; return; }
  if (++nb == 32) { rxCodigo = bits; rxContador++; nb = -1; }
}

// ---- logica das lanternas: pisca de curva > freio > modo base ----
static void luzAplicarAtual() {
  if (piscaAtivo) return;
  tx433Enviar(freioAtivo ? CMD_LUZ_VERMELHO : LUZ_CMD[luzBase]);
}
static void luzSetBase(int modo) {
  luzBase = modo;
  prefs.begin("c3cfg", false); prefs.putInt("luz_base", luzBase); prefs.end();
  luzAplicarAtual();
  Serial.printf("[luz] base: %s\n", LUZ_NOME[modo]);
}
static void piscaToggle(int lado) {        // apertar de novo volta ao modo base (ou vermelho, freando)
  if (piscaAtivo == lado) { piscaAtivo = 0; luzAplicarAtual(); Serial.println("[luz] pisca off"); }
  else { piscaAtivo = lado; tx433Enviar(lado == 1 ? CMD_PISCA_ESQ : CMD_PISCA_DIR); Serial.printf("[luz] pisca %s\n", lado == 1 ? "esquerda" : "direita"); }
}

static void freioLoop() {
  static uint32_t ultAmostra = 0, ligouEm = 0;
  static int calmas = 0;
  bool novo = freioAtivo;
  if (millis() < freioTesteAte) novo = true;
  else if (!FREIO_AUTO || msSemDados() > 2000) novo = false;
  else if (wd.lastUpdate != ultAmostra) {
    ultAmostra = wd.lastUpdate;
    bool freando = wd.current < -FREIO_LIGA_A && wd.speed > FREIO_VEL_MIN;
    bool solto = wd.current > -FREIO_DESLIGA_A || wd.speed < FREIO_VEL_MIN;
    if (freando) { novo = true; calmas = 0; }
    else if (solto) calmas++;
    else calmas = 0;
    if (freioAtivo && !freando && calmas >= 2 && millis() - ligouEm > FREIO_MIN_MS) novo = false;
  }
  if (novo == freioAtivo) return;
  freioAtivo = novo;
  if (novo) ligouEm = millis(); else freioTesteAte = 0;
  Serial.printf("[freio] %s (%.1f A, %.0f km/h)\n", novo ? "ACENDE" : "apaga", wd.current, wd.speed);
  luzAplicarAtual();
}

// ---- controle 433 -> acoes ----
// Acoes por 433 esperam o controle PARAR de transmitir (senao embaralha no ar); a buzina
// da roda (Bluetooth) sai na hora. 2 quadros iguais = valido; uma vez por aperto.
static bool rx433UsaRadio(uint8_t cmd) { return cmd == 0x99 || cmd == 0xCC || cmd == 0x33 || cmd == 0x55; }
static void rx433Acao(uint32_t cod) {
  uint8_t cmd = cod & 0xFF;
  switch (cmd) {
    case 0xAA: Serial.println("[433 rx] buzina da roda"); sendKs(0x88, 0, 0, 0, 0); break;
    case 0x99: piscaToggle(1); break;
    case 0xCC: piscaToggle(2); break;
    case 0x33: Serial.println("[433 rx] buzina externa"); buzExtEnviar(0xAA); break;
    case 0x55: Serial.println("[433 rx] desliga alarme da buzina externa"); buzExtEnviar(0x33); break;
    default:   Serial.printf("[433 rx] botao %02X sem funcao\n", cmd); break;
  }
}
static const uint32_t RX_SOLTOU_MS = 80;
static void rx433Loop() {
  static uint32_t visto = 0, ultimo = 0, tUltimo = 0, tPrimeiro = 0, pendente = 0;
  static int repet = 0;
  static bool agiu = false;
  if (pendente && millis() - tUltimo > RX_SOLTOU_MS) {
    uint32_t p = pendente; pendente = 0;
    Serial.printf("[433 rx] controle transmitiu %lu ms\n", (unsigned long)(tUltimo - tPrimeiro));
    rx433Acao(p);
  }
  uint32_t c = rxContador;
  if (c == visto) return;
  visto = c;
  uint32_t cod = rxCodigo;
  if ((cod >> 8) != CONTROLE_ID) return;
  if (cod == ultimo && millis() - tUltimo < 300) repet++;
  else { ultimo = cod; repet = 1; agiu = false; tPrimeiro = millis(); }
  tUltimo = millis();
  if (!agiu && repet >= 2) {
    agiu = true;
    if (rx433UsaRadio(cod & 0xFF)) pendente = cod;
    else rx433Acao(cod);
  }
}

static void radio433Setup() {
  tx433Ok = rmtInit(TX433_PIN, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 1000000);   // 1 tick = 1 us
  Serial.printf("433 tx: %s (GPIO%d)\n", tx433Ok ? "ok" : "ERRO ao iniciar o RMT", TX433_PIN);
  pinMode(RX433_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(RX433_PIN), rx433ISR, CHANGE);
  Serial.printf("433 rx: escutando o controle no GPIO%d\n", RX433_PIN);
  prefs.begin("c3cfg", true);
  luzBase = prefs.getInt("luz_base", LUZ_OFF);
  prefs.end();
  if (luzBase != LUZ_OFF) { tx433Enviar(LUZ_CMD[luzBase]); Serial.printf("433: luz base %s\n", LUZ_NOME[luzBase]); }
}

// 2a palavra da linha ("433 e" -> "e"; "433" sozinho -> "")
static String palavra2(const String& s) {
  int i = s.indexOf(' ');
  if (i < 0) return "";
  String r = s.substring(i + 1); r.trim();
  int j = r.indexOf(' ');
  return j < 0 ? r : r.substring(0, j);
}

// ================= Configuracao / console =================
static void carregarConfig() {
  prefs.begin("c3cfg", true);
  for (auto& o : OPCOES_BOOL)  *o.valor = prefs.getBool(o.nome, o.padrao);
  for (auto& o : OPCOES_TEXTO) *o.valor = prefs.getString(o.nome, o.padrao);
  prefs.end();
}
static void mostrarConfig() {
  Serial.println("--- configuracao ---");
  for (auto& o : OPCOES_BOOL)  Serial.printf("  %-16s = %s\n", o.nome, *o.valor ? "true" : "false");
  for (auto& o : OPCOES_TEXTO) Serial.printf("  %-16s = \"%s\"\n", o.nome, o.valor->c_str());
}
static bool comandoConfig(String linha) {
  linha.trim();
  if (linha == "config") { mostrarConfig(); return true; }
  if (linha == "config padrao") {
    prefs.begin("c3cfg", false); prefs.clear(); prefs.end();
    for (auto& o : OPCOES_BOOL)  *o.valor = o.padrao;
    for (auto& o : OPCOES_TEXTO) *o.valor = o.padrao;
    Serial.println("Configuracao de fabrica restaurada");
    mostrarConfig();
    return true;
  }
  linha.replace("=", " ");
  int esp = linha.indexOf(' ');
  String nome = (esp < 0) ? linha : linha.substring(0, esp);
  String valor = (esp < 0) ? "" : linha.substring(esp + 1);
  nome.trim(); valor.trim(); nome.toUpperCase();
  for (auto& o : OPCOES_BOOL) {
    if (nome != o.nome) continue;
    String v = valor; v.toLowerCase();
    bool novo;
    if (v == "true" || v == "1" || v == "on") novo = true;
    else if (v == "false" || v == "0" || v == "off") novo = false;
    else { Serial.printf("%s = %s  (use true/false)\n", o.nome, *o.valor ? "true" : "false"); return true; }
    *o.valor = novo;
    prefs.begin("c3cfg", false); prefs.putBool(o.nome, novo); prefs.end();
    Serial.printf("%s = %s (salvo)\n", o.nome, novo ? "true" : "false");
    return true;
  }
  for (auto& o : OPCOES_TEXTO) {
    if (nome != o.nome) continue;
    valor.replace("\"", "");
    *o.valor = valor;
    prefs.begin("c3cfg", false); prefs.putString(o.nome, valor); prefs.end();
    Serial.printf("%s = \"%s\" (salvo; vale na proxima busca pela roda)\n", o.nome, valor.c_str());
    return true;
  }
  return false;
}

static void handleSerialConsole() {
  static char line[160];
  static int n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') { if (n < (int)sizeof(line) - 1) line[n++] = c; continue; }
    line[n] = 0; n = 0;
    if (comandoConfig(String(line))) continue;
    String orig = String(line); orig.trim();
    if (orig == "freio") { freioTesteAte = millis() + 2500; Serial.println("[freio] teste: 2,5 s"); continue; }
    if (orig.startsWith("433 raw ")) {           // 433 raw <hexa> [curto longo]
      String resto = orig.substring(8); resto.trim();
      int e1 = resto.indexOf(' ');
      String hx = e1 < 0 ? resto : resto.substring(0, e1);
      uint16_t cu = BUZ_CURTO, lo = BUZ_LONGO;
      if (e1 >= 0) { String t = resto.substring(e1 + 1); t.trim(); int e2 = t.indexOf(' ');
                     if (e2 > 0) { cu = t.substring(0, e2).toInt(); lo = t.substring(e2 + 1).toInt(); } }
      tx433Raw(hx.c_str(), cu, lo, true);
      continue;
    }
    if (orig.startsWith("433")) {                // 433 e|d|x|l|v|p|teste
      String a = palavra2(orig);
      if (a == "e") piscaToggle(1);
      else if (a == "d") piscaToggle(2);
      else if (a == "x") luzSetBase(LUZ_OFF);
      else if (a == "l") luzSetBase(LUZ_LARANJA);
      else if (a == "v") luzSetBase(LUZ_VERMELHO);
      else if (a == "p") luzSetBase(LUZ_PISCANDO);
      else if (a == "teste") { tx433Teste = !tx433Teste; Serial.printf("433 teste: %s\n", tx433Teste ? "LIGADO (rajadas 00 9F 1E NN)" : "desligado"); }
      else Serial.printf("433: %s | base=%s pisca=%d freio=%s  (use: 433 e|d|x|l|v|p|teste | 433 raw <hexa>)\n",
                         tx433Ok ? "ok" : "sem RMT", LUZ_NOME[luzBase], piscaAtivo, freioAtivo ? "SIM" : "nao");
      continue;
    }
    if (orig.startsWith("buz")) {                // buz AA (buzina externa), buz 33 (desliga alarme)...
      String a = palavra2(orig);
      if (a.length()) buzExtEnviar((uint8_t)strtoul(a.c_str(), nullptr, 16));
      else Serial.println("use: buz AA (buzina) | 99 vol+ | CC vol- | 55 alarme liga | 33 desliga | 66/FF troca o som");
      continue;
    }
    if (orig == "status") {
      Serial.printf("%s | roda: %s | apps: %d/%d | dados: %s | %.1f km/h %.2f A %.1f V pwm %d\n", VERSAO,
                    mainRemote ? "conectada" : "nao", appConns, MAX_APPS,
                    msSemDados() < 2000 ? "chegando" : "parados", wd.speed, wd.current, wd.voltage, wd.pwm);
      continue;
    }
    char* save;
    char* tok = strtok_r(line, " ", &save);
    if (!tok) continue;
    String nome = tok;
    uint8_t v[24]; int cnt = 0;
    while ((tok = strtok_r(nullptr, " ", &save)) && cnt < 24) v[cnt++] = strtoul(tok, nullptr, 16);
    NimBLERemoteCharacteristic* r = mainRemote;
    uint8_t p[20] = {0xAA, 0x55};
    if (nome == "t" && cnt == 3 && v[1] >= 2 && v[1] <= 15) {
      p[v[1]] = v[2]; p[16] = v[0]; p[17] = 0x14; p[18] = 0x5A; p[19] = 0x5A;
    } else if (nome == "raw" && cnt == 20) {
      memcpy(p, v, 20);
    } else {
      Serial.println("comandos: status | config | config padrao | NOME = valor (ex.: DUMP_HEX = true, FREIO_AUTO = off)");
      Serial.println("          433 e|d (piscas) | 433 x|l|v|p (luz base) | 433 teste | 433 raw <hexa> | buz <cmd> | freio");
      Serial.println("          t <cmd> <pos 2-15> <valor> (hexa, ex.: t 88 2 0 = buzina) | raw <20 bytes hexa>");
      continue;
    }
    if (!r) { Serial.println("Roda nao conectada"); continue; }
    r->writeValue(p, sizeof(p), false);
    printHex("D> ", p, sizeof(p));
  }
}

static void ledLoop() {
  bool roda = mainRemote != nullptr;
  uint32_t t = millis();
  bool aceso;
  if (!roda)              aceso = (t / 100) % 2;          // procurando: rapido
  else if (appConns == 0) aceso = (t / 700) % 2;          // roda ok, ninguem conectado: devagar
  else                    aceso = true;                   // roda + apps: aceso direto
  digitalWrite(PIN_LED, aceso ? LOW : HIGH);              // LED aceso em LOW
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);
  delay(1500);                                            // tempo do USB CDC aparecer
  Serial.printf("\n=== S16 Companion - ponte C3 %s ===\n", VERSAO);
  carregarConfig();
  Serial.printf("Procurando a roda: %s\n", (TARGET_MAC.length() && TARGET_MAC != "-") ? TARGET_MAC.c_str() : NAME_FILTER.c_str());
  writeQ = xQueueCreate(16, sizeof(PendingWrite));
  NimBLEDevice::init("");
  NimBLEDevice::setMTU(247);
  server = NimBLEDevice::createServer();
  server->setCallbacks(&serverCB, false);
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCB, false);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  radio433Setup();
  startScan();
}

void loop() {
  if (doConnect) {
    doConnect = false;
    if (connectToWheel()) { startAdvertisingAsWheel(); Serial.println("Ponte pronta"); }
    else { Serial.println("Falha na conexao, procurando de novo"); needScan = true; }
  }
  if (needScan) { needScan = false; startScan(); }
  processWrites();
  syncSubscriptions();
  handleSerialConsole();
  updateAdvertising();
  rx433Loop();                 // controle 433
  freioLoop();                 // luz de freio pela corrente da roda
  buzExtLoop();                // comandos da buzina externa
  tx433Loop();                 // comandos das lanternas
  tx433TesteLoop();            // '433 teste'
  ledLoop();
  delay(1);
}
