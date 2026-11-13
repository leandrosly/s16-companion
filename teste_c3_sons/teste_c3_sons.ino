// Amostras dos sons do app tocando no ESP32-C3 + PCM5102A (so para escutar a qualidade).
// Os sons foram gerados pelo mesmo motor de som do app (SomMotor.kt), com a velocidade fazendo:
//   parado 2,5 s -> acelera ate 35 km/h em 6 s -> 35 km/h por 2 s -> freia ate parar em 2,5 s -> desliga
// e guardados em sons.h (mono, 16 kHz, 16 bits, ~1 MB).
//
// IMPORTANTE: em Ferramentas -> Partition Scheme escolha "Huge APP (3MB No OTA/1MB SPIFFS)"
// (o esquema padrao so tem 1,2 MB para o programa e os sons nao cabem).
//
// Ligacoes (mapa da placa nova): GPIO0 -> BCK, GPIO1 -> DIN, GPIO2 -> LCK, 5V -> VIN, GND -> GND.
// Ciclo: Eletrico VESS, 1 s de silencio, Harley estudio (com partida e desligar), 1 s, fon-fon de 2 tons, 1 s.
#include <Arduino.h>
#include "driver/i2s_std.h"
#include "sons.h"

const char *VERSAO = "teste_c3_sons v1 - 2026-11-11 21:45 (VESS, Harley, fon-fon a 16 kHz)";
const int PINO_BCK = 0, PINO_DIN = 1, PINO_LCK = 2;
const int TAXA = 16000;
const float VOLUME = 1.0f;             // os sons ja vem em ~50% da escala

i2s_chan_handle_t tx = nullptr;
int32_t buf[256 * 2];

bool iniciaI2S() {
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.auto_clear = true;
  cc.dma_desc_num = 8;
  cc.dma_frame_num = 256;
  if (i2s_new_channel(&cc, &tx, nullptr) != ESP_OK) return false;
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
  if (i2s_channel_init_std_mode(tx, &sc) != ESP_OK) return false;
  return i2s_channel_enable(tx) == ESP_OK;
}

// toca um som (mono nos dois lados); n = numero de amostras (NULL = silencio)
void tocar(const int16_t *som, long n) {
  const int32_t ganho = (int32_t)(VOLUME * 65536);
  long feitos = 0;
  while (feitos < n) {
    int k = min(256L, n - feitos);
    for (int i = 0; i < k; i++) {
      int32_t s = som ? (int32_t)som[feitos + i] * ganho : 0;   // 16 bits -> 32 bits (x65536 = volume 1)
      buf[i * 2] = s;
      buf[i * 2 + 1] = s;
    }
    size_t escritos = 0;
    i2s_channel_write(tx, buf, k * 8, &escritos, portMAX_DELAY);
    feitos += k;
  }
}

#define TAM(a) (long)(sizeof(a) / sizeof(a[0]))

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.printf("\n=== %s ===\n", VERSAO);
  if (!iniciaI2S()) { Serial.println("Falha ao iniciar o I2S"); while (true) delay(1000); }
  Serial.println("I2S ok");
}

void loop() {
  Serial.println("Eletrico VESS");          tocar(SOM_VESS, TAM(SOM_VESS));          tocar(nullptr, TAXA);
  Serial.println("Harley estudio");         tocar(SOM_HARLEYEST, TAM(SOM_HARLEYEST)); tocar(nullptr, TAXA);
  Serial.println("Fon-fon de 2 tons");      tocar(SOM_FONFON, TAM(SOM_FONFON));      tocar(nullptr, TAXA);
}
