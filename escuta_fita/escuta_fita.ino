/*
  =====================================================================
   ESCUTA FITA - le o sinal que a roda manda para uma fita de LED
   endereçavel (tipo WS2812) e mostra no Serial: quantos pixels, os
   tempos dos bits e as cores. So escuta: nao mexe em nada na roda.
   Placa: ESP32-C3 Supermini (a 2a plaquinha)
  =====================================================================

  LIGACAO (o DATA da fita e de 5 V - medido no osciloscopio em 30/10):

     DATA da fita ──┤◄── 1N4148 ──┬── GPIO5 do C3
                  (faixa do lado  │
                   da fita)       └── resistor 4,7k ── 3V3 do C3
     GND da fita ───────────────────── GND do C3

  Nivel alto: o diodo bloqueia e o resistor puxa o GPIO5 para 3,3 V.
  Nivel baixo: o diodo conduz e puxa o GPIO5 para ~0,6 V. O C3 nunca
  ve os 5 V, e a fita continua recebendo o sinal da roda normalmente.
  (O GPIO6 sai um "clock" interno do SPI: deixe esse pino SOLTO.)

  COMO FUNCIONA
    O SPI do C3 em modo de leitura vira um "gravador": amostra o GPIO5
    8 milhoes de vezes por segundo (uma amostra a cada 125 ns) durante
    16 ms, por DMA (sem buracos). Depois o programa procura a pausa de
    "reset" (fio parado em 0 por mais de 50 us), le os bits do quadro
    seguinte pela largura do pulso alto (curto = 0, longo = 1) e conta.

  SERIAL (115200): captura sozinho a cada 3 s. Digite:
    c   captura agora        p   liga/desliga a lista de cores
    a   liga/desliga a captura automatica

  Placa: "ESP32C3 Dev Module", USB CDC On Boot: Enabled
*/

#include <Arduino.h>
#include "driver/spi_master.h"
#include "esp_heap_caps.h"

static const int PIN_DATA = 5;          // entrada (pelo diodo + resistor)
static const int PIN_SCK  = 6;          // clock do SPI: nao usado, deixe solto
static const int AMOSTRA_HZ = 8000000;  // 8 MHz -> 125 ns por amostra
static const int CAPT_BYTES = 16000;    // 16000 x 8 = 128000 amostras = 16 ms
static const int RESET_MIN  = 400;      // >= 50 us parado em 0 = reset (fim de quadro)
static const int ALTO_UM    = 4;        // pulso alto >= 4 amostras (500 ns) = bit 1

static spi_device_handle_t spiDev;
static uint8_t* buf = nullptr;
static bool mostrarCores = true;
static bool automatico = true;

static inline int amostra(int i) { return (buf[i >> 3] >> (7 - (i & 7))) & 1; }

static bool capturar() {
  spi_transaction_t t = {};
  t.length = 0;
  t.rxlength = (size_t)CAPT_BYTES * 8;
  t.rx_buffer = buf;
  esp_err_t e = spi_device_polling_transmit(spiDev, &t);
  if (e != ESP_OK) { Serial.printf("erro na captura: %d\n", (int)e); return false; }
  return true;
}

