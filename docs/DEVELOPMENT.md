# Desenvolvimento

Este repositório é um port/reimplementação nativo em estágio inicial. A meta é executar o código de CPU x86-64 do build PS4 em um processo Windows, implementar serviços Orbis necessários e traduzir a GPU para APIs nativas. Isso é diferente de emular um PS4 completo, mas ainda exige compatibilidade com formatos, ABI, serviços e comandos do título.

## Mapa do código

- `src/loader/`: leitura, metadados, mapeamento ELF e ligação dinâmica.
- `src/abi/`: ponte de ABI guest/host, TLS, execução controlada e primitivas de threads.
- `src/hle/`: serviços host que resolvem imports Orbis conhecidos.
- `src/gpu/`: ponte entre o host e o backend GPU opcional.
- `tests/`: fixtures sintéticas e testes de contratos isolados.
- `analysis/`: evidências e notas internas de pesquisa. Leia antes de alterar comportamentos em investigação.
- `third_party/`: componentes/dados de terceiros com avisos e termos próprios.

## Fluxo de trabalho

1. Reproduza o comportamento com uma fixture sintética sempre que possível.
2. Registre NID, versão e identidade do módulo para imports; não resolva por nome parcial.
3. Separe o comportamento observado das hipóteses e anote evidências em `analysis/`.
4. Execute os testes relevantes e depois o conjunto `ctest --preset win-clang-debug`.
5. Não chame uma função do jogo, construtor ou entry sem que os requisitos de ABI, relocations, TLS e imports usados estejam validados.
6. Nunca adicione arquivos de jogo/firmware ao teste; use fixtures criadas pelo próprio projeto.

## Backend ISS

O backend GPU é uma integração experimental opcional baseada numa árvore externa associada ao projeto [HayatoG/iss_pc](https://github.com/HayatoG/iss_pc). O upstream declara o código em `port/` sob GPL v2 ou posterior, o que permite reutilização cumprindo os termos da licença. As dependências listadas em `port/gpu/VENDOR.txt` mantêm termos próprios. O diretório local de desenvolvimento sob `out/iss-audit` é ignorado pelo Git; se for redistribuir seu conteúdo, mantenha o código-fonte e todos os avisos/licenças aplicáveis. Código/documentação do ISS servem como referência técnica; correções específicas de inFAMOUS não devem ser aplicadas ao LBP3 sem evidência reproduzível.

## Controles locais

Quando nenhum controle XInput está conectado, o host expõe o teclado como um controle virtual conectado. O mapeamento atual é WASD para o analógico esquerdo, setas para o analógico direito, I/J/K/L para o direcional, Espaço para Cross, Shift esquerdo para Circle, E/Q para Square/Triangle, 1/3 para L1/R1, R/F para L2/R2, Z/C para L3/R3 e Enter para Options. Espaço/Cross já avançou o fluxo inicial até a tela de calibração; o menu principal e o gameplay ainda não foram alcançados.

## Relatórios

Inclua Windows/build/compiler, preset e comando, resultado de CTest e erro mínimo reproduzível. Não anexe executáveis, dumps, saves ou capturas com conteúdo protegido. Para o estado conhecido, veja [STATUS.md](STATUS.md); para questões legais e proveniência, veja [LEGAL.md](LEGAL.md).
