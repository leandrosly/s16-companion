/*
  =====================================================================
   CAPTURA 433 MHz - descobrir os codigos do controle dos piscas
   Placa: ESP32-C3 Supermini  |  Receptor: MX-RM-5V (ou SRX882)
  =====================================================================

  LIGACOES (MX-RM-5V)
    Receptor VCC  -> pino 5V do C3 (vem direto da USB)
    Receptor GND  -> GND
    Receptor DATA -> divisor de tensao -> GPIO4
                     DATA ---[10k]---+---[20k]--- GND
                                     |
                                   GPIO4
    (o receptor devolve 5 V e o ESP32 aguenta no maximo 3,3 V: o divisor
     reduz para ~3,3 V. Os dois pinos DATA do meio do receptor sao o mesmo
     sinal, pode usar qualquer um.)

    Com o SRX882: VCC no 3V3, CS tambem no 3V3, DATA direto no GPIO4 (sem divisor).

  ANTENA: um fio de uns 5-17 cm no furo "ANT" do receptor ajuda, mas com o
  controle a poucos centimetros nem precisa.

  DOIS MODOS (escolha em MODO, la embaixo):
    MODO_RCSWITCH: usa a biblioteca rc-switch, que reconhece os formatos mais
                   comuns (EV1527, PT2262...) e mostra o codigo como numero.
                   Instale pelo gerenciador: "rc-switch" (sui77).
    MODO_CRU:      grava a duracao de cada pulso. Use se o modo acima nao
                   mostrar nada: funciona com qualquer formato.

  COMO USAR
    Abra o Serial (115200), aperte cada botao do controle algumas vezes,
    um de cada vez, e anote o que aparece para cada um.
*/

#include <RCSwitch.h>

#define MODO_RCSWITCH 1
#define MODO_CRU      2
#define MODO          MODO_RCSWITCH      // <- troque aqui

const int PINO_RX = 4;

// ---------------------------------------------------------------------
// MODO RC-SWITCH
// ---------------------------------------------------------------------
RCSwitch receptor;

void setupRcSwitch() {
  // enableReceive recebe o numero da INTERRUPCAO; no ESP32 e o proprio pino
  receptor.enableReceive(digitalPinToInterrupt(PINO_RX));
  Serial.println("Modo rc-switch: aperte os botoes do controle");
}

void loopRcSwitch() {
  if (!receptor.available()) return;
  unsigned long valor = receptor.getReceivedValue();
  if (valor == 0) {
    Serial.println("Recebi algo, mas o formato nao foi reconhecido");
  } else {
    Serial.printf("Codigo: %lu  |  %d bits  |  protocolo %d  |  pulso %d us  |  ",
                  valor, receptor.getReceivedBitlength(),
                  receptor.getReceivedProtocol(), receptor.getReceivedDelay());
    // mostra tambem em binario, com zeros a esquerda
    for (int i = receptor.getReceivedBitlength() - 1; i >= 0; i--) Serial.print((valor >> i) & 1);
    Serial.println();
  }
  receptor.resetAvailable();
}

// ---------------------------------------------------------------------
// MODO CRU
// ---------------------------------------------------------------------
// Cada mudanca de nivel no pino gera uma interrupcao. Guardamos quanto tempo
// passou desde a mudanca anterior. Um "silencio" longo (> 4 ms) marca o fim
// de um quadro. Pulsos curtissimos (< 80 us) sao ruido e zeram a captura.
const int MAX_PULSOS = 400;
volatile uint16_t duracoes[MAX_PULSOS];
volatile int nPulsos = 0;
volatile uint32_t ultimaBorda = 0;
volatile bool quadroPronto = false;

void IRAM_ATTR bordaISR() {
  uint32_t agora = micros();
  uint32_t dt = agora - ultimaBorda;
  ultimaBorda = agora;
  if (quadroPronto) return;                 // esperando o loop imprimir
  if (dt > 4000) {                          // silencio: terminou um quadro?
    if (nPulsos >= 40) quadroPronto = true; // quadro com tamanho razoavel
    else nPulsos = 0;                       // pequeno demais: era ruido
    return;
  }
  if (dt < 80) { nPulsos = 0; return; }     // ruido
  if (nPulsos < MAX_PULSOS) duracoes[nPulsos++] = dt;
}

void setupCru() {
  pinMode(PINO_RX, INPUT);
  attachInterrupt(digitalPinToInterrupt(PINO_RX), bordaISR, CHANGE);
  Serial.println("Modo cru: aperte os botoes do controle");
}

void loopCru() {
  if (!quadroPronto) return;
  int n = nPulsos;

  // Limite entre "curto" e "longo": meio caminho entre o menor e o maior pulso
  uint16_t menor = 65535, maior = 0;
  for (int i = 0; i < n; i++) {
    uint16_t d = duracoes[i];               // copia (variavel volatile confunde o min/max)
    if (d < menor) menor = d;
    if (d > maior) maior = d;
  }
  uint16_t limite = (menor + maior) / 2;

  Serial.printf("\nQuadro com %d pulsos (curto ~%u us, longo ~%u us)\n", n, menor, maior);
  // Versao "desenhada": . = curto, - = longo. Facil de comparar entre botoes.
  for (int i = 0; i < n; i++) Serial.print(duracoes[i] < limite ? '.' : '-');
  Serial.println();
  // Valores exatos (para reproduzir depois)
  for (int i = 0; i < n; i++) { Serial.print(duracoes[i]); Serial.print(i < n - 1 ? "," : "\n"); }

  nPulsos = 0;
  quadroPronto = false;
}

// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== Captura 433 MHz ===");
#if MODO == MODO_RCSWITCH
  setupRcSwitch();
#else
  setupCru();
#endif
}

void loop() {
#if MODO == MODO_RCSWITCH
  loopRcSwitch();
#else
  loopCru();
#endif
}
