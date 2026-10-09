/*
  =====================================================================
   TESTE I2C - MPU-6050 dividindo o barramento com o touch
   Placa: ES3C28P (ESP32-S3, tela 2.8" ILI9341, touch FT6336)
  =====================================================================

  A DUVIDA QUE ESTE TESTE RESPONDE
    O conector I2C da placa (IO15/IO16) e o MESMO barramento do touch
    (0x38) e do codec de audio ES8311 (0x18). A LovyanGFX controla esse
    barramento direto nos registradores do ESP32. Se outra biblioteca
    (a Wire) tentar mandar no mesmo barramento, os dois "donos" brigam.

    PLANO A (USAR_WIRE 0): o MPU usa as funcoes de I2C da PROPRIA
      LovyanGFX (lgfx::i2c::...). Um dono so, o barramento e um so.
    PLANO B (USAR_WIRE 1): o MPU vai para um barramento SEPARADO,
      nos pinos IO21 (SDA) e IO14 (SCL), usando a Wire1 (porta I2C 1).
      Atencao: tem que ser a Wire1. A Wire normal e a porta 0, a mesma
      que a LovyanGFX ja usa para o touch.

  LIGACOES
    Plano A - conector "I2C" da placa (4 pinos):
      3.3V -> VCC do MPU     GND -> GND
      IO16 -> SDA            IO15 -> SCL
    Plano B - conector "GPIO" (IO21, IO14, IO3, IO2) + 3.3V/GND do conector I2C:
      IO21 -> SDA            IO14 -> SCL
    AD0 do MPU solto ou no GND = endereco 0x68. INT nao e usado aqui.

  O QUE APARECE NA TELA
    - Lista dos aparelhos encontrados no barramento (deve ter 0x18, 0x38 e 0x68)
    - Aceleracao (g), giro (graus/s), temperatura, inclinacao (pitch/roll)
    - Um "nivel de bolha" que se mexe quando a placa inclina
    - Contadores: leituras do MPU por segundo, erros do MPU, erros do touch,
      tempo maximo de uma leitura (se o barramento estiver brigando, sobe)
    - Embaixo, uma area para arrastar o dedo (testa o touch AO MESMO TEMPO
      que o MPU e lido 100 vezes por segundo) e o botao ZERAR.

  COMO SABER SE PASSOU
    Deixe rodando uns minutos arrastando o dedo sem parar na area de baixo.
    "erros MPU" e "erros touch" devem ficar em 0 (ou quase) e o touch nao
    pode travar. Puxe o fio do MPU e recoloque: os erros sobem enquanto esta
    solto e as leituras voltam sozinhas depois (o teste tenta religar o MPU).

  CONFIGURACAO DA PLACA (igual a da ponte)
    USB CDC On Boot: Enabled | Flash 16MB | PSRAM: OPI PSRAM
*/

#define USAR_WIRE 0      // 0 = Plano A (LovyanGFX, IO15/16)   1 = Plano B (Wire1, IO21/14)

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#if USAR_WIRE
#include <Wire.h>
#endif

// ---------------------------------------------------------------------
// TELA + TOUCH (mesma configuracao da ponte)
// ---------------------------------------------------------------------
const bool INVERTER_TOUCH_X = true;
const bool INVERTER_TOUCH_Y = true;

// Barramento I2C do touch (e do conector I2C da placa)
const int I2C_PORTA_TOUCH = 0;
const int PINO_SDA_TOUCH  = 16;
const int PINO_SCL_TOUCH  = 15;

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
      cfg.pin_bl = 45; cfg.freq = 44100; cfg.pwm_channel = 7;
      _light.config(cfg); _panel.setLight(&_light); }
    { auto cfg = _touch.config();
      cfg.i2c_port = I2C_PORTA_TOUCH; cfg.i2c_addr = 0x38;
      cfg.pin_sda = PINO_SDA_TOUCH; cfg.pin_scl = PINO_SCL_TOUCH;
      cfg.pin_int = 17; cfg.pin_rst = 18; cfg.freq = 400000;
      if (INVERTER_TOUCH_X) { cfg.x_min = 239; cfg.x_max = 0; } else { cfg.x_min = 0; cfg.x_max = 239; }
      if (INVERTER_TOUCH_Y) { cfg.y_min = 319; cfg.y_max = 0; } else { cfg.y_min = 0; cfg.y_max = 319; }
      _touch.config(cfg); _panel.setTouch(&_touch); }
    setPanel(&_panel);
  }
};

LGFX tela;
LGFX_Sprite bolha(&tela);     // desenho do nivel (evita piscar)

