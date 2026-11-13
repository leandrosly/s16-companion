// Teste de bancada do modulo PCM5102A com um ESP32-C3 SuperMini (core Arduino-ESP32 3.x).
// Versao 4: driver I2S do ESP-IDF direto, sem a ESP_I2S.
// Chaves no topo do codigo: VOLUME, SLOT32 (32 ou 16 bits), FORMATO_MSB e PULAR_TESTE_PINOS.
//
// Ligacoes (mapa da placa nova):
//   C3 GPIO0 -> BCK      C3 GPIO1 -> DIN      C3 GPIO2 -> LCK (LRCK)
//   C3 5V    -> VIN      C3 GND   -> GND      SCK no GND (jumper de cima do modulo fechado)
// Jumpers no verso do modulo: H1L=L  H2L=L  H3L=H  H4L=L
//
// 1) Teste dos pinos (12 s): cada pino fica 2 s em 3,3 V e 2 s em 0 V, um de cada vez
//    (o Serial diz qual). Mede no pino do MODULO: tem que acompanhar.
// 2) Depois toca em ciclo: 2 s de 440 Hz so no esquerdo, 2 s de 660 Hz so no direito,
//    4 s de varredura 200 Hz -> 2 kHz nos dois, 1 s de silencio.
//    Tocando, o multimetro deve mostrar ~1,6 V no BCK e no LCK.
#include <Arduino.h>
#include "driver/i2s_std.h"
#include <math.h>

const char *VERSAO = "teste_pcm5102 v4 - 2026-11-11 21:00 (IDF, chave FORMATO_MSB)";
const int PINO_BCK = 0, PINO_DIN = 1, PINO_LCK = 2;
const int TAXA = 44100;
// --- para testar a qualidade, mude aqui ---
const float VOLUME = 0.05f;            // 5% da escala (fone direto na saida de linha: comece baixo)
const bool SLOT32 = true;              // true: amostras de 32 bits (BCK = 64x a taxa, o que o PLL do
                                       //       PCM5102A mais gosta); false: 16 bits (BCK = 32x)
const bool PULAR_TESTE_PINOS = true;   // o teste dos pinos ja passou
const bool FORMATO_MSB = false;        // false: I2S padrao (Philips), o que o modulo espera com FMT (H4L) em L
                                       // true: "left justified": o certo se o FMT estiver em H

i2s_chan_handle_t tx = nullptr;
int16_t buf[256 * 2];                  // 256 quadros estereo (16 bits)
int32_t buf32[256 * 2];                // idem (32 bits)
float fase = 0;

void testePinos() {
  const int pinos[3] = {PINO_BCK, PINO_DIN, PINO_LCK};
  const char *nomes[3] = {"BCK (GPIO0)", "DIN (GPIO1)", "LCK (GPIO2)"};
  for (int i = 0; i < 3; i++) { pinMode(pinos[i], OUTPUT); digitalWrite(pinos[i], LOW); }
  for (int i = 0; i < 3; i++) {
    Serial.printf("%s em 3,3 V\n", nomes[i]); digitalWrite(pinos[i], HIGH); delay(2000);
    Serial.printf("%s em 0 V\n", nomes[i]);   digitalWrite(pinos[i], LOW);  delay(2000);
  }
  for (int i = 0; i < 3; i++) pinMode(pinos[i], INPUT);
}

bool iniciaI2S() {
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.auto_clear = true;
  esp_err_t e = i2s_new_channel(&cc, &tx, nullptr);
  if (e != ESP_OK) { Serial.printf("i2s_new_channel: %s\n", esp_err_to_name(e)); return false; }
  const i2s_data_bit_width_t bits = SLOT32 ? I2S_DATA_BIT_WIDTH_32BIT : I2S_DATA_BIT_WIDTH_16BIT;
  i2s_std_slot_config_t slotPhilips = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, I2S_SLOT_MODE_STEREO);
  i2s_std_slot_config_t slotMsb = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(bits, I2S_SLOT_MODE_STEREO);
  i2s_std_config_t sc = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(TAXA),
    .slot_cfg = FORMATO_MSB ? slotMsb : slotPhilips,
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)PINO_BCK,
      .ws = (gpio_num_t)PINO_LCK,
      .dout = (gpio_num_t)PINO_DIN,
      .din = I2S_GPIO_UNUSED,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  e = i2s_channel_init_std_mode(tx, &sc);
  if (e != ESP_OK) { Serial.printf("i2s_channel_init_std_mode: %s\n", esp_err_to_name(e)); return false; }
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
      fase += 2.0f * PI * f / TAXA;
      if (fase > 2.0f * PI) fase -= 2.0f * PI;
      int16_t s = (int16_t)(sinf(fase) * VOLUME * 32767);
      buf[i * 2] = esq ? s : 0;        // canal esquerdo
      buf[i * 2 + 1] = dir ? s : 0;    // canal direito
      buf32[i * 2] = (int32_t)buf[i * 2] << 16;
      buf32[i * 2 + 1] = (int32_t)buf[i * 2 + 1] << 16;
    }
    size_t escritos = 0;
    if (SLOT32) i2s_channel_write(tx, buf32, n * 8, &escritos, portMAX_DELAY);
    else        i2s_channel_write(tx, buf, n * 4, &escritos, portMAX_DELAY);
    feitos += n;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.printf("\n=== %s ===\n", VERSAO);
  if (!PULAR_TESTE_PINOS) { Serial.println("\n--- Teste dos pinos ---"); testePinos(); }
  Serial.printf("--- Iniciando I2S (driver do IDF, formato %s) ---\n", FORMATO_MSB ? "MSB/left justified" : "Philips");
  if (!iniciaI2S()) { Serial.println("Falha ao iniciar o I2S"); while (true) delay(1000); }
  Serial.printf("I2S ok - tocando (%s, volume %.0f%%)\n", SLOT32 ? "32 bits" : "16 bits", VOLUME * 100);
}

void loop() {
  Serial.println("440 Hz so no ESQUERDO");   tocar(440, 440, 2, true, false);
  Serial.println("660 Hz so no DIREITO");    tocar(660, 660, 2, false, true);
  Serial.println("Varredura 200 -> 2000 Hz"); tocar(200, 2000, 4, true, true);
  Serial.println("Silencio");                tocar(0, 0, 1, false, false);
}
