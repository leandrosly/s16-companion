/*
  =====================================================================
   TESTE DO PIEZO no conector SPEAKER (via codec ES8311 + amplificador)
   Placa: ES3C28P
  =====================================================================

  O CAMINHO DO SOM
    ESP32 --I2C (config)--> ES8311   (endereco 0x18, mesmo barramento do touch)
    ESP32 --I2S (amostras)--> ES8311 --analogico--> FM8002E --> SPEAKER
                                                      ^
                                             IO1 liga (0) / desliga (1)
    Pinos I2S (lcdwiki): MCLK IO4, BCLK IO5, LRCK IO7,
                         IO8 = dados ESP32 -> codec (som), IO6 = codec -> ESP32 (microfone)

  O QUE O ESP32 FAZ
    Uma tarefa separada gera o som amostra por amostra (32 000 por segundo)
    e manda pelo I2S. O loop() cuida da tela, do touch e do codec (I2C):
    lembra da regra "todo I2C na mesma tarefa"? A tarefa de som so usa I2S.

  NA TELA
    - Frequencia (barrinha, 500 a 8000 Hz) e Volume (do codec)
    - TOCAR/PARAR: som continuo na frequencia escolhida
    - VARRER: sobe de 1 a 8 kHz em 12 s. Quando ficar MAIS ALTO, toque
      em MARCAR: a frequencia vai para a barrinha. Essa e a ressonancia
      do seu piezo (onde ele "canta" mais alto).
    - ONDA: quadrada (mais alta, mais "aspera") ou senoide (mais limpa)
    - BIPE / PISCA / ALARME: exemplos de sons para a ponte, na frequencia
      escolhida
    Frequencia e volume ficam salvos (Preferences "s16som").

  VOLUME
    O registrador de volume do ES8311 vai de -95,5 dB a +32 dB.
    75% = 0 dB (sem ganho). Acima disso o codec amplifica digitalmente:
    a onda "estoura" (corta o topo). Com piezo isso deixa ate mais alto,
    so fica mais aspero.
*/

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <ESP_I2S.h>
#include <Preferences.h>

// ---------------------------------------------------------------------
// PINOS DE AUDIO
// ---------------------------------------------------------------------
const int PINO_AMP_EN = 1;    // 0 = amplificador ligado, 1 = desligado
const int PINO_MCLK = 4, PINO_BCLK = 5, PINO_LRCK = 7;
const int PINO_DOUT = 8;      // ESP32 -> codec (o som)
const int PINO_DIN  = 6;      // codec -> ESP32 (microfone; nao usado aqui)
const uint32_t TAXA = 32000;  // amostras por segundo

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
      cfg.pin_bl = 45; cfg.freq = 44100; cfg.pwm_channel = 7;
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
LGFX_Sprite faixa(&tela);
I2SClass i2s;
Preferences prefs;

// ---------------------------------------------------------------------
// ES8311 (via funcoes I2C da LovyanGFX, como o MPU)
// ---------------------------------------------------------------------
// Sequencia baseada no driver oficial da Espressif (esp-bsp/components/es8311),
// para: codec "escravo" (o ESP32 gera os relogios), MCLK = 256 x 32 kHz
// = 8,192 MHz vindo do pino MCLK, formato I2S, 16 bits.
const uint8_t END_CODEC = 0x18;
bool codecOk = false;
uint8_t codecId = 0;

bool codecEscrever(uint8_t reg, uint8_t v) {
  return lgfx::i2c::writeRegister8(I2C_PORTA, END_CODEC, reg, v, 0, 400000).has_value();
}
uint8_t codecLer(uint8_t reg) {
  auto r = lgfx::i2c::readRegister8(I2C_PORTA, END_CODEC, reg, 400000);
  return r.has_value() ? r.value() : 0;
}

bool codecIniciar() {
  codecId = codecLer(0xFD);                        // 0x83 = ES8311
  if (!codecEscrever(0x00, 0x1F)) return false;    // reset
  delay(20);
  codecEscrever(0x00, 0x00);
  codecEscrever(0x00, 0x80);                       // liga
  // Relogios: todos ligados, MCLK vem do pino MCLK
  codecEscrever(0x01, 0x3F);
  // Divisores para 8,192 MHz / 32 kHz (tabela do driver): pre_div 1, x1, osr 0x10
  codecEscrever(0x02, codecLer(0x02) & 0x07);
  codecEscrever(0x03, 0x10);
  codecEscrever(0x04, 0x10);
  codecEscrever(0x05, 0x00);
  codecEscrever(0x06, (codecLer(0x06) & 0xC0) | 0x03);   // BCLK nao invertido, div 4
  codecEscrever(0x07, codecLer(0x07) & 0xC0);
  codecEscrever(0x08, 0xFF);
  // Formato: escravo, I2S, 16 bits na entrada e na saida
  codecEscrever(0x00, codecLer(0x00) & 0xBF);
  codecEscrever(0x09, 0x0C);
  codecEscrever(0x0A, 0x0C);
  // Liga as partes analogicas, o DAC e a saida
  codecEscrever(0x0D, 0x01);
  codecEscrever(0x0E, 0x02);
  codecEscrever(0x12, 0x00);
  codecEscrever(0x13, 0x10);
  codecEscrever(0x1C, 0x6A);
  codecEscrever(0x37, 0x08);
  codecEscrever(0x31, codecLer(0x31) & ~0x60);     // tira o mudo
  return true;
}

