/*
  =====================================================================
   GALERIA DE FOTOS - exemplo minimo de TELA + TOUCH
   Placa: ES3C28P (ESP32-S3 com tela 2.8" 240x320 e touch capacitivo)
  =====================================================================

  O QUE FAZ
    Mostra as fotos que foram gravadas JUNTO com o programa na memoria do
    ESP32. Arraste o dedo para a ESQUERDA para ir para a proxima foto,
    ou para a DIREITA para voltar.

  COMO AS FOTOS ENTRAM NA PLACA
    O Arduino so grava na placa o que e CODIGO. Por isso as fotos sao
    transformadas em codigo: cada JPG vira uma lista de bytes no arquivo
    "fotos.h", que e compilado junto com este sketch.

    galeria_fotos/
      galeria_fotos.ino     <- este arquivo
      converter_fotos.py    <- script que gera o fotos.h
      fotos.h               <- gerado pelo script (nao edite a mao)
      imagens/
        foto1.jpg ... foto5.jpg   <- suas fotos, de qualquer tamanho

    1. Coloque as fotos na pasta "imagens"
    2. Rode:  python converter_fotos.py   (precisa do: pip install pillow)
    3. Grave o sketch normalmente

  BIBLIOTECAS
    - LovyanGFX (lovyan03) -> tela e touch

  CONFIGURACAO DA PLACA (menu Ferramentas)
    Placa: ESP32S3 Dev Module
    USB CDC On Boot: Enabled | USB Mode: Hardware CDC and JTAG
    Flash Size: 16MB | PSRAM: OPI PSRAM   <- obrigatorio (as 3 fotos em memoria usam ~450 KB)
*/

// ---------------------------------------------------------------------
// 1) INCLUDES
// ---------------------------------------------------------------------
#define LGFX_USE_V1          // usa a versao 1 da LovyanGFX (a atual)
#include <LovyanGFX.hpp>

// As fotos (gerado pelo converter_fotos.py). As aspas, em vez de < >, dizem
// ao compilador para procurar o arquivo na pasta do proprio sketch.
#include "fotos.h"


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
// Se, ao arrastar, a foto andar no sentido CONTRARIO ao do dedo, o touch
// esta "espelhado" em relacao a tela. Troque o valor do eixo errado (true <-> false).
// Nesta placa os DOIS eixos vem invertidos: o touch esta "de cabeca para baixo"
// (girado 180 graus) em relacao a imagem.
const bool INVERTER_TOUCH_X = true;   // esquerda <-> direita
const bool INVERTER_TOUCH_Y = true;   // cima <-> baixo

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
      // Para espelhar, basta inverter o minimo e o maximo do eixo X.
      if (INVERTER_TOUCH_X) { cfg.x_min = 239; cfg.x_max = 0; }
      else                  { cfg.x_min = 0;   cfg.x_max = 239; }
      if (INVERTER_TOUCH_Y) { cfg.y_min = 319; cfg.y_max = 0; }
      else                  { cfg.y_min = 0;   cfg.y_max = 319; }
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }

    setPanel(&_panel);
  }
};

LGFX tela;   // o objeto que vamos usar no resto do programa


// ---------------------------------------------------------------------
// 3) AS FOTOS E AS "TELAS INVISIVEIS" (SPRITES)
// ---------------------------------------------------------------------
// A lista "fotos" e o numero TOTAL_FOTOS vem prontos do fotos.h.
//
// Para a foto poder DESLIZAR, nao da para desenhar direto na tela: decodificar
// um JPG leva uns 40 ms, e a animacao travaria. A solucao e usar SPRITES:
// imagens 240x320 guardadas na memoria (PSRAM), ja decodificadas, que a gente
// "carimba" na tela em qualquer posicao, bem rapido.
//
// Mantemos 3 sprites prontos: a foto ANTERIOR, a ATUAL e a PROXIMA.
// Ao arrastar, a atual anda junto com o dedo e a vizinha aparece colada nela.
//
//      [ anterior ][   ATUAL   ][ proxima ]
//                  ^ a tela mostra so esta janela de 240 pixels

LGFX_Sprite spriteA(&tela), spriteB(&tela), spriteC(&tela);

// Ponteiros: dizem qual sprite faz o papel de anterior/atual/proxima.
// Ao trocar de foto, so "rodamos" os papeis, sem copiar 150 KB de imagem.
LGFX_Sprite* anterior = &spriteA;
LGFX_Sprite* atual    = &spriteB;
LGFX_Sprite* proxima  = &spriteC;

