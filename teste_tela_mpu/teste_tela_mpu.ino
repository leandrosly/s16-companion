/*
  =====================================================================
   TESTE: APAGAR/ACENDER A TELA PELA POSICAO DO PULSO + DORMIR PARADO
   Placa: ES3C28P  |  MPU-6050 no conector I2C (Plano A: IO16 SDA, IO15 SCL)
  =====================================================================

  TRES ESTADOS
    LIGADA   - tela normal.
    APAGADA  - luz de fundo e painel desligados, mas o ESP32 continua
               rodando (na ponte, o BLE com a roda e os apps NAO para).
               Acende ao levantar o pulso ou ao tocar na tela.
    DORMINDO - "deep sleep": quase tudo desliga e o programa para.
               Acorda pelo botao BOOT ou, se o fio INT do MPU estiver
               ligado no IO14, ao mexer o braco. Acordar = reiniciar
               o programa do zero (como apertar RESET).

  DUAS TELAS
    MONITOR - valores ao vivo + botoes CONFIG e DORMIR
    CONFIG  - GRAVAR posicao, AUTO sim/nao, PADRAO (toque 2x: volta os
              valores de fabrica, mantem a posicao gravada) e quatro barrinhas:
                acende abaixo de (graus)   - com marcador do angulo atual
                apaga acima de (graus)     - idem
                sensibilidade do "parado"  - com barra de movimento ao vivo
                dormir apos (segundos; no fim da barra = "nunca")
              Tudo e salvo na memoria (Preferences) ao soltar o dedo.
              Enquanto a tela CONFIG esta aberta ela NAO apaga sozinha
              (mostra "apagaria" em vez disso), para voce poder testar
              os angulos olhando.

  COMO A POSICAO E RECONHECIDA
    Ponha o braco na posicao de OLHAR o display e toque GRAVAR.
    O teste mede o angulo entre a gravidade de agora e a gravada:
      angulo < "acende" durante T_LIGAR   -> acende
      angulo > "apaga"  durante T_APAGAR  -> apaga
    Os dois limites diferentes (histerese) evitam pisca-pisca na divisa.

  "PARADO"
    O giro e a aceleracao passam por uma media (~1/3 s). Assim um toque
    na mesa, digitar ou tossir (pancadas de milissegundos) quase nao
    mexem na media; mexer o braco de verdade mexe. A sensibilidade (1-10)
    escolhe o limite. Tambem ajusta o quanto de movimento acorda a placa
    (so com o fio INT ligado). O giro tem o "zero" corrigido sozinho e a
    aceleracao e comparada com a gravidade lenta, para que o erro de
    fabrica do sensor nao pareca movimento.
    Antes de dormir: 5 s de aviso com uma barra; tocar ou mexer cancela.

  LIGACAO EXTRA (opcional, para acordar mexendo o braco)
    INT do MPU -> IO14 (conector GPIO). Sem esse fio, acorda so pelo BOOT.
*/

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Preferences.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

// ---------------------------------------------------------------------
// AJUSTES FIXOS (os outros estao na tela CONFIG)
// ---------------------------------------------------------------------
const uint32_t T_LIGAR  = 250;     // ms na posicao para acender
const uint32_t T_APAGAR = 1500;    // ms fora da posicao para apagar
const uint32_t T_TOQUE  = 8000;    // ms acesa depois de um toque, mesmo fora da posicao
const uint8_t  BRILHO   = 200;

#define USAR_INT_MPU 1             // 1 = fio INT do MPU no IO14 acorda o ESP32
const gpio_num_t PINO_INT_MPU = GPIO_NUM_14;
const gpio_num_t PINO_BOOT    = GPIO_NUM_0;
const gpio_num_t PINO_LUZ     = GPIO_NUM_45;

// ---------------------------------------------------------------------
// CONFIGURACAO AJUSTAVEL (valores iniciais; depois vem da memoria)
// ---------------------------------------------------------------------
// Valores de fabrica (o botao PADRAO volta para eles)
const float PADRAO_LIGA = 30, PADRAO_APAGA = 50, PADRAO_SENS = 5, PADRAO_DORMIR = 60;
const float DORMIR_NUNCA = 615;   // ultima posicao da barrinha = nunca dorme
const uint32_t T_AVISO = 5000;    // ms de aviso antes de dormir

float angLiga  = PADRAO_LIGA;     // graus
float angApaga = PADRAO_APAGA;    // graus
float sensib   = PADRAO_SENS;     // 1 = so movimento forte ... 10 = qualquer coisinha
float dormirS  = PADRAO_DORMIR;   // segundos parado para dormir (DORMIR_NUNCA = desligado)
bool  autoLigado = true;

