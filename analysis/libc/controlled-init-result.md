# Preparação da inicialização controlada da libc

Resultado: **execução bloqueada por contrato incompatível do mutex**. Nenhuma
entrada, construtor ou função da libc original foi executada nesta etapa.
ELF analisado: SHA-256
`9811633cd5d49ae461ec0550468db5ef037d576abe1d732348844b930f397566`.

## Entrada e sequência confirmadas

O wrapper DT_INIT está em VA 0, usa SysV e preserva RDI/RSI para uma possível
continuação em RDX. O tipo de chamada de referência é
`int(size_t args, const void* argp, void* param)`; a implementação de referência
não estabelece, sozinha, o significado de `param` nesta versão da libc.
Nesta imagem, um RDX não nulo é chamado como função: portanto não se deve
passar o bloco de parâmetros do eboot nesse registrador. Uma futura tentativa
sem argumentos/callback usaria `(0, nullptr, nullptr)`, condicionada à validação
das dependências e do ambiente. Isso ainda não é um contrato de bootstrap completo.

Referência primária consultada em 2026-10-16, somente para comparar contrato,
sem copiar implementação:
[shadPS4 Module::Start e EntryFunc](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/module.cpp).
A interpretação de RDX como continuação vem do disassembly local, VA 0x77–0x94.

O próprio wrapper já executa PREINIT_ARRAY: chamar essa lista separadamente
duplicaria a pré-inicialização. O slot 0xbaa00 é relocacionado para 0xa100.
Depois percorre a lista legada de trás para frente, começando em 0xba9e0,
até a sentinela -1 em 0xba938. Foram confirmados **21 alvos executáveis**,
com ordem registrada em `startup.json` (`legacy_constructors.call_order`).
A validação dos endereços não comprova todos os efeitos desses construtores.

## Bloqueio comprovado no caminho inicial

1. 0xa100 chama 0x620. Com o ProcParam real, o campo +0x38 é nulo;
   o fluxo chega a 0x883. Esse fallback não evita os serviços de heap.
2. 0x88a chama PLT 0xf0: `_sceKernelRtldSetApplicationHeapAPI`, NID
   `p5EcQeEeJAE`, ainda não implementado. RDI aponta para a tabela em 0xb4ab8.
   Não foi inserido retorno de sucesso fictício.
3. 0x89c chama 0x5b9d0; com RBX nulo, esse helper retorna zero.
4. 0x90d salta pelo slot 0xb4a58, cuja relocation aponta para 0xad0.
   Esse wrapper entra em 0xfc0, inicialização do heap.
5. 0x105a inicializa atributos de mutex; 0x107f chama scePthreadMutexInit.
   Em 0x108c lê o handle produzido; **0x108f faz OR de 0x100 no endereço
   handle + 0x20**, sem passar por uma API host.

Os mutexes atuais retornam tokens com tag/geração, não ponteiros para uma
estrutura guest. A instrução em 0x108f tentaria acessar memória inválida.
Os testes de mutex continuam corretos para o subconjunto da API, mas não
estabelecem compatibilidade com o layout privado exigido por esta libc.
Trocar o token por um ponteiro sem confirmar os demais campos e acessos
também não resolveria o contrato. Esse é o próximo trabalho concreto:
inventariar os acessos ao objeto e implementar uma representação guest estável,
com validade, destruição, concorrência e testes contra os acessos observados.

Trechos reproduzíveis: `heap-dispatch-disassembly.txt`,
`heap-entry-disassembly.txt`, `heap-init-disassembly.txt`.
`exit-registration-disassembly.txt` mostra o registro de callback posterior:
o primeiro registro usa armazenamento estático; o caminho de expansão pode
alocar. Não foi declarado que toda a closure dos 21 construtores está validada.

## Correções e testes concluídos

- Resolvido `__stack_chk_fail`, NID `Ou3iL1abvng`, identidade libkernel
  versão 1/módulo 0x0101 e tipo função. O handler SysV registra a falha em
  stderr e termina o processo com código diagnóstico próprio `0xe04c4201`.
  Não retorna e não tenta unwinding dos frames ELF. O código não é erro PS4.
- Teste chama o endereço obtido do resolver em filho descartável e verifica
  diagnóstico, código de término e ausência de retorno. Tipo de dados e
  versão incorreta são recusados antes da chamada.
- Auditoria de construtores agora resolve relocations RELATIVE, ignora slots
  nulos e exige sentinela em varredura limitada e alvos executáveis.
  Fixtures cobrem ordem reversa, ausência de sentinela, desalinhamento,
  ponteiro externo não resolvido e alvo fora de código.
- Build clang-cl x64 e **12/12 conjuntos CTest passaram**.
- Link real atualizado: relative=30885, local=1313, guest_bindings=683,
  host_bindings=57, tls_relocations=27, **pending=524**, todos símbolos.
  Na libc: **51 referências pendentes a 50 símbolos distintos**.

O handler não é um supervisor completo de execução. Ainda faltam o contrato
de heap/mutex, os serviços alcançáveis e a validação de todos os construtores;
acesso TLS por instruções originais, exceções e proteção de red zone também
não foram comprovados para este fluxo. A etapa termina sem executar a libc,
conforme a condição de só executar após validação dos requisitos.

Reprodução da auditoria:

```bat
out\unfself\python\python.exe tools\audit_libc_init.py --elf out\modules\lbp3\libc.elf --pending analysis\imports\pending\pending.json --output analysis\libc\startup.json --legacy-last-slot 0xba9e0
ctest --preset win-clang-debug --output-on-failure
```
