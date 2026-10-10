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
| 1 | **Mochila iFlight (módulo Dream)** | Controlar pelo display: ligar/desligar, cor e efeitos (`teste_mochila`). Depois: tela da mochila integrada. |
| 2 | **Luz de freio** | Acender o vermelho (mochila / pisca 433) quando a corrente fica negativa na frenagem (limite ~−2 A, com histerese). |
| 3 | **Integração na ponte v5** | Juntar MPU, piezo e 433 (já provados em teste) como funções da ponte. |

**Concluídos** (viram funções na integração): ✅ MPU (`teste_tela_mpu`), ✅ Piezo (`teste_piezo`), ✅ Piscas 433 MHz (`captura_433` + `teste_tx433`).

---

## 2. Estado do projeto

### Funcionando ✅

- Ponte BLE estável: o display conecta na roda e se apresenta aos celulares com o mesmo nome e os mesmos serviços (clone completo da GATT).
- **EUC World, Mono Riders, WheelLog e app da KingSong** funcionando pela ponte, com **2 apps garantidos** (já rodaram os 4 juntos na v5 2APPS, com o Android dividindo conexões).
- **WheelLog só funciona na v5 2APPS:** ele exige a lista de serviços idêntica à da roda, e a v5 MIDIA tem 2 serviços a mais (HID `1812` e bateria `180F`).
- **Controle de mídia** (v5 MIDIA): música e volume do celular direto pelo display, numa identidade Bluetooth separada ("S16 Controle").
- 3 telas por deslize: **BMS** ← **principal** → **controles**.
- **MPU na ponte (parte 1):** a tela acende/apaga pela posição do pulso. Começa desligado; ative pelo Serial: `mpu on` depois `mpu gravar` (na posição de olhar). Comandos: `mpu` (status), `mpu on`/`mpu off`, `mpu gravar`. O deep sleep ("roda sumiu") fica para o marco de energia. Tocar na tela sempre acende.
- **Som na ponte (parte 2):** codec ES8311 (config por I2C no `loop`/`setup`) + I2S numa **tarefa separada** (só mexe no I2S, não briga com o touch/MPU). Bipe de confirmação ao apertar a buzina (BOOT). Comandos Serial: `som bipe`, `som alarme`, `som on`/`som off`. `SOM_ON` liga/desliga (salvo). Frequência fixa em 4 kHz (ressonância do piezo de 35 mm). Alarmes de PWM entram depois, usando `somAlarme()`.
- Buzina no botão BOOT e na tela; farol (liga/desliga/auto), LEDs, volume da roda.
- Logs no cartão SD: eventos sempre, viagem sob demanda (5 linhas/s).
- Relógio acertado pela própria roda ao conectar.
- Configuração pelo Serial, salva na flash.

### Em andamento 🔧

- Protocolo do módulo de LEDs Dream: comandos decifrados, falta a resposta de estado e o número de efeitos.

### Problemas conhecidos ⚠️

- **Um 3º app** às vezes funciona porque o Android coloca dois apps na mesma conexão, mas pode cair em cascata. Garantido: 2 apps.
- **Controle de mídia repareia a cada reboot** e às vezes aparece como "KSN-S16P--4C4B": limitação conhecida, **em aberto**. A tentação é pedir a IRK do celular (`setSecurityRespKey(ENC|ID)`) — isso conserta o re-pareamento, **mas liga a resolução de endereço no controlador e quebra as conexões dos apps na identidade da roda** (testado: no nRF a aba da roda conecta e fica com GATT vazio). Então ficamos com `setSecurityInitKey(ENC|ID)` + `setSecurityRespKey(ENC)`: apps funcionam, controle repareia. Conserto futuro precisa reconhecer o celular SEM ligar a resolução global (ex.: controle sem bonding, ou resolução por software). Diagnóstico: `bonds` no Serial, `[bond] restaurados no boot: N`.
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
| Bateria (ADC) | 9 |
| LED RGB (WS2812) | 42 |
| Botão BOOT | 0 (usado como buzina) |
| USB nativo | 19, 20 |

**Conectores (todos 1,25 mm):**

