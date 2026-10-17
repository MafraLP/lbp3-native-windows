# Pendências nativas de ligação

356 registros pendentes.

| Motivo | Registros |
|---|---:|
| unresolved_symbol | 356 |

## Símbolos não resolvidos por biblioteca

| Biblioteca | Referências |
|---|---:|
| libSceHttp | 38 |
| libkernel | 28 |
| libSceNet | 24 |
| libSceNpManager | 23 |
| libSceNpWebApi | 20 |
| libSceAvPlayer | 15 |
| libScePosix | 15 |
| libSceLibcInternal | 15 |
| libSceGnmDriver | 12 |
| libSceGameLiveStreaming | 10 |
| libSceCamera | 9 |
| libScePlayGo | 8 |
| libSceSaveData | 8 |
| libSceSaveDataDialog | 8 |
| libSceRtc | 8 |
| libSceNpUtility | 8 |
| libScePad | 7 |
| libSceAppContent | 7 |
| libSceNpSignaling | 7 |
| libSceNpTrophy | 7 |
| libSceNpCommerce | 7 |
| libSceCompanionHttpd | 7 |
| libSceUserService | 6 |
| libSceMoveTracker | 6 |
| libSceFiber | 5 |
| libSceMove | 5 |
| libSceAudioOut | 5 |
| libSceImeDialog | 5 |
| libSceContentSearch | 5 |
| libSceJpegDec | 5 |
| libSceMsgDialog | 4 |
| libSceErrorDialog | 4 |
| libSceNpAuth | 3 |
| libSceSsl | 2 |
| libSceVideoOut | 2 |
| libSceSysmodule | 2 |
| libSceAudioIn | 2 |
| libSceDiscMap | 2 |
| libSceNpCommon | 1 |
| libSceCommonDialog | 1 |

## Limites e próximo trabalho

Os símbolos não resolvidos incluem funções e dados; não são todos chamadas de função.
Os serviços host habilitados são registrados separadamente no loader. As linhas deste relatório continuam pendentes. Nomes e candidatos ISS servem para análise, sem garantia de semântica ou ABI compatível.
A ponte ABI e os blocos TLS foram testados com fixtures sintéticas; ainda faltam o acesso TLS pelo código original, exceções, varargs, agregados e validação de red zone.
Próximo trabalho: iniciar serviços host com contratos explícitos e testes, mantendo símbolos desconhecidos pendentes.
Restam 0 relocations TLS e 0 casos RELATIVE com símbolo não zero neste relatório.
