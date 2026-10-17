# Investigação da inicialização da libc LBP3

Marco atual: libc, seis auxiliares e construtores/destrutores do eboot passaram
em subprocesso (`out/libc-probe/constructors-04`). A entrada principal chegou à
abertura de VideoOut (`out/libc-probe/entry-04`) e parou no import ainda ausente.
São 31 conjuntos CTest aprovados, 409 referências pendentes no conjunto e nove
na libc. Os relatos abaixo são históricos.

Atualização: DT_INIT da libc real retornou 0 em subprocesso diagnóstico com os
oito ELFs fixados por SHA-256. Passaram 36 alocações de 1 byte a 1 MiB e 256
alocações em dois pthreads guest concorrentes. Não houve acionamento das
armadilhas de imports desconhecidos. O entry do eboot não foi chamado.
Evidência: `out/libc-probe/attempt-05/stdout.txt`; histórico em
[IMPLEMENTATION_LOG.md](../IMPLEMENTATION_LOG.md).

26/26 conjuntos CTest aprovados. Ligação normal: 432 registros pendentes.
O resolver diagnóstico inclui armadilhas e reservas NOACCESS; resolução nesse
modo não significa implementação dos imports. As análises abaixo são registros
históricos; o gate estático não cobre os caminhos indiretos exercitados agora.

Estado mais recente: [gate estático bloqueado](static-gate.md), após corrigir
o rastreador e implementar diagnósticos fatais e máscaras de sinais.
22/22 conjuntos CTest aprovados; 488 pendências globais, 28 na libc.
Quatro imports potenciais e 74 indiretos ainda impedem validar o bootstrap.
Contrato e limitações: [resultado detalhado](../evidence/libc-diagnostic-gate.md).

Atualização autônoma: [Once, chaves, memória POSIX e condições](../evidence/thread-services-and-conditions.md).
19/19 conjuntos CTest aprovados, 493 pendências globais e 33 na libc.
Sinais, diagnóstico/unwinding e indiretos ainda impedem validar DT_INIT.
Falhas e limitações ficam em `analysis/IMPLEMENTATION_LOG.md`.

Estado atual: [dependências potenciais dos construtores](constructor-dependencies.md).
Rastreador estático testado, __error/getpagesize implementados. Restam 516
pendências globais e 45 na libc; 18 imports pendentes aparecem nos caminhos
potenciais analisados. Indiretos continuam impedindo fechar a análise.

Estado mais recente: [memória flexível para o heap](flexible-memory.md).
MapNamedFlexibleMemory/Mlock/Munmap têm um subconjunto implementado e testado.
Restam 520 pendências globais e 47 na libc. A libc segue sem execução;
falta fechar dependências e requisitos dos construtores.

Estado atual: [registro de callbacks de heap](heap-api-registration.md)
implementado e testado no subconjunto observado. Restam 523 pendências
globais e 50 na libc. A libc continua sem execução; serviços de memória e
dependências dos construtores ainda precisam de validação.

Atualização seguinte: [representação guest de mutex](mutex-guest-layout.md).
O acesso direto ao offset 0x20 agora está coberto por implementação e testes.
Isso não valida todo o layout privado; o registro da API de heap permanece
pendente e a libc ainda não foi executada.

Estado mais recente: [resultado da preparação controlada](controlled-init-result.md).
São 524 pendências globais e 51 na libc. O handler `__stack_chk_fail` foi
implementado e testado; a execução foi impedida por um acesso direto ao
layout privado do mutex no heap (VA 0x108f), incompatível com os tokens atuais.
Os detalhes abaixo documentam a investigação anterior.

ELF SHA-256: `9811633cd5d49ae461ec0550468db5ef037d576abe1d732348844b930f397566`.
Análise estática e testes sintéticos: nenhuma função da libc real foi executada.

Atualização: parâmetros reais do processo e família mínima de mutexes foram
implementados e testados. Resultado atual: 531 pendências globais e 52 na libc.
Contrato e limites: `analysis/evidence/libc-process-mutex.md`.

## Entradas e sequência observada

- DT_INIT está presente e vale **0x0**. Isso é uma VA ELF válida na região RX,
  não ausência de entrada. O endereço host é load_bias + 0.
- DT_FINI: 0x7fca0.
- PREINIT_ARRAY: 0xbaa00, 8 bytes. O valor bruto é zero, mas uma relocation
  RELATIVE aponta o slot para 0xa100. Ler só os bytes em disco perderia a função.
- INIT_ARRAY e FINI_ARRAY estão presentes com tamanho zero.
- O wrapper em 0x0 percorre a região de pré-inicialização e uma lista legada
  de construtores terminada por sentinela. Também examina o terceiro argumento
  como possível callback, antes de um fallback para 0xa140 (xor eax,eax; ret).
  A assinatura e os argumentos de bootstrap ainda precisam ser confirmados.

Portanto, não basta chamar uma função chamada module_start nem descartar
DT_INIT quando seu valor é zero. O teste de metadados diferencia tag ausente,
tag presente com VA zero e VA fora de código executável. O loader ainda não
oferece execução de entradas reais de módulos.