// Limites derivados da sensibilidade (recalculados por aplicarSensib)
float   limGiro = 17;    // graus/s (media)
float   limAcel = 0.12f; // g (media da diferenca entre a aceleracao e a gravidade lenta)
uint8_t motThr  = 30;    // limite do detector de movimento do MPU (para acordar)

// Interpolacao "geometrica": cada passo multiplica pelo mesmo fator.
// Fica mais natural que linear para coisas como sensibilidade.
float entre(float a, float b, float t) { return a * powf(b / a, t); }

void aplicarSensib() {
  float t = (sensib - 1) / 9.0f;            // 0..1
  limGiro = entre(40.0f, 1.5f, t);          // nivel 1: 40 graus/s ... nivel 10: 1,5
  limAcel = entre(0.30f, 0.012f, t);        // nivel 1: 0,30 g ... nivel 10: 0,012 g
  motThr  = (uint8_t)roundf(entre(60.0f, 5.0f, t));
}

// ---------------------------------------------------------------------
// TELA + TOUCH (mesma configuracao da ponte)
// ---------------------------------------------------------------------
const bool INVERTER_TOUCH_X = true;
const bool INVERTER_TOUCH_Y = true;
const int  I2C_PORTA = 0;

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel;
  lgfx::Bus_SPI       _bus;
  lgfx::Light_PWM     _light;
  lgfx::Touch_FT5x06  _touch;
public:
  LGFX() {
    { auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST; cfg.spi_mode = 0;
      cfg.freq_write = 40000000; cfg.freq_read = 16000000;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 12; cfg.pin_mosi = 11; cfg.pin_miso = 13; cfg.pin_dc = 46;
      _bus.config(cfg); _panel.setBus(&_bus); }
    { auto cfg = _panel.config();
      cfg.pin_cs = 10; cfg.pin_rst = -1;
      cfg.panel_width = 240; cfg.panel_height = 320;
      cfg.invert = true; cfg.rgb_order = false; cfg.readable = true;
      _panel.config(cfg); }
    { auto cfg = _light.config();
      cfg.pin_bl = PINO_LUZ; cfg.freq = 44100; cfg.pwm_channel = 7;
      _light.config(cfg); _panel.setLight(&_light); }
    { auto cfg = _touch.config();
      cfg.i2c_port = I2C_PORTA; cfg.i2c_addr = 0x38;
      cfg.pin_sda = 16; cfg.pin_scl = 15;
      cfg.pin_int = 17; cfg.pin_rst = 18; cfg.freq = 400000;
      if (INVERTER_TOUCH_X) { cfg.x_min = 239; cfg.x_max = 0; } else { cfg.x_min = 0; cfg.x_max = 239; }
      if (INVERTER_TOUCH_Y) { cfg.y_min = 319; cfg.y_max = 0; } else { cfg.y_min = 0; cfg.y_max = 319; }
      _touch.config(cfg); _panel.setTouch(&_touch); }
    setPanel(&_panel);
  }
};

LGFX tela;
LGFX_Sprite faixa(&tela);   // desenha cada barrinha fora da tela e joga pronta (sem piscar)
Preferences prefs;

// ---------------------------------------------------------------------
// I2C (Plano A: funcoes da propria LovyanGFX, porta 0)
// ---------------------------------------------------------------------
const uint32_t FREQ_I2C = 400000;
const uint8_t  END_MPU = 0x68;
const uint8_t  END_TOUCH = 0x38;

bool i2cLer(uint8_t end, uint8_t reg, uint8_t* d, size_t n) {
  return lgfx::i2c::readRegister(I2C_PORTA, end, reg, d, n, FREQ_I2C).has_value();
}
bool i2cEscrever(uint8_t end, uint8_t reg, uint8_t v) {
  return lgfx::i2c::writeRegister8(I2C_PORTA, end, reg, v, 0, FREQ_I2C).has_value();
}

// ---------------------------------------------------------------------
// MPU-6050
// ---------------------------------------------------------------------
const float LSB_ACEL = 8192.0f;   // +-4 g
const float LSB_GIRO = 65.5f;     // +-500 graus/s

bool  mpuOk = false;
float ax, ay, az, gx, gy, gz;
float fx = 0, fy = 0, fz = 1;     // gravidade filtrada
float movGiro = 0, movAcel = 0;   // medias do movimento
float zgx = 0, zgy = 0, zgz = 0;  // "zero" do giro: o que ele marca parado (cada chip tem o seu)
float sx = 0, sy = 0, sz = 1;     // gravidade LENTA (media de ~1 s)

