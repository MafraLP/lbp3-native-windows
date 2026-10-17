# Aproveitamento do ISS no LBP3

Revisão analisada: `a065f561aef8544df2d0cf711dea93881afd04c3`.
Esta entrega é uma auditoria estática; nenhum fonte ISS foi integrado ao host,
nenhuma build do ISS foi compilada e nenhum código do jogo foi executado.

## Resultado

Dos 600 imports diretos do eboot, 342 têm candidatos em resolvers/tabelas
registradas, 136 têm somente nomes conhecidos e 122 não tiveram correspondência.
478 nomes foram identificados. Esses números não representam percentual de
implementação ou de compatibilidade do jogo.

As 342 linhas possuem 346 evidências porque alguns nomes aparecem em mais de
uma tabela. A ferramenta não determina qual resolver vence. Entre as evidências,
108 têm retorno constante/trivial, 12 têm marcador de stub, 10 não tiveram corpo
localizado e uma é export de dados. As demais 215 têm corpo localizado, mas não
passaram por validação de semântica. Retorno trivial pode ser válido para algumas
APIs; não é, isoladamente, prova de bug.

## O que vale reaproveitar

| Componente | Evidência | Trabalho de integração |
|---|---|---|
| Loader/linker de módulos | `port/scripts/link_modules.py` prepara módulos, bindings e relocations | Adaptar entrada/saída, identidade de bibliotecas, versões, TLS e validações ao nosso loader |
| Memória guest | `port/src/runtime_memory.c`, incluindo VirtualAlloc2 e views de backing compartilhado | Conciliar arena, tabelas de ownership e callbacks com a memória da GPU |
| Threads e sincronização | `runtime_thread.c`, `runtime_mutex.c`, `runtime_sema.c`, `runtime_rwlock.c` | Winpthreads e layouts guest; testar chamadas e callbacks com ABI correta |
| Arquivos e tempo | `runtime_file.c`, `runtime_kernel.c`, `runtime_time.c` | Mounts, caminhos, erros e estruturas guest |
| GPU | `port/gpu/shim/tlggpu.cpp` registra GnmDriver e VideoOut | Integração com memória, eventos, janela e fault handling; validar shaders LBP3 |
| Controle e saída de áudio | `runtime_pad.c`, `runtime_audio.c` | SDL3, callbacks e contratos usados pelo jogo |
| Diagnóstico de funções | `port/src/import_names.inc` | Já utilizado como referência para os nomes no relatório; não é tabela de implementações |

Encontramos candidatos para 45/45 imports de GnmDriver, 8/8 de VideoOut,
15/15 de AvPlayer, 7/7 de Pad e 5/5 de AudioOut. Não significa que gráficos,
vídeos, controle ou áudio de LBP3 já funcionem.

## Limitações confirmadas por leitura

- `runtime_services.c:140`: `http_fail` retorna erro de rede; várias funções HTTP apontam para ele.
- `runtime_services.c:164`: `np_signed_out` retorna estado sem login; várias funções NP usam esse alvo.
- `runtime_services.c:649`: `audio_in_open` retorna áudio de entrada não aberto; `sceAudioInOpen` e `sceAudioInInput` apontam para esse alvo.
- Funções Gnm também incluem stubs: o CSV identifica os marcadores encontrados nos alvos importados.
- Serviços de câmera, Move, MoveTracker e streaming não tiveram candidatos nesta busca.
- Patches/hooks de inFAMOUS, offsets de código e suposições de versão não devem ser aplicados a LBP3 sem evidência específica.

## Módulos que já existem na cópia local

A pasta extraída contém `libc.prx`, `libSceFios2.prx`, `fmodextinput.prx`,
`fmodsmsreverb.prx`, `fmodsmswavehammer.prx`, `fmodvoipmixer.prx` e `spu.prx`.
Isso combina com dependências declaradas pelo eboot. O linker do ISS prevê
carregar libc e Fios2 do próprio jogo e resolver exports nativos.
Não analisamos os exports desses PRX nem suas dependências transitivas nesta
comparação: os nomes dos arquivos, sozinhos, não provam cobertura.

