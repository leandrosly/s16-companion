# Patch da NimBLE-Arduino 2.5.1 para a ponte do S16

Estes arquivos substituem os originais da biblioteca **NimBLE-Arduino 2.5.1**.
Só servem para a 2.5.1: se a biblioteca for atualizada, as mudanças precisam ser refeitas na versão nova.

Pasta da biblioteca: `Documents\Arduino\libraries\NimBLE-Arduino\src\`

| Arquivo | Destino (dentro de `src\`) | Por quê |
|---|---|---|
| `nimconfig.h` | `nimconfig.h` | 4 conexões (roda + 2 apps + controle de mídia) e anúncio estendido com 2 identidades |
| `ble_svc_gap.c` | `nimble\nimble\host\services\gap\src\` | Generic Access igual ao da roda (2A00/2A01 com escrita, 2A04, 2AC9) — o EUC World confere |
| `ble_svc_gatt.c` | `nimble\nimble\host\services\gatt\src\` | Generic Attribute igual ao da roda (só 2A05, com Read) |
| `ble_gap.c` | `nimble\nimble\host\src\` | Corrige bug: arrays indexados pelo número da conexão estouravam com handle ≥ 4 (reinícios ao desconectar) |
| `ble_hs_hci_evt.c` | `nimble\nimble\host\src\` | Mesma correção (declaração externa de um dos arrays) |

Todas as mudanças estão marcadas com `PATCH S16` no código.

## Ao trocar de PC ou atualizar a biblioteca

1. Instale a NimBLE-Arduino **2.5.1** (Gerenciador de Bibliotecas → escolher a versão).
2. Copie estes 5 arquivos para os destinos acima.
3. Sintoma de patch faltando: EUC World recusa a ponte (GAP/GATT), reinícios ao desconectar apps (ble_gap / ble_hs_hci_evt) ou erro de compilação em `NimBLEExtAdvertising` (nimconfig.h).