bool mpuIniciar() {
  if (!i2cEscrever(END_MPU, 0x6B, 0x80)) return false;   // reset
  delay(100);
  if (!i2cEscrever(END_MPU, 0x6B, 0x01)) return false;   // acorda
  i2cEscrever(END_MPU, 0x6C, 0x00);
  i2cEscrever(END_MPU, 0x1A, 0x03);
  i2cEscrever(END_MPU, 0x1B, 0x08);
  i2cEscrever(END_MPU, 0x1C, 0x08);
  i2cEscrever(END_MPU, 0x38, 0x00);
  return true;
}

bool mpuLer() {
  uint8_t b[14];
  if (!i2cLer(END_MPU, 0x3B, b, 14)) return false;
  auto v = [&](int i) { return (int16_t)((b[i] << 8) | b[i + 1]); };
  ax = v(0) / LSB_ACEL; ay = v(2) / LSB_ACEL; az = v(4) / LSB_ACEL;
  gx = v(8) / LSB_GIRO; gy = v(10) / LSB_GIRO; gz = v(12) / LSB_GIRO;
  fx += 0.1f * (ax - fx); fy += 0.1f * (ay - fy); fz += 0.1f * (az - fz);
  // ZERO DO GIRO: parado, o giro deveria marcar 0, mas cada chip marca
  // alguns graus/s (o "bias"). Era isso que deixava o nivel 7+ sempre verde.
  // Enquanto o giro e pequeno (< 8 graus/s em cada eixo), o zero vai sendo
  // ajustado devagar (~5 s). Movimento de verdade passa de 8 e nao entra.
  gx -= zgx; gy -= zgy; gz -= zgz;
  if (fabsf(gx) < 8 && fabsf(gy) < 8 && fabsf(gz) < 8) {
    zgx += 0.002f * gx; zgy += 0.002f * gy; zgz += 0.002f * gz;
  }
  // ACELERACAO: antes era |acel| - 1 g, mas o acelerometro tambem tem erro
  // de fabrica (parado marca 0,97 ou 1,03 g). Agora compara a aceleracao com
  // a gravidade "lenta": o erro fixo aparece nas duas e se cancela.
  sx += 0.01f * (ax - sx); sy += 0.01f * (ay - sy); sz += 0.01f * (az - sz);
  float dx = ax - sx, dy = ay - sy, dz = az - sz;
  float dAcel = sqrtf(dx * dx + dy * dy + dz * dz);
  float giro = sqrtf(gx * gx + gy * gy + gz * gz);
  // Medias do movimento: a 100 leituras/s, peso 3% = "memoria" de ~1/3 s.
  // Uma pancada de 20 ms quase nao aparece; 1 s mexendo o braco aparece inteiro.
  movGiro += 0.03f * (giro - movGiro);
  movAcel += 0.03f * (dAcel - movAcel);
  return true;
}

// Quanto de movimento ha, em relacao ao limite: >= 1 conta como "mexendo"
float nivelMovimento() { return max(movGiro / limGiro, movAcel / limAcel); }

void mpuModoVigia() {
  i2cEscrever(END_MPU, 0x1C, 0x09);   // +-4 g + passa-alta 5 Hz
  i2cEscrever(END_MPU, 0x1F, motThr); // MOT_THR (vem da sensibilidade)
  i2cEscrever(END_MPU, 0x20, 1);      // MOT_DUR
  i2cEscrever(END_MPU, 0x37, 0x20);   // INT ativo em 1, travado
  i2cEscrever(END_MPU, 0x38, 0x40);   // so deteccao de movimento
  delay(50);
  uint8_t st; i2cLer(END_MPU, 0x3A, &st, 1);
  i2cEscrever(END_MPU, 0x6C, 0x47);   // acorda a 5 Hz, giro em espera
  i2cEscrever(END_MPU, 0x6B, 0x28);   // modo ciclo
}

// ---------------------------------------------------------------------
// MEMORIA (Preferences)
// ---------------------------------------------------------------------
bool  temRef = false;
float rx, ry, rz;

void carregarConfig() {
  prefs.begin("s16mpu", true);
  temRef = prefs.getBool("tem", false);
  rx = prefs.getFloat("rx", 0); ry = prefs.getFloat("ry", 0); rz = prefs.getFloat("rz", 1);
  angLiga    = prefs.getFloat("liga", angLiga);
  angApaga   = prefs.getFloat("apaga", angApaga);
  sensib     = prefs.getFloat("sens", sensib);
  dormirS    = prefs.getFloat("dormir", dormirS);
  autoLigado = prefs.getBool("auto", autoLigado);
  prefs.end();
  aplicarSensib();
}

