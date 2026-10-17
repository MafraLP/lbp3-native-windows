# Memória flexível para o heap da libc

Implementado em `src/memory/memory.cpp`, com contexto explícito
`ScopedFlexibleMemory`, resolver tipado SysV e teste `--memory-self-test`.
Build clang-cl x64 e **14/14 conjuntos CTest passaram**. Nenhuma função da
libc original ou do jogo foi executada.

## Caminho observado

No ELF libc de SHA-256
`9811633cd5d49ae461ec0550468db5ef037d576abe1d732348844b930f397566`,
o helper em VA 0x14e0 chama PLT 0x150 em 0x1530 para mapear memória com
proteção 3, flags zero, ponteiro de saída na stack e nome em R8. Se o flag
global em 0xbaf4c estiver ativo, chama PLT 0x160 em 0x1549 para mlock.
O caminho de erro chama PLT 0x170 em 0x1559 para liberar o mapeamento.
O helper de alocação em 0x1590 também chama esses serviços, por exemplo
o map em 0x1644. O recorte está em `flexible-map-path-disassembly.txt`.

Isso comprova a necessidade desses serviços nos caminhos de alocação,
não que todos sejam usados incondicionalmente por DT_INIT. Com ProcParam
+0x38 nulo, o bootstrap do heap segue por 0x1309; outros globais controlam
se há alocação antecipada. A closure dos 21 construtores continua incompleta.

Assinaturas e associações de NIDs foram comparadas à referência primária
[shadPS4 memory.cpp](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/memory.cpp),
consultada em 2026-10-16. Implementação própria sobre Win32; nenhum código
da implementação de referência foi incorporado.

## Subconjunto implementado

| Serviço | NID | Comportamento |
|---|---|---|
| sceKernelMapNamedFlexibleMemory | mL8NDH86iQI | VirtualAlloc, registro de propriedade e publicação do endereço |
| sceKernelMunmap | cQke9UuBQOk | Liberação completa de mapeamento pertencente ao contexto |
| sceKernelMlock | 3k6kx-zOOSQ | VirtualLock em intervalo contido em um mapeamento do contexto |

Somente função libkernel/biblioteca versão 1/módulo 0x0101 é resolvida.
Comprimentos precisam ser positivos e múltiplos de 16 KiB. Map aceita
endereço inicial nulo e flags zero. Proteções CPU suportadas: 0 (sem acesso),
1 (leitura) e 3 (leitura/escrita). Endereços são alinhados pelo Windows a
64 KiB, atendendo ao requisito de 16 KiB. Memória comprometida nasce zerada.
Nomes devem ser legíveis, não nulos e terminados em até 32 bytes incluindo NUL.

Ponteiros de saída são conferidos como graváveis, inclusive quando cruzam
regiões virtuais. Publicação usa WriteProcessMemory; falha descarta o novo
mapeamento. A tabela mantém nome, tamanho e proteção; não depende do buffer
de nome depois da chamada. Pedidos malformados não alteram a saída.

Contexto diagnóstico padrão: quota lógica de 512 MiB e até 256 regiões
vivas. Esses são limites do host, não do PS4. Windows pode reservar espaço
virtual adicional por sua granularidade; a quota conta os comprimentos
pedidos. Em testes usa-se quota menor para verificar esgotamento e restituição.

Flags, hints/endereço fixo, GPU/execução e liberação parcial ainda não são
suportados: retornam ENOTSUP. Tamanho inválido, overflow e memória sem
proprietário retornam EINVAL; ponteiros/proteções inacessíveis retornam
EFAULT; nome longo retorna ENAMETOOLONG; quota/alocação retorna ENOMEM.
São erros no formato SCE (0x80020000 | errno), para esse subconjunto.
Não há alteração de proteção após map, reservas guest em endereço fixo,
memória direta/GPU, split de intervalos, munlock ou equivalência completa
com gerenciamento virtual PS4.

Mlock não é um stub: sucesso significa que VirtualLock retornou sucesso.
Se Windows recusar por working-set/recursos, retorna ENOMEM. Não são
alterados privilégios ou limites do processo. O teste admite essa falha
legítima, mas não infere sucesso do bloqueio físico. A liberação do
mapeamento também libera o bloqueio host associado.

## Lifetime, propriedade e testes

O controlador cria o contexto antes de workers e o destrói após join,
antes de descarregar módulos. A tabela é serializada por mutex e só contém
mapeamentos criados por esta API. Munmap nunca libera código ELF, memória
do Windows ou de outro subsistema. Na destruição são liberadas as regiões
restantes e removido o contexto; contextos nested/simultâneos são recusados.
O contexto foi conectado ao loader junto dos escopos de heap e threads.

O teste chama os três endereços obtidos do resolver e verifica:

- identidade, versões e tipo do export;
- alinhamento, zero fill integral e escrita nos extremos;
- proteções observadas via VirtualQuery, incluindo no-access;
- quota, restituição, limite de 256 regiões, nome, tamanho e flags inválidos;
- ponteiro de saída read-only, ponteiro de nome inválido e memória externa;
- mlock dentro da região, fora dela e em páginas no-access;
- recusa de unmap parcial e double unmap, preservando dados após erro;
- dois workers em ciclos de map/escrita/unmap;
- limpeza de região não liberada explicitamente e contexto novo sem estado antigo.

Fixtures do linker também conferem os três novos NIDs. Link real:
relative=30885, local=1313, guest_bindings=683, **host_bindings=61**,
tls_relocations=27, **pending=520** (todos símbolos). Na libc restam
**47 referências a 46 símbolos distintos**. Originais e ELFs não foram alterados.

## Próximo passo

Fechar os caminhos de pré-inicialização e dos construtores, identificando
quais referências restantes são de fato alcançadas, além dos acessos TLS
e requisitos de execução. Os testes atuais não justificam chamar DT_INIT
real ainda. O suporte a memória agora pode ser usado por esse trabalho;
alocador da libc e callbacks reais continuam sem execução.

```bat
out\build\win-clang-debug\lbp3_host.exe --memory-self-test
ctest --preset win-clang-debug --output-on-failure
```