int fotoAtual = 0;   // qual foto esta na tela (0 = a primeira)

// Indices das vizinhas, dando a volta nas pontas (da ultima volta para a primeira)
int indiceAnterior() { return (fotoAtual - 1 + TOTAL_FOTOS) % TOTAL_FOTOS; }
int indiceProxima()  { return (fotoAtual + 1) % TOTAL_FOTOS; }


// ---------------------------------------------------------------------
// 4) FUNCOES AUXILIARES
// ---------------------------------------------------------------------

// Cria um sprite do tamanho da tela na PSRAM. Devolve false se faltar memoria.
bool criarSprite(LGFX_Sprite& sp) {
  sp.setPsram(true);       // usa a PSRAM (8 MB) em vez da RAM interna (512 KB)
  sp.setColorDepth(16);    // 16 bits por pixel, o mesmo que a tela usa
  return sp.createSprite(240, 320) != nullptr;
}

// Decodifica uma foto DENTRO de um sprite (nada aparece na tela ainda)
void carregarFoto(LGFX_Sprite* sp, int indice) {
  sp->fillScreen(TFT_BLACK);
  if (!sp->drawJpg(fotos[indice].dados, fotos[indice].tamanho, 0, 0)) {
    sp->setTextDatum(middle_center);
    sp->setFont(&fonts::Font2);
    sp->setTextColor(TFT_WHITE);
    sp->drawString("Erro na foto", 120, 160);
  }
}

// Desenha a cena com a foto atual deslocada "dx" pixels para o lado.
//   dx < 0: a atual foi para a esquerda -> a proxima aparece pela direita
//   dx > 0: a atual foi para a direita  -> a anterior aparece pela esquerda
// O que fica fora da tela e simplesmente cortado pela biblioteca.
void desenharDeslocado(int dx) {
  atual->pushSprite(dx, 0);
  if (dx < 0)      proxima->pushSprite(dx + 240, 0);
  else if (dx > 0) anterior->pushSprite(dx - 240, 0);
}

