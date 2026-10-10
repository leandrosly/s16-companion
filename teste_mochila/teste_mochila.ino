/*
  =====================================================================
   TESTE DA MOCHILA iFlight (modulo de LED "Dream" / Happy Lighting)
   Placa: ES3C28P  |  O display conecta no modulo por BLE e controla
  =====================================================================

  O QUE FAZ
    Ate agora a mochila so foi controlada pelo nRF Connect, mandando
    comandos na mao. Aqui o proprio display vira o controle: escaneia,
    acha o modulo "DREAM#...", conecta e manda os comandos pela tela.

  IMPORTANTE
    O modulo aceita UMA conexao por vez. FECHE o app Happy Lighting
    (e o nRF Connect) antes de testar, senao o display nao conecta.

  PROTOCOLO (familia Triones, variante Dream) - descoberto antes:
    Escrita em FFD9 (dentro do servico FFD5), sem resposta.
      Liga / desliga : CC 23 33 / CC 24 33
      Cor RGB        : 56 RR GG BB 00 F0 AA
      Efeito         : 9E 00 [efeito] [veloc] [brilho] 00 E9
                       efeito comeca em 0; veloc 01-FF; brilho 19-FF
    Respostas (notificacao) em FFD4: o handshake do app pede o estado
    com EF 01 77 - aqui a gente escuta FFD4 e mostra o que vier, pra
    finalmente descobrir o formato da resposta.

  NA TELA
    - Status da conexao (procurando / conectado / caiu)
    - LIGA / DESLIGA
    - 6 cores + uma barrinha de brilho
    - EFEITO: escolhe o numero (0..N) e manda; barrinhas de velocidade
      e brilho. Util pra descobrir QUANTOS efeitos existem: vá subindo
      o numero e veja ate onde muda.
    - HANDSHAKE: manda a sequencia do app (EF 01 77 ...) e mostra no
      Serial o que o modulo responde em FFD4.

  OBS: so cliente BLE (nao anuncia nada). Pode compilar com a SUA NimBLE
  2.5.1, com ou sem os patches da ponte: os patches sao um acrescimo e
  nao atrapalham o cliente. Nao precisa trocar de biblioteca.
*/

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <NimBLEDevice.h>

// ---------------------------------------------------------------------
// UUIDs do modulo Dream
// ---------------------------------------------------------------------
static NimBLEUUID SVC_LED("0000ffd5-0000-1000-8000-00805f9b34fb");
static NimBLEUUID CHR_ESCR("0000ffd9-0000-1000-8000-00805f9b34fb");
static NimBLEUUID SVC_NOT("0000ffd0-0000-1000-8000-00805f9b34fb");
static NimBLEUUID CHR_NOT("0000ffd4-0000-1000-8000-00805f9b34fb");

// ---------------------------------------------------------------------
// TELA + TOUCH
// ---------------------------------------------------------------------
const bool INVERTER_TOUCH_X = true;
const bool INVERTER_TOUCH_Y = true;

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel; lgfx::Bus_SPI _bus; lgfx::Light_PWM _light; lgfx::Touch_FT5x06 _touch;
public:
  LGFX() {
    { auto c=_bus.config(); c.spi_host=SPI2_HOST; c.spi_mode=0; c.freq_write=40000000; c.freq_read=16000000;
      c.dma_channel=SPI_DMA_CH_AUTO; c.pin_sclk=12; c.pin_mosi=11; c.pin_miso=13; c.pin_dc=46; _bus.config(c); _panel.setBus(&_bus); }
    { auto c=_panel.config(); c.pin_cs=10; c.pin_rst=-1; c.panel_width=240; c.panel_height=320;
      c.invert=true; c.rgb_order=false; c.readable=true; _panel.config(c); }
    { auto c=_light.config(); c.pin_bl=45; c.freq=44100; c.pwm_channel=7; _light.config(c); _panel.setLight(&_light); }
    { auto c=_touch.config(); c.i2c_port=0; c.i2c_addr=0x38; c.pin_sda=16; c.pin_scl=15; c.pin_int=17; c.pin_rst=18; c.freq=400000;
      if(INVERTER_TOUCH_X){c.x_min=239;c.x_max=0;}else{c.x_min=0;c.x_max=239;}
      if(INVERTER_TOUCH_Y){c.y_min=319;c.y_max=0;}else{c.y_min=0;c.y_max=319;}
      _touch.config(c); _panel.setTouch(&_touch); }
    setPanel(&_panel);
  }
};
LGFX tela;
LGFX_Sprite barSpr(&tela);   // desenha a barra inteira fora da tela e joga pronta (sem piscar)

// ---------------------------------------------------------------------
// BLE cliente
// ---------------------------------------------------------------------
NimBLEClient* cliente = nullptr;
NimBLERemoteCharacteristic* chrEscr = nullptr;
NimBLEAddress enderecoAlvo;
bool temAlvo = false;

