# Ciclo de threads com TLS

Criada a primitiva host `abi::GuestThread`, independente das tabelas de handles
e dos contratos scePthread. A entrada é uma função `void* (sysv_abi *)(void*) noexcept`.
Ela só foi utilizada com fixtures sintéticas compiladas pelo nosso toolchain.

Sequência implementada:

1. Copiar os templates para armazenamento próprio e criar a thread Windows via std::thread.
2. Criar os blocos TLS, anexar o contexto da thread e confirmar a preparação ao criador.
3. Chamar a entrada SysV com argumento e guardar o ponteiro de retorno.
4. Destacar o contexto, liberar TLS e concluir. `join()` sincroniza e devolve o resultado.

Erros na preparação são comunicados ao criador depois de esperar a thread
terminar; a entrada não é chamada. Argumentos são emprestados e precisam viver
até a conclusão da thread. Os templates são copiados; o criador pode liberá-los
ou alterá-los depois da construção. Não se deve devolver ponteiro para o TLS
da própria thread, que já estará liberado quando join retornar.

Entradas nulas e join repetido/sobre a própria thread são recusados. O destrutor
aguarda uma thread ainda ativa. Não há detach, cancelamento ou terminação forçada;
uma entrada que não retornar fará join/destruição esperar. Criar, usar e destruir
o objeto é responsabilidade da thread de controle; autodestruição é inválida.

Teste `--thread-self-test`:

- Duas entradas SysV coexistem com blocos TLS distintos, inicializados e zerados.
- Cada entrada altera seu bloco e preserva seu valor após uma barreira.
- O TLS da thread principal continua anexado e inalterado.
- Os dois valores de retorno são conferidos após join.
- Falha de template não alcança a entrada; entrada nula e join repetido são recusados.
- O destrutor espera a conclusão de uma entrada simples.
- O wrapper verifica que o contexto TLS não permanece anexado após liberar os blocos.

Build clang-cl x64 e sete conjuntos CTest passaram. Não foram conectados novos
NIDs: `pending=579`, `host_bindings=8` e `tls_relocations=27` permanecem válidos.

Limites: não implementa scePthreadCreate/Join, handles e atributos Orbis,
pthread_exit, cancelamento, finalizadores de chaves TLS, TCB/DTV completo,
red zone do código original ou captura de falhas de instruções do jogo.
Exceções C++ não podem atravessar a entrada SysV. Nenhum código LBP3 foi executado.

Próximo trabalho: definir handles guest e o subconjunto de atributos/erros
necessário para expor criação/join pela API PS4, testando antes com callbacks
sintéticos e sem chamar entradas dos módulos reais.
