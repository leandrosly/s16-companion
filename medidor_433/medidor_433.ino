/*
  =====================================================================
   MEDIDOR 433 MHz - mede o alcance do transmissor do display (S16 Companion)
   Placa: ESP32-C3 Supermini | Receptor: SRX882S | Tela: LCD 16x2 I2C (PCF8574)
  =====================================================================

  COMO FUNCIONA
    No display, digite '433 teste' no Serial (pode tirar o USB depois: o teste
    continua e o display nao dorme enquanto ele estiver ligado). O display manda
    uma RAJADA a cada ~0,8 s: o mesmo quadro repetido 12 vezes, no formato da
    buzina, com uma identidade que ninguem obedece:  00 9F 1E NN
    (NN = numero da rajada, 0..255, para saber quando uma rajada sumiu inteira).

    Este medidor conta quantas das 12 repeticoes chegaram inteiras:
      12/12 = sinal folgado ... 0/12 = nao chegou nada.
    O receptor 433 nao informa a "forca" do sinal (o SRX882S nao tem pino de
    RSSI), entao a medida e a TAXA DE ACERTO, que e o que importa na pratica.

  TELA
    Q:100% ########     media das ultimas 10 rajadas + barrinha
    ult 12/12 sum 0     repeticoes da ultima rajada | rajadas sumidas (das 10)

  DICA DE USO
    Perto, tudo da 100%. Ponha o medidor ONDE FICA a lanterna (ou mais longe,
    ate comecar a cair para uns 50-80%) e NAO mexa mais nele. Ai va mudando
    so a antena do display e veja o numero subir ou descer. Espere ~10 s a
    cada mudanca (a media e das ultimas 10 rajadas).

  LIGACOES
    SRX882S: VCC -> 3V3 | GND -> GND | CS -> 3V3 (obrigatorio) | DATA -> GPIO4 | antena
    LCD I2C: VCC -> 5V  | GND -> GND | SDA -> GPIO8 | SCL -> GPIO9
             (endereco 0x27; se a tela ficar em branco, o sketch procura sozinho
              e mostra no Serial; ajuste o contraste no trimpot azul de tras)

  Bibliotecas: LiquidCrystal_I2C (Frank de Brabander)
  Placa: "ESP32C3 Dev Module", USB CDC On Boot: Enabled
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>

static const int PINO_RX = 4;
static const int I2C_SDA = 8, I2C_SCL = 9;
static uint8_t   lcdAddr = 0x27;

static const uint32_t ID_TESTE = 0x009F1E;   // identidade das rajadas de teste
static const int      REPET    = 12;         // repeticoes por rajada (TX433_REPET no display)
static const int      JANELA   = 10;         // media das ultimas N rajadas

LiquidCrystal_I2C* lcd = nullptr;

// ---------- Decodificador (o mesmo do display: formato do controle da buzina) ----------
//   intervalo BAIXO longo (1-20 ms) = sincronismo; ALTO+BAIXO curto+longo = 0, longo+curto = 1
static const uint16_t CURTO_MIN = 200, CURTO_MAX = 600, LONGO_MIN = 700, LONGO_MAX = 1150;
static const uint32_t SINC_MIN = 1000, SINC_MAX = 20000;

static volatile uint32_t fila[32];
static volatile uint8_t  filaIni = 0, filaFim = 0;

void IRAM_ATTR rxISR() {
  static uint32_t tAnt = 0, bits = 0, altoAnt = 0;
  static int nb = -1;
  uint32_t agora = micros();
  uint32_t d = agora - tAnt;
  tAnt = agora;
  bool eraAlto = !digitalRead(PINO_RX);       // o intervalo que acabou tinha o nivel oposto
  if (eraAlto) {
    altoAnt = d;
    bool ok = (d >= CURTO_MIN && d <= CURTO_MAX) || (d >= LONGO_MIN && d <= LONGO_MAX);
    if (!ok) nb = -1;
    return;
  }
  if (d >= SINC_MIN && d <= SINC_MAX) { nb = 0; bits = 0; return; }
  if (nb < 0) return;
  bool hCurto = altoAnt <= CURTO_MAX;
  bool lCurto = d >= CURTO_MIN && d <= CURTO_MAX;
  bool lLongo = d >= LONGO_MIN && d <= LONGO_MAX;
  if (hCurto && lLongo)       bits = bits << 1;
  else if (!hCurto && lCurto) bits = (bits << 1) | 1;
  else { nb = -1; return; }
  if (++nb == 32) {
    uint8_t prox = (filaFim + 1) % 32;
    if (prox != filaIni) { fila[filaFim] = bits; filaFim = prox; }
    nb = -1;
  }
}

// ---------- Contagem ----------
static int      hist[JANELA];          // repeticoes recebidas em cada rajada (0..12)
static int      nHist = 0, posHist = 0;
static int      seqAtual = -1;         // rajada em andamento
static int      contAtual = 0;
static int      ultimaRajada = -1;     // repeticoes da ultima rajada fechada
static uint32_t ultQuadro = 0;
static uint32_t outrosCod = 0;         // quadros validos de outra identidade (controle, buzina...)

static void guardar(int n) {
  hist[posHist] = n;
  posHist = (posHist + 1) % JANELA;
  if (nHist < JANELA) nHist++;
  ultimaRajada = n;
}

static void fecharRajada() {
  if (seqAtual < 0 || contAtual <= 0) return;      // nenhuma aberta / ja fechada
  guardar(contAtual > REPET ? REPET : contAtual);
  Serial.printf("rajada %3d: %2d/%d\n", seqAtual, contAtual, REPET);
}

static void tratar(uint32_t cod) {
  if ((cod >> 8) != ID_TESTE) { outrosCod++; return; }
  int seq = cod & 0xFF;
  ultQuadro = millis();
  if (seq == seqAtual) { contAtual++; return; }
  // rajada nova: fecha a anterior e conta as que sumiram inteiras no meio
  if (seqAtual >= 0) {
    fecharRajada();
    int pulou = (seq - seqAtual - 1 + 256) % 256;
    if (pulou > JANELA) pulou = JANELA;          // (display reiniciado etc.)
    for (int i = 0; i < pulou; i++) { guardar(0); Serial.println("rajada sumiu: 0/12"); }
  }
  seqAtual = seq;
  contAtual = 1;
}

static void mostrar() {
  if (!lcd) return;
  char l1[17], l2[17];
  uint32_t semNada = millis() - ultQuadro;
  if (!ultQuadro) {                               // nunca recebeu nada
    snprintf(l1, sizeof(l1), "%-16s", "esperando...");
    snprintf(l2, sizeof(l2), "%-16s", "'433 teste' liga");
  } else if (semNada > 3000) {                    // recebia e parou
    snprintf(l1, sizeof(l1), "%-16s", "SEM SINAL");
    char t[17]; snprintf(t, sizeof(t), "ha %lus", (unsigned long)(semNada / 1000));
    snprintf(l2, sizeof(l2), "%-16s", t);
  } else if (nHist == 0) {                        // 1a rajada chegando
    snprintf(l1, sizeof(l1), "%-16s", "recebendo...");
    snprintf(l2, sizeof(l2), "%-16s", "");
  } else {
    int soma = 0, sumiu = 0;
    for (int i = 0; i < nHist; i++) { soma += hist[i]; if (hist[i] == 0) sumiu++; }
    int q = soma * 100 / (nHist * REPET);
    char barra[9];
    int nb = (q * 8 + 50) / 100;
    for (int i = 0; i < 8; i++) barra[i] = i < nb ? (char)0xFF : ' ';   // 0xFF = bloco cheio no LCD
    barra[8] = 0;
    snprintf(l1, sizeof(l1), "Q:%3d%% %s", q, barra);
    snprintf(l2, sizeof(l2), "ult %2d/%d sum %-2d", ultimaRajada, REPET, sumiu);
  }
  lcd->setCursor(0, 0); lcd->print(l1);
  lcd->setCursor(0, 1); lcd->print(l2);
}

static void acharLcd() {
  Wire.begin(I2C_SDA, I2C_SCL);
  const uint8_t cands[] = { 0x27, 0x3F };
  for (uint8_t a : cands) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { lcdAddr = a; break; }
  }
  Serial.printf("LCD no endereco 0x%02X\n", lcdAddr);
  lcd = new LiquidCrystal_I2C(lcdAddr, 16, 2);
  lcd->init();
  lcd->backlight();
  lcd->setCursor(0, 0); lcd->print("Medidor 433 MHz ");
  lcd->setCursor(0, 1); lcd->print("S16 Companion   ");
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println("\n=== Medidor 433 (rajadas 00 9F 1E NN do display) ===");
  acharLcd();
  pinMode(PINO_RX, INPUT);
  attachInterrupt(digitalPinToInterrupt(PINO_RX), rxISR, CHANGE);
  delay(1200);
}

void loop() {
  while (filaIni != filaFim) {
    uint32_t c = fila[filaIni];
    filaIni = (filaIni + 1) % 32;
    tratar(c);
  }
  // rajada parada ha mais de 0,6 s (sem a proxima chegar): fecha para a tela andar
  if (seqAtual >= 0 && contAtual > 0 && millis() - ultQuadro > 600) {
    fecharRajada();
    contAtual = -1000;                 // ja fechada: repeticoes atrasadas da mesma rajada nao contam
  }
  static uint32_t tTela = 0;
  if (millis() - tTela > 300) { tTela = millis(); mostrar(); }
}
