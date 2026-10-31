// VERSAO: v6-202610262207   (aparece no Serial ao ligar e na cortina de Ajustes)
/*
  Ponte BLE KingSong KS-S16 <-> ESP32-S3 (ES3C28P 2.8") <-> Celular
  v5 "MIDIA": ate 2 apps + controle de midia (botoes de musica/volume direto no celular)

  O radio e configurado pelo CONFIG_BT_NIMBLE_MAX_CONNECTIONS do nimconfig.h: com o
  anuncio estendido, conexoes + anuncios precisam caber em MAX-1. Com 6 (patch), cabem
  roda + 2 apps + controle de midia.

  - Conecta na roda, descobre TODOS os servicos/caracteristicas.
  - Recria todos eles no servidor do ESP32 (exceto 0x1800/0x1801, que a pilha ja tem),
    com as mesmas propriedades. Valores legiveis (ex.: Device Information 0x180A)
    sao copiados.
  - Anuncia com o mesmo nome, UUIDs e dados de fabricante da roda.
  - Repassa CRU: notificacoes de qualquer caracteristica -> celular;
    escritas do celular em qualquer caracteristica -> roda.
  - FFE1 e sempre assinada (para a tela); as demais so quando o celular assina.

  Novidades (12/10/2026):
  - Cortina de AJUSTES: arraste pra baixo comecando na barra de cima (funciona ate
    sem a roda). Botoes: MPU > (tela de config), Tela auto, Som, Mochila, Print, Fechar.
  - Tela de config do MPU: GRAVAR, AUTO, PADRAO e barras dos angulos (salvos).
  - Barra de cima so com icones + bateria do display (IO9) e tudo cinza quando inativo.
  - Print da tela em BMP no SD (/prints). Serial: print | bat | 433 raw <hexa>.
  - Botoes dos piscas: antirrepique novo (sem o "duplo clique").
  - Carregando: raio no icone da bateria (USB do PC ou "degrau" de tensao; ver batLoop).
  - Print pela cortina nao fecha mais a cortina. Serial: 'comandos' lista tudo.
  - Pico do PWM na barra (segura 5 s, depois desce) + "pico NN%" ao lado.
  - '433 raw' com os tempos medidos do controle da buzina (e tempos opcionais).
  - RECEPTOR 433 no IO3: o controle da buzina vira entrada do display (buzina da roda,
    piscas, buzina externa). Botoes fisicos dos piscas desligados (BOTOES_PISCA).
  - Tela da buzina externa (7 funcoes) na cortina; cadeado fechado = desliga o alarme dela.
  Novidades (13/10/2026):
  - DORME sozinho: 10 min sem roda e sem USB, ou bateria no fim (< 3,30 V). BOOT acorda.
  - Raio de "carregando" nao fica mais preso depois de bipe/alarme/433.
  Novidades (14/10/2026):
  - Paginas de deslizar so BMS / principal / controles. PISCAS = cortina de BAIXO
    (arrastar pra cima a partir da borda, onde fica a "alca"); MOCHILA = botao da cortina.
  - Botao Dormir na cortina (2 toques). Controle -> 433 responde mais rapido (80 ms).
  - Mochila: botao mostra CONECTANDO... / PROCURANDO... (retorno visual).
  - "Acorda mexendo" (cortina): no sono, o INT do MPU (IO14) tambem acorda. Salvo.
  - Dormir apaga piscas E mochila; acordar devolve os dois como estavam (estado da
    mochila agora salvo na flash). Indicador das lanternas dos lados da velocidade.
  - LUZ DE FREIO: corrente < -2 A andando -> piscas vermelhos (sem pisca de curva) e
    mochila vermelha no maximo; depois volta como estava. Botao na cortina dos piscas.
  Novidades (15/10/2026):
  - PAREAMENTO_V2: o S16 Controle nao repareia mais ao reiniciar/dormir (privacidade
    no host da NimBLE; ver README). Chave PAREAMENTO_V2 no topo.
  - TELA_GIRADA (Serial): gira a tela e o touch 180 graus. Salvo.
  Novidades (16/10/2026):
  - Gravacao da viagem LEMBRADA: o botao do log liga a intencao (salva); a roda
    sumiu -> fecha o arquivo; voltou (ate depois de dormir) -> abre outro sozinho.
    Icone laranja = ligada, esperando a roda; vermelho = gravando.
  Novidades (26/10/2026) - DISPLAY v6 (PONTE_C3):
  - Com PONTE_C3 1 o display conecta no C3 fixo na roda: nao anuncia mais a copia
    da roda, nao usa o radio 433; piscas/buzina externa/freio viram comandos para o
    C3 (canal proprio na FFE1) e os indicadores mostram o estado que o C3 manda.
  Novidades (22/10/2026):
  - '433 teste': rajadas numeradas (00 9F 1E NN) para o medidor_433 (C3 + LCD 16x2)
    medir o alcance enquanto se posiciona a antena.
  Novidades (17/10/2026) - correcoes da revisao do Claude Code:
  - Mochila ausente nao trava mais o loop andando (so tenta parado; espera cresce).
  - msSemDados(): fim da disputa entre nucleos no "tempo sem dados" (gravacao/freio).
  - Gravacao so fecha apos 10 s sem dados (queda curta do BLE nao divide a viagem).
  - Console: "433"/"som"/"dream"/"mpu" sozinhos nao repetem mais o comando anterior.
  - Log desliga pelo toque mesmo sem cartao; parseBms checa o tamanho.

  Bibliotecas: NimBLE-Arduino 2.5.1 COM O PATCH (pasta patch_nimble_2.5.1), LovyanGFX (lovyan03)
  Placa: "ESP32S3 Dev Module"
         USB CDC On Boot: Enabled | USB Mode: Hardware CDC and JTAG
         Flash Size: 16MB | PSRAM: OPI PSRAM | JTAG Adapter: Disabled
         Partition Scheme: "16M Flash (3MB APP/9.9MB FATFS)" (ou qualquer uma de 16MB)
*/

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <SD_MMC.h>          // antes da LovyanGFX
#include <sys/time.h>
#include <esp_mac.h>          // MAC do chip, para gerar o endereco do "S16 Controle"
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <vector>
#include <Preferences.h>     // guarda a configuracao na flash (sobrevive ao desligar)
#include <ESP_I2S.h>         // audio para o piezo/alto-falante (codec ES8311)
#include <esp_sleep.h>        // dormir (deep sleep) sem roda/USB ou com a bateria no fim
#include <driver/rtc_io.h>
#include <driver/gpio.h>

// Chave de teste: 0 desliga TODO o som (nao inicia I2S/codec/tarefa). Serve para
// isolar se um problema vem do piezo ou de outra mudanca. 1 = som normal.
#define USAR_SOM 1

// Chave do PAREAMENTO do "S16 Controle" (ver README, secao "Pareamento V2"):
//   0 = jeito antigo (testado): apps OK, mas o controle de midia repareia
//       sempre que o display reinicia.
//   1 = V2: pede a identidade (IRK) do celular e RESOLVE o endereco dele no
//       HOST (software da NimBLE), sem ligar a resolucao no controlador
//       (que era o que quebrava os apps). Tambem para de mandar a NOSSA
//       identidade (ela e o endereco da roda e confundia o celular).
//       EXIGE o nimconfig.h do patch com CONFIG_BT_NIMBLE_HOST_BASED_PRIVACY 1.
// Para voltar atras: ponha 0 aqui (o nimconfig.h novo pode ficar).
#define PAREAMENTO_V2 1

#if PAREAMENTO_V2 && (!defined(CONFIG_BT_NIMBLE_HOST_BASED_PRIVACY) || !CONFIG_BT_NIMBLE_HOST_BASED_PRIVACY)
#error "PAREAMENTO_V2 precisa do nimconfig.h do patch (CONFIG_BT_NIMBLE_HOST_BASED_PRIVACY 1) copiado para a biblioteca NimBLE-Arduino/src. Ou ponha PAREAMENTO_V2 0."
#endif
#if PAREAMENTO_V2
// Da NimBLE (C): quantos celulares tem "registro de identidade" para a
// resolucao no host. So existe com a privacidade no host ligada.
extern "C" int ble_rpa_get_num_peer_dev_records(void);
#endif

// PONTE_C3 (etapa 3 do plano "C3 na roda", ver README):
//   1 = o display conecta no C3 fixo na roda (que se apresenta igual a roda).
//       O display NAO anuncia mais a copia da roda (os apps conectam no C3),
//       NAO usa mais o radio 433 (o C3 faz piscas, buzina, freio e le o
//       controle); os botoes da tela viram comandos para o C3 por um canal
//       proprio dentro da FFE1. Continua: tela, "S16 Controle", mochila, MPU,
//       som, logs. Use TARGET_MAC = <MAC do C3> (o C3 mostra no 'status').
//   0 = como antes (o display e a ponte e tem o radio 433).
#define PONTE_C3 1

// Canal display <-> C3: quadros de 20 bytes na FFE1 com a assinatura
//   [0..1] AA 55 | [2] tipo (01 comando, 02 estado) | ... | [16] EE [17..19] 'S' '1' '6'
// A roda nunca manda isso; o C3 nao repassa para a roda nem para os apps.
static const uint8_t C3_MARCA[4] = { 0xEE, 0x53, 0x31, 0x36 };
enum { C3_HELLO = 1, C3_PISCA = 2, C3_LUZBASE = 3, C3_BUZEXT = 4, C3_FREIOAUTO = 5, C3_FREIOTESTE = 6, C3_TESTE433 = 7 };
static volatile bool    c3Ok = false;          // o C3 respondeu (recebemos estado dele)
static volatile uint32_t c3EstadoEm = 0;       // millis do ultimo estado
static volatile uint8_t c3Luz = 0, c3Pisca = 0, c3Freio = 0, c3FreioAuto = 1, c3Teste = 0, c3Radio = 0;
static volatile uint8_t c3EvBtn = 0, c3EvSeq = 0;
static volatile uint8_t c3Apps = 0;            // apps conectados no C3 (sem contar o display)

// Versao deste arquivo (data e hora em que foi gerado: AAAAMMDDHHMM). Mesma da 1a linha.
static const char* VERSAO = "v6-202610262207";

// ================= Configuracao =================
// Estes sao os valores PADRAO. Da para mudar pelo Serial sem regravar, por exemplo:
//     DUMP_OTHER = false        (ou "DUMP_OTHER 0", "DUMP_OTHER off")
//     config                    (mostra todos os valores atuais)
//     config padrao             (volta tudo para os valores abaixo)
// As mudancas ficam guardadas na flash e valem tambem depois de desligar.
// NAME_FILTER e TARGET_MAC so fazem efeito na proxima busca pela roda (reinicie o display).
static String NAME_FILTER = "KS";
static String TARGET_MAC  = "";            // ex.: "c5:39:32:38:4c:4b"  (vazio = usa o NAME_FILTER)
static bool DUMP_HEX   = false;            // frames da FFE1 vindos da roda (R>) - muito volume
static bool DUMP_PHONE = true;             // tudo que o CELULAR faz: leituras, escritas, assinaturas
static bool DUMP_NOVOS = true;             // frames da roda de tipos que NAO aparecem no fluxo normal (R?)
static bool DUMP_OTHER = true;             // trafego nas OUTRAS caracteristicas (raro, interessante)
// Quando dois apps dividem a MESMA conexao (o Android faz isso por conta propria), um app que
// fecha "cancela a assinatura" da FFE1 e corta os dados do outro junto - era o Mono Riders
// derrubando o EUC World. Com esta opcao, o display ignora o cancelamento e continua mandando
// os dados enquanto a conexao existir. Quem nao quiser mais dados fecha a conexao de verdade.
static bool IGNORA_CANCELAR = true;

// MPU-6050: acende a tela quando o pulso esta na posicao de olhar, apaga
// quando abaixa. Comeca DESLIGADO: so vale depois de "mpu on" + "mpu gravar".
static bool TELA_AUTO = false;

// Som (piezo/alto-falante pelo codec ES8311). SOM_ON liga/desliga os bipes.
static bool SOM_ON = true;

// Mochila Dream: manter conectado automaticamente (conexao bloqueante com
// timeout curto; a assincrona e que estourava o buffer). Salvo na config.
static bool DREAM_AUTO = true;

// Acordar do sono MEXENDO o braco (INT do MPU no IO14). Opcao porque, guardado na
// mochila, ele acordaria a cada sacolejo. Nunca vale no sono por bateria no fim.
static bool ACORDA_MEXENDO = false;

// TELA GIRADA 180 graus (imagem e touch juntos). false = como sempre foi;
// true = de cabeca pra baixo (lado largo/USB-C do outro lado). Salvo na flash;
// muda pelo Serial: TELA_GIRADA = true / false (vale na hora).
static bool TELA_GIRADA = false;

// GRAVACAO DA VIAGEM "lembrada": e a INTENCAO (botao do log). Com ela ligada o display
// grava enquanto a roda manda dados; a roda some (desligou, dormiu) -> fecha o arquivo;
// a roda volta (mesmo depois de dormir/reiniciar) -> abre um arquivo novo sozinho.
// So o botao (ou GRAVAR_VIAGEM = false no Serial) desliga. Salvo na flash.
static bool GRAVAR_VIAGEM = false;

// LUZ DE FREIO automatica: corrente negativa (regeneracao) = freando -> piscas em vermelho
// (so sem pisca de curva) e mochila vermelha no brilho maximo; depois volta como estava.
static bool FREIO_AUTO = true;
static const float    FREIO_LIGA_A = 2.0f;      // acende com corrente abaixo de -2 A...
static const float    FREIO_DESLIGA_A = 0.8f;   // ...apaga quando volta acima de -0,8 A (2 leituras)
static const float    FREIO_VEL_MIN = 3.0f;     // so andando (parado a corrente oscila)
static const uint32_t FREIO_MIN_MS = 1000;      // fica no minimo 1 s aceso (nao pisca)
static bool     freioAtivo = false;
static uint32_t freioTesteAte = 0;              // comando 'freio': simula por 2,5 s

// Mochila (Dream): estado aqui no topo porque o console usa.
static const char* DREAM_MAC = "2b:80:04:13:0e:4d";
static NimBLEUUID DREAM_SVC((uint16_t)0xFFD5);
static NimBLEUUID DREAM_WRCHR((uint16_t)0xFFD9);
static NimBLEClient* dreamCli = nullptr;
static NimBLERemoteCharacteristic* dreamWr = nullptr;
static bool dreamQuer = false;          // deve manter conectado (vem de DREAM_AUTO)
static bool dreamConectado = false;
static bool dreamConectando = false;
static uint32_t dreamProxTentativa = 0;
static int      dreamFalhas = 0;        // tentativas seguidas que falharam
// estado desejado das luzes da mochila (a tela mexe nisto; aplicado ao conectar)
static bool    dreamLigado = false;
static uint8_t dreamModo = 0;           // 0 = cor fixa, 1 = efeito
static uint8_t dreamR = 255, dreamG = 60, dreamB = 0;
static uint8_t dreamBrilho = 255;
static uint8_t dreamEf = 0, dreamVel = 0x40;
static bool    dreamDemo = false;
static bool    dreamAplicarPend = false;   // aplicar o estado assim que (re)conectar

// 433 MHz (piscas): constantes/estado aqui no topo porque o console usa.
static const int      TX433_PIN = 21;
static const uint16_t TX433_CURTO = 330, TX433_LONGO = 990, TX433_SILENCIO = 9000;
static const int      TX433_REPET = 12;              // rajada por comando (fila garante que presses rapidos nao se percam)
static const uint8_t  TX433_IDENT[3] = { 0x3C, 0x24, 0x06 };
static const uint8_t  CMD_PISCA_ESQ = 0x11, CMD_PISCA_DIR = 0x12, CMD_LUZ_OFF = 0x03;
static const uint8_t  CMD_LUZ_LARANJA = 0x13, CMD_LUZ_VERMELHO = 0x23, CMD_LUZ_PISCANDO = 0x33;
static const int      TX433_SIMB = 41;               // 40 bits + 1 simbolo final
static rmt_data_t     tx433Simb[TX433_SIMB * TX433_REPET];
// Controle da BUZINA (medido na captura): pulso curto ~352 us, longo ~920 us
// (proporcao ~2,6, diferente da dos piscas) e ~1,2 ms entre as repeticoes. Usado no '433 raw'.
static const uint16_t BUZ_CURTO = 352, BUZ_LONGO = 920, BUZ_INTERVALO = 1224;
// Buzina EXTERNA: pareada com esta identidade do display (12/10/2026). Fila de 1 posicao
// (o mais recente ganha), enviada quando o radio libera - igual aos piscas.
static const uint32_t BUZ_EXT_ID = 0x009F1D;
// TESTE DE ALCANCE ('433 teste'): rajadas continuas com uma identidade que NINGUEM
// obedece (00 9F 1E + numero de sequencia), para o medidor_433 (C3 + LCD) contar
// quantas repeticoes chegam. Liga/desliga pelo Serial; continua rodando sem o USB.
static const uint32_t TESTE433_ID = 0x009F1E;
static bool    tx433Teste = false;
static uint8_t tx433TesteSeq = 0;
static volatile bool    buzExtPend = false;
static volatile uint8_t buzExtCmd = 0;
static bool           tx433Ok = false;
// Radio 433 disponivel? (com o C3: o dele, que chega pelo canal)
#define RADIO433_OK (PONTE_C3 ? (c3Ok && c3Radio) : tx433Ok)

// Estado das luzes/piscas (lanterninhas de bike: capacete + traseira do mono)
static const int PIN_PISCA_ESQ = 2, PIN_PISCA_DIR = 3;   // botoes ao GND (INPUT_PULLUP)
// Os botoes fisicos dos piscas foram substituidos pelo controle 433 (12/10/2026):
// o IO3 agora e do RECEPTOR. false = botoes desligados (o IO2 fica livre).
static const bool BOTOES_PISCA = false;
// Receptor 433 (SRX882S) no IO3: o controle da buzina vira entrada do display.
static const int      RX433_PIN = 3;
static const uint32_t CONTROLE_ID = 0x009F1C;     // identidade do controle (3 primeiros bytes)
enum { LUZ_OFF = 0, LUZ_LARANJA = 1, LUZ_VERMELHO = 2, LUZ_PISCANDO = 3 };
static const uint8_t LUZ_CMD[4] = { CMD_LUZ_OFF, CMD_LUZ_LARANJA, CMD_LUZ_VERMELHO, CMD_LUZ_PISCANDO };
static const char*   LUZ_NOME[4] = { "apagado", "laranja", "vermelho", "piscando" };
static int  luzBase = LUZ_OFF;          // modo "parado" das luzes; salvo na flash
static int  piscaAtivo = 0;             // 0 nenhum, 1 esquerda, 2 direita

// Pinos e estado do som (aqui no topo porque o console/buzina usam estas
// variaveis; as FUNcoes do som ficam mais abaixo, perto do setup).
// Pinos (lcdwiki): amplificador EN=IO1 (0=liga), MCLK 4, BCLK 5, LRCK 7,
// IO8 = saida de audio ESP32->codec. Codec ES8311 no I2C 0x18.
static const int SOM_AMP_EN = 1, SOM_MCLK = 4, SOM_BCLK = 5, SOM_LRCK = 7, SOM_DOUT = 8, SOM_DIN = 6;
static const uint32_t SOM_TAXA = 32000;
static const float    SOM_FREQ = 4000;     // ressonancia do piezo de 35 mm
static I2SClass somI2s;
static bool somOk = false;
enum { SOM_NADA, SOM_BIPE, SOM_ALARME };
static volatile int somModo = SOM_NADA;
static volatile int somPedido = 0;         // muda a cada pedido; a tarefa reinicia o tempo

// Estado e constantes do MPU (aqui no topo porque o handleTouch usa estas
// variaveis; as FUNcoes do MPU ficam mais abaixo, perto do setup).
static const uint8_t MPU_END = 0x68;
static const float   MPU_LSB_ACEL = 8192.0f;      // escala +-4 g
static const float   MPU_ANG_LIGA = 30;           // PADRAO (graus): perto da posicao -> acende
static const float   MPU_ANG_APAGA = 50;          // PADRAO (graus): longe -> apaga
static float mpuAngLiga = MPU_ANG_LIGA;           // valores em uso (ajustaveis na tela do MPU, salvos)
static float mpuAngApaga = MPU_ANG_APAGA;
static uint32_t mpuCfgConfirma = 0;               // 1o toque no PADRAO (pede "certeza?" por 3 s)
static uint32_t dormirConfirma = 0;               // 1o toque no Dormir da cortina (idem)
static const uint32_t MPU_T_LIGAR = 250;          // ms na posicao para acender
static const uint32_t MPU_T_APAGAR = 1500;        // ms fora para apagar
static const uint32_t MPU_T_TOQUE = 8000;         // ms acesa depois de um toque
static const uint8_t  MPU_BRILHO = 200;           // brilho com a tela acesa
static const int      MPU_PINO_INT = 14;          // INT do MPU (ja ligado) -> acordar mexendo
static const uint8_t  MPU_MOT_THR = 20;           // limite do detector de movimento (~sensibilidade 5 do teste)
static bool  mpuOk = false;
static bool  mpuTemRef = false;
static float mpuRx = 0, mpuRy = 0, mpuRz = 1;     // gravidade na posicao "olhando"
static float mpuFx = 0, mpuFy = 0, mpuFz = 1;     // gravidade filtrada (tira a tremida)
static float mpuAngulo = 0;
static bool  telaAcesa = true;
static uint32_t mpuUltimoToque = 0;

// Telas "por cima" das paginas: o painel de Ajustes (puxado de cima) e a config do MPU.
// (int em vez de enum: o Arduino cria prototipos no topo, antes de um enum existir)
static const int TELA_NORMAL = 0, TELA_AJUSTES = 1, TELA_MPU = 2, TELA_BUZEXT = 3,
                 TELA_DREAM = 4,    // mochila: aberta pelo botao "Mochila >" da cortina
                 TELA_PISCAS = 5;   // piscas: cortina de BAIXO (arrastar pra cima a partir da borda)
static int modoTela = TELA_NORMAL;

// Print da tela: hora marcada para salvar o BMP (0 = nenhum pedido)
static uint32_t printEm = 0;

// Bateria do display: IO9 le a METADE da tensao (divisor 2:1 da placa).
// Carregador TP4054 (ate 500 mA) na placa; ela NAO tem protecao de descarga.
static const int BAT_PIN = 9;
static float batV = 0;                    // tensao filtrada (V); 0 = ainda sem leitura
// Calibracao (ganho) da leitura: o ADC + divisor da placa erram alguns %. Ajuste com
// um multimetro: 'bat cal 4.10' (a tensao medida) -> calcula e salva o fator.
// 'bat cal 1' volta ao padrao. Vale para tudo: %, alarme e o corte do sono.
static float BAT_CAL = 1.0f;
static int   batPct = -1;                 // 0..100; -1 = sem bateria / leitura invalida
static bool  batAvisouBaixa = false;
static bool  batCarregando = false;       // estimado (a placa nao tem pino de "carregando")
static bool  usbPC = false;               // USB ligado num COMPUTADOR (o USB nativo percebe)

// DORMIR (deep sleep): quase tudo desliga, inclusive o Bluetooth. Acorda no BOOT
// (= reiniciar). Dorme quando: 10 min SEM roda e SEM USB/carregador (na mesa, parado),
// ou bateria no fim (< BAT_CORTE_V por 30 s) - a placa nao protege a celula sozinha.
static const uint32_t DORMIR_SEM_RODA_MS = 10UL * 60UL * 1000UL;
static const float    BAT_CORTE_V = 3.30f;

// Pico do PWM (como o pontinho de um VU de aparelho de som): marca o maior PWM,
// SEGURA por um tempo e depois desce devagar. Assim da para ver, segundos depois,
// o pico de uma lomba ou acelerada em que nao estava olhando o display.
static const uint32_t PWM_PICO_SEGURA = 5000;    // ms parado no pico
static const float    PWM_PICO_CAI = 4.0f;       // depois cai X pontos de % por segundo
static int      pwmPicoV = 0;                    // valor do pico quando foi marcado
static uint32_t pwmPicoT = 0;                    // quando foi marcado

// ---- tabela das opcoes (para o console e para salvar/carregar) ----
struct OpcaoBool  { const char* nome; bool* valor; bool padrao; };
struct OpcaoTexto { const char* nome; String* valor; const char* padrao; };
static OpcaoBool OPCOES_BOOL[] = {
  { "DUMP_HEX",   &DUMP_HEX,   false },
  { "DUMP_PHONE", &DUMP_PHONE, true  },
  { "DUMP_NOVOS", &DUMP_NOVOS, true  },
  { "DUMP_OTHER", &DUMP_OTHER, true  },
  { "IGNORA_CANCELAR", &IGNORA_CANCELAR, true },
  { "TELA_AUTO", &TELA_AUTO, false },
  { "SOM_ON", &SOM_ON, true },
  { "DREAM_AUTO", &DREAM_AUTO, true },
  { "ACORDA_MEXENDO", &ACORDA_MEXENDO, false },
  { "FREIO_AUTO", &FREIO_AUTO, true },
  { "TELA_GIRADA", &TELA_GIRADA, false },
  { "GRAVAR_VIAGEM", &GRAVAR_VIAGEM, false },
};
static OpcaoTexto OPCOES_TEXTO[] = {
  { "NAME_FILTER", &NAME_FILTER, "KS" },
  { "TARGET_MAC",  &TARGET_MAC,  ""   },
};
static Preferences prefs;


static NimBLEUUID MAIN_SVC("FFE0");
static NimBLEUUID MAIN_CHR("FFE1");

// ================= Tela ES3C28P (pinos do LCDWIKI) =================
// ILI9341V via SPI: CS=10 DC=46 SCK=12 MOSI=11 MISO=13, backlight=45, RST compartilhado com o EN
// Se as cores sairem "negativas", troque PANEL_INVERT. Se vermelho/azul trocados, troque PANEL_RGB_ORDER.
static const bool PANEL_INVERT    = true;   // o init do fabricante liga a inversao (cmd 0x21)
static const bool PANEL_RGB_ORDER = false;  // painel BGR (padrao do ILI9341)

// O touch desta placa vem girado 180 graus em relacao a imagem (vimos na galeria)
static const bool INVERTER_TOUCH_X = true;
static const bool INVERTER_TOUCH_Y = true;

