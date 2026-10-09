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

  COMO A POSICAO E RECONHECIDA
    Nao importa como o MPU foi montado: voce ensina a posicao.
    Ponha o braco na posicao de OLHAR o display e toque GRAVAR.
    Depois disso o teste mede o angulo entre a gravidade de agora e a
    gravidade gravada:
      angulo < ANG_LIGA durante T_LIGAR       -> acende
      angulo > ANG_APAGA durante T_APAGAR     -> apaga
    Os dois limites diferentes (histerese) evitam pisca-pisca na divisa.
    A posicao gravada fica na memoria (Preferences), sobrevive a reinicio.

  "PARADO"
    Giro abaixo de LIM_GIRO e aceleracao total perto de 1 g (so a
    gravidade). Parado por T_DORMIR -> DORMINDO. No teste T_DORMIR e curto
    (60 s) para nao ter que esperar; na ponte seria algo como 10 min e so
    com a roda desconectada.

  LIGACAO EXTRA (opcional, para acordar mexendo o braco)
    INT do MPU -> IO14 (conector GPIO). Sem esse fio, acorda so pelo BOOT.
    O acordar pelo movimento usa o "motion detection" do proprio MPU, que
    continua ligado em modo economico enquanto o ESP32 dorme.
    Clones do MPU-6050 variam nisso: se nao acordar, nao e erro seu.

  BOTOES NA TELA
    GRAVAR  - grava a posicao atual como "olhando"
    AUTO    - liga/desliga o apagar automatico (para comparar)
    DORMIR  - entra em deep sleep agora (para testar o acordar)
*/

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Preferences.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

// ---------------------------------------------------------------------
// AJUSTES (mexa a vontade)
// ---------------------------------------------------------------------
const float    ANG_LIGA   = 30;      // graus: perto da posicao de olhar -> acende
const float    ANG_APAGA  = 50;      // graus: longe -> apaga
const uint32_t T_LIGAR    = 250;     // ms na posicao para acender
const uint32_t T_APAGAR   = 1500;    // ms fora da posicao para apagar
const uint32_t T_TOQUE    = 8000;    // ms acesa depois de um toque, mesmo fora da posicao
const float    LIM_GIRO   = 6;       // graus/s: abaixo disso conta como parado
const float    LIM_ACEL   = 0.06f;   // g: |aceleracao - 1 g| abaixo disso conta como parado
const uint32_t T_DORMIR   = 60000;   // ms parado para dormir (teste)
const uint8_t  BRILHO     = 200;

#define USAR_INT_MPU 1               // 1 = fio INT do MPU no IO14 acorda o ESP32
const gpio_num_t PINO_INT_MPU = GPIO_NUM_14;
const gpio_num_t PINO_BOOT    = GPIO_NUM_0;
const gpio_num_t PINO_LUZ     = GPIO_NUM_45;   // luz de fundo da tela

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
float ax, ay, az;          // aceleracao (g)
float gx, gy, gz;          // giro (graus/s)
float fx = 0, fy = 0, fz = 1;   // gravidade filtrada (tira tremida)

bool mpuIniciar() {
  if (!i2cEscrever(END_MPU, 0x6B, 0x80)) return false;   // reset (tambem sai do modo economico)
  delay(100);
  if (!i2cEscrever(END_MPU, 0x6B, 0x01)) return false;   // acorda
  i2cEscrever(END_MPU, 0x6C, 0x00);                      // todos os eixos ligados
  i2cEscrever(END_MPU, 0x1A, 0x03);                      // filtro ~44 Hz
  i2cEscrever(END_MPU, 0x1B, 0x08);                      // giro +-500
  i2cEscrever(END_MPU, 0x1C, 0x08);                      // acel +-4 g
  i2cEscrever(END_MPU, 0x38, 0x00);                      // sem interrupcao enquanto acordado
  return true;
}

bool mpuLer() {
  uint8_t b[14];
  if (!i2cLer(END_MPU, 0x3B, b, 14)) return false;
  auto v = [&](int i) { return (int16_t)((b[i] << 8) | b[i + 1]); };
  ax = v(0) / LSB_ACEL; ay = v(2) / LSB_ACEL; az = v(4) / LSB_ACEL;
  gx = v(8) / LSB_GIRO; gy = v(10) / LSB_GIRO; gz = v(12) / LSB_GIRO;
  // media movel exponencial: cada leitura nova pesa 10%
  fx += 0.1f * (ax - fx); fy += 0.1f * (ay - fy); fz += 0.1f * (az - fz);
  return true;
}