void salvarConfig() {
  prefs.begin("s16mpu", false);
  prefs.putFloat("liga", angLiga);
  prefs.putFloat("apaga", angApaga);
  prefs.putFloat("sens", sensib);
  prefs.putFloat("dormir", dormirS);
  prefs.putBool("auto", autoLigado);
  prefs.end();
  Serial.printf("[cfg] acende <%.0f  apaga >%.0f  sensib %.0f (giro %.1f, acel %.3f, mot %u)  dormir %.0fs  auto %s\n",
                angLiga, angApaga, sensib, limGiro, limAcel, motThr, dormirS, autoLigado ? "sim" : "nao");
}

// Volta as barrinhas e o AUTO para os valores de fabrica.
// A posicao gravada fica (ela depende de como o MPU esta montado).
void voltarPadrao() {
  angLiga = PADRAO_LIGA; angApaga = PADRAO_APAGA; sensib = PADRAO_SENS; dormirS = PADRAO_DORMIR;
  autoLigado = true;
  aplicarSensib();
  salvarConfig();
  Serial.println("[cfg] valores de fabrica");
}

void gravarRef() {
  float m = sqrtf(fx * fx + fy * fy + fz * fz);
  if (m < 0.5f) return;
  rx = fx / m; ry = fy / m; rz = fz / m; temRef = true;
  prefs.begin("s16mpu", false);
  prefs.putBool("tem", true);
  prefs.putFloat("rx", rx); prefs.putFloat("ry", ry); prefs.putFloat("rz", rz);
  prefs.end();
  Serial.printf("[ref] gravada: %.2f %.2f %.2f\n", rx, ry, rz);
}

float anguloAteRef() {
  float m = sqrtf(fx * fx + fy * fy + fz * fz);
  if (m < 0.1f) return 180;
  float c = constrain((fx * rx + fy * ry + fz * rz) / m, -1.0f, 1.0f);
  return acosf(c) * RAD_TO_DEG;
}

// ---------------------------------------------------------------------
// ESTADO
// ---------------------------------------------------------------------
enum Estado { LIGADA, APAGADA };
enum Pagina { MONITOR, CONFIG };
Estado estado = LIGADA;
Pagina pagina = MONITOR;

uint32_t desdeNaPosicao = 0, desdeForaPosicao = 0;
uint32_t ultimoToque = 0, ultimoMovimento = 0;
uint32_t contaApagou = 0, contaAcendeu = 0;
float    angulo = 0;

void acender(const char* motivo) {
  if (estado == LIGADA) return;
  tela.wakeup();
  estado = LIGADA; contaAcendeu++;
  Serial.printf("[tela] ACENDE (%s)\n", motivo);
}
void apagar(const char* motivo) {
  if (estado == APAGADA) return;
  tela.sleep();
  estado = APAGADA; contaApagou++;
  Serial.printf("[tela] APAGA (%s)\n", motivo);
}

// ---------------------------------------------------------------------
// DEEP SLEEP
// ---------------------------------------------------------------------
void dormir(const char* motivo) {
  Serial.printf("[sono] dormindo (%s)\n", motivo);
  if (estado == APAGADA) tela.wakeup();
  tela.fillScreen(TFT_BLACK);
  tela.setTextColor(TFT_DARKGREY);
  tela.drawCenterString("dormindo...", 120, 150, &fonts::Font4);
  delay(600);

  if (mpuOk) mpuModoVigia();
  i2cEscrever(END_TOUCH, 0xA5, 0x03);   // touch em hibernacao
  tela.sleep();

  gpio_reset_pin(PINO_LUZ);              // luz de fundo presa em 0 durante o sono
  gpio_set_direction(PINO_LUZ, GPIO_MODE_OUTPUT);
  gpio_set_level(PINO_LUZ, 0);
  gpio_hold_en(PINO_LUZ);
  gpio_deep_sleep_hold_en();

  rtc_gpio_pullup_en(PINO_BOOT);
  esp_sleep_enable_ext0_wakeup(PINO_BOOT, 0);
#if USAR_INT_MPU
  rtc_gpio_pulldown_en(PINO_INT_MPU);
  rtc_gpio_pullup_dis(PINO_INT_MPU);
  esp_sleep_enable_ext1_wakeup(1ULL << PINO_INT_MPU, ESP_EXT1_WAKEUP_ANY_HIGH);
#endif
  Serial.flush();
  esp_deep_sleep_start();
}

const char* motivoAcordou() {
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT0: return "botao BOOT";
    case ESP_SLEEP_WAKEUP_EXT1: return "movimento (INT do MPU)";
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "ligou/reset";
    default: return "outro";
  }
}
String textoAcordou;

