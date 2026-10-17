# Registro da API de heap da aplicação

Implementado em `src/abi/heap_api.cpp`, exportado pelo resolver HLE como
`p5EcQeEeJAE`, função libkernel, biblioteca versão 1 e módulo 0x0101.
Build clang-cl x64 e **13/13 conjuntos CTest passaram**.
Nenhuma função da libc original ou do jogo foi executada.

## Evidência e contrato suportado

A libc local passa em RDI o endereço 0xb4ab8 (VA ELF) na chamada de 0x88a
para PLT 0xf0. Não passa tamanho e não usa o valor de retorno. A tabela é
um vetor de endereços, sem cabeçalho de tamanho. A estrutura de referência
do shadPS4 tem dez slots. Consultado e fixado o commit
`32021e41bc52f744c80d122e732e24e97496c98d`, somente para comparação de formato:
[HeapAPI e SetHeapAPI](https://github.com/shadps4-emu/shadPS4/blob/32021e41bc52f744c80d122e732e24e97496c98d/src/core/linker.h).
Não foi copiada implementação do projeto externo.

`startup.json.heap_api_table` registra os dez slots em disco e respectivas
relocations. Nesta imagem, inicialmente só os índices 0, 1 e 6 têm
relocations, para VAs 0x950, 0x970 e 0x9d0. Os demais são nulos. Os wrappers
em 0x950/0x970 encaminham para o alocador local; o terceiro encaminha três
argumentos em 0x9d0. Campos opcionais podem ser substituídos pelo caminho de
ProcParam em 0x730–0x792 e 0x866–0x87c, antes do registro.

O subconjunto implementado exige malloc/free não nulos, permite os outros
oito slots nulos e exige endereço comprometido com permissão executável
para cada callback não nulo. Copia todos os 80 bytes com ReadProcessMemory
e só publica a nova tabela após validar a cópia inteira. Falhas preservam
o registro anterior. O registro não chama nenhum callback.

A função host tipada SysV recebe um ponteiro e retorna status diagnóstico
int32: zero, EINVAL para contexto/tabela obrigatória ausente, EFAULT para
memória ou alvos inválidos, EIO para falhas internas. **Esses códigos e a
assinatura de retorno não foram validados em hardware**; são política do
subconjunto diagnóstico. A compatibilidade observada é com a chamada local,
que ignora RAX. Não se afirma compatibilidade com outros chamadores/firmwares.
As assinaturas dos oito callbacks opcionais não são usadas por este runtime.

## Lifetime e concorrência

ScopedHeapApi é criado pelo controlador antes de iniciar workers e destruído
depois de juntá-los, antes de descarregar código. O loader prepara esse escopo
junto ao contexto de threads. Contextos simultâneos/nested são recusados.
Um mutex serializa publicação, leitura e limpeza do registro.

A tabela copiada não depende do lifetime do buffer de origem. Já os alvos
dos callbacks precisam continuar mapeados enquanto o contexto estiver vivo.
`heap_api_snapshot` entrega cópia para consumidores desse escopo: não concede
posse nem mantém módulos vivos. Não se deve guardar ou chamar essa cópia
após destruir o contexto. A destruição remove o registro, sem invocar callbacks.
Não há consumidores de heap reais habilitados automaticamente, nem fallback
para malloc do Windows. Uma futura integração de alocação precisa preservar
a origem de cada ponteiro para que free use o mesmo alocador que malloc.

## Testes

`--heap-api-self-test` chama o endereço devolvido pelo resolver, não um stub
de sucesso. Confere identidade/tipo/versões, contexto ausente, tabela copiada,
callbacks opcionais nulos, substituição, preservação após erro, ponteiros
inválidos, callback apontando para dados e tabela cruzando página inacessível.
O registro não executa callbacks. Um worker lê a cópia e chama malloc/free
SysV sintéticos, verifica ponteiro, escrita e contadores. Os callbacks acessam
o próprio registro, demonstrando que o lock não fica retido durante chamadas.
O teste confirma limpeza e ausência de estado em um contexto novo.

Fixtures do linker verificam o binding do novo NID. A auditoria Python testa
relocations e slots opcionais nulos, rejeitando tabela fora de PT_LOAD.

Link real atualizado: relative=30885, local=1313, guest_bindings=683,
host_bindings=58, tls_relocations=27, **pending=523** (todos símbolos).
Libc: **50 referências pendentes a 49 símbolos distintos**.

## O que permanece

Registro de callbacks não inicializa o alocador. O heap local usa serviços
de memória ainda pendentes, incluindo `sceKernelMapNamedFlexibleMemory`
(PLT 0x150), `sceKernelMlock` (0x160) e `sceKernelMunmap` (0x170).
A presença desses imports não prova que os três sejam executados em todo
bootstrap: com ProcParam +0x38 nulo, 0xfc0 segue por 0x1309, e outros campos
globais ainda controlam os caminhos de alocação. Próximo trabalho: fechar
esse fluxo e implementar o subconjunto de memória efetivamente necessário,
com testes de alinhamento, proteção, limites e liberação. Os 21 construtores
também ainda exigem fechamento das dependências antes de executar DT_INIT.

Reprodução:

```bat
out\build\win-clang-debug\lbp3_host.exe --heap-api-self-test
out\unfself\python\python.exe tools\audit_libc_init.py --elf out\modules\lbp3\libc.elf --pending analysis\imports\pending\pending.json --output analysis\libc\startup.json --legacy-last-slot 0xba9e0 --heap-table-va 0xb4ab8
ctest --preset win-clang-debug --output-on-failure
```
