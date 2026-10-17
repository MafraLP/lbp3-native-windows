# LittleBigPlanet 3 PS4 → Windows: plano de ataque técnico

Documento de arquitetura e investigação — 16/10/2026.

## Objetivo, premissas e limites de evidência

Executar o código x86-64 original do LBP3 em um processo Windows x64, usando um carregador próprio, uma camada de compatibilidade Orbis/FreeBSD e tradução gráfica para Vulkan. Não haverá VM, kernel convidado nem interpretador geral de CPU. HLE continua sendo emulação dos serviços do sistema; a distinção é que a maioria das instruções da CPU executará diretamente no processador.

O nome `eboot.bin` não identifica seu formato: a entrada pode estar em um contêiner SELF ou já ser um ELF utilizável. A implementação proposta recebe uma imagem ELF/OELF já disponível e analisável, acompanhada dos módulos e recursos da mesma versão. O carregador deve identificar e recusar contêineres não suportados com um diagnóstico explícito. A obtenção e transformação de conteúdo protegido não fazem parte deste plano.

Não foi fornecido um eboot de LBP3 nesta sessão. Portanto, nenhum offset, NID, layout de estrutura, sequência de inicialização ou requisito específico desse executável foi confirmado. Todos esses itens deverão ser medidos por versão, sem reutilizar endereços de inFAMOUS. Registrar região/CUSA, atualização, SHA-256 dos módulos, configuração e versão do runtime em cada execução.

## Arquiteturas extraídas das referências

### HayatoG/iss_pc

