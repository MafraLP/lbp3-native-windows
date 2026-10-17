# Representação de mutex acessível pela libc

Implementação: `src/abi/mutex.cpp`. Build clang-cl x64 e 12/12 conjuntos
CTest passaram após as alterações, incluindo as verificações abaixo.
Nenhuma instrução da libc original foi executada.

## Evidência e escopo

No ELF libc com SHA-256
`9811633cd5d49ae461ec0550468db5ef037d576abe1d732348844b930f397566`,
os pares de instruções em 0x108c/0x108f e 0x242e/0x2431 leem o handle
produzido por scePthreadMutexInit e fazem `orl $0x100, 0x20(%rax)`.
Os recortes estão em `heap-init-disassembly.txt` e
`heap-secondary-mutex-disassembly.txt`. A busca no disassembly completo
por essa operação encontrou esses dois locais; ela não prova ausência de
outros acessos indiretos ou de instruções com outra codificação.

Agora os handles de mutex são endereços registrados de memória guest RW,
inicialmente zerada, com uma palavra de 32 bits acessível no offset 0x20.
A escrita observada é preservada pelas operações de lock/unlock. A fixture
SysV `mark_libc_mutex` reproduz exatamente as duas instruções com assembly
próprio, sem chamar a libc original.

Não foi atribuído significado não comprovado ao bit 0x100: ele fica armazenado
no prefixo e não altera a sincronização host. Esta é compatibilidade com os
acessos observados, **não validação de todo o layout privado ou semântica de
libkernel**, nem uma declaração de que a libc já pode iniciar.

## Estado host e validade

Owner, profundidade de recursão, waiters, condition_variable e tipo continuam
em um objeto host separado. Nenhum desses campos é exposto nos bytes guest.
O resolver da API aceita somente o endereço exato de um objeto vivo na tabela;
não dereferencia ponteiros recebidos como handles para decidir sua validade.
Ponteiros internos, falsificados, de atributos e destruídos são recusados.
Handles de atributos continuam usando tag e geração.

Há até 256 mutexes vivos. Uma arena reserva 16 MiB, com até 4096 endereços
distintos de uma página cada por processo. As páginas são comprometidas sob
demanda. Destruir retira o registro e descompromete a página, preservando a
reserva. Endereços emitidos nunca são reutilizados, inclusive após falha de
publicação. Isso impede que um handle antigo se torne válido por reutilização
do endereço; atingir o orçamento de endereços retorna EAGAIN. É um limite
explícito do runtime diagnóstico, não um limite PS4. A reserva é liberada na
saída do processo. Um futuro runtime de longa duração precisará rever esse
orçamento sem reintroduzir reutilização insegura de handles.

A tabela continua serializando inicialização, validação, publicação e
destruição. Destruir com owner ou waiters retorna EBUSY. Esperas liberam o
lock da tabela. O contrato exige que o chamador sincronize escritas diretas
no prefixo com destruição; escrever após destruir continua sendo uso inválido.

## Validação

- Acesso real por instruções SysV ao offset 0x20, leitura do bit e preservação
  após lock/unlock; mutex marcado compartilhado com 4000 incrementos em dois
  workers sem perda de atualização.
- Inicialização estática concorrente; materialização estática também produz
  ponteiro com prefixo acessível.
- Ponteiros falsos e internos recusados; página destruída em estado reservado
  sem acesso; 64 ciclos seguintes não reutilizam o endereço antigo.
- Ciclos até esgotar o orçamento de endereços: EAGAIN sem publicar handle.
  Essa fixture esgota intencionalmente o orçamento no processo de teste.
- Mantidos testes de recursão, ownership, self-deadlock, busy, timeout,
  atributos, saída read-only, capacidade de 256 mutexes e limpeza.

## Próximo bloqueio

`_sceKernelRtldSetApplicationHeapAPI` continua pendente no caminho inicial
(PLT 0xf0, chamada em 0x88a). É necessário validar o contrato e o lifetime da
tabela de callbacks e implementar testes antes de ligá-la. Ainda falta fechar
as chamadas alcançáveis pelos 21 construtores. Não houve novos bindings nesta
etapa: continuam 524 registros pendentes globais, 51 na libc.
