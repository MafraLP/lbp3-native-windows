# Compilar e testar no Windows

Este guia compila o host de diagnóstico e roda testes locais. Não inicia o jogo nem fornece arquivos de jogo.

## Requisitos

- Windows 10/11 x64.
- Visual Studio 2022 ou Build Tools, ferramentas C++ e Windows SDK.
- LLVM/Clang com `clang-cl` x64, CMake 3.24 ou mais recente, e Ninja.
- Um **x64 Native Tools Command Prompt** ou **Developer PowerShell**. O shell precisa encontrar `clang-cl`, CMake, Ninja e o linker/SDK do Visual Studio.

## Build padrão

Na raiz do checkout:

```powershell
cmake --fresh --preset win-clang-debug
cmake --build --preset win-clang-debug
ctest --preset win-clang-debug
```

O executável fica em `out/build/win-clang-debug/lbp3_host.exe`. O preset `win-clang-relwithdebinfo` prepara um build otimizado com símbolos.

Para inspecionar um ELF que você possa usar legalmente:

```powershell
out\build\win-clang-debug\lbp3_host.exe "C:\caminho\para\eboot.elf"
```

O modo de inspeção não é um modo de jogar. Os parâmetros de diagnóstico evoluem; consulte as opções implementadas no código e os testes antes de usar modos avançados.

## Backend GPU experimental

A opção `LBP3_ENABLE_ISS_GPU` é desativada por padrão. O ISS declara seu código de `port/` sob GPL v2 ou posterior, que permite usá-lo e modificá-lo cumprindo os termos da licença. Prepare também as subdependências e bibliotecas Windows compatíveis e preserve os avisos de cada uma. O CMake local espera um diretório com `CMakeLists.txt` em `LBP3_ISS_GPU_SOURCE_DIR`; o padrão do workspace de desenvolvimento aponta para `out/iss-audit/source/port/gpu`, que não faz parte do clone deste projeto.

Exemplo, depois de configurar todas as dependências requeridas pelo backend:

```powershell
cmake --fresh --preset win-clang-debug `
  -DLBP3_ENABLE_ISS_GPU=ON `
  -DLBP3_ISS_GPU_SOURCE_DIR="C:/src/iss_pc/port/gpu"
cmake --build --preset win-clang-debug
```

Esse exemplo ilustra as opções do projeto; não garante que o backend upstream compile no Windows com qualquer configuração. Consulte [`port/UPSTREAM.md`](https://github.com/HayatoG/iss_pc/blob/main/port/UPSTREAM.md), [`port/gpu/VENDOR.txt`](https://github.com/HayatoG/iss_pc/blob/main/port/gpu/VENDOR.txt) e os termos das dependências. O build básico não precisa dessa árvore.

## Validação

`ctest` roda fixtures próprias sem depender do jogo. Para uma compilação local completa, `tools/validate.ps1` coordena build e testes; leia o script antes de executá-lo e confirme que o preset e o compilador correspondem ao seu ambiente.

Se CMake não for encontrado, abra o Developer Command Prompt do Visual Studio, confirme com `where cmake`, `where ninja` e `where clang-cl`, e então repita o comando a partir da raiz do projeto. Se o CMake disser para configurar uma toolchain x64, abra um shell de desenvolvimento **x64** e reconfigure com `--fresh`.