// ---------------------------------------------------------------------
// I2C DO MPU - as duas formas, com a MESMA "cara" para o resto do programa
// ---------------------------------------------------------------------
const uint32_t FREQ_I2C = 400000;
const uint8_t  END_MPU  = 0x68;

#if USAR_WIRE
const int PINO_SDA_MPU = 21;
const int PINO_SCL_MPU = 14;

void iniciarBarramentoMpu() { Wire1.begin(PINO_SDA_MPU, PINO_SCL_MPU, FREQ_I2C); }

bool mpuLer(uint8_t reg, uint8_t* dados, size_t n) {
  Wire1.beginTransmission(END_MPU);
  Wire1.write(reg);
  if (Wire1.endTransmission(false) != 0) return false;        // false = "repeated start"
  if (Wire1.requestFrom(END_MPU, (uint8_t)n) != n) return false;
  for (size_t i = 0; i < n; i++) dados[i] = Wire1.read();
  return true;
}
bool mpuEscrever(uint8_t reg, uint8_t valor) {
  Wire1.beginTransmission(END_MPU);
  Wire1.write(reg); Wire1.write(valor);
  return Wire1.endTransmission() == 0;
}
bool existeNoBarramentoMpu(uint8_t end) {
  Wire1.beginTransmission(end);
  return Wire1.endTransmission() == 0;
}
const char* NOME_PLANO = "PLANO B: Wire1 IO21/IO14";
#else
// Plano A: a LovyanGFX ja ligou a porta 0 nos pinos 16/15 quando iniciou o touch.
// Aqui so pedimos para ela mandar/receber bytes para o endereco do MPU.
// Cada funcao devolve um "resultado": has_value() = deu certo.
void iniciarBarramentoMpu() { }

bool mpuLer(uint8_t reg, uint8_t* dados, size_t n) {
  return lgfx::i2c::readRegister(I2C_PORTA_TOUCH, END_MPU, reg, dados, n, FREQ_I2C).has_value();
}
bool mpuEscrever(uint8_t reg, uint8_t valor) {
  return lgfx::i2c::writeRegister8(I2C_PORTA_TOUCH, END_MPU, reg, valor, 0, FREQ_I2C).has_value();
}
const char* NOME_PLANO = "PLANO A: LovyanGFX IO16/IO15";
#endif

// Leitura de um registrador no barramento do TOUCH (sempre via LovyanGFX)
bool touchBusLer8(uint8_t end, uint8_t reg, uint8_t* valor) {
  auto r = lgfx::i2c::readRegister8(I2C_PORTA_TOUCH, end, reg, FREQ_I2C);
  if (r.has_error()) return false;
  *valor = r.value();
  return true;
}
// "Tem alguem neste endereco?" - tenta ler 1 byte; se ninguem responder (NACK), da erro
bool existeNoBarramentoTouch(uint8_t end) {
  uint8_t b;
  return lgfx::i2c::transactionRead(I2C_PORTA_TOUCH, end, &b, 1, 100000).has_value();
}

// ---------------------------------------------------------------------
// MPU-6050
// ---------------------------------------------------------------------
// Registradores usados:
//   0x6B PWR_MGMT_1   - acorda o chip (ele liga "dormindo")
//   0x1A CONFIG       - filtro passa-baixa interno
//   0x1B GYRO_CONFIG  - escala do giro
//   0x1C ACCEL_CONFIG - escala do acelerometro
//   0x3B..0x48        - 14 bytes: acel X,Y,Z, temperatura, giro X,Y,Z
//   0x75 WHO_AM_I     - "quem e voce": 0x68 no MPU-6050 original (clones variam)
const float LSB_ACEL = 8192.0f;   // +-4 g
const float LSB_GIRO = 65.5f;     // +-500 graus/s

bool  mpuOk = false;
uint8_t mpuWho = 0;
float ax, ay, az, gx, gy, gz, tempC;
float offGx = 0, offGy = 0, offGz = 0;   // "zero" do giro (calibrado parado)
float pitch = 0, roll = 0;               // angulo filtrado
float zeroPitch = 0, zeroRoll = 0;       // botao ZERAR

bool mpuIniciar() {
  if (!mpuEscrever(0x6B, 0x80)) return false;  // reset
  delay(100);
  if (!mpuEscrever(0x6B, 0x01)) return false;  // acorda, relogio do giro X (mais estavel)
  mpuEscrever(0x1A, 0x03);                     // filtro ~44 Hz
  mpuEscrever(0x1B, 0x08);                     // giro +-500 graus/s
  mpuEscrever(0x1C, 0x08);                     // acel +-4 g
  mpuLer(0x75, &mpuWho, 1);
  return true;
}