// ---------------------------------------------------------------------
// DESENHO: utilidades
// ---------------------------------------------------------------------
void botao(int x, int y, int w, int h, const char* txt, uint16_t cor) {
  tela.fillRoundRect(x, y, w, h, 8, cor);
  tela.setTextColor(TFT_WHITE);
  tela.drawCenterString(txt, x + w / 2, y + h / 2 - 8, &fonts::Font2);
}

void linha(int y, uint16_t cor, const char* fmt, ...) {
  char buf[48];
  va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
  tela.setTextColor(cor, TFT_BLACK);
  tela.setTextPadding(236);
  tela.drawString(buf, 4, y, &fonts::Font2);
  tela.setTextPadding(0);
}

// ---------------------------------------------------------------------
// BARRINHAS (sliders)
// ---------------------------------------------------------------------
// Cada barrinha aponta para uma das variaveis de configuracao. Usamos o
// numero da barrinha (0..3) nas funcoes, em vez de passar a struct, para
// nao cair na pegadinha dos prototipos automaticos do Arduino.
struct Barra {
  const char* nome;
  int   y;              // topo da faixa (altura ALT_BARRA)
  float minimo, maximo, passo;
  float* valor;
  const char* unidade;
};
const int ALT_BARRA = 44;
const int TRILHO_X1 = 16, TRILHO_X2 = 224;   // onde o trilho comeca e termina

const int B_LIGA = 0, B_APAGA = 1, B_SENS = 2, B_DORMIR = 3, N_BARRAS = 4;
Barra barras[N_BARRAS] = {
  { "Acende abaixo de",   62, 5,  80, 1,  &angLiga,  "graus" },
  { "Apaga acima de",    106, 10, 90, 1,  &angApaga, "graus" },
  { "Sensibilidade",     150, 1,  10, 1,  &sensib,   ""      },
  { "Dormir apos",       204, 15, DORMIR_NUNCA, 15, &dormirS, "s" },
};

int posicaoNoTrilho(int i, float v) {
  const Barra& b = barras[i];
  return TRILHO_X1 + (int)((v - b.minimo) / (b.maximo - b.minimo) * (TRILHO_X2 - TRILHO_X1));
}

void desenharBarra(int i) {
  const Barra& b = barras[i];
  faixa.fillSprite(TFT_BLACK);
  faixa.setTextColor(TFT_WHITE);
  faixa.drawString(b.nome, 4, 0, &fonts::Font2);
  char txt[16];
  if (i == B_DORMIR && *b.valor >= DORMIR_NUNCA) snprintf(txt, sizeof(txt), "nunca");
  else snprintf(txt, sizeof(txt), "%.0f %s", *b.valor, b.unidade);
  faixa.setTextColor(TFT_CYAN);
  faixa.drawRightString(txt, 236, 0, &fonts::Font2);

  const int yt = 28;                                  // altura do trilho dentro da faixa
  int xv = posicaoNoTrilho(i, *b.valor);
  faixa.fillRoundRect(TRILHO_X1, yt - 3, TRILHO_X2 - TRILHO_X1, 6, 3, TFT_DARKGREY);
  faixa.fillRoundRect(TRILHO_X1, yt - 3, xv - TRILHO_X1, 6, 3, TFT_CYAN);

  // Marcador ao vivo nas barras de angulo: onde o braco esta AGORA
  if ((i == B_LIGA || i == B_APAGA) && temRef && mpuOk) {
    float a = constrain(angulo, b.minimo, b.maximo);
    int xa = posicaoNoTrilho(i, a);
    faixa.fillTriangle(xa - 5, yt - 14, xa + 5, yt - 14, xa, yt - 6, TFT_YELLOW);
  }
  faixa.fillCircle(xv, yt, 9, TFT_WHITE);
  faixa.pushSprite(0, b.y);
}

// Movimento ao vivo embaixo da barra de sensibilidade:
// a linha branca no meio e o limite; passou dela = "mexendo".
void desenharMedidorMovimento() {
  const int y = barras[B_SENS].y + ALT_BARRA + 2, x1 = TRILHO_X1, w = TRILHO_X2 - TRILHO_X1;
  float n = constrain(nivelMovimento() / 2.0f, 0.0f, 1.0f);   // 0..2x o limite
  bool mexendo = nivelMovimento() >= 1.0f;
  tela.fillRect(x1, y, w, 6, TFT_DARKGREY);
  tela.fillRect(x1, y, (int)(n * w), 6, mexendo ? TFT_GREEN : TFT_YELLOW);
  tela.drawFastVLine(x1 + w / 2, y - 2, 10, TFT_WHITE);
}