// Prepara o MPU para ficar de vigia enquanto o ESP32 dorme:
// acelerometro em modo economico (acorda 5x/s), giro desligado,
// e o pino INT sobe quando houver movimento acima de MOT_THR.
void mpuModoVigia() {
  i2cEscrever(END_MPU, 0x1C, 0x09);   // +-4 g + filtro passa-alta 5 Hz (o detector usa a variacao, nao a gravidade)
  i2cEscrever(END_MPU, 0x1F, 20);     // MOT_THR: sensibilidade (menor = mais sensivel)
  i2cEscrever(END_MPU, 0x20, 1);      // MOT_DUR: 1 ms acima do limite ja conta
  i2cEscrever(END_MPU, 0x37, 0x20);   // INT_PIN_CFG: ativo em 1, push-pull, TRAVADO ate ler o status
  i2cEscrever(END_MPU, 0x38, 0x40);   // INT_ENABLE: so deteccao de movimento
  delay(50);
  uint8_t st; i2cLer(END_MPU, 0x3A, &st, 1);   // le o status = limpa interrupcao antiga
  i2cEscrever(END_MPU, 0x6C, 0x47);   // PWR_MGMT_2: acorda a 5 Hz, giro X/Y/Z em espera
  i2cEscrever(END_MPU, 0x6B, 0x28);   // PWR_MGMT_1: modo ciclo, sensor de temperatura desligado
}

// ---------------------------------------------------------------------
// POSICAO DE REFERENCIA
// ---------------------------------------------------------------------
bool  temRef = false;
float rx, ry, rz;          // gravidade na posicao "olhando" (vetor de tamanho 1)

void carregarRef() {
  prefs.begin("s16mpu", true);
  temRef = prefs.getBool("tem", false);
  rx = prefs.getFloat("rx", 0); ry = prefs.getFloat("ry", 0); rz = prefs.getFloat("rz", 1);
  prefs.end();
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

// Angulo entre a gravidade atual e a gravada (produto escalar -> acos)
float anguloAteRef() {
  float m = sqrtf(fx * fx + fy * fy + fz * fz);
  if (m < 0.1f) return 180;
  float c = (fx * rx + fy * ry + fz * rz) / m;
  c = constrain(c, -1.0f, 1.0f);
  return acosf(c) * RAD_TO_DEG;
}

// ---------------------------------------------------------------------
// ESTADO DA TELA
// ---------------------------------------------------------------------
enum Estado { LIGADA, APAGADA };
Estado estado = LIGADA;
bool   autoLigado = true;

uint32_t desdeNaPosicao = 0, desdeForaPosicao = 0;   // 0 = condicao nao esta valendo
uint32_t ultimoToque = 0, ultimoMovimento = 0;
uint32_t contaApagou = 0, contaAcendeu = 0;
float    angulo = 0, giroTotal = 0, desvioAcel = 0;

void acender(const char* motivo) {
  if (estado == LIGADA) return;
  tela.wakeup();
  estado = LIGADA; contaAcendeu++;
  Serial.printf("[tela] ACENDE (%s)\n", motivo);
}
void apagar(const char* motivo) {
  if (estado == APAGADA) return;
  tela.sleep();                      // luz em 0 + painel em modo de espera
  estado = APAGADA; contaApagou++;
  Serial.printf("[tela] APAGA (%s)\n", motivo);
}

// ---------------------------------------------------------------------
// DEEP SLEEP
// ---------------------------------------------------------------------
void dormir(const char* motivo) {
  Serial.printf("[sono] dormindo (%s)\n", motivo);
  tela.fillScreen(TFT_BLACK);
  tela.setTextColor(TFT_DARKGREY);
  tela.drawString("dormindo...", 70, 150, &fonts::Font4);
  delay(600);

  if (mpuOk) mpuModoVigia();
  i2cEscrever(END_TOUCH, 0xA5, 0x03);   // touch em hibernacao (acorda com o RST no proximo boot)
  tela.sleep();

  // O pino da luz de fundo nao e um pino "RTC": no deep sleep ele ficaria
  // solto e a luz poderia acender fraquinha. Forcamos 0 e "congelamos".
  gpio_reset_pin(PINO_LUZ);
  gpio_set_direction(PINO_LUZ, GPIO_MODE_OUTPUT);
  gpio_set_level(PINO_LUZ, 0);
  gpio_hold_en(PINO_LUZ);
  gpio_deep_sleep_hold_en();

  // Acordar pelo BOOT (nivel 0 quando apertado)
  rtc_gpio_pullup_en(PINO_BOOT);
  esp_sleep_enable_ext0_wakeup(PINO_BOOT, 0);
#if USAR_INT_MPU
  // Acordar pelo INT do MPU (nivel 1 quando houver movimento).
  // O pulldown segura o pino em 0 se o fio nao estiver ligado.
  rtc_gpio_pulldown_en(PINO_INT_MPU);
  rtc_gpio_pullup_dis(PINO_INT_MPU);
  esp_sleep_enable_ext1_wakeup(1ULL << PINO_INT_MPU, ESP_EXT1_WAKEUP_ANY_HIGH);
#endif
  Serial.flush();
  esp_deep_sleep_start();             // nao volta daqui: o proximo passo e o setup()
}

const char* motivoAcordou() {
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT0: return "botao BOOT";
    case ESP_SLEEP_WAKEUP_EXT1: return "movimento (INT do MPU)";
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "ligou/reset";
    default: return "outro";
  }
}