class LGFX : public lgfx::LGFX_Device {
  lgfx::Touch_FT5x06 _touch;
  lgfx::Panel_ILI9341 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 12;
      cfg.pin_mosi = 11;
      cfg.pin_miso = 13;
      cfg.pin_dc = 46;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = 10;
      cfg.pin_rst = -1;
      cfg.pin_busy = -1;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.readable = true;
      cfg.invert = PANEL_INVERT;
      cfg.rgb_order = PANEL_RGB_ORDER;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = 45;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    {
      auto cfg = _touch.config();
      cfg.i2c_port = 0;
      cfg.i2c_addr = 0x38;
      cfg.pin_sda = 16;
      cfg.pin_scl = 15;
      cfg.pin_int = 17;
      cfg.pin_rst = 18;
      cfg.freq = 400000;
      if (INVERTER_TOUCH_X) { cfg.x_min = 239; cfg.x_max = 0; } else { cfg.x_min = 0; cfg.x_max = 239; }
      if (INVERTER_TOUCH_Y) { cfg.y_min = 319; cfg.y_max = 0; } else { cfg.y_min = 0; cfg.y_max = 319; }
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

static LGFX tft;
static LGFX_Sprite spr(&tft);   // quadro inteiro na PSRAM -> sem piscar

// ---- mapeamento remoto <-> local ----
struct Map {
  NimBLEUUID svcUuid;
  NimBLEUUID chrUuid;
  NimBLERemoteCharacteristic* remote = nullptr;
  NimBLECharacteristic* local = nullptr;
  bool remoteWriteNR = false;
  bool isMain = false;
  bool phoneSub = false;      // algum celular assinou a caracteristica local
  bool remoteSub = false;     // ESP32 assinou a remota
  // Quais conexoes (celulares) assinaram esta caracteristica. Enviamos so para elas,
  // uma a uma: mandar "para todos" deixava a NimBLE tentar notificar um celular que
  // estava no meio da desconexao -> ponteiro nulo -> reinicio.
  uint16_t subs[4] = {BLE_HS_CONN_HANDLE_NONE, BLE_HS_CONN_HANDLE_NONE,
                      BLE_HS_CONN_HANDLE_NONE, BLE_HS_CONN_HANDLE_NONE};
};

static void subAdd(Map& m, uint16_t h) {
  for (auto& s : m.subs) if (s == h) return;
  for (auto& s : m.subs) if (s == BLE_HS_CONN_HANDLE_NONE) { s = h; break; }
  m.phoneSub = true;
}
static void subRemove(Map& m, uint16_t h) {
  bool any = false;
  for (auto& s : m.subs) { if (s == h) s = BLE_HS_CONN_HANDLE_NONE; if (s != BLE_HS_CONN_HANDLE_NONE) any = true; }
  m.phoneSub = any;
}
static std::vector<Map> maps;

// ---- lado roda ----
static const NimBLEAdvertisedDevice* target = nullptr;
static NimBLEClient* client = nullptr;
static NimBLERemoteCharacteristic* mainRemote = nullptr;
static volatile bool doConnect = false;
static volatile bool needScan  = false;
static std::string wheelAdvName;
static std::string wheelMfgData;
static std::vector<NimBLEUUID> wheelAdvUuids;

// ---- lado celular ----
static NimBLEServer* server = nullptr;
static bool serverStarted = false;
// ================= Duas identidades Bluetooth =================
// O ESP32-S3 anuncia DOIS "aparelhos" (extended advertising, 2 instancias):
//   ADV_RODA: "KSN-S16P--..." com o endereco normal do ESP32, SEM pareamento -> o app
//   ADV_HID:  "S16 Controle" com um endereco proprio, PAREADO -> so o controle de midia
// Assim o pareamento do controle nao se mistura com a conexao do app.
// Com CONFIG_BT_NIMBLE_MAX_CONNECTIONS 6 no nimconfig.h cabem: roda + 2 apps + controle.
static const char* NOME_CONTROLE = "S16 Controle";
static const int MAX_APPS = 2;
static const uint8_t ADV_RODA = 0, ADV_HID = 1;
static uint8_t hidAddrVal[6];                       // endereco "random static" do S16 Controle
static volatile int appConns = 0;                   // apps conectados na identidade da roda
static volatile uint16_t hidConnHandle = BLE_HS_CONN_HANDLE_NONE;   // conexao do controle
static volatile bool hidAtivo = false;             // o Android assinou as teclas de midia
static volatile bool advDirty = true;               // pede para o loop rever o anuncio
static bool advConfigured = false;
static volatile int phoneCount = 0;
static volatile bool phoneConnected = false;
static volatile uint16_t phoneMtu = 247;  // menor MTU entre os celulares conectados
static uint32_t phoneConnT = 0;

// ---- fila de escritas que precisam de resposta (nao podem ser feitas no callback) ----
struct PendingWrite { uint8_t idx; uint16_t len; uint8_t data[244]; };
static QueueHandle_t writeQ;

// ---- dados para a tela ----
struct WheelData {
  float voltage = 0, speed = 0, current = 0, temp = 0, tripKm = 0, topSpeed = 0;
  float temp2 = 0;        // B9 14-15 (motor? a confirmar com o app da KingSong)
  float speedLimit = 0;   // F6 2-3
  int cooling = -1;       // B9 12 ("Cooling" no app)
  int pwm = -1;
  uint32_t lastUpdate = 0;
} wd;

// Ha quanto tempo (ms) nao chega dado novo da roda. USE SEMPRE ESTA FUNCAO em vez de
// "millis() - wd.lastUpdate": o lastUpdate e escrito no callback da NimBLE, que roda
// no OUTRO nucleo. Se o millis() for lido antes e um frame chegar no meio, a conta
// "agora - lastUpdate" fica negativa, da a volta no uint32 e vira ~49 dias ("roda
// sumiu" por engano: fechava a gravacao, piscava o freio...). Aqui o lastUpdate e
// copiado ANTES do millis(), e um valor "do futuro" conta como 0.
static uint32_t msSemDados() {
  uint32_t t = wd.lastUpdate;          // copia primeiro
  if (t == 0) return 0xFFFFFFFFUL;     // nunca chegou nada
  int32_t d = (int32_t)(millis() - t);
  return d < 0 ? 0 : (uint32_t)d;
}

// BMS: a roda manda sozinha um pacote longo por pack (F1 = pack 1, F2 = pack 2, subtipo D0)
//   [21] numero de celulas | [22..] tensoes (mV, 2 bytes cada)
//   depois: numero de sensores de temperatura, temperaturas (0,1 K), corrente (/100 A),
//   tensao do pack (/100 V) e SoC (/10 %)
struct BmsPack {
  uint16_t cell[24];
  int nCells = 0;
  float temps[8];
  int nTemps = 0;
  float current = 0, voltage = 0, soc = 0;
  uint32_t lastUpdate = 0;
} bms[2];

static char statusL1[32] = "KS-S16 ponte v5";
static char statusL2[32] = "Iniciando...";

// ================= Logs no cartao SD =================
// Pinos do slot (manual do fabricante): CLK=38, CMD=40, D0=39 (modo de 1 linha)
// /logs/eventos.csv  -> sempre: conexoes e desconexoes da roda e dos apps
// /logs/viagem_*.csv -> quando a gravacao e ligada na tela de controles (5 linhas/s)
static bool sdOk = false;
static bool clockSet = false;          // relogio acertado pelo app da KingSong?
static bool rideLogging = false;
static File rideFile;
static char rideFileName[48] = "";
static uint32_t rideLines = 0;

// Eventos podem acontecer dentro dos callbacks do Bluetooth, onde nao e bom
// escrever no cartao (demora). Eles entram numa fila e o loop grava depois.
struct LogEvt { uint32_t ms; char txt[80]; };
static QueueHandle_t logQ;

static void logEvent(const char* fmt, ...) {
  if (!logQ) return;
  LogEvt e;
  e.ms = millis();
  va_list ap; va_start(ap, fmt);
  vsnprintf(e.txt, sizeof(e.txt), fmt, ap);
  va_end(ap);
  xQueueSend(logQ, &e, 0);
}

// "AAAA-MM-DD HH:MM:SS" se o relogio foi acertado; senao, vazio
static void clockStr(char* out, size_t n) {
  if (!clockSet) { out[0] = 0; return; }
  time_t t = time(nullptr);
  struct tm tmv;
  localtime_r(&t, &tmv);
  strftime(out, n, "%Y-%m-%d %H:%M:%S", &tmv);
}

// Comando F9 = relogio da roda. O mesmo formato serve para os dois sentidos:
//   app -> roda (acerta):   AA 55 01 00 [4-5]=ano [6]=mes [7]=dia [8]=hora [9]=min [10]=seg ... F9 01 <crc>
//   roda -> nos (resposta): AA 55 00 00 [4-5]=ano ...                                      F9 01 <crc>
// Ao conectar, o display PERGUNTA a hora para a roda (frame capturado do app, com CRC pronto).
// Se depois o app da KingSong acertar a roda, pegamos carona e acertamos de novo.
static const uint8_t PEDE_HORA[20] = {0xAA, 0x55, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xF9, 0x01, 0xB9, 0x78};

static void setClockFromF9(const uint8_t* d, size_t len, const char* origem) {
  if (len < 20 || d[16] != 0xF9 || d[17] != 0x01) return;
  int year = d[4] | (d[5] << 8);
  if (year < 2024 || year > 2100) return;
  struct tm tmv = {};
  tmv.tm_year = year - 1900; tmv.tm_mon = d[6] - 1; tmv.tm_mday = d[7];
  tmv.tm_hour = d[8]; tmv.tm_min = d[9]; tmv.tm_sec = d[10];
  struct timeval tv = { mktime(&tmv), 0 };
  settimeofday(&tv, nullptr);
  bool primeira = !clockSet;
  clockSet = true;
  if (primeira) {
    char ts[24]; clockStr(ts, sizeof(ts));
    logEvent("relogio acertado pel%s: %s", origem, ts);
  }
}

static void writeEvents() {
  LogEvt e;
  while (logQ && xQueueReceive(logQ, &e, 0) == pdTRUE) {
    Serial.printf("[evento] %s\n", e.txt);
    if (!sdOk) continue;
    File f = SD_MMC.open("/logs/eventos.csv", FILE_APPEND);
    if (!f) continue;
    if (f.size() == 0) f.println("uptime_ms,data_hora,evento");
    char ts[24]; clockStr(ts, sizeof(ts));
    f.printf("%lu,%s,%s\n", (unsigned long)e.ms, ts, e.txt);
    f.close();
  }
}

static void startRideLog() {
  if (!sdOk || rideLogging) return;
  if (clockSet) {
    time_t t = time(nullptr); struct tm tmv; localtime_r(&t, &tmv);
    strftime(rideFileName, sizeof(rideFileName), "/logs/viagem_%Y%m%d_%H%M%S.csv", &tmv);
  } else {
    for (int i = 1; i < 1000; i++) {           // sem relogio: viagem_001, 002...
      snprintf(rideFileName, sizeof(rideFileName), "/logs/viagem_%03d.csv", i);
      if (!SD_MMC.exists(rideFileName)) break;
    }
  }
  rideFile = SD_MMC.open(rideFileName, FILE_WRITE);
  if (!rideFile) { logEvent("erro ao criar %s", rideFileName); return; }
  rideFile.println("uptime_ms,data_hora,velocidade_kmh,pwm_pct,corrente_a,tensao_v");
  rideLines = 0;
  rideLogging = true;
  logEvent("gravacao iniciada: %s", rideFileName);
}

static void stopRideLog() {
  if (!rideLogging) return;
  rideLogging = false;
  rideFile.close();
  logEvent("gravacao parada: %s (%lu linhas)", rideFileName, (unsigned long)rideLines);
}

// Liga/desliga a INTENCAO de gravar (botao do log). Quem abre/fecha o arquivo e o rideLoop.
static void setGravarViagem(bool on) {
  GRAVAR_VIAGEM = on;
  prefs.begin("s16cfg", false); prefs.putBool("GRAVAR_VIAGEM", on); prefs.end();
  if (!on) stopRideLog();
  logEvent("gravacao da viagem: %s", on ? "LIGADA (grava sempre que a roda estiver conectada)" : "desligada");
}

// Abre o arquivo quando a roda esta mandando dados e fecha quando ela some.
//  - abre: GRAVAR_VIAGEM ligado + dados da roda chegando + relogio acertado
//    (ou 5 s de espera, para o nome do arquivo sair com data e hora);
//  - fecha: 10 s sem dados da roda (desligou / desconectou), sem quebrar o
//    arquivo por uma falhinha curta.
static void rideLoop() {
  static uint32_t vivaDesde = 0;
  uint32_t agora = millis();
  uint32_t sem = msSemDados();
  bool viva = mainRemote != nullptr && sem < 2000;
  if (viva) { if (!vivaDesde) vivaDesde = agora; } else vivaDesde = 0;
  // So o tempo sem dados decide (uma queda curta do Bluetooth com reconexao em
  // poucos segundos NAO divide a viagem; o writeRideLine ja pula linhas sem dados).
  if (rideLogging && sem > 10000) {
    Serial.println("[log] roda sumiu: fechando a gravacao (reabre quando ela voltar)");
    stopRideLog();
  }
  if (rideLogging && !GRAVAR_VIAGEM) stopRideLog();     // desligado pelo Serial (GRAVAR_VIAGEM = false)
  static uint32_t tentou = 0;                          // se o cartao falhar, tenta de novo a cada 5 s
  if (GRAVAR_VIAGEM && !rideLogging && sdOk && viva && (clockSet || agora - vivaDesde > 5000)
      && (tentou == 0 || agora - tentou > 5000)) {
    tentou = agora;
    startRideLog();
  }
}

static void writeRideLine() {
  static uint32_t last = 0, lastFlush = 0;
  if (!rideLogging || millis() - last < 200) return;     // 5 linhas por segundo
  last = millis();
  if (msSemDados() > 2000) return;                       // sem dados novos da roda
  char ts[24]; clockStr(ts, sizeof(ts));
  rideFile.printf("%lu,%s,%.2f,%d,%.2f,%.2f\n", (unsigned long)millis(), ts,
                  wd.speed, wd.pwm, wd.current, wd.voltage);
  rideLines++;
  if (millis() - lastFlush > 5000) {    // grava de fato a cada 5 s (protege contra desligar)
    lastFlush = millis();
    rideFile.flush();
  }
}

// ================= Utilitarios =================
static uint16_t u16(const uint8_t* d, int i) { return d[i] | (d[i + 1] << 8); }
static int16_t  s16(const uint8_t* d, int i) { return (int16_t)u16(d, i); }
static uint32_t u32r(const uint8_t* d, int i) { return ((uint32_t)u16(d, i) << 16) | u16(d, i + 2); }

static void printHex(const char* prefix, const uint8_t* d, size_t len) {
  Serial.print(prefix);
  for (size_t i = 0; i < len; i++) Serial.printf("%02X ", d[i]);
  Serial.println();
}

static void drawStatus();
static void statusMsg(const char* l1, const char* l2) {
  strlcpy(statusL1, l1, sizeof(statusL1));
  strlcpy(statusL2, l2, sizeof(statusL2));
  drawStatus();
}

static Map* findByRemote(NimBLERemoteCharacteristic* rc) {
  for (auto& m : maps) if (m.remote == rc) return &m;
  return nullptr;
}
static int findIdxByLocal(NimBLECharacteristic* lc) {
  for (size_t i = 0; i < maps.size(); i++) if (maps[i].local == lc) return (int)i;
  return -1;
}

// ================= Decodificacao (so para a tela) =================
static void parseBms(const uint8_t* d, size_t len) {
  if (len < 22) return;                        // precisa ao menos do byte 21 (n de celulas)
  BmsPack& b = bms[d[16] == 0xF1 ? 0 : 1];
  int n = min((int)d[21], 24);
  size_t base = 22 + 2 * n;                    // onde termina a lista de celulas
  if (len < base + 1) return;
  int nt = min((int)d[base], 8);
  size_t after = base + 1 + 2 * nt;            // depois das temperaturas
  if (len < after + 6) return;
  for (int i = 0; i < n; i++) b.cell[i] = u16(d, 22 + 2 * i);
  b.nCells = n;
  for (int i = 0; i < nt; i++) {
    uint16_t raw = u16(d, base + 1 + 2 * i);
    b.temps[i] = raw ? (raw - 2731) / 10.0f : NAN;   // 0 = sensor ausente
  }
  b.nTemps = nt;
  b.current = s16(d, after) / 100.0f;
  b.voltage = u16(d, after + 2) / 100.0f;
  b.soc     = u16(d, after + 4) / 10.0f;
  b.lastUpdate = millis();
}

static void parseFrame(const uint8_t* d, size_t len) {
  if (len < 20 || d[0] != 0xAA || d[1] != 0x55) return;
  if (len > 20 && (d[16] == 0xF1 || d[16] == 0xF2) && d[17] == 0xD0) { parseBms(d, len); return; }
  if (d[16] == 0xF9 && d[2] == 0x00) { setClockFromF9(d, len, "a roda"); return; }
  switch (d[16]) {
    case 0xA9:
      wd.voltage = u16(d, 2) / 100.0f;
      wd.speed   = u16(d, 4) / 100.0f;
      wd.current = s16(d, 10) / 100.0f;
      wd.temp    = u16(d, 12) / 100.0f;
      wd.lastUpdate = millis();
      break;
    case 0xB9:
      wd.tripKm   = u32r(d, 2) / 1000.0f;
      wd.topSpeed = u16(d, 8) / 100.0f;
      wd.cooling  = d[12];
      wd.temp2    = u16(d, 14) / 100.0f;
      break;
    case 0xF6:
      wd.speedLimit = u16(d, 2) / 100.0f;
      break;
    case 0xF5:
      wd.pwm = d[15];
      break;
  }
}

// ================= Repasse roda -> celular =================
static void forwardToPhone(Map* m, const uint8_t* d, size_t len) {
  if (!phoneConnected || !m->phoneSub || !m->local) return;
  size_t maxLen = phoneMtu > 3 ? phoneMtu - 3 : 20;
  for (uint16_t h : m->subs) {
    if (h == BLE_HS_CONN_HANDLE_NONE) continue;
    for (size_t off = 0; off < len; off += maxLen) {
      size_t n = min(maxLen, len - off);
      m->local->notify(d + off, n, h);   // so para este celular
    }
  }
}

// Quadro do canal display <-> C3? (aqui embaixo, e nao junto das variaveis no topo:
// uma FUNCAO antes do struct Map faz o Arduino gerar os prototipos cedo demais)
static bool ehCanalC3(const uint8_t* d, size_t len) {
  return len >= 20 && d[0] == 0xAA && d[1] == 0x55 && memcmp(d + 16, C3_MARCA, 4) == 0;
}

static void remoteNotifyCB(NimBLERemoteCharacteristic* rc, uint8_t* data, size_t len, bool) {
  Map* m = findByRemote(rc);
  if (!m) return;
  if (m->isMain && ehCanalC3(data, len)) {      // estado mandado pelo C3 (canal proprio)
    if (data[2] == 0x02) {
      c3Luz = data[3]; c3Pisca = data[4]; c3Freio = data[5]; c3FreioAuto = data[6];
      c3Teste = data[7]; c3EvBtn = data[8]; c3EvSeq = data[9]; c3Radio = data[10]; c3Apps = data[11];
      c3EstadoEm = millis(); c3Ok = true;
    }
    return;                                       // nunca vai para a tela/apps como dado da roda
  }
  if (m->isMain) {
    if (DUMP_HEX) printHex("R> ", data, len);
    else if (DUMP_NOVOS && len >= 20) {
      // o fluxo normal da roda: A9 B9 F5 F6 C9 F1 F2, e BB (nome) / B3 (serie), que sao as
      // respostas ao handshake do proprio display. O resto e resposta a algum comando.
      static const uint8_t comuns[] = {0xA9, 0xB9, 0xF5, 0xF6, 0xC9, 0xF1, 0xF2, 0xBB, 0xB3};
      bool comum = false;
      for (uint8_t t : comuns) if (data[16] == t) comum = true;
      if (!comum) printHex("R? ", data, len);
    }
    parseFrame(data, len);
  } else if (DUMP_OTHER) {
    Serial.printf("R[%s]> ", m->chrUuid.toString().c_str());
    printHex("", data, len);
  }
  forwardToPhone(m, data, len);
}

// ================= Servidor (lado celular) =================
class LocalChrCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    int idx = findIdxByLocal(c);
    if (idx < 0) return;
    Map& m = maps[idx];
    NimBLEAttValue v = c->getValue();
    if (m.isMain) { if (DUMP_PHONE) printHex("C> ", v.data(), v.size()); if (v.size() >= 20 && v.data()[2] == 0x01) setClockFromF9(v.data(), v.size(), "o app"); }
    else if (DUMP_OTHER) {
      Serial.printf("C[%s]> ", m.chrUuid.toString().c_str());
      printHex("", v.data(), v.size());
    }
    if (!m.remote) return;
    if (m.remoteWriteNR) {
      m.remote->writeValue(v.data(), v.size(), false);   // rapido, seguro no callback
    } else {
      static PendingWrite pw;   // static: nao ocupa a pilha (pequena) da tarefa do Bluetooth
      pw.idx = idx;
      pw.len = (uint16_t)std::min((size_t)sizeof(pw.data), (size_t)v.size());
      memcpy(pw.data, v.data(), pw.len);
      xQueueSend(writeQ, &pw, 0);
    }
  }
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    if (!DUMP_PHONE) return;
    int idx = findIdxByLocal(c);
    Serial.printf("Celular leu %s\n", idx >= 0 ? maps[idx].chrUuid.toString().c_str() : "?");
  }
  void onSubscribe(NimBLECharacteristic* c, NimBLEConnInfo& info, uint16_t subValue) override {
    int idx = findIdxByLocal(c);
    if (idx < 0) return;
    if (subValue) {
      subAdd(maps[idx], info.getConnHandle());
    } else if (maps[idx].isMain && IGNORA_CANCELAR) {
      // nao remove: a conexao pode estar levando outro app "de carona" (ver IGNORA_CANCELAR)
      Serial.printf("Celular cancelou %s (handle %u) - ignorado, os dados continuam ate a conexao fechar\n",
                    maps[idx].chrUuid.toString().c_str(), info.getConnHandle());
      return;
    } else {
      subRemove(maps[idx], info.getConnHandle());
    }
    Serial.printf("Celular %s %s\n", subValue ? "assinou" : "cancelou",
                  maps[idx].chrUuid.toString().c_str());
  }
} localChrCB;

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
    if (phoneCount == 0) phoneMtu = 247;
    phoneCount++;
    // Por qual identidade ele entrou? O endereco que NOS usamos nesta conexao diz.
    struct ble_gap_conn_desc d;
    bool viaHid = false;
    if (ble_gap_conn_find(info.getConnHandle(), &d) == 0)
      viaHid = memcmp(d.our_ota_addr.val, hidAddrVal, 6) == 0;
    if (viaHid) hidConnHandle = info.getConnHandle(); else appConns++;
    phoneConnected = true;
    phoneConnT = millis();
    Serial.printf("Celular conectado: %s (handle %u) - %d conectado(s)\n",
                  info.getAddress().toString().c_str(), info.getConnHandle(), phoneCount);
    Serial.printf("  -> pela identidade %s (apps: %d)\n", viaHid ? NOME_CONTROLE : "da roda", appConns);
    logEvent("%s conectou %s", viaHid ? "controle de midia" : "app", info.getAddress().toString().c_str());
    advDirty = true;
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int reason) override {
    if (phoneCount > 0) phoneCount--;
    bool eraHid = (info.getConnHandle() == hidConnHandle);
    if (eraHid) { hidConnHandle = BLE_HS_CONN_HANDLE_NONE; hidAtivo = false; }
    else if (appConns > 0) appConns--;
    phoneConnected = (phoneCount > 0);
    for (auto& m : maps) subRemove(m, info.getConnHandle());   // esquece as assinaturas dele
    logEvent("%s desconectou, motivo 0x%X", eraHid ? "controle de midia" : "app", reason);
    Serial.printf("Celular desconectou apos %lu ms, motivo %d (0x%X) - restam %d\n",
                  (unsigned long)(millis() - phoneConnT), reason, reason, phoneCount);
    advDirty = true;
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    Serial.printf("Celular pediu seguranca: cifrado=%d pareado=%d\n", info.isEncrypted(), info.isBonded());
    // DIAGNOSTICO do pareamento: "no ar" = endereco que o celular usou agora
    // (muda a cada ~15 min); "identidade" = endereco fixo dele, se ja sabemos.
    // Com o PAREAMENTO_V2 funcionando, depois de religar o display a identidade
    // deve vir IGUAL a de antes e "pareado=1" sem o celular pedir nada.
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(info.getConnHandle(), &d) == 0) {
      Serial.printf("[bond] no ar %s | identidade %s\n",
                    NimBLEAddress(d.peer_ota_addr).toString().c_str(),
                    NimBLEAddress(d.peer_id_addr).toString().c_str());
    }
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo& info) override {
    // o MTU da conexao do controle de midia nao importa: ela nao recebe os dados da roda
    if (info.getConnHandle() != hidConnHandle && mtu < phoneMtu) phoneMtu = mtu;
    Serial.printf("MTU do celular: %u\n", mtu);
  }
} serverCB;

static uint32_t propsOf(NimBLERemoteCharacteristic* rc) {
  uint32_t p = 0;
  if (rc->canRead())            p |= NIMBLE_PROPERTY::READ;
  if (rc->canWrite())           p |= NIMBLE_PROPERTY::WRITE;
  if (rc->canWriteNoResponse()) p |= NIMBLE_PROPERTY::WRITE_NR;
  if (rc->canNotify())          p |= NIMBLE_PROPERTY::NOTIFY;
  if (rc->canIndicate())        p |= NIMBLE_PROPERTY::INDICATE;
  return p;
}

static bool isGenericService(const NimBLEUUID& u) {
  return u == NimBLEUUID((uint16_t)0x1800) || u == NimBLEUUID((uint16_t)0x1801);
}

// ---------------------------------------------------------------------------
// A NimBLE NAO permite montar/alterar o servidor GATT enquanto existe QUALQUER
// conexao aberta (inclusive a nossa com a roda) -> assert em ble_svc_gap_init.
// Por isso o clone acontece em 3 etapas:
//   1. Na primeira conexao: grava um "modelo" da GATT da roda (UUIDs, props, valores).
//   2. Desconecta da roda e monta o servidor local a partir do modelo.
//   3. Reconecta e mapeia remoto <-> local pelos UUIDs.
// Nas reconexoes seguintes so a etapa 3 e feita.
// ---------------------------------------------------------------------------
struct DescInfo { NimBLEUUID uuid; std::string value; };
struct ChrInfo { NimBLEUUID uuid; uint32_t props; std::string value; std::vector<DescInfo> descs; };
struct SvcInfo { NimBLEUUID uuid; std::vector<ChrInfo> chrs; };
static std::vector<SvcInfo> gattTemplate;

