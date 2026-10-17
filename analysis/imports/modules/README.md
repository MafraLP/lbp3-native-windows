# Módulos LBP3

Preparação e bindings estáticos; não são funções já ligadas em memória.

| Arquivo | Imports | Exports | Candidatos guest | Externos sem binding guest |
|---|---:|---:|---:|---:|
| eboot.elf | 600 | 0 | 161 | 439 |
| fmodextinput.elf | 11 | 1 | 7 | 4 |
| fmodsmsreverb.elf | 10 | 1 | 6 | 4 |
| fmodsmswavehammer.elf | 12 | 1 | 8 | 4 |
| fmodvoipmixer.elf | 5 | 1 | 3 | 2 |
| spu.elf | 20 | 1 | 11 | 9 |
| libc.elf | 74 | 2095 | 8 | 66 |
| libSceFios2.elf | 81 | 165 | 0 | 81 |

Dos 609 imports sem binding guest, 448 têm candidato estático no ISS, 41 apenas nome e 120 nenhuma correspondência.

## Dependências externas por biblioteca

| Biblioteca | Imports |
|---|---:|
| libkernel | 211 |
| libSceGnmDriver | 45 |
| libSceHttp | 38 |
| libSceNet | 24 |
| libSceNpManager | 24 |
| libSceNpWebApi | 20 |
| libScePosix | 17 |
| libSceLibcInternal | 16 |
| libSceAvPlayer | 15 |
| (símbolo sem identidade de biblioteca) | 13 |
| libSceGameLiveStreaming | 10 |
| libSceCamera | 9 |
| libSceNpUtility | 8 |
| libScePlayGo | 8 |
| libSceRtc | 8 |
| libSceSaveData | 8 |
| libSceSaveDataDialog | 8 |
| libSceVideoOut | 8 |
| libSceAppContent | 7 |
| libSceCompanionHttpd | 7 |
| libSceFiber | 7 |
| libSceNpCommerce | 7 |
| libSceNpSignaling | 7 |
| libSceNpTrophy | 7 |
| libScePad | 7 |
| libSceMoveTracker | 6 |
| libSceNetCtl | 6 |
| libSceUserService | 6 |
| libSceAudioOut | 5 |
| libSceContentSearch | 5 |
| libSceImeDialog | 5 |
| libSceJpegDec | 5 |
| libSceMove | 5 |
| libSceSystemService | 5 |
| libSceErrorDialog | 4 |
| libSceMsgDialog | 4 |
| libSceNpAuth | 3 |
| libSceSsl | 3 |
| libSceAudioIn | 2 |
| libSceSysmodule | 2 |
| libSceDiscMap | 2 |
| libSceCommonDialog | 1 |
| libSceNpCommon | 1 |

## Limites

Os candidatos exigem igualdade de NID, biblioteca, módulo e versões. Não foram aplicadas relocations ou chamadas entradas de módulos.
Os estados ISS são somente evidências estáticas do relatório anterior e não comprovam implementação compatível.
modules.json registra SHA-256 da origem, do ELF e dos segmentos copiados por índice SELF. Os originais são apenas lidos.
A ordem de inicialização e o carregamento dinâmico ainda não foram determinados.
