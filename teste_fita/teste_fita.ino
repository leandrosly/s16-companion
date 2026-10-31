/*
  =====================================================================
   TESTE FITA - o C3 manda na fita do topo da roda (WS2812, ordem G R B)
   Placa: ESP32-C3 Supermini (a 2a plaquinha)
  =====================================================================

  O QUE A ESCUTA DESCOBRIU (31/10): fita do topo = 76 pixels (a roda manda
  80; os 4 ultimos sempre apagados), WS2812, ordem G R B, sinal de 5 V.

  LIGACAO (teste com 3,3 V direto - a fita pode aceitar ou nao):
     GPIO7 ── 330 ohm ── DATA da fita   (DATA DESCONECTADO da placa da roda!)
     GND do C3 ───────── GND da fita
     +5V da fita continua vindo da roda (NAO liga no C3)

  Com o transistor BC548 (sinal invertido): GPIO7 -> 2,2k -> base; coletor
  -> DATA com 1k para +5V; emissor no GND. Digite 'i' para inverter.

  SERIAL (115200):
    r g b w   cor fixa (vermelho, verde, azul, branco)    o   apaga
    a         arco-iris girando (igual ao da roda)        v   violeta respirando
    s         varredura vermelha (vai e volta)            f   "freio": vermelho forte
    + / -     brilho (comeca em 25%, maximo 50%)          i   inverte o sinal (BC548)
    n         CONTAGEM: pixel 0 verde, a cada 10 um vermelho (10, 20, 30...), o resto azul fraco
    k         ponto branco andando devagar; o Serial mostra o numero do pixel aceso
    k 63      so o pixel 63 aceso, parado (para confirmar o ultimo)
  (Serial Monitor com "Nova linha": cada comando e uma linha)

  ATENCAO A CORRENTE: cada pixel puxa ate ~60 mA em branco total. 76 pixels
  em branco no maximo = ~4,5 A do 5 V da roda! O brilho fica limitado a 50%
  e o branco a 1/3 por cor. Use cores e brilho baixo no teste.

  Placa: "ESP32C3 Dev Module", USB CDC On Boot: Enabled
*/

#include <Arduino.h>

static const int PIN_FITA = 7;
static const int NPIX = 80;                 // a roda manda 80 (a fita do topo tem 76)
static const int BRILHO_MAX = 128;        // 50%

static rmt_data_t simb[NPIX * 24];
static uint8_t px[NPIX][3];                // R G B (a ordem G R B e feita na hora de mandar)
static int brilho = 64;                    // 25%
static bool invertido = false;
static char modo = 'a';
static int pixelFixo = 0;                  // modo 'x' ("k N"): so este pixel aceso
static bool rmtOk = false;

// tempos em ticks de 100 ns (RMT a 10 MHz): bit 0 = 400 alto + 900 baixo; bit 1 = 900 + 400
static void montaBit(rmt_data_t& s, bool um) {
  uint8_t alto = invertido ? 0 : 1;
  s.level0 = alto;  s.duration0 = um ? 9 : 4;
  s.level1 = !alto; s.duration1 = um ? 4 : 9;
}

static void mostrar() {
  if (!rmtOk) return;
  int k = 0;
  for (int i = 0; i < NPIX; i++) {
    uint8_t c[3] = { px[i][1], px[i][0], px[i][2] };       // G R B
    for (int j = 0; j < 3; j++) {
      uint8_t v = (uint16_t)c[j] * brilho / 255;
      for (int b = 7; b >= 0; b--) montaBit(simb[k++], (v >> b) & 1);
    }
  }
  rmtWrite(PIN_FITA, simb, k, RMT_WAIT_FOR_EVER);
  delayMicroseconds(300);                                   // reset (>= 280 us)
}

static void tudo(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < NPIX; i++) { px[i][0] = r; px[i][1] = g; px[i][2] = b; }
}

static void roda(uint8_t pos, uint8_t& r, uint8_t& g, uint8_t& b) {   // arco-iris 0-255
  if (pos < 85)       { r = 255 - pos * 3; g = pos * 3; b = 0; }
  else if (pos < 170) { pos -= 85;  r = 0; g = 255 - pos * 3; b = pos * 3; }
  else                { pos -= 170; r = pos * 3; g = 0; b = 255 - pos * 3; }
}