// ---------------------------------------------------------------------
// TELA
// ---------------------------------------------------------------------
const int BTN_Y = 262, BTN_H = 56, BTN_W = 76;
const int BTN_GRAVAR = 0, BTN_AUTO = 82, BTN_DORMIR = 164;
String textoAcordou;

void botao(int x, const char* txt, uint16_t cor) {
  tela.fillRoundRect(x, BTN_Y, BTN_W, BTN_H, 8, cor);
  tela.setTextColor(TFT_WHITE);
  tela.drawCenterString(txt, x + BTN_W / 2, BTN_Y + 20, &fonts::Font2);
}

void desenharFixo() {
  tela.fillScreen(TFT_BLACK);
  tela.setTextColor(TFT_CYAN, TFT_BLACK);
  tela.drawString("Tela x posicao do pulso", 4, 2, &fonts::Font2);
  tela.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tela.drawString("acordou: " + textoAcordou, 4, 20, &fonts::Font2);
  tela.drawFastHLine(0, 38, 240, TFT_DARKGREY);
  botao(BTN_GRAVAR, "GRAVAR", TFT_NAVY);
  botao(BTN_AUTO, autoLigado ? "AUTO: sim" : "AUTO: nao", autoLigado ? TFT_DARKGREEN : TFT_MAROON);
  botao(BTN_DORMIR, "DORMIR", TFT_PURPLE);
}

void linha(int y, uint16_t cor, const char* fmt, ...) {
  char buf[48];
  va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
  tela.setTextColor(cor, TFT_BLACK);
  tela.setTextPadding(236);
  tela.drawString(buf, 4, y, &fonts::Font2);
  tela.setTextPadding(0);
}

void desenharValores() {
  uint32_t agora = millis();
  if (!mpuOk) linha(44, TFT_RED, "MPU nao responde");
  else if (!temRef) linha(44, TFT_YELLOW, "Sem posicao: olhe o display e GRAVAR");
  else linha(44, angulo < ANG_LIGA ? TFT_GREEN : angulo > ANG_APAGA ? TFT_RED : TFT_YELLOW,
             "angulo ate a posicao: %5.1f", angulo);

  linha(64,  TFT_WHITE,  "limites: acende <%.0f  apaga >%.0f", ANG_LIGA, ANG_APAGA);
  linha(84,  TFT_ORANGE, "giro total %5.1f graus/s", giroTotal);
  linha(104, TFT_ORANGE, "|acel - 1g| %5.3f g", desvioAcel);
  bool parado = giroTotal < LIM_GIRO && desvioAcel < LIM_ACEL;
  linha(124, parado ? TFT_YELLOW : TFT_GREEN, parado ? "PARADO" : "em movimento");
  uint32_t parou = agora - ultimoMovimento;
  linha(144, TFT_WHITE, "dorme em %lu s", parou >= T_DORMIR ? 0 : (T_DORMIR - parou) / 1000);
  linha(164, TFT_DARKGREY, "apagou %lu  acendeu %lu", contaApagou, contaAcendeu);
  linha(184, TFT_DARKGREY, "gravidade %5.2f %5.2f %5.2f", fx, fy, fz);
  if (temRef) linha(204, TFT_DARKGREY, "posicao   %5.2f %5.2f %5.2f", rx, ry, rz);
  linha(230, TFT_DARKGREY, "toque na tela apagada = acende");
}

