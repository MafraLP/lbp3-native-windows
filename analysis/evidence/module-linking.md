# Relocations e bindings nativos: primeira etapa

O host oferece `--link-set out/modules/lbp3`. Ele mantém todos os módulos
residentes, lê as tabelas dinâmicas de cada ELF e aplica registros suportados.
As imagens no disco permanecem inalteradas. O teste termina liberando as reservas.

Validação no conjunto de oito arquivos LBP3:

| Categoria aplicada e verificada | Registros |
|---|---:|
| RELATIVE com símbolo zero | 30.879 |
| Símbolos locais/absolutos | 1.313 |
| Bindings com exports de outros módulos | 683 |
| Total aplicado | 32.875 |
| Pendentes, sem alteração | 614 |

Contagens são de registros de relocation, não de funções únicas. Um mesmo
símbolo pode ser referenciado por várias relocations. Não equivalem aos 204
candidatos por símbolo do inventário anterior.

Regras implementadas:

- Orbis `DT_SCE_*` relativos ao segmento DYNLIBDATA, com limites de tabelas,
  strings e índices. O arquivo fica aberto sem compartilhamento de escrita
  durante mapeamento, análise e aplicação.
- RELA: `R_X86_64_RELATIVE` com símbolo zero usa B+A; `R_X86_64_64` usa S+A;
  `GLOB_DAT` e `JUMP_SLOT` usam S e exigem addend zero nesta implementação.
- Exports externos exigem NID, biblioteca/módulo e versões iguais, visibilidade
  pública/protegida, binding global/fraco e tipo compatível. Mais de um candidato
  compatível causa falha, sem escolher arbitrariamente.
- Alvos de escrita devem estar completamente em um PT_LOAD, não podem se
  sobrepor e têm aritmética checada. Metadados e plano completos são validados
  antes de qualquer escrita. Falha posterior descarta todas as reservas.
- As permissões são alteradas temporariamente e restauradas; o valor escrito
  é relido e comparado, e FlushInstructionCache é chamado.

Casos pendentes incluem imports de host, TLS e variantes ainda não suportadas.
Na libc local foram observados seis RELATIVE com símbolo não zero. Eles foram
mantidos pendentes para não aplicar uma regra ainda sem teste específico.
Não há stubs de sucesso automático nem tentativa de executar código guest.

Testes:

- C++: B+A/S+A positivos, negativos, INT64_MIN, overflow e underflow.
- Fixtures nativas geradas em Python: RELATIVE, referência local e três bindings
  entre dois módulos. Valores relidos da memória são comparados com cálculos
  independentes baseados nas bases ASLR registradas pelo loader.
- Recusa de target fora de PT_LOAD, writes sobrepostos e índice inválido.
- Versão divergente e TLS ficam pendentes.
- Fixtures anteriores de inspeção/mapeamento continuam passando.

Limites: ligação parcial apenas; não há lifetime persistente fora do teste,
inicialização de TLS, runtime ISS integrado, política final de interposição de
símbolos fracos, execução de construtores ou chamada da entrada LBP3.
Mesmo uma fixture com zero pendências não comprova ABI ou inicialização do jogo.

Próxima etapa: classificar e implementar a ponte ABI/TLS e os primeiros serviços
host, tomando como referência os candidatos ISS. Manter as variantes de libc
pendentes até haver fixtures e evidência suficientes para sua semântica.
