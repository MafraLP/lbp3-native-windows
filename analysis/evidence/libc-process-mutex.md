# Parâmetros reais e família mínima de mutexes: objetivo concluído

Implementações próprias, com contratos/NIDs confrontados com os arquivos do
ISS já auditados (`runtime.c` e `runtime_mutex.c`, revisão
`a065f561aef8544df2d0cf711dea93881afd04c3`). Nenhum fonte do runtime ISS foi copiado.

## Parâmetros do processo

sceKernelGetProcParam, NID `959qrazPIrg`, retorna o endereço do bloco
PT_SCE_PROCPARAM do executável principal. O loader verifica que os bytes
correspondam a um PT_LOAD legível e ao mesmo offset de arquivo, então valida
size >= 0x40, size dentro do segmento, magic ORBI (0x4942524f) e versão 1.
O contexto é instalado após relocations e antes dos workers; é removido depois
que o runtime de threads é destruído, antes de liberar a imagem.

No LBP3 o bloco fica em VA 0xf44000, tem 64 bytes e os qwords observados no
arquivo são: 0x40, 0x14942524f, 0x1700081, 0, 0, 0, 0, 0.
Os campos e ponteiros são preservados, incluindo zeros. Não há fabricação de
estrutura de heap ou inferência de que os campos opcionais deveriam estar preenchidos.

O ponteiro é process-wide e consultado atomicamente, sem usar o TLS do Windows
como parâmetros de processo. Configuração/restauração deve ocorrer na thread de
controle antes/depois dos workers, sem sobrescrever o contexto de um processo
guest ativo em paralelo. O storage mapeado deve durar mais que o contexto.
Sem contexto, a consulta retorna nullptr. Não existe ainda runtime persistente
de jogo: o contexto atual dura somente durante o diagnóstico de ligação.

## Mutexes e atributos

Entradas SysV: scePthreadMutexattrInit/Destroy/Settype/Setprotocol,
scePthreadMutexInit/Destroy/Lock/Unlock/Trylock/Timedlock.
O resolver exige STT_FUNC, libkernel/biblioteca 1 e módulo libkernel 0x0101.

- Handles são tokens de 64 bits com tipo, geração de 24 bits e índice;
  não são ponteiros ou HANDLEs Windows. Há 128 atributos e 256 mutexes.
- Mutexes copiam o tipo do atributo na criação; destruir o atributo depois
  não altera o mutex. Tipos ERRORCHECK=1, RECURSIVE=2 e DEFAULT=4 são aceitos;
  DEFAULT usa ERRORCHECK neste subconjunto. NORMAL=3 retorna ENOTSUP.
- Setprotocol aceita NONE=0. Inherit/protect (1/2) retornam ENOTSUP, sem fingir
  que o agendador Windows implementa a política PS4.
- Handles 0/1 de inicialização estática são materializados uma única vez sob
  o lock da tabela. Destruição grava sentinela 2 e invalida a geração.
- Proprietário, profundidade recursiva e waiters são controlados sob mutex
  host. Esperas usam condition_variable e liberam o lock da tabela. Destruição
  de mutex ocupado/com waiters retorna EBUSY.
- Timedlock usa prazo relativo em microssegundos; timeout retorna ETIMEDOUT.
- Ponteiros são lidos com ReadProcessMemory; escritas exigem backing comprometido
  gravável antes de WriteProcessMemory. Nome opcional tem limite de 32 bytes e
  é apenas validado; não altera nomes de objetos no Windows.
- Atributos/mutexes são process-wide nesta versão e precisam de Destroy para
  liberar o slot. Não há protocolo robusto de recuperação de owner morto,
  cancelamento de esperas, prioridades ou limpeza automática de mutexes vivos.

| Caso | Erro SCE |
|---|---|
| Handle/tipo inválido, uso de sentinela destruída | EINVAL (22) |
| Ponteiro inválido/saída sem escrita | EFAULT (14) |
| Mutex ocupado, trylock contendido, init de objeto já vivo | EBUSY (16) |
| Lock pelo proprietário em ERRORCHECK/DEFAULT | EDEADLK (11) |
| Unlock por outra thread ou mutex sem proprietário | EPERM (1) |
| Timedlock contendido expira | ETIMEDOUT (60) |
| Tipo NORMAL/protocolo de prioridade | ENOTSUP (45) |
| Tabela cheia/profundidade recursiva esgotada | EAGAIN (35) |
| Falha de alocação | ENOMEM (12), quando distinguível |
| Falha interna inesperada | EIO (5) |

## Validação

`--libc-services-self-test` obtém ProcParam pelo resolver e os testes de mutex
chamam ponteiros SysV retornados pelo resolver de NIDs. Foram conferidos:

- Bloco válido, consulta em outra thread, escopos aninhados e remoção de contexto.
- Tamanho, magic, versão e truncamento inválidos.
- Fixture ELF de processo com bloco real no PT_LOAD e recusa de magic incorreta.
- Init/destroy de atributo, cópia de atributos, recursão, ERRORCHECK e handles antigos.
- Unlock por outra thread, trylock ocupado e timedlock expirado.
- Duas threads fazendo 4.000 incrementos compartilhados sem perda.
- Inicialização/destruição estática, tipo errado de handle e página somente leitura.
- Esgotamento e liberação das tabelas completas de atributos e mutexes.
- Ligações dos NIDs de parâmetros e mutexes em fixtures nativas.

Build clang-cl x64 e onze conjuntos CTest passaram. Nos oito módulos LBP3,
o bloco real foi validado e 29 novas referências foram conectadas:
`host_bindings=50`, `pending=531`, `tls_relocations=27`.
Na libc restam 52 referências a 51 símbolos distintos. Nenhuma função da libc
real ou do LBP3 foi executada. A família de mutexes não substitui os serviços
de heap, finalização, sinais e outros imports ainda pendentes.