## Build

A documentação Windows do ISS usa MSYS2 CLANG64, MinGW-w64 e winpthreads.
O nosso host usa clang-cl/MSVC, e atualmente só declara CXX no CMake.
O runtime ISS é C e usa `__attribute__((sysv_abi))` nas entradas guest.
Há dependências POSIX/winpthreads e bibliotecas C++ que precisam ser compiladas
no ambiente escolhido. Não basta adicionar todos os arquivos ao nosso CMake.

O CMake da GPU encontra Vulkan, SDL3, fmt, Boost, robin-map, xxhash, VMA,
Zydis, glslang, SPIR-V e outras dependências; a build Windows liga também
onecore, bcrypt e ws2_32. Os submódulos precisam ser obtidos nas revisões fixadas.

Recomendação: manter o host atual como ferramenta de inspeção e primeiro
compilar uma cópia isolada do ISS no ambiente suportado por ele. Validar um
teste pequeno de ABI antes de decidir portar seus componentes para clang-cl
ou manter um executável de runtime separado em CLANG64. Nenhum desses passos
exige trocar nosso projeto atual imediatamente.

## Licenças verificadas

- `port/LICENSE`: texto GNU GPL versão 2. Não há LICENSE na raiz do snapshot consultado.
- `port/gpu/shadps4/LICENSE`: texto GNU GPL versão 2; arquivos como
  `core/libraries/avplayer/avplayer.cpp` declaram `GPL-2.0-or-later` no SPDX.
- `.gitmodules` registra dependências externas com suas próprias revisões;
  baixar os fontes principais não traz automaticamente conteúdo/licenças dos submódulos.
- Existem licenças separadas para outros componentes, incluindo sirit, half e DLSS bridge.

Preservar notices e atribuições dos arquivos reaproveitados. A integração ou
distribuição precisa respeitar os termos aplicáveis a cada componente;
esta auditoria não conclui que todo o repositório tenha uma única licença
nem que os submódulos estejam integralmente auditados.

## Sequência recomendada

1. Usar a lista de nomes/candidatos deste relatório para orientar a implementação.
2. Inventariar exports e imports dos PRX locais, começando por libc e Fios2,
   depois FMOD e SPU. Resolver dependências pela identidade e versão do módulo.
3. Fazer uma build isolada do ISS no ambiente suportado, com submódulos fixados.
4. Validar ponte ABI com funções sintéticas, TLS e callbacks; depois relocations
   e bindings com um módulo sintético. Evitar saltar diretamente à entrada LBP3.
5. Integrar primeiro memória/kernel/arquivos e diagnóstico de import ausente;
   em seguida a fronteira GPU. Manter patches específicos de inFAMOUS desativados.

## Reproduzir

Os fontes consultados estão em `out/iss-audit/source`, fora dos targets do host.
Os scripts de linker adicionais estão em `out/iss-audit/runtime/port/scripts`.

```bat
out\unfself\python\python.exe tools\compare_iss_imports.py --source out\iss-audit\source --imports analysis\imports\lbp3\imports.json --commit a065f561aef8544df2d0cf711dea93881afd04c3 --output analysis\imports\iss-comparison
out\unfself\python\python.exe tests\elf_fixtures\iss_comparison_test.py
```

Fontes fixados:
- https://github.com/HayatoG/iss_pc/tree/a065f561aef8544df2d0cf711dea93881afd04c3/port/src
- https://github.com/HayatoG/iss_pc/blob/a065f561aef8544df2d0cf711dea93881afd04c3/port/gpu/CMakeLists.txt
- https://github.com/HayatoG/iss_pc/blob/a065f561aef8544df2d0cf711dea93881afd04c3/port/packaging/windows/README.md
- https://github.com/HayatoG/iss_pc/blob/a065f561aef8544df2d0cf711dea93881afd04c3/port/scripts/link_modules.py
- https://github.com/HayatoG/iss_pc/blob/a065f561aef8544df2d0cf711dea93881afd04c3/port/LICENSE