static void recordTemplate() {
  gattTemplate.clear();
  for (auto* rs : client->getServices(false)) {
    if (isGenericService(rs->getUUID())) continue;
    SvcInfo si;
    si.uuid = rs->getUUID();
    for (auto* rc : rs->getCharacteristics(false)) {
      ChrInfo ci;
      ci.uuid = rc->getUUID();
      ci.props = propsOf(rc);
      if (rc->canRead()) {
        NimBLEAttValue v = rc->readValue();
        ci.value.assign((const char*)v.data(), v.size());
      }
      Serial.printf("  %s props=0x%02X val=\"", ci.uuid.toString().c_str(), (unsigned)ci.props);
      for (char ch : ci.value) Serial.print(isprint((uint8_t)ch) ? ch : '.');
      Serial.println("\"");
      // Descritores: so o 0x2901 (User Description).
      // O 0x2902 a NimBLE cria sozinha. E a descoberta de descritores devolve, por engano,
      // a DECLARACAO da proxima caracteristica (0x2803) - clonar isso corrompia a GATT.
      for (auto* rd : rc->getDescriptors(true)) {
        if (rd->getUUID() != NimBLEUUID((uint16_t)0x2901)) continue;
        DescInfo di;
        di.uuid = rd->getUUID();
        NimBLEAttValue dv = rd->readValue();
        di.value.assign((const char*)dv.data(), dv.size());
        Serial.printf("      desc %s = \"", di.uuid.toString().c_str());
        for (char ch : di.value) Serial.print(isprint((uint8_t)ch) ? ch : '.');
        Serial.println("\"");
        ci.descs.push_back(di);
      }
      si.chrs.push_back(ci);
    }
    gattTemplate.push_back(si);
  }
}

// ================= Controle de midia (HID over GATT) =================
// Um "teclado" que so tem as teclas de midia. O Android le o Report Map, entende que
// e um controle de midia e repassa as teclas para o app que estiver tocando.
// Cada aperto = 1 byte com um bit ligado (tecla pressionada) e, logo depois, 0 (solta).
static const uint8_t HID_REPORT_MAP[] = {
  0x05, 0x0C,   // Usage Page (Consumer)          - "teclas de consumo" (midia)
  0x09, 0x01,   // Usage (Consumer Control)
  0xA1, 0x01,   // Collection (Application)
  0x85, 0x01,   //   Report ID (1)
  0x15, 0x00,   //   Logical Minimum (0)
  0x25, 0x01,   //   Logical Maximum (1)
  0x75, 0x01,   //   Report Size (1 bit por tecla)
  0x95, 0x08,   //   Report Count (8 teclas)
  0x09, 0xB5,   //   bit 0: Scan Next Track      (proxima)
  0x09, 0xB6,   //   bit 1: Scan Previous Track  (anterior)
  0x09, 0xCD,   //   bit 2: Play/Pause
  0x09, 0xE9,   //   bit 3: Volume Up    (do celular)
  0x09, 0xEA,   //   bit 4: Volume Down  (do celular)
  0x09, 0xE2,   //   bit 5: Mute
  0x09, 0xB7,   //   bit 6: Stop
  0x09, 0xCD,   //   bit 7: (repete play/pause, so para completar 8 bits)
  0x81, 0x02,   //   Input (Data, Variable, Absolute)
  0xC0          // End Collection
};
static const uint8_t HID_PROXIMA = 0x01, HID_ANTERIOR = 0x02, HID_PLAY = 0x04,
                     HID_VOL_MAIS = 0x08, HID_VOL_MENOS = 0x10;

static NimBLECharacteristic* hidInput = nullptr;
static uint32_t hidSoltarEm = 0;

class HidCB : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& info, uint16_t subValue) override {
    hidAtivo = subValue != 0;
    Serial.printf("Controle de midia %s (handle %u, cifrado=%d)\n",
                  hidAtivo ? "ATIVO no celular" : "desativado", info.getConnHandle(), info.isEncrypted());
    logEvent("controle de midia %s", hidAtivo ? "ativo" : "desativado");
  }
} hidCB;

static void buildHid() {
  NimBLEService* hid = server->createService(NimBLEUUID((uint16_t)0x1812));
  const uint8_t info[4] = {0x11, 0x01, 0x00, 0x02};          // HID 1.11, sem pais, "normally connectable"
  hid->createCharacteristic(NimBLEUUID((uint16_t)0x2A4A), NIMBLE_PROPERTY::READ)->setValue(info, sizeof(info));
  hid->createCharacteristic(NimBLEUUID((uint16_t)0x2A4B), NIMBLE_PROPERTY::READ)
     ->setValue(HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
  hid->createCharacteristic(NimBLEUUID((uint16_t)0x2A4C), NIMBLE_PROPERTY::WRITE_NR);   // HID Control Point
  const uint8_t modo = 0x01;                                                            // "report mode"
  hid->createCharacteristic(NimBLEUUID((uint16_t)0x2A4E), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE_NR)
     ->setValue(&modo, 1);
  // A caracteristica das teclas: so pode ser lida com a conexao cifrada (pareada), como o Android exige
  hidInput = hid->createCharacteristic(NimBLEUUID((uint16_t)0x2A4D),
                                       NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC);
  const uint8_t ref[2] = {0x01, 0x01};                       // Report ID 1, tipo "entrada"
  hidInput->createDescriptor(NimBLEUUID((uint16_t)0x2908), NIMBLE_PROPERTY::READ, 2)->setValue(ref, 2);
  const uint8_t zero = 0;
  hidInput->setValue(&zero, 1);
  hidInput->setCallbacks(&hidCB);
  hid->start();

  // Servico de bateria: o Android espera encontrar um junto com o HID
  NimBLEService* bat = server->createService(NimBLEUUID((uint16_t)0x180F));
  const uint8_t nivel = 100;
  bat->createCharacteristic(NimBLEUUID((uint16_t)0x2A19), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY)
     ->setValue(&nivel, 1);
  bat->start();
  Serial.println("Controle de midia (HID) incluido na GATT");
}

// "Aperta" uma tecla de midia; o loop "solta" 40 ms depois
static void hidTecla(uint8_t bits) {
  if (!hidInput) return;
  if (!hidAtivo) Serial.println("Controle de midia ainda nao ativo: pareie o display no Bluetooth do celular");
  hidInput->setValue(&bits, 1);
  hidInput->notify();
  hidSoltarEm = millis() + 40;
}
static void hidSoltar() {
  if (!hidSoltarEm || millis() < hidSoltarEm) return;
  hidSoltarEm = 0;
  const uint8_t zero = 0;
  hidInput->setValue(&zero, 1);
  hidInput->notify();
}

static void buildServer() {
  for (auto& si : gattTemplate) {
    NimBLEService* ls = server->createService(si.uuid);
    for (auto& ci : si.chrs) {
      NimBLECharacteristic* lc = ls->createCharacteristic(ci.uuid, ci.props);
      lc->setCallbacks(&localChrCB);
      if (!ci.value.empty()) lc->setValue((const uint8_t*)ci.value.data(), ci.value.size());
      for (auto& di : ci.descs) {
        NimBLEDescriptor* ld = lc->createDescriptor(di.uuid, NIMBLE_PROPERTY::READ,
                                                    di.value.size() ? di.value.size() : 1);
        ld->setValue((const uint8_t*)di.value.data(), di.value.size());
      }
    }
    ls->start();
  }
  buildHid();
  server->start();
  serverStarted = true;
  Serial.println("Servidor local montado");
}

static void mapGatt() {
  maps.clear();
  maps.reserve(48);
  mainRemote = nullptr;
  for (auto* rs : client->getServices(false)) {
    if (isGenericService(rs->getUUID())) continue;
    NimBLEService* ls = server->getServiceByUUID(rs->getUUID());
    if (!ls) continue;
    for (auto* rc : rs->getCharacteristics(false)) {
      NimBLECharacteristic* lc = ls->getCharacteristic(rc->getUUID());
      if (!lc) continue;
      Map m;
      m.svcUuid = rs->getUUID();
      m.chrUuid = rc->getUUID();
      m.remote = rc;
      m.local = lc;
      m.remoteWriteNR = rc->canWriteNoResponse();
      m.isMain = (rs->getUUID() == MAIN_SVC && rc->getUUID() == MAIN_CHR);
      if (m.isMain) mainRemote = rc;
      maps.push_back(m);
    }
  }
  Serial.printf("Mapeadas %u caracteristicas\n", (unsigned)maps.size());
}

static void startAdvertisingAsWheel() {
  NimBLEDevice::setDeviceName(wheelAdvName);   // nome GAP (0x2A00) igual ao da roda
  NimBLEExtAdvertising* adv = NimBLEDevice::getAdvertising();

  // Identidade 1: a roda (anuncio "legado", que todo celular entende)
  NimBLEExtAdvertisement roda;
  roda.setLegacyAdvertising(true);
  roda.setConnectable(true);
  roda.setScannable(true);
  roda.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  roda.setName(wheelAdvName);
  for (auto& u : wheelAdvUuids) roda.addServiceUUID(u);
  if (wheelAdvUuids.empty()) roda.addServiceUUID(MAIN_SVC);
  adv->setInstanceData(ADV_RODA, roda);
  NimBLEExtAdvertisement respRoda;                // resposta ao "scan ativo"
  respRoda.setLegacyAdvertising(true);
  if (!wheelMfgData.empty()) respRoda.setManufacturerData(wheelMfgData);
  adv->setScanResponseData(ADV_RODA, respRoda);

  // Identidade 2: o controle de midia, com endereco proprio
  {
    NimBLEExtAdvertisement hid;
    hid.setLegacyAdvertising(true);
    hid.setConnectable(true);
    hid.setScannable(true);
    hid.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    hid.setName(NOME_CONTROLE);
    hid.setAppearance(0x0180);                    // "controle remoto generico"
    hid.addServiceUUID(NimBLEUUID((uint16_t)0x1812));
    ble_addr_t a;
    a.type = BLE_ADDR_RANDOM;
    memcpy(a.val, hidAddrVal, 6);
    hid.setAddress(NimBLEAddress(a));
    adv->setInstanceData(ADV_HID, hid);
    NimBLEExtAdvertisement respHid;
    respHid.setLegacyAdvertising(true);
    adv->setScanResponseData(ADV_HID, respHid);
  }
  advConfigured = true;
  advDirty = true;
  Serial.printf("Anunciando como \"%s\" e como \"%s\"\n", wheelAdvName.c_str(), NOME_CONTROLE);
}

// Liga/desliga cada anuncio conforme as vagas. Chamado pelo loop.
static void updateAdvertising() {
  static uint32_t last = 0;
  if (!advConfigured || (!advDirty && millis() - last < 1000)) return;
  advDirty = false;
  last = millis();
  NimBLEExtAdvertising* adv = NimBLEDevice::getAdvertising();
  bool queroRoda = !PONTE_C3 && mainRemote && appConns < MAX_APPS;   // com o C3, os apps conectam nele
  bool queroHid  = hidConnHandle == BLE_HS_CONN_HANDLE_NONE;
  // Diagnostico: mostra cada mudanca e, principalmente, quando o controlador RECUSA ligar um anuncio
  static uint32_t ultimoAvisoFalha = 0;
  auto ajusta = [&](uint8_t inst, bool quero, const char* nome) {
    if (quero == adv->isActive(inst)) return;
    if (!quero) { adv->stop(inst); Serial.printf("[anuncio] %s: parado\n", nome); return; }
    if (adv->start(inst)) { Serial.printf("[anuncio] %s: ligado\n", nome); return; }
    if (millis() - ultimoAvisoFalha > 5000) {            // nao inunda o Serial
      ultimoAvisoFalha = millis();
      struct ble_gap_conn_desc d;
      int conexoes = 0;
      for (uint16_t h = 0; h < 16; h++) if (ble_gap_conn_find(h, &d) == 0) conexoes++;
      Serial.printf("[anuncio] %s: o controlador RECUSOU ligar (%d conexoes abertas, contando a roda)\n",
                    nome, conexoes);
    }
  };
  ajusta(ADV_RODA, queroRoda, "roda");
  ajusta(ADV_HID, queroHid, NOME_CONTROLE);
}

// ================= Cliente (lado roda) =================
class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (target) return;
    std::string name = dev->getName();
    bool match = TARGET_MAC.length()
                   ? dev->getAddress().toString() == std::string(TARGET_MAC.c_str())
                   : (!name.empty() && name.find(NAME_FILTER.c_str()) != std::string::npos);
    if (!match) return;
    target = dev;
    NimBLEDevice::getScan()->stop();

    wheelAdvName = name;
    wheelMfgData = dev->getManufacturerData();
    wheelAdvUuids.clear();
    for (int i = 0; i < dev->getServiceUUIDCount(); i++) wheelAdvUuids.push_back(dev->getServiceUUID(i));

    Serial.printf("Roda encontrada: %s \"%s\"\n", dev->getAddress().toString().c_str(), name.c_str());
    for (auto& u : wheelAdvUuids) Serial.printf("  anuncia UUID %s\n", u.toString().c_str());
    if (!wheelMfgData.empty())
      printHex("  dados fabricante: ", (const uint8_t*)wheelMfgData.data(), wheelMfgData.size());
    doConnect = true;
  }
} scanCB;

static volatile bool intentionalDisconnect = false;

class ClientCB : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient*, int reason) override {
    mainRemote = nullptr;
    for (auto& m : maps) { m.remote = nullptr; m.remoteSub = false; }
    if (intentionalDisconnect) {           // desconexao planejada para montar o servidor
      intentionalDisconnect = false;
      return;
    }
    Serial.printf("Roda desconectou (%d)\n", reason);
    logEvent("roda desconectou (motivo 0x%X)", reason);
    NimBLEDevice::getAdvertising()->stop(ADV_RODA);
    advDirty = true;
    // derruba os apps (ficariam sem dados), mas mantem o controle de midia conectado
    if (server) for (uint16_t h : server->getPeerDevices()) if (h != hidConnHandle) server->disconnect(h);
    needScan = true;
  }
} clientCB;

// Monta e envia um comando no formato padrao da KingSong:
//   AA 55 | b2 b3 b4 b5 (parametros) 00.. | [16]=comando | 14 5A 5A
static void sendKs(uint8_t cmd, uint8_t b2, uint8_t b3, uint8_t b4, uint8_t b5) {
  if (!mainRemote) return;
  uint8_t p[20] = {0xAA, 0x55, b2, b3, b4, b5};
  p[16] = cmd; p[17] = 0x14; p[18] = 0x5A; p[19] = 0x5A;
  mainRemote->writeValue(p, sizeof(p), false);
  if (DUMP_PHONE) printHex("D> ", p, sizeof(p));   // D> = enviado pelo display
}
static void sendCmd(uint8_t cmd) { sendKs(cmd, 0, 0, 0, 0); }

// Comando para o C3 pelo canal proprio (so faz sentido com PONTE_C3)
static void c3Enviar(uint8_t cmd, uint8_t arg) {
  NimBLERemoteCharacteristic* r = mainRemote;
  if (!r) { Serial.println("[c3] sem conexao com o C3"); return; }
  uint8_t p[20] = {0xAA, 0x55, 0x01, cmd, arg};
  memcpy(p + 16, C3_MARCA, 4);
  r->writeValue(p, sizeof(p), false);
  if (DUMP_PHONE) printHex("D>C3 ", p, sizeof(p));
}

// ---------- Configuracao pelo Serial ----------
// (funcoes ficam aqui embaixo, e nao junto das variaveis no topo, porque o Arduino cria
//  "prototipos" de todas as funcoes antes da primeira funcao do arquivo - se ela viesse
//  antes das structs, os prototipos citariam tipos que ainda nao existem)
static void carregarConfig() {
  prefs.begin("s16cfg", true);                 // true = so leitura
  for (auto& o : OPCOES_BOOL)  *o.valor = prefs.getBool(o.nome, o.padrao);
  for (auto& o : OPCOES_TEXTO) *o.valor = prefs.getString(o.nome, o.padrao);
  BAT_CAL = prefs.getFloat("BAT_CAL", 1.0f);
  if (BAT_CAL < 0.85f || BAT_CAL > 1.15f) BAT_CAL = 1.0f;   // valor estranho: ignora
  prefs.end();
}

static void mostrarConfig() {
  Serial.println("--- configuracao ---");
  for (auto& o : OPCOES_BOOL)  Serial.printf("  %-16s = %s\n", o.nome, *o.valor ? "true" : "false");
  for (auto& o : OPCOES_TEXTO) Serial.printf("  %-16s = \"%s\"\n", o.nome, o.valor->c_str());
}

// Tenta interpretar a linha como "NOME = valor" (ou "NOME valor"). Devolve false se nao for.
static bool comandoConfig(String linha) {
  linha.trim();
  if (linha == "config") { mostrarConfig(); return true; }
  if (linha == "config padrao") {
    prefs.begin("s16cfg", false); prefs.clear(); prefs.end();
    for (auto& o : OPCOES_BOOL)  *o.valor = o.padrao;
    for (auto& o : OPCOES_TEXTO) *o.valor = o.padrao;
    Serial.println("Configuracao de fabrica restaurada");
    mostrarConfig();
    return true;
  }
  linha.replace("=", " ");
  int esp = linha.indexOf(' ');
  String nome = (esp < 0) ? linha : linha.substring(0, esp);
  String valor = (esp < 0) ? "" : linha.substring(esp + 1);
  nome.trim(); valor.trim();
  nome.toUpperCase();

  for (auto& o : OPCOES_BOOL) {
    if (nome != o.nome) continue;
    String v = valor; v.toLowerCase();
    bool novo;
    if (v == "true" || v == "1" || v == "on")        novo = true;
    else if (v == "false" || v == "0" || v == "off") novo = false;
    else { Serial.printf("%s = %s  (use true/false)\n", o.nome, *o.valor ? "true" : "false"); return true; }
    *o.valor = novo;
    prefs.begin("s16cfg", false); prefs.putBool(o.nome, novo); prefs.end();
    Serial.printf("%s = %s (salvo)\n", o.nome, novo ? "true" : "false");
    return true;
  }
  for (auto& o : OPCOES_TEXTO) {
    if (nome != o.nome) continue;
    if (valor == "\"\"" || valor == "-") valor = "";      // "" ou - = vazio
    valor.replace("\"", "");
    *o.valor = valor;
    prefs.begin("s16cfg", false); prefs.putString(o.nome, valor); prefs.end();
    Serial.printf("%s = \"%s\" (salvo; vale na proxima busca pela roda)\n", o.nome, valor.c_str());
    return true;
  }
  return false;
}

// Lista de todos os comandos do Serial ('comandos', 'ajuda' ou '?')
static void mostrarComandos() {
  static const char* const L[] = {
    "--- comandos (Serial Monitor, 115200, \"Nova linha\") ---",
    "comandos | ajuda | ?          esta lista",
    "config                        mostra a configuracao salva",
    "config padrao                 volta tudo ao padrao de fabrica",
    "NOME = valor                  muda uma opcao (ex.: DUMP_OTHER = false, NAME_FILTER = KS)",
    "TELA_GIRADA = true | false    gira a tela (e o touch) 180 graus; salvo",
    "bat                           bateria do display: tensao, %, carregando",
    "bat cal 4.10                  calibra a leitura pela tensao do multimetro (bat cal 1 = padrao)",
    "print                         salva a tela agora em /prints/print_NNN.bmp",
    "dormir                        dorme agora (deep sleep); o BOOT acorda",
    "freio                         simula a luz de freio por 2,5 s (FREIO_AUTO = on/off liga a automatica)",
    "mpu | mpu on | mpu off        status / tela automatica pelo pulso",
    "mpu gravar                    grava a posicao de olhar (braco na posicao)",
    "som bipe | som alarme         testa o som",
    "som on | som off              liga/desliga os bipes",
    "433 e | 433 d                 pisca esquerda / direita (liga; de novo volta ao modo base)",
    "433 teste                     liga/desliga o teste de alcance (rajadas para o medidor_433)",
    "433 x | l | v | p             luz: apagada / laranja / vermelha / piscando",
    "433 raw <hexa> [curto longo]  bits crus no formato da buzina (ex.: 433 raw 009F1CAA 352 920)",
    "dream on | dream off          manter a mochila conectada (salvo)",
    "dream liga | dream desliga    luzes da mochila",
    "dream cor RR GG BB            cor fixa (hexa)",
    "dream ef N                    efeito N (hexa)",
    "bonds                         lista os pareamentos guardados",
    "esquecer                      apaga os pareamentos (esqueca no celular tambem)",
    "t <cmd> <pos 2-15> <valor>    comando para a roda (hexa), ex.: t 95 6 FF",
    "raw <20 bytes hexa>           frame cru para a roda",
  };
  for (const char* s : L) Serial.println(s);
}

// ---------- Console de experimentos (digite no Serial Monitor, "Nova linha" ligado) ----------
//   t 95 6 FF                 -> comando 95 com o byte 6 = FF (resto zero, final 14 5A 5A)
//   raw AA 55 00 ... 5A 5A    -> envia exatamente esses 20 bytes (ex.: um frame capturado)
//   esquecer                  -> apaga os pareamentos (controle de midia) guardados no display
// As respostas "diferentes" da roda aparecem como R? (ver DUMP_NOVOS).
// 2a palavra da linha ("dream cor 10 20 30" -> "cor"; "433" sozinho -> "").
// Antes lia line+N direto: com o comando sozinho, passava do fim da linha e pegava
// a SOBRA da linha anterior (ex.: "433 e" e depois "433" ligava o pisca de novo).
static String palavra2(const String& s) {
  int i = s.indexOf(' ');
  if (i < 0) return "";
  String r = s.substring(i + 1); r.trim();
  int j = r.indexOf(' ');
  return j < 0 ? r : r.substring(0, j);
}

static void handleSerialConsole() {
  static char line[160];
  static int n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') { if (n < (int)sizeof(line) - 1) line[n++] = c; continue; }
    line[n] = 0; n = 0;
    if (comandoConfig(String(line))) continue;   // "DUMP_OTHER = false", "config"...
    String orig = String(line);                  // copia inteira (o strtok abaixo picota a linha)
    orig.trim();
    if (orig.startsWith("433 raw ")) {           // 433 raw <hexa> [curto longo]: bits crus no formato da buzina
      String resto = orig.substring(8); resto.trim();
      int e1 = resto.indexOf(' ');
      String hx = e1 < 0 ? resto : resto.substring(0, e1);
      uint16_t curto = BUZ_CURTO, longo = BUZ_LONGO;
      if (e1 > 0) {                              // tempos opcionais em microssegundos
        String t = resto.substring(e1 + 1); t.trim();
        int e2 = t.indexOf(' ');
        if (e2 > 0) { curto = t.substring(0, e2).toInt(); longo = t.substring(e2 + 1).toInt(); }
        else Serial.println("433 raw: tempos sao 2 numeros (curto longo), ex.: 433 raw 009F1CAA 350 920");
      }
      tx433Raw(hx.c_str(), curto, longo);
      continue;
    }
    if (orig == "comandos" || orig == "ajuda" || orig == "help" || orig == "?") { mostrarComandos(); continue; }
    if (orig == "print") { printEm = millis() + 1; continue; }   // print da tela agora
    if (orig == "dormir") { dormir("comando dormir", true); continue; }  // testa o sono agora
    if (orig == "freio") {
      if (PONTE_C3) { c3Enviar(C3_FREIOTESTE, 0); continue; }
      freioTesteAte = millis() + 2500; Serial.println("[freio] teste: 2,5 s"); continue;
    }
    if (orig == "bat") {
      Serial.printf("bateria: %.2f V  %d%%  %s  (IO9 cru: %lu mV = metade; calibracao x%.4f)  USB do PC: %s\n", batV, batPct,
                    batCarregando ? "CARREGANDO" : "na bateria",
                    (unsigned long)analogReadMilliVolts(BAT_PIN), BAT_CAL, usbPC ? "sim" : "nao");
      continue;
    }
    if (orig.startsWith("bat cal")) {           // bat cal 4.10  (tensao do multimetro) | bat cal 1 (padrao)
      float med = orig.substring(7).toFloat();
      float novo;
      if (med > 0.5f && med < 1.5f) novo = 1.0f;                   // "bat cal 1" = sem calibracao
      else if (med >= 3.0f && med <= 4.5f && batV > 2.5f) novo = BAT_CAL * med / batV;
      else { Serial.println("use: bat cal <tensao medida no multimetro, ex. 4.10>  |  bat cal 1 (padrao). Meca SEM carregador."); continue; }
      if (novo < 0.85f || novo > 1.15f) { Serial.printf("bat cal: fator %.3f fora de 0,85-1,15 (conferir a medida)\n", novo); continue; }
      BAT_CAL = novo;
      prefs.begin("s16cfg", false); prefs.putFloat("BAT_CAL", BAT_CAL); prefs.end();
      batV = 0;                                 // recomeca o filtro (sem "degrau" falso de carregando)
      Serial.printf("bat cal: fator x%.4f salvo\n", BAT_CAL);
      continue;
    }

    char* save;
    char* tok = strtok_r(line, " ", &save);
    if (!tok) continue;
    String nome = tok;
    uint8_t v[24]; int cnt = 0;
    while ((tok = strtok_r(nullptr, " ", &save)) && cnt < 24) v[cnt++] = strtoul(tok, nullptr, 16);

    if (nome == "esquecer") {          // apaga os pareamentos guardados no display
      NimBLEDevice::deleteAllBonds();
      Serial.println("Pareamentos apagados (esqueca o display no Bluetooth do celular tambem)");
      continue;
    }
    if (nome == "bonds") {             // lista os pareamentos guardados agora
      int nb = NimBLEDevice::getNumBonds();
      Serial.printf("[bond] guardados agora: %d\n", nb);
      for (int i = 0; i < nb; i++)
        Serial.printf("[bond]   %d: %s\n", i, NimBLEDevice::getBondedAddress(i).toString().c_str());
#if PAREAMENTO_V2
      Serial.printf("[bond] registros de identidade (host): %d\n", ble_rpa_get_num_peer_dev_records());
#endif
      continue;
    }
    if (nome == "dream") {             // dream on|off|liga|desliga|cor RR GG BB|ef N
      String sub = palavra2(orig);                 // 1a palavra depois de "dream "
      // (os numeros "RR GG BB" / "N" ja estao em v[1..] parseados como hexa)
      if (sub == "on")       dreamSetAuto(true);
      else if (sub == "off") dreamSetAuto(false);
      else if (sub == "liga")    dreamSetLiga(true);
      else if (sub == "desliga") dreamSetLiga(false);
      else if (sub == "cor" && cnt >= 4)  dreamSetCor(v[1], v[2], v[3]);   // v[1..3] = RR GG BB
      else if (sub == "ef"  && cnt >= 2)  dreamSetEfeito(v[1]);
      else Serial.printf("[dream] %s  (use: dream on|off|liga|desliga|cor RR GG BB|ef N)\n",
                         dreamConectado ? "conectado" : dreamConectando ? "conectando" : dreamQuer ? "tentando" : "off");
      continue;
    }
    if (nome == "433") {               // 433 e|d|x|l|v|p (esq, dir, desliga, laranja, verm, piscando)
      String a = palavra2(orig);
      if (a == "e") piscaToggle(1);
      else if (a == "d") piscaToggle(2);
      else if (a == "x") luzSetBase(LUZ_OFF);
      else if (a == "l") luzSetBase(LUZ_LARANJA);
      else if (a == "v") luzSetBase(LUZ_VERMELHO);
      else if (a == "p") luzSetBase(LUZ_PISCANDO);
      else if (a == "teste" && PONTE_C3) c3Enviar(C3_TESTE433, 0);
      else if (a == "teste") {
        tx433Teste = !tx433Teste;
        Serial.printf("433 teste: %s\n", tx433Teste
          ? "LIGADO - rajada 00 9F 1E NN a cada ~0,8 s (veja o medidor_433). '433 teste' de novo desliga"
          : "desligado");
      }
      else Serial.printf("433: %s  base=%s pisca=%d  (use: 433 e|d|x|l|v|p  ou  433 raw <hexa>)\n",
                         tx433Ok ? "ok" : "sem RMT", LUZ_NOME[luzBase], piscaAtivo);
      continue;
    }
    if (nome == "som") {               // som | som bipe | som alarme | som on | som off
      String a = palavra2(orig);
      if (a == "bipe") somBipe();
      else if (a == "alarme") somAlarme();
      else if (a == "on" || a == "off") comandoConfig(String("SOM_ON ") + a);
      else Serial.printf("som: %s | SOM_ON=%s  (use: som bipe | som alarme | som on | som off)\n",
                         somOk ? "ok" : "sem codec", SOM_ON ? "on" : "off");
      continue;
    }
    if (nome == "mpu") {               // mpu | mpu gravar | mpu on | mpu off
      String a = palavra2(orig);                   // a palavra depois de "mpu "
      if (a == "gravar") mpuSalvarRef();
      else if (a == "on" || a == "off") { comandoConfig(String("TELA_AUTO ") + a); }
      else Serial.printf("mpu: %s | TELA_AUTO=%s | ref=%s | angulo=%.1f (acende <%.0f, apaga >%.0f) | tela=%s\n",
                         mpuOk ? "ok" : "sem sensor", TELA_AUTO ? "on" : "off",
                         mpuTemRef ? "sim" : "nao", mpuAngulo, mpuAngLiga, mpuAngApaga,
                         telaAcesa ? "acesa" : "apagada");
      continue;
    }
    if (nome != "t" && nome != "raw") { Serial.println("Comando nao reconhecido. Digite 'comandos' para ver a lista."); continue; }
    if (!mainRemote) { Serial.println("Roda nao conectada"); continue; }
    uint8_t p[20] = {0xAA, 0x55};
    if (nome == "t" && cnt == 3 && v[1] >= 2 && v[1] <= 15) {
      p[v[1]] = v[2];
      p[16] = v[0]; p[17] = 0x14; p[18] = 0x5A; p[19] = 0x5A;
    } else if (nome == "raw" && cnt == 20) {
      memcpy(p, v, 20);
    } else {
      Serial.println("Uso:  t <cmd> <posicao 2-15> <valor> (hexa), ex.: t 95 6 FF  |  raw <20 bytes hexa>");
      continue;
    }
    mainRemote->writeValue(p, sizeof(p), false);
    printHex("D> ", p, sizeof(p));
  }
}

