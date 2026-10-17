# Comparação LBP3 × ISS

Revisão ISS: `a065f561aef8544df2d0cf711dea93881afd04c3`.
ELF SHA-256: `c5387ed91227a406e230351539107e81ff507f84f230df8db79d4d2438b7b86e`.

600 imports: 342 com candidato no código, 136 somente com nome identificado e 122 sem correspondência.

**São correspondências estáticas, não funções comprovadamente compatíveis.**

| Biblioteca | Candidatos | Só nome | Não encontrado |
|---|---:|---:|---:|
| FMODExtInput | 0 | 0 | 1 |
| FMODSmsReverb | 0 | 0 | 1 |
| FMODSmsWaveHammer | 0 | 0 | 1 |
| FMODVoIPMixer | 0 | 0 | 1 |
| libSceAppContent | 5 | 0 | 2 |
| libSceAudioIn | 2 | 0 | 0 |
| libSceAudioOut | 5 | 0 | 0 |
| libSceAvPlayer | 15 | 0 | 0 |
| libSceCamera | 0 | 0 | 9 |
| libSceCommonDialog | 1 | 0 | 0 |
| libSceCompanionHttpd | 0 | 0 | 7 |
| libSceContentSearch | 4 | 0 | 1 |
| libSceErrorDialog | 4 | 0 | 0 |
| libSceFiber | 4 | 0 | 3 |
| libSceFios2 | 0 | 16 | 10 |
| libSceGameLiveStreaming | 0 | 0 | 10 |
| libSceGnmDriver | 45 | 0 | 0 |
| libSceHttp | 25 | 0 | 13 |
| libSceImeDialog | 5 | 0 | 0 |
| libSceJpegDec | 5 | 0 | 0 |
| libSceMove | 0 | 0 | 5 |
| libSceMoveTracker | 0 | 0 | 6 |
| libSceMsgDialog | 4 | 0 | 0 |
| libSceNet | 24 | 0 | 0 |
| libSceNetCtl | 6 | 0 | 0 |
| libSceNpAuth | 2 | 0 | 1 |
| libSceNpCommerce | 4 | 0 | 3 |
| libSceNpCommon | 1 | 0 | 0 |
| libSceNpManager | 18 | 0 | 6 |
| libSceNpSignaling | 5 | 0 | 2 |
| libSceNpTrophy | 7 | 0 | 0 |
| libSceNpUtility | 4 | 0 | 4 |
| libSceNpWebApi | 14 | 0 | 6 |
| libScePad | 7 | 0 | 0 |
| libScePlayGo | 8 | 0 | 0 |
| libScePosix | 7 | 0 | 10 |
| libSceRtc | 8 | 0 | 0 |
| libSceSaveData | 8 | 0 | 0 |
| libSceSaveDataDialog | 8 | 0 | 0 |
| libSceSsl | 3 | 0 | 0 |
| libSceSysmodule | 2 | 0 | 0 |
| libSceSystemService | 5 | 0 | 0 |
| libSceUserService | 6 | 0 | 0 |
| libSceVideoOut | 8 | 0 | 0 |
| libc | 12 | 103 | 15 |
| libkernel | 51 | 17 | 4 |
| spu | 0 | 0 | 1 |

## Evidências e limites

comparison.csv contém os nomes, alvos, arquivos e linhas. comparison.json inclui hashes dos fontes consultados.
A busca considera tabelas RuntimeExport, retornos diretos do resolver e registros de GnmDriver, VideoOut, Equeue e AvPlayer.
Comentários são excluídos da busca. A análise não executa o runtime, não verifica precedência de resolvers e não garante que cada candidato seja alcançável na build Windows.
Retornos triviais e marcadores STUBBED são assinalados para revisão. Outros corpos também podem conter limitações.
Ausências podem ser atendidas por módulos PRX do jogo; não significam que precisamos reimplementar tudo.
O nome de uma função, isoladamente, não comprova que ela exista no runtime.

## Revisão manual e integração

Ver integration.md para licenças, build e sequência recomendada.