// volume 0..100 -> registrador 0x32 (75% = 0xBF = 0 dB)
void codecVolume(int vol) {
  vol = constrain(vol, 0, 100);
  codecEscrever(0x32, vol == 0 ? 0 : vol * 256 / 100 - 1);
}

// ---------------------------------------------------------------------
// GERADOR DE SOM (roda numa tarefa separada)
// ---------------------------------------------------------------------
// Modos de som. O loop escolhe; a tarefa toca.
enum { SOM_PARADO, SOM_CONTINUO, SOM_VARRER, SOM_BIPE, SOM_PISCA, SOM_ALARME };
volatile int   somModo = SOM_PARADO;
volatile int   somPedido = 0;        // muda a cada pedido novo: a tarefa reinicia o tempo
volatile float freqBase = 3000;      // Hz
volatile bool  ondaQuadrada = true;
volatile float freqAgora = 0;        // o que esta tocando agora (para mostrar na tela)

const float VARRER_DE = 1000, VARRER_ATE = 8000, VARRER_S = 12;

// Para um instante t (segundos desde o inicio do modo), diz qual frequencia
// tocar e se o som esta ligado. Devolve false quando o modo terminou.
bool programa(int modo, float t, float& f, bool& ligado) {
  float fb = freqBase;
  switch (modo) {
    case SOM_CONTINUO:
      f = fb; ligado = true; return true;
    case SOM_VARRER:                                  // sobe em escala "musical" (log)
      if (t >= VARRER_S) return false;
      f = VARRER_DE * powf(VARRER_ATE / VARRER_DE, t / VARRER_S); ligado = true; return true;
    case SOM_BIPE:                                    // bi-bip
      if (t >= 0.40f) return false;
      f = fb; ligado = (t < 0.12f) || (t >= 0.20f && t < 0.32f); return true;
    case SOM_PISCA: {                                 // tic ... tac ... (8 vezes)
      if (t >= 3.2f) return false;
      int n = (int)(t / 0.4f);
      f = (n % 2) ? fb * 0.75f : fb;
      ligado = fmodf(t, 0.4f) < 0.025f; return true;
    }
    case SOM_ALARME: {                                // sirene subindo, 3 vezes
      if (t >= 1.8f) return false;
      float frac = fmodf(t, 0.6f) / 0.6f;
      f = fb * (0.7f + 0.6f * frac); ligado = true; return true;
    }
  }
  return false;
}

void tarefaSom(void*) {
  const int N = 256;                     // amostras por bloco (8 ms)
  static int16_t buf[N * 2];             // estereo: esquerdo, direito, esquerdo...
  float fase = 0;
  uint32_t n = 0;                        // amostras desde o inicio do modo
  int pedidoVisto = -1;
  for (;;) {
    int modo = somModo;
    if (somPedido != pedidoVisto) { pedidoVisto = somPedido; n = 0; }
    for (int i = 0; i < N; i++) {
      float f = 0; bool ligado = false;
      if (modo != SOM_PARADO && !programa(modo, n / (float)TAXA, f, ligado)) {
        somModo = modo = SOM_PARADO;     // acabou: avisa o loop
      }
      int16_t v = 0;
      if (ligado) {
        fase += f / TAXA;
        if (fase >= 1) fase -= 1;
        v = ondaQuadrada ? (fase < 0.5f ? 30000 : -30000)
                         : (int16_t)(30000 * sinf(2 * PI * fase));
      }
      buf[2 * i] = buf[2 * i + 1] = v;   // mesmo som nos dois canais
      n++;
      if (i == 0) freqAgora = ligado ? f : 0;
    }
    // write espera ter espaco no buffer de DMA: e isso que da o ritmo
    i2s.write((uint8_t*)buf, sizeof(buf));
  }
}

void tocar(int modo) {
  digitalWrite(PINO_AMP_EN, LOW);        // liga o amplificador
  somModo = modo;
  somPedido = somPedido + 1;
}
void parar() { somModo = SOM_PARADO; }