static bool connectToWheel() {
  if (!client) {
    client = NimBLEDevice::createClient();
    client->setClientCallbacks(&clientCB, false);
  }
  if (!client->connect(target)) return false;
  Serial.println("Conectado na roda, descobrindo servicos...");

  if (!client->discoverAttributes()) {
    Serial.println("Falha na descoberta");
    client->disconnect();
    return false;
  }

  if (!serverStarted) {
    // Etapa 1: grava o modelo
    recordTemplate();

    // Etapa 2: desconecta e monta o servidor sem nenhuma conexao aberta
    intentionalDisconnect = true;
    client->disconnect();
    uint32_t t0 = millis();
    while (client->isConnected() && millis() - t0 < 3000) delay(10);
    delay(300);
    buildServer();

    // Etapa 3: reconecta (algumas tentativas, a roda leva um instante para voltar a anunciar)
    bool ok = false;
    for (int i = 0; i < 5 && !ok; i++) {
      delay(500);
      Serial.printf("Reconectando na roda (tentativa %d)...\n", i + 1);
      ok = client->connect(target) && client->discoverAttributes();
    }
    if (!ok) { Serial.println("Falha ao reconectar"); return false; }
  }

  mapGatt();

  if (!mainRemote || !mainRemote->subscribe(true, remoteNotifyCB)) {
    Serial.println("Falha ao assinar FFE1");
    client->disconnect();
    return false;
  }
  for (auto& m : maps) if (m.isMain) m.remoteSub = true;
  Serial.println("Inscrito em FFE1");

  sendCmd(0x9B);
  delay(100);
  sendCmd(0x63);
  delay(100);
  mainRemote->writeValue(PEDE_HORA, sizeof(PEDE_HORA), false);   // resposta chega como F9
#if PONTE_C3
  c3Ok = false;
  c3Enviar(C3_HELLO, 0);          // "sou o display": o C3 passa a mandar o estado para esta conexao
#endif
  return true;
}

static void startScan() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->clearResults();
  target = nullptr;
  scan->start(0, false, true);
}

// Sincroniza assinaturas das caracteristicas secundarias com o que o celular pediu
static void syncSubscriptions() {
  for (auto& m : maps) {
    if (m.isMain || !m.remote) continue;
    bool want = phoneConnected && m.phoneSub;
    if (want && !m.remoteSub) {
      bool useNotify = m.remote->canNotify();
      if (m.remote->subscribe(useNotify, remoteNotifyCB)) {
        m.remoteSub = true;
        Serial.printf("Assinada na roda: %s\n", m.chrUuid.toString().c_str());
      }
    } else if (!want && m.remoteSub) {
      m.remote->unsubscribe();
      m.remoteSub = false;
    }
  }
}

static void processWrites() {
  PendingWrite pw;
  while (xQueueReceive(writeQ, &pw, 0) == pdTRUE) {
    if (pw.idx < maps.size() && maps[pw.idx].remote)
      maps[pw.idx].remote->writeValue(pw.data, pw.len, true);
  }
}

// ================= Tela =================
static const uint16_t COL_DIM   = 0x7BEF;  // cinza
static const uint16_t COL_FRAME = 0x39E7;  // cinza escuro

static uint16_t pwmColor(int p) {
  if (p < 70) return TFT_GREEN;
  if (p < 85) return TFT_YELLOW;
  return TFT_RED;
}

// Telas de deslizar: 0 = BMS (esquerda), 1 = principal, 2 = controles (direita).
// (piscas e mochila sairam das bolinhas: piscas = cortina de baixo, mochila = botao da cortina)
static const int NUM_PAGES = 3;
static int page = 1;
static uint32_t hornFlashUntil = 0;     // mostra "BUZINA" na barra por um instante

// Barra superior: so icones (todos sempre visiveis; cinza escuro = desligado/inativo)
//   roda | app | nota (controle de midia) | buzina | mochila | ventoinha | gravacao
//   | bolinhas das telas | bateria | hora
static void drawTopBar() {
  bool wheelOk = client && client->isConnected();
  spr.fillRect(0, 0, 240, 22, 0x18E3);
  spr.setFont(&fonts::Font2);
  spr.setTextSize(1);
  // roda: pneu (aro duplo) + cubo. Verde = conectada, vermelho = nao
  uint16_t cr0 = wheelOk ? TFT_GREEN : TFT_RED;
  spr.drawCircle(10, 11, 7, cr0); spr.drawCircle(10, 11, 6, cr0);
  spr.fillCircle(10, 11, 2, cr0);
  // app: celular. Ciano = um app conectado na identidade da roda
  uint16_t ca = (PONTE_C3 ? (c3Ok ? (int)c3Apps : 0) : (int)appConns) > 0 ? TFT_CYAN : COL_FRAME;   // com o C3: apps conectados nele
  spr.drawRoundRect(22, 3, 10, 16, 2, ca);
  spr.drawFastHLine(25, 15, 4, ca);
  // nota musical: controle de midia conectado e ativo no celular
  bool hidOk = hidAtivo && hidConnHandle != BLE_HS_CONN_HANDLE_NONE;
  uint16_t cn = hidOk ? TFT_MAGENTA : COL_FRAME;
  spr.fillCircle(40, 15, 3, cn);
  spr.drawFastVLine(43, 4, 12, cn);
  spr.drawLine(43, 4, 47, 7, cn);
  // buzina: corneta. Amarela por um instante quando toca
  uint16_t cz = millis() < hornFlashUntil ? TFT_YELLOW : COL_FRAME;
  spr.fillRect(51, 8, 4, 6, cz);
  spr.fillTriangle(55, 8, 63, 3, 63, 18, cz);
  spr.fillTriangle(55, 8, 55, 13, 63, 18, cz);
  // mochila (Dream): 3 bolinhas em triangulo. RGB = conectado; piscando = tentando; cinza = desligado
  {
    int dcx = 74;
    bool tentando = dreamQuer && !dreamConectado;
    bool piscaOn = (millis() / 400) % 2;
    uint16_t cr, cg, cb;
    if (dreamConectado)      { cr = TFT_RED; cg = TFT_GREEN; cb = TFT_BLUE; }
    else if (tentando)       { cr = cg = cb = piscaOn ? TFT_YELLOW : 0x18E3; }   // pisca (0x18E3 = fundo)
    else                     { cr = cg = cb = COL_FRAME; }
    spr.fillCircle(dcx, 7, 2, cr);
    spr.fillCircle(dcx - 4, 14, 2, cg);
    spr.fillCircle(dcx + 4, 14, 2, cb);
  }
  // ventoinha da roda ("Cooling"): laranja quando ligada
  uint16_t cv = wd.cooling > 0 ? TFT_ORANGE : COL_FRAME;
  spr.fillCircle(89, 11, 2, cv);
  spr.fillCircle(89, 5, 2, cv); spr.fillCircle(84, 14, 2, cv); spr.fillCircle(94, 14, 2, cv);
  // gravacao da viagem no SD: vermelho gravando; laranja = ligada, esperando a roda;
  // contorno = sem cartao
  if (sdOk) spr.fillCircle(102, 11, 4, rideLogging ? TFT_RED : GRAVAR_VIAGEM ? TFT_ORANGE : COL_FRAME);
  else      spr.drawCircle(102, 11, 4, COL_FRAME);
  // bolinhas das telas (na cortina de Ajustes / tela do MPU nenhuma fica marcada)
  for (int i = 0; i < NUM_PAGES; i++) {
    int x = 114 + i * 8;
    if (i == page && modoTela == TELA_NORMAL) spr.fillCircle(x, 11, 3, TFT_WHITE);
    else spr.drawCircle(x, 11, 3, COL_DIM);
  }
  // bateria do display: verde > 40%, amarela > 15%, vermelha abaixo. Contorno so = sem leitura
  {
    uint16_t cb0 = batPct < 0 ? COL_FRAME : batPct > 40 ? TFT_GREEN : batPct > 15 ? TFT_YELLOW : TFT_RED;
    if (batPct >= 0 && batPct <= 15 && !batCarregando && (millis() / 500) % 2) cb0 = COL_FRAME;   // pisca quando baixa
    spr.drawRect(160, 6, 20, 11, cb0);
    spr.fillRect(180, 9, 2, 5, cb0);
    if (batPct > 0) spr.fillRect(162, 8, max(1, 16 * batPct / 100), 7, cb0);
    if (batCarregando) {                       // raio por cima, como nos celulares
      spr.fillTriangle(173, 3, 164, 12, 170, 12, 0x18E3);   // contorno escuro (fundo da barra)
      spr.fillTriangle(168, 10, 176, 10, 166, 20, 0x18E3);
      spr.fillTriangle(172, 4, 165, 11, 170, 11, TFT_YELLOW);
      spr.fillTriangle(169, 11, 175, 11, 167, 19, TFT_YELLOW);
    }
  }
  spr.setTextDatum(middle_right);
  char buf[16];
  if (clockSet) {                       // hora (acertada pela roda ou pelo app)
    time_t t = time(nullptr); struct tm tmv; localtime_r(&t, &tmv);
    strftime(buf, sizeof(buf), "%H:%M", &tmv);
    spr.setTextColor(TFT_WHITE);
    spr.drawString(buf, 236, 11);
  }
}

// "Alca" na borda de baixo: indica que da pra puxar a cortina dos piscas dali
static void drawAlca() {
  spr.fillRoundRect(100, 315, 40, 4, 2, COL_DIM);
}

static void drawCell(int x, int y, int w, int h, const char* label, const char* value) {
  spr.drawRoundRect(x, y, w, h, 6, COL_FRAME);
  spr.setFont(&fonts::Font2);
  spr.setTextSize(1);
  spr.setTextColor(COL_DIM);
  spr.setTextDatum(top_left);
  spr.drawString(label, x + 6, y + 3);
  spr.setFont(&fonts::FreeSansBold12pt7b);
  spr.setTextColor(TFT_WHITE);
  spr.setTextDatum(bottom_right);
  spr.drawString(value, x + w - 6, y + h - 3);
}

static void drawStatus() {
  spr.fillScreen(TFT_BLACK);
  drawTopBar();
  spr.setTextDatum(middle_center);
  spr.setFont(&fonts::FreeSansBold12pt7b);
  spr.setTextColor(TFT_WHITE);
  spr.drawString(statusL1, 120, 140);
  spr.setFont(&fonts::FreeSans9pt7b);
  spr.setTextColor(COL_DIM);
  spr.drawString(statusL2, 120, 175);
  spr.pushSprite(0, 0);
}

// Pico que aparece agora: o marcado, parado por PWM_PICO_SEGURA, depois descendo
// PWM_PICO_CAI pontos/s - mas nunca abaixo do PWM atual.
static int pwmPicoMostrar() {
  uint32_t passou = millis() - pwmPicoT;
  int v = pwmPicoV;
  if (passou > PWM_PICO_SEGURA) v -= (int)((passou - PWM_PICO_SEGURA) * PWM_PICO_CAI / 1000.0f);
  return max(v, max(wd.pwm, 0));
}
// Chamada a cada volta do loop (o pico conta mesmo com outra tela aberta ou a tela apagada)
static void pwmPicoAtualizar() {
  if (wd.pwm < 0 || msSemDados() > 2000) return;
  if (wd.pwm >= pwmPicoMostrar()) { pwmPicoV = wd.pwm; pwmPicoT = millis(); }
}

// Indicador das lanternas na tela principal (lado 1 = esquerda, 2 = direita), como as
// lanternas estao AGORA:
//   vermelho / laranja = bolinha da cor do modo base nos dois lados
//   piscando           = bolinha laranja com um pontinho piscando no meio (discreto)
//   pisca de curva     = SETA VERDE piscando no lado da curva, apontando pra fora (como no
//                        painel de carro); o outro lado fica apagado, igual a lanterna
//   apagado            = nada
static void drawLuzLado(int cx, int lado) {
  const int cy = 42;
  bool fase = (millis() / 400) % 2;
  if (piscaAtivo) {
    if (piscaAtivo != lado || !fase) return;
    int d = (lado == 1) ? -1 : 1;                 // -1 aponta pra esquerda, +1 pra direita
    spr.fillTriangle(cx + d * 12, cy, cx, cy - 11, cx, cy + 11, TFT_GREEN);   // ponta
    spr.fillRect(d < 0 ? cx : cx - 11, cy - 4, 12, 9, TFT_GREEN);              // haste
    return;
  }
  if (freioAtivo) {                               // freando: vermelho com contorno branco
    spr.fillCircle(cx, cy, 10, TFT_RED);
    spr.drawCircle(cx, cy, 11, TFT_WHITE);
    return;
  }
  if (luzBase == LUZ_OFF) return;
  uint16_t c = (luzBase == LUZ_VERMELHO) ? (uint16_t)TFT_RED : (uint16_t)0xFD20;
  spr.fillCircle(cx, cy, 9, c);
  if (luzBase == LUZ_PISCANDO && fase) spr.fillCircle(cx, cy, 3, TFT_BLACK);
}

static void drawData() {
  if (msSemDados() > 2000) { statusMsg("Conectado", "Sem dados..."); return; }
  char buf[24];
  spr.fillScreen(TFT_BLACK);
  drawTopBar();

  // ---- velocidade ----
  spr.setFont(&fonts::Font7);          // fonte de 7 segmentos (so digitos)
  spr.setTextSize(2);
  spr.setTextDatum(middle_center);
  spr.setTextColor(wd.pwm >= 0 ? pwmColor(wd.pwm) : TFT_WHITE);
  snprintf(buf, sizeof(buf), "%d", (int)(wd.speed + 0.5f));
  spr.drawString(buf, 120, 78);
  spr.setTextSize(1);
  spr.setFont(&fonts::Font2);
  spr.setTextColor(COL_DIM);
  if (wd.speedLimit > 0) {
    snprintf(buf, sizeof(buf), "km/h   lim %.0f", wd.speedLimit);
    spr.drawString(buf, 120, 135);
  } else {
    spr.drawString("km/h", 120, 135);
  }

  // ---- lanternas: um indicador de cada lado da velocidade ----
  drawLuzLado(24, 1);
  drawLuzLado(216, 2);

  // ---- barra de PWM ----
  const int bx = 8, by = 150, bw = 224, bh = 34;
  spr.drawRoundRect(bx, by, bw, bh, 6, COL_FRAME);
  if (wd.pwm > 0) {
    int fill = (bw - 4) * min(wd.pwm, 100) / 100;
    spr.fillRoundRect(bx + 2, by + 2, fill, bh - 4, 4, pwmColor(wd.pwm));
  }
  // pico: risquinho na barra + "pico NN%" no canto (so quando esta acima do atual)
  int pico = pwmPicoMostrar();
  if (pico > 0 && pico > wd.pwm) {
    int xp = bx + 2 + (bw - 4) * min(pico, 100) / 100;
    spr.fillRect(xp - 3, by + 2, 4, bh - 4, pwmColor(pico));
    snprintf(buf, sizeof(buf), "pico %d%%", pico);
    spr.setFont(&fonts::Font2);
    spr.setTextDatum(middle_right);
    spr.setTextColor(pwmColor(pico));
    spr.drawString(buf, 232, 135);
  }
  spr.setFont(&fonts::FreeSansBold12pt7b);
  spr.setTextDatum(middle_center);
  spr.setTextColor(TFT_WHITE);
  if (wd.pwm >= 0) snprintf(buf, sizeof(buf), "PWM %d%%", wd.pwm); else strcpy(buf, "PWM --");
  spr.drawString(buf, 120, by + bh / 2);

  // ---- grade de valores (3 linhas x 2 colunas) ----
  const int gy = 192, cw = 114, ch = 38, gap = 4, x1 = 4, x2 = 4 + cw + gap + 4;   // termina em 314: abaixo, a alca
  snprintf(buf, sizeof(buf), "%.1f V", wd.voltage);   drawCell(x1, gy,               cw, ch, "TENSAO", buf);
  snprintf(buf, sizeof(buf), "%.1f A", wd.current);   drawCell(x2, gy,               cw, ch, "CORRENTE", buf);
  snprintf(buf, sizeof(buf), "%.0f C", wd.temp);      drawCell(x1, gy + ch + gap,     cw, ch, "TEMP", buf);
  snprintf(buf, sizeof(buf), "%.0f C", wd.temp2);     drawCell(x2, gy + ch + gap,     cw, ch, "TEMP 2", buf);
  snprintf(buf, sizeof(buf), "%.2f km", wd.tripKm);   drawCell(x1, gy + 2 * (ch + gap), cw, ch, "TRIP", buf);
  snprintf(buf, sizeof(buf), "%.1f", wd.topSpeed);    drawCell(x2, gy + 2 * (ch + gap), cw, ch, "MAX km/h", buf);

  drawAlca();
  spr.pushSprite(0, 0);
}

// ---------- Tela BMS (tensoes das celulas) ----------
// Para cada pack: um cabecalho (tensao, SoC, temperatura maxima), a linha min/max/diferenca
// e um grafico de barras com as 20 celulas. A escala das barras e "ampliada" em volta
// das tensoes reais, senao diferencas de poucos mV ficariam invisiveis.
static void drawBmsPack(int idx, int y0, uint16_t lo, uint16_t hi) {
  const BmsPack& b = bms[idx];
  char buf[48];
  spr.setFont(&fonts::Font2);
  spr.setTextSize(1);
  spr.setTextDatum(top_left);
  spr.setTextColor(TFT_WHITE);

  if (b.nCells == 0 || millis() - b.lastUpdate > 10000) {
    snprintf(buf, sizeof(buf), "PACK %d: aguardando dados", idx + 1);
    spr.setTextColor(COL_DIM);
    spr.drawString(buf, 6, y0 + 4);
    return;
  }

  float tmax = NAN;
  for (int i = 0; i < b.nTemps; i++) if (!isnan(b.temps[i]) && (isnan(tmax) || b.temps[i] > tmax)) tmax = b.temps[i];
  snprintf(buf, sizeof(buf), "PACK %d  %.1fV  %.0f%%  %.0fC", idx + 1, b.voltage, b.soc, isnan(tmax) ? 0.0f : tmax);
  spr.drawString(buf, 6, y0);

  int iMin = 0, iMax = 0;
  for (int i = 1; i < b.nCells; i++) {
    if (b.cell[i] < b.cell[iMin]) iMin = i;
    if (b.cell[i] > b.cell[iMax]) iMax = i;
  }
  snprintf(buf, sizeof(buf), "min %.3f(%d) max %.3f(%d) d%dmV",
           b.cell[iMin] / 1000.0f, iMin + 1, b.cell[iMax] / 1000.0f, iMax + 1, b.cell[iMax] - b.cell[iMin]);
  spr.setTextColor(COL_DIM);
  spr.drawString(buf, 6, y0 + 17);

  // barras
  const int gx = 6, gy = y0 + 36, gw = 228, gh = 96;
  spr.drawRect(gx, gy, gw, gh, COL_FRAME);
  int bw = (gw - 4) / b.nCells;
  for (int i = 0; i < b.nCells; i++) {
    int h = (int)((long)(constrain(b.cell[i], lo, hi) - lo) * (gh - 4) / (hi - lo));
    uint16_t col = (i == iMin) ? TFT_ORANGE : (i == iMax) ? TFT_GREEN : TFT_CYAN;
    spr.fillRect(gx + 2 + i * bw + 1, gy + gh - 2 - h, bw - 2, h, col);
  }
}

static void drawBms() {
  spr.fillScreen(TFT_BLACK);
  drawTopBar();
  // escala comum aos dois packs, com pelo menos 50 mV de janela
  uint16_t lo = 65535, hi = 0;
  for (auto& b : bms) for (int i = 0; i < b.nCells; i++) { lo = min(lo, b.cell[i]); hi = max(hi, b.cell[i]); }
  if (hi < lo) { lo = 3500; hi = 4200; }
  lo -= 20; hi += 5;
  if (hi - lo < 50) lo = hi - 50;
  drawBmsPack(0, 28, lo, hi);
  drawBmsPack(1, 176, lo, hi);
  drawAlca();
  spr.pushSprite(0, 0);
}

// ---------- Tela de controles (icones desenhados com formas simples) ----------
// Grade de 4 linhas x 3 colunas. Cada botao tem um tipo de icone e uma acao.
enum Icone { IC_CEL_MENOS, IC_CEL_MAIS, IC_VAZIO,
             IC_PREV, IC_PLAY, IC_NEXT,
             IC_VOL_MENOS, IC_VOL_MAIS, IC_LOG,
             IC_FAROL, IC_LEDS, IC_BUZINA };
// Layout (4 linhas x 3 colunas):
//   [cel -] [cel +] [     ]      <- CELULAR: volume (teclas de midia HID)
//   [ |<< ] [ >|| ] [ >>| ]      <-          musica (HID)
//   [roda-] [roda+] [ log ]      <- RODA: volume dos falantes (comando 95) e gravacao
//   [farol] [ LEDs] [buzina]     <-       farol e LEDs alternam a cada toque
static const Icone layout[12] = {
  IC_CEL_MENOS, IC_CEL_MAIS, IC_VAZIO,
  IC_PREV, IC_PLAY, IC_NEXT,
  IC_VOL_MENOS, IC_VOL_MAIS, IC_LOG,
  IC_FAROL, IC_LEDS, IC_BUZINA,
};
static const int BT_W = 72, BT_H = 62;
static int botaoX(int i) { return 6 + (i % 3) * 78; }
static int botaoY(int i) { return 32 + (i / 3) * 72; }
static int botaoAceso = -1;              // destaque rapido no botao tocado
static uint32_t botaoAcesoAte = 0;

// Estado dos botoes que alternam. A roda nao conta o estado atual, entao comecamos
// em "desconhecido" (-1) e passamos a saber a partir do primeiro toque.
//   farol: 0 liga -> 1 desliga -> 2 automatico -> 0 ...   (comando 73: 12 / 13 / 14)
//   LEDs:  1 ligados <-> 0 desligados                      (comando 6C: 00 liga / 01 desliga)
static int farolModo = -1;
static int ledsLigados = -1;
static const uint8_t FAROL_CMD[3] = {0x12, 0x13, 0x14};
static const char* FAROL_NOME[3] = {"liga", "desliga", "auto"};

// --- pecas de icones (cx, cy = centro do botao) ---
static void icFarol(int cx, int cy, uint16_t c, bool raios) {
  spr.fillCircle(cx, cy, 9, c);
  if (!raios) return;
  for (int a = 0; a < 360; a += 45) {          // 8 raios em volta
    float r = a * DEG_TO_RAD;
    spr.drawLine(cx + cosf(r) * 13, cy + sinf(r) * 13, cx + cosf(r) * 19, cy + sinf(r) * 19, c);
  }
}
static void icTriangulo(int x, int cy, int dir, uint16_t c) {   // dir = +1 direita, -1 esquerda
  spr.fillTriangle(x, cy - 9, x, cy + 9, x + dir * 13, cy, c);
}
static void icAltoFalante(int cx, int cy, uint16_t c) {
  spr.fillRect(cx - 16, cy - 5, 7, 10, c);
  spr.fillTriangle(cx - 9, cy, cx - 1, cy - 11, cx - 1, cy + 11, c);
}
static void icCelular(int cx, int cy, uint16_t c) {        // celular a esquerda do centro
  spr.drawRoundRect(cx - 18, cy - 14, 15, 28, 3, c);
  spr.drawFastHLine(cx - 14, cy - 10, 7, c);              // "alto-falante" do celular
  spr.fillCircle(cx - 10, cy + 9, 1, c);                  // "botao"
}
static void icMenos(int cx, int cy, uint16_t c) { spr.fillRect(cx + 4, cy - 2, 12, 4, c); }
static void icMais(int cx, int cy, uint16_t c)  { icMenos(cx, cy, c); spr.fillRect(cx + 8, cy - 6, 4, 12, c); }
static void icArquivo(int cx, int cy, uint16_t c) {
  const int x = cx - 11, y = cy - 15, w = 22, h = 30, dobra = 7;
  spr.drawLine(x, y, x + w - dobra, y, c);                 // topo
  spr.drawLine(x + w - dobra, y, x + w, y + dobra, c);     // canto dobrado
  spr.drawLine(x + w, y + dobra, x + w, y + h, c);
  spr.drawLine(x, y + h, x + w, y + h, c);
  spr.drawLine(x, y, x, y + h, c);
  for (int k = 0; k < 3; k++) spr.drawFastHLine(x + 4, y + 12 + k * 6, w - 8, c);   // "linhas de texto"
}
static void icInterrogacao(int cx, int cy, uint16_t c) {
  spr.setFont(&fonts::Font2); spr.setTextDatum(middle_center);
  spr.setTextColor(c); spr.drawString("?", cx, cy + 1);
}

