# Templates TLS e DTPMOD64 locais

Implementados blocos explícitos de TLS por thread, sem alterar FS/GS ou TEB.
Cada bloco tem ID de módulo não zero, alinhamento, cópia de bytes inicializados
e cauda zerada. O acesso C++ resolve (ID, offset, tamanho) com validação de
intervalo; IDs ausentes e acessos fora do bloco são recusados.

Os templates são obtidos de PT_TLS e, quando há bytes inicializados, exigem
backing consistente no PT_LOAD legível correspondente. São copiados depois
das escritas de relocation, preservando ponteiros já ajustados. Duplicatas e
limites excessivos são recusados. Limites iniciais: 16 MiB por bloco,
64 MiB por thread, alinhamento máximo 1 MiB.

O linker aplica R_X86_64_DTPMOD64 local com addend zero como ID de módulo,
não como endereço. Aceita símbolo zero e os marcadores locais explicitamente
validados. A variante observada nos três PRX é STB_LOCAL/STT_SECTION sem nome,
SHN_UNDEF, value/size/visibility zero. Foi confrontada com os arquivos locais
e com `local_tls` do linker ISS fixado em
`a065f561aef8544df2d0cf711dea93881afd04c3/port/scripts/link_modules.py`.
Marcadores desconhecidos, referências TLS externas e tipos DTPOFF/TPOFF
continuam pendentes. Não foi copiada implementação do ISS.

| Arquivo | ID no conjunto atual | Inicializados | Total |
|---|---:|---:|---:|
| eboot | 1 | 84 | 512 |
| Fios2 | 2 | 0 | 64 |
| libc | 3 | 544 | 1.168 |
| SPU | 4 | 0 | 256 |

Os IDs são atribuídos deterministamente aos módulos com TLS no conjunto
ordenado carregado, não são IDs fixos de firmware. O teste prepara 2.000 bytes
de TLS para a thread de diagnóstico e depois libera tudo.

Resultado real: 27 registros TLS aplicados e verificados; pendências de 610
para 583 (577 referências a símbolos e 6 RELATIVE especiais). Os quatro
bindings host e os demais ajustes anteriores são preservados.

Validação:

- Duas threads reais coexistem com templates iguais; alterações em uma não
  afetam a outra nem o template original. Alinhamento e zeros são conferidos.
- IDs inexistentes, intervalos inválidos e duplicação de IDs são recusados.
- Fixture nativa Orbis escreve ID 1 no destino DTPMOD64 e confere o valor lido.
- Fixture sem backing coerente falha antes das escritas; tipo TLS não suportado
  continua pendente. Todos os seis conjuntos CTest passaram.

Limite essencial: isso prepara a memória e os IDs. Ainda não implementa o
TCB/DTV Orbis, `__tls_get_addr`, acesso FS/GS do código original, lifetime das
threads guest ou callbacks de finalização TLS. Nenhum código do jogo foi chamado.

```bat
out\build\win-clang-debug\lbp3_host.exe --tls-self-test
out\build\win-clang-debug\lbp3_host.exe --link-set out\modules\lbp3
```