## Bloqueios confirmados no caminho de pré-inicialização

| Evidência | Dependência/efeito |
|---|---|
| 0xa100 chama 0x620; 0x627 chama PLT 0xe0 | sceKernelGetProcParam, implementado com o bloco validado do eboot |
| 0x620 em diante valida tamanho e ponteiros do proc param, com caminhos de fallback | Exige modelar o proc param e decidir o contrato correto; não foi substituído por dados inventados |
| 0xa100 chama 0x30180 | Próximo trecho de inicialização observado |
| 0x3018b lê o slot 0xb4498 e dereferencia o ponteiro | __stack_chk_guard, corrigido nesta etapa |
| 0x301bd chama PLT 0x130 | scePthreadMutexInit, implementado no subconjunto testado |
| 0x301dc/0x3023b chamam PLT 0x110 se o guard divergir | __stack_chk_fail, ainda pendente; precisa encerrar/diagnosticar falhas, não retornar sucesso |
| 0xa10b pode alcançar PLT 0x100 no caminho de erro | sceKernelDebugRaiseException, ainda pendente |
| 0xa126 salta para 0x301f0, que chama 0x30250 | Registro/continuação da inicialização; closure de chamadas ainda não completamente analisada |

O inventário inclui também chaves TLS, once, heap,
mapa de memória, arquivos, sinais e dados __progname. A presença desses imports
não prova que todos sejam chamados nesta inicialização. A ordem completa e os
alvos indiretos ainda não foram estabelecidos.

`startup.json` contém metadados, thunks PLT associados a imports, os seis
registros RELATIVE observados e a lista atual de pendências da libc:
**52 referências a 51 símbolos distintos**. Não é uma lista de 51 funções:
há dados e nomes ainda sem correspondência.

## Correções implementadas e testadas

### RELATIVE com índice de símbolo não zero

Os seis casos tinham addends iguais aos valores dos respectivos símbolos,
mas a regra correta não é somar o símbolo novamente: **B+A**, ignorando-o.
Isso é confirmado tanto pelo linker do ISS auditado quanto pelo shadPS4.
O parser ainda valida a consistência das tabelas e índices; não flexibilizamos
limites ou validação de alvos para aplicar a correção.

Uma fixture usa um símbolo externo não resolvido pelo mesmo endereço-base e
comprova que a relocation usa a base do módulo requisitante, não a do provedor
nem o valor do símbolo. Testes prévios de overflow/underflow continuam ativos.

Referências de formato/implementação:
- https://github.com/HayatoG/iss_pc/blob/a065f561aef8544df2d0cf711dea93881afd04c3/port/scripts/link_modules.py
- https://github.com/shadps4-emu/shadPS4/blob/main/src/core/linker.cpp

### __stack_chk_guard

Adicionado export de dados STT_OBJECT libkernel, NID `f7uOxY9mM1U`, biblioteca
versão 1 e módulo versão 0x0101. Fornece o **endereço de um uintptr_t** de
lifetime de processo, inicializado com BCryptGenRandom, não o valor como se
fosse endereço e não um callback. O valor não é registrado nos logs.
Falha do gerador ou valor zero impede a inicialização do runtime.

O teste host confere valor não zero, endereço/valor estáveis após initialize
repetido e recusa de resolução como função. A fixture ELF executável importa
e dereferencia o objeto, exigindo valor não zero antes de testar TLS e retornar.
As fixtures de linker também distinguem o tipo de objeto do tipo de função.
O guard permite verificações de canário; não substitui __stack_chk_fail.

## Validação e resultado

- Build clang-cl x64 e onze conjuntos CTest passaram na atualização atual.
- Seis RELATIVE adicionais aplicados: relative=30885.
- Sete novas referências ao guard conectadas: host_bindings=21.
- A investigação inicial reduziu as pendências de 573 para 560. A implementação
  seguinte de ProcParam/mutexes reduziu para 531; todas são referências a símbolos.
- Demais números preservados: guest_bindings=683 e tls_relocations=27.
- Originais e ELFs no disco não foram alterados pelas relocations.

## Próximo bloqueio concreto

O contrato mínimo de sceKernelGetProcParam e mutexes foi concluído nos testes.
Ainda é necessário confirmar argumentos e listas legadas de construtores,
avaliar os demais imports alcançáveis e os handlers de falha antes de qualquer
chamada ao DT_INIT real.
Os handlers atuais de falha sintética não comprovam unwinding de frames da libc.

Artefatos de disassembly são recortes produzidos por llvm-objdump a partir do
ELF sectionless, usando PT_LOAD. Não são uma análise completa de alcançabilidade.
`init-callee-disassembly.txt` contém o fallback e funções vizinhas; os arquivos
`preinit-*.txt` contêm os trechos citados na tabela.

Reprodução:

```bat
out\unfself\python\python.exe tools\audit_libc_init.py --elf out\modules\lbp3\libc.elf --pending analysis\imports\pending\pending.json --output analysis\libc\startup.json
ctest --preset win-clang-debug --output-on-failure
```