| Conector | Pinos | Plano de uso |
|---|---|---|
| I2C | 3.3V, GND, IO15 (SCL), IO16 (SDA) | **MPU-6050 (0x68)** no mesmo barramento do touch (0x38) e do codec ES8311 (0x18), usando as funções `lgfx::i2c` da LovyanGFX (testado: 0 erros com o touch em uso) |
| GPIO | IO21, IO14, IO3, IO2 | IO14 = INT do MPU (acordar do deep sleep); IO2 = pisca esquerda; IO3 = pisca direita (botões ao GND); IO21 = STX882 (433 MHz) |
| UART | 5V, GND, TXD (IO44), RXD (IO43) | livre: 5 V para receptor 433 MHz, ou UART para GPS |
| BAT | + / − | LiPo 3,7 V (**conferir a polaridade antes de ligar!**) |
| SPEAKER | 2 pinos | piezo passivo + 100 Ω em série (saída em ponte: nenhum dos pinos é GND) |
| BOOT (IO0) | soldar nos pads do botão | botão da buzina em paralelo (não segurar ao ligar a placa) |

Não há outros pinos livres no chip. Para mais portas: **MCP23017** (16 GPIO por I2C).

### Outros

- **2× ESP32-C3 Supermini**: protótipos (ponte v3 com LCD 16×2, captura de 433 MHz) e, no futuro, controlador da fita de LED da mochila.
- **Módulo de LEDs iFlight "Dream#0E4D"**: mochila dos drones, controlado pelo app Happy Lighting.
- **Receptor/transmissor 433 MHz**: FS1000A + MX-RM-5V (gaveta); STX882 + SRX882 (melhor, a comprar).
- **MPU-6050** (GY-521).
- Controle dos piscas sem fio: 433,92 MHz (cristal de 13,56 MHz × 32).

---

## 4. Como compilar

### Arduino IDE

- Core **esp32 by Espressif 3.3.12**.
- Bibliotecas:
  - **NimBLE-Arduino 2.5.1** — **COM O PATCH** da pasta `patch_nimble_2.5.1/` (ver o README de lá). Sem o patch: o EUC World recusa a ponte, a placa reinicia ao desconectar apps e os anúncios são recusados.
  - **LovyanGFX** (lovyan03).
  - `SD_MMC` e `Preferences` já vêm com o core.

> ⚠️ **Não atualize a NimBLE-Arduino** sem refazer o patch. Ao trocar de PC: instale a 2.5.1 e copie os 5 arquivos do patch.

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
| `ks_s16_ponte_v3.ino` | Versão antiga para o ESP32-C3 com LCD 16×2 |
| `galeria_fotos/` | Exemplo de aprendizado: tela + touch + sprites + zoom (+ script `converter_fotos.py`) |
| `captura_433/` | Sketch para descobrir os códigos do controle dos piscas (ESP32-C3) |
| `teste_i2c_mpu/` | Teste do MPU-6050 dividindo o I2C com o touch (Plano A LovyanGFX / Plano B Wire1) |
| `teste_tx433/` | Display imitando o controle dos piscas (STX882 no IO21, RMT) |
| `teste_mochila/` | Display como cliente BLE do módulo Dream: liga/cor/efeito/handshake |
| `teste_piezo/` | Teste do piezo no SPEAKER: ES8311 + I2S, varredura para achar a ressonância, bipe/pisca/alarme |
| `teste_tela_mpu/` | Teste: acender/apagar a tela pela posição do pulso + deep sleep parado (acorda pelo BOOT ou INT do MPU) |

---

## 6. Usando o display

### Telas (deslizar o dedo)

```
[ BMS ]  ←  [ PRINCIPAL ]  →  [ CONTROLES ]
```

- **Principal:** velocidade (cor pelo PWM: verde < 70% < amarelo < 85% < vermelho), barra de PWM, tensão, corrente, temperaturas, trip, máxima e limite.
- **BMS:** os 2 packs com tensão, SoC, temperatura, célula mínima e máxima, diferença em mV e gráfico das 20 células (escala ampliada).
- **Controles (v5 MIDIA):**

| | | |
|---|---|---|
| 📱− volume celular | 📱+ volume celular | |
| ⏮ anterior | ⏯ play/pause | ⏭ próxima |
| 🔈− volume roda | 🔊+ volume roda | 📄 gravar log |
| ☀ farol (liga → desliga → auto) | 🔴🟢🔵 LEDs (liga/desliga) | 📢 buzina |

### Barra superior

`RODA` (verde/vermelho) · `APP` (ciano com app conectado) · ♪ (magenta com o controle de mídia ativo) · ● amarela (buzina) · `FAN` · bolinhas da tela atual · ● vermelha (gravando) · hora.