// ---------------------------------------------------------------------
// PAGINAS
// ---------------------------------------------------------------------
// Botoes de baixo (as duas paginas usam a mesma faixa)
const int BTN_Y = 266, BTN_H = 50, BTN_W = 116, BTN_X2 = 124;
// Botoes de cima da pagina CONFIG
const int CB_Y = 22, CB_H = 38, CB_W = 76, CB_X2 = 82, CB_X3 = 164;
uint32_t confirmaPadrao = 0;   // hora do 1o toque em PADRAO (0 = nao esta pedindo confirmacao)

void desenharPagina() {
  tela.fillScreen(TFT_BLACK);
  tela.setTextColor(TFT_CYAN, TFT_BLACK);
  if (pagina == MONITOR) {
    tela.drawString("Tela x posicao do pulso", 4, 2, &fonts::Font2);
    tela.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tela.drawString("acordou: " + textoAcordou, 4, 20, &fonts::Font2);
    tela.drawFastHLine(0, 38, 240, TFT_DARKGREY);
    botao(0, BTN_Y, BTN_W, BTN_H, "CONFIG", TFT_NAVY);
  } else {
    tela.drawString("Configuracao", 4, 2, &fonts::Font2);
    botao(0, CB_Y, CB_W, CB_H, "GRAVAR", temRef ? TFT_NAVY : TFT_ORANGE);
    botao(CB_X2, CB_Y, CB_W, CB_H, autoLigado ? "AUTO sim" : "AUTO nao",
          autoLigado ? TFT_DARKGREEN : TFT_MAROON);
    if (confirmaPadrao) botao(CB_X3, CB_Y, CB_W, CB_H, "certeza?", TFT_RED);
    else botao(CB_X3, CB_Y, CB_W, CB_H, "PADRAO", TFT_DARKGREY);
    for (int i = 0; i < N_BARRAS; i++) desenharBarra(i);
    botao(0, BTN_Y, BTN_W, BTN_H, "VOLTAR", TFT_NAVY);
  }
  botao(BTN_X2, BTN_Y, BTN_W, BTN_H, "DORMIR", TFT_PURPLE);
}

// Tela de aviso: barra que encolhe nos ultimos 5 s
uint32_t avisoDormir = 0;   // hora em que o aviso comecou (0 = sem aviso)
void desenharAviso(uint32_t passou) {
  if (passou == 0) {
    tela.fillScreen(TFT_BLACK);
    tela.setTextColor(TFT_YELLOW);
    tela.drawCenterString("Vou dormir...", 120, 110, &fonts::Font4);
    tela.setTextColor(TFT_WHITE);
    tela.drawCenterString("toque na tela ou mexa o braco", 120, 145, &fonts::Font2);
    tela.drawCenterString("para continuar acordado", 120, 163, &fonts::Font2);
  }
  float resta = 1.0f - min(1.0f, passou / (float)T_AVISO);
  int w = (int)(200 * resta);
  tela.fillRect(20, 200, 200, 14, TFT_DARKGREY);
  tela.fillRect(20, 200, w, 14, TFT_YELLOW);
}

void atualizarMonitor(uint32_t agora) {
  if (!mpuOk) linha(44, TFT_RED, "MPU nao responde");
  else if (!temRef) linha(44, TFT_YELLOW, "Sem posicao: CONFIG > GRAVAR");
  else linha(44, angulo < angLiga ? TFT_GREEN : angulo > angApaga ? TFT_RED : TFT_YELLOW,
             "angulo ate a posicao: %5.1f", angulo);
  linha(64,  TFT_WHITE,  "acende <%.0f  apaga >%.0f  auto %s", angLiga, angApaga, autoLigado ? "sim" : "nao");
  linha(84,  TFT_ORANGE, "giro (media) %5.2f / %.2f", movGiro, limGiro);
  linha(104, TFT_ORANGE, "acel (media) %5.3f / %.3f", movAcel, limAcel);
  bool mexendo = nivelMovimento() >= 1.0f;
  linha(124, mexendo ? TFT_GREEN : TFT_YELLOW, mexendo ? "em movimento" : "PARADO");
  uint32_t parou = agora - ultimoMovimento, limite = (uint32_t)dormirS * 1000;
  if (dormirS >= DORMIR_NUNCA) linha(144, TFT_WHITE, "dormir: nunca");
  else linha(144, TFT_WHITE, "dorme em %lu s", parou >= limite ? 0 : (limite - parou) / 1000);
  linha(164, TFT_DARKGREY, "apagou %lu  acendeu %lu", contaApagou, contaAcendeu);
  linha(184, TFT_DARKGREY, "sensibilidade %.0f  (acordar: %u)", sensib, motThr);
  linha(204, TFT_DARKGREY, "zero giro %5.2f %5.2f %5.2f", zgx, zgy, zgz);
}

