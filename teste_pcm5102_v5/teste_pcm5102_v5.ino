// Teste de bancada do modulo PCM5102A com um ESP32-C3 SuperMini (core Arduino-ESP32 3.x).
// Versao 5: procura a causa dos "buracos" no som (a gravacao mostrou o tom sumindo ~8 ms a cada ~150 ms).
//
// Ligacoes (mapa da placa nova):
//   C3 GPIO0 -> BCK      C3 GPIO1 -> DIN      C3 GPIO2 -> LCK (LRCK)
//   C3 5V    -> VIN      C3 GND   -> GND      SCK no GND (jumper de cima do modulo fechado)
// Jumpers no verso do modulo: H1L=L  H2L=L  H3L=H  H4L=L
//
// Ciclo: 2 s de 440 Hz so no esquerdo, 2 s de 660 Hz so no direito, 4 s de varredura 200 Hz -> 2 kHz,
// 1 s de silencio. A cada ciclo o Serial mostra quantas vezes faltou dado para o I2S ("buracos").
//
// Chave CLOCK_INTEIRO:
//   false: 44100 Hz. O C3 nao tem um clock que divida certinho para 44,1 kHz, entao o BCK sai com
//          tremedeira (divisor fracionario). O PLL do PCM5102A pode perder a sincronia e emudecer
//          por alguns ms de tempos em tempos.
//   true:  50000 Hz, com o clock do C3 (160 MHz) dividido por numeros inteiros: BCK limpo.
#include <Arduino.h>
#include "driver/i2s_std.h"
#include <math.h>

const char *VERSAO = "teste_pcm5102 v5 - 2026-11-11 21:40 (CLOCK_INTEIRO, contador de buracos)";
const int PINO_BCK = 0, PINO_DIN = 1, PINO_LCK = 2;
// --- mude aqui ---
const bool CLOCK_INTEIRO = false;       // true: 50 kHz com divisores inteiros; false: 44,1 kHz
const float VOLUME = 0.20f;

const int TAXA = CLOCK_INTEIRO ? 50000 : 44100;
i2s_chan_handle_t tx = nullptr;
int32_t buf[256 * 2];                  // 256 quadros estereo, 32 bits por amostra
float tabela[1024];                    // seno pronto (o C3 nao tem FPU: sinf() e lento)
float fase = 0;                        // 0..1024
volatile uint32_t buracos = 0;

static bool IRAM_ATTR faltouDado(i2s_chan_handle_t h, i2s_event_data_t *ev, void *ctx) {
  buracos++;
  return false;
}

bool iniciaI2S() {
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.auto_clear = true;
  cc.dma_desc_num = 8;
  cc.dma_frame_num = 256;
  esp_err_t e = i2s_new_channel(&cc, &tx, nullptr);
  if (e != ESP_OK) { Serial.printf("i2s_new_channel: %s\n", esp_err_to_name(e)); return false; }
  i2s_std_config_t sc = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(TAXA),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)PINO_BCK,
      .ws = (gpio_num_t)PINO_LCK,
      .dout = (gpio_num_t)PINO_DIN,
      .din = I2S_GPIO_UNUSED,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  // 160 MHz / (128 x 50 kHz) = 25 (inteiro) e BCK = 64 x 50 kHz = MCLK / 2 (inteiro)
  if (CLOCK_INTEIRO) sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_128;
  e = i2s_channel_init_std_mode(tx, &sc);
  if (e != ESP_OK) { Serial.printf("i2s_channel_init_std_mode: %s\n", esp_err_to_name(e)); return false; }
  i2s_event_callbacks_t cbs = { .on_recv = nullptr, .on_recv_q_ovf = nullptr, .on_sent = nullptr, .on_send_q_ovf = faltouDado };
  i2s_channel_register_event_callback(tx, &cbs, nullptr);
  e = i2s_channel_enable(tx);
  if (e != ESP_OK) { Serial.printf("i2s_channel_enable: %s\n", esp_err_to_name(e)); return false; }
  return true;
}

void tocar(float freqIni, float freqFim, float segundos, bool esq, bool dir) {
  long total = (long)(segundos * TAXA), feitos = 0;
  while (feitos < total) {
    int n = min(256L, total - feitos);
    for (int i = 0; i < n; i++) {
      float f = freqIni + (freqFim - freqIni) * (float)(feitos + i) / total;
      fase += 1024.0f * f / TAXA;
      if (fase >= 1024.0f) fase -= 1024.0f;
      int32_t s = (int32_t)(tabela[(int)fase] * VOLUME * 2147483000.0f);
      buf[i * 2] = esq ? s : 0;        // canal esquerdo
      buf[i * 2 + 1] = dir ? s : 0;    // canal direito
    }
    size_t escritos = 0;
    i2s_channel_write(tx, buf, n * 8, &escritos, portMAX_DELAY);
    feitos += n;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.printf("\n=== %s ===\n", VERSAO);
  for (int i = 0; i < 1024; i++) tabela[i] = sinf(2.0f * PI * i / 1024.0f);
  if (!iniciaI2S()) { Serial.println("Falha ao iniciar o I2S"); while (true) delay(1000); }
  Serial.printf("I2S ok - tocando a %d Hz (%s), volume %.0f%%\n", TAXA,
                CLOCK_INTEIRO ? "divisores inteiros" : "divisor fracionario", VOLUME * 100);
}

void loop() {
  uint32_t b0 = buracos;
  Serial.println("440 Hz so no ESQUERDO");   tocar(440, 440, 2, true, false);
  Serial.println("660 Hz so no DIREITO");    tocar(660, 660, 2, false, true);
  Serial.println("Varredura 200 -> 2000 Hz"); tocar(200, 2000, 4, true, true);
  Serial.println("Silencio");                tocar(0, 0, 1, false, false);
  Serial.printf("buracos (faltou dado pro I2S) neste ciclo: %lu\n", (unsigned long)(buracos - b0));
}