// ---------------------------------------------------------------------
// TOUCH
// ---------------------------------------------------------------------
bool estavaTocando = false;
bool toqueSoParaAcender = false;   // o toque que acendeu a tela nao aperta botao

void tratarTouch() {
  int32_t x, y;
  bool tocando = tela.getTouch(&x, &y);
  if (tocando && !estavaTocando) {             // inicio de um toque
    ultimoToque = millis();
    ultimoMovimento = millis();                // tocar tambem conta como "em uso"
    if (estado == APAGADA) {
      acender("toque");
      toqueSoParaAcender = true;
    } else if (y >= BTN_Y) {
      if (x < BTN_AUTO) { gravarRef(); }
      else if (x < BTN_DORMIR) { autoLigado = !autoLigado; desenharFixo(); }
      else { dormir("botao DORMIR"); }
    }
  }
  if (!tocando) toqueSoParaAcender = false;
  estavaTocando = tocando;
}

// ---------------------------------------------------------------------
void setup() {
  // Se viemos de um deep sleep, o pino da luz ainda esta "congelado"
  gpio_hold_dis(PINO_LUZ);
  gpio_deep_sleep_hold_dis();

  Serial.begin(115200);
  delay(500);
  textoAcordou = motivoAcordou();
  Serial.printf("\n=== Teste tela x MPU === acordou por: %s\n", textoAcordou.c_str());

  tela.init();
  tela.setBrightness(BRILHO);
  carregarRef();

  mpuOk = mpuIniciar();
  if (mpuOk) {   // enche o filtro da gravidade antes de comecar a decidir
    for (int i = 0; i < 40; i++) { mpuLer(); delay(5); }
    fx = ax; fy = ay; fz = az;
  }
  Serial.printf("[mpu] %s  |  posicao gravada: %s\n", mpuOk ? "ok" : "NAO RESPONDE", temRef ? "sim" : "nao");
  ultimoMovimento = ultimoToque = millis();
  desenharFixo();
}

void loop() {
  static uint32_t tMpu = 0, tTela = 0, tSerial = 0;
  uint32_t agora = millis();

  // 1) MPU a 100 Hz e as contas
  if (agora - tMpu >= 10) {
    tMpu = agora;
    if (mpuOk && mpuLer()) {
      giroTotal  = sqrtf(gx * gx + gy * gy + gz * gz);
      desvioAcel = fabsf(sqrtf(ax * ax + ay * ay + az * az) - 1.0f);
      if (giroTotal >= LIM_GIRO || desvioAcel >= LIM_ACEL) ultimoMovimento = agora;
      if (temRef) angulo = anguloAteRef();
    }
  }

  // 2) touch
  tratarTouch();

  // 3) decide acender/apagar
  if (autoLigado && temRef && mpuOk) {
    bool naPosicao = angulo < ANG_LIGA;
    bool foraPosicao = angulo > ANG_APAGA;
    if (naPosicao) { if (!desdeNaPosicao) desdeNaPosicao = agora; } else desdeNaPosicao = 0;
    if (foraPosicao) { if (!desdeForaPosicao) desdeForaPosicao = agora; } else desdeForaPosicao = 0;

    if (estado == APAGADA && desdeNaPosicao && agora - desdeNaPosicao >= T_LIGAR)
      acender("pulso levantado");
    if (estado == LIGADA && desdeForaPosicao && agora - desdeForaPosicao >= T_APAGAR &&
        agora - ultimoToque >= T_TOQUE)
      apagar("pulso abaixado");
  } else if (estado == APAGADA) {
    acender("AUTO desligado");
  }

  // 4) parado demais -> dorme
  if (agora - ultimoMovimento >= T_DORMIR) dormir("parado");

  // 5) tela 5x por segundo (so se estiver acesa)
  if (estado == LIGADA && agora - tTela >= 200) { tTela = agora; desenharValores(); }

  // 6) Serial 2x por segundo (funciona mesmo com a tela apagada)
  if (agora - tSerial >= 500) {
    tSerial = agora;
    Serial.printf("[%s] ang %5.1f  giro %5.1f  dAcel %.3f  dorme em %lus\n",
                  estado == LIGADA ? "LIGADA " : "APAGADA", angulo, giroTotal, desvioAcel,
                  (agora - ultimoMovimento) >= T_DORMIR ? 0 : (T_DORMIR - (agora - ultimoMovimento)) / 1000);
  }
}