// Le os 14 bytes de uma vez e converte. Os valores vem em pares (alto, baixo).
bool mpuLerTudo() {
  uint8_t b[14];
  if (!mpuLer(0x3B, b, 14)) return false;
  auto v = [&](int i) { return (int16_t)((b[i] << 8) | b[i + 1]); };
  ax = v(0) / LSB_ACEL;  ay = v(2) / LSB_ACEL;  az = v(4) / LSB_ACEL;
  tempC = v(6) / 340.0f + 36.53f;
  gx = v(8) / LSB_GIRO - offGx;  gy = v(10) / LSB_GIRO - offGy;  gz = v(12) / LSB_GIRO - offGz;
  return true;
}

void calibrarGiro() {
  tela.fillScreen(TFT_BLACK);
  tela.setTextColor(TFT_YELLOW, TFT_BLACK);
  tela.drawString("Calibrando o giro...", 10, 140, &fonts::Font2);
  tela.drawString("deixe a placa parada", 10, 160, &fonts::Font2);
  offGx = offGy = offGz = 0;
  float sx = 0, sy = 0, sz = 0; int n = 0;
  for (int i = 0; i < 300; i++) {
    if (mpuLerTudo()) { sx += gx; sy += gy; sz += gz; n++; }
    delay(5);
  }
  if (n) { offGx = sx / n; offGy = sy / n; offGz = sz / n; }
  // comeca o filtro ja no angulo do acelerometro
  pitch = atan2f(-ax, sqrtf(ay * ay + az * az)) * RAD_TO_DEG;
  roll  = atan2f(ay, az) * RAD_TO_DEG;
}

// Filtro complementar: o giro e preciso no curto prazo mas "escorrega" com o
// tempo; o acelerometro nao escorrega mas treme e se confunde com aceleracao.
// Juntando 98% giro + 2% acelerometro, fica o melhor dos dois.
void atualizarAngulos(float dt) {
  float pAcel = atan2f(-ax, sqrtf(ay * ay + az * az)) * RAD_TO_DEG;
  float rAcel = atan2f(ay, az) * RAD_TO_DEG;
  pitch = 0.98f * (pitch + gy * dt) + 0.02f * pAcel;
  roll  = 0.98f * (roll  + gx * dt) + 0.02f * rAcel;
}

// ---------------------------------------------------------------------
// ESTATISTICAS
// ---------------------------------------------------------------------
uint32_t mpuLeituras = 0, mpuErros = 0, mpuErrosSeguidos = 0;
uint32_t touchVerif = 0, touchErros = 0, toques = 0;
uint32_t leiturasNoSegundo = 0, taxaMpu = 0;
uint32_t tempoMaxUs = 0, tempoMaxSegundoUs = 0;

// ---------------------------------------------------------------------
// TELA
// ---------------------------------------------------------------------
const int PAD_X = 0, PAD_Y = 262, PAD_W = 160, PAD_H = 58;   // area de arrastar
const int BTN_X = 164, BTN_Y = 262, BTN_W = 76, BTN_H = 58;  // ZERAR
const int BOLHA_X = 4, BOLHA_Y = 150, BOLHA_T = 108;

String aparelhos;   // resultado da varredura

void varrerBarramento() {
  aparelhos = "";
  for (uint8_t end = 0x08; end < 0x78; end++) {
    bool achou = existeNoBarramentoTouch(end);
#if USAR_WIRE
    if (!achou && end == END_MPU) achou = existeNoBarramentoMpu(end);
#endif
    if (!achou) continue;
    char tmp[24];
    const char* nome = end == 0x18 ? "ES8311" : end == 0x38 ? "touch" :
                       (end == 0x68 || end == 0x69) ? "MPU" : "?";
    snprintf(tmp, sizeof(tmp), "%02X %s  ", end, nome);
    aparelhos += tmp;
    Serial.printf("[i2c] encontrado 0x%02X (%s)\n", end, nome);
  }
  if (aparelhos.length() == 0) aparelhos = "nada encontrado!";
}