enum Estado { PROCURANDO, CONECTANDO, CONECTADO, CAIU };
volatile Estado estado = PROCURANDO;
bool pedirReconectar = false;

void notificacao(NimBLERemoteCharacteristic* c, uint8_t* d, size_t n, bool) {
  Serial.printf("[resp %s] (%u bytes):", c->getUUID().toString().c_str(), n);
  for (size_t i = 0; i < n; i++) Serial.printf(" %02X", d[i]);
  Serial.println();
}

// Procura por um modulo que anuncie o servico FFD5 ou cujo nome tenha "DREAM"
class CallbacksScan : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    bool ehDream = dev->isAdvertisingService(SVC_LED) ||
                   (dev->getName().rfind("DREAM", 0) == 0) ||
                   (dev->getName().rfind("Dream", 0) == 0);
    if (ehDream && !temAlvo) {
      enderecoAlvo = dev->getAddress();
      temAlvo = true;
      Serial.printf("[scan] achei: %s  %s\n", dev->getName().c_str(), enderecoAlvo.toString().c_str());
      NimBLEDevice::getScan()->stop();
    }
  }
};

bool conectar() {
  estado = CONECTANDO;
  if (!cliente) cliente = NimBLEDevice::createClient();
  if (!cliente->connect(enderecoAlvo)) { Serial.println("[ble] falhou conectar"); return false; }

  chrEscr = nullptr;

  // PRIMEIRO descobre tudo (uma unica vez, com refresh). CUIDADO: chamar
  // getServices(true)/getCharacteristics(true) DE NOVO depois disto apaga
  // estes objetos e qualquer ponteiro guardado (ex.: chrEscr) vira lixo ->
  // foi o que causava o crash em qualquer toque. Por isso: descobrir aqui,
  // guardar chrEscr depois, e nunca mais dar refresh.
  for (auto svcN : cliente->getServices(true)) {
    for (auto c : svcN->getCharacteristics(true)) {
      if (c->canNotify() || c->canIndicate()) {
        c->subscribe(c->canNotify(), notificacao);
        Serial.printf("[ble] escutando %s\n", c->getUUID().toString().c_str());
      }
    }
  }

  // Agora, SEM refresh, pega a caracteristica de escrita do cache ja estavel
  NimBLERemoteService* svc = cliente->getService(SVC_LED);
  if (!svc) { Serial.println("[ble] sem servico FFD5"); cliente->disconnect(); return false; }
  chrEscr = svc->getCharacteristic(CHR_ESCR);
  if (!chrEscr) { Serial.println("[ble] sem caracteristica FFD9"); cliente->disconnect(); return false; }

  estado = CONECTADO;
  Serial.println("[ble] conectado");
  return true;
}

// Manda bytes para a mochila. O app repete 3-4x; fazemos 2x por garantia.
void enviar(const uint8_t* d, size_t n) {
  if (estado != CONECTADO || !chrEscr) return;
  for (int r = 0; r < 2; r++) { chrEscr->writeValue(d, n, false); delay(8); }
  Serial.print("[tx]"); for (size_t i = 0; i < n; i++) Serial.printf(" %02X", d[i]); Serial.println();
}

void ligar(bool on)  { uint8_t c[3] = { 0xCC, on ? (uint8_t)0x23 : (uint8_t)0x24, 0x33 }; enviar(c, 3); }
uint8_t brilho = 0xFF;
void corRGB(uint8_t r, uint8_t g, uint8_t b) {
  // aplica o brilho como escala simples (0..255)
  r = r * brilho / 255; g = g * brilho / 255; b = b * brilho / 255;
  uint8_t c[7] = { 0x56, r, g, b, 0x00, 0xF0, 0xAA }; enviar(c, 7);
}
uint8_t efeito = 0, veloc = 0x40;
bool demoEfeito = false;   // percorre os efeitos sozinho, 1 por ~1,5 s
void mandarEfeito() {
  uint8_t c[7] = { 0x9E, 0x00, efeito, veloc, brilho < 0x19 ? (uint8_t)0x19 : brilho, 0x00, 0xE9 };
  enviar(c, 7);
}
void handshake() {
  const uint8_t a[] = { 0xEF, 0x01, 0x77 };
  const uint8_t b[] = { 0xC5, 0xF0, 0x5C };
  const uint8_t c[] = { 0xCF, 0x01, 0x02, 0x03, 0x04, 0xFC };
  const uint8_t d[] = { 0x24, 0x2A, 0x2B, 0x42 };
  Serial.println("[hs] mandando handshake, veja a resposta em [ffd4]");
  enviar(a, 3); enviar(b, 3); enviar(c, 6); enviar(d, 4);
}

