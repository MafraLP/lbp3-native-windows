# LittleBigPlanet 3 — Native Windows Port Research

[English](#english) | [Português (Brasil)](#português-brasil)

## English

**Experimental, incomplete research project. This is not a playable release or a console emulator.** The project explores running the PS4 x86-64 game code in a native Windows process, reimplementing required Orbis interfaces, and translating graphics to PC APIs. It is not affiliated with, approved, or endorsed by Sony Interactive Entertainment, Media Molecule, or Sony XDev.

The latest visual milestone is the game's “Health Warning” screen rendered by an experimental Vulkan backend in a local development setup. This shows an early graphics path, but does not prove that input can advance the screen or that the game is playable. That optional GPU integration depends on an external source tree associated with [HayatoG/iss_pc](https://github.com/HayatoG/iss_pc), which is not included here. The ISS port [declares its `port/` code under GNU GPL v2 or later](https://github.com/HayatoG/iss_pc/blob/main/port/UPSTREAM.md); reuse and redistribution are permitted under those license terms. The GPU tree and its dependencies carry additional notices that must be preserved. See [project status](docs/STATUS.md) and [licensing](docs/LEGAL.md).

**No game, firmware, keys, dumps, saves, or other protected game files are included.** This project does not download or decrypt the game. Only use files you are legally entitled to use, and do not publish or share them.

### Guides

- [Status, evidence, and limitations](docs/STATUS.md)
- [Build and run tests](docs/BUILDING.md)
- [Local game files and privacy](docs/GAME_FILES.md)
- [Development and GPU backend](docs/DEVELOPMENT.md)
- [Licensing, credits, and publishing](docs/LEGAL.md)
- [Technical attack plan](LBP3_Windows_Attack_Plan.md)

Research notes under `analysis/` are evolving material. Some files may contain local paths, raw logs, or development-environment details; review each file before making it public.

### Build on Windows x64

Requirements: Visual Studio 2022 or Build Tools with C++ tools and the Windows SDK, x64 `clang-cl`, CMake 3.24 or newer, and Ninja. Open an **x64 Native Tools Command Prompt** or **Developer PowerShell** in the repository directory:

```powershell
cmake --fresh --preset win-clang-debug
cmake --build --preset win-clang-debug
ctest --preset win-clang-debug
```

This builds the host and tests. It does not include the external GPU backend or game files. See the [build guide](docs/BUILDING.md) for details and the experimental GPU configuration.

### Project status

The host maps ELF64 modules, prepares relocations and TLS, and implements host services covered by synthetic tests. In a local experimental build with the external GPU backend, the game's initial health warning was captured. Automated tests validate individual components and contracts; they do **not** establish compatibility or full-game playability.

Important runtime APIs, input, audio, stability, and GPU compatibility remain under investigation. There is no player release or promise of support for any particular game build.

### Credits and licensing

The repository includes GNM initialization data derived from shadPS4 under GPL-2.0-or-later; its notice and license are preserved in [`third_party/shadps4`](third_party/shadps4/README.md). The optional ISS GPU backend is also GPL v2-or-later, with separate notices for its third-party dependencies.

**This repository does not yet declare a project-wide license.** The GPL license permits reuse of the ISS port when its terms are followed, but the project still needs a compatible root license and preserved third-party notices. See [licensing and publishing](docs/LEGAL.md) before creating the GitHub repository.

This project is unofficial and is not endorsed by Sony Interactive Entertainment, Media Molecule, or Sony XDev. *LittleBigPlanet 3*, PlayStation, and related marks belong to their respective owners.

---

## Português (Brasil)

**Projeto experimental e incompleto. Não é uma versão jogável nem um emulador de console.** O projeto pesquisa como executar o código x86-64 do build PS4 em um processo Windows nativo, reimplementar interfaces necessárias do Orbis e traduzir a saída gráfica para APIs de PC. Não é afiliado, aprovado ou endossado pela Sony Interactive Entertainment, Media Molecule ou Sony XDev.

O marco visual mais recente é a tela “Health Warning” do jogo, renderizada por um backend Vulkan experimental numa configuração local de desenvolvimento. Isso mostra um caminho gráfico inicial, mas não prova que a entrada avance a tela nem que o jogo seja jogável. Essa integração GPU opcional depende de uma árvore externa de código associada ao [HayatoG/iss_pc](https://github.com/HayatoG/iss_pc), que não está incluída aqui. O port ISS [declara o código em `port/` sob GNU GPL v2 ou posterior](https://github.com/HayatoG/iss_pc/blob/main/port/UPSTREAM.md); é permitido reutilizá-lo e redistribuí-lo cumprindo os termos da licença. A árvore GPU e suas dependências têm avisos adicionais que devem ser preservados. Veja o [status do projeto](docs/STATUS.md) e o guia de [licenças](docs/LEGAL.md).

**Nenhum jogo, firmware, chave, dump, save ou outro arquivo protegido está incluído.** O projeto não baixa nem descriptografa o jogo. Use apenas arquivos que você tenha direito de usar e não os publique nem compartilhe.

### Guias

- [Status, evidências e limitações](docs/STATUS.md)
- [Compilar e executar os testes](docs/BUILDING.md)
- [Arquivos locais do jogo e privacidade](docs/GAME_FILES.md)
- [Desenvolvimento e backend gráfico](docs/DEVELOPMENT.md)
- [Licenças, créditos e publicação](docs/LEGAL.md)
- [Plano técnico de ataque](LBP3_Windows_Attack_Plan.md)

As notas em `analysis/` são material de pesquisa em evolução. Algumas podem conter caminhos locais, logs brutos ou detalhes do ambiente de desenvolvimento; revise cada arquivo antes de torná-lo público.

### Compilar no Windows x64

Requisitos: Visual Studio 2022 ou Build Tools com ferramentas C++ e Windows SDK, `clang-cl` x64, CMake 3.24 ou mais recente e Ninja. Abra um **x64 Native Tools Command Prompt** ou **Developer PowerShell** na pasta do repositório:

```powershell
cmake --fresh --preset win-clang-debug
cmake --build --preset win-clang-debug
ctest --preset win-clang-debug
```

Isso compila o host e os testes. Não inclui o backend GPU externo nem arquivos do jogo. Consulte o [guia de compilação](docs/BUILDING.md) para detalhes e para a configuração experimental da GPU.

### Estado do projeto

O host mapeia módulos ELF64, prepara relocations e TLS, e implementa serviços host cobertos por testes sintéticos. Em um build experimental local com o backend GPU externo, foi possível capturar o aviso inicial de saúde do jogo. Os testes automatizados validam componentes e contratos isolados; eles **não** demonstram compatibilidade nem jogabilidade completa.

APIs importantes do runtime, entrada, áudio, estabilidade e compatibilidade de GPU seguem em investigação. Ainda não há uma versão para jogadores nem promessa de suporte a um build específico do jogo.

### Créditos e licenças

O repositório inclui dados de inicialização GNM derivados do shadPS4. O aviso GPL-2.0-or-later e o texto da licença estão preservados em [`third_party/shadps4`](third_party/shadps4/README.md). O backend GPU opcional e suas dependências são externos, com procedência e termos próprios.

**Este repositório ainda não declara uma licença global para o projeto.** A licença GPL permite reutilizar o código do ISS cumprindo seus termos, mas o projeto ainda precisa de uma licença raiz compatível e dos avisos de terceiros preservados. Consulte [licenças e publicação](docs/LEGAL.md) antes de criar o repositório no GitHub.

Este é um projeto não oficial e não endossado pela Sony Interactive Entertainment, Media Molecule ou Sony XDev. *LittleBigPlanet 3*, PlayStation e marcas relacionadas pertencem aos respectivos titulares.
