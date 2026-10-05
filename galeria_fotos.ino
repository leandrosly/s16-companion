/*
  =====================================================================
   GALERIA DE FOTOS - exemplo minimo de TELA + TOUCH + CARTAO SD
   Placa: ES3C28P (ESP32-S3 com tela 2.8" 240x320 e touch capacitivo)
  =====================================================================

  O QUE FAZ
    Mostra 5 fotos que estao no cartao microSD. Arraste o dedo para a
    ESQUERDA para ir para a proxima foto, ou para a DIREITA para voltar.

  COMO PREPARAR O CARTAO
    1. Formate o cartao em FAT32.
    2. Crie uma pasta chamada "imagens" na raiz do cartao.
    3. Coloque la: foto1.jpg, foto2.jpg, foto3.jpg, foto4.jpg, foto5.jpg
       - tamanho 240 x 320 pixels (em pe)
       - JPG normal ("baseline"), NAO progressivo
    4. Coloque o cartao na placa e ligue.

  BIBLIOTECAS
    - LovyanGFX (lovyan03) -> tela e touch
    - SD_MMC              -> ja vem com o pacote esp32, nao precisa instalar

  CONFIGURACAO DA PLACA (menu Ferramentas)
    Placa: ESP32S3 Dev Module
    USB CDC On Boot: Enabled | USB Mode: Hardware CDC and JTAG
    Flash Size: 16MB | PSRAM: OPI PSRAM
*/

// ---------------------------------------------------------------------
// 1) INCLUDES
// ---------------------------------------------------------------------
// IMPORTANTE: o SD_MMC precisa vir ANTES da LovyanGFX. Assim a LovyanGFX
// "percebe" que existe um sistema de arquivos e ativa as funcoes que leem
// imagens direto de arquivos (como a drawJpgFile que usamos la embaixo).
#include <SD_MMC.h>

#define LGFX_USE_V1          // usa a versao 1 da LovyanGFX (a atual)
#include <LovyanGFX.hpp>


// ---------------------------------------------------------------------
// 2) DESCRICAO DO HARDWARE PARA A LOVYANGFX
// ---------------------------------------------------------------------
// A LovyanGFX funciona com centenas de telas diferentes. Por isso, a gente
// precisa "contar" para ela como a NOSSA placa esta montada: qual chip a
// tela usa, em quais pinos ela esta ligada, onde fica o touch, etc.
// Fazemos isso criando uma classe (LGFX) que junta 4 pecas:
//
//   _bus   -> o "cabo" de comunicacao com a tela (SPI)
//   _panel -> a tela em si (chip ILI9341)
//   _light -> o controle da luz de fundo (backlight)
//   _touch -> o chip do touch (FT6336, que e da familia FT5x06)
//
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel;
  lgfx::Bus_SPI       _bus;
  lgfx::Light_PWM     _light;
  lgfx::Touch_FT5x06  _touch;

