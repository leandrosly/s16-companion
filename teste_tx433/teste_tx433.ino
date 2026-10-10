/*
  =====================================================================
   TESTE TX 433 MHz - o display imitando o controle dos piscas
   Placa: ES3C28P  |  Transmissor: STX882 no IO21
  =====================================================================

  LIGACAO DO STX882
    VCC  -> 3.3V (conector I2C ou GPIO)   GND -> GND
    DATA -> IO21 (conector GPIO)
    ANT  -> antena espiral (obrigatoria no transmissor!)

  O PROTOCOLO (descoberto com o captura_433 v2)
    Cada quadro tem 40 bits = 5 bytes, e depois um pulso curto + 9 ms de silencio:

        3C 24 06  [comando]  [comando XOR 05]
        |------|                 |
        identidade do controle   conferencia (checksum)

    Bit 1 = sinal ligado ~990 us + desligado ~330 us
    Bit 0 = sinal ligado ~330 us + desligado ~990 us
    O controle repete o quadro ~14 vezes por aperto.

    Comandos:
        0x11 pisca esquerda       0x12 pisca direita
        0x03 desliga (tudo)       0x13 luz laranja
        0x23 luz vermelha         0x33 laranja piscando
    (nibble de baixo = funcao: 1 esq, 2 dir, 3 luz; nibble de cima = modo)

  COMO O TEMPO E GERADO
    Pelo periferico RMT do ESP32 (Remote Control): a gente entrega a lista
    de pulsos e ele gera sozinho, com precisao de 1 us, sem ocupar o
    processador. O programa nao trava enquanto transmite.

  PARA CONFERIR
    Deixe o C3 com o captura_433 v2 rodando perto: os bits que ele mostra
    quando o display transmite devem ser iguais aos do controle original.
*/

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ---------------------------------------------------------------------
// PROTOCOLO
// ---------------------------------------------------------------------
const int      PINO_TX   = 21;
const uint16_t T_CURTO   = 330;     // us
const uint16_t T_LONGO   = 990;     // us
const uint16_t T_SILENCIO = 9000;   // us depois do pulso final
const int      REPETICOES = 12;     // quadros por envio (o controle manda ~14)
const uint8_t  IDENT[3] = { 0x3C, 0x24, 0x06 };

const uint8_t CMD_ESQ = 0x11, CMD_DIR = 0x12, CMD_DESLIGA = 0x03;
const uint8_t CMD_LARANJA = 0x13, CMD_VERMELHO = 0x23, CMD_PISCANDO = 0x33;

// Cada "simbolo" do RMT = um par (nivel, duracao) + (nivel, duracao).
// Um bit cabe exatamente num simbolo. Quadro = 40 bits + 1 simbolo final.
const int SIMB_QUADRO = 41;
rmt_data_t simbolos[SIMB_QUADRO * REPETICOES];

void montarQuadro(rmt_data_t* s, uint8_t cmd) {
  uint8_t bytes[5] = { IDENT[0], IDENT[1], IDENT[2], cmd, (uint8_t)(cmd ^ 0x05) };
  int k = 0;
  for (int b = 0; b < 5; b++) {
    for (int i = 7; i >= 0; i--) {                  // bit mais significativo primeiro
      bool um = (bytes[b] >> i) & 1;
      s[k].level0 = 1; s[k].duration0 = um ? T_LONGO : T_CURTO;   // sinal ligado
      s[k].level1 = 0; s[k].duration1 = um ? T_CURTO : T_LONGO;   // sinal desligado
      k++;
    }
  }
  // pulso curto final + silencio
  s[k].level0 = 1; s[k].duration0 = T_CURTO;
  s[k].level1 = 0; s[k].duration1 = T_SILENCIO;
}

bool enviar(uint8_t cmd) {
  if (!rmtTransmitCompleted(PINO_TX)) return false;   // ainda enviando o anterior
  for (int r = 0; r < REPETICOES; r++) montarQuadro(&simbolos[r * SIMB_QUADRO], cmd);
  bool ok = rmtWriteAsync(PINO_TX, simbolos, SIMB_QUADRO * REPETICOES);
  Serial.printf("[tx] 3C 24 06 %02X %02X  x%d  %s\n", cmd, cmd ^ 0x05, REPETICOES, ok ? "ok" : "ERRO");
  return ok;
}