void atualizarConfig() {
  desenharBarra(B_LIGA);
  desenharBarra(B_APAGA);
  desenharMedidorMovimento();
  // Como a tela CONFIG nao apaga sozinha, mostra o que aconteceria
  const char* txt = !temRef ? "grave a posicao primeiro" :
                    angulo < angLiga ? "acenderia" : angulo > angApaga ? "apagaria" : "entre os dois (mantem)";
  tela.setTextColor(TFT_YELLOW, TFT_BLACK);
  tela.setTextPadding(236);
  tela.drawString(txt, 4, 249, &fonts::Font2);
  tela.setTextPadding(0);
}

// ---------------------------------------------------------------------
// TOUCH
// ---------------------------------------------------------------------
bool estavaTocando = false;
bool toqueSoParaAcender = false;
int  arrastando = -1;              // qual barrinha esta sendo arrastada

void ajustarBarra(int i, int x) {
  Barra& b = barras[i];
  float t = constrain((float)(x - TRILHO_X1) / (TRILHO_X2 - TRILHO_X1), 0.0f, 1.0f);
  float v = b.minimo + t * (b.maximo - b.minimo);
  v = roundf(v / b.passo) * b.passo;           // encaixa no passo (1 grau, 15 s...)
  v = constrain(v, b.minimo, b.maximo);
  if (v == *b.valor) return;
  *b.valor = v;
  // Mantem pelo menos 5 graus de histerese: um empurra o outro
  if (i == B_LIGA && angApaga < angLiga + 5) { angApaga = angLiga + 5; desenharBarra(B_APAGA); }
  if (i == B_APAGA && angLiga > angApaga - 5) { angLiga = angApaga - 5; desenharBarra(B_LIGA); }
  if (i == B_SENS) aplicarSensib();
  desenharBarra(i);
}

void toqueComecou(int x, int y) {
  if (y >= BTN_Y) {                                   // faixa de baixo
    if (x >= BTN_X2) { dormir("botao DORMIR"); return; }
    pagina = (pagina == MONITOR) ? CONFIG : MONITOR;
    desenharPagina();
    return;
  }
  if (pagina != CONFIG) return;
  if (y >= CB_Y && y < CB_Y + CB_H) {                 // GRAVAR / AUTO / PADRAO
    if (x < CB_X2) gravarRef();
    else if (x < CB_X3) { autoLigado = !autoLigado; salvarConfig(); }
    else if (!confirmaPadrao) confirmaPadrao = millis();   // 1o toque: pede confirmacao
    else { voltarPadrao(); confirmaPadrao = 0; }           // 2o toque: confirma
    desenharPagina();
    return;
  }
  for (int i = 0; i < N_BARRAS; i++) {
    if (y >= barras[i].y && y < barras[i].y + ALT_BARRA) { arrastando = i; ajustarBarra(i, x); return; }
  }
}

void tratarTouch(uint32_t agora) {
  int32_t x, y;
  bool tocando = tela.getTouch(&x, &y);
  if (tocando) ultimoMovimento = agora;            // mexer na tela = em uso
  if (tocando && !estavaTocando) {
    ultimoToque = agora;
    if (estado == APAGADA) { acender("toque"); toqueSoParaAcender = true; }
    else if (!avisoDormir) toqueComecou(x, y);    // na tela de aviso, o toque so cancela
  } else if (tocando && arrastando >= 0) {
    ajustarBarra(arrastando, x);
  }
  if (!tocando) {
    if (arrastando >= 0) { salvarConfig(); arrastando = -1; }
    toqueSoParaAcender = false;
  }
  estavaTocando = tocando;
}