public:
  LGFX() {
    // ----- Barramento SPI (como o ESP32 conversa com a tela) -----
    {
      auto cfg = _bus.config();
      cfg.spi_host   = SPI2_HOST;        // qual controlador SPI do ESP32 usar
      cfg.spi_mode   = 0;
      cfg.freq_write = 40000000;         // 40 MHz para escrever (rapido)
      cfg.freq_read  = 16000000;         // 16 MHz para ler
      cfg.dma_channel = SPI_DMA_CH_AUTO; // DMA = transfere sem ocupar o processador
      cfg.pin_sclk = 12;                 // clock
      cfg.pin_mosi = 11;                 // dados ESP32 -> tela
      cfg.pin_miso = 13;                 // dados tela -> ESP32
      cfg.pin_dc   = 46;                 // diz se o que vai e "comando" ou "dado"
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }

    // ----- A tela -----
    {
      auto cfg = _panel.config();
      cfg.pin_cs   = 10;                 // chip select da tela
      cfg.pin_rst  = -1;                 // reset compartilhado com o botao RST da placa
      cfg.panel_width  = 240;
      cfg.panel_height = 320;
      cfg.invert    = true;              // este painel precisa de cores invertidas
      cfg.rgb_order = false;             // ordem BGR (padrao do ILI9341)
      cfg.readable  = true;
      _panel.config(cfg);
    }

    // ----- Luz de fundo -----
    {
      auto cfg = _light.config();
      cfg.pin_bl = 45;                   // pino que liga a luz de fundo
      cfg.freq = 44100;                  // PWM permite controlar o brilho
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }

    // ----- Touch (FT6336 via I2C) -----
    {
      auto cfg = _touch.config();
      cfg.i2c_port = 0;
      cfg.i2c_addr = 0x38;               // endereco I2C do chip de touch
      cfg.pin_sda  = 16;                 // dados I2C
      cfg.pin_scl  = 15;                 // clock I2C
      cfg.pin_int  = 17;                 // avisa quando ha toque
      cfg.pin_rst  = 18;                 // reset do chip de touch
      cfg.freq     = 400000;
      // Faixa de coordenadas que o touch devolve (igual ao tamanho da tela)
      cfg.x_min = 0;  cfg.x_max = 239;
      cfg.y_min = 0;  cfg.y_max = 319;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }

    setPanel(&_panel);
  }
};

LGFX tela;   // o objeto que vamos usar no resto do programa


// ---------------------------------------------------------------------
// 3) PINOS DO CARTAO SD (do manual do fabricante)
// ---------------------------------------------------------------------
// O slot usa o modo "SDIO". Ele pode usar 4 linhas de dados, mas o modo
// de 1 linha so precisa de 3 pinos e ja e rapido o bastante para fotos.
const int SD_CLK = 38;
const int SD_CMD = 40;
const int SD_D0  = 39;


// ---------------------------------------------------------------------
// 4) A LISTA DE FOTOS
// ---------------------------------------------------------------------
const char* fotos[] = {
  "/imagens/foto1.jpg",
  "/imagens/foto2.jpg",
  "/imagens/foto3.jpg",
  "/imagens/foto4.jpg",
  "/imagens/foto5.jpg",
};
// Conta quantos itens tem a lista (tamanho total / tamanho de um item).
// Assim, se voce acrescentar fotos na lista, nao precisa mudar mais nada.
const int TOTAL_FOTOS = sizeof(fotos) / sizeof(fotos[0]);

int fotoAtual = 0;   // qual foto esta na tela (0 = a primeira)


// ---------------------------------------------------------------------
// 5) FUNCOES AUXILIARES
// ---------------------------------------------------------------------

// Mostra uma mensagem de texto no meio da tela (usado para erros)
void mostrarMensagem(const char* linha1, const char* linha2) {
  tela.fillScreen(TFT_BLACK);
  tela.setTextDatum(middle_center);        // texto centralizado no ponto dado
  tela.setFont(&fonts::FreeSansBold12pt7b);
  tela.setTextColor(TFT_WHITE);
  tela.drawString(linha1, 120, 145);
  tela.setFont(&fonts::Font2);
  tela.setTextColor(TFT_LIGHTGREY);
  tela.drawString(linha2, 120, 180);
}

// Desenha as "bolinhas" de posicao na parte de baixo (tipo galeria de celular)
void desenharIndicador() {
  const int raio = 4;
  const int espaco = 16;                              // distancia entre bolinhas
  int xInicial = 120 - (TOTAL_FOTOS - 1) * espaco / 2; // centraliza o conjunto
  for (int i = 0; i < TOTAL_FOTOS; i++) {
    int x = xInicial + i * espaco;
    if (i == fotoAtual) tela.fillCircle(x, 308, raio, TFT_WHITE);   // atual: cheia
    else                tela.drawCircle(x, 308, raio, TFT_WHITE);   // outras: vazia
  }
}

