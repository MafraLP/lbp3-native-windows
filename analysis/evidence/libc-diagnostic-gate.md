# Rastreamento corrigido, diagnósticos fatais e máscara de sinais

Resultado: **22/22 conjuntos CTest passaram**; a verificação de prontidão
da libc real retornou **1 / BLOCKED**. Essa recusa foi registrada em
`analysis/IMPLEMENTATION_LOG.md`. Nenhuma instrução da libc ou jogo real
foi executada. Não houve falha de compilação nem crash guest nesta rodada.

## Bug corrigido no rastreador

O grafo antigo seguia o fallthrough após __stack_chk_fail e _exit. Isso
alcançava bytes de dados, que apareciam como opcodes desconhecidos. Agora
os imports correspondentes aos bindings host que terminam o processo não
têm aresta de retorno. A fixture nova prova que dados após a chamada não
são percorridos. Não se aplicou essa hipótese arbitrariamente aos demais
imports ou a callbacks desconhecidos.

O disassembly linear também perdeu dois limites de instrução por causa
de dados intercalados: VAs 0x55100 e 0x56470. O rastreador agora aceita
`--objdump` e decodifica lacunas alcançáveis a partir do endereço correto.
Confere os bytes novos contra o PT_LOAD executável do ELF, limita a 16
rodadas e impõe timeout por processo LLVM. O JSON guarda as lacunas
decodificadas e os imports marcados como noreturn. As mesmas limitações
de fluxo de dados, indiretos e exceções continuam válidas.

## Política de diagnóstico fatal

| Export | NID | Saída do processo |
|---|---|---|
| sceKernelDebugRaiseException | OMDRKKAZ8I4 | 0xe04c4202 |
| sceKernelDebugRaiseExceptionOnReleaseMode | zE-wXIZjLoM | 0xe04c4203 |
| _exit | 6Z83sYWFlA8 | argumento uint32 recebido |

Biblioteca/módulo libkernel, versão 1/0x0101, tipo função. Os handlers
registram nome e código guest em stderr e chamam TerminateProcess.
Não retornam, não provocam unwinding através de frames ELF e não executam
destrutores: a parada é do processo diagnóstico inteiro.

Isso é uma política **fail-stop do host de diagnóstico**, não equivalência
com depurador/exceções PS4. Em particular, as duas APIs de debug podem ter
comportamento de notificação/continuação em ambientes PS4 que não modelamos;
nosso host termina deliberadamente para não continuar após uma falha guest.
Os códigos 0xe04c4202/3 são próprios do host, não códigos PS4.
Os testes chamam os endereços tipados SysV obtidos do resolver em três filhos
descartáveis, verificando mensagens e códigos; não se tentou capturar ou
desenrolar a stack da libc. O contexto opcional de debug não é dereferenciado.

## Máscaras de sinais

Exports sigfillset (`VkTAsrZDcJ0`) e sigprocmask (`aPcyptbOiZs`), mesmas
identidades libkernel. Estrutura guest de quatro uint32 (16 bytes), como
observado nos buffers stack da libc em 0x54673/0x54684 e 0x583b7/0x583e4.
SIG_BLOCK=1, SIG_UNBLOCK=2, SIG_SETMASK=3. Consulta com set nulo ignora how.
SIGKILL=9 e SIGSTOP=17 são removidos da máscara efetiva. Sigfillset ainda
produz todos os bits, como operação sobre um conjunto.

O estado é por thread host. GuestThread copia a máscara do pai na criação;
alterações no filho não alteram o pai. Input é copiado antes de escrever
oldset, permitindo alias. Falha de leitura/escrita não confirma nova máscara.
Erros POSIX retornam -1 com errno guest; sucesso preserva errno. O teste
cobre herança, isolamento, opções, buffer read-only, ponteiros inválidos,
alias, block/unblock/set/query e sinais não bloqueáveis.

**Não há entrega de sinais, fila de sinais pendentes, signal handler guest,
trampolim de retorno ou alteração de handlers Windows.** A máscara é estado
necessário para esse futuro subsistema, não sua implementação completa.
Em particular, bloquear sinais ainda não garante adiar uma interrupção
guest, pois o runtime não gera/entrega tais interrupções. A libc real continua
proibida pelo gate até os requisitos do caminho de execução estarem fechados.

## Verificação real e ponto de retomada

`tools/check_libc_static_gate.py` confere hash do ELF e presença de todas as
raízes de lifecycle revisadas. Produz relatório Markdown e recusa a
inicialização se houver imports pendentes alcançáveis ou barreiras.
Uma aprovação desse gate seria apenas aprovação estática, nunca autorização
automática para executar código. Fixtures verificam sucesso estático mínimo,
recusa por imports/barreiras e rejeição de hashes ou raízes inconsistentes.

Resultado real em `analysis/libc/static-gate.md`: quatro imports potenciais
sem binding: sceKernelWrite, signal, sceKernelGetModuleInfoFromAddr e
_is_signal_return. **74 barreiras indiretas** permanecem. Foram visitados
16.710 endereços; nenhum unknown/gap ficou nesse percurso após as correções.
Isso continua sem ser uma prova de closure: branches são sobreaproximados,
callbacks e tabelas de saltos ainda precisam de análise e exceções não
estão incluídas no grafo. Não afirmar que faltam somente quatro funções.

Link real atualizado: relative=30885, local=1313, guest_bindings=683,
tls_relocations=27, **host_bindings=93, pending=488**. Na libc:
**28 referências pendentes a 27 símbolos distintos**.

Reprodução da verificação que bloqueou:

```powershell
out/unfself/python/python.exe tools/trace_libc_startup.py --elf out/modules/lbp3/libc.elf --disassembly out/libc-full-disassembly.txt --audit analysis/libc/startup.json --output analysis/libc/reachability.json --additional-root 0x55dc0 --additional-root 0x55df0 --objdump 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\llvm-objdump.exe'
out/unfself/python/python.exe tools/check_libc_static_gate.py --elf out/modules/lbp3/libc.elf --audit analysis/libc/startup.json --graph analysis/libc/reachability.json --output analysis/libc/static-gate.md --log analysis/IMPLEMENTATION_LOG.md
```