// ---------------------------------------------------------------------
// TELA + TOUCH (mesma configuracao da ponte)
// ---------------------------------------------------------------------
const bool INVERTER_TOUCH_X = true;
const bool INVERTER_TOUCH_Y = true;

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
      cfg.i2c_port = 0; cfg.i2c_addr = 0x38;
      cfg.pin_sda = 16; cfg.pin_scl = 15;
      cfg.pin_int = 17; cfg.pin_rst = 18; cfg.freq = 400000;
      if (INVERTER_TOUCH_X) { cfg.x_min = 239; cfg.x_max = 0; } else { cfg.x_min = 0; cfg.x_max = 239; }
      if (INVERTER_TOUCH_Y) { cfg.y_min = 319; cfg.y_max = 0; } else { cfg.y_min = 0; cfg.y_max = 319; }
      _touch.config(cfg); _panel.setTouch(&_touch); }
    setPanel(&_panel);
  }
};
LGFX tela;

// ---------------------------------------------------------------------
// BOTOES (3 linhas x 2 colunas)
// ---------------------------------------------------------------------
struct Botao { const char* nome; uint8_t cmd; uint16_t cor; };
const Botao BOTOES[6] = {
  { "< ESQ",    CMD_ESQ,      TFT_ORANGE },
  { "DIR >",    CMD_DIR,      TFT_ORANGE },
  { "LARANJA",  CMD_LARANJA,  0xFC00     },
  { "VERMELHO", CMD_VERMELHO, TFT_RED    },
  { "PISCANDO", CMD_PISCANDO, TFT_MAGENTA},
  { "DESLIGA",  CMD_DESLIGA,  TFT_DARKGREY },
};
const int BX[2] = { 4, 124 }, BY0 = 40, BW = 112, BH = 70, BESP = 76;

void desenharBotao(int i, bool aceso) {
  int x = BX[i % 2], y = BY0 + (i / 2) * BESP;
  tela.fillRoundRect(x, y, BW, BH, 10, aceso ? TFT_WHITE : BOTOES[i].cor);
  tela.setTextColor(aceso ? TFT_BLACK : TFT_WHITE);
  tela.drawCenterString(BOTOES[i].nome, x + BW / 2, y + BH / 2 - 8, &fonts::Font2);
}

void desenharTudo() {
  tela.fillScreen(TFT_BLACK);
  tela.setTextColor(TFT_CYAN);
  tela.drawString("Controle 433 MHz (IO21)", 4, 4, &fonts::Font2);
  for (int i = 0; i < 6; i++) desenharBotao(i, false);
}

void mostrarUltimo(uint8_t cmd) {
  char t[40];
  snprintf(t, sizeof(t), "enviado: 3C 24 06 %02X %02X", cmd, cmd ^ 0x05);
  tela.setTextColor(TFT_YELLOW, TFT_BLACK);
  tela.setTextPadding(240);
  tela.drawString(t, 4, 274, &fonts::Font2);
  tela.setTextPadding(0);
}

// ---------------------------------------------------------------------
bool estavaTocando = false;
int  botaoAceso = -1;
uint32_t tAceso = 0;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Teste TX 433 MHz ===");
  Serial.println("Serial: e d x l v p (esq, dir, desliga, laranja, vermelho, piscando) ou 'h 13' (comando em hexa)");

  // RMT no pino 21, relogio de 1 MHz = cada "tick" vale 1 us
  if (!rmtInit(PINO_TX, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 1000000))
    Serial.println("[tx] ERRO ao iniciar o RMT");

  tela.init();
  tela.setBrightness(200);
  desenharTudo();
}

void loop() {
  uint32_t agora = millis();

  // touch
  int32_t x, y;
  bool tocando = tela.getTouch(&x, &y);
  if (tocando && !estavaTocando && y >= BY0) {
    int col = x < BX[1] ? 0 : 1;
    int lin = (y - BY0) / BESP;
    int i = lin * 2 + col;
    if (lin < 3 && enviar(BOTOES[i].cmd)) {
      if (botaoAceso >= 0) desenharBotao(botaoAceso, false);
      botaoAceso = i; tAceso = agora;
      desenharBotao(i, true);
      mostrarUltimo(BOTOES[i].cmd);
    }
  }
  estavaTocando = tocando;

  // apaga o destaque do botao quando a transmissao termina
  if (botaoAceso >= 0 && agora - tAceso > 100 && rmtTransmitCompleted(PINO_TX)) {
    desenharBotao(botaoAceso, false);
    botaoAceso = -1;
  }

  // Serial
  if (Serial.available()) {
    String s = Serial.readStringUntil('\n');
    s.trim();
    int cmd = -1;
    if (s == "e") cmd = CMD_ESQ;
    else if (s == "d") cmd = CMD_DIR;
    else if (s == "x") cmd = CMD_DESLIGA;
    else if (s == "l") cmd = CMD_LARANJA;
    else if (s == "v") cmd = CMD_VERMELHO;
    else if (s == "p") cmd = CMD_PISCANDO;
    else if (s.startsWith("h ")) cmd = strtol(s.c_str() + 2, nullptr, 16) & 0xFF;
    if (cmd >= 0) { enviar(cmd); mostrarUltimo(cmd); }
  }
}