// ---------------------------------------------------------------------
// TELA
// ---------------------------------------------------------------------
int volume = 75;
const float F_MIN = 500, F_MAX = 8000;
const int TRILHO_X1 = 16, TRILHO_X2 = 224, ALT_BARRA = 44;
const int Y_FREQ = 70, Y_VOL = 116;
const int B_Y1 = 168, B_Y2 = 222, B_H = 46, B_W = 76, B_X2 = 82, B_X3 = 164;

// frequencia em escala log: cada pedaco da barra = mesma "distancia musical"
float freqDaPosicao(float t) { return F_MIN * powf(F_MAX / F_MIN, t); }
float posicaoDaFreq(float f) { return logf(f / F_MIN) / logf(F_MAX / F_MIN); }

void desenharBarra(int y, const char* nome, const char* valor, float t) {
  faixa.fillSprite(TFT_BLACK);
  faixa.setTextColor(TFT_WHITE);
  faixa.drawString(nome, 4, 0, &fonts::Font2);
  faixa.setTextColor(TFT_CYAN);
  faixa.drawRightString(valor, 236, 0, &fonts::Font2);
  int xv = TRILHO_X1 + (int)(constrain(t, 0.0f, 1.0f) * (TRILHO_X2 - TRILHO_X1));
  faixa.fillRoundRect(TRILHO_X1, 25, TRILHO_X2 - TRILHO_X1, 6, 3, TFT_DARKGREY);
  faixa.fillRoundRect(TRILHO_X1, 25, xv - TRILHO_X1, 6, 3, TFT_CYAN);
  faixa.fillCircle(xv, 28, 9, TFT_WHITE);
  faixa.pushSprite(0, y);
}
void desenharBarras() {
  char t[16];
  snprintf(t, sizeof(t), "%.0f Hz", (float)freqBase);
  desenharBarra(Y_FREQ, "Frequencia", t, posicaoDaFreq(freqBase));
  snprintf(t, sizeof(t), "%d %%", volume);
  desenharBarra(Y_VOL, "Volume", t, volume / 100.0f);
}

void botao(int x, int y, const char* txt, uint16_t cor) {
  tela.fillRoundRect(x, y, B_W, B_H, 8, cor);
  tela.setTextColor(TFT_WHITE);
  tela.drawCenterString(txt, x + B_W / 2, y + B_H / 2 - 8, &fonts::Font2);
}
void desenharBotoes() {
  int m = somModo;
  botao(0,    B_Y1, m == SOM_CONTINUO ? "PARAR" : "TOCAR", m == SOM_CONTINUO ? TFT_MAROON : TFT_DARKGREEN);
  botao(B_X2, B_Y1, m == SOM_VARRER ? "MARCAR" : "VARRER", m == SOM_VARRER ? TFT_ORANGE : TFT_NAVY);
  botao(B_X3, B_Y1, ondaQuadrada ? "quadrada" : "senoide", TFT_DARKGREY);
  botao(0,    B_Y2, "BIPE",   TFT_PURPLE);
  botao(B_X2, B_Y2, "PISCA",  TFT_PURPLE);
  botao(B_X3, B_Y2, "ALARME", TFT_PURPLE);
}

void desenharTudo() {
  tela.fillScreen(TFT_BLACK);
  tela.setTextColor(TFT_CYAN);
  tela.drawString("Teste do piezo", 4, 2, &fonts::Font2);
  char t[40];
  snprintf(t, sizeof(t), "ES8311 id 0x%02X %s", codecId, codecOk ? "ok" : "NAO RESPONDE");
  tela.setTextColor(codecOk ? TFT_DARKGREY : TFT_RED);
  tela.drawRightString(t, 236, 2, &fonts::Font2);
  desenharBarras();
  desenharBotoes();
  tela.setTextColor(TFT_DARKGREY);
  tela.drawString("VARRER: toque MARCAR quando", 4, 276, &fonts::Font2);
  tela.drawString("o som estiver mais alto", 4, 294, &fonts::Font2);
}

// Frequencia tocando agora, grande, no topo
void desenharAgora() {
  tela.setTextColor(somModo != SOM_PARADO ? TFT_YELLOW : TFT_DARKGREY, TFT_BLACK);
  tela.setTextPadding(240);
  char t[16];
  if (somModo != SOM_PARADO && freqAgora > 0) snprintf(t, sizeof(t), "%.0f Hz", (float)freqAgora);
  else snprintf(t, sizeof(t), somModo != SOM_PARADO ? "..." : "parado");
  tela.drawCenterString(t, 120, 26, &fonts::Font4);
  tela.setTextPadding(0);
}