void desenharFixo() {
  tela.fillScreen(TFT_BLACK);
  tela.setTextColor(TFT_CYAN, TFT_BLACK);
  tela.drawString(NOME_PLANO, 4, 2, &fonts::Font2);
  tela.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tela.drawString(aparelhos, 4, 20, &fonts::Font2);
  tela.drawFastHLine(0, 38, 240, TFT_DARKGREY);

  tela.drawRect(PAD_X, PAD_Y, PAD_W, PAD_H, TFT_DARKGREY);
  tela.setTextColor(TFT_DARKGREY);
  tela.drawString("arraste o dedo aqui", PAD_X + 14, PAD_Y + 22, &fonts::Font2);
  tela.fillRoundRect(BTN_X, BTN_Y, BTN_W, BTN_H, 8, TFT_NAVY);
  tela.setTextColor(TFT_WHITE);
  tela.drawString("ZERAR", BTN_X + 16, BTN_Y + 20, &fonts::Font2);
}

void linha(int y, uint16_t cor, const char* fmt, ...) {
  char buf[48];
  va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
  tela.setTextColor(cor, TFT_BLACK);
  tela.setTextPadding(236);       // apaga o resto da linha antiga
  tela.drawString(buf, 4, y, &fonts::Font2);
  tela.setTextPadding(0);
}

void desenharValores() {
  if (!mpuOk) {
    linha(44, TFT_RED, "MPU nao responde (0x68)");
  } else {
    linha(44, TFT_WHITE, "WHO_AM_I 0x%02X  temp %.1f C", mpuWho, tempC);
  }
  linha(62,  TFT_GREEN,  "acel  X%6.2f Y%6.2f Z%6.2f g", ax, ay, az);
  linha(80,  TFT_ORANGE, "giro  X%6.0f Y%6.0f Z%6.0f", gx, gy, gz);
  linha(98,  TFT_YELLOW, "pitch %6.1f   roll %6.1f", pitch - zeroPitch, roll - zeroRoll);
  tela.drawFastHLine(0, 118, 240, TFT_DARKGREY);
  linha(124, TFT_WHITE,  "MPU %lu/s   max %lu us", taxaMpu, tempoMaxSegundoUs);

  // contadores ao lado da bolha
  int x = BOLHA_X + BOLHA_T + 8;
  tela.setTextColor(mpuErros ? TFT_RED : TFT_GREEN, TFT_BLACK);
  tela.setTextPadding(240 - x);
  tela.drawString("erros MPU", x, 152, &fonts::Font2);
  tela.drawNumber(mpuErros, x, 168, &fonts::Font2);
  tela.setTextColor(touchErros ? TFT_RED : TFT_GREEN, TFT_BLACK);
  tela.drawString("erros touch", x, 190, &fonts::Font2);
  tela.drawNumber(touchErros, x, 206, &fonts::Font2);
  tela.setTextColor(TFT_WHITE, TFT_BLACK);
  tela.drawString("toques", x, 228, &fonts::Font2);
  tela.drawNumber(toques, x, 244 - 4, &fonts::Font2);
  tela.setTextPadding(0);

  // nivel de bolha: +-30 graus ocupam o raio todo
  int c = BOLHA_T / 2, r = c - 2;
  bolha.fillSprite(TFT_BLACK);
  bolha.drawCircle(c, c, r, TFT_DARKGREY);
  bolha.drawCircle(c, c, r / 3, TFT_DARKGREY);
  bolha.drawFastHLine(2, c, BOLHA_T - 4, TFT_DARKGREY);
  bolha.drawFastVLine(c, 2, BOLHA_T - 4, TFT_DARKGREY);
  float px = constrain((roll - zeroRoll) / 30.0f, -1.0f, 1.0f) * (r - 8);
  float py = constrain((pitch - zeroPitch) / 30.0f, -1.0f, 1.0f) * (r - 8);
  bolha.fillCircle(c + (int)px, c + (int)py, 8, mpuOk ? TFT_GREEN : TFT_RED);
  bolha.pushSprite(BOLHA_X, BOLHA_Y);
}

// ---------------------------------------------------------------------
// TOUCH
// ---------------------------------------------------------------------
bool estavaTocando = false;
int ultX = -1, ultY = -1;

void tratarTouch() {
  int32_t x, y;
  bool tocando = tela.getTouch(&x, &y);
  if (tocando) {
    if (!estavaTocando) {
      toques++;
      if (x >= BTN_X && y >= BTN_Y) {               // ZERAR
        zeroPitch = pitch; zeroRoll = roll;
        Serial.println("[touch] ZERAR");
      }
    }
    // rastro dentro da area de arrastar
    if (x > PAD_X + 2 && x < PAD_X + PAD_W - 3 && y > PAD_Y + 2 && y < PAD_Y + PAD_H - 3) {
      if (estavaTocando && ultX >= 0) tela.drawLine(ultX, ultY, x, y, TFT_MAGENTA);
      else tela.fillCircle(x, y, 2, TFT_MAGENTA);
      ultX = x; ultY = y;
    } else ultX = -1;
  } else if (estavaTocando) {
    // soltou: limpa a area de arrastar
    tela.fillRect(PAD_X + 1, PAD_Y + 1, PAD_W - 2, PAD_H - 2, TFT_BLACK);
    tela.setTextColor(TFT_DARKGREY);
    tela.drawString("arraste o dedo aqui", PAD_X + 14, PAD_Y + 22, &fonts::Font2);
    ultX = -1;
  }
  estavaTocando = tocando;
}