static void aplicarInversao() {
  rmtSetEOT(PIN_FITA, invertido ? 1 : 0);                   // nivel em repouso (a fita ve 0)
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== Teste fita (76 px, GRB) no GPIO7 ===");
  rmtOk = rmtInit(PIN_FITA, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000);   // 100 ns por tick
  Serial.printf("RMT: %s\n", rmtOk ? "ok" : "ERRO");
  aplicarInversao();
  Serial.println("r g b w o | a arco-iris | v violeta | s varredura | f freio | + - brilho | i inverte");
}

void loop() {
  // le uma LINHA (Serial Monitor com "Nova linha"): "a", "+", "k", "k 63"...
  static char linha[24];
  static int nl = 0;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\r') continue;
    if (ch != '\n') { if (nl < (int)sizeof(linha) - 1) linha[nl++] = ch; continue; }
    linha[nl] = 0; nl = 0;
    char c = linha[0];
    int num = -1;
    if (linha[1] == ' ' && linha[2] >= '0' && linha[2] <= '9') num = atoi(linha + 2);
    if (c == '+') { brilho = min(brilho + 16, BRILHO_MAX); Serial.printf("brilho %d%%\n", brilho * 100 / 255); }
    else if (c == '-') { brilho = max(brilho - 16, 0); Serial.printf("brilho %d%%\n", brilho * 100 / 255); }
    else if (c == 'i') { invertido = !invertido; aplicarInversao(); Serial.printf("sinal %s\n", invertido ? "INVERTIDO (BC548)" : "normal"); }
    else if (c == 'k' && num >= 0) {                        // "k 63": so o pixel 63 aceso, parado
      if (num >= NPIX) { Serial.printf("pixel vai de 0 a %d\n", NPIX - 1); continue; }
      pixelFixo = num; modo = 'x';
      Serial.printf("so o pixel %d aceso\n", num);
    }
    else if (c && strchr("rgbwoavsfnk", c)) { modo = c; Serial.printf("modo %c\n", c); }
  }

  static uint32_t t0 = 0;
  if (millis() - t0 < 20) return;                           // ~50 quadros/s
  t0 = millis();
  uint32_t t = millis();
  switch (modo) {
    case 'r': tudo(255, 0, 0); break;
    case 'g': tudo(0, 255, 0); break;
    case 'b': tudo(0, 0, 255); break;
    case 'w': tudo(85, 85, 85); break;                      // branco limitado (corrente)
    case 'o': tudo(0, 0, 0); break;
    case 'f': { int bk = brilho; brilho = BRILHO_MAX; tudo(255, 0, 0); mostrar(); brilho = bk; return; }
    case 'a': {
      uint8_t desloc = t / 10;
      for (int i = 0; i < NPIX; i++) roda((uint8_t)(i * 10 + desloc), px[i][0], px[i][1], px[i][2]);
      break;
    }
    case 'v': {
      float f = (1 - cosf(t * 0.0025f)) / 2;                // 0..1 respirando
      tudo((uint8_t)(0x4E * f * 2), 0, (uint8_t)(0x8F * f * 1.7f));
      break;
    }
    case 'x': tudo(0, 0, 0); px[pixelFixo][0] = 255; px[pixelFixo][1] = 255; px[pixelFixo][2] = 255; break;
    case 'n':                                               // contagem de pixels
      for (int i = 0; i < NPIX; i++) {
        if (i == 0)           { px[i][0] = 0;   px[i][1] = 255; px[i][2] = 0; }
        else if (i % 10 == 0) { px[i][0] = 255; px[i][1] = 0;   px[i][2] = 0; }
        else                  { px[i][0] = 0;   px[i][1] = 0;   px[i][2] = 40; }
      }
      break;
    case 'k': {                                             // ponto andando (1 pixel a cada 0,7 s)
      static int ult = -1;
      int pos = (t / 700) % NPIX;
      if (pos != ult) { ult = pos; Serial.printf("pixel %d\n", pos); }
      tudo(0, 0, 0); px[pos][0] = 255; px[pos][1] = 255; px[pos][2] = 255;
      break;
    }
    case 's': {
      int pos = (t / 15) % (2 * NPIX); if (pos >= NPIX) pos = 2 * NPIX - 1 - pos;
      for (int i = 0; i < NPIX; i++) {
        int d = abs(i - pos);
        uint8_t v = d < 6 ? 255 - d * 40 : 0;
        px[i][0] = v; px[i][1] = 0; px[i][2] = 0;
      }
      break;
    }
  }
  mostrar();
}
