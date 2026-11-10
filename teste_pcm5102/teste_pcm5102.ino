// Teste de bancada do modulo PCM5102A com um ESP32-C3 (core Arduino-ESP32 3.x, biblioteca ESP_I2S).
// Ligacoes (as mesmas da placa):
//   C3 GPIO0  -> BCK      C3 GPIO2  -> LCK      C3 GPIO21 -> DIN
//   C3 5V     -> VIN      C3 GND    -> GND e SCK (SCK no GND = PLL interno)
// Jumpers no verso do modulo: H1L=L  H2L=L  H3L=H  H4L=L
// Escute num fone no jack do modulo. Ciclo: 2 s so no esquerdo, 2 s so no direito,
// 4 s de varredura 200 Hz -> 2 kHz nos dois, 1 s de silencio. Repete.
#include <ESP_I2S.h>
#include <math.h>

const int PINO_BCK = 0, PINO_LCK = 2, PINO_DIN = 21;
const int TAXA = 44100;
const float VOLUME = 0.20f;            // 20% da escala: confortavel no fone

I2SClass i2s;
int16_t buf[256 * 2];                  // 256 quadros estereo
float fase = 0;

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
    }
    i2s.write((uint8_t *)buf, n * 4);
    feitos += n;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  i2s.setPins(PINO_BCK, PINO_LCK, PINO_DIN);
  if (!i2s.begin(I2S_MODE_STD, TAXA, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("Falha ao iniciar o I2S");
    while (true) delay(1000);
  }
  Serial.println("I2S ok - tocando");
}

void loop() {
  Serial.println("440 Hz so no ESQUERDO");   tocar(440, 440, 2, true, false);
  Serial.println("660 Hz so no DIREITO");    tocar(660, 660, 2, false, true);
  Serial.println("Varredura 200 -> 2000 Hz"); tocar(200, 2000, 4, true, true);
  Serial.println("Silencio");                tocar(0, 0, 1, false, false);
}