// (recebe int em vez de Icone: o Arduino gera "prototipos" das funcoes no topo do
//  arquivo, antes do enum existir, e um parametro do tipo Icone daria erro)
static void drawIcone(int icInt, int cx, int cy, uint16_t c) {
  Icone ic = (Icone)icInt;
  switch (ic) {
    case IC_CEL_MENOS:  icCelular(cx, cy, c); icMenos(cx, cy, c); break;
    case IC_CEL_MAIS:   icCelular(cx, cy, c); icMais(cx, cy, c); break;
    case IC_LOG:        icArquivo(cx, cy, rideLogging ? TFT_RED : GRAVAR_VIAGEM ? TFT_ORANGE : (sdOk ? c : COL_FRAME)); break;
    case IC_PREV:       spr.fillRect(cx - 16, cy - 9, 3, 18, c);
                        icTriangulo(cx - 1, cy, -1, c); icTriangulo(cx + 12, cy, -1, c); break;
    case IC_NEXT:       spr.fillRect(cx + 13, cy - 9, 3, 18, c);
                        icTriangulo(cx + 1, cy, +1, c); icTriangulo(cx - 12, cy, +1, c); break;
    case IC_PLAY:       icTriangulo(cx - 14, cy, +1, c);
                        spr.fillRect(cx + 4, cy - 9, 4, 18, c); spr.fillRect(cx + 11, cy - 9, 4, 18, c); break;
    case IC_VOL_MENOS:  icAltoFalante(cx, cy, c); icMenos(cx, cy, c); break;
    case IC_VOL_MAIS:   icAltoFalante(cx, cy, c); icMais(cx, cy, c); break;
    case IC_FAROL:
      if (farolModo == 0)      icFarol(cx, cy, TFT_YELLOW, true);
      else if (farolModo == 1) { icFarol(cx, cy, COL_DIM, false);
                                 spr.drawLine(cx - 14, cy + 14, cx + 14, cy - 14, TFT_RED); }
      else if (farolModo == 2) { icFarol(cx, cy, TFT_YELLOW, false);
                                 spr.setFont(&fonts::Font2); spr.setTextDatum(middle_center);
                                 spr.setTextColor(TFT_BLACK); spr.drawString("A", cx, cy + 1); }
      else                     { icFarol(cx, cy, COL_FRAME, false); icInterrogacao(cx, cy, TFT_WHITE); }
      break;
    case IC_LEDS:
      if (ledsLigados == 1) {
        spr.fillCircle(cx - 13, cy, 6, TFT_RED); spr.fillCircle(cx, cy, 6, TFT_GREEN); spr.fillCircle(cx + 13, cy, 6, TFT_BLUE);
      } else {
        spr.drawCircle(cx - 13, cy, 6, COL_DIM); spr.drawCircle(cx, cy, 6, COL_DIM); spr.drawCircle(cx + 13, cy, 6, COL_DIM);
        if (ledsLigados == 0) spr.drawLine(cx - 20, cy + 10, cx + 20, cy - 10, TFT_RED);
        else                  icInterrogacao(cx, cy, TFT_WHITE);
      }
      break;
    case IC_BUZINA:     spr.fillRect(cx - 14, cy - 5, 6, 10, c);                  // corneta
                        spr.fillTriangle(cx - 8, cy - 5, cx - 8, cy + 5, cx + 6, cy - 12, c);
                        spr.fillTriangle(cx - 8, cy + 5, cx + 6, cy + 12, cx + 6, cy - 12, c);
                        spr.drawArc(cx + 6, cy, 12, 10, 300, 360, c);
                        spr.drawArc(cx + 6, cy, 12, 10, 0, 60, c); break;
    case IC_VAZIO:      break;
  }
}

static void drawControls() {
  spr.fillScreen(TFT_BLACK);
  drawTopBar();
  for (int i = 0; i < 12; i++) {
    if (layout[i] == IC_VAZIO) continue;                  // posicao livre: sem botao
    int x = botaoX(i), y = botaoY(i);
    bool aceso = (i == botaoAceso && millis() < botaoAcesoAte);
    spr.fillRoundRect(x, y, BT_W, BT_H, 10, aceso ? 0x6B4D : 0x2124);
    spr.drawRoundRect(x, y, BT_W, BT_H, 10, (layout[i] == IC_LOG && GRAVAR_VIAGEM) ? (rideLogging ? TFT_RED : TFT_ORANGE) : COL_FRAME);
    drawIcone(layout[i], x + BT_W / 2, y + BT_H / 2, TFT_WHITE);
  }
  drawAlca();
  spr.pushSprite(0, 0);
}


static void tocarControles(int x, int y) {
  for (int i = 0; i < 12; i++) {
    int bx = botaoX(i), by = botaoY(i);
    if (x < bx || x >= bx + BT_W || y < by || y >= by + BT_H) continue;
    Icone ic = layout[i];
    if (ic == IC_VAZIO) return;
    botaoAceso = i;
    botaoAcesoAte = millis() + 250;

    switch (ic) {
      case IC_CEL_MENOS: logEvent("toque: volume celular -"); hidTecla(HID_VOL_MENOS); break;
      case IC_CEL_MAIS:  logEvent("toque: volume celular +"); hidTecla(HID_VOL_MAIS); break;
      case IC_PREV:      logEvent("toque: musica anterior");  hidTecla(HID_ANTERIOR); break;
      case IC_PLAY:      logEvent("toque: play/pause");       hidTecla(HID_PLAY); break;
      case IC_NEXT:      logEvent("toque: musica proxima");   hidTecla(HID_PROXIMA); break;
      case IC_VOL_MENOS: logEvent("toque: volume roda -");    sendKs(0x95, 0, 0xFF, 0, 0); break;
      case IC_VOL_MAIS:  logEvent("toque: volume roda +");    sendKs(0x95, 0xFF, 0, 0, 0); break;
      case IC_LOG:
        logEvent("toque: log");
        if (!sdOk && !GRAVAR_VIAGEM) { Serial.println("Sem cartao SD"); break; }   // desligar vale sempre
        setGravarViagem(!GRAVAR_VIAGEM);    // abre/fecha o arquivo no rideLoop (abre ja, se a roda estiver mandando dados)
        break;
      case IC_FAROL:
        farolModo = (farolModo + 1) % 3;          // -1 (desconhecido) vira 0 = liga
        sendKs(0x73, FAROL_CMD[farolModo], 0, 0, 0);
        logEvent("toque: farol %s", FAROL_NOME[farolModo]);
        break;
      case IC_LEDS:
        ledsLigados = (ledsLigados == 1) ? 0 : 1; // desconhecido vira "liga"
        sendKs(0x6C, ledsLigados ? 0x00 : 0x01, 0, 0, 0);
        logEvent("toque: leds %s", ledsLigados ? "liga" : "desliga");
        break;
      case IC_BUZINA:
        logEvent("toque: buzina");
        sendKs(0x88, 0, 0, 0, 0);
        hornFlashUntil = millis() + 600;
        break;
      default: break;
    }
    return;
  }
}

// ---------- Tela 3: Piscas / Luz ----------
static const uint16_t LUZ_COR[4] = { 0x3186 /*cinza*/, 0xFD20 /*laranja*/, TFT_RED, TFT_MAGENTA };
// 4 botoes de modo (2x2) e 2 botoes de pisca grandes
static const int PM_X[2] = { 6, 124 }, PM_W = 110, PM_H = 46, PM_Y0 = 40, PM_DY = 52;
static const int PB_Y = 176, PB_H = 92;
static const int PF_Y = 144, PF_H = 26;            // botao "Luz de freio: sim/nao" (entre os modos e os piscas)

static void drawPiscas() {
  spr.fillScreen(TFT_BLACK);
  drawTopBar();
  spr.setTextDatum(middle_center);
  spr.setFont(&fonts::Font2);
  spr.setTextColor(COL_DIM);
  spr.drawString("Luz (modo parado)", 120, 32);

  for (int m = 0; m < 4; m++) {
    int x = PM_X[m % 2], y = PM_Y0 + (m / 2) * PM_DY;
    bool sel = (luzBase == m && !piscaAtivo);
    spr.fillRoundRect(x, y, PM_W, PM_H, 8, sel ? LUZ_COR[m] : 0x2124);
    spr.drawRoundRect(x, y, PM_W, PM_H, 8, LUZ_COR[m]);
    spr.setTextColor(sel && m == 0 ? TFT_WHITE : sel ? TFT_BLACK : TFT_WHITE);
    spr.drawString(LUZ_NOME[m], x + PM_W / 2, y + PM_H / 2);
  }

  // luz de freio automatica (salvo). Freando agora: o botao fica vermelho vivo
  spr.fillRoundRect(6, PF_Y, 228, PF_H, 6, freioAtivo ? (uint16_t)TFT_RED : FREIO_AUTO ? (uint16_t)0x6000 : (uint16_t)0x2124);
  spr.drawRoundRect(6, PF_Y, 228, PF_H, 6, TFT_RED);
  spr.setTextColor(TFT_WHITE);
  spr.drawString(freioAtivo ? "FREANDO" : FREIO_AUTO ? "Luz de freio: sim" : "Luz de freio: nao", 120, PF_Y + PF_H / 2);

  const char* lbl[2] = { "< ESQ", "DIR >" };
  for (int k = 0; k < 2; k++) {
    int x = PM_X[k]; bool on = (piscaAtivo == k + 1);
    spr.fillRoundRect(x, PB_Y, PM_W, PB_H, 10, on ? 0xFD20 : 0x2124);
    spr.drawRoundRect(x, PB_Y, PM_W, PB_H, 10, 0xFD20);
    spr.setFont(&fonts::FreeSansBold12pt7b);
    spr.setTextColor(on ? TFT_BLACK : TFT_WHITE);
    spr.drawString(lbl[k], x + PM_W / 2, PB_Y + PM_H / 2);
    if (on) { spr.setFont(&fonts::Font2); spr.drawString("piscando", x + PM_W / 2, PB_Y + PB_H - 16); }
  }
  spr.setFont(&fonts::Font2);
  spr.setTextColor(COL_DIM);
  spr.drawString(RADIO433_OK ? "arraste pra baixo: fecha" : (PONTE_C3 ? "sem o C3" : "433 sem RMT"), 120, 300);
  spr.pushSprite(0, 0);
}

static void tocarPiscas(int x, int y) {
  for (int m = 0; m < 4; m++) {
    int bx = PM_X[m % 2], by = PM_Y0 + (m / 2) * PM_DY;
    if (x >= bx && x < bx + PM_W && y >= by && y < by + PM_H) { luzSetBase(m); return; }
  }
  for (int k = 0; k < 2; k++)
    if (x >= PM_X[k] && x < PM_X[k] + PM_W && y >= PB_Y && y < PB_Y + PB_H) { piscaToggle(k + 1); return; }
  if (y >= PF_Y && y < PF_Y + PF_H) {             // liga/desliga a luz de freio automatica (salvo)
    comandoConfig(String("FREIO_AUTO ") + (FREIO_AUTO ? "off" : "on"));
    if (PONTE_C3) { c3Enviar(C3_FREIOAUTO, FREIO_AUTO ? 1 : 0); return; }
    if (!FREIO_AUTO && freioAtivo) { freioAtivo = false; freioTesteAte = 0; freioAplicar(false); }
  }
}

// ---------- Tela 4: Mochila (Dream) ----------
static const uint16_t DREAM_COR[6] = { TFT_RED, TFT_GREEN, TFT_BLUE, 0xFD20, TFT_CYAN, TFT_WHITE };
static const uint8_t  DREAM_RGB[6][3] = { {255,0,0},{0,255,0},{0,0,255},{255,180,0},{0,255,255},{255,255,255} };
static const int DRX[3] = { 6, 84, 162 }, DR_W = 72;
static const int DCON_X = 130, DCON_Y = 24, DCON_W = 106, DCON_H = 22;  // botao CONECTAR/DESCONECTAR
static const int DON_Y = 52, DON_H = 30;            // liga/desliga (luz)
static const int DC_Y0 = 88, DC_H = 28, DC_DY = 32; // 2 linhas de cores
static const int DBRI_Y = 158, DEF_Y = 196, DVEL_Y = 234, DBAR_H = 30;
static const int DVOLTAR_Y = 274, DVOLTAR_H = 40;  // VOLTAR (a mochila agora abre pela cortina)
static const int DTR_X1 = 92, DTR_X2 = 232;         // trilho dos sliders

static void drawSliderSpr(int y, const char* nome, int val, int vmax) {
  spr.setFont(&fonts::Font2); spr.setTextDatum(middle_left);
  spr.setTextColor(COL_DIM); spr.drawString(nome, 6, y + DBAR_H / 2);
  int cy = y + DBAR_H / 2;
  int xv = DTR_X1 + (DTR_X2 - DTR_X1) * constrain(val, 0, vmax) / vmax;
  spr.fillRoundRect(DTR_X1, cy - 3, DTR_X2 - DTR_X1, 6, 3, 0x4208);
  spr.fillRoundRect(DTR_X1, cy - 3, xv - DTR_X1, 6, 3, TFT_CYAN);
  spr.fillCircle(xv, cy, 7, TFT_WHITE);
}

static void drawDream() {
  spr.fillScreen(TFT_BLACK);
  drawTopBar();
  const char* st = dreamConectado ? "conectada" : dreamConectando ? "conectando..." : dreamQuer ? "procurando" : "desligada";
  spr.setFont(&fonts::Font2); spr.setTextDatum(middle_left);
  spr.setTextColor(dreamConectado ? TFT_GREEN : dreamQuer ? TFT_YELLOW : COL_DIM);
  spr.drawString(st, 6, 34);
  // botao conectar/desconectar (salvo). Procurando: amarelo escuro "PROCURANDO..." (tocar cancela)
  const char* bt = dreamConectado ? "DESCONECTAR" : dreamConectando ? "CONECTANDO..." : dreamQuer ? "PROCURANDO..." : "CONECTAR";
  uint16_t bc = dreamConectado ? (uint16_t)TFT_DARKGREEN : dreamQuer ? (uint16_t)0x8400 : (uint16_t)0x4208;
  spr.fillRoundRect(DCON_X, DCON_Y, DCON_W, DCON_H, 6, bc);
  spr.setTextDatum(middle_center); spr.setTextColor(TFT_WHITE);
  spr.drawString(bt, DCON_X + DCON_W / 2, DCON_Y + DCON_H / 2 - 1);

  spr.setTextDatum(middle_center);
  spr.fillRoundRect(DRX[0], DON_Y, DR_W + 30, DON_H, 7, dreamLigado ? TFT_DARKGREEN : 0x2124);
  spr.setTextColor(TFT_WHITE); spr.drawString("LIGA", DRX[0] + (DR_W + 30) / 2, DON_Y + DON_H / 2 - 1);
  spr.fillRoundRect(124, DON_Y, DR_W + 30, DON_H, 7, !dreamLigado ? TFT_MAROON : 0x2124);
  spr.drawString("DESLIGA", 124 + (DR_W + 30) / 2, DON_Y + DON_H / 2 - 1);

  for (int i = 0; i < 6; i++) {
    int x = DRX[i % 3], y = DC_Y0 + (i / 3) * DC_DY;
    bool sel = (dreamModo == 0 && dreamLigado && dreamR == DREAM_RGB[i][0] && dreamG == DREAM_RGB[i][1] && dreamB == DREAM_RGB[i][2]);
    spr.fillRoundRect(x, y, DR_W, DC_H, 6, sel ? DREAM_COR[i] : 0x2124);
    spr.drawRoundRect(x, y, DR_W, DC_H, 6, DREAM_COR[i]);
  }

  drawSliderSpr(DBRI_Y, "brilho", dreamBrilho, 255);

  // efeito:  [-] [+]   efeito N        [DEMO]
  int ey = DEF_Y + DBAR_H / 2 - 1;
  spr.fillRoundRect(6,  DEF_Y, 40, DBAR_H, 6, TFT_NAVY); spr.setTextColor(TFT_WHITE);
  spr.drawString("-", 26, ey);
  spr.fillRoundRect(50, DEF_Y, 40, DBAR_H, 6, TFT_NAVY);
  spr.drawString("+", 70, ey);
  spr.setTextDatum(middle_left);
  spr.setTextColor(dreamModo == 1 && dreamLigado ? TFT_YELLOW : COL_DIM);
  char eb[16]; snprintf(eb, sizeof(eb), "efeito %d", dreamEf);
  spr.drawString(eb, 98, ey);
  spr.setTextDatum(middle_center);
  spr.fillRoundRect(180, DEF_Y, 54, DBAR_H, 6, dreamDemo ? TFT_MAROON : TFT_PURPLE);
  spr.setTextColor(TFT_WHITE); spr.drawString(dreamDemo ? "parar" : "demo", 207, ey);

  drawSliderSpr(DVEL_Y, "veloc", dreamVel, 255);
  spr.fillRoundRect(6, DVOLTAR_Y, 228, DVOLTAR_H, 8, TFT_NAVY);
  spr.setFont(&fonts::Font2); spr.setTextDatum(middle_center); spr.setTextColor(TFT_WHITE);
  spr.drawString("VOLTAR", 120, DVOLTAR_Y + DVOLTAR_H / 2);
  spr.pushSprite(0, 0);
}

static void tocarDream(int x, int y) {
  if (y >= DVOLTAR_Y) { modoTela = TELA_NORMAL; page = 1; Serial.println("Tela 1"); return; }   // volta pra principal
  if (x >= DCON_X && y >= DCON_Y && y < DCON_Y + DCON_H) { dreamSetAuto(!dreamQuer); return; }
  if (y >= DON_Y && y < DON_Y + DON_H) { dreamSetLiga(x < 120); return; }
  for (int i = 0; i < 6; i++) {
    int bx = DRX[i % 3], by = DC_Y0 + (i / 3) * DC_DY;
    if (x >= bx && x < bx + DR_W && y >= by && y < by + DC_H) { dreamSetCor(DREAM_RGB[i][0], DREAM_RGB[i][1], DREAM_RGB[i][2]); return; }
  }
  if (y >= DBRI_Y && y < DBRI_Y + DBAR_H && x >= DTR_X1 - 8) {
    dreamBrilho = constrain((x - DTR_X1) * 255 / (DTR_X2 - DTR_X1), 0, 255); dreamReaplicar(); return;
  }
  if (y >= DVEL_Y && y < DVEL_Y + DBAR_H && x >= DTR_X1 - 8) {
    dreamVel = constrain((x - DTR_X1) * 255 / (DTR_X2 - DTR_X1), 1, 255);
    if (dreamModo == 1) dreamEnviarEfeito();
    return;
  }
  if (y >= DEF_Y && y < DEF_Y + DBAR_H) {
    if (x >= 6 && x < 46) dreamSetEfeito(dreamEf == 0 ? 0 : dreamEf - 1);
    else if (x >= 50 && x < 90) dreamSetEfeito(dreamEf + 1);
    else if (x >= 180) dreamDemo = !dreamDemo;
    return;
  }
}

// --- Sliders por ARRASTO (chamados pelo handleTouch) ---
// Qual slider esta sob (x,y)? 1 = brilho, 2 = velocidade, 0 = nenhum.
static int dreamHitSlider(int x, int y) {
  if (x < DTR_X1 - 12) return 0;
  if (y >= DBRI_Y && y < DBRI_Y + DBAR_H) return 1;
  if (y >= DVEL_Y && y < DVEL_Y + DBAR_H) return 2;
  return 0;
}
static void dreamArrastaSlider(int which, int x) {
  int v = constrain((x - DTR_X1) * 255 / (DTR_X2 - DTR_X1), which == 2 ? 1 : 0, 255);
  if (which == 1) dreamBrilho = v; else dreamVel = v;   // move a bolinha JA (a tela redesenha a ~10 q/s)
  // mas so manda pro modulo de vez em quando: a cada quadro floda o BLE da mochila e o arrasto "morre"
  static uint32_t ult = 0;
  if (millis() - ult < 80) return;
  ult = millis();
  if (which == 1) dreamReaplicar(); else if (dreamModo == 1) dreamEnviarEfeito();
}

// ---------- Cortina de AJUSTES (arrastar de cima pra baixo a partir da barra) ----------
// Grade 2 x 4:   [MPU >]       [Tela auto]
//                [Som]         [Mochila >]
//                [Buzina ext >][Print]
//                [Dormir]      [Acorda mexendo]
// Embaixo: bateria, cartao, sensores, versao. Fecha arrastando pra cima.
static const int AJ_X[2] = { 6, 124 }, AJ_W = 110, AJ_H = 46, AJ_Y0 = 48, AJ_DY = 52;
static const int AJ_N = 8;

static void drawBotaoAjuste(int i, const char* titulo, const char* sub, uint16_t fundo, uint16_t corSub) {
  int x = AJ_X[i % 2], y = AJ_Y0 + (i / 2) * AJ_DY;
  spr.fillRoundRect(x, y, AJ_W, AJ_H, 8, fundo);
  spr.drawRoundRect(x, y, AJ_W, AJ_H, 8, COL_FRAME);
  spr.setFont(&fonts::Font2); spr.setTextDatum(middle_center);
  spr.setTextColor(TFT_WHITE);
  spr.drawString(titulo, x + AJ_W / 2, y + AJ_H / 2 - 9);
  spr.setTextColor(corSub);
  spr.drawString(sub, x + AJ_W / 2, y + AJ_H / 2 + 10);
}

static void drawAjustes() {
  spr.fillScreen(TFT_BLACK);
  drawTopBar();
  spr.setFont(&fonts::Font2);
  spr.setTextDatum(middle_left);  spr.setTextColor(TFT_WHITE); spr.drawString("Ajustes", 6, 36);
  spr.setTextDatum(middle_right); spr.setTextColor(COL_DIM);   spr.drawString("arraste p/ cima: fecha", 234, 36);

  const uint16_t LIG = TFT_DARKGREEN, DESL = 0x2124;
  drawBotaoAjuste(0, "MPU  >", mpuOk ? "configurar" : "sem sensor", TFT_NAVY, mpuOk ? COL_DIM : TFT_RED);
  drawBotaoAjuste(1, "Tela auto", TELA_AUTO ? "sim" : "nao", TELA_AUTO ? LIG : DESL, TELA_AUTO ? TFT_WHITE : COL_DIM);
  drawBotaoAjuste(2, "Som", !somOk ? "sem codec" : SOM_ON ? "sim" : "nao", SOM_ON && somOk ? LIG : DESL,
                  SOM_ON && somOk ? TFT_WHITE : COL_DIM);
  drawBotaoAjuste(3, "Mochila  >", dreamConectado ? "conectada" : dreamQuer ? "procurando" : "desligada",
                  TFT_NAVY, dreamConectado ? TFT_GREEN : dreamQuer ? TFT_YELLOW : COL_DIM);
  drawBotaoAjuste(4, "Buzina ext. >", RADIO433_OK ? "som/vol/alarme" : "sem 433", 0x7800, RADIO433_OK ? COL_DIM : TFT_RED);
  char subPrint[16];
  if (!sdOk)        snprintf(subPrint, sizeof(subPrint), "sem cartao");
  else if (printEm) snprintf(subPrint, sizeof(subPrint), "em %lu s...",
                             (unsigned long)((printEm > millis() ? printEm - millis() : 0) + 999) / 1000);
  else              snprintf(subPrint, sizeof(subPrint), "em 3 s");
  drawBotaoAjuste(5, "Print", subPrint, TFT_PURPLE, !sdOk ? TFT_RED : printEm ? TFT_YELLOW : COL_DIM);
  if (dormirConfirma && millis() - dormirConfirma > 3000) dormirConfirma = 0;   // "certeza?" expira
  drawBotaoAjuste(6, dormirConfirma ? "certeza?" : "Dormir", dormirConfirma ? "toque de novo" : "BOOT acorda",
                  dormirConfirma ? (uint16_t)TFT_RED : (uint16_t)0x4208, dormirConfirma ? TFT_WHITE : COL_DIM);
  drawBotaoAjuste(7, "Acorda mexendo", !mpuOk ? "sem MPU" : ACORDA_MEXENDO ? "sim" : "nao",
                  ACORDA_MEXENDO && mpuOk ? LIG : DESL, ACORDA_MEXENDO && mpuOk ? TFT_WHITE : COL_DIM);

  // informacoes
  char b[48];
  int y = AJ_Y0 + 4 * AJ_DY + 6;
  spr.setTextDatum(top_left);
  if (batPct >= 0) snprintf(b, sizeof(b), "Bateria: %.2f V  %d%%%s", batV, batPct, batCarregando ? "  carregando" : "");
  else             snprintf(b, sizeof(b), "Bateria: sem leitura (%.2f V)", batV);
  spr.setTextColor(batPct < 0 ? COL_DIM : batPct > 15 ? TFT_WHITE : TFT_RED);
  spr.drawString(b, 6, y);
  snprintf(b, sizeof(b), "Cartao SD: %s    MPU: %s", sdOk ? "ok" : "ausente", mpuOk ? "ok" : "--");
  spr.setTextColor(COL_DIM); spr.drawString(b, 6, y + 18);
  snprintf(b, sizeof(b), "433 %s  som %s  %s", PONTE_C3 ? (c3Ok ? (c3Radio ? "C3" : "C3?") : "C3--") : (tx433Ok ? "ok" : "--"),
           somOk ? "ok" : "--", VERSAO);
  spr.drawString(b, 6, y + 36);
  spr.pushSprite(0, 0);
}

static void abrirAjustes() { modoTela = TELA_AJUSTES; Serial.println("Ajustes"); }
static void fecharAjustes() { modoTela = TELA_NORMAL; mpuCfgConfirma = 0; dormirConfirma = 0; Serial.printf("Tela %d\n", page); }

static void tocarAjustes(int x, int y) {
  for (int i = 0; i < AJ_N; i++) {
    int bx = AJ_X[i % 2], by = AJ_Y0 + (i / 2) * AJ_DY;
    if (x < bx || x >= bx + AJ_W || y < by || y >= by + AJ_H) continue;
    switch (i) {
      case 0: modoTela = TELA_MPU; mpuCfgConfirma = 0; break;
      case 1: comandoConfig(String("TELA_AUTO ") + (TELA_AUTO ? "off" : "on")); break;
      case 2: comandoConfig(String("SOM_ON ") + (SOM_ON ? "off" : "on")); somBipe(); break;   // bipe so se ligou
      case 3: modoTela = TELA_DREAM; break;        // a tela da mochila tem o CONECTAR e o LIGA/DESLIGA
      case 4: modoTela = TELA_BUZEXT; break;
      case 5:
        // A cortina FICA aberta: em 3 s sai o print do que estiver na tela. Da tempo de
        // fechar (arrastar pra cima) e ir para outra tela, ou de deixar e "printar" a cortina.
        if (!sdOk) { Serial.println("[print] sem cartao SD"); break; }
        printEm = millis() + 3000;
        Serial.println("[print] em 3 s...");
        break;
      case 6:                                      // Dormir: 2 toques (o 1o pede "certeza?")
        if (!dormirConfirma) dormirConfirma = millis();
        else { dormirConfirma = 0; dormir("botao Dormir", true); }
        break;
      case 7: comandoConfig(String("ACORDA_MEXENDO ") + (ACORDA_MEXENDO ? "off" : "on")); break;   // salvo
    }
    return;
  }
}