// Anima o deslocamento de "de" ate "para", rapido no inicio e freando no fim
// (fica mais natural do que andar sempre na mesma velocidade).
void animar(int de, int para) {
  const int QUADROS = 10;                    // mais quadros = animacao mais longa
  for (int i = 1; i <= QUADROS; i++) {
    float t = (float)i / QUADROS;            // vai de 0.1 ate 1.0
    float suave = 1 - (1 - t) * (1 - t) * (1 - t);   // "ease-out": desacelera no fim
    desenharDeslocado(de + (para - de) * suave);
  }
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

// Avanca para a proxima foto: os papeis "andam uma casa" para a esquerda.
//   antes:  anterior=A  atual=B  proxima=C
//   depois: anterior=B  atual=C  proxima=A  (A e reaproveitado e recebe a nova vizinha)
void avancar() {
  LGFX_Sprite* sobra = anterior;
  anterior = atual;
  atual = proxima;
  proxima = sobra;
  fotoAtual = indiceProxima();
  carregarFoto(proxima, indiceProxima());   // so precisa decodificar UMA foto nova
}

// Volta para a foto anterior: o mesmo, no sentido contrario.
void voltar() {
  LGFX_Sprite* sobra = proxima;
  proxima = atual;
  atual = anterior;
  anterior = sobra;
  fotoAtual = indiceAnterior();
  carregarFoto(anterior, indiceAnterior());
}


// ----- ZOOM -----
// O zoom e guardado como: o FATOR (1.0 = normal, 2.0 = dobro) e o PONTO DA FOTO
// que fica no centro da tela (centroX, centroY), em coordenadas da foto (0-239, 0-319).
const float ZOOM_MAXIMO = 2.0;
float zoom = 1.0;
float centroX = 120, centroY = 160;

// Com zoom, a tela mostra so um pedaco da foto. Esta funcao impede que esse
// pedaco "saia" da foto (o que deixaria lixo nas bordas da tela).
void limitarCentro() {
  float meiaLargura = 120 / zoom;    // metade da largura visivel, em pixels da foto
  float meiaAltura  = 160 / zoom;
  centroX = constrain(centroX, meiaLargura, 240 - meiaLargura);
  centroY = constrain(centroY, meiaAltura,  320 - meiaAltura);
}

// Desenha a foto atual com o zoom e o centro atuais.
void desenharZoom() {
  if (zoom <= 1.001f) {              // sem zoom: carimbo simples, mais rapido
    atual->pushSprite(0, 0);
    return;
  }
  // pushRotateZoom pega o "pivo" do sprite, coloca no ponto (120,160) da tela
  // e amplia em volta dele. O pivo e justamente o ponto da foto que queremos no centro.
  atual->setPivot(centroX, centroY);
  atual->pushRotateZoom(&tela, 120, 160, 0, zoom, zoom);   // (destino, x, y, angulo, zoomX, zoomY)
}

// ---------------------------------------------------------------------
// 5) SETUP - roda uma vez quando a placa liga
// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);

  // Liga a tela
  tela.init();
  tela.setRotation(0);       // 0 = em pe (retrato)
  tela.setBrightness(200);   // 0 a 255

  Serial.printf("%d fotos gravadas junto com o programa\n", TOTAL_FOTOS);

  // Cria os 3 sprites. Se falhar, quase sempre e a PSRAM desligada no menu Ferramentas.
  if (!criarSprite(spriteA) || !criarSprite(spriteB) || !criarSprite(spriteC)) {
    tela.fillScreen(TFT_BLACK);
    tela.setTextDatum(middle_center);
    tela.setTextColor(TFT_RED);
    tela.setFont(&fonts::Font2);
    tela.drawString("Sem memoria: ligue a PSRAM", 120, 160);
    tela.drawString("(Ferramentas > PSRAM > OPI)", 120, 180);
    while (true) delay(1000);
  }

  // Sorteia a foto inicial. esp_random() usa o gerador de numeros aleatorios
  // do proprio chip (baseado em ruido eletrico), entao muda a cada vez que liga.
  // O "%" transforma o numero sorteado (enorme) em um indice de 0 a TOTAL_FOTOS-1.
  fotoAtual = esp_random() % TOTAL_FOTOS;
  Serial.printf("Comecando pela foto %d (%s)\n", fotoAtual + 1, fotos[fotoAtual].nome);

  // Prepara as 3 fotos iniciais e mostra a atual
  carregarFoto(atual, fotoAtual);
  carregarFoto(anterior, indiceAnterior());
  carregarFoto(proxima, indiceProxima());
  desenharDeslocado(0);
  desenharIndicador();
}


// ---------------------------------------------------------------------
// 6) LOOP - roda sem parar
// ---------------------------------------------------------------------
// O touch desta placa (FT6336) entende ate 2 DEDOS ao mesmo tempo.
// Com isso o loop reconhece 4 gestos:
//
//   ARRASTAR (1 dedo, sem zoom) -> desliza para a foto vizinha
//   MOVER    (1 dedo, com zoom) -> passeia pela foto ampliada
//   PINCA    (2 dedos)          -> afastar/aproximar os dedos muda o zoom
//   TOQUE DUPLO                 -> alterna entre normal e 2x, no ponto tocado
//
// Cada gesto comeca quando o primeiro dedo encosta e termina quando TODOS soltam.

enum Gesto { NENHUM, ARRASTAR, MOVER, PINCA };
Gesto gesto = NENHUM;

const int DISTANCIA_MINIMA = 60;   // pixels que o dedo precisa andar para trocar de foto
const int TOQUE_MAX_MOVIMENTO = 12;     // ate isso, o dedo "nao andou" (foi um toque)
const unsigned long TOQUE_MAX_MS = 250; // toque rapido
const unsigned long DUPLO_MAX_MS = 350; // intervalo maximo entre os 2 toques

int xInicio = 0, yInicio = 0;      // onde o gesto comecou
int xUltimo = 0, yUltimo = 0;      // ultima posicao lida (para o MOVER)
int maiorMovimento = 0;            // o quanto o dedo chegou a andar no gesto
unsigned long inicioGesto = 0;     // quando o gesto comecou
unsigned long ultimoToque = 0;     // quando foi o ultimo toque rapido (para o duplo)
int dxAtual = 0;                   // deslocamento da foto no ARRASTAR
float distanciaInicial = 0;        // distancia entre os 2 dedos no inicio da PINCA
float zoomInicial = 1;             // zoom no inicio da PINCA