// O getTouch nao avisa se a comunicacao falhou (so diz "sem toque").
// Para contar erros do touch, lemos de vez em quando um registrador
// fixo dele (0xA8 = codigo do fabricante) e conferimos se respondeu.
uint8_t touchFabricante = 0;
void verificarTouch() {
  uint8_t v;
  touchVerif++;
  if (!touchBusLer8(0x38, 0xA8, &v) || (touchFabricante && v != touchFabricante)) {
    touchErros++;
    Serial.printf("[touch] ERRO de leitura (%lu)\n", touchErros);
  }
}

// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.printf("\n=== Teste I2C - %s ===\n", NOME_PLANO);

  tela.init();                 // liga tela E touch (abre a porta I2C 0 em 16/15)
  tela.setBrightness(200);
  bolha.createSprite(BOLHA_T, BOLHA_T);
  iniciarBarramentoMpu();

  // Codec de audio ES8311: so confere que ele esta la (registrador 0xFD = 0x83)
  uint8_t idCodec = 0;
  if (touchBusLer8(0x18, 0xFD, &idCodec))
    Serial.printf("[i2c] ES8311 id 0x%02X (esperado 0x83)\n", idCodec);
  touchBusLer8(0x38, 0xA8, &touchFabricante);
  Serial.printf("[i2c] touch fabricante 0x%02X\n", touchFabricante);

  varrerBarramento();
  mpuOk = mpuIniciar();
  Serial.printf("[mpu] %s, WHO_AM_I=0x%02X\n", mpuOk ? "ok" : "NAO RESPONDE", mpuWho);
  if (mpuOk) calibrarGiro();
  desenharFixo();
}

void loop() {
  static uint32_t tMpu = 0, tTela = 0, tTouchChk = 0, tSeg = 0, tReinicio = 0;
  uint32_t agora = millis();

  // 1) MPU a 100 Hz
  if (agora - tMpu >= 10) {
    float dt = (agora - tMpu) / 1000.0f;
    tMpu = agora;
    if (mpuOk) {
      uint32_t t0 = micros();
      bool ok = mpuLerTudo();
      uint32_t dur = micros() - t0;
      if (dur > tempoMaxUs) tempoMaxUs = dur;
      if (ok) {
        mpuLeituras++; leiturasNoSegundo++; mpuErrosSeguidos = 0;
        if (dt < 0.1f) atualizarAngulos(dt);
      } else {
        mpuErros++;
        if (++mpuErrosSeguidos >= 20) {              // ~0,2 s sem resposta: desistiu
          mpuOk = false;
          Serial.println("[mpu] perdido, vou tentar religar");
        }
      }
    } else if (agora - tReinicio > 1000) {           // MPU fora: tenta de novo a cada 1 s
      tReinicio = agora;
      if (mpuIniciar()) { mpuOk = true; mpuErrosSeguidos = 0; Serial.println("[mpu] voltou"); }
    }
  }

  // 2) touch em toda volta do loop (o mais rapido possivel)
  tratarTouch();

  // 3) confere a saude do touch 20x por segundo
  if (agora - tTouchChk >= 50) { tTouchChk = agora; verificarTouch(); }

  // 4) tela 10x por segundo
  if (agora - tTela >= 100) { tTela = agora; desenharValores(); }

  // 5) resumo a cada segundo
  if (agora - tSeg >= 1000) {
    tSeg = agora;
    taxaMpu = leiturasNoSegundo; leiturasNoSegundo = 0;
    tempoMaxSegundoUs = tempoMaxUs; tempoMaxUs = 0;
    Serial.printf("[1s] mpu %lu/s  max %lu us  erros mpu %lu  touch %lu/%lu erros  toques %lu  pitch %.1f roll %.1f\n",
                  taxaMpu, tempoMaxSegundoUs, mpuErros, touchErros, touchVerif, toques,
                  pitch - zeroPitch, roll - zeroRoll);
  }
}