// ---------- Tela de config do MPU (aberta pelos Ajustes) ----------
// [GRAVAR] [AUTO sim/nao] [PADRAO]  +  barras "acende abaixo de" / "apaga acima de"
// (com a seta amarela = onde o braco esta AGORA) + estado ao vivo + VOLTAR.
// Enquanto esta tela esta aberta a tela nao apaga sozinha (da pra testar olhando).
static const int MC_BY = 46, MC_BH = 36, MC_BW = 74;
static const int MC_BX[3] = { 4, 83, 162 };
static const int MC_BAR_Y[2] = { 92, 144 }, MC_BAR_H = 46;
static const int MC_TR_X1 = 16, MC_TR_X2 = 224;
static const float MC_MIN[2] = { 5, 10 }, MC_MAX[2] = { 80, 90 };
static const int MC_VOLTAR_Y = 268, MC_VOLTAR_H = 46;

static int mpuCfgX(int i, float v) {
  return MC_TR_X1 + (int)((v - MC_MIN[i]) / (MC_MAX[i] - MC_MIN[i]) * (MC_TR_X2 - MC_TR_X1));
}

static void drawMpuBarra(int i) {
  int y = MC_BAR_Y[i];
  float v = (i == 0) ? mpuAngLiga : mpuAngApaga;
  char t[16]; snprintf(t, sizeof(t), "%.0f graus", v);
  spr.setFont(&fonts::Font2);
  spr.setTextDatum(top_left);  spr.setTextColor(TFT_WHITE);
  spr.drawString(i == 0 ? "Acende abaixo de" : "Apaga acima de", 6, y);
  spr.setTextDatum(top_right); spr.setTextColor(TFT_CYAN);
  spr.drawString(t, 234, y);
  int yt = y + 32, xv = mpuCfgX(i, v);
  spr.fillRoundRect(MC_TR_X1, yt - 3, MC_TR_X2 - MC_TR_X1, 6, 3, 0x4208);
  spr.fillRoundRect(MC_TR_X1, yt - 3, xv - MC_TR_X1, 6, 3, TFT_CYAN);
  if (mpuOk && mpuTemRef) {                     // seta: angulo do braco agora
    float a = constrain(mpuAngulo, MC_MIN[i], MC_MAX[i]);
    int xa = mpuCfgX(i, a);
    spr.fillTriangle(xa - 5, yt - 14, xa + 5, yt - 14, xa, yt - 6, TFT_YELLOW);
  }
  spr.fillCircle(xv, yt, 8, TFT_WHITE);
}

static void drawMpuCfg() {
  if (mpuCfgConfirma && millis() - mpuCfgConfirma > 3000) mpuCfgConfirma = 0;   // "certeza?" expira
  spr.fillScreen(TFT_BLACK);
  drawTopBar();
  spr.setFont(&fonts::Font2);
  spr.setTextDatum(middle_left); spr.setTextColor(TFT_WHITE);
  spr.drawString("Tela pelo pulso (MPU)", 6, 34);

  const char* nome[3] = { "GRAVAR", TELA_AUTO ? "AUTO sim" : "AUTO nao", mpuCfgConfirma ? "certeza?" : "PADRAO" };
  const uint16_t cor[3] = { mpuTemRef ? (uint16_t)TFT_NAVY : (uint16_t)TFT_ORANGE,
                            TELA_AUTO ? (uint16_t)TFT_DARKGREEN : (uint16_t)TFT_MAROON,
                            mpuCfgConfirma ? (uint16_t)TFT_RED : (uint16_t)0x4208 };
  spr.setTextDatum(middle_center); spr.setTextColor(TFT_WHITE);
  for (int k = 0; k < 3; k++) {
    spr.fillRoundRect(MC_BX[k], MC_BY, MC_BW, MC_BH, 8, cor[k]);
    spr.drawString(nome[k], MC_BX[k] + MC_BW / 2, MC_BY + MC_BH / 2);
  }

  drawMpuBarra(0);
  drawMpuBarra(1);

  // estado ao vivo
  char b[40];
  int y = MC_BAR_Y[1] + MC_BAR_H + 10;
  spr.setTextDatum(top_left);
  if (!mpuOk)          { spr.setTextColor(TFT_RED);    spr.drawString("MPU nao responde", 6, y); }
  else if (!mpuTemRef) { spr.setTextColor(TFT_YELLOW); spr.drawString("Sem posicao: toque GRAVAR", 6, y); }
  else {
    bool acende = mpuAngulo < mpuAngLiga, apaga = mpuAngulo > mpuAngApaga;
    snprintf(b, sizeof(b), "agora: %.0f graus -> %s", mpuAngulo,
             acende ? "acenderia" : apaga ? "apagaria" : "mantem");
    spr.setTextColor(acende ? TFT_GREEN : apaga ? TFT_RED : TFT_YELLOW);
    spr.drawString(b, 6, y);
  }
  spr.setTextColor(COL_DIM);
  spr.drawString("GRAVAR = posicao de olhar", 6, y + 22);
  spr.drawString("PADRAO = angulos de fabrica", 6, y + 40);

  spr.fillRoundRect(4, MC_VOLTAR_Y, 232, MC_VOLTAR_H, 8, TFT_NAVY);
  spr.setTextDatum(middle_center); spr.setTextColor(TFT_WHITE);
  spr.drawString("VOLTAR", 120, MC_VOLTAR_Y + MC_VOLTAR_H / 2);
  spr.pushSprite(0, 0);
}

static void mpuCfgSalvar() {
  prefs.begin("s16cfg", false);
  prefs.putFloat("mpu_liga", mpuAngLiga); prefs.putFloat("mpu_apaga", mpuAngApaga);
  prefs.end();
  Serial.printf("mpu: acende abaixo de %.0f, apaga acima de %.0f (salvo)\n", mpuAngLiga, mpuAngApaga);
}

// Qual barra esta sob (x,y)? 0 = acende, 1 = apaga, -1 = nenhuma
static int mpuCfgHitBarra(int x, int y) {
  for (int i = 0; i < 2; i++) if (y >= MC_BAR_Y[i] && y < MC_BAR_Y[i] + MC_BAR_H) return i;
  return -1;
}
static void mpuCfgArrasta(int i, int x) {
  float t = constrain((float)(x - MC_TR_X1) / (MC_TR_X2 - MC_TR_X1), 0.0f, 1.0f);
  float v = roundf(MC_MIN[i] + t * (MC_MAX[i] - MC_MIN[i]));
  if (i == 0) { mpuAngLiga = v;  if (mpuAngApaga < mpuAngLiga + 5) mpuAngApaga = mpuAngLiga + 5; }   // 5 graus de folga
  else        { mpuAngApaga = v; if (mpuAngLiga > mpuAngApaga - 5) mpuAngLiga = mpuAngApaga - 5; }
}

static void tocarMpuCfg(int x, int y) {
  if (y >= MC_VOLTAR_Y) { modoTela = TELA_NORMAL; page = 1; Serial.println("Tela 1"); return; }   // volta pra principal
  if (y < MC_BY || y >= MC_BY + MC_BH) return;
  if (x < MC_BX[1]) { mpuSalvarRef(); somBipe(); }
  else if (x < MC_BX[2]) comandoConfig(String("TELA_AUTO ") + (TELA_AUTO ? "off" : "on"));
  else if (!mpuCfgConfirma) mpuCfgConfirma = millis();          // 1o toque: pede confirmacao
  else {                                                          // 2o toque: confirma
    mpuAngLiga = MPU_ANG_LIGA; mpuAngApaga = MPU_ANG_APAGA; mpuCfgConfirma = 0;
    mpuCfgSalvar();
  }
}

// ---------- Tela da BUZINA EXTERNA (aberta pelos Ajustes) ----------
// As 7 funcoes do controle original, mandadas com a identidade do display (00 9F 1D):
//   [        BUZINA        ]
//   [ volume - ] [ volume + ]
//   [  < som   ] [  som >   ]   (= segurar vol+ / vol- no controle: troca o som)
//   [alarme liga][alarme desl]
//   [        VOLTAR        ]
static const uint8_t BX_CMD[7]  = { 0xAA, 0xCC, 0x99, 0x66, 0xFF, 0x55, 0x33 };
static const char*   BX_NOME[7] = { "BUZINA", "volume -", "volume +", "< som", "som >", "alarme liga", "alarme desliga" };
static const int BX_X[2] = { 6, 124 }, BX_W = 110, BX_H = 46, BX_Y0 = 108, BX_DY = 52;
static const int BX_BUZ_Y = 50, BX_BUZ_H = 50, BX_VOLTAR_Y = 270, BX_VOLTAR_H = 44;
static int      bxAceso = -1;           // botao tocado (destaque rapido)
static uint32_t bxAcesoAte = 0;

// Agenda um comando para a buzina externa (sai quando o radio 433 liberar; ver buzExtLoop)
static void buzExtEnviar(uint8_t cmd) {
  if (PONTE_C3) { c3Enviar(C3_BUZEXT, cmd); return; }
  buzExtCmd = cmd; buzExtPend = true;
}

static void bxRet(int i, int& x, int& y, int& w, int& h) {   // retangulo do botao i
  if (i == 0) { x = 6; y = BX_BUZ_Y; w = 228; h = BX_BUZ_H; return; }
  x = BX_X[(i - 1) % 2]; y = BX_Y0 + ((i - 1) / 2) * BX_DY; w = BX_W; h = BX_H;
}

static void drawBuzExt() {
  spr.fillScreen(TFT_BLACK);
  drawTopBar();
  spr.setFont(&fonts::Font2);
  spr.setTextDatum(middle_left); spr.setTextColor(TFT_WHITE);
  spr.drawString("Buzina externa (433)", 6, 34);
  spr.setTextDatum(middle_center);
  for (int i = 0; i < 7; i++) {
    int x, y, w, h; bxRet(i, x, y, w, h);
    bool aceso = (i == bxAceso && millis() < bxAcesoAte);
    uint16_t fundo = aceso ? (uint16_t)0x6B4D : (i == 0) ? (uint16_t)0x7800 : (uint16_t)0x2124;
    spr.fillRoundRect(x, y, w, h, 8, fundo);
    spr.drawRoundRect(x, y, w, h, 8, i == 0 ? TFT_ORANGE : COL_FRAME);
    spr.setTextColor(TFT_WHITE);
    spr.drawString(BX_NOME[i], x + w / 2, y + h / 2);
  }
  spr.fillRoundRect(6, BX_VOLTAR_Y, 228, BX_VOLTAR_H, 8, TFT_NAVY);
  spr.drawString("VOLTAR", 120, BX_VOLTAR_Y + BX_VOLTAR_H / 2);
  spr.pushSprite(0, 0);
}

static void tocarBuzExt(int x, int y) {
  if (y >= BX_VOLTAR_Y) { modoTela = TELA_NORMAL; page = 1; Serial.println("Tela 1"); return; }
  for (int i = 0; i < 7; i++) {
    int bx, by, bw, bh; bxRet(i, bx, by, bw, bh);
    if (x < bx || x >= bx + bw || y < by || y >= by + bh) continue;
    bxAceso = i; bxAcesoAte = millis() + 250;
    logEvent("buzina externa: %s", BX_NOME[i]);
    buzExtEnviar(BX_CMD[i]);
    if (i == 0) hornFlashUntil = millis() + 600;
    return;
  }
}

static void drawPage() {
  if (modoTela == TELA_AJUSTES) { drawAjustes(); return; }
  if (modoTela == TELA_MPU)     { drawMpuCfg();  return; }
  if (modoTela == TELA_BUZEXT)  { drawBuzExt();  return; }
  if (modoTela == TELA_DREAM)   { drawDream();   return; }
  if (modoTela == TELA_PISCAS)  { drawPiscas();  return; }
  if (page == 0) drawBms();
  else if (page == 2) drawControls();
  else drawData();
}

// ---------- Touch: deslizar troca de tela, toque aperta botao ----------
// Roda sempre (com ou sem roda): sem a roda, so a cortina de Ajustes funciona.
//   arrastar pra BAIXO comecando na barra de cima -> abre os Ajustes
//   arrastar pra CIMA com os Ajustes abertos       -> fecha
static void handleTouch() {
  static bool dedo = false;
  static bool acordouComToque = false;            // o toque que acendeu a tela nao aperta botao
  static int arrastaSlider = 0;                   // slider do Dream sendo arrastado (1/2) ou 0
  static int arrastaMpu = -1;                     // barra da tela do MPU sendo arrastada (0/1) ou -1
  static int x0, y0, xu, yu;
  static uint32_t t0;
  int32_t x, y;
  if (tft.getTouch(&x, &y)) {
    mpuUltimoToque = millis();                     // tocar conta como "em uso"
    if (!dedo) {
      dedo = true; x0 = x; y0 = y; t0 = millis();
      acordouComToque = !telaAcesa;                // estava apagada? este toque so acende
      if (!telaAcesa) { telaAcesa = true; tft.setBrightness(MPU_BRILHO); }
      // comecou em cima de um slider/barra? entao e arrasto, nao swipe de tela
      arrastaSlider = 0; arrastaMpu = -1;
      if (!acordouComToque) {
        if (modoTela == TELA_MPU) arrastaMpu = mpuCfgHitBarra(x, y);
        else if (modoTela == TELA_DREAM) arrastaSlider = dreamHitSlider(x, y);
      }
    }
    xu = x; yu = y;
    if (arrastaSlider) dreamArrastaSlider(arrastaSlider, x);   // arrasto ao vivo
    if (arrastaMpu >= 0) mpuCfgArrasta(arrastaMpu, x);
    return;
  }
  if (!dedo) return;
  dedo = false;                                   // o dedo acabou de soltar
  if (acordouComToque) { acordouComToque = false; arrastaSlider = 0; arrastaMpu = -1; return; }
  if (arrastaSlider) {                                 // era arrasto de slider: manda o valor final e sai
    if (arrastaSlider == 1) dreamReaplicar(); else if (dreamModo == 1) dreamEnviarEfeito();
    arrastaSlider = 0; return;                         // nem swipe nem toque
  }
  if (arrastaMpu >= 0) { mpuCfgSalvar(); arrastaMpu = -1; return; }   // salva ao soltar

  int dx = xu - x0, dy = yu - y0;
  bool vertical = abs(dy) > 50 && abs(dy) > abs(dx);
  bool toque = abs(dx) < 15 && abs(dy) < 15 && millis() - t0 < 600;
  // cortina de CIMA (Ajustes): puxa de cima (comecando na barra) / empurra pra cima
  if (modoTela == TELA_NORMAL && vertical && dy > 0 && y0 < 30) { abrirAjustes(); return; }
  if (modoTela == TELA_AJUSTES && vertical && dy < 0) { fecharAjustes(); return; }
  // cortina de BAIXO (Piscas): puxa de baixo (comecando na borda, na "alca") / empurra pra baixo
  if (modoTela == TELA_NORMAL && vertical && dy < 0 && y0 >= 290) { modoTela = TELA_PISCAS; Serial.println("Piscas"); return; }
  if (modoTela == TELA_PISCAS && vertical && dy > 0) { modoTela = TELA_NORMAL; Serial.printf("Tela %d\n", page); return; }
  if (modoTela == TELA_AJUSTES) { if (toque) tocarAjustes(x0, y0); return; }
  if (modoTela == TELA_MPU)     { if (toque) tocarMpuCfg(x0, y0); return; }
  if (modoTela == TELA_BUZEXT)  { if (toque) tocarBuzExt(x0, y0); return; }
  if (modoTela == TELA_DREAM)   { if (toque) tocarDream(x0, y0); return; }
  if (modoTela == TELA_PISCAS)  { if (toque) tocarPiscas(x0, y0); return; }

  if (!(client && client->isConnected())) return;   // sem roda: paginas paradas (so as cortinas)
  if (abs(dx) > 60 && abs(dx) > abs(dy)) {        // deslizou na horizontal
    if (dx < 0 && page < NUM_PAGES - 1) page++;   // para a esquerda: tela da direita
    if (dx > 0 && page > 0) page--;
    Serial.printf("Tela %d\n", page);
  } else if (toque) {                             // foi um toque
    if (page == 2) tocarControles(x0, y0);
  }
}

// ---------- Botao BOOT = buzina ----------
static const int PIN_BUZINA = 0;           // botao BOOT da placa
static void handleHornButton() {
  static bool estavaApertado = false;
  static uint32_t ultimaMudanca = 0;
  bool apertado = digitalRead(PIN_BUZINA) == LOW;   // INPUT_PULLUP: apertado = LOW
  if (apertado != estavaApertado && millis() - ultimaMudanca > 30) {   // filtra o "repique"
    ultimaMudanca = millis();
    estavaApertado = apertado;
    if (apertado) {                               // so na hora de apertar
      sendKs(0x88, 0, 0, 0, 0);
      somBipe();                                  // bipe local de confirmacao
      hornFlashUntil = millis() + 600;
      logEvent("botao BOOT: buzina");
    }
  }
}

// ================= MOCHILA (modulo Dream / Happy Lighting) — cliente BLE =================
// ETAPA 1 (provar coexistencia): o display conecta no Dream SOB COMANDO (Serial),
// so para confirmar que a roda e os apps continuam de pe com um 2o cliente ativo.
// Endereco fixo (do nRF): 2B:80:04:13:0E:4D. Conexao DIRETA pelo endereco, sem
// mexer no scanner da roda. Escrita em FFD9 (dentro do servico FFD5), sem resposta.
// (estado la em cima, antes do console; aqui so as funcoes)
// Envio bruto (so quando conectado). A tarefa do NimBLE e a do loop nao
// escrevem juntas: tudo sai do loop/console, entao e seguro.
static void dreamEnviar(const uint8_t* d, size_t n) {
  if (!dreamConectado || !dreamWr) return;
  for (int r = 0; r < 2; r++) { dreamWr->writeValue(d, n, false); delay(6); }
}
static void dreamEnviarLiga(bool on) { uint8_t c[3] = { 0xCC, on ? (uint8_t)0x23 : (uint8_t)0x24, 0x33 }; dreamEnviar(c, 3); }
static void dreamEnviarCor(uint8_t r, uint8_t g, uint8_t b) {
  uint8_t br = dreamBrilho;                       // escala a cor pelo brilho
  uint8_t c[7] = { 0x56, (uint8_t)(r*br/255), (uint8_t)(g*br/255), (uint8_t)(b*br/255), 0x00, 0xF0, 0xAA };
  dreamEnviar(c, 7);
}
static void dreamEnviarEfeito() {
  uint8_t bri = dreamBrilho < 0x19 ? 0x19 : dreamBrilho;
  uint8_t c[7] = { 0x9E, 0x00, dreamEf, dreamVel, bri, 0x00, 0xE9 };
  dreamEnviar(c, 7);
}
// Aplica o estado atual (usado ao conectar/reconectar, pra mochila casar com a tela)
static void dreamAplicar() {
  dreamEnviarLiga(dreamLigado);
  if (!dreamLigado) return;
  if (dreamModo == 0) dreamEnviarCor(dreamR, dreamG, dreamB);
  else dreamEnviarEfeito();
}
// --- API de alto nivel (mexem no estado local E mandam, se conectado) ---
static void dreamSetLiga(bool on) { dreamLigado = on; if (!on) dreamDemo = false; dreamEnviarLiga(on); if (on) dreamAplicar(); }
static void dreamSetCor(uint8_t r, uint8_t g, uint8_t b) {
  dreamModo = 0; dreamR = r; dreamG = g; dreamB = b; dreamLigado = true;
  dreamEnviarLiga(true); dreamEnviarCor(r, g, b);
}
static void dreamSetEfeito(uint8_t ef) { dreamModo = 1; dreamEf = ef; dreamLigado = true; dreamEnviarLiga(true); dreamEnviarEfeito(); }

// --- Conexao ASSINCRONA (nao bloqueia a ponte) ---
// So precisamos saber quando a mochila CAI (a conexao em si e feita, bloqueante,
// no dreamLoop). Ao cair, marca desconectado e espera antes de reconectar.
class DreamCB : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient*, int reason) override {
    dreamConectado = false; dreamProxTentativa = millis() + 4000;
    Serial.printf("[dream] caiu (%d)\n", reason);
  }
} dreamCB;

// Liga/desliga o "manter conectado" e SALVA (botao da tela e 'dream on/off').
static void dreamSetAuto(bool on) {
  DREAM_AUTO = on; dreamQuer = on;
  prefs.begin("s16cfg", false); prefs.putBool("DREAM_AUTO", on); prefs.end();
  if (!on) { if (dreamCli && dreamCli->isConnected()) dreamCli->disconnect(); dreamConectado = false; }
  else { dreamProxTentativa = 0; dreamFalhas = 0; }
  Serial.printf("[dream] manter conectado: %s\n", on ? "SIM" : "NAO");
}
static void dreamReaplicar() {   // re-manda o modo atual (apos mexer brilho/cor/vel)
  if (!dreamLigado) return;
  if (dreamModo == 0) dreamEnviarCor(dreamR, dreamG, dreamB); else dreamEnviarEfeito();
}
// --- Estado da mochila SALVO na flash (ligada, modo, cor, brilho, efeito, velocidade) ---
// Antes nao era salvo: ao religar, o display achava "desligada" e apagava a mochila
// ao reconectar. Agora volta como estava. Grava so depois de 2 s sem mudar (o slider
// nao gasta a flash a cada passo; o DEMO, que muda o efeito a cada 1,5 s, so grava ao parar).
static void dreamEstado(uint8_t* a) {
  a[0] = dreamLigado ? 1 : 0; a[1] = dreamModo; a[2] = dreamR; a[3] = dreamG; a[4] = dreamB;
  a[5] = dreamBrilho; a[6] = dreamEf; a[7] = dreamVel;
}
static void dreamCarregar() {
  uint8_t a[8];
  prefs.begin("s16cfg", true);
  size_t n = prefs.getBytes("dream", a, 8);
  prefs.end();
  if (n != 8) return;                            // nunca salvo: fica o padrao
  dreamLigado = a[0] != 0; dreamModo = a[1]; dreamR = a[2]; dreamG = a[3]; dreamB = a[4];
  dreamBrilho = a[5]; dreamEf = a[6]; dreamVel = a[7];
  Serial.printf("[dream] estado salvo: %s, %s\n", dreamLigado ? "ligada" : "desligada", dreamModo ? "efeito" : "cor");
}
static void dreamSalvarSeMudou(bool agora) {     // agora = true: grava ja (antes de dormir)
  static uint8_t salvo[8], ultimo[8];
  static bool iniciou = false;
  static uint32_t tMudou = 0;
  uint8_t a[8]; dreamEstado(a);
  if (!iniciou) { memcpy(salvo, a, 8); memcpy(ultimo, a, 8); iniciou = true; return; }
  if (memcmp(a, ultimo, 8)) { memcpy(ultimo, a, 8); tMudou = millis(); }
  if (memcmp(a, salvo, 8) && (agora || millis() - tMudou > 2000)) {
    prefs.begin("s16cfg", false); prefs.putBytes("dream", a, 8); prefs.end();
    memcpy(salvo, a, 8);
  }
}

static void dreamLoop() {
  dreamSalvarSeMudou(false);
  // aplica o estado desejado uma vez, ja no contexto do loop (nao no callback do NimBLE)
  if (dreamConectado && dreamAplicarPend) { dreamAplicarPend = false; dreamAplicar(); }

  // DEMO: percorre os efeitos sozinho
  static uint32_t tDemo = 0;
  if (dreamDemo && dreamConectado && dreamLigado && !freioAtivo && millis() - tDemo > 1500) { tDemo = millis(); dreamSetEfeito(dreamEf + 1); }

  // A RODA VEM SEMPRE PRIMEIRO: so tenta a mochila com a roda ja conectada,
  // para o scan/conexao da roda nunca disputar o radio com a mochila no boot.
  // A RODA VEM SEMPRE PRIMEIRO
  bool rodaOk = client && client->isConnected();
  if (!rodaOk || !dreamQuer || dreamConectado || millis() < dreamProxTentativa) return;
  // A conexao TRAVA o loop ate 2,5 s (freio, buzina, 433, touch param). Andando, nao
  // tenta: so com a roda parada (ou sem dados). Quem liga a mochila andando, ela
  // conecta na proxima parada.
  if (msSemDados() < 2000 && wd.speed > 3) return;

  // Conexao BLOQUEANTE (a assincrona estourava o buffer do radio - obrigado Leandro).
  // Com timeout curto, se o modulo estiver ausente a tentativa custa ~2,5 s em vez
  // de travar; tenta de novo so a cada 15 s. Com o modulo presente, conecta rapido.
  if (!dreamCli) { dreamCli = NimBLEDevice::createClient(); dreamCli->setClientCallbacks(&dreamCB, false); }
  dreamCli->setConnectTimeout(2500);
  NimBLEAddress a(std::string(DREAM_MAC), BLE_ADDR_PUBLIC);   // provado: tipo PUBLIC
  // retorno visual ANTES de travar ate 2,5 s: a tela da mochila mostra "CONECTANDO..."
  dreamConectando = true;
  if (modoTela == TELA_DREAM && telaAcesa) drawPage();
  bool ok = dreamCli->connect(a);
  dreamConectando = false;
  if (ok) {
    NimBLERemoteService* svc = dreamCli->getService(DREAM_SVC);
    dreamWr = svc ? svc->getCharacteristic(DREAM_WRCHR) : nullptr;
    if (dreamWr) { dreamConectado = true; dreamFalhas = 0; dreamAplicarPend = true; Serial.println("[dream] conectado (FFD9 ok)"); }
    else { dreamCli->disconnect(); dreamProxTentativa = millis() + 15000; }
  } else {
    // ausente: espera cada vez mais (15 s nas 3 primeiras, depois 1 min, depois 5 min).
    // O botao CONECTAR / 'dream on' zera a contagem e tenta na hora.
    dreamFalhas++;
    uint32_t espera = dreamFalhas <= 3 ? 15000UL : dreamFalhas <= 6 ? 60000UL : 300000UL;
    dreamProxTentativa = millis() + espera;
    Serial.printf("[dream] nao encontrado (modulo desligado/longe; falha %d); nova tentativa em %lu s\n",
                  dreamFalhas, (unsigned long)(espera / 1000));
  }
}