// Carrega a foto do cartao e desenha na tela
void mostrarFoto(int indice) {
  Serial.printf("Mostrando %s\n", fotos[indice]);
  tela.fillScreen(TFT_BLACK);   // limpa (caso a foto seja menor que a tela)

  // drawJpgFile le o arquivo do cartao, decodifica o JPG e desenha.
  // Parametros: (sistema de arquivos, caminho, x, y)
  bool ok = tela.drawJpgFile(SD_MMC, fotos[indice], 0, 0);

  if (!ok) {
    mostrarMensagem("Erro na foto", fotos[indice]);
    Serial.println("  -> nao abriu: confira o nome e se o JPG nao e progressivo");
  }
  desenharIndicador();
}


// ---------------------------------------------------------------------
// 6) SETUP - roda uma vez quando a placa liga
// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);

  // Liga a tela
  tela.init();
  tela.setRotation(0);       // 0 = em pe (retrato)
  tela.setBrightness(200);   // 0 a 255

  // Liga o cartao SD
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);   // diz em quais pinos o cartao esta
  // begin(ponto_de_montagem, modo_1_linha)
  if (!SD_MMC.begin("/sdcard", true)) {
    mostrarMensagem("Sem cartao SD", "Insira o cartao e reinicie");
    Serial.println("Falha ao montar o cartao SD");
    while (true) delay(1000);   // para aqui: sem cartao nao ha o que mostrar
  }
  Serial.printf("Cartao SD OK: %llu MB\n", SD_MMC.cardSize() / (1024 * 1024));

  mostrarFoto(fotoAtual);
}


// ---------------------------------------------------------------------
// 7) LOOP - roda sem parar
// ---------------------------------------------------------------------
// COMO DETECTAMOS O "ARRASTAR":
//   - quando o dedo ENCOSTA, guardamos onde ele comecou (xInicio)
//   - enquanto ele esta na tela, vamos atualizando onde ele esta (xAtual)
//   - quando o dedo SOLTA, comparamos o fim com o inicio:
//       andou muito para a esquerda  -> proxima foto
//       andou muito para a direita   -> foto anterior
//       andou pouco                  -> foi so um toque, ignoramos
//
const int DISTANCIA_MINIMA = 50;   // pixels que o dedo precisa andar para contar

bool dedoNaTela = false;
int xInicio = 0;
int xAtual  = 0;

void loop() {
  int32_t x, y;

  // getTouch devolve a quantidade de toques (0 = ninguem tocando)
  // e preenche x e y com a posicao do dedo.
  bool tocando = tela.getTouch(&x, &y) > 0;

  if (tocando) {
    if (!dedoNaTela) {        // o dedo acabou de encostar
      dedoNaTela = true;
      xInicio = x;
    }
    xAtual = x;               // vai acompanhando o dedo
  }
  else if (dedoNaTela) {      // nao esta tocando, mas estava -> o dedo soltou
    dedoNaTela = false;
    int deslocamento = xAtual - xInicio;   // negativo = foi para a esquerda

    if (deslocamento < -DISTANCIA_MINIMA) {
      // Proxima foto. O "%" (resto da divisao) faz voltar para a primeira
      // depois da ultima: 4 + 1 = 5, e 5 % 5 = 0.
      fotoAtual = (fotoAtual + 1) % TOTAL_FOTOS;
      mostrarFoto(fotoAtual);
    }
    else if (deslocamento > DISTANCIA_MINIMA) {
      // Foto anterior. Somamos TOTAL_FOTOS antes do "%" para nao ficar negativo:
      // da primeira (0) volta para a ultima (4).
      fotoAtual = (fotoAtual - 1 + TOTAL_FOTOS) % TOTAL_FOTOS;
      mostrarFoto(fotoAtual);
    }
  }

  delay(10);   // pequena pausa; o touch nao precisa ser lido mais rapido que isso
}