O projeto documenta execução nativa de CPU, reimplementação do sistema e tradução de GPU. Isso fundamenta a separação do nosso projeto em loader, runtime HLE e backend gráfico, sem implicar compatibilidade com outro jogo. Fonte: [README](https://github.com/HayatoG/iss_pc/blob/main/README.md).

A leitura de [port/build.sh](https://github.com/HayatoG/iss_pc/blob/main/port/build.sh) identifica `src/probe.c`, `runtime*.c`, `compat_win.c`, `cpu_check.c`, `isa_rewrite.c` e `isa_emulate.c`. O script combina o runtime com a biblioteca gráfica; a rota Windows usa Clang/MSYS2 e CMake. Esses são pontos de investigação do loader, da compatibilidade Windows e das adaptações de instruções. A inspeção aqui foi do script que os referencia, não uma auditoria completa desses arquivos.

O [CMake do backend](https://github.com/HayatoG/iss_pc/blob/main/port/gpu/CMakeLists.txt) mostra `video_core`, `shader_recompiler`, drivers Gnm/VideoOut de origem shadPS4 e a fronteira `shim/`. No Windows, `tlggpu` é estática e resolve os serviços do runtime no executável. Também aparecem Zydis, VMA, SDL3, SPIR-V e Xbyak. A separação entre núcleo gráfico e shims é a principal arquitetura reutilizável.

**Decisão proposta:** preservar essa fronteira conceitual, substituindo dependências implícitas por interfaces explícitas de memória, eventos, relógio e logging. Não exigir DLLs para todos os componentes: uma biblioteca estática pode implementar módulos PS4 lógicos. Antes de copiar código, fixar commit e verificar licenças de cada componente, inclusive código derivado; não assumir uma licença única pelo nome do port.

### morluto/rea

O [mapa de arquitetura](https://github.com/morluto/rea/blob/main/docs/architecture.mermaid) separa entradas CLI/MCP, workflows, sessões, contratos, domínio de evidências e adapters de providers. Ghidra usa uma bridge Java; IDA tem adaptação de MCP externo. Essa arquitetura sugere manter a investigação fora do processo do jogo e correlacionar artefatos por identidade estável.

O [contrato MCP](https://github.com/morluto/rea/blob/main/docs/mcp-contracts.md) expõe resultados normalizados, identidade de evidência e limitações. Usaremos esses registros para distinguir observações, hipóteses e lacunas. A disponibilidade dos comandos e schemas deverá ser descoberta na versão instalada; não presumir ferramentas só pelo nome.

A documentação distingue análise nativa de capacidades específicas de plataforma. O suporte Ghidra no Windows é descrito como experimental; análise de crashes Linux não equivale a suporte a minidumps Windows. Validar primeiro uma amostra pequena, conforme [README](https://github.com/morluto/rea/blob/main/README.md) e [Windows Ghidra](https://github.com/morluto/rea/blob/main/docs/windows-ghidra-p0.md).

## Organização proposta

```text
lbp3_host.exe
 ├─ loader_orbis       parser ELF, módulos, relocations, imports
 ├─ guest_memory       intervalos, permissões, aliases e ownership
 ├─ abi_bridge        entrada, callbacks, TLS e trampolines
 ├─ hle_runtime       kernel, arquivos, threads, serviços
 ├─ gpu_bridge        GNM/PM4, shaders, recursos e Vulkan
 └─ diagnostics       eventos, mapa de endereços e crash capture

REA MCP/CLI ↔ Ghidra ou IDA ↔ eboot/módulos imutáveis
                  ↑
       relatórios Windows + evidências + patches versionados
```

Criar interfaces `GuestMemory`, `ModuleResolver`, `GuestThread`, `KernelEvents` e `GpuSubmission`. Ponteiros do convidado devem ter tipo próprio e validação de intervalo; handles Windows nunca devem vazar como handles Orbis. Usar tabelas com geração para detectar handles obsoletos.

## Fase 1: O Carregador — ELF Loader & Memória no Windows

### 1.1 Inventário e parser

1. Inspecionar magic, ELFCLASS64, little-endian, máquina x86-64, tipo Orbis observado e entry point. Validar somas, tamanhos, limites de arquivo e sobreposições antes de alocar.
2. Ler program headers, especialmente `PT_LOAD`, `PT_DYNAMIC`, `PT_TLS` e metadados específicos SCE presentes. Não depender de section headers: podem estar ausentes.
3. Para cada segmento, garantir `p_filesz <= p_memsz`, copiar bytes do arquivo e zerar a cauda BSS. Respeitar a congruência de `p_vaddr`, `p_offset` e `p_align`.
4. Extrair tabelas dinâmicas, símbolos, strings, relocations e dependências. Tags `DT_SCE_*` exigem interpretação Orbis; offsets não devem ser tratados indiscriminadamente como ponteiros ELF genéricos.
5. Construir grafo dos PRX necessários, resolver imports antes dos construtores e tratar carregamento dinâmico posterior pela mesma infraestrutura.

### 1.2 Modelo de endereçamento

Adotar inicialmente um endereço virtual guest igual ao endereço host dentro de uma arena reservada: isso preserva ponteiros internos sem tradução por acesso. Reservar cedo, antes de carregar bibliotecas opcionais. Usar o intervalo realmente pedido pelos módulos e pelas APIs de memória; não escolher uma base arbitrária e esperar que código com endereços absolutos funcione.

Para imagens relocáveis, calcular `runtime_va = load_bias + elf_va`. Para imagens fixas, falhar de maneira clara caso o intervalo não esteja disponível. Registrar cada segmento com endereço ELF, endereço host, arquivo, offset, tamanho e permissões. Separar memória direta Orbis, memória flexível e mapeamentos de arquivos: podem compartilhar backing ou possuir regras diferentes.

Windows distingue granularidade de reserva e tamanho de página. Consultar `GetSystemInfo`; representar separadamente a página Orbis, tipicamente 16 KiB a confirmar, a página host e a granularidade de reserva, frequentemente 64 KiB. Uma reserva pode conter vários subintervalos comprometidos. Não arredondar um pedido e permitir que ele destrua o vizinho. A separação reserve/commit e suas restrições vêm de [VirtualAlloc](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc).

### 1.3 Adaptador POSIX/Orbis → Windows

| Semântica guest | Base Windows | Trabalho adicional |
|---|---|---|
| `mmap` anônimo | `VirtualAlloc` reserve/commit | endereços, alinhamento, zero inicial e tabela de intervalos |
| `mprotect` | `VirtualProtect` | converter flags e verificar ownership de todas as páginas |
| `munmap` | `VirtualFree` decommit/release | release exige a base da reserva; unmap parcial exige bookkeeping |
| `mmap` de arquivo | `CreateFileMapping`/`MapViewOfFile`, ou backing próprio | alinhamento do offset, prefixo, visibilidade e copy-on-write |
| memória direta e aliases | backing compartilhado e views apropriadas | preservar identidade física lógica; duas cópias não equivalem a alias |
| `open/read/pread/stat` | `CreateFileW`, `ReadFile`, consultas de arquivo | descritores guest, offsets independentes e layouts Orbis |
| tempo monotônico | `QueryPerformanceCounter` | escala e epoch guest; converter timeouts com cuidado |

`MAP_FIXED` só substitui mapeamentos pertencentes à arena guest. A memória do processo host nunca é um destino aceitável. Manter política própria para colisões e distinguir pedido fixo de mera sugestão. Falhas Windows devem ser traduzidas para o domínio correto de erros POSIX ou SCE.

Permissões finais: nenhum acesso → `PAGE_NOACCESS`; R → `PAGE_READONLY`; RW → `PAGE_READWRITE`; RX → `PAGE_EXECUTE_READ`. Tratar combinações adicionais explicitamente. Carregar/relocar com escrita temporária, finalizar permissões e chamar `FlushInstructionCache` após alterar código. Não transformar uma access violation de dados em motivo para habilitar execução globalmente. Mapeamentos guest RWX, se comprovadamente necessários, terão tratamento localizado.

### 1.4 Relocations, entrada, ABI e TLS

Implementar primeiro os tipos encontrados no inventário, como `R_X86_64_RELATIVE`, `GLOB_DAT`, `JUMP_SLOT` e `R_X86_64_64`, com resolução por módulo e validação de addends. Relocations TLS precisam de implementação própria; tipo desconhecido bloqueia o boot com endereço e diagnóstico.

**Risco central:** Orbis usa uma ABI derivada de System V AMD64; Win64 usa outra convenção. Chamadas simples começam em RDI/RSI/RDX/RCX/R8/R9 no lado System V e RCX/RDX/R8/R9 no Win64. Isso não cobre floats, structs, retornos agregados ou varargs. Win64 exige shadow space, e há diferenças de preservação de registradores e uso da pilha. Ver [convenção x64 Microsoft](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170).

Usar thunks assembly/Xbyak ou fronteiras `sysv_abi` comprovadas com Clang para cada assinatura. MSVC x64 não oferece uma troca universal de ABI via `__cdecl`. Verificar suporte efetivo no toolchain escolhido com testes de argumentos mistos e retornos. Callbacks host → guest exigem a ponte inversa. Nenhuma exceção C++ deve atravessar essa fronteira.

Construir pilha inicial, argumentos, ambiente e metadados conforme a rotina de entrada observada; `e_entry` não deve ser chamado como se fosse `main()` de C++. Reproduzir ordem de inicialização dos módulos, construtores e finalizadores.

Implementar blocos TLS e DTV por thread conforme os acessos observados. O GS do Windows pertence ao TEB; não o substituir globalmente. Se houver acessos TLS incompatíveis via FS/GS, reescrever somente instruções identificadas para acessar o bloco guest, mantendo o estado necessário nos thunks. Preservar a red zone System V nas transições e avaliar seu uso em código guest sujeito a exceções Windows. Desvios para stubs devem respeitar alcance relativo, instruções RIP-relative e limites de instrução.

Inspecionar CPUID e as instruções efetivamente usadas. x86-64 comum não garante SSE4a ou todas as extensões da CPU PS4 em Intel/AMD atuais. Preferir requisito de CPU explícito ou substituições pontuais verificadas. Isso não exige tradução geral de CPU.

**Saída da fase:** um ELF sintético executa e retorna pelo bridge; testes de BSS, relocations, proteção RX, unmap parcial, TLS de duas threads e callbacks passam. O LBP3 chega à primeira importação observável, com mapa completo de segmentos.

## Fase 2: Interceptação de Syscalls e HLE

### 2.1 Imports antes de hooks

Criar registro por identidade de módulo/biblioteca, versão e NID/símbolo. Resolver GOT/PLT e imports dinâmicos para funções HLE com assinatura guest. Não presumir que o eboot terá nomes legíveis. Manter origem e confiança do mapeamento NID → função.

Módulos lógicos `libkernel`, `libSceVideoOut`, `libSceGnmDriver` e outros confirmados pelo inventário podem ser implementados por `hle_kernel.dll`, `hle_videoout.dll` etc. O nosso loader faz a ligação; o loader PE do Windows não resolve imports Orbis. Exportar uma API C host estável de registro, sem expor STL entre DLLs. A opção inicial de link estático reduz problemas de lifetime e ordem de inicialização.

Distinguir três superfícies:

1. Imports de bibliotecas: redirecionamento pelo resolver, caminho preferencial.
2. Chamadas internas/inlining: instrumentação ou patch de um ponto comprovado por disassembly.
3. Instrução `syscall` direta: nunca deixá-la executar como syscall NT. Localizar os stubs alcançáveis por análise de código e reescrevê-los para o dispatcher guest antes da execução. Não fazer busca cega por dois bytes em dados; instruções x86 têm comprimento variável. Preservar a semântica de retorno, flags e registradores conforme o stub e a ABI Orbis observados.

VEH não é interceptador genérico de `syscall`: a instrução pode entrar no kernel Windows sem gerar a exceção esperada. Bloquear boot quando a cobertura dos stubs necessários não estiver demonstrada.

### 2.2 Ordem de implementação

Começar com memória, resolução de módulos, saída de diagnóstico, TLS, relógio, arquivos e threads. Prosseguir com mutexes, condições, semáforos, filas de eventos e waits, usando SRWLOCK, CONDITION_VARIABLE, WaitOnAddress e objetos próprios onde a semântica exigir. Afinidade, prioridade, recursão, timeout e destruição não têm correspondência perfeita.

Mapear `/app0` para o dump e áreas graváveis para diretórios de usuário. Resolver diferenças de separador, case sensitivity, arquivos base/update e acesso assíncrono. Não passar `struct stat`, `pthread_*` ou estruturas SCE diretamente para o CRT Windows. Usar estruturas de largura fixa e `static_assert` de tamanho/alinhamento/offset.

Retornos devem distinguir wrappers POSIX (`-1` e errno guest por thread) de APIs SCE com códigos próprios. Stubs desconhecidos registram argumentos e devolvem erro coerente; sucesso fictício pode causar corrupção muito depois da chamada. Áudio, input, usuário e saves entram conforme o trace real. Recursos online devem retornar estados controlados e nunca bloquear indefinidamente a inicialização local.

### 2.3 Identificação inicial com Ghidra/REA

Importar o ELF como x86-64 little-endian, com loader Orbis se necessário para interpretar suas tabelas. Se o provider não aceitar o formato, preparar projeto/script Ghidra que reconstrua os segmentos e exporte metadados; validar esse caminho antes de automatizá-lo no REA.

Partir do entry point real, seguir startup, construtores e import thunks. Buscar referências a inicialização de memória, módulos, threads, arquivos e apresentação. A ordem “heap → workers → arquivos → vídeo” é hipótese de investigação, não um fato sobre LBP3. Correlacionar o grafo estático com o trace temporal HLE para identificar o caminho executado.

Produzir `imports.csv` com módulo, NID, assinatura, chamadores, frequência e status HLE; `boot_graph.md` com evidências; `types/` com estruturas verificadas. Os comandos `open_binary`, `list_procedures` e `analyze_function` estão documentados no [guia nativo REA](https://github.com/morluto/rea/blob/main/docs/native-investigation.md); usar schemas da versão instalada.

**Saída da fase:** inicialização reproduzível, workers e I/O operacionais, nenhum import crítico sem resolução e chegada a uma submissão gráfica válida. Registrar cobertura por cenário, não apenas percentual de funções implementadas.

## Fase 3: Tradução Gráfica e Ciclo de Renderização

### 3.1 Fronteira GNM/GNMX

GNMX pode estar estaticamente ligado e produzir comandos sem imports de alto nível. Interceptar somente nomes GNM não basta. A fronteira deve aceitar submissões e interpretar os command buffers produzidos pelo jogo, incluindo buffers indiretos, estado de registradores e sincronização.

Pipeline proposto: chamadas HLE GnmDriver → filas guest/PM4 → estado GPU intermediário → tradução de shaders GCN → SPIR-V → pipelines, recursos e comandos Vulkan. Partir dos componentes identificados no CMake do iss_pc e adaptar os shims ao nosso runtime. Não transportar correções específicas de inFAMOUS sem reproduzir seu problema em LBP3.

Implementar progressivamente packets realmente observados: escrita de estado, draw/dispatch, indireção, cópia, eventos, labels e waits. Preservar ordem entre filas e semântica de conclusão. GNMX gera comandos; não há correspondência obrigatória de uma função GNMX para uma chamada Vulkan.

### 3.2 Shaders e memória

O tradutor precisa de decoding GCN, fluxo de controle/IR, recursos e geração SPIR-V. glslang ou shaderc compilam shaders host; não traduzem sozinhos o binário GCN do jogo. Validar divergência EXEC, precisão, atomics, LDS, barreiras e operações wave64 em GPUs com subgrupos diferentes.

Manter catálogo de shaders por hash, stage, recursos e estado relevante. Chaves de cache incluem versão do tradutor e opções; pipelines do driver incluem identidade e compatibilidade do dispositivo. Shader não suportado deve produzir diagnóstico e captura reproduzível.

A memória unificada do PS4 precisa virar sincronização explícita: intervalo guest → recurso Vulkan, uploads de alterações CPU, readbacks de alterações GPU, tiling/detiling e aliases buffer/textura. Um ponteiro guest não é automaticamente um device address Vulkan. VMA administra alocação host de GPU, mas não implementa essa coerência.

Começar com caminho conservador de cópia e sincronização em pontos conhecidos. Depois introduzir rastreamento de escrita e readback por intervalo. Se usar proteção de páginas para dirty tracking, coordená-la com o gerenciador de memória e distinguir faults esperados de crashes. Tratar caches de textura, formatos, depth/stencil, compressão e buffers indiretos como trabalho central, não acabamento.

### 3.3 HWND e apresentação

1. Registrar classe Win32 e criar HWND com `CreateWindowExW`; manter message pump responsivo e tratamento de resize/DPI.
2. Inicializar volk, criar instância com `VK_KHR_surface` e `VK_KHR_win32_surface`, carregar dispatch e criar `VkWin32SurfaceKHR` com HINSTANCE/HWND.
3. Selecionar GPU e filas com suporte gráfico e apresentação; consultar features, limites, formatos e present modes. Vulkan 1.3 é a meta inicial, sujeita às necessidades do backend escolhido.
4. Criar device, swapchain, image views e recursos por frame; tratar `OUT_OF_DATE`, minimização e recriação por resize. A conexão superfície/swapchain está descrita no [guia WSI Vulkan](https://docs.vulkan.org/guide/latest/wsi.html).
5. `sceVideoOutRegisterBuffers` registra os buffers guest. O flip espera a produção do buffer, copia/converte para a imagem adquirida, submete e apresenta. Contagem de flip, eventos e vblank são estados guest separados do retorno de `vkQueuePresentKHR`.

Usar semáforos binários onde WSI exige, fences por frame e timeline semaphores nas dependências internas compatíveis. Vulkan não usa um contexto corrente anexado ao HWND como OpenGL; o vínculo ocorre pela surface e swapchain.

**Marcos:** janela com clear color → replay de submissão capturada → primeiro frame real → menu navegável → primeira cena. Validar com validation layers e capturas RenderDoc quando aplicável. Evitar `vkDeviceWaitIdle` por draw; aceitar sincronização grosseira temporária apenas para estabelecer correção.

## Fase 4: Integração do Fluxo de Trabalho de IA

### 4.1 Captura Windows

Criar supervisor de debug que execute o host e use eventos de exceção Windows, ou um coletor externo coordenado com o processo. Em access violation, conservar `EXCEPTION_RECORD`, `CONTEXT`, RIP/RSP/RFLAGS, registradores gerais, estado SIMD disponível, thread, endereço acessado e tipo de acesso. `ExceptionInformation[0]` distingue leitura, escrita e execução; `[1]` fornece o endereço da violação.

Gerar minidump com DbgHelp e relatório JSON próprio. Captura em processo via VEH deve escrever apenas em buffers pré-alocados e sinalizar o coletor: evitar heap, locks, Vulkan ou análise dentro do handler. Não continuar execução após AV desconhecida. Manter tratamento específico para faults deliberados de instrumentação.

Salvar mapa guest/host e módulos, hashes, bytes próximos ao RIP, páginas e permissões, últimas chamadas HLE por thread, eventos de memória e últimos submits. Para hang, coletar todas as threads, waits e progresso das filas. Para GPU device lost, conservar breadcrumbs e diagnostics disponíveis do driver; registradores CPU isolados raramente explicam a falha gráfica.

### 4.2 Conversão correta de endereços

Não usar offset de arquivo como se fosse endereço de análise. Para RIP em um módulo carregado:

```text
elf_va = runtime_rip - load_bias
ghidra_va = elf_va + analysis_bias
file_offset = p_offset + (elf_va - p_vaddr)
```

A última fórmula só vale dentro de um `PT_LOAD` e da região com backing de arquivo (`delta < p_filesz`). BSS não tem bytes correspondentes no arquivo. `analysis_bias` é o deslocamento realmente aplicado na importação Ghidra. Se RIP estiver em thunk, usar o mapa de trampolines para localizar a instrução original; se estiver em DLL host, analisar símbolos/PDB host e sua origem guest.

### 4.3 Ciclo iterativo

1. Reproduzir um cenário determinístico com build, hashes, save e configuração registrados.
2. Classificar a falha: loader/relocation, ABI, TLS, memória/alias, HLE, sincronização, shader ou conteúdo ausente.
3. Consultar no REA a função que contém o endereço, assembly, callers, referências e acessos às estruturas envolvidas. Trabalhar com consultas pequenas, não uma decompilação indiscriminada do eboot inteiro.
4. Registrar hipótese e evidências: instrução, registradores, origem do ponteiro, contrato esperado e lacunas. Exemplo hipotético: um getter HLE devolveu sucesso sem preencher um ponteiro; a correção fica no getter, sustentada pelo trace do consumidor.
5. Escrever correção C++ no subsistema responsável e um caso mínimo que diferencie comportamento incorreto do correto. Reexecutar o cenário e verificar também consumidores relacionados.
6. Exportar evidências e atualizar tipos, imports e registro de incógnitas. Fixar versão do provider e perfil de análise para manter comparabilidade.

REA coordena a análise; o adapter `windows_crash_report` será código nosso. Não assumir que o servidor abre `.dmp` ou correlaciona endereços Orbis automaticamente. A IA deve justificar assinaturas e estruturas com instruções e usos observados; pseudocódigo sozinho não prova layout ou ownership.

### 4.4 Patches revisáveis

Priorizar correções de contrato no runtime. Para patch inevitável no guest, armazenar SHA-256 do módulo, endereço ELF, bytes esperados, bytes novos/trampoline, justificativa e evidência. Conferir assinatura antes de aplicar; suspender/coordenar threads, alterar proteção temporariamente, escrever, limpar instruction cache e restaurar permissões. Guardar bytes originais e permitir desligar cada patch.

Não avançar RIP para ignorar crashes nem trocar saltos por NOP sem explicar o estado perdido. MinHook pode ajudar em hooks host compatíveis, mas não resolve ABI Orbis, imports ELF ou relocation de qualquer bloco arbitrário.

**Saída da fase:** cada incidente tem cenário, relatório, endereço correlacionado, hipótese testável e patch rastreável. O mesmo processo também trata hangs e divergências visuais.

## Base C++ / Visual Studio / CMake

| Componente | Escolha proposta | Papel e restrições |
|---|---|---|
| Toolchain | Visual Studio com Clang/LLVM e Ninja; C++23 | começar com ABI comprovada; compatibilidade MSVC exige adaptar flags e assembly |
| Parser ELF | parser próprio restrito ou ELFIO | extensão para tags SCE e validação obrigatória |
| Disassembly | Zydis ou Capstone | selecionar um inicialmente; Zydis acompanha a referência |
| Stubs | Xbyak ou AsmJit | thunks, callbacks e reescritas pontuais |
| Hooks | MinHook, opcional | preferir resolução de imports sob nosso controle |
| Vulkan | Vulkan SDK, volk, VMA | dispatch, validação e alocação GPU |
| Shaders | núcleo GCN adaptado, SPIRV-Tools, glslang | tradução guest e shaders auxiliares são pipelines distintos |
| Diagnóstico | fmt/spdlog, nlohmann/json, DbgHelp | logs estruturados, relatórios e dumps |
| Input/áudio | SDL3; backend Windows quando necessário | integrar depois dos contratos guest observados |
| Testes | Catch2 ou GoogleTest + CTest | fixtures ELF, ABI e contratos HLE |

Essas são recomendações de arquitetura, não uma lista de dependências já integrada. Fixar versões/commits em manifest e preservar licenças. Não misturar automaticamente o ABI C++ de bibliotecas MSYS2 com binários MSVC: recompilar dependências no toolchain adotado ou usar fronteiras C.

Estrutura inicial:

```text
CMakeLists.txt / CMakePresets.json
src/{loader,memory,abi,hle,gpu,diagnostics,platform/win32}
include/lbp3_host/
tools/{crash_collector,report_correlator}
analysis/{manifests,imports,types,evidence,unknowns}
patches/<module_sha256>/
tests/{elf_fixtures,abi,hle,gpu_replay}
third_party/
```

Criar targets distintos `orbis_loader`, `guest_memory`, `abi_bridge`, `hle_runtime`, `gpu_bridge`, `lbp3_host` e `crash_collector`. Para DLLs, usar interface C versionada e tabela de funções explícita. Presets `win-clang-debug` e `win-clang-relwithdebinfo`, PDBs e frame pointers habilitados. Sanitizers são úteis no código host isolado, com avaliação específica de compatibilidade com arena e código guest.

Comandos planejados após criar esses targets e presets:

```powershell
cmake --preset win-clang-debug
cmake --build --preset win-clang-debug
ctest --preset win-clang-debug --output-on-failure
```

## Sequência de execução e critérios de decisão

| Marco | Entrega verificável | Dependência |
|---|---|---|
| M0 | manifest do dump, imports e formato confirmados; commits das referências fixados | acesso aos arquivos LBP3 |
| M1 | ELF sintético, ponte ABI, TLS e memória protegida funcionando | Fase 1 |
| M2 | startup LBP3 com trace HLE e diagnóstico de chamadas desconhecidas | M1 + serviços mínimos |
| M3 | HWND/Vulkan e replay gráfico independente | pode avançar junto de M2 |
| M4 | primeiro frame real e menu com input | M2 + M3 + sincronização |
| M5 | cena jogável com áudio e save/load consistente | cobertura orientada pelo trace |
| M6 | estabilidade em sessões repetidas e GPUs distintas | regressões CPU/GPU e evidências |

Não estimar prazo de gameplay antes de M0: imports, bibliotecas estáticas, middleware, shaders e requisitos do conteúdo ainda são desconhecidos. Carregar o ELF é apenas o primeiro marco. A maior incerteza técnica está na semântica do runtime, coerência de memória, sincronização e tradução de shaders.

O plano é implementável por incrementos verificáveis, mas ainda não é uma demonstração de viabilidade específica do LBP3. Este documento foi elaborado por consulta às fontes públicas e desenho de arquitetura; nenhum binário do jogo foi executado, nenhum provider REA foi instalado e nenhum offset foi medido nesta sessão.