// ---------------------------------------------------------------------
// TELA
// ---------------------------------------------------------------------
struct Cor { const char* nome; uint8_t r, g, b; uint16_t tft; };
const Cor CORES[6] = {
  { "verm",  255,0,0,   TFT_RED },
  { "verde", 0,255,0,   TFT_GREEN },
  { "azul",  0,0,255,   TFT_BLUE },
  { "amar",  255,180,0, TFT_YELLOW },
  { "ciano", 0,255,255, TFT_CYAN },
  { "branco",255,255,255, TFT_WHITE },
};

// layout
const int ONOFF_Y = 40, ONOFF_H = 40;
const int COR_Y = 86, COR_H = 36;            // 2 linhas de 3 cores
const int BRILHO_Y = 162, EFEITO_Y = 198, VELOC_Y = 234, BARRA_H = 32;
const int HS_Y = 274, HS_H = 40;
const int TRILHO_X1 = 90, TRILHO_X2 = 232;

void botao(int x, int y, int w, int h, const char* txt, uint16_t cor, uint16_t txtcor = TFT_WHITE) {
  tela.fillRoundRect(x, y, w, h, 7, cor);
  tela.setTextColor(txtcor);
  tela.drawCenterString(txt, x + w / 2, y + h / 2 - 8, &fonts::Font2);
}

void desenharBarra(int y, const char* nome, int valor, int vmax) {
  // Tudo desenhado no sprite (240x22) e jogado de uma vez: nao pisca nem
  // deixa rastro, do mesmo jeito que no teste do MPU.
  const int cy = 11;
  int xv = TRILHO_X1 + (TRILHO_X2 - TRILHO_X1) * valor / vmax;
  barSpr.fillSprite(TFT_BLACK);
  barSpr.setTextColor(TFT_WHITE);
  barSpr.drawString(nome, 4, 2, &fonts::Font2);
  barSpr.fillRect(TRILHO_X1, cy - 3, TRILHO_X2 - TRILHO_X1, 6, TFT_DARKGREY);
  barSpr.fillRect(TRILHO_X1, cy - 3, xv - TRILHO_X1, 6, TFT_CYAN);
  barSpr.fillCircle(xv, cy, 8, TFT_WHITE);
  barSpr.pushSprite(0, y + BARRA_H / 2 - cy);
}

void desenharStatus() {
  const char* t = estado == CONECTADO ? "conectado" : estado == CONECTANDO ? "conectando..." :
                  estado == CAIU ? "caiu - reconectando" : "procurando mochila...";
  uint16_t c = estado == CONECTADO ? TFT_GREEN : estado == CAIU ? TFT_RED : TFT_YELLOW;
  tela.setTextColor(c, TFT_BLACK);
  tela.setTextPadding(236);
  tela.drawString(t, 4, 4, &fonts::Font2);
  tela.setTextPadding(0);
}

void desenharTudo() {
  tela.fillScreen(TFT_BLACK);
  desenharStatus();
  botao(4, ONOFF_Y, 112, ONOFF_H, "LIGA", TFT_DARKGREEN);
  botao(124, ONOFF_Y, 112, ONOFF_H, "DESLIGA", TFT_MAROON);
  for (int i = 0; i < 6; i++) {
    int x = 4 + (i % 3) * 78, y = COR_Y + (i / 3) * (COR_H + 4);
    botao(x, y, 74, COR_H, CORES[i].nome, CORES[i].tft, i == 5 ? TFT_BLACK : TFT_WHITE);
  }
  desenharBarra(BRILHO_Y, "brilho", brilho, 255);
  char e[20]; snprintf(e, sizeof(e), "efeito %d", efeito);
  tela.setTextColor(TFT_WHITE, TFT_BLACK); tela.drawString(e, 4, EFEITO_Y + 8, &fonts::Font2);
  botao(TRILHO_X1, EFEITO_Y, 34, BARRA_H, "-", TFT_NAVY);
  botao(TRILHO_X1 + 40, EFEITO_Y, 34, BARRA_H, "+", TFT_NAVY);
  botao(TRILHO_X1 + 84, EFEITO_Y, 58, BARRA_H, "manda", TFT_PURPLE);
  desenharBarra(VELOC_Y, "veloc", veloc, 255);
  botao(4, HS_Y, 74, HS_H, demoEfeito ? "PARAR" : "DEMO ef", demoEfeito ? TFT_MAROON : TFT_PURPLE);
  botao(82, HS_Y, 72, HS_H, "EF0177", TFT_DARKGREY);
  botao(160, HS_Y, 76, HS_H, "reconect", TFT_NAVY);
}

// ---------------------------------------------------------------------
// TOUCH
// ---------------------------------------------------------------------
bool estavaTocando = false;
int arrastando = 0;   // 1 brilho, 2 veloc