### Controle de mídia (v5 MIDIA)

1. Pareie **"S16 Controle"** nas configurações de Bluetooth do celular (uma vez).
2. Os apps continuam conectando em **"KSN-S16P--4C4B"**, sem pareamento.
3. Problema? Esqueça os dois no celular, digite `esquecer` no Serial e pareie de novo.

### Logs no cartão (`/logs`)

- `eventos.csv`: sempre ativo. Display ligado, roda e apps conectando/desconectando (com motivo), cada toque no display, relógio acertado.
- `viagem_AAAAMMDD_HHMMSS.csv`: velocidade, PWM, corrente e tensão, 5×/s, ligado no ícone 📄.

### Console Serial (115200, "Nova linha")

| Comando | Efeito |
|---|---|
| `config` | mostra a configuração |
| `config padrao` | volta aos valores de fábrica |
| `DUMP_PHONE = false` | (e `DUMP_HEX`, `DUMP_NOVOS`, `DUMP_OTHER`, `IGNORA_CANCELAR`) liga/desliga; fica salvo na flash |
| `NAME_FILTER = KS` / `TARGET_MAC = c5:39:...` / `TARGET_MAC = -` | como achar a roda (vale ao reiniciar) |
| `t 95 6 FF` | envia o comando `95` com o byte 6 = `FF` (experimentos) |
| `raw AA 55 ... 5A 5A` | envia 20 bytes exatos (ex.: um frame capturado) |
| `esquecer` | apaga os pareamentos (v5 MIDIA) |

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
- **Duas identidades** (anúncio estendido): a roda (sem pareamento) e o "S16 Controle" (pareado, endereço próprio). Assim o pareamento não "contamina" a conexão dos apps.
- Cada notificação é enviada **por conexão** para quem assinou; o cancelamento da FFE1 é ignorado (`IGNORA_CANCELAR`) porque o Android às vezes coloca dois apps na mesma conexão.
- **Orçamento do rádio:** com anúncio estendido, conexões + anúncios ≤ `MAX_CONNECTIONS − 1`. Com `MAX_CONNECTIONS 6`: roda + 2 apps + controle + anúncio. A mochila (Dream) vai precisar de mais uma vaga.

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
| `F5` | 14 carga de CPU? · **15 PWM (%)** |
| `F6` | 2–3 limite de velocidade (/100; começa 20, vira 32) · 14 flags de alarme? (`DA` com PWM alto) |
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

---

## 10. Lições aprendidas (as pegadinhas)

**Arduino / C++**
- O Arduino gera protótipos de todas as funções antes da primeira função do arquivo: funções que usam `enum`/`struct` próprios precisam ficar **depois** deles (ou receber `int`).
- Evite argumentos padrão em funções do sketch (mesmo motivo).
- `min()`/`max()` exigem os dois argumentos do mesmo tipo.
- `volatile` não combina com `min`/`max`: copie para uma variável local.

**NimBLE 2.5.1**
- Não dá para montar o servidor com conexão aberta (`assert ble_svc_gap_init`).
- `getServices(true)` apaga os objetos antigos: ponteiros guardados viram lixo.
- Descoberta de descritores devolve `0x2803` (declarações) por engano: clonar só `0x2901`.
- **Bug da biblioteca:** arrays indexados pelo número da conexão estouravam com handle ≥ 4 → reinício ao desconectar. Corrigido no patch.
- O nº de "atividades" do rádio vem de `CONFIG_BT_NIMBLE_MAX_CONNECTIONS`; com anúncio estendido, `advertiseOnDisconnect` não existe.
- No pareamento, não entregar a identidade **do display** (senão o Android junta as duas identidades), mas aceitar a **do celular** (senão cada reconexão/reboot pede novo pareamento). **Pegadinha:** na NimBLE, `setSecurityInitKey` = chaves que o **display** distribui (`our`) e `setSecurityRespKey` = chaves que o **celular** distribui (`their`) — o contrário do que o nome sugere. **MAS** pedir a IRK do celular liga a resolução de endereço e derruba o clone da roda (apps não conectam). Conflito real: ficamos com `setSecurityInitKey(ENC|ID)` + `setSecurityRespKey(ENC)` (apps OK, controle repareia). Diagnóstico: `bonds` no Serial e `[bond] restaurados no boot: N`.

