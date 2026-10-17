# Inventário de imports LBP3

ELF SHA-256: `c5387ed91227a406e230351539107e81ff507f84f230df8db79d4d2438b7b86e`

600 símbolos importados; 49 módulos; 50 bibliotecas.

| Biblioteca | Símbolos importados |
|---|---:|
| libc | 130 |
| libkernel | 72 |
| libSceGnmDriver | 45 |
| libSceHttp | 38 |
| libSceFios2 | 26 |
| libSceNet | 24 |
| libSceNpManager | 24 |
| libSceNpWebApi | 20 |
| libScePosix | 17 |
| libSceAvPlayer | 15 |
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
| FMODExtInput | 1 |
| FMODSmsReverb | 1 |
| FMODSmsWaveHammer | 1 |
| FMODVoIPMixer | 1 |
| libSceCommonDialog | 1 |
| libSceNpCommon | 1 |
| spu | 1 |

## Módulos declarados

- libSceSystemService (ID B)
- spu (ID C)
- FMODExtInput (ID D)
- FMODSmsReverb (ID E)
- FMODSmsWaveHammer (ID F)
- FMODVoIPMixer (ID G)
- libSceFiber (ID H)
- libSceFios2 (ID I)
- libSceSsl (ID J)
- libSceHttp (ID K)
- libSceNet (ID L)
- libSceNetCtl (ID M)
- libSceIme (ID N)
- libSceImeDialog (ID O)
- libSceSysmodule (ID P)
- libScePad (ID Q)
- libSceUserService (ID R)
- libSceAudioOut (ID S)
- libSceNpCommon (ID T)
- libSceNpManager (ID U)
- libSceNpAuth (ID V)
- libSceVideoOut (ID W)
- libSceGnmDriver (ID X)
- libkernel (ID Y)
- libSceMove (ID Z)
- libSceAudioIn (ID a)
- libSceCamera (ID b)
- libSceMoveTracker (ID c)
- libSceRtc (ID d)
- libSceNpUtility (ID e)
- libSceNpWebApi (ID f)
- libSceNpCommerce (ID g)
- libSceCommonDialog (ID h)
- libSceErrorDialog (ID i)
- libSceMsgDialog (ID j)
- libGameCustomDataDialog (ID k)
- libSceInvitationDialog (ID l)
- libSceNpSignaling (ID m)
- libSceSaveData (ID n)
- libSceNpTrophy (ID o)
- libSceAppContentUtil (ID p)
- libSceAvPlayer (ID q)
- libScePlayGo (ID r)
- libSceGameLiveStreaming (ID s)
- libSceContentSearch (ID t)
- libSceJpegDec (ID u)
- libSceCompanionHttpd (ID v)
- libSceSaveDataDialog (ID w)
- libc (ID x)

## Relocations por tipo

```json
{
  "1": 494,
  "6": 14,
  "7": 536,
  "8": 29937
}
```

## Limites

Lista estática dos imports diretos deste ELF. Não inclui dependências transitivas nem bibliotecas carregadas durante a execução.
As funções estão identificadas pelos NIDs originais em imports.csv. Os nomes legíveis ainda não foram resolvidos.
A contagem não indica ordem de inicialização nem prioridade de implementação.

Referências de formato: https://github.com/shadps4-emu/shadPS4/blob/main/src/core/loader/elf.h e https://github.com/shadps4-emu/shadPS4/blob/main/src/core/module.cpp