// ================= 433 MHz: RECEBER o controle da buzina (SRX882S no IO3) =================
// O receptor solta ruido o tempo todo quando nao ha sinal, entao a interrupcao roda
// direto - por isso ela e curtinha e decodifica "na hora", borda a borda:
//   intervalo BAIXO longo (1-20 ms)  = sincronismo: comeca um quadro
//   par ALTO+BAIXO curto+longo = bit 0, longo+curto = bit 1 (tempos do controle da buzina)
//   32 bits = quadro completo -> rxCodigo / rxContador (o loop decide o que fazer)
// Qualquer pulso fora das faixas descarta o quadro (e assim o ruido nunca vira codigo).
static volatile uint32_t rxCodigo = 0;     // ultimo quadro valido (32 bits)
static volatile uint32_t rxContador = 0;   // +1 a cada quadro valido
static const uint16_t RX_CURTO_MIN = 200, RX_CURTO_MAX = 600, RX_LONGO_MIN = 700, RX_LONGO_MAX = 1150;
static const uint32_t RX_SINC_MIN = 1000, RX_SINC_MAX = 20000;

void IRAM_ATTR rx433ISR() {
  static uint32_t tAnt = 0, bits = 0, altoAnt = 0;
  static int nb = -1;                         // bits ja lidos; -1 = esperando sincronismo
  uint32_t agora = micros();
  uint32_t d = agora - tAnt;
  tAnt = agora;
  bool eraAlto = !digitalRead(RX433_PIN);     // o intervalo que acabou tinha o nivel oposto ao atual
  if (eraAlto) {
    altoAnt = d;
    bool ok = (d >= RX_CURTO_MIN && d <= RX_CURTO_MAX) || (d >= RX_LONGO_MIN && d <= RX_LONGO_MAX);
    if (!ok) nb = -1;
    return;
  }
  // terminou um intervalo BAIXO
  if (d >= RX_SINC_MIN && d <= RX_SINC_MAX) { nb = 0; bits = 0; return; }   // sincronismo
  if (nb < 0) return;
  bool hCurto = altoAnt <= RX_CURTO_MAX, lCurto = d >= RX_CURTO_MIN && d <= RX_CURTO_MAX;
  bool lLongo = d >= RX_LONGO_MIN && d <= RX_LONGO_MAX;
  if (hCurto && lLongo)       bits = bits << 1;          // curto + longo = 0
  else if (!hCurto && lCurto) bits = (bits << 1) | 1;    // longo + curto = 1
  else { nb = -1; return; }
  if (++nb == 32) { rxCodigo = bits; rxContador++; nb = -1; }   // o proximo sincronismo reabre
}

// ================= 433 MHz: imitar o controle dos piscas (STX882 no IO21) =================
// Independente do BLE e do I2C: usa o periferico RMT, que gera os pulsos sozinho.
// Protocolo (ver README): 40 bits = 5 bytes [3C 24 06 cmd cmd^05] + pulso curto + ~9 ms.
// Bit 1 = ligado ~990us + desligado ~330us; Bit 0 = o contrario.
// (constantes/estado do 433 estao la em cima, antes do console que os usa)
static void tx433Montar(rmt_data_t* s, uint8_t cmd) {
  uint8_t b[5] = { TX433_IDENT[0], TX433_IDENT[1], TX433_IDENT[2], cmd, (uint8_t)(cmd ^ 0x05) };
  int k = 0;
  for (int by = 0; by < 5; by++)
    for (int i = 7; i >= 0; i--) {
      bool um = (b[by] >> i) & 1;
      s[k].level0 = 1; s[k].duration0 = um ? TX433_LONGO : TX433_CURTO;
      s[k].level1 = 0; s[k].duration1 = um ? TX433_CURTO : TX433_LONGO;
      k++;
    }
  s[k].level0 = 1; s[k].duration0 = TX433_CURTO;
  s[k].level1 = 0; s[k].duration1 = TX433_SILENCIO;
}
// Fila de 1 posicao: o comando mais recente ganha. Apertar dois botoes em
// sequencia rapida nao perde o segundo; ele sai assim que o radio liberar.
static volatile bool    tx433Pend = false;
static volatile uint8_t tx433Cmd = 0;
static uint32_t         tx433LivreEm = 0;      // quando o radio volta a estar livre (por tempo)
static void tx433Enviar(uint8_t cmd) { if (PONTE_C3) return; tx433Cmd = cmd; tx433Pend = true; }   // so agenda (com o C3: quem transmite e ele)
static void tx433Loop() {
  // Em vez de depender do rmtTransmitCompleted() (que pode ficar preso em
  // "ocupado" apos um envio assincrono), espacamos os envios por tempo:
  // cada quadro dura ~62 ms (40 bits + silencio), vezes as repeticoes.
  if (!tx433Pend || !tx433Ok || millis() < tx433LivreEm) return;
  tx433Pend = false;
  uint8_t cmd = tx433Cmd;
  for (int r = 0; r < TX433_REPET; r++) tx433Montar(&tx433Simb[r * TX433_SIMB], cmd);
  rmtWriteAsync(TX433_PIN, tx433Simb, TX433_SIMB * TX433_REPET);
  tx433LivreEm = millis() + (uint32_t)TX433_REPET * 63 + 60;
  Serial.printf("[433] 3C 24 06 %02X %02X x%d\n", cmd, cmd ^ 0x05, TX433_REPET);
}
static void tx433Setup() {
  if (PONTE_C3) { Serial.println("433: no C3 (PONTE_C3) - o display nao usa o radio"); return; }
  tx433Ok = rmtInit(TX433_PIN, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 1000000);   // 1 tick = 1 us
  Serial.printf("433: %s\n", tx433Ok ? "ok (IO21)" : "ERRO ao iniciar o RMT");
  if (BOTOES_PISCA) {
    pinMode(PIN_PISCA_ESQ, INPUT_PULLUP);
    pinMode(PIN_PISCA_DIR, INPUT_PULLUP);
  }
  // receptor: cada borda do sinal chama a interrupcao, que decodifica sozinha
  pinMode(RX433_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(RX433_PIN), rx433ISR, CHANGE);
  Serial.printf("433 rx: escutando o controle no IO%d\n", RX433_PIN);
  prefs.begin("s16cfg", true);
  luzBase = prefs.getInt("luz_base", LUZ_OFF);
  prefs.end();
  // Ao ligar/acordar, as lanternas voltam ao modo base salvo (ao dormir elas sao apagadas).
  // O pisca de curva nao volta: e coisa do momento.
  if (luzBase != LUZ_OFF) {
    tx433Enviar(LUZ_CMD[luzBase]);               // sai no loop, quando o radio estiver livre
    Serial.printf("433: restaurando a luz base (%s)\n", LUZ_NOME[luzBase]);
  }
}

// ---- Logica das luzes/piscas ----
// O que as lanternas devem mostrar AGORA, por prioridade: pisca de curva > freio > modo base.
static void luzAplicarAtual() {
  if (piscaAtivo) return;                   // o pisca de curva manda (o freio nao interfere)
  tx433Enviar(freioAtivo ? CMD_LUZ_VERMELHO : LUZ_CMD[luzBase]);
}
static void luzSetBase(int modo) {         // escolhe o modo "parado" e ja aplica
  if (PONTE_C3) { c3Enviar(C3_LUZBASE, modo); return; }   // o C3 aplica e devolve o estado
  luzBase = modo;
  prefs.begin("s16cfg", false); prefs.putInt("luz_base", luzBase); prefs.end();
  luzAplicarAtual();                        // (com pisca ligado, aplica ao desligar; freando, segue vermelho)
  logEvent("luz base: %s", LUZ_NOME[modo]);
}
// Aperta o pisca: liga; apertando de novo, VOLTA ao modo base (nao manda "desligar").
static void piscaToggle(int lado) {         // 1 esquerda, 2 direita
  if (PONTE_C3) { c3Enviar(C3_PISCA, lado); return; }
  if (piscaAtivo == lado) {                 // desliga -> volta ao que estava (ou vermelho, se freando)
    piscaAtivo = 0;
    luzAplicarAtual();
    logEvent("pisca off -> %s", freioAtivo ? "freio" : LUZ_NOME[luzBase]);
  } else {
    piscaAtivo = lado;
    tx433Enviar(lado == 1 ? CMD_PISCA_ESQ : CMD_PISCA_DIR);
    logEvent("pisca %s", lado == 1 ? "esquerda" : "direita");
  }
}
// ---- LUZ DE FREIO ----
// Liga/desliga o "modo freio": piscas (via luzAplicarAtual, respeitando o pisca de curva)
// e mochila (vermelho sólido no brilho maximo, SEM mexer no estado salvo; ao soltar,
// dreamAplicar devolve exatamente o que estava).
static void freioAplicar(bool on) {
  luzAplicarAtual();
  if (!dreamConectado) return;
  if (on) {
    dreamEnviarLiga(true);
    uint8_t c[7] = { 0x56, 0xFF, 0x00, 0x00, 0x00, 0xF0, 0xAA };   // vermelho puro, brilho maximo
    dreamEnviar(c, 7);
  } else {
    dreamAplicar();
  }
}

// A roda manda a corrente ~2x por segundo (frame A9). Uma leitura abaixo de -2 A andando
// ja acende; para apagar, 2 leituras seguidas acima de -0,8 A (ou parado) e no minimo
// 1 s aceso. Entre os dois limites, mantem como esta (histerese: nao fica piscando).
// Com o C3: estado e eventos que chegaram pelo canal (o callback so guardou)
static void c3Loop() {
  if (!PONTE_C3) return;
  if (c3Ok && millis() - c3EstadoEm > 6000) { c3Ok = false; Serial.println("[c3] sem estado do C3 ha 6 s"); }
  if (!c3Ok) {                                   // ainda nao respondeu: repete o "oi" a cada 3 s
    static uint32_t tOi = 0;
    if (mainRemote && millis() - tOi > 3000) { tOi = millis(); c3Enviar(C3_HELLO, 0); }
    return;
  }
  luzBase = c3Luz < 4 ? c3Luz : 0;               // indicadores da tela = o que o C3 esta fazendo
  piscaAtivo = c3Pisca;
  FREIO_AUTO = c3FreioAuto != 0;
  tx433Teste = c3Teste != 0;
  if ((bool)c3Freio != freioAtivo) {             // freio decidido no C3; a mochila continua aqui
    freioAtivo = c3Freio;
    Serial.printf("[freio] %s (pelo C3)\n", freioAtivo ? "ACENDE" : "apaga");
    freioAplicar(freioAtivo);
  }
  static uint8_t seqVisto = 0;
  static bool primeiro = true;
  if (primeiro) { seqVisto = c3EvSeq; primeiro = false; }
  if (c3EvSeq != seqVisto) {                     // apertaram um botao do controle (no C3)
    seqVisto = c3EvSeq;
    if (c3EvBtn == 0xAA || c3EvBtn == 0x33) hornFlashUntil = millis() + 600;
    logEvent("controle (pelo C3): botao %02X", c3EvBtn);
  }
}

static void freioLoop() {
  if (PONTE_C3) return;                          // com o C3, quem decide e ele (ver c3Loop)
  static uint32_t ultAmostra = 0, ligouEm = 0;
  static int calmas = 0;
  bool novo = freioAtivo;
  if (millis() < freioTesteAte) novo = true;                       // comando 'freio' (teste)
  else if (!FREIO_AUTO || msSemDados() > 2000) novo = false;       // desligado / sem dados
  else if (wd.lastUpdate != ultAmostra) {                          // leitura nova da roda
    ultAmostra = wd.lastUpdate;
    bool freando = wd.current < -FREIO_LIGA_A && wd.speed > FREIO_VEL_MIN;
    bool solto = wd.current > -FREIO_DESLIGA_A || wd.speed < FREIO_VEL_MIN;
    if (freando) { novo = true; calmas = 0; }
    else if (solto) calmas++;
    else calmas = 0;
    if (freioAtivo && !freando && calmas >= 2 && millis() - ligouEm > FREIO_MIN_MS) novo = false;
  }
  // (fim do teste: sem roda apaga na hora pelo 2o ramo; com a roda parada, em ~1 s pelo 3o)
  if (novo == freioAtivo) return;
  freioAtivo = novo;
  if (novo) ligouEm = millis(); else freioTesteAte = 0;
  Serial.printf("[freio] %s (%.1f A, %.0f km/h)\n", novo ? "ACENDE" : "apaga", wd.current, wd.speed);
  freioAplicar(novo);
}

// Botoes fisicos IO2/IO3 (independem da roda; funcionam sempre)
// Antirrepique "reage na hora, rearma so depois de solto de verdade": o 1o contato
// ja liga o pisca; outro aperto so vale depois de 50 ms SEGUIDOS sem contato.
// (o antigo aceitava qualquer mudanca apos 30 ms, e um contato que "treme" mais
//  que isso - fio encostado, botao ruim - virava dois cliques: liga e desliga)
static void handlePiscaBotoes() {
  static bool armado[2] = { true, true };        // pode aceitar um novo aperto?
  static uint32_t tContato[2] = { 0, 0 };        // ultimo instante em que encostou
  const int pinos[2] = { PIN_PISCA_ESQ, PIN_PISCA_DIR };
  for (int k = 0; k < 2; k++) {
    bool ap = digitalRead(pinos[k]) == LOW;
    if (ap) {
      tContato[k] = millis();                    // qualquer encostada reinicia a contagem
      if (armado[k]) { armado[k] = false; piscaToggle(k + 1); }   // reage NA HORA
    } else if (!armado[k] && millis() - tContato[k] > 50) {
      armado[k] = true;                          // 50 ms solto de verdade: rearma
    }
  }
}

// Envia bits crus pelo 433 (ate 40 bits = 10 digitos hexa), no formato do controle
// da BUZINA: bit 1 = longo+curto, bit 0 = curto+longo; entre as repeticoes, pulso
// curto + ~1,2 ms. Serve para o teste de parear a buzina com uma identidade nossa:
//   433 raw 009F1DAA   (buzina, identidade 00 9F 1D em vez da original 00 9F 1C)
// TEMPOS: os do controle da buzina, medidos na captura (curto ~352, longo ~920 us,
// proporcao ~2,6). Os dos piscas (330/990, proporcao 3) NAO serviram para ela.
// Da para testar outros sem regravar:  433 raw 009F1CAA 350 920  (curto longo, em us)
// (BUZ_CURTO/BUZ_LONGO/BUZ_INTERVALO estao la em cima: o console usa)
static void tx433Raw(const char* hex, uint16_t curto, uint16_t longo) {
  if (curto < 100 || curto > 2000 || longo < 100 || longo > 5000) { Serial.println("433 raw: tempos fora da faixa"); return; }
  int nd = strlen(hex);
  if (nd < 1 || nd > 10) { Serial.println("433 raw: use de 1 a 10 digitos hexa (ex.: 433 raw 009F1DAA)"); return; }
  if (!tx433Ok) { Serial.println("433: sem RMT"); return; }
  if (millis() < tx433LivreEm) { Serial.println("433: radio ocupado, tente de novo"); return; }
  const int nbits = nd * 4;
  int k = 0;
  for (int r = 0; r < TX433_REPET; r++) {
    for (int d = 0; d < nd; d++) {
      char c = hex[d];
      int val = (c >= '0' && c <= '9') ? c - '0'
              : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
      if (val < 0) { Serial.printf("433 raw: '%c' nao e hexa\n", c); return; }
      for (int i = 3; i >= 0; i--) {
        bool um = (val >> i) & 1;
        tx433Simb[k].level0 = 1; tx433Simb[k].duration0 = um ? longo : curto;
        tx433Simb[k].level1 = 0; tx433Simb[k].duration1 = um ? curto : longo;
        k++;
      }
    }
    tx433Simb[k].level0 = 1; tx433Simb[k].duration0 = curto;          // pulso final + intervalo
    tx433Simb[k].level1 = 0; tx433Simb[k].duration1 = BUZ_INTERVALO;
    k++;
  }
  rmtWriteAsync(TX433_PIN, tx433Simb, k);       // k <= 12 x 41: cabe no mesmo buffer dos piscas
  uint32_t quadroUs = (uint32_t)nbits * (curto + longo) + curto + BUZ_INTERVALO;
  tx433LivreEm = millis() + quadroUs * TX433_REPET / 1000 + 60;
  Serial.printf("[433] raw %s (%d bits, curto %u / longo %u us) x%d\n", hex, nbits, curto, longo, TX433_REPET);
}

// Manda o comando agendado da buzina externa assim que o radio liberar
static void buzExtLoop() {
  if (!buzExtPend || !tx433Ok || millis() < tx433LivreEm) return;
  buzExtPend = false;
  char hx[12];
  snprintf(hx, sizeof(hx), "%06lX%02X", (unsigned long)BUZ_EXT_ID, buzExtCmd);
  tx433Raw(hx, BUZ_CURTO, BUZ_LONGO);
}

// Teste de alcance: uma rajada (12 repeticoes, ~0,5 s) a cada ~0,8 s, numerada,
// sempre DEPOIS dos comandos de verdade (piscas e buzina externa tem prioridade).
static void tx433TesteLoop() {
  static uint32_t ultimo = 0;
  if (!tx433Teste || !tx433Ok || tx433Pend || buzExtPend) return;
  if (millis() < tx433LivreEm || millis() - ultimo < 800) return;
  ultimo = millis();
  char hx[12];
  snprintf(hx, sizeof(hx), "%06lX%02X", (unsigned long)TESTE433_ID, tx433TesteSeq++);
  tx433Raw(hx, BUZ_CURTO, BUZ_LONGO);
}

// ---- Controle da buzina como entrada do display ----
// A buzina externa foi pareada com a identidade do DISPLAY (00 9F 1D) e nao obedece mais
// o controle (00 9F 1C): o controle agora so fala com o display, que decide o que fazer.
//   botao buzina (AA)   -> buzina da RODA (Bluetooth: age na hora)
//   volume + (99)       -> pisca ESQUERDA  (433: age quando soltar o botao)
//   volume - (CC)       -> pisca DIREITA   (433: idem)
//   cadeado aberto (33) -> buzina EXTERNA, o display manda 00 9F 1D AA (433: idem)
//   cadeado fechado (55)-> DESLIGA o alarme da buzina externa (00 9F 1D 33) (433: idem)
//   segurar vol+ (66) / vol- (FF) -> livres por enquanto
// Por que "quando soltar": enquanto o dedo esta no botao o controle continua
// transmitindo; se o display transmitir junto, os dois sinais se embaralham no ar
// e a buzina externa/os piscas nao entendem (era o "funciona as vezes").
static bool rx433UsaRadio(uint8_t cmd) { return cmd == 0x99 || cmd == 0xCC || cmd == 0x33 || cmd == 0x55; }

static void rx433Acao(uint32_t cod) {
  uint8_t cmd = cod & 0xFF;
  switch (cmd) {
    case 0xAA:
      logEvent("controle: buzina da roda");
      sendKs(0x88, 0, 0, 0, 0);
      hornFlashUntil = millis() + 600;
      break;
    case 0x99: logEvent("controle: pisca esquerda"); piscaToggle(1); break;
    case 0xCC: logEvent("controle: pisca direita");  piscaToggle(2); break;
    case 0x33:
      logEvent("controle: buzina externa");
      buzExtEnviar(0xAA);
      hornFlashUntil = millis() + 600;
      break;
    case 0x55:
      logEvent("controle: desliga alarme da buzina externa");
      buzExtEnviar(0x33);
      break;
    default:
      Serial.printf("[433 rx] botao %02X sem funcao\n", cmd);
      break;
  }
}

// Um aperto manda o mesmo quadro varias vezes seguidas (~40 ms cada). So age depois de
// 2 quadros iguais (contra ruido) e UMA vez por aperto: outro aperto do mesmo botao so
// conta depois de 300 ms sem quadros dele (soltou).
// Acoes que usam o radio 433 ficam guardadas ate o controle PARAR de transmitir
// (150 ms sem quadros = soltou o botao) - ver rx433UsaRadio.
// 80 ms: entre um quadro e outro o controle pausa so ~1,2 ms, entao 80 ms de silencio
// (~2 quadros) ja e "soltou" com folga. (era 150: um pouco de demora a mais.)
static const uint32_t RX_SOLTOU_MS = 80;
static void rx433Loop() {
  static uint32_t visto = 0, ultimo = 0, tUltimo = 0, tPrimeiro = 0, pendente = 0;
  static int repet = 0;
  static bool agiu = false;
  if (pendente && millis() - tUltimo > RX_SOLTOU_MS) {   // controle calou: agora o ar esta livre
    uint32_t p = pendente; pendente = 0;
    // quanto o controle transmitiu neste aperto (+ a espera): se for muito, a demora e dele
    Serial.printf("[433 rx] controle transmitiu %lu ms; respondendo %lu ms apos o 1o quadro\n",
                  (unsigned long)(tUltimo - tPrimeiro), (unsigned long)(millis() - tPrimeiro));
    rx433Acao(p);
  }
  uint32_t c = rxContador;
  if (c == visto) return;
  visto = c;
  uint32_t cod = rxCodigo;
  if ((cod >> 8) != CONTROLE_ID) return;       // outro aparelho (piscas, o proprio display...)
  if (cod == ultimo && millis() - tUltimo < 300) repet++;
  else { ultimo = cod; repet = 1; agiu = false; tPrimeiro = millis(); }
  tUltimo = millis();
  if (!agiu && repet >= 2) {
    agiu = true;
    if (rx433UsaRadio(cod & 0xFF)) pendente = cod;   // espera soltar
    else rx433Acao(cod);                             // Bluetooth: na hora
  }
}

// ================= SOM: piezo/alto-falante pelo codec ES8311 =================
// O som vai por DOIS caminhos: config do codec por I2C (porta 0, no loop/setup,
// como tudo que e I2C) e as amostras de audio por I2S, numa TAREFA separada
// (que so mexe no I2S, nunca no I2C, entao nao briga com o touch/MPU).
// (os pinos/estado do som estao la em cima, antes do handleSerialConsole que
//  usa somOk/somBipe; aqui ficam so as funcoes)
static bool somCodecEscr(uint8_t reg, uint8_t v) {
  return lgfx::i2c::writeRegister8(0, 0x18, reg, v, 0, 400000).has_value();
}
static uint8_t somCodecLer(uint8_t reg) {
  auto r = lgfx::i2c::readRegister8(0, 0x18, reg, 400000);
  return r.has_value() ? r.value() : 0;
}
// Configura o ES8311 (escravo, MCLK=256*32k=8,192 MHz vindo do pino, I2S 16 bits).
// Mesma sequencia do driver oficial da Espressif, adaptada no teste_piezo.
static bool somCodecIniciar() {
  uint8_t id = somCodecLer(0xFD);            // 0x83 = ES8311
  if (!somCodecEscr(0x00, 0x1F)) return false;
  delay(20);
  somCodecEscr(0x00, 0x00); somCodecEscr(0x00, 0x80);
  somCodecEscr(0x01, 0x3F);
  somCodecEscr(0x02, somCodecLer(0x02) & 0x07);
  somCodecEscr(0x03, 0x10); somCodecEscr(0x04, 0x10); somCodecEscr(0x05, 0x00);
  somCodecEscr(0x06, (somCodecLer(0x06) & 0xC0) | 0x03);
  somCodecEscr(0x07, somCodecLer(0x07) & 0xC0); somCodecEscr(0x08, 0xFF);
  somCodecEscr(0x00, somCodecLer(0x00) & 0xBF);
  somCodecEscr(0x09, 0x0C); somCodecEscr(0x0A, 0x0C);
  somCodecEscr(0x0D, 0x01); somCodecEscr(0x0E, 0x02); somCodecEscr(0x12, 0x00);
  somCodecEscr(0x13, 0x10); somCodecEscr(0x1C, 0x6A); somCodecEscr(0x37, 0x08);
  somCodecEscr(0x31, somCodecLer(0x31) & ~0x60);    // tira o mudo
  somCodecEscr(0x32, 0xCF);                         // volume ~0 dB
  Serial.printf("som: ES8311 id 0x%02X (esperado 0x83)\n", id);
  return true;
}

// Para um instante t (s) do modo, diz a frequencia e se o som esta ligado.
// Devolve false quando acabou.
static bool somPrograma(int modo, float t, float& f, bool& on) {
  f = SOM_FREQ;
  if (modo == SOM_BIPE) {                    // bi-bip curto
    if (t >= 0.40f) return false;
    on = (t < 0.12f) || (t >= 0.20f && t < 0.32f);
    return true;
  }
  if (modo == SOM_ALARME) {                  // sirene subindo, 3 vezes
    if (t >= 1.8f) return false;
    float frac = fmodf(t, 0.6f) / 0.6f;
    f = SOM_FREQ * (0.7f + 0.5f * frac); on = true;
    return true;
  }
  return false;
}