**Android**
- Guarda a lista de serviços em cache: desligar/ligar o Bluetooth (não pareado) ou esquecer e parear de novo (pareado).
- Às vezes coloca vários apps na **mesma** conexão.
- O nRF Connect em segundo plano segura conexões.

**Placa**
- Touch girado 180° em relação à imagem.
- Sem `USB CDC On Boot` o Serial fica mudo.
- Sem PSRAM, os sprites não cabem.

- **I2C com um dono só**: a LovyanGFX controla a porta I2C 0 (touch) direto nos registradores. Outros chips no mesmo barramento (MPU, ES8311, RTC) usam `lgfx::i2c::readRegister`/`writeRegister8` — **não** a `Wire`. Se um dia precisar da `Wire`, tem que ser a `Wire1` (porta 1) em outros pinos. E todo acesso I2C fica na mesma tarefa (`loop()`).
- **Endereços I2C** são de 7 bits e fixos de fábrica. DS3231 (RTC) também é 0x68: se entrar, o MPU vai para 0x69 (AD0 no 3.3V). Datasheet com 0xD0/0xD1 = o mesmo 0x68 com o bit de leitura/escrita junto.

---

## 11. Roadmap

### Agora → ver [Foco atual](#1-foco-atual)

### Próximo
- [ ] **Tela da mochila** no display (liga/desliga, paleta, brilho, efeitos) → frente ativa nº 1.
- [ ] **Luz de freio** pela corrente negativa (limite ~−2 A, com histerese) → frente ativa nº 2.
- [ ] **Setas** por botão (mochila + piscas 433 MHz), com desligamento automático.
- [ ] **Bateria do display**: LiPo + leitura no IO9 + ícone na tela.
- [ ] **Energia**: chave física + sono automático quando a roda some (acordar pelo botão ou pelo INT do MPU).
- [ ] **Alarmes**: PWM alto (som/tela piscando); bateria cheia + regeneração forte.
- [ ] Botão externo de buzina no IO3.

### Depois
- [ ] Teste de rua longo (alcance, reconexão, estabilidade).
- [ ] Caixa para a luva.
- [ ] Mochila 2: fita endereçável + C3 com firmware próprio (FastLED + BLE compatível com o Happy Lighting).
- [ ] Commit/tag de cada versão estável.

---

## 12. Ideias guardadas

Registradas para não se perderem — **não** são compromisso.

**Display**
- Tela acender por inclinação do braço (MPU, com histerese contra vibração).
- Rotação automática da tela; detecção de queda.
- Motor de vibração para alertas (sente no pulso).
- Alto-falante de notebook (4–8 Ω) para bipes.
- Teclado/tela de senha para destravar a roda (precisa decifrar o desafio-resposta).

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

- **Serial da ponte** (`DUMP_PHONE`, `DUMP_NOVOS`): mostra cada comando que um app manda para a roda e as respostas diferentes. Mudar **uma configuração por vez** no app.
- **nRF Connect** (Android): ver serviços/características e mandar bytes na mão.
- **Registro HCI do Android:** Opções do desenvolvedor → "Registro de snoop HCI Bluetooth" = **Ativado** → desligar/ligar o Bluetooth → fazer as ações com pausas de 5 s, anotando a ordem → gerar o relatório de bugs **com o snoop ainda ligado** → arquivo `btsnoop_hci.log` dentro do zip.
- **Wireshark:** filtro `(btatt.opcode == 0x52 || btatt.opcode == 0x12) && bluetooth.dst == <MAC>` (escritas); `btatt.opcode == 0x1b` (notificações). "Apply as Column" no campo Value e exportar como CSV. **Compactar binários em .zip** antes de enviar.

---

## 14. Referências

- [WheelLog](https://github.com/Wheellog/Wheellog.Android) — base do protocolo KingSong.
- [EUCSpeedo](https://github.com/ihatechoosingusernames/EUCSpeedo) — display ESP32 para rodas.
- [EUC World](https://euc.world) · [Mono Riders](https://app.monoriders.com.br) · [EUC Viewer](https://eucviewer.ried.no/)
- [LCDWIKI — 2.8inch ESP32-S3 Display](https://www.lcdwiki.com/2.8inch_ESP32-S3_Display)
- [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) · [LovyanGFX](https://github.com/lovyan03/LovyanGFX)