// Distancia entre dois pontos (Pitagoras)
float distancia(const lgfx::touch_point_t& a, const lgfx::touch_point_t& b) {
  float dx = a.x - b.x, dy = a.y - b.y;
  return sqrtf(dx * dx + dy * dy);
}

// Chamado quando todos os dedos soltam: finaliza o gesto que estava acontecendo
void terminarGesto() {
  // Foi um toque rapido, sem arrastar? Entao pode ser a metade de um toque duplo.
  bool foiToque = maiorMovimento < TOQUE_MAX_MOVIMENTO &&
                  millis() - inicioGesto < TOQUE_MAX_MS && gesto != PINCA;

  if (gesto == ARRASTAR) {          // (num toque rapido, dxAtual e pequeno e a foto so volta ao lugar)
    if (dxAtual < -DISTANCIA_MINIMA)      { animar(dxAtual, -240); avancar(); }
    else if (dxAtual > DISTANCIA_MINIMA)  { animar(dxAtual,  240); voltar();  }
    else                                  { animar(dxAtual, 0); }
    dxAtual = 0;
    desenharDeslocado(0);
    desenharIndicador();
  }

  if (gesto == PINCA && zoom < 1.05f) {   // pinca quase fechada: volta ao normal
    zoom = 1;
    desenharDeslocado(0);
    desenharIndicador();
  }

  if (foiToque) {
    if (millis() - ultimoToque < DUPLO_MAX_MS) {   // segundo toque: e um toque DUPLO
      ultimoToque = 0;
      if (zoom > 1.001f) {
        zoom = 1;                                  // ja tinha zoom: volta ao normal
        desenharDeslocado(0);
        desenharIndicador();
      } else {
        zoom = ZOOM_MAXIMO;                        // amplia no ponto tocado
        centroX = xInicio;                         // sem zoom, tela e foto coincidem
        centroY = yInicio;
        limitarCentro();
        desenharZoom();
      }
    } else {
      ultimoToque = millis();                      // primeiro toque: espera o segundo
    }
  }

  gesto = NENHUM;
}

void loop() {
  // Le ate 2 dedos. "n" diz quantos estao encostados (0, 1 ou 2).
  lgfx::touch_point_t dedos[2];
  int n = tela.getTouch(dedos, 2);

  if (n == 0) {
    if (gesto != NENHUM) terminarGesto();
    delay(5);
    return;
  }

  // ----- inicio de um gesto -----
  if (gesto == NENHUM) {
    xInicio = xUltimo = dedos[0].x;
    yInicio = yUltimo = dedos[0].y;
    maiorMovimento = 0;
    inicioGesto = millis();
    gesto = (zoom > 1.001f) ? MOVER : ARRASTAR;
  }

  // ----- dois dedos: vira PINCA (mesmo que o gesto tenha comecado com um) -----
  if (n >= 2) {
    if (gesto != PINCA) {
      if (gesto == ARRASTAR && dxAtual != 0) {   // estava deslizando: desfaz
        dxAtual = 0;
        desenharDeslocado(0);
      }
      gesto = PINCA;
      distanciaInicial = max(distancia(dedos[0], dedos[1]), 10.0f);
      zoomInicial = zoom;
    }
    // Dedos 30% mais afastados que no inicio = 30% mais zoom
    zoom = constrain(zoomInicial * distancia(dedos[0], dedos[1]) / distanciaInicial,
                     1.0f, ZOOM_MAXIMO);
    limitarCentro();
    desenharZoom();
    return;
  }

  // ----- um dedo -----
  int x = dedos[0].x, y = dedos[0].y;
  maiorMovimento = max(maiorMovimento, max(abs(x - xInicio), abs(y - yInicio)));

  if (gesto == ARRASTAR && TOTAL_FOTOS > 1) {
    int dx = constrain(x - xInicio, -240, 240);
    if (dx != dxAtual) { dxAtual = dx; desenharDeslocado(dxAtual); }
  }
  else if (gesto == MOVER) {
    // O dedo andou N pixels na TELA = N/zoom pixels na FOTO.
    // O sinal e negativo: arrastar para a direita mostra o que esta a ESQUERDA.
    centroX -= (x - xUltimo) / zoom;
    centroY -= (y - yUltimo) / zoom;
    limitarCentro();
    desenharZoom();
  }
  // (se for PINCA e um dos dedos soltou, so espera o outro soltar)

  xUltimo = x;
  yUltimo = y;
  delay(5);
}