static void somTarefa(void*) {
  const int N = 256;
  static int16_t buf[N * 2];
  float fase = 0; uint32_t n = 0; int pedidoVisto = -1; bool ampLigado = false;
  for (;;) {
    int modo = somModo;
    if (modo == SOM_NADA) {                  // ocioso: desliga o amplificador e descansa
      if (ampLigado) { digitalWrite(SOM_AMP_EN, HIGH); ampLigado = false; }
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (!ampLigado) { digitalWrite(SOM_AMP_EN, LOW); ampLigado = true; }
    if (somPedido != pedidoVisto) { pedidoVisto = somPedido; n = 0; }
    for (int i = 0; i < N; i++) {
      float f = SOM_FREQ; bool on = false;
      if (!somPrograma(modo, n / (float)SOM_TAXA, f, on)) { somModo = SOM_NADA; on = false; }
      int16_t v = 0;
      if (on) { fase += f / SOM_TAXA; if (fase >= 1) fase -= 1; v = (fase < 0.5f) ? 22000 : -22000; }
      buf[2 * i] = buf[2 * i + 1] = v;
      n++;
    }
    somI2s.write((uint8_t*)buf, sizeof(buf));   // espera espaco no DMA: isso da o ritmo
  }
}

static void somTocar(int modo) { if (somOk && SOM_ON) { somModo = modo; somPedido++; } }
static void somBipe()   { somTocar(SOM_BIPE); }
static void somAlarme() { somTocar(SOM_ALARME); }

static void somSetup() {
#if !USAR_SOM
  Serial.println("som: DESLIGADO (USAR_SOM=0)");
  return;                      // nao inicia I2S, codec nem a tarefa
#endif
  pinMode(SOM_AMP_EN, OUTPUT);
  digitalWrite(SOM_AMP_EN, HIGH);            // amplificador desligado ate tocar algo
  somI2s.setPins(SOM_BCLK, SOM_LRCK, SOM_DOUT, SOM_DIN, SOM_MCLK);
  if (!somI2s.begin(I2S_MODE_STD, SOM_TAXA, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("som: ERRO ao iniciar o I2S"); return;
  }
  somOk = somCodecIniciar();
  if (somOk) xTaskCreatePinnedToCore(somTarefa, "som", 4096, nullptr, 4, nullptr, 1);
  Serial.printf("som: %s\n", somOk ? "ok" : "NAO RESPONDE");
}

// ================= MPU-6050: acender a tela pela posicao do pulso =================
// (as variaveis estao la em cima, antes do handleTouch que as usa; aqui ficam
//  so as funcoes, para nao cair no problema dos prototipos do Arduino)
static bool mpuEscrever(uint8_t reg, uint8_t v) {
  return lgfx::i2c::writeRegister8(0, MPU_END, reg, v, 0, 400000).has_value();
}
static bool mpuLerAccel() {
  uint8_t b[6];
  if (!lgfx::i2c::readRegister(0, MPU_END, 0x3B, b, 6, 400000).has_value()) return false;
  float ax = (int16_t)((b[0] << 8) | b[1]) / MPU_LSB_ACEL;
  float ay = (int16_t)((b[2] << 8) | b[3]) / MPU_LSB_ACEL;
  float az = (int16_t)((b[4] << 8) | b[5]) / MPU_LSB_ACEL;
  mpuFx += 0.1f * (ax - mpuFx); mpuFy += 0.1f * (ay - mpuFy); mpuFz += 0.1f * (az - mpuFz);
  return true;
}
// "Modo vigia" para o sono: o MPU fica sozinho medindo a 5 Hz (giroscopio parado,
// gasta quase nada) e levanta o INT quando detecta movimento -> acorda o ESP32.
// (mesma sequencia do teste_tela_mpu; o proximo boot reseta o MPU no mpuSetup)
static void mpuModoVigia() {
  mpuEscrever(0x1C, 0x09);          // +-4 g + filtro passa-alta 5 Hz (so ve MUDANCA)
  mpuEscrever(0x1F, MPU_MOT_THR);   // MOT_THR
  mpuEscrever(0x20, 1);             // MOT_DUR
  mpuEscrever(0x37, 0x20);          // INT ativo em 1, travado ate ler
  mpuEscrever(0x38, 0x40);          // so a interrupcao de movimento
  delay(50);
  uint8_t st; lgfx::i2c::readRegister(0, MPU_END, 0x3A, &st, 1, 400000);   // limpa INT pendente
  mpuEscrever(0x6C, 0x47);          // acorda a 5 Hz, giroscopio em espera
  mpuEscrever(0x6B, 0x28);          // modo ciclo
}

static void mpuSalvarRef() {
  float m = sqrtf(mpuFx * mpuFx + mpuFy * mpuFy + mpuFz * mpuFz);
  if (m < 0.5f) { Serial.println("mpu: leitura ruim, tente de novo parado"); return; }
  mpuRx = mpuFx / m; mpuRy = mpuFy / m; mpuRz = mpuFz / m; mpuTemRef = true;
  prefs.begin("s16cfg", false);
  prefs.putBool("mpu_tem", true);
  prefs.putFloat("mpu_rx", mpuRx); prefs.putFloat("mpu_ry", mpuRy); prefs.putFloat("mpu_rz", mpuRz);
  prefs.end();
  Serial.printf("mpu: posicao gravada (%.2f %.2f %.2f)\n", mpuRx, mpuRy, mpuRz);
}
static float mpuAnguloAteRef() {
  float m = sqrtf(mpuFx * mpuFx + mpuFy * mpuFy + mpuFz * mpuFz);
  if (m < 0.1f) return 180;
  float c = (mpuFx * mpuRx + mpuFy * mpuRy + mpuFz * mpuRz) / m;
  if (c > 1) c = 1; else if (c < -1) c = -1;
  return acosf(c) * 57.2958f;
}
static void mpuSetup() {
  prefs.begin("s16cfg", true);
  mpuTemRef = prefs.getBool("mpu_tem", false);
  mpuRx = prefs.getFloat("mpu_rx", 0); mpuRy = prefs.getFloat("mpu_ry", 0); mpuRz = prefs.getFloat("mpu_rz", 1);
  mpuAngLiga  = prefs.getFloat("mpu_liga", MPU_ANG_LIGA);
  mpuAngApaga = prefs.getFloat("mpu_apaga", MPU_ANG_APAGA);
  prefs.end();
  mpuEscrever(0x6B, 0x80); delay(100);            // reset
  mpuOk = mpuEscrever(0x6B, 0x01);                // acorda
  if (mpuOk) {
    mpuEscrever(0x1C, 0x08);                      // acel +-4 g
    for (int i = 0; i < 30; i++) { mpuLerAccel(); delay(5); }   // enche o filtro
  }
  Serial.printf("mpu: %s%s\n", mpuOk ? "ok" : "NAO RESPONDE",
                mpuTemRef ? ", posicao gravada" : ", sem posicao (use 'mpu gravar')");
  mpuUltimoToque = millis();
}

// Chamada a cada volta do loop; cuida de ler o sensor e acender/apagar a tela.
static void mpuAtualizar() {
  static uint32_t tLer = 0, desdeNa = 0, desdeFora = 0;
  uint32_t agora = millis();
  if (!mpuOk || agora - tLer < 20) return;        // ~50 leituras/s bastam para a tela
  tLer = agora;
  mpuLerAccel();
  if (mpuTemRef) mpuAngulo = mpuAnguloAteRef();   // sempre (a tela do MPU mostra ao vivo)
  if (!TELA_AUTO || !mpuTemRef) { if (!telaAcesa) { telaAcesa = true; tft.setBrightness(MPU_BRILHO); } return; }

  if (mpuAngulo < mpuAngLiga)  { if (!desdeNa)   desdeNa = agora; }   else desdeNa = 0;
  if (mpuAngulo > mpuAngApaga) { if (!desdeFora) desdeFora = agora; } else desdeFora = 0;

  if (!telaAcesa && desdeNa && agora - desdeNa >= MPU_T_LIGAR) {
    telaAcesa = true; tft.setBrightness(MPU_BRILHO);
  }
  // com os Ajustes ou a tela do MPU abertos ela nao apaga (da pra testar olhando)
  if (telaAcesa && modoTela == TELA_NORMAL && desdeFora && agora - desdeFora >= MPU_T_APAGAR &&
      agora - mpuUltimoToque >= MPU_T_TOQUE) {
    telaAcesa = false; tft.setBrightness(0);
  }
}

// ================= BATERIA do display (IO9, divisor 2:1) =================
// Curva aproximada de uma celula LiPo/18650 em repouso (tensao -> %).
// Com carga a tensao cai um pouco, entao o % e uma estimativa.
static int batPorcento(float v) {
  static const float TAB[][2] = { {4.20f, 100}, {4.10f, 90}, {4.00f, 80}, {3.92f, 70}, {3.86f, 60},
                                {3.82f, 50},  {3.79f, 40}, {3.76f, 30}, {3.73f, 20}, {3.69f, 10},
                                {3.60f, 5},   {3.30f, 0} };
  const int N = sizeof(TAB) / sizeof(TAB[0]);
  if (v >= TAB[0][0]) return 100;
  for (int i = 1; i < N; i++)
    if (v >= TAB[i][0]) {                          // entre T[i] e T[i-1]: interpola
      float f = (v - TAB[i][0]) / (TAB[i - 1][0] - TAB[i][0]);
      return (int)(TAB[i][1] + f * (TAB[i - 1][1] - TAB[i][1]) + 0.5f);
    }
  return 0;
}

// USB ligado num computador? O USB nativo do ESP32-S3 recebe um "sinal de vida" do PC
// a cada 1 ms. Carregador de parede/powerbank NAO manda isso (so energia).
static bool usbPcLigado() {
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
  return Serial.isPlugged();
#else
  return false;
#endif
}

// "Degrau" de tensao que conta como ligar/desligar o carregador (ver batLoop).
// Se o icone de carregando errar, e este numero que se ajusta.
static const float BAT_DEGRAU = 0.06f;
// Carregando, a tensao nao cai: se cair isso abaixo do maximo visto desde que
// "comecou a carregar", foi alarme falso (ou o carregador terminou) -> desliga o raio.
static const float BAT_CAIU = 0.04f;

// A cada 0,5 s: le, filtra e calcula o %. Abaixo de 2,5 V = "sem bateria" (fio solto /
// so no USB). Bateria baixa (< 3,35 V): alarme uma vez e evento no log.
// CARREGANDO: a placa nao tem um pino que avise, entao usamos dois sinais:
//  1) USB num computador (usbPcLigado) -> com certeza tem 5 V -> carregando.
//  2) "degrau" na tensao: ao ligar o carregador a placa passa a rodar pelo USB e a
//     bateria so recebe carga -> a tensao SOBE de repente; ao tirar, CAI de repente.
//     Compara a media das 2 leituras mais novas com a das 2 de ~4 s atras.
//  PEGADINHA (13/10): bipe/alarme e transmissao 433 puxam corrente e a tensao AFUNDA;
//  quando acabam, ela VOLTA de repente - parece o degrau do carregador e o raio ficava
//  preso. Por isso: (a) degraus durante som/433 e ate 4 s depois sao ignorados;
//  (b) com o raio aceso (sem PC), se a tensao cair BAT_CAIU abaixo do maximo visto, apaga.
static void batLoop() {
  static uint32_t t = 0, tRuido = 0;
  static float hist[8];                    // leituras cruas dos ultimos ~4 s
  static int hi = 0;
  static float maxCarga = 0;               // maior tensao vista desde que "comecou a carregar"
  if (somModo != SOM_NADA || millis() < tx433LivreEm) tRuido = millis();   // carga extra agora
  if (millis() - t < 500) return;
  t = millis();
  float v = analogReadMilliVolts(BAT_PIN) * 2 / 1000.0f * BAT_CAL;
  if (batV == 0) for (int i = 0; i < 8; i++) hist[i] = v;   // 1a leitura: sem degrau falso
  hist[hi] = v; hi = (hi + 1) % 8;
  batV = (batV == 0) ? v : batV + 0.2f * (v - batV);
  batPct = (batV < 2.5f) ? -1 : batPorcento(batV);

  float novas = (hist[(hi + 7) % 8] + hist[(hi + 6) % 8]) / 2;   // as 2 mais recentes
  float velhas = (hist[hi] + hist[(hi + 1) % 8]) / 2;             // as 2 mais antigas
  bool pc = usbPcLigado();
  bool antes = batCarregando;
  bool quieto = millis() - tRuido > 4000;  // sem som/433 ha 4 s: degraus valem
  if (batPct >= 0 && quieto) {
    if (novas - velhas > BAT_DEGRAU) batCarregando = true;
    else if (velhas - novas > BAT_DEGRAU) batCarregando = false;
  }
  if (batCarregando && !pc) {              // autocorrecao: carregando, a tensao nao cai
    if (batV > maxCarga) maxCarga = batV;  // (filtrada: a crua oscila +-15 mV)
    if (maxCarga - batV > BAT_CAIU) batCarregando = false;
  }
  if (pc) batCarregando = true;
  else if (usbPC) batCarregando = false;  // acabou de sair do computador
  usbPC = pc;
  if (batCarregando != antes) {
    for (int i = 0; i < 8; i++) hist[i] = v;   // zera a comparacao (nao dispara de novo)
    maxCarga = batV;
    logEvent("bateria: %s (%.2f V%s)", batCarregando ? "carregando" : "na bateria", v, pc ? ", USB do PC" : "");
  }

  if (batPct >= 0 && !batCarregando && batV < 3.35f && !batAvisouBaixa) {
    batAvisouBaixa = true;
    somAlarme();
    logEvent("bateria do display baixa: %.2f V", batV);
  }
  if (batV > 3.50f) batAvisouBaixa = false;      // carregou: pode avisar de novo
}

// ================= DORMIR (deep sleep) =================
// Fecha a gravacao, grava os eventos, apaga as lanternas (433), mostra o aviso e dorme.
// Pinos que precisam ficar FIXOS durante o sono: luz de fundo (IO45) em 0 e o
// amplificador (IO1) desligado (1) - solto, ele poderia ligar e gastar bateria.
// podeMexer: se ACORDA_MEXENDO estiver ligado, o movimento (INT do MPU) tambem acorda.
// Na bateria no fim e false: acordar mexendo ali so gastaria o resto da celula.
static void dormir(const char* motivo, bool podeMexer) {
  bool mexer = podeMexer && ACORDA_MEXENDO && mpuOk;
  Serial.printf("[sono] dormindo: %s (acorda no BOOT%s)\n", motivo, mexer ? " ou mexendo" : "");
  logEvent("dormindo: %s", motivo);
  stopRideLog();
  writeEvents();                                    // grava os eventos pendentes no cartao
  // Dormir apaga tudo; acordar devolve tudo como estava (piscas no tx433Setup, mochila ao reconectar)
  dreamSalvarSeMudou(true);                         // guarda o estado da mochila antes
  if (dreamConectado && dreamLigado) dreamEnviarLiga(false);   // apaga a mochila (sem mexer no estado salvo)
  if (tx433Ok && (piscaAtivo || luzBase != LUZ_OFF)) {   // apaga as lanternas (as outras pilhas agradecem)
    while (millis() < tx433LivreEm) delay(10);
    tx433Enviar(CMD_LUZ_OFF); tx433Loop();
    while (millis() < tx433LivreEm) delay(10);
  }
  telaAcesa = true; tft.setBrightness(MPU_BRILHO);
  spr.fillScreen(TFT_BLACK);
  spr.setTextDatum(middle_center);
  spr.setFont(&fonts::FreeSansBold12pt7b); spr.setTextColor(TFT_WHITE);
  spr.drawString("Dormindo", 120, 130);
  spr.setFont(&fonts::Font2); spr.setTextColor(COL_DIM);
  spr.drawString(motivo, 120, 165);
  spr.setTextColor(TFT_YELLOW);
  spr.drawString(mexer ? "aperte BOOT ou mexa o braco" : "aperte BOOT para ligar", 120, 195);
  spr.pushSprite(0, 0);
  delay(2500);

  lgfx::i2c::writeRegister8(0, 0x38, 0xA5, 0x03, 0, 400000);   // touch em hibernacao
  if (mexer) mpuModoVigia();                                   // MPU vigiando movimento
  else if (mpuOk) mpuEscrever(0x6B, 0x40);                     // MPU dorme de vez
  tft.sleep();
  digitalWrite(SOM_AMP_EN, HIGH);                              // amplificador desligado...
  gpio_hold_en((gpio_num_t)SOM_AMP_EN);                        // ...e preso assim
  gpio_reset_pin(GPIO_NUM_45);                                 // luz de fundo presa em 0
  gpio_set_direction(GPIO_NUM_45, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_45, 0);
  gpio_hold_en(GPIO_NUM_45);
  gpio_deep_sleep_hold_en();
  rtc_gpio_pullup_en(GPIO_NUM_0);                              // BOOT acorda
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
  if (mexer) {                                                 // INT do MPU (sobe ao mexer) acorda
    rtc_gpio_pulldown_en((gpio_num_t)MPU_PINO_INT);
    rtc_gpio_pullup_dis((gpio_num_t)MPU_PINO_INT);
    esp_sleep_enable_ext1_wakeup(1ULL << MPU_PINO_INT, ESP_EXT1_WAKEUP_ANY_HIGH);
  }
  Serial.flush();
  esp_deep_sleep_start();
}

// Decide se e hora de dormir. Fica acordado enquanto: roda conectada, USB/carregador
// ligado (ou sem bateria = so no USB) ou alguem tocou na tela nos ultimos 10 min.
static void sonoLoop() {
  static uint32_t ativoEm = 0, baixaDesde = 0;
  uint32_t agora = millis();
  bool roda = client && client->isConnected();
  bool tomada = usbPC || batCarregando || batPct < 0;
  if (roda || tomada || tx433Teste || ativoEm == 0) ativoEm = agora;   // teste 433 rodando: nao dorme
  if (mpuUltimoToque > ativoEm) ativoEm = mpuUltimoToque;     // tocou na tela
  if (agora - ativoEm >= DORMIR_SEM_RODA_MS) dormir("10 min sem roda e sem USB", true);
  if (batPct >= 0 && !batCarregando && !usbPC && batV < BAT_CORTE_V) { if (!baixaDesde) baixaDesde = agora; }
  else baixaDesde = 0;
  if (baixaDesde && agora - baixaDesde > 30000) dormir("bateria no fim", false);
}

// ================= PRINT da tela -> BMP no cartao (/prints/print_NNN.bmp) =================
// Copia o sprite (o quadro inteiro que vai pra tela) pixel a pixel, em BMP de 24 bits
// (abre em qualquer visualizador). Leva ~1 s; a ponte BLE segue pela tarefa dela.
static void salvarPrint() {
  if (!sdOk) { Serial.println("[print] sem cartao SD"); return; }
  SD_MMC.mkdir("/prints");
  char nome[40];
  for (int i = 1; i < 1000; i++) {
    snprintf(nome, sizeof(nome), "/prints/print_%03d.bmp", i);
    if (!SD_MMC.exists(nome)) break;
  }
  File f = SD_MMC.open(nome, FILE_WRITE);
  if (!f) { Serial.printf("[print] erro ao criar %s\n", nome); return; }
  const int W = 240, H = 320;
  const uint32_t dados = (uint32_t)W * 3 * H;   // 720 bytes por linha (ja e multiplo de 4)
  uint8_t cab[54] = { 0 };
  auto p32 = [&](int i, uint32_t v) { cab[i] = v; cab[i + 1] = v >> 8; cab[i + 2] = v >> 16; cab[i + 3] = v >> 24; };
  cab[0] = 'B'; cab[1] = 'M';
  p32(2, 54 + dados);  // tamanho do arquivo
  p32(10, 54);         // onde comecam os pixels
  p32(14, 40);         // tamanho do cabecalho de informacao
  p32(18, W); p32(22, H);
  cab[26] = 1;         // planos
  cab[28] = 24;        // bits por pixel
  p32(34, dados);
  f.write(cab, sizeof(cab));
  static uint8_t lin[240 * 3];
  for (int y = H - 1; y >= 0; y--) {             // o BMP guarda as linhas de baixo pra cima
    for (int x = 0; x < W; x++) {
      uint16_t c = spr.readPixel(x, y);          // RGB565
      uint8_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
      lin[x * 3 + 0] = (b << 3) | (b >> 2);      // ... e cada pixel como B, G, R
      lin[x * 3 + 1] = (g << 2) | (g >> 4);
      lin[x * 3 + 2] = (r << 3) | (r >> 2);
    }
    f.write(lin, sizeof(lin));
  }
  f.close();
  Serial.printf("[print] salvo: %s\n", nome);
  logEvent("print: %s", nome);
  somBipe();
}

// Dispara o print agendado (botao Print dos Ajustes = 3 s; 'print' no Serial = ja)
static void printLoop() {
  if (!printEm || millis() < printEm) return;
  printEm = 0;
  bool wheelOk = client && client->isConnected();
  if (telaAcesa && (wheelOk || modoTela != TELA_NORMAL)) drawPage();   // quadro atualizado
  salvarPrint();
}

// ================= Setup / Loop =================
void setup() {
  // voltando do sono: solta os pinos que ficaram presos (luz de fundo e amplificador)
  gpio_hold_dis(GPIO_NUM_45);
  gpio_hold_dis((gpio_num_t)SOM_AMP_EN);
  gpio_deep_sleep_hold_dis();
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n=== S16 Companion ponte %s ===\n", VERSAO);
  {
    esp_sleep_wakeup_cause_t c = esp_sleep_get_wakeup_cause();
    Serial.printf("ligou por: %s\n", c == ESP_SLEEP_WAKEUP_EXT0 ? "botao BOOT (acordou do sono)"
                                   : c == ESP_SLEEP_WAKEUP_EXT1 ? "movimento (INT do MPU, acordou do sono)"
                                   : "energia/reset");
  }
  carregarConfig();
  dreamCarregar();             // estado das luzes da mochila (volta como estava ao reconectar)
  tft.init();
  tft.setRotation(TELA_GIRADA ? 2 : 0);   // retrato 240x320; 2 = girada 180 (TELA_GIRADA). O touch gira junto (LovyanGFX)
  tft.setBrightness(200);
  mpuSetup();                  // depois do tft.init(): a porta I2C 0 ja esta aberta
  somSetup();                  // codec ES8311 (I2C) + I2S + tarefa de som
  tx433Setup();                // transmissor 433 MHz (RMT no IO21)
  dreamQuer = DREAM_AUTO;      // manter a mochila conectada automaticamente
  spr.setPsram(true);
  spr.setColorDepth(16);
  if (!spr.createSprite(240, 320)) Serial.println("ERRO: sem memoria para o sprite (PSRAM ligada?)");
  Serial.printf("PSRAM: %lu bytes\n", (unsigned long)ESP.getPsramSize());
  statusMsg("KS-S16 ponte v5", "Escaneando...");

  pinMode(PIN_BUZINA, INPUT_PULLUP);
  logQ = xQueueCreate(16, sizeof(LogEvt));

  // Cartao SD
  SD_MMC.setPins(38, 40, 39);
  if (SD_MMC.begin("/sdcard", true)) {
    sdOk = true;
    SD_MMC.mkdir("/logs");
    Serial.printf("Cartao SD: %llu MB\n", SD_MMC.cardSize() / (1024 * 1024));
    logEvent("display ligado");
  } else {
    Serial.println("Sem cartao SD (logs desligados)");
  }
  writeQ = xQueueCreate(16, sizeof(PendingWrite));

  NimBLEDevice::init("");
  NimBLEDevice::setMTU(247);
  {
    // Endereco do "S16 Controle": o MAC Bluetooth do chip, com os 2 bits mais altos ligados
    // (e o que caracteriza um endereco "random static"). Fixo: o celular reconhece sempre.
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    for (int k = 0; k < 6; k++) hidAddrVal[k] = mac[5 - k];   // ordem invertida (little-endian)
    hidAddrVal[0] ^= 0x01;                                    // diferente do endereco da roda
    hidAddrVal[5] |= 0xC0;
  }
  {
    // Pareamento "so confirmar" (sem PIN), com bonding (o celular lembra do display)
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    // ATENCAO ao nome enganoso da NimBLE:
    //   setSecurityInitKey -> sm_our_key_dist    = chaves que O DISPLAY distribui
    //   setSecurityRespKey -> sm_their_key_dist  = chaves que O CELULAR distribui
    //
    // Historia (ver README): pedir a IRK do celular com a privacidade NO CONTROLADOR
    // quebrava os apps na identidade da roda; o jeito antigo (0) nao pedia e o
    // controle repareava a cada reinicio. A V2 pede a IRK e resolve NO HOST.
#if PAREAMENTO_V2
    // V2: NAO mandamos a nossa identidade (o "S16 Controle" ja usa um endereco
    // fixo proprio, random static, que serve de identidade) e PEDIMOS a do
    // celular. A IRK dele vai para a lista de resolucao do HOST: o controlador
    // continua sem resolver nada, entao as conexoes dos apps ficam como antes.
    NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC);
    NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    Serial.println("[bond] PAREAMENTO_V2: identidade do celular resolvida no host");
#else
    NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC);
    Serial.println("[bond] pareamento antigo (PAREAMENTO_V2 0)");
#endif
  }

  // DIAGNOSTICO (pareamento): quantos bonds foram restaurados da memoria no boot.
  // Se parear, religar e aqui vier 0 -> problema e PERSISTIR os bonds.
  // Se vier 1 (ou mais) -> bonds persistem; o problema e RECONHECER o celular que volta.
  {
    int nb = NimBLEDevice::getNumBonds();
    Serial.printf("[bond] restaurados no boot: %d\n", nb);
    for (int i = 0; i < nb; i++)
      Serial.printf("[bond]   %d: %s\n", i, NimBLEDevice::getBondedAddress(i).toString().c_str());
#if PAREAMENTO_V2
    Serial.printf("[bond] registros de identidade (host): %d\n", ble_rpa_get_num_peer_dev_records());
#endif
  }

  server = NimBLEDevice::createServer();
  server->setCallbacks(&serverCB, false);
  // (advertiseOnDisconnect nao existe com o anuncio estendido: quem decide e o updateAdvertising)

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCB, false);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  startScan();
}

void loop() {
  {
    // TELA_GIRADA mudou pelo Serial: gira na hora. O setRotation so vale para o que
    // for desenhado DEPOIS; sem a roda (tela "Escaneando...") nada era redesenhado e
    // parecia que o comando nao funcionou. Por isso reenvia o quadro atual na hora.
    static int rotAtual = -1;
    int rot = TELA_GIRADA ? 2 : 0;
    if (rot != rotAtual) {
      tft.setRotation(rot);
      if (rotAtual >= 0) spr.pushSprite(0, 0);
      rotAtual = rot;
    }
  }
  if (doConnect) {
    doConnect = false;
    statusMsg("Conectando...", wheelAdvName.c_str());
    if (connectToWheel()) {
      startAdvertisingAsWheel();
      logEvent("roda conectada: %s", wheelAdvName.c_str());
      statusMsg("Roda OK", "Aguardando dados...");
    } else {
      statusMsg("Falha na conexao", "Reescaneando...");
      needScan = true;
    }
  }

  if (needScan) {
    needScan = false;
    statusMsg("KS-S16 ponte v5", "Escaneando...");
    startScan();
  }

  processWrites();
  syncSubscriptions();
  writeEvents();
  rideLoop();
  writeRideLine();
  handleSerialConsole();
  hidSoltar();
  updateAdvertising();
  mpuAtualizar();              // le o MPU e acende/apaga a tela pela posicao
  if (BOTOES_PISCA) handlePiscaBotoes();   // botoes fisicos (desligados: agora e o controle 433)
  rx433Loop();                 // controle 433 (receptor no IO3): buzina, piscas, buzina externa
  buzExtLoop();                // comando agendado da buzina externa (sai quando o radio libera)
  tx433Loop();                 // manda o comando 433 agendado assim que o radio libera
  tx433TesteLoop();            // '433 teste': rajadas para o medidor de alcance
  dreamLoop();                 // mantem/recupera a conexao com a mochila (Dream), se pedida
  batLoop();                   // tensao da bateria do display (IO9)
  pwmPicoAtualizar();          // pico do PWM (conta mesmo com a tela apagada)
  sonoLoop();                  // 10 min sem roda e sem USB, ou bateria no fim -> dorme (BOOT acorda)
  freioLoop();                 // luz de freio: corrente negativa -> piscas e mochila em vermelho
  c3Loop();                    // PONTE_C3: estado/eventos do C3 (piscas, freio, controle)
  printLoop();                 // print da tela agendado (botao Print / comando 'print')

  bool wheelOk = client && client->isConnected();
  if (wheelOk) handleHornButton();
  handleTouch();               // sempre: sem a roda, a cortina de Ajustes ainda funciona

  static uint32_t lastDraw = 0;
  static int lastPage = -1, lastModo = -1;
  bool sobreposta = modoTela != TELA_NORMAL;      // Ajustes / tela do MPU
  // nao desenha com a tela apagada (so o backlight some; a ponte segue rodando)
  if ((wheelOk || sobreposta) && telaAcesa &&
      (millis() - lastDraw > 100 || page != lastPage || modoTela != lastModo)) {   // ~10 quadros/s
    lastDraw = millis();
    lastPage = page;
    lastModo = modoTela;
    drawPage();
  }
  // fechou os Ajustes sem a roda conectada: volta a mostrar a mensagem de status
  static bool sobrepostaAntes = false;
  if (sobrepostaAntes && !sobreposta && !wheelOk) drawStatus();
  sobrepostaAntes = sobreposta;
}