static void analisar() {
  const int N = CAPT_BYTES * 8;
  // 1) procura o 1o reset (corrida de zeros >= RESET_MIN)
  int i = 0, zeros = 0, inicio = -1;
  for (; i < N; i++) {
    if (amostra(i) == 0) { if (++zeros >= RESET_MIN) { inicio = i; break; } }
    else zeros = 0;
  }
  if (inicio < 0) {
    int uns = 0; for (int k = 0; k < N; k++) uns += amostra(k);
    Serial.printf("Nenhum reset em 16 ms (%d%% das amostras em 1). %s\n", uns * 100 / N,
                  uns == 0 ? "Fio parado em 0: a fita esta recebendo dados? (efeito animado ajuda)"
                           : "Sinal sem pausas: ligacao/diodo invertido?");
    return;
  }
  // mede o tamanho do reset
  int r = inicio; while (r < N && amostra(r) == 0) r++;
  int resetAmostras = r - inicio + RESET_MIN;
  if (r >= N) { Serial.println("So zeros depois do reset: a roda nao mandou quadro nesta janela"); return; }

  // 2) le os bits ate o proximo reset
  static uint8_t dados[2048];               // ate ~680 pixels
  int nbits = 0, hist[16] = {0}, somaPer = 0, nPer = 0;
  int pos = r;                              // primeira borda de subida
  bool completo = false;
  while (pos < N) {
    int a = pos; while (a < N && amostra(a) == 1) a++;          // fim do alto
    int alto = a - pos;
    int b = a;   while (b < N && amostra(b) == 0 && b - a < RESET_MIN) b++;
    int baixo = b - a;
    if (a >= N) break;
    int bit = alto >= ALTO_UM ? 1 : 0;
    if (nbits < (int)sizeof(dados) * 8) {
      if (bit) dados[nbits >> 3] |= (0x80 >> (nbits & 7)); else dados[nbits >> 3] &= ~(0x80 >> (nbits & 7));
    }
    nbits++;
    hist[alto < 15 ? alto : 15]++;
    if (baixo >= RESET_MIN) { completo = true; break; }            // proximo reset: fim do quadro
    if (b >= N) break;
    somaPer += alto + baixo; nPer++;
    pos = b;
  }

  Serial.println("------------------------------------------------------------");
  Serial.printf("Reset antes do quadro: %.1f us (ou mais)\n", resetAmostras * 0.125f);
  if (!completo) Serial.println("ATENCAO: quadro cortado no fim da janela (aumente CAPT_BYTES)");
  Serial.printf("Quadro: %d bits = %d pixels de 24 bits", nbits, nbits / 24);
  if (nbits % 24) Serial.printf(" + %d bits sobrando", nbits % 24);
  if (nbits % 24 && nbits % 32 == 0) Serial.printf("  (divide por 32: fita RGBW? = %d pixels)", nbits / 32);
  Serial.println();
  if (nPer) Serial.printf("Periodo medio do bit: %.0f ns (%.0f kHz)\n", somaPer * 125.0f / nPer, 8000.0f * nPer / somaPer);
  Serial.print("Larguras do pulso alto (amostras de 125 ns: quantos): ");
  for (int k = 0; k < 16; k++) if (hist[k]) Serial.printf("%d:%d  ", k, hist[k]);
  Serial.printf("  (< %d = bit 0, >= %d = bit 1)\n", ALTO_UM, ALTO_UM);

  if (mostrarCores) {
    int px = nbits / 24; if (px > (int)sizeof(dados) / 3) px = sizeof(dados) / 3;
    Serial.println("Cores na ordem que chegam (WS2812 costuma ser G R B):");
    for (int p = 0; p < px; p++) {
      Serial.printf("%3d:%02X %02X %02X  ", p, dados[p * 3], dados[p * 3 + 1], dados[p * 3 + 2]);
      if (p % 6 == 5) Serial.println();
    }
    Serial.println();
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== Escuta fita (WS2812) - GPIO5 pelo diodo, 8 MHz, 16 ms ===");
  buf = (uint8_t*)heap_caps_malloc(CAPT_BYTES, MALLOC_CAP_DMA);
  if (!buf) { Serial.println("sem memoria DMA"); return; }

  spi_bus_config_t bus = {};
  bus.mosi_io_num = -1;
  bus.miso_io_num = PIN_DATA;
  bus.sclk_io_num = PIN_SCK;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = CAPT_BYTES;
  esp_err_t e = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
  if (e != ESP_OK) { Serial.printf("spi_bus_initialize: %d\n", (int)e); return; }

  spi_device_interface_config_t dev = {};
  dev.clock_speed_hz = AMOSTRA_HZ;
  dev.mode = 0;
  dev.spics_io_num = -1;
  dev.queue_size = 1;
  dev.flags = SPI_DEVICE_HALFDUPLEX;            // so le (sem fase de envio)
  e = spi_bus_add_device(SPI2_HOST, &dev, &spiDev);
  if (e != ESP_OK) { Serial.printf("spi_bus_add_device: %d\n", (int)e); return; }
  Serial.println("Pronto. 'c' captura, 'p' cores on/off, 'a' automatico on/off");
}

void loop() {
  static uint32_t t = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'c') { if (capturar()) analisar(); }
    else if (c == 'p') { mostrarCores = !mostrarCores; Serial.printf("cores: %s\n", mostrarCores ? "sim" : "nao"); }
    else if (c == 'a') { automatico = !automatico; Serial.printf("automatico: %s\n", automatico ? "sim" : "nao"); }
  }
  if (buf && automatico && millis() - t > 3000) {
    t = millis();
    if (capturar()) analisar();
  }
}
