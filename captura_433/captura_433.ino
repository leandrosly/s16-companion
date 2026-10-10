/*
  =====================================================================
   CAPTURA 433 MHz - descobrir os codigos do controle dos piscas  (v2)
   Placa: ESP32-C3 Supermini  |  Receptor: SRX882S (ou MX-RM-5V)
  =====================================================================

  LIGACOES (SRX882S)
    VCC  -> 3V3        GND -> GND
    CS   -> 3V3   (obrigatorio no SRX882S: solto = dormindo)
    DATA -> GPIO4 (direto)
    ANT  -> antena espiral
  (MX-RM-5V: VCC no 5V e DATA por divisor 10k/20k, como na v1)

  O QUE MUDOU NA v2
    A v1 parava de gravar enquanto imprimia. Como o controle repete o
    mesmo quadro varias vezes seguidas, o quadro seguinte era gravado
    a partir do meio: os primeiros pulsos se perdiam. Agora a gravacao
    NUNCA para: cada quadro completo (de um silencio ao proximo) e
    copiado de uma vez para outra area, e o loop imprime essa copia.

    Tambem mostra:
      - o nivel do 1o pulso (ALTO = com sinal de radio, BAIXO = sem)
      - quanto durou o silencio entre as repeticoes
      - os BITS ja decodificados (pares curto+longo / longo+curto)

  POR QUE A rc-switch NAO MOSTROU NADA
    Ela aceita no maximo 32 bits. Este controle manda uns 38-40.

  COMO USAR
    Serial 115200. Aperte cada botao (um de cada vez, 2-3 vezes).
    Para cada botao, copie umas 3 repeticoes.
*/

const int PINO_RX = 4;
const int MAX_PULSOS = 300;
const uint32_t SILENCIO_US = 4000;   // mais que isso = fim de quadro

// Gravacao (so a interrupcao mexe)
volatile uint16_t gravando[MAX_PULSOS];
volatile int      nGravando = 0;
volatile bool     nivelInicio = false;   // nivel do 1o pulso do quadro em gravacao
volatile uint32_t ultimaBorda = 0;

// Copia pronta para imprimir
volatile uint16_t quadro[MAX_PULSOS];
volatile int      nQuadro = 0;
volatile bool     nivelQuadro = false;
volatile uint32_t silencioQuadro = 0;
volatile bool     quadroPronto = false;

void IRAM_ATTR bordaISR() {
  uint32_t agora = micros();
  uint32_t dt = agora - ultimaBorda;
  ultimaBorda = agora;
  // Nivel NOVO no pino; o intervalo que acabou de terminar tinha o nivel oposto
  bool nivelNovo = digitalRead(PINO_RX);

  if (dt > SILENCIO_US) {
    // Fim de um quadro: se for de tamanho razoavel e o anterior ja foi
    // impresso, copia. Em qualquer caso, comeca a gravar o proximo.
    if (nGravando >= 40 && !quadroPronto) {
      for (int i = 0; i < nGravando; i++) quadro[i] = gravando[i];
      nQuadro = nGravando;
      nivelQuadro = nivelInicio;
      silencioQuadro = dt;
      quadroPronto = true;
    }
    nGravando = 0;
    nivelInicio = nivelNovo;          // o proximo intervalo comeca com este nivel
    return;
  }
  if (dt < 80) { nGravando = 0; return; }      // ruido
  if (nGravando == 0) nivelInicio = !nivelNovo;
  if (nGravando < MAX_PULSOS) gravando[nGravando++] = dt;
}

// Tenta ler os pares como bits a partir de "inicio" (0 ou 1):
// longo+curto = 1, curto+longo = 0. Devolve quantos pares deram certo.
int decodificar(uint16_t* d, int n, int inicio, uint16_t limite, char* bits) {
  int k = 0;
  for (int i = inicio; i + 1 < n; i += 2) {
    bool a = d[i] > limite, b = d[i + 1] > limite;
    if (a == b) { bits[k] = 0; return -1; }    // par invalido
    bits[k++] = a ? '1' : '0';
  }
  bits[k] = 0;
  return k;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== Captura 433 MHz v2 ===  aperte os botoes do controle");
  pinMode(PINO_RX, INPUT);
  attachInterrupt(digitalPinToInterrupt(PINO_RX), bordaISR, CHANGE);
}

void loop() {
  if (!quadroPronto) return;
  static uint16_t d[MAX_PULSOS];
  int n = nQuadro;
  for (int i = 0; i < n; i++) d[i] = quadro[i];
  bool nivel0 = nivelQuadro;
  uint32_t silencio = silencioQuadro;
  quadroPronto = false;                       // libera para o proximo

  uint16_t menor = 65535, maior = 0;
  for (int i = 0; i < n; i++) { if (d[i] < menor) menor = d[i]; if (d[i] > maior) maior = d[i]; }
  uint16_t limite = (menor + maior) / 2;

  Serial.printf("\nQuadro: %d pulsos | 1o pulso %s | silencio %lu us | curto ~%u, longo ~%u\n",
                n, nivel0 ? "ALTO" : "BAIXO", silencio, menor, maior);
  for (int i = 0; i < n; i++) Serial.print(d[i] < limite ? '.' : '-');
  Serial.println();
  for (int i = 0; i < n; i++) { Serial.print(d[i]); Serial.print(i < n - 1 ? "," : "\n"); }

  static char bits[MAX_PULSOS / 2 + 2];
  for (int ini = 0; ini < 2; ini++) {
    int k = decodificar(d, n, ini, limite, bits);
    if (k > 0) Serial.printf("bits (a partir do pulso %d, %d bits): %s%s\n",
                             ini, k, bits, (n - ini) % 2 ? "  + 1 pulso no fim" : "");
  }
}