void ajuste(int qual, int x) {
  int v = constrain((x - TRILHO_X1) * 255 / (TRILHO_X2 - TRILHO_X1), 0, 255);
  if (qual == 1) { brilho = v; desenharBarra(BRILHO_Y, "brilho", brilho, 255); }
  else { veloc = v < 1 ? 1 : v; desenharBarra(VELOC_Y, "veloc", veloc, 255); }
}

void toque(int x, int y) {
  if (y >= ONOFF_Y && y < ONOFF_Y + ONOFF_H) { if (x < 120) ligar(true); else ligar(false); return; }
  for (int i = 0; i < 6; i++) {
    int bx = 4 + (i % 3) * 78, by = COR_Y + (i / 3) * (COR_H + 4);
    if (x >= bx && x < bx + 74 && y >= by && y < by + COR_H) { corRGB(CORES[i].r, CORES[i].g, CORES[i].b); return; }
  }
  if (y >= BRILHO_Y && y < BRILHO_Y + BARRA_H && x >= TRILHO_X1) { arrastando = 1; ajuste(1, x); return; }
  if (y >= VELOC_Y && y < VELOC_Y + BARRA_H && x >= TRILHO_X1) { arrastando = 2; ajuste(2, x); return; }
  if (y >= EFEITO_Y && y < EFEITO_Y + BARRA_H) {
    if (x >= TRILHO_X1 && x < TRILHO_X1 + 34) { if (efeito) efeito--; }
    else if (x >= TRILHO_X1 + 40 && x < TRILHO_X1 + 74) { efeito++; }
    else if (x >= TRILHO_X1 + 84) { mandarEfeito(); return; }
    char e[20]; snprintf(e, sizeof(e), "efeito %d ", efeito);
    tela.setTextColor(TFT_WHITE, TFT_BLACK); tela.drawString(e, 4, EFEITO_Y + 8, &fonts::Font2);
    return;
  }
  if (y >= HS_Y && y < HS_Y + HS_H) {
    if (x < 78) { demoEfeito = !demoEfeito; desenharTudo(); }
    else if (x < 158) handshake();
    else pedirReconectar = true;
  }
}

// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Teste da mochila (Dream) ===  feche o Happy Lighting!");

  tela.init();
  tela.setBrightness(200);
  barSpr.createSprite(240, 22);
  desenharTudo();

  NimBLEDevice::init("S16 mochila");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
}

void loop() {
  static uint32_t tStatus = 0, tScan = 0;
  Estado e = estado;

  // Gestao da conexao
  if (e == PROCURANDO && !temAlvo && millis() - tScan > 500) {
    tScan = millis();
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(new CallbacksScan(), false);
    scan->setActiveScan(true);
    scan->getResults(2000, false);      // bloqueia 2 s procurando
  }
  if ((e == PROCURANDO || e == CAIU) && temAlvo) {
    if (!conectar()) { estado = CAIU; temAlvo = false; }
    desenharTudo();
  }
  if (e == CONECTADO && cliente && !cliente->isConnected()) {
    estado = CAIU; temAlvo = false;
    Serial.println("[ble] caiu");
  }

  // reconectar a pedido: desconecta PRIMEIRO (senao o modulo continua preso
  // em nos e nunca mais anuncia, e a busca fica eterna)
  if (pedirReconectar) {
    pedirReconectar = false; demoEfeito = false;
    if (cliente && cliente->isConnected()) cliente->disconnect();
    delay(200);
    temAlvo = false; chrEscr = nullptr; estado = PROCURANDO;
    desenharTudo();
    Serial.println("[ble] reconectar: desconectei, procurando de novo");
  }

  // demo dos efeitos: sobe 1 a cada 1,5 s e mostra o numero grande
  static uint32_t tDemo = 0;
  if (demoEfeito && estado == CONECTADO && millis() - tDemo > 1500) {
    tDemo = millis();
    mandarEfeito();
    char t[16]; snprintf(t, sizeof(t), "ef %d", efeito);
    tela.setTextColor(TFT_YELLOW, TFT_BLACK); tela.setTextPadding(80);   // < TRILHO_X1, nao invade o botao "-"
    tela.drawString(t, 4, EFEITO_Y + 8, &fonts::Font2); tela.setTextPadding(0);
    Serial.printf("[demo] efeito %d\n", efeito);
    efeito++;                         // uint8_t: depois de 255 volta a 0 sozinho
  }

  // touch
  int32_t x, y;
  bool tocando = tela.getTouch(&x, &y);
  if (tocando && !estavaTocando) toque(x, y);
  else if (tocando && arrastando) ajuste(arrastando, x);
  if (!tocando) arrastando = 0;
  estavaTocando = tocando;

  if (millis() - tStatus > 300) { tStatus = millis(); desenharStatus(); }
}
