# APIs PS4 create/join: objetivo concluído nos testes

Implementadas entradas SysV registradas pelo resolver estrito libkernel,
biblioteca versão 1, módulo versão 0x0101 e STT_FUNC:

- scePthreadCreate: NID `6UgtwV+0zb4`; output handle, attr, entry, arg, name.
- scePthreadJoin: NID `onNY9Byn-W8`; handle e ponteiro opcional para resultado.

Assinaturas e códigos de erro foram confrontados com o runtime_thread.c do
ISS fixado em `a065f561aef8544df2d0cf711dea93881afd04c3`. A implementação foi
escrita aqui; não foram copiados os fontes do runtime ISS.

## Contrato implementado

Somente atributos padrão (`attr == nullptr`), threads joinable e término por
retorno normal da entrada. Atributos não nulos são recusados com ENOTSUP,
sem fingir suporte a stack, prioridade, afinidade ou detach. Nomes opcionais
são validados como strings terminadas em até 32 bytes; ainda não configuram
um nome de thread no agendador Windows.

O handle é um token de 64 bits (geração/índice), não um ponteiro nem HANDLE
do Windows. Há 64 slots por runtime. O slot é invalidado depois do join e
sua geração avança; uma geração que retorna a zero aposenta o slot. Os tokens
são locais ao runtime e não devem ser transportados entre runtimes diferentes.

Create prepara TLS antes de liberar a entrada. Um gate garante que o handle
e o registro estejam publicados antes do callback. O worker herda o runtime,
recebe TLS independente e pode chamar create/join para seus próprios filhos.
O argumento continua emprestado: deve viver até a conclusão. O resultado não
pode apontar para TLS liberado ao terminar a thread.

A tabela é protegida por mutex. Join reserva exclusivamente o registro e
espera fora do lock. Um segundo join concorrente é recusado. Após conclusão,
o resultado é copiado, o slot invalidado e o registro liberado. Saída inválida
detectada antes da espera deixa o handle disponível para tentar novamente.

Ponteiros de saída são verificados para memória comprometida gravável e
copiados por WriteProcessMemory. Nome é lido por ReadProcessMemory, e a entrada
precisa estar em memória executável. Isso não constitui isolamento completo
de arena guest nem validação de que o endereço seja uma função apropriada.

| Situação | Erro SCE |
|---|---|
| Output/entry nulos, entrada não executável, nome sem terminação | EINVAL (22) |
| Atributos não padrão | ENOTSUP (45) |
| Ponteiro inacessível ou saída não gravável | EFAULT (14) |
| Handle desconhecido/antigo, incluindo join já consumido | ESRCH (3) |
| Self-join | EDEADLK (11) |
| Segundo join concorrente | EINVAL (22) |
| Tabela cheia/runtime fechando/falha de criação host | EAGAIN (35) |
| Falha de alocação | ENOMEM (12) |
| Falha interna durante join | EIO (5) |

Os números são codificados no domínio `0x80020000 | errno`. Entradas ABI são
noexcept e não propagam exceções C++ através da fronteira.

O runtime acompanha workers ainda sem join e espera por eles na destruição,
fora do lock. Não abandona threads em background. Uma entrada que nunca
retorne pode bloquear a destruição. O proprietário não deve destruir o runtime
concorrentemente com chamadas de controle externas nem de dentro de seus workers.
Ciclos de espera entre threads distintas ainda não são detectados.

## Validação concluída

O teste `--pthread-self-test` obtém create/join pelo resolver e chama seus
endereços usando assinaturas SysV. Verifica:

- Duas entradas simultâneas, resultados individuais e TLS separado entre elas e o pai.
- Worker tenta self-join e recebe EDEADLK, sem bloquear.
- Thread cria filha, faz join e preserva seu próprio TLS.
- Duas threads de controle disputam join: uma espera, outra recebe EINVAL.
- Handle consumido é recusado; slot reutilizado tem geração diferente.
- Resultado opcional, resultado inacessível com tentativa posterior válida.
- Entradas/output nulos, endereço de entrada não executável, nome inacessível,
  output inacessível e página de output somente leitura.
- Atributos não padrão, esgotamento dos 64 slots e posterior liberação.
- Destruição do runtime espera por worker sem join e restaura o contexto anterior.
- Fixtures de linker conectam ambos os NIDs reais em destinos sintéticos.

Build clang-cl x64 e oito conjuntos CTest passaram. Nos módulos LBP3, seis
novos registros foram conectados (create/join em eboot, Fios2 e libc):
`host_bindings=14`, `pending=573`, `tls_relocations=27`. Restam 567 referências
a símbolos e seis RELATIVE especiais. Nenhum código do jogo foi executado.

## Limites restantes

Sem pthread_exit, detach, cancelamento, atributos personalizados, stack guest
própria, prioridades/afinidade, finalizadores TLS, TCB/DTV completo ou tratamento
das instruções FS/GS originais. A pilha é a de std::thread/Windows: o teste não
prova compatibilidade com red zone, guard pages ou exceções do executável LBP3.
O objetivo de criação/join via APIs PS4 funcionando em testes está concluído
no subconjunto padrão descrito acima; isso não autoriza concluir que as threads
do jogo completo já possam executar.