void salvar() {
  prefs.begin("s16som", false);
  prefs.putFloat("freq", freqBase);
  prefs.putInt("vol", volume);
  prefs.putBool("quad", ondaQuadrada);
  prefs.end();
  Serial.printf("[som] salvo: %.0f Hz, volume %d, %s\n", (float)freqBase, volume, ondaQuadrada ? "quadrada" : "senoide");
}

// ---------------------------------------------------------------------
// TOUCH
// ---------------------------------------------------------------------
bool estavaTocando = false;
int  arrastando = 0;          // 0 nada, 1 frequencia, 2 volume

void arrastar(int x) {
  float t = constrain((float)(x - TRILHO_X1) / (TRILHO_X2 - TRILHO_X1), 0.0f, 1.0f);
  if (arrastando == 1) {
    freqBase = roundf(freqDaPosicao(t) / 10) * 10;   // de 10 em 10 Hz
  } else {
    int v = (int)roundf(t * 100);
    if (v == volume) return;
    volume = v;
    codecVolume(volume);                              // I2C: no loop, como deve ser
  }
  desenharBarras();
}

void toqueComecou(int x, int y) {
  if (y >= Y_FREQ && y < Y_FREQ + ALT_BARRA) { arrastando = 1; arrastar(x); return; }
  if (y >= Y_VOL && y < Y_VOL + ALT_BARRA)   { arrastando = 2; arrastar(x); return; }
  int col = x < B_X2 ? 0 : x < B_X3 ? 1 : 2;
  if (y >= B_Y1 && y < B_Y1 + B_H) {
    if (col == 0) { if (somModo == SOM_CONTINUO) parar(); else tocar(SOM_CONTINUO); }
    if (col == 1) {
      if (somModo == SOM_VARRER) {                    // MARCAR
        freqBase = roundf(freqAgora / 10) * 10;
        parar();
        desenharBarras();
        salvar();
        Serial.printf("[som] marcado: %.0f Hz\n", (float)freqBase);
      } else tocar(SOM_VARRER);
    }
    if (col == 2) { ondaQuadrada = !ondaQuadrada; salvar(); }
    desenharBotoes();
  } else if (y >= B_Y2 && y < B_Y2 + B_H) {
    tocar(col == 0 ? SOM_BIPE : col == 1 ? SOM_PISCA : SOM_ALARME);
    desenharBotoes();
  }
}

void tratarTouch() {
  int32_t x, y;
  bool tocando = tela.getTouch(&x, &y);
  if (tocando && !estavaTocando) toqueComecou(x, y);
  else if (tocando && arrastando) arrastar(x);
  if (!tocando && arrastando) { arrastando = 0; salvar(); }
  estavaTocando = tocando;
}

// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Teste do piezo ===");

  pinMode(PINO_AMP_EN, OUTPUT);
  digitalWrite(PINO_AMP_EN, HIGH);       // amplificador desligado ate tocar algo

  tela.init();
  tela.setBrightness(200);
  faixa.createSprite(240, ALT_BARRA);

  prefs.begin("s16som", true);
  freqBase = prefs.getFloat("freq", 3000);
  volume = prefs.getInt("vol", 75);
  ondaQuadrada = prefs.getBool("quad", true);
  prefs.end();

  // I2S primeiro: o codec precisa do MCLK chegando para "acordar" direito
  i2s.setPins(PINO_BCLK, PINO_LRCK, PINO_DOUT, PINO_DIN, PINO_MCLK);
  if (!i2s.begin(I2S_MODE_STD, TAXA, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO))
    Serial.println("[i2s] ERRO ao iniciar");

  codecOk = codecIniciar();
  codecVolume(volume);
  Serial.printf("[codec] %s, id 0x%02X (esperado 0x83)\n", codecOk ? "ok" : "NAO RESPONDE", codecId);

  // Tarefa de som no nucleo 0 (o loop roda no 1)
  xTaskCreatePinnedToCore(tarefaSom, "som", 4096, nullptr, 5, nullptr, 0);

  desenharTudo();
}

void loop() {
  static uint32_t tTela = 0, desdeParado = 0;
  static int modoAntes = -1;
  uint32_t agora = millis();

  tratarTouch();

  // Quando um som curto termina sozinho, atualiza os botoes
  int m = somModo;
  if (m != modoAntes) { modoAntes = m; desenharBotoes(); desdeParado = agora; }

  // Amplificador: desliga 300 ms depois de parar (evita chiado e economiza)
  if (m == SOM_PARADO && agora - desdeParado > 300) digitalWrite(PINO_AMP_EN, HIGH);

  if (agora - tTela >= 100) { tTela = agora; desenharAgora(); }
}
