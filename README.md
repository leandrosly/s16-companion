# S16 Companion

Display de pulso e "ponte" Bluetooth para o monociclo elétrico **KingSong KS-S16**, feito com um ESP32-S3 com tela touch de 2,8".

O display se conecta na roda, mostra os dados em tempo real, grava logs, controla farol, buzina, LEDs e música, e repassa tudo para os apps do celular (EUC World, Mono Riders, KingSong) como se ele fosse a própria roda.

> Este README é também o **caderno do projeto**: o que já funciona, o que estamos fazendo agora, as ideias guardadas para depois e tudo o que já descobrimos sobre os protocolos.

---

## Sumário

1. [Foco atual](#1-foco-atual)
2. [Estado do projeto](#2-estado-do-projeto)
3. [Hardware](#3-hardware)
4. [Como compilar](#4-como-compilar)
5. [Estrutura do repositório](#5-estrutura-do-repositório)
6. [Usando o display](#6-usando-o-display)
7. [Arquitetura da ponte BLE](#7-arquitetura-da-ponte-ble)
8. [Protocolo KingSong (S16)](#8-protocolo-kingsong-s16)
9. [Protocolo do módulo de LEDs "Dream" (Happy Lighting)](#9-protocolo-do-módulo-de-leds-dream-happy-lighting)
10. [Lições aprendidas (as pegadinhas)](#10-lições-aprendidas-as-pegadinhas)
11. [Roadmap](#11-roadmap)
12. [Ideias guardadas](#12-ideias-guardadas)
13. [Ferramentas e técnicas de investigação](#13-ferramentas-e-técnicas-de-investigação)
14. [Referências](#14-referências)

---

## 1. Foco atual

Para não misturar tudo: **no máximo 3 frentes ativas**. O resto fica na [lista de ideias](#12-ideias-guardadas) até uma destas terminar.

| # | Frente | Próximo passo concreto |
|---|---|---|
| 1 | **Pareamento do S16 Controle (PAREAMENTO_V2)** | ✅ Funcionou em 15/10 (reinício, sono e bateria desconectada: o controle volta sozinho). Confirmar no uso do dia a dia: depois de >15 min (o celular troca o endereço "no ar"), com os apps juntos e numa volta de verdade. Se seguir firme, passa para "Funcionando". |
| 2 | **Luz de freio** | ✅ Feita (14/10) — **testar andando**: ver se os limites (−2 A acende, −0,8 A apaga) ficam bons na prática (descida longa também regenera e acende). Ver [Luz de freio](#luz-de-freio). |
| 3 | **Alcance do 433 na luva** | Na volta de 16/10 o controle e os piscas "funcionam no geral" mas falham na rua. Suspeita: antenas molinha (~20 mm) escondidas atrás do display, as 3 plaquinhas (RX, TX, MPU) emboladas. Testar antena de fio esticado (**17,3 cm** = ¼ de onda), afastar RX/TX entre si e do display, e ver se o 5 V do conector existe na bateria (o STX882 transmite mais forte em 5 V). Ver [433 na prática](#433-na-prática-antenas). |

**Bateria do display** (saiu do foco): 18650 de ~3000 mAh aguentou **3,5 h de volta e voltou com ~60%** (16/10) → consumo na casa de ~300–350 mA, autonomia estimada de 8 h+. O % vem da tensão, então é aproximado. Falta: conferir a leitura com o multímetro e a célula fina definitiva.

**Concluídos** (na ponte v5 MIDIA): ✅ MPU (tela pelo pulso + config na tela), ✅ som (piezo), ✅ piscas 433 MHz, ✅ mochila Dream (tela, conectar, estado salvo), ✅ cortina de Ajustes, ✅ print da tela, ✅ barra de ícones com bateria e "carregando", ✅ pico do PWM, ✅ controle da buzina como botões do display (receptor no IO3) + buzina externa pareada com o display, ✅ dormir/acordar (sozinho, botão, acorda mexendo), ✅ indicadores das lanternas na principal, ✅ luz de freio, ✅ pareamento do S16 Controle que sobrevive ao reinício (V2, em observação).

**Lembrete de pedal:** desativar a **economia de bateria do celular** antes de rodar — com ela ligada o GPS do EUC World ficou falhando na metade da volta de 11/10.

---

## 2. Estado do projeto

### Funcionando ✅

**Ponte BLE**
- O display conecta na roda e se apresenta aos celulares com o mesmo nome e os mesmos serviços (clone completo da GATT).
- **EUC World, Mono Riders, WheelLog e app da KingSong** funcionando pela ponte, com **2 apps garantidos** (já rodaram os 4 juntos na v5 2APPS, com o Android dividindo conexões).
- **WheelLog só funciona na v5 2APPS:** ele exige a lista de serviços idêntica à da roda, e a v5 MIDIA tem 2 serviços a mais (HID `1812` e bateria `180F`).
- **Controle de mídia** (v5 MIDIA): música e volume do celular direto pelo display, numa identidade Bluetooth separada ("S16 Controle"). **Desde 15/10 (PAREAMENTO_V2) o pareamento sobrevive** a reinício, sono e falta de bateria: o controle volta sozinho. Ver [Controle de mídia](#controle-de-mídia-v5-midia).
- **Coexistência provada** com `MAX_CONNECTIONS` 6: roda + apps + controle de mídia + cliente da mochila (Dream) + MPU + som + 433, ninguém caiu.
- Relógio acertado pela própria roda ao conectar.

**Tela e controles**
- 3 páginas por deslize (**BMS** ← **principal** → **controles**) + **cortina de Ajustes** (de cima) + **cortina dos Piscas** (de baixo). Ver [Telas](#telas-deslizar-o-dedo).
- Velocidade com cor pelo PWM, barra de PWM com **pico** (segura 5 s), indicadores das lanternas dos lados da velocidade.
- Barra de cima só com ícones: roda, apps, controle de mídia, buzina, mochila, ventoinha, gravação, bateria do display (com raio de "carregando") e hora.
- Buzina no botão BOOT e na tela; farol (liga/desliga/auto), LEDs, volume da roda.
- **Print da tela** em BMP no cartão (`/prints`).

**Periféricos**
- **MPU-6050:** a tela acende/apaga pela posição do pulso (`Tela auto` na cortina, começa desligado). Config na tela: GRAVAR a posição de olhar, ângulos de acender/apagar, PADRÃO. Também acorda o display do sono, se `Acorda mexendo` estiver ligado.
- **Som:** codec ES8311 + I2S numa tarefa separada, piezo a 4 kHz (ressonância do piezo de 35 mm). Bipes de confirmação e alarme de bateria baixa. `Som` na cortina liga/desliga.
- **Piscas / lanternas 433 MHz** (STX882 no IO21, RMT): modo base (apagado/laranja/vermelho/piscando, salvo) e piscas esq/dir na cortina de baixo; apertar o pisca de novo volta ao modo base.
- **Controle da buzina como botões do display** (receptor SRX882S no IO3): buzina da roda, piscas esq/dir, buzina externa e desligar o alarme dela. Ver [Controle da buzina](#controle-da-buzina-sem-fio-43392-mhz).
- **Buzina externa** pareada com a identidade do display (`00 9F 1D`): tela com as 7 funções do controle original.
- **Mochila (Dream) como 2º cliente BLE:** conecta sozinha depois da roda (conexão bloqueante de até 2,5 s, **só com a roda parada** — a tentativa trava o loop, e andando isso pararia freio, buzina e 433; se não acha, tenta de novo em 15 s nas 3 primeiras vezes, depois a cada 1 min e depois a cada 5 min; o botão CONECTAR / `dream on` zera e tenta na hora), tela própria (liga, cores, brilho, efeito, DEMO, velocidade), estado salvo na flash e reaplicado ao reconectar.
- **Luz de freio** automática pela corrente negativa: lanternas vermelhas + mochila vermelha no máximo. Ver [Luz de freio](#luz-de-freio).

**Energia**
- Leitura da bateria no IO9, % e "carregando" (USB do PC ou degrau de tensão). Alarme abaixo de 3,35 V.
- **Dorme sozinho** (10 min sem roda e sem USB, ou bateria < 3,30 V), botão **Dormir** na cortina, acorda no BOOT (ou mexendo). Ao acordar, lanternas e mochila voltam como estavam.

**Registros e configuração**
- Logs no cartão SD: eventos sempre, viagem sob demanda (5 linhas/s).
- Configuração pelo Serial e pela tela, salva na flash. Versão do firmware no Serial e no rodapé da cortina.

### Em andamento 🔧

- Protocolo do módulo de LEDs Dream: comandos e resposta de estado decifrados; o índice do efeito não aparece na resposta e os quadros `D0`/`A0` seguem sem significado (baixa prioridade).
- PAREAMENTO_V2 em observação (ver [Foco atual](#1-foco-atual)).

### Problemas conhecidos ⚠️

- **Um 3º app** às vezes funciona porque o Android coloca dois apps na mesma conexão, mas pode cair em cascata. Garantido: 2 apps.
- **Disputa entre núcleos no `maps`** (revisão de 17/10, item 6, pendente): o `onDisconnect` da roda zera os ponteiros remotos no núcleo do Bluetooth enquanto o `loop()` (`syncSubscriptions`/`processWrites`) testa e usa o mesmo ponteiro; e o `mapGatt()` limpa o `maps` enquanto callbacks do celular o percorrem. Raro (nunca visto na prática), mas pode reiniciar a placa. Conserto: mutex ou copiar o ponteiro para uma variável local antes de testar e usar. Fazer separado e com teste.
- **Limite de 3 pareamentos** guardados (`CONFIG_BT_NIMBLE_MAX_BONDS` padrão da NimBLE). Com um celular só não faz diferença; com mais aparelhos, aumentar no `nimconfig.h`.
- Os comandos de próxima/anterior **da roda** (`95`) pulam mais de uma música (é da roda, acontece também no app da KingSong). A v5 MIDIA resolve controlando o celular direto.
- O app da KingSong mostra o histórico com horário deslocado (bug de fuso do app; a roda guarda a hora local certa).

---

## 3. Hardware

### Placa principal: ES3C28P (LCDWIKI)

ESP32-S3 (16 MB flash, 8 MB PSRAM OPI), tela IPS 2,8" 240×320 (ILI9341V, SPI), touch capacitivo FT6336G (I2C 0x38), slot microSD (SDIO), amplificador de áudio 1,5 W, LED RGB, carregador de LiPo.

| Função | Pinos |
|---|---|
| Tela (SPI) | SCK 12, MOSI 11, MISO 13, CS 10, DC 46, backlight 45 |
| Touch (I2C) | SDA 16, SCL 15, INT 17, RST 18 — **touch girado 180°: inverter X e Y** |
| Cartão SD (SDIO, 1 linha) | CLK 38, CMD 40, D0 39 |
| Áudio (I2S) | EN 1 (amplificador: 0 = liga), MCLK 4, BCLK 5, LRCK 7, **IO8 = som ESP32→codec**, IO6 = microfone codec→ESP32. Codec ES8311 configurado por I2C (0x18) |
| Bateria (ADC) | 9 — lê **metade** da tensão (divisor 2:1 da placa); o firmware multiplica por 2 |
| LED RGB (WS2812) | 42 |
| Botão BOOT | 0 (usado como buzina) |
| USB nativo | 19, 20 |

**Conectores (todos 1,25 mm):**

| Conector | Pinos | Plano de uso |
|---|---|---|
| I2C | 3.3V, GND, IO15 (SCL), IO16 (SDA) | **MPU-6050 (0x68)** no mesmo barramento do touch (0x38) e do codec ES8311 (0x18), usando as funções `lgfx::i2c` da LovyanGFX (testado: 0 erros com o touch em uso) |
| GPIO | IO21, IO14, IO3, IO2 | IO21 = STX882 (transmissor 433 MHz); IO14 = INT do MPU (acorda o display do sono com `Acorda mexendo`); **IO3 = DATA do receptor SRX882S** (controle 433); IO2 = livre (era botão do pisca; os botões físicos foram desligados com `BOTOES_PISCA = false`) |
| UART | 5V, GND, TXD (IO44), RXD (IO43) | livre (ex.: UART para GPS) |
| BAT | + / − | LiPo 3,7 V (**conferir a polaridade antes de ligar!**). Carregador **TP4054** na placa (até 500 mA): carrega sozinho com o USB-C ligado, nada a fazer no firmware. Com USB presente a placa roda pelo USB (Q3 corta a bateria). ⚠️ **A placa não tem proteção de descarga**: usar célula **com placa de proteção (PCM)** — 18650 "protegida" ou LiPo "with protection board". |
| SPEAKER | 2 pinos | piezo passivo + 100 Ω em série (saída em ponte: nenhum dos pinos é GND) |
| BOOT (IO0) | soldar nos pads do botão | botão da buzina em paralelo (não segurar ao ligar a placa) |

**Receptor SRX882S no display:** VCC → 3V3 · GND → GND · **CS → 3V3** (obrigatório: solto = dormindo) · DATA → IO3 · antena espiral. Alimentado em **3,3 V** (não 5 V), para a saída DATA não mandar 5 V no ESP32.

Não há outros pinos livres no chip. Para mais portas: **MCP23017** (16 GPIO por I2C).

### Outros

- **2× ESP32-C3 Supermini**: protótipos (ponte v3 com LCD 16×2, captura de 433 MHz) e, no futuro, controlador da fita de LED da mochila.
- **Módulo de LEDs iFlight "Dream#0E4D"**: mochila dos drones, controlado pelo app Happy Lighting — e agora pelo display.
- **Rádio 433 MHz**: **STX882** (transmissor, no display, IO21) + **SRX882S** (receptor, no display, IO3). FS1000A + MX-RM-5V na gaveta.
- **MPU-6050** (GY-521), no I2C do display, INT no IO14.
- **Lanternas/piscas tipo bike** (par no capacete + par na traseira do mono), controle sem fio 433,92 MHz.
- **Buzina sem fio 433 MHz** com controle de 5 botões (o controle virou entrada do display; a buzina obedece à identidade do display).
- Bateria de teste: 18650 protegida (definitiva: LiPo fina `404070` com proteção).

---

## 4. Como compilar

**Versão do arquivo:** a 1ª linha do `.ino` traz `// VERSAO: v5-AAAAMMDDHHMM` (data e hora em que foi gerado). A mesma versão aparece no Serial ao ligar (`=== S16 Companion ponte v5-... ===`) e no rodapé da cortina de Ajustes — assim dá pra saber qual arquivo está gravado no display.


### Arduino IDE

- Core **esp32 by Espressif 3.3.12**.
- Bibliotecas:
  - **NimBLE-Arduino 2.5.1** — **COM O PATCH** da pasta `patch_nimble_2.5.1/` (ver o README de lá). Sem o patch: o EUC World recusa a ponte, a placa reinicia ao desconectar apps e os anúncios são recusados. O `nimconfig.h` do patch também liga a **privacidade no host** (`CONFIG_BT_NIMBLE_HOST_BASED_PRIVACY 1`), exigida pelo `PAREAMENTO_V2` — sem ela a compilação para num `#error` que explica.
  - **LovyanGFX** (lovyan03).
  - `SD_MMC` e `Preferences` já vêm com o core.

> ⚠️ **Não atualize a NimBLE-Arduino** sem refazer o patch. Ao trocar de PC: instale a 2.5.1 e copie os 5 arquivos do patch. Mudar o `nimconfig.h` faz a biblioteca inteira recompilar (a 1ª compilação demora mais).

**Chaves no topo do sketch:** `USAR_SOM` (0 desliga todo o som, para isolar problemas) e `PAREAMENTO_V2` (1 = pareamento novo do S16 Controle; 0 = jeito antigo, que repareia a cada reinício).

### Menu Ferramentas (ES3C28P)

```
Placa:            ESP32S3 Dev Module
USB CDC On Boot:  Enabled
USB Mode:         Hardware CDC and JTAG
Flash Size:       16MB (128Mb)       ← antes do Partition Scheme
Partition Scheme: 16M Flash (3MB APP/9.9MB FATFS)
PSRAM:            OPI PSRAM          ← obrigatório
JTAG Adapter:     Disabled
```

Conferências: no fim da gravação, "Maximum is 3145728 bytes"; no Serial, `PSRAM: 8388608 bytes`.

### Decodificar um reinício (Guru Meditation)

1. Com o binário que está na placa: **Sketch > Export Compiled Binary** (gera o `.elf` em `build/`).
2. Copie a linha `Backtrace:` e rode:

```powershell
& "$env:LOCALAPPDATA\Arduino15\packages\esp32\tools\esp-x32\2601\bin\xtensa-esp32s3-elf-addr2line.exe" -pfiaC -e "CAMINHO\sketch.ino.elf" 0x4200... 0x4200...
```

Os endereços só valem para **o mesmo binário**. Alternativa: extensão "ESP Exception Decoder" (dankeboy36).

---

## 5. Estrutura do repositório

| Pasta / arquivo | O que é |
|---|---|
| `ks_s16_ponte_v5_midia/` | **Versão principal**: ponte + 2 apps + controle de mídia (2 identidades BLE) |
| `ks_s16_ponte_v5_2apps/` | Mesma ponte, sem controle de mídia (mais simples, fallback estável; **a única que funciona com o WheelLog**) |
| `patch_nimble_2.5.1/` | Os 5 arquivos corrigidos da NimBLE e onde vai cada um |
| `ks_s16_ponte_v4_s3/` | v4: primeira versão no ES3C28P (3 telas, buzina no BOOT, logs, mídia HID). Histórico |
| `ks_s16_ponte/` | v3: ponte com clone completo da GATT no ESP32-C3 + LCD 16×2. Histórico |
| `ks_s16_lcd_test/` | Primeiro teste: roda → ESP32-C3 → LCD 16×2. Histórico |
| `galeria_fotos/` | Exemplo de aprendizado: tela + touch + sprites + zoom (+ script `converter_fotos.py`) |
| `captura_433/` | Sketch para descobrir os códigos do controle dos piscas (ESP32-C3) |
| `medidor_433/` | **Medidor de alcance do 433** (ESP32-C3 + SRX882S + LCD 16×2): conta quantas repetições das rajadas do `433 teste` chegam |
| `teste_i2c_mpu/` | Teste do MPU-6050 dividindo o I2C com o touch (Plano A LovyanGFX / Plano B Wire1) |
| `teste_tx433/` | Display imitando o controle dos piscas (STX882 no IO21, RMT) |
| `teste_mochila/` | Display como cliente BLE do módulo Dream: liga/cor/efeito/handshake |
| `teste_piezo/` | Teste do piezo no SPEAKER: ES8311 + I2S, varredura para achar a ressonância, bipe/pisca/alarme |
| `teste_tela_mpu/` | Teste: acender/apagar a tela pela posição do pulso + deep sleep parado (acorda pelo BOOT ou INT do MPU) |
| `prints/` | Prints da tela tirados pelo display (cópias do cartão) |
| `Conexões.drawio` | Diagrama das ligações |

---

## 6. Usando o display

### Telas (deslizar o dedo)

```
              ↓ arrastar pra BAIXO a partir da barra de cima
    [ AJUSTES ]  →  MPU >  /  Mochila >  /  Buzina ext. >   (cada uma com VOLTAR = principal)

[ BMS ]  ←  [ PRINCIPAL ]  →  [ CONTROLES ]        (deslizar para os lados)

              ↑ arrastar pra CIMA a partir da borda de baixo ("alça")
    [ PISCAS ]   (arrastar pra baixo fecha)
```

- **Ajustes (cortina de cima):** arraste pra **baixo** começando na barra de cima; arraste pra **cima** para fechar. Funciona **até sem a roda** (bancada). Botões: **MPU >** (abre a config), **Tela auto**, **Som**, **Mochila >** (abre a tela da mochila, com CONECTAR e LIGA/DESLIGA), **Buzina ext. >**, **Print** (salva a tela 3 s depois; a cortina **continua aberta** — deixa ela pra "printar" a própria cortina, ou arrasta pra cima e vai pra outra tela dentro dos 3 s), **Dormir** (2 toques: o 1º pede "certeza?"), **Acorda mexendo** (o MPU acorda o display do sono). Embaixo: bateria (V, % e "carregando"), cartão, sensores e a versão do firmware.
- **Mochila:** o botão do canto mostra o estado: **CONECTAR** (cinza) → **CONECTANDO...** (enquanto tenta, até 2,5 s) → **PROCURANDO...** (amarelo escuro, entre tentativas; tocar cancela) → **DESCONECTAR** (verde, conectada).
- **Piscas (cortina de baixo):** nas páginas de deslizar há uma "alça" cinza no meio da borda de baixo; arrastando pra **cima** a partir dela abre a tela dos piscas/luz; arrastando pra **baixo**, fecha. Também funciona sem a roda.
- **Buzina externa:** as 7 funções do controle original, mandadas com a identidade do display: **BUZINA**, volume −/+, **< som / som >** (= segurar vol+/vol− no controle: troca entre os 4 sons), alarme liga/desliga. Se o rádio 433 estiver ocupado, o comando espera na fila e sai em seguida. VOLTAR = tela principal. ✅ Testado em 12/10 (v5-202610122151): troca de sons funcionando pelo display, piscas na ordem certa pelo controle.
- **Config do MPU:** **GRAVAR** (com o braço na posição de olhar), **AUTO sim/não**, **PADRÃO** (toque 2×; volta os ângulos 30/50) e as barras *acende abaixo de* / *apaga acima de* (seta amarela = onde o braço está agora). Salva ao soltar o dedo. Com esta tela (ou os Ajustes) aberta a tela não apaga sozinha.

- **Principal:** velocidade (cor pelo PWM: verde < 70% < amarelo < 85% < vermelho), barra de PWM, tensão, corrente, temperaturas, trip, máxima e limite.
- **Lanternas na principal:** um indicador de cada lado da velocidade, logo abaixo da barra: bolinha **vermelha** ou **laranja** (modo base), laranja com um **pontinho piscando** no meio (modo piscando), **seta verde piscando** apontando pra fora no lado da curva (pisca ativo — o outro lado fica apagado, igual à lanterna); nada = apagadas.
- **Pico do PWM** (como o pontinho de um VU): um risco na barra marca o maior PWM recente e "pico NN%" aparece à direita, acima da barra. Fica **parado 5 s** e depois **desce 4 pontos/s** (`PWM_PICO_SEGURA`, `PWM_PICO_CAI`). Conta mesmo com a tela apagada ou em outra página — dá pra ver depois o pico daquela lomba em que não estava olhando.
- **BMS:** os 2 packs com tensão, SoC, temperatura, célula mínima e máxima, diferença em mV e gráfico das 20 células (escala ampliada).
- **Controles (v5 MIDIA):**

| | | |
|---|---|---|
| 📱− volume celular | 📱+ volume celular | |
| ⏮ anterior | ⏯ play/pause | ⏭ próxima |
| 🔈− volume roda | 🔊+ volume roda | 📄 gravar log |
| ☀ farol (liga → desliga → auto) | 🔴🟢🔵 LEDs (liga/desliga) | 📢 buzina |

### Barra superior

Só ícones, **sempre visíveis** — cinza escuro = desligado/inativo:

roda (verde conectada / vermelha) · celular (ciano com app conectado) · ♪ (magenta com o controle de mídia ativo) · corneta (amarela quando a buzina toca) · 3 bolinhas da mochila (RGB conectada, amarelo piscando procurando) · ventoinha (laranja ligada) · ● gravação (vermelha gravando, laranja ligada esperando a roda; só contorno = sem cartão) · bolinhas das telas · bateria (verde > 40%, amarela > 15%, vermelha piscando abaixo; **raio amarelo = carregando**) · hora.

**Como o display sabe que está carregando** (a placa não tem um pino pra isso — o `CHRG` do TP4054 não vai pro ESP32): (1) USB ligado num **computador** → o USB nativo do ESP32 percebe o PC (carregador de parede/powerbank não é percebido assim); (2) **degrau de tensão**: ao ligar o carregador a placa passa a rodar pelo USB e a bateria só recebe carga, então a tensão **sobe de repente**; ao tirar, **cai de repente**. Limite em `BAT_DEGRAU` (0,06 V) — se o raio errar, é ele que se ajusta. Cada mudança vira evento no log (`bateria: carregando`).

**Raio preso (13/10, corrigido):** bipe/alarme e transmissão 433 puxam corrente e a tensão **afunda**; quando acabam, ela **volta de repente** — parecia o degrau do carregador, e o raio ficava aceso (3,95 V, "carregando" na cortina, sem carregador). Agora: degraus durante som/433 e até 4 s depois são ignorados, e com o raio aceso (sem PC) a tensão filtrada não pode cair mais que `BAT_CAIU` (0,04 V) abaixo do máximo — carregando ela não cai; se cair, o raio apaga.

### Luz de freio

Automática (salva em `FREIO_AUTO`, começa **ligada**; botão "Luz de freio: sim/não" na cortina dos piscas):
- **Acende** com a corrente da roda **abaixo de −2 A** (regeneração = freando) andando a mais de 3 km/h. **Apaga** quando a corrente volta acima de −0,8 A por 2 leituras seguidas (ou parou), e fica no mínimo 1 s acesa (histerese: não fica piscando).
- **Piscas:** vermelho; depois volta ao modo base. **Com pisca de curva ativo não mexe** (o aviso de curva tem prioridade); se o pisca terminar durante a frenagem, volta pro vermelho. Prioridade das lanternas: pisca de curva > freio > modo base.
- **Mochila** (se conectada): vermelho puro no brilho máximo, **sem mexer no estado salvo**; depois volta exatamente como estava. O DEMO pausa enquanto freia.
- **Tela:** as bolinhas dos lados da velocidade ficam vermelhas com contorno branco; na cortina dos piscas o botão vira "FREANDO".
- **Limitação:** a roda manda a corrente ~2 vezes por segundo (frame `A9`) → até ~0,5 s de atraso. Descida longa também regenera, então também acende.
- Teste parado: `freio` no Serial (2,5 s). Limites no topo do código: `FREIO_LIGA_A`, `FREIO_DESLIGA_A`, `FREIO_VEL_MIN`, `FREIO_MIN_MS`.

### Dormir (deep sleep)

O display **dorme sozinho** (quase tudo desliga, inclusive o Bluetooth) quando:
- fica **10 min sem roda e sem USB/carregador** (e sem toque na tela) — na mesa, parado; ou
- a **bateria chega ao fim**: abaixo de **3,30 V** por 30 s (a placa não protege a célula sozinha).

**Dormir apaga tudo, acordar devolve tudo** (14/10): ao dormir apaga as lanternas (433) e a mochila (se conectada); ao ligar/acordar, as lanternas voltam ao **modo base salvo** (o pisca de curva não volta) e a mochila, ao reconectar, volta **como estava** — o estado dela (ligada, cor/efeito, brilho, velocidade) agora é salvo na flash (2 s depois da última mudança, para não gastar a flash com o slider). Antes ela apagava ao reconectar porque o display religava achando que estava "desligada".

Antes de dormir: fecha a gravação da viagem, grava os eventos, apaga as lanternas pelo 433 (se estavam acesas) e mostra "Dormindo — aperte BOOT para ligar" por 2,5 s. **Acorda no BOOT** (é um reinício: a ponte começa do zero). Também dá pra mandar dormir pelo botão **Dormir** da cortina (2 toques). **Acorda mexendo** (botão na cortina, salvo em `ACORDA_MEXENDO`, começa **desligado**): antes de dormir o MPU entra em "modo vigia" (mede a 5 Hz sozinho, gasta quase nada) e o INT dele (IO14, já ligado) acorda o ESP32 ao detectar movimento. Desligado por padrão porque, guardado na mochila, ele acordaria a cada sacolejo (e ficaria 10 min acordado). **Nunca** vale no sono por bateria no fim. Sensibilidade: `MPU_MOT_THR` (20). Por isso o MPU fica sempre alimentado (o LED dele aceso no sono é normal: é ligado direto na alimentação da plaquinha). Andando ele nunca dorme (roda conectada). Teste: `dormir` no Serial. Ajustes no topo do código: `DORMIR_SEM_RODA_MS`, `BAT_CORTE_V`.

### Controle de mídia (v5 MIDIA)

1. Pareie **"S16 Controle"** nas configurações de Bluetooth do celular (**uma vez só**: com o `PAREAMENTO_V2`, depois de reiniciar, dormir ou ficar sem bateria o display reconhece o celular e o controle reconecta sozinho).
2. Os apps continuam conectando em **"KSN-S16P--4C4B"**, sem pareamento.
3. Problema? Esqueça os dois no celular, digite `esquecer` no Serial, reinicie o display e pareie de novo (ver a [faxina](#10-lições-aprendidas-as-pegadinhas)).

**Como funciona o PAREAMENTO_V2** (15/10): o celular usa um endereço "no ar" que muda a cada ~15 min (RPA); para reconhecê-lo depois de um reinício, o display precisa da **IRK** (a "chave de identidade") dele. Pedir a IRK sempre consertou isso, mas a NimBLE então ligava a resolução de endereço **no controlador** do rádio, e isso derrubava os apps na identidade da roda. A V2: (1) pede a IRK do celular (`setSecurityRespKey(ENC|ID)`); (2) **não manda a nossa identidade** (`setSecurityInitKey(ENC)` — ela era o endereço público = o da roda, e podia fazer o celular "juntar" o S16 Controle com a roda; o S16 Controle já tem endereço próprio fixo, random static); (3) liga a **privacidade no host** da NimBLE: a IRK vai para uma lista **em software**, o endereço é resolvido na hora da conexão e o controlador nunca liga a resolução. No ESP32 "clássico" a NimBLE usa isso sempre; no S3 é opção do `nimconfig.h`.

**Diagnóstico no Serial:** no boot, `[bond] restaurados no boot: N` e `[bond] registros de identidade (host): N`; ao parear/reconectar, `[bond] no ar XX | identidade YY` (a identidade tem que ser a mesma depois de reiniciar). `bonds` mostra os dois números a qualquer hora.

**Voltar atrás:** `PAREAMENTO_V2 0` no sketch (a linha do `nimconfig.h` pode ficar; para voltar 100% ao estado antigo, comente-a também) e faxina dos pareamentos dos dois lados. A versão de antes da V2 é a `v5-202610142150` (tag `antes-pareamento-v2`).

### Logs no cartão (`/logs`)

- `eventos.csv`: sempre ativo. Display ligado, roda e apps conectando/desconectando (com motivo), cada toque no display, relógio acertado.
- `viagem_AAAAMMDD_HHMMSS.csv`: velocidade, PWM, corrente e tensão, 5×/s, ligado no ícone 📄.
- **A gravação é "lembrada"** (16/10): o 📄 liga a *intenção* de gravar (salva na flash, `GRAVAR_VIAGEM`). Com ela ligada, o display grava sempre que a roda está mandando dados: a roda desligou/sumiu por 10 s → fecha o arquivo; voltou (mesmo depois de o display dormir ou reiniciar) → abre um **arquivo novo** sozinho (espera até 5 s pelo relógio da roda, para o nome sair com data e hora). Só o 📄 desliga. Cores (ícone e bolinha da barra): **laranja** = ligada, esperando a roda; **vermelho** = gravando. Motivo: na volta de 16/10 a roda desligou numa parada longa e a gravação não voltou.

### Console Serial (115200, "Nova linha")

| Comando | Efeito |
|---|---|
| `config` | mostra a configuração |
| `config padrao` | volta aos valores de fábrica |
| `NOME = valor` | muda uma opção e salva na flash. Liga/desliga: `DUMP_HEX`, `DUMP_PHONE`, `DUMP_NOVOS`, `DUMP_OTHER`, `IGNORA_CANCELAR`, `TELA_AUTO`, `SOM_ON`, `DREAM_AUTO`, `ACORDA_MEXENDO`, `FREIO_AUTO`, `TELA_GIRADA`, `GRAVAR_VIAGEM` (ex.: `DUMP_PHONE = false`) |
| `TELA_GIRADA = true` / `false` | gira a tela **e o touch** 180° (lado largo/USB-C pro outro lado, para a caixa); vale na hora e fica salvo |
| `NAME_FILTER = KS` / `TARGET_MAC = c5:39:...` / `TARGET_MAC = -` | como achar a roda (vale ao reiniciar) |
| `t 95 6 FF` | envia o comando `95` com o byte 6 = `FF` (experimentos) |
| `raw AA 55 ... 5A 5A` | envia 20 bytes exatos (ex.: um frame capturado) |
| `esquecer` | apaga os pareamentos (v5 MIDIA); esqueça no celular também |
| `bonds` | lista os pareamentos guardados e os registros de identidade (V2) |
| `freio` | simula a luz de freio por 2,5 s |
| `print` | salva a tela agora em `/prints/print_NNN.bmp` |
| `dormir` | dorme agora (deep sleep); o BOOT acorda |
| `comandos` (ou `ajuda`, `?`) | lista todos os comandos |
| `bat` | tensão da bateria (filtrada), %, carregando ou não, leitura crua do IO9 e se o USB é de um PC |
| `bat cal 4.10` | **calibra** a leitura da bateria: digite a tensão medida no multímetro (sem carregador); o display calcula o fator e salva (`bat cal 1` volta ao padrão). Vale para %, alarme e corte do sono. 22/10: display mostrava 4,05 V com 4,10 V no multímetro |
| `mpu` / `mpu on` / `mpu off` / `mpu gravar` | status do MPU (com os ângulos) / tela automática / grava a posição |
| `som bipe` / `som alarme` / `som on` / `som off` | testa o som / liga-desliga os bipes |
| `433 e\|d\|x\|l\|v\|p` | piscas esq/dir, luz apagada/laranja/vermelha/piscando |
| `433 teste` | liga/desliga o **teste de alcance**: rajadas `00 9F 1E NN` a cada ~0,8 s para o `medidor_433`. Continua sem o USB e o display não dorme enquanto estiver ligado |
| `433 raw 009F1DAA [curto longo]` | manda bits crus (até 10 dígitos hexa) no formato do controle da buzina — teste de pareamento. Tempos opcionais em µs (padrão 352 920) |
| `dream on\|off\|liga\|desliga\|cor RR GG BB\|ef N` | mochila |

Prefixos no Serial: `C>` app → roda · `D>` display → roda · `R?` resposta "incomum" da roda · `R>` todos os frames da roda (`DUMP_HEX`).

---

## 7. Arquitetura da ponte BLE

```
 Roda S16  ◄──BLE──►  ESP32-S3 (cliente + servidor)  ◄──BLE──►  Celular
 (Freqchip)           clone da GATT da roda                     EUC World / Mono Riders / KingSong
                      + "S16 Controle" (HID)                    + Android (controle de mídia)
```

- **Clone da GATT em 3 etapas:** conecta → grava o modelo dos serviços → **desconecta** → monta o servidor (a NimBLE não deixa montar com conexão aberta) → reconecta e mapeia.
- Os serviços genéricos **1800/1801 precisam ser idênticos aos da roda** (o EUC World confere): resolvido no patch.
- **Duas identidades** (anúncio estendido): a roda (sem pareamento) e o "S16 Controle" (pareado, endereço próprio random static, derivado do MAC). Assim o pareamento não "contamina" a conexão dos apps. O display só distribui a chave de cifragem, nunca a identidade dele; a do celular é resolvida no host (`PAREAMENTO_V2`).
- **Cliente duplo:** além da roda, o display é cliente da mochila (Dream), conectando só depois da roda.
- Cada notificação é enviada **por conexão** para quem assinou; o cancelamento da FFE1 é ignorado (`IGNORA_CANCELAR`) porque o Android às vezes coloca dois apps na mesma conexão.
- **Orçamento do rádio:** com anúncio estendido, conexões + anúncios ≤ `MAX_CONNECTIONS − 1`. Com `MAX_CONNECTIONS 6`: roda + 2 apps + controle + anúncio — e na prática **coube também o cliente da mochila** (provado com tudo conectado; o aumento para 7 não foi preciso).

---

## 8. Protocolo KingSong (S16)

Serviço `FFE0`, característica `FFE1` (notificação + escrita). Frame padrão de 20 bytes:

```
AA 55 [b2 .. b15: dados] [b16: tipo/comando] [b17] [b18 b19]
                                               14   5A 5A    ← formato "clássico"
                                               xx   CRC CRC  ← formato estendido (CRC desconhecido)
```

A roda: `KS-S16P-0254`, série `KSSE61O231020J158`, anuncia como `KSN-S16P--4C4B`, MAC `c5:39:32:38:4c:4b`. Módulo BLE: Freqchip. Aceita **uma** conexão.

### Dados que a roda envia sozinha

| Tipo | Conteúdo |
|---|---|
| `A9` | 2–3 tensão (/100 V) · 4–5 velocidade (/100 km/h) · 10–11 corrente (signed, /100 A; **negativa = regeneração/freio**) · 12–13 temperatura da **placa-mãe** (/100 °C) |
| `B9` | 2–5 trip (u32 "invertido", /1000 km) · 6–7 tempo (s) · 8–9 máxima (/100 km/h) · 12 ventoinha ("Cooling") · 14–15 temperatura do **motor** (confirmado com o app) |
| `F5` | 14 carga de CPU (%) · **15 PWM (%)**. **Captura de 14/10 (liga → em pé → deita):** a carga de CPU fica **~36%** com a roda desarmada e pula para **~56%** enquanto ela está **armada** (o controle do motor rodando), voltando a ~36% ao deitar — é o melhor indicador de "armado" que apareceu (não há um byte-flag limpo). No mesmo período a corrente do `A9` fica **exatamente 0,00 A** (desarmada mostra −0,04…−0,10 A, ruído do sensor). |
| `F6` | 2–3 limite de velocidade (/100; começa 20, vira 32) · 14 flags de alarme? (`DA` com PWM alto). **Pista (14/10):** fica **20 até a roda "armar"** (motor ligado mas desativado → ativa ao pôr o mono em pé, quando os giros entram) e **vira 32 ao armar**; ao desarmar e deitar, **continua 32**. Hipótese: 20 = limite "de partida/segurança" antes do 1º armar. Captura de 14/10: o F6 mudou de `D0 07` (20) para `80 0C` (32) **~2 s depois de armar** e não voltou. Nenhum outro byte do F6/A9/B9/C9 marcou armado/desarmado nos dois sentidos; o indicador que serve é a **carga de CPU do F5** (ver acima). Também confirmado: `B9` 6–7 conta segundos desde que a roda ligou. |
| `F1`/`F2` subtipo `D0` | BMS pack 1/2 (frame longo): [21] nº células, depois tensões (mV), nº de sensores, temperaturas (0,1 K), corrente, tensão, SoC (/10 %) |
| `C9` | tudo zero |

### Comandos (byte 16 = comando)

| Função | Comando | Parâmetros |
|---|---|---|
| Nome / série (handshake) | `9B` / `63` | resposta `BB` / `B3` em ASCII |
| **Buzina** | `88` | — |
| **Farol** | `73` | b2: `12` liga, `13` desliga, `14` auto |
| Intensidade do farol | `54` | b4 = 0–100, b14 = `01`, b15 = `F2` |
| Pisca do farol | `53` | b2: `01`/`00` |
| **Volume da roda** | `95` | b2 = `FF` sobe, b3 = `FF` desce |
| Música (na roda) | `95` | b4 = `FF` próxima, b5 = `FF` anterior |
| LEDs liga/desliga | `6C` | b2: `00` liga, `01` desliga |
| Efeito dos LEDs | `50` | b2: 0 auto, 1 cor fixa, 2 água corrente, 3 respirando |
| Equalizador | `97` | b2: 0–3, 4 desliga |
| Cores dos LEDs | `59` | RGB nos bytes — a decifrar |
| Modo de pilotagem | `87` | b2: `02` iniciante, `01` intermediário, `00` expert (b3 = `E0`) |
| Valor 0–100 do modo | `87` (b17 = `20`) | b2 = `9C`, b3 = `01`, b4 = valor |
| Senha | `41` define / `42` apaga | ASCII nos bytes 2–5 |
| Trava | `5D` | b2: `01` normal, `02` silencioso |
| Destrava | `5D` | 6 dígitos ASCII nos bytes 10–15 — **desafio-resposta** (o `5E` devolve o desafio); não reutilizável |

### Respostas descobertas (`R?`)

| Pedido → resposta | Significado provável |
|---|---|
| `F9` (formato estendido) → `F9` | **Relógio**: b4–5 ano, b6 mês, b7 dia, b8 h, b9 min, b10 s. Frame para perguntar: `AA 55 00…00 F9 01 B9 78` |
| `98` → `B5` | alarmes/tiltback em km/h (`1E 1F 20 20` = 30, 31, 32, 32) |
| `99` → `E7` | "123456" (outra senha?) |
| `E3`/`E4` `B1` → datas | relógios das BMS (discordam entre si) |
| `5E` → `5F` | desafio da trava ("840200", "000000") |
| `E5`/`E6`, `E1`/`E2` | modelo da roda ("KSS16+A-3055-2.04") e das BMS ("KSC02BV0225D11...") |

### Observações

- "Weak magnetic acceleration" no app = **field weakening**: +velocidade, −torque, mais calor. Com ele ligado, o PWM deixa de ser uma medida fiel da margem de segurança.
- Regeneração com bateria cheia em descida longa pode levar a tensão acima do limite.
- O app da KingSong acerta o relógio da roda e das BMS na hora local.
- Cada pack tem 6 temperaturas (4 de células, MOS e ambiente). O app mostra a **média** delas como "temperatura do pack"; o display hoje mostra a **máxima**.
- "Ridinglogging" do app: a roda guarda um **resumo por sessão** (liga → desliga): velocidade máxima/média, potência, energia, tensão mín./máx., corrente máx., temperaturas, distância e tempo. Os horários das sessões estão certos, mas o app **agrupa por dia com +11 h** (fuso da China), e os gráficos são **um ponto só** por sessão, desenhado no horário de fim e "suavizado" em curva. A sessão atual só aparece depois de desligar a roda.

---

## 9. Protocolo do módulo de LEDs "Dream" (Happy Lighting)

Família "Triones", variante Dream. Nome `DREAM#0E4D`, MAC `2b:80:04:13:0e:4d`. Escrita em **FFD5/FFD9** (sem resposta), respostas em **FFD0/FFD4** (notificação). Aceita uma conexão: **feche o Happy Lighting** antes de conectar outro.

| Função | Comando |
|---|---|
| Liga / desliga | `CC 23 33` / `CC 24 33` |
| Cor RGB | `56 RR GG BB 00 F0 AA` |
| Branco? | `56 00 00 00 WW 0F AA` (a testar) |
| **Efeito** | `9E 00 [efeito] [velocidade] [brilho] 00 E9` — efeito começa em 0 (Style 001 = `00`); velocidade `01`–`FF`; brilho `19`–`FF` |
| `BB xx yy 44` (Triones clássico) | **não funciona** neste módulo |

Handshake do app ao conectar: `EF 01 77` (pede o estado), `C5 F0 5C`, `CF 01 02 03 04 FC`, relógio `10 14 1A [mês] [dia] [h] [min] [s] [dia da semana] 0F 01`, `24 2A 2B 42`. O app manda cada comando 3–4 vezes.

**Resposta de estado (FFD4), descoberta com `teste_mochila`:** o módulo **responde** ao `EF 01 77`. A notificação principal é um quadro de 12 bytes que começa em `66` e termina em `99`:

```
66 00 [23|24] 00 02 28 FF B4 00 00 00 99
      23 = ligado / 24 = desligado
```

Mapa dos bytes do quadro `66…99` (testado mandando efeitos 5, 12 e 21 com velocidades diferentes):

| byte | significado |
|---|---|
| 2 | liga (`23`) / desliga (`24`) — igual ao comando `CC` |
| 5 | velocidade (escala interna; acompanha o que foi enviado) |
| 6 | brilho (enviei `FF`, voltou `F6`) |
| — | **o índice do efeito NÃO aparece** no quadro |

Então o display pode **sincronizar** liga/desliga, velocidade e brilho ao conectar, mas não tem como ler qual efeito está rodando. Também chegam quadros `D0 … 0D` (12 B), `A0 F0 00 0A` (4 B) e uma rajada de 8 bytes `[x]0 0F 00 00 [n] 00 0F [chk]` (`n` = 0,5,10,15,20,25) — provavelmente temporizadores do módulo.

**Efeitos:** o byte do `9E` é de 1 byte e muda a aparência até o final → tratar como **0–255** (pode haver repetições no topo, mas não importa). Sem nomes: a tela usa "efeito N" com − / + e um DEMO para percorrer.

**A descobrir (menor prioridade):** se o `9E` funciona sem o handshake; o significado dos quadros `D0`/`A0`.

### Controle dos piscas (433,92 MHz)

Capturado com `captura_433` v2 (SRX882S no C3). Quadro de **40 bits** (5 bytes, MSB primeiro) + pulso curto + ~9 ms de silêncio; o controle repete ~14×.

| | |
|---|---|
| Bit 1 | ligado ~990 µs + desligado ~330 µs |
| Bit 0 | ligado ~330 µs + desligado ~990 µs |
| Quadro | `3C 24 06 [cmd] [cmd XOR 05]` (3C 24 06 = identidade deste controle) |

| cmd | Função |
|---|---|
| `11` | pisca esquerda liga |
| `12` | pisca direita liga |
| `03` | desliga (piscas e luz) |
| `13` / `23` / `33` | luz laranja / vermelha / laranja piscando |

O botão central cicla laranja → vermelho → piscando → desliga, mas o código de cada modo é diferente: o display pode escolher o modo direto. A rc-switch não decodifica (máx. 32 bits). Transmissão: `teste_tx433` (STX882 no IO21, pelo periférico RMT).

### Controle da buzina sem fio (433,92 MHz)

Capturado com `captura_433` (11/10/2026), 2 apertos por botão — **código fixo** (os dois apertos são idênticos). Mesma **ideia** dos piscas (bit 1 = longo + curto, bit 0 = curto + longo), mas quadro de **32 bits** (4 bytes, MSB primeiro) + 1 pulso de parada + ~1,2 ms de intervalo entre repetições.

**Tempos medidos (média de 10 capturas):** curto ~352 µs, longo ~907–936 µs → proporção **~2,6** (a dos piscas é 3,0: 330/990). O 1º teste com os tempos dos piscas **não fez a buzina tocar** (12/10); o `433 raw` passou a usar os tempos da buzina (`BUZ_CURTO` 352, `BUZ_LONGO` 920, `BUZ_INTERVALO` 1224) e aceita outros para teste: `433 raw 009F1CAA 352 920`.

| | |
|---|---|
| Quadro | `00 9F 1C [cmd]` (00 9F 1C = identidade deste controle) |

| cmd | Botão |
|---|---|
| `AA` | buzina |
| `CC` | volume − |
| `99` | volume + |
| `55` | liga (alarme de movimento) |
| `33` | desliga (alarme de movimento) |
| `66` | volume + **segurado** (troca o som da buzina, "anterior") |
| `FF` | volume − **segurado** (troca o som da buzina, "próximo") |

Recaptura de 12/10 (2 toques + 2 segurando cada botão): buzina, alarme liga e desliga mandam **o mesmo código** tocando ou segurando; só os de volume mudam quando segurados (`66` e `FF`). Ou seja, o controle dá **7 entradas** para o display: 5 toques + 2 segurados. O ajuste de sensibilidade do alarme não apareceu como código diferente.

✅ **12/10: `433 raw 009F1CAA` (tempos 352/920) fez a buzina tocar de primeira** — o display imita o controle.

✅ **12/10: buzina PAREADA com a identidade do display (`00 9F 1D`).** Como parear: toca no touch da buzina → ela faz **10 bipes** (é o modo de aprendizado!) → enquanto bipa, aperta o botão de buzina do controle desejado (ou `433 raw 009F1DAA` no display). Ela guarda **um controle só**: depois disso **não obedece mais o controle original**. Para voltar ao controle original, é só parear de novo com ele.

**Controle → display (receptor SRX882S no IO3):**

| Botão do controle | Código | Ação no display |
|---|---|---|
| buzina | `AA` | buzina da **roda** |
| volume + | `99` | pisca **esquerda** |
| volume − | `CC` | pisca **direita** |
| cadeado aberto | `33` | buzina **externa** (o display manda `00 9F 1D AA`) |
| cadeado fechado | `55` | **desliga o alarme** da buzina externa (o display manda `00 9F 1D 33`) |
| segurar vol± | `66` / `FF` | livres (aparece `[433 rx] botao XX sem funcao` no Serial) |

**Colisão no ar (12/10):** no 1º teste a buzina externa e os piscas funcionavam "às vezes", mesmo com o display transmitindo toda vez. Motivo: o display respondia **com o dedo ainda no botão**, e o controle seguia transmitindo — dois sinais na mesma frequência ao mesmo tempo se embaralham. Agora as ações que usam o rádio 433 (piscas, buzina externa) **esperam o controle parar**. A buzina da roda (Bluetooth) continua na hora.

O display só age depois de 2 quadros iguais (contra ruído) e **uma vez por aperto**. Ações pelo 433 saem **80 ms depois que o controle para** de transmitir (era 150 ms); o Serial mostra `[433 rx] controle transmitiu X ms` — se X for grande num toque rápido, a demora é do próprio controle. Ele ignora tudo que não for a identidade `00 9F 1C` (piscas, o próprio display transmitindo a buzina externa...). Enquanto o display transmite no 433 (~0,5–0,8 s), o receptor fica "surdo".

Obs.: a sensibilidade do alarme se ajusta nos botões de volume, logo depois do cadeado fechado (antes de ativar).

Curiosidade: cada cmd é um nibble repetido (A·A, C·C, 9·9, 5·5, 3·3). Obs.: o detector automático do `captura_433` confundiu o intervalo de ~1226 µs com o "longo"; a decodificação acima foi feita separando as repetições por esse intervalo.

**Plano de teste do pareamento (12/10) — ✅ feito, deu certo (buzina guarda 1 controle só):**
1. Com o display, mandar o código **original** da buzina: `433 raw 009F1CAA`. Se ela tocar, o display imita o controle certinho (e o `captura_433` no C3 pode conferir que o quadro saiu igual ao do controle).
2. Pôr a buzina em modo de aprendizado (segurar o touch liga/desliga, como no dia em que ela "voltou a funcionar") e mandar uma identidade **nova**: `433 raw 009F1DAA`.
3. Testar os dois: se a buzina passar a obedecer o `9F1D` e **parar** de obedecer o controle original → guarda **1** controle só (provável). Se obedecer os dois → guarda vários (aí precisa descobrir como apagar a memória dela).
4. Se der certo: o controle original vira entrada **só do display** (receptor SRX882S) e o display repassa a buzina com a identidade dele — sem conflito com vol±/alarme.

Por isso a buzina **não obedece mais ao controle original** (só à identidade do display): o controle virou botões do display sem o risco de vol± mexer no volume dela ou o cadeado armar o alarme andando.

### 433 na prática (antenas)

Na bancada tudo funciona; na rua (16/10) falhou às vezes. O que pesa, do mais para o menos importante:
- **Comprimento da antena:** ¼ de onda em 433,92 MHz = **17,3 cm** de fio reto. A molinha de ~20 mm é uma antena "encurtada" (helicoidal): funciona, mas rende bem menos, e ainda menos se ficar perto de metal/placa.
- **Vizinhança:** placa de circuito, tela, bateria e o próprio braço absorvem e desafinam. Antena **para fora** da caixa (ou correndo pela pulseira), longe do plano de terra do display; RX e TX afastados entre si e com as molinhas em direções diferentes.
- **Tensão do transmissor:** o STX882 aceita até ~5 V e transmite mais forte quanto maior a tensão (o DATA em 3,3 V continua valendo). Antes de mudar: medir se o 5 V do conector UART existe **rodando na bateria** (pode só existir com USB).
- **Termo-retrátil por cima:** efeito desprezível em 433 MHz. Não é o culpado.
- **Software (se o hardware não bastar):** mais repetições por envio (hoje 12 nos piscas), à custa de mais tempo no ar.
- Diagnóstico: o Serial mostra `[433 rx] controle transmitiu X ms` para cada aperto recebido; os que não aparecem não chegaram.
- **Teste de 18/10:** afastar o transmissor da placa fez **muita diferença**. Ligação da molinha por um cabinho blindado (rabicho de antena Wi-Fi de notebook: vivo no ANT, malha no GND do lado do módulo) funcionou bem. Falta achar o lugar na caixa — ideia: um "cantinho da antena" sem metal numa ponta da caixa (é o que os fabricantes fazem).

**Medindo o alcance (`medidor_433`, 22/10):** o SRX882S não tem pino de RSSI, então a medida é a **taxa de acerto**. No display, `433 teste` (pode tirar o USB depois); ele manda uma rajada de 12 repetições a cada ~0,8 s, com a identidade `00 9F 1E` (ninguém obedece) e um número de sequência. O C3 conta quantas das 12 chegam e mostra no LCD: `Q:` = média das últimas 10 rajadas (%), `ult` = a última rajada, `sum` = rajadas que sumiram inteiras. Uso: perto tudo dá 100%; ponha o medidor **onde fica a lanterna** (ou mais longe, até cair para uns 50–80%), **não mexa mais nele** e vá mudando só a antena do display, esperando ~10 s a cada mudança.

---

## 10. Lições aprendidas (as pegadinhas)

**Arduino / C++**
- O Arduino gera protótipos de todas as funções antes da primeira função do arquivo: funções que usam `enum`/`struct` próprios precisam ficar **depois** deles (ou receber `int`).
- Evite argumentos padrão em funções do sketch (mesmo motivo).
- `min()`/`max()` exigem os dois argumentos do mesmo tipo.
- `volatile` não combina com `min`/`max`: copie para uma variável local.
- **Dois núcleos:** os callbacks da NimBLE rodam num núcleo e o `loop()` no outro. Para tempos, copie o valor escrito pelo callback **antes** de ler o `millis()` (senão a subtração dá a volta) — use `msSemDados()` para "há quanto tempo sem dados da roda".
- **Conexão BLE bloqueante trava o `loop()`** inteiro (freio, buzina, touch): só fazer parado.

**NimBLE 2.5.1**
- Não dá para montar o servidor com conexão aberta (`assert ble_svc_gap_init`).
- `getServices(true)` apaga os objetos antigos: ponteiros guardados viram lixo.
- Descoberta de descritores devolve `0x2803` (declarações) por engano: clonar só `0x2901`.
- **Bug da biblioteca:** arrays indexados pelo número da conexão estouravam com handle ≥ 4 → reinício ao desconectar. Corrigido no patch.
- O nº de "atividades" do rádio vem de `CONFIG_BT_NIMBLE_MAX_CONNECTIONS`; com anúncio estendido, `advertiseOnDisconnect` não existe.
- No pareamento, não entregar a identidade **do display** (senão o Android junta as duas identidades), mas aceitar a **do celular** (senão cada reconexão/reboot pede novo pareamento). **Pegadinha:** na NimBLE, `setSecurityInitKey` = chaves que o **display** distribui (`our`) e `setSecurityRespKey` = chaves que o **celular** distribui (`their`) — o contrário do que o nome sugere.
- Guardar a IRK do celular com a privacidade **no controlador** (padrão no S3) liga a resolução de endereço no rádio e derruba o clone da roda (apps não conectam). **Solução (15/10):** privacidade **no host** (`CONFIG_BT_NIMBLE_HOST_BASED_PRIVACY 1`): a NimBLE resolve o endereço do celular em software, guarda os "registros de identidade" na flash e o controlador fica de fora. Ver [Controle de mídia](#controle-de-mídia-v5-midia).

**Pareamento — tiques cosméticos:** o primeiro toque pra parear o controle às vezes falha e só o segundo oferece parear; o controle pode aparecer no Android com um nome tipo "82CE"/ícone de controle. Ambos inofensivos.

**Pareamento — faxina (sempre que mexer em bond/segurança):** `esquecer` no Serial; no celular, esquecer **TODAS** as entradas do display (inclusive uma possível **KSN-S16P...**, que é o resíduo que trava os apps); liga/desliga o Bluetooth do celular. Pular isso contamina o teste: um bond velho grudado no endereço da roda faz os apps conectarem e caírem no aperto de mão (nada aparece no Serial do display).

**Android**
- Guarda a lista de serviços em cache: desligar/ligar o Bluetooth (não pareado) ou esquecer e parear de novo (pareado).
- Às vezes coloca vários apps na **mesma** conexão.
- O nRF Connect em segundo plano segura conexões.

**Qual versão está no display?**
- O Arduino IDE pode **não recarregar** um `.ino` alterado por fora enquanto está aberto — compila a versão velha sem avisar. Antes de compilar, fechar e reabrir o sketch, e depois de gravar conferir a versão no rodapé da cortina (12/10: "o vol+ continua invertido" era o display rodando a versão anterior). 14/10: depois de um Salvar no IDE, o arquivo ficou com o código novo mas o **rótulo** da versão antigo (o editor desfez só aquelas linhas). Quando o IDE avisar que o arquivo mudou no disco, escolher **recarregar**, e evitar Ctrl+Z / Salvar logo depois.

**Rádio 433: um de cada vez no ar**
- Se o display transmitir enquanto o controle ainda está transmitindo (dedo no botão), os sinais se embaralham. Resposta via 433 só depois que o controle cala.

**RMT (433 MHz)**
- `rmtTransmitCompleted()` pode ficar preso em "ocupado" após um envio assíncrono e travar TODOS os envios seguintes. Não dependa dele: espace os envios por **tempo** (`millis()`), calculado pela duração da rajada. Fila de 1 posição (último comando ganha) cobre cliques rápidos.

**Placa**
- Touch girado 180° em relação à imagem (já compensado na config: `INVERTER_TOUCH_X/Y`). Para girar a tela inteira use `TELA_GIRADA`: a LovyanGFX gira o touch junto com o `setRotation`.
- Sem `USB CDC On Boot` o Serial fica mudo.
- Sem PSRAM, os sprites não cabem.

- **I2C com um dono só**: a LovyanGFX controla a porta I2C 0 (touch) direto nos registradores. Outros chips no mesmo barramento (MPU, ES8311, RTC) usam `lgfx::i2c::readRegister`/`writeRegister8` — **não** a `Wire`. Se um dia precisar da `Wire`, tem que ser a `Wire1` (porta 1) em outros pinos. E todo acesso I2C fica na mesma tarefa (`loop()`).
- **Endereços I2C** são de 7 bits e fixos de fábrica. DS3231 (RTC) também é 0x68: se entrar, o MPU vai para 0x69 (AD0 no 3.3V). Datasheet com 0xD0/0xD1 = o mesmo 0x68 com o bit de leitura/escrita junto.

---

## 11. Roadmap

### Agora → ver [Foco atual](#1-foco-atual)

### Próximo
- [ ] **Funções para segurar vol± no controle** (`66` / `FF`, hoje livres).
- [ ] **Alarmes**: PWM alto (som/tela piscando); bateria cheia + regeneração forte.
- [ ] **Setas com desligamento automático** (piscas que apagam sozinhos depois da curva).
- [ ] Bateria: conferir a leitura com o multímetro e montar a célula definitiva.

### Feito recentemente
- [x] Bateria do display: leitura no IO9, ícone, % nos Ajustes, alarme abaixo de 3,35 V, "carregando" (12–13/10).
- [x] Energia: dorme sozinho sem roda/USB ou com a bateria no fim, botão Dormir, acorda no BOOT ou mexendo (13–14/10).
- [x] Tela da mochila + conectar pela cortina (14/10).
- [x] Controle da buzina como botões do display + buzina externa pareada (12/10).
- [x] Luz de freio (14/10) — falta ajustar os limites andando.
- [x] S16 Controle sem repareamento (PAREAMENTO_V2, 15/10) — em observação.

### Depois
- [ ] Teste de rua longo (alcance, reconexão, estabilidade).
- [ ] Caixa para a luva.
- [ ] Mochila 2: fita endereçável + C3 com firmware próprio (FastLED + BLE compatível com o Happy Lighting).
- [ ] Commit/tag de cada versão estável.

---

## 12. Ideias guardadas

Registradas para não se perderem — **não** são compromisso.

**Display**
- ~~Tela acender por inclinação do braço~~ → **feito** (MPU, `Tela auto`).
- Rotação automática da tela; detecção de queda.
- Motor de vibração para alertas (sente no pulso).
- Alto-falante de notebook (4–8 Ω) para bipes mais altos que o piezo.
- Teclado/tela de senha para destravar a roda (precisa decifrar o desafio-resposta).
- ~~Painel de Ajustes "estilo Android" e print da tela~~ → **feitos em 12/10**; botão **Dormir** na cortina → **feito em 14/10**.
- ~~Botões do polegar sem fio~~ → **feito em 12/10** com o controle da buzina e o receptor no IO3 (ver seção 9).
- **Dormir pelo controle** (ex.: segurar um botão): os segurados `66`/`FF` estão livres. Lembrar que dormir derruba a ponte BLE (os apps perdem a roda).

**Celular**
- Notificações no display (Gadgetbridge como Bangle.js, Chronos, ou app próprio).
- "Tocando agora" (música atual) pelo mesmo caminho.
- Navegação curva a curva (Chronos + Google Maps).

**Dados**
- Mesclar o CSV do display com o do EUC World pela hora (EUC Viewer).
- Script Python para gráficos das viagens.
- Log cru de todos os frames no SD, para engenharia reversa em movimento.
- Descobrir o CRC do formato estendido (temos vários `F9` com CRC para testar).
- Explorar configurações da roda (tiltback, alarmes) e as cores dos LEDs (`59`).
- Baixar os resumos de sessão da roda ("Ridinglogging") pela ponte e gravar no SD com as datas certas.
- **WheelLog na v5 MIDIA:** ver no código do WheelLog (aberto) exatamente como ele compara os serviços (quantidade? lista exata? só os da marca?) e tentar contornar, para ter controle de mídia e WheelLog na mesma versão.

**Hardware**
- GPS (módulo de drone M10, ex. HGLRC M100 Mini) na UART — também dá hora exata.
- Placa futura com tudo embutido: Waveshare ESP32-S3-Touch-LCD-2.8 (IMU + RTC) ou LilyGO T-Watch Ultra.
- Programar pelo tablet: code-server + arduino-cli no servidor de casa + OTA.
- Projeto paralelo: corrigir a exportação de vídeo do EUC Viewer no Firefox.

---

## 13. Ferramentas e técnicas de investigação

- **Revisão + compilação com o Claude Code** (17/10): aberto na pasta do repo, ele compila com o `arduino-cli` (core 3.3.12 e as opções do menu Ferramentas) e revisa o código sem alterar nada. A 1ª revisão achou a mochila travando o loop andando, a disputa entre núcleos no "tempo sem dados", a gravação dividida por queda curta do BLE e a sobra de linha no console — corrigidos na v5-202610171238.

- **Serial da ponte** (`DUMP_PHONE`, `DUMP_NOVOS`): mostra cada comando que um app manda para a roda e as respostas diferentes. Mudar **uma configuração por vez** no app.
- **nRF Connect** (Android): ver serviços/características e mandar bytes na mão.
- **Registro HCI do Android:** Opções do desenvolvedor → "Registro de snoop HCI Bluetooth" = **Ativado** → desligar/ligar o Bluetooth → fazer as ações com pausas de 5 s, anotando a ordem → gerar o relatório de bugs **com o snoop ainda ligado** → arquivo `btsnoop_hci.log` dentro do zip.
- **Visão geral do repositório:** [GitDiagram](https://gitdiagram.com/leandrosly/s16-companion) (diagrama interativo da estrutura do projeto) e [Gitingest](https://gitingest.com/leandrosly/s16-companion) (o repositório inteiro num texto só, bom para colar numa IA).
- **Wireshark:** filtro `(btatt.opcode == 0x52 || btatt.opcode == 0x12) && bluetooth.dst == <MAC>` (escritas); `btatt.opcode == 0x1b` (notificações). "Apply as Column" no campo Value e exportar como CSV. **Compactar binários em .zip** antes de enviar.

---

## 14. Referências

- [WheelLog](https://github.com/Wheellog/Wheellog.Android) — base do protocolo KingSong.
- [EUCSpeedo](https://github.com/ihatechoosingusernames/EUCSpeedo) — display ESP32 para rodas.
- [EUC World](https://euc.world) · [Mono Riders](https://app.monoriders.com.br) · [EUC Viewer](https://eucviewer.ried.no/)
- [LCDWIKI — 2.8inch ESP32-S3 Display](https://www.lcdwiki.com/2.8inch_ESP32-S3_Display)
- [GitDiagram do projeto](https://gitdiagram.com/leandrosly/s16-companion) · [Gitingest do projeto](https://gitingest.com/leandrosly/s16-companion)
- [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) · [LovyanGFX](https://github.com/lovyan03/LovyanGFX)