// ---------------------------------------------------------------------
void setup() {
  gpio_hold_dis(PINO_LUZ);
  gpio_deep_sleep_hold_dis();

  Serial.begin(115200);
  delay(500);
  textoAcordou = motivoAcordou();
  Serial.printf("\n=== Teste tela x MPU === acordou por: %s\n", textoAcordou.c_str());

  tela.init();
  tela.setBrightness(BRILHO);
  faixa.createSprite(240, ALT_BARRA);
  carregarConfig();

  mpuOk = mpuIniciar();
  if (mpuOk) {
    for (int i = 0; i < 40; i++) { mpuLer(); delay(5); }
    fx = sx = ax; fy = sy = ay; fz = sz = az;
    // zero inicial do giro: media de 0,5 s (se estiver parado; senao o
    // ajuste automatico corrige em alguns segundos)
    float tx = 0, ty = 0, tz = 0;
    for (int i = 0; i < 50; i++) { mpuLer(); tx += gx; ty += gy; tz += gz; delay(10); }
    tx /= 50; ty /= 50; tz /= 50;
    if (fabsf(tx) < 10 && fabsf(ty) < 10 && fabsf(tz) < 10) { zgx += tx; zgy += ty; zgz += tz; }
    Serial.printf("[mpu] zero do giro: %.2f %.2f %.2f\n", zgx, zgy, zgz);
    movGiro = movAcel = 0;
  }
  Serial.printf("[mpu] %s  |  posicao gravada: %s\n", mpuOk ? "ok" : "NAO RESPONDE", temRef ? "sim" : "nao");
  salvarConfig();   // so para mostrar a configuracao no Serial
  ultimoMovimento = ultimoToque = millis();
  desenharPagina();
}

void loop() {
  static uint32_t tMpu = 0, tTela = 0, tSerial = 0;
  uint32_t agora = millis();      // UMA leitura do relogio por volta (lembra do bug?)

  // 1) MPU a 100 Hz
  if (agora - tMpu >= 10) {
    tMpu = agora;
    if (mpuOk && mpuLer()) {
      if (nivelMovimento() >= 1.0f) ultimoMovimento = agora;
      if (temRef) angulo = anguloAteRef();
    }
  }

  // 2) touch
  tratarTouch(agora);

  // 3) acender/apagar (na tela CONFIG so acende, nunca apaga)
  if (avisoDormir) {
    // durante o aviso de dormir a tela fica acesa
  } else if (autoLigado && temRef && mpuOk) {
    if (angulo < angLiga) { if (!desdeNaPosicao) desdeNaPosicao = agora; } else desdeNaPosicao = 0;
    if (angulo > angApaga) { if (!desdeForaPosicao) desdeForaPosicao = agora; } else desdeForaPosicao = 0;

    if (estado == APAGADA && desdeNaPosicao && agora - desdeNaPosicao >= T_LIGAR)
      acender("pulso levantado");
    if (estado == LIGADA && pagina == MONITOR && desdeForaPosicao &&
        agora - desdeForaPosicao >= T_APAGAR && agora - ultimoToque >= T_TOQUE)
      apagar("pulso abaixado");
  } else if (estado == APAGADA) {
    acender("AUTO desligado");
  }

  // 4) parado demais -> aviso de 5 s -> dorme
  //    Durante o aviso, tocar na tela ou mexer o braco cancela
  //    (os dois atualizam ultimoMovimento, e ai a conta volta a ficar abaixo do limite).
  bool parouDemais = dormirS < DORMIR_NUNCA && agora - ultimoMovimento >= (uint32_t)dormirS * 1000;
  if (parouDemais && !avisoDormir) {
    avisoDormir = agora;
    if (estado == APAGADA) acender("aviso de dormir");
    desenharAviso(0);
    Serial.println("[sono] aviso: dorme em 5 s");
  }
  if (avisoDormir && !parouDemais) {                  // cancelado
    avisoDormir = 0;
    estavaTocando = true;                             // o toque que cancelou nao aperta botao
    desenharPagina();
    Serial.println("[sono] cancelado");
  }
  if (avisoDormir) {
    uint32_t passou = agora - avisoDormir;
    if (passou >= T_AVISO) dormir("parado");
    static uint32_t tAviso = 0;
    if (agora - tAviso >= 50) { tAviso = agora; desenharAviso(passou); }
    return;                                           // durante o aviso, nada mais acontece
  }

  // pedido de confirmacao do PADRAO expira em 3 s
  if (confirmaPadrao && agora - confirmaPadrao > 3000) { confirmaPadrao = 0; if (pagina == CONFIG) desenharPagina(); }

  // 5) tela 5x por segundo (so acesa; nunca no meio de um arrasto)
  if (estado == LIGADA && arrastando < 0 && agora - tTela >= 200) {
    tTela = agora;
    if (pagina == MONITOR) atualizarMonitor(agora); else atualizarConfig();
  }

  // 6) Serial 2x por segundo
  if (agora - tSerial >= 500) {
    tSerial = agora;
    Serial.printf("[%s] ang %5.1f  giro %5.1f/%.1f  acel %.3f/%.3f  mov %.2f\n",
                  estado == LIGADA ? "LIGADA " : "APAGADA", angulo, movGiro, limGiro,
                  movAcel, limAcel, nivelMovimento());
  }
}
