# Carregamento dos módulos LBP3

Implementado um teste nativo `--map-set` que mantém o eboot e sete PRX
preparados simultaneamente na memória Windows x64. O teste verifica os bytes
copiados e a BSS, aplica permissões e libera as reservas ao terminar.
Falhas liberam também os módulos previamente mapeados.

Os PRX locais usam SELF sem criptografia/compressão. O novo conversor copia
por índice de segmento, verifica ranges e sobreposições e rejeita dados ausentes.
Os arquivos originais são somente lidos. Entradas/saídas têm SHA-256 no inventário.

Inventário estático dos oito arquivos:

- 813 símbolos importados no total (contagens por módulo, não funções únicas).
- 204 têm um único candidato exportado pelo conjunto, exigindo NID e identidades/versões iguais.
- O eboot tem 161 desses candidatos: libc 130, Fios2 26, quatro módulos FMOD e SPU um cada.
- Restam 609 imports sem binding guest: 448 com candidato estático no ISS,
  41 somente com nome e 120 sem correspondência nessa busca.

Esses números não significam funções ligadas ou compatibilidade comprovada.
O conjunto é mapeado apenas como teste: relocations, import bindings, TLS,
construtores e runtime ISS ainda não são executados. Não foi compilado o ISS.
Imports de módulos locais podem exigir serviços adicionais do host, por isso
não é correto subtrair 161 de 600 e ignorar as dependências dos PRX.

Próximo trabalho: representar os módulos com lifetime persistente, aplicar e
testar relocations com módulos sintéticos e construir bindings a partir dos
candidatos validados. Preparar o teste de ABI/runtime ISS no ambiente CLANG64
antes de tentar executar entradas ou construtores de LBP3.

Artefatos:
- `analysis/imports/modules/modules.json`: origens, hashes, imports/exports e evidências ISS.
- `analysis/imports/modules/bindings.csv`: candidatos por símbolo e módulo.
- `analysis/imports/modules/README.md`: resumo e dependências externas.
- `out/modules/lbp3/*.elf`: arquivos preparados, fora do versionamento.

Validação: build clang-cl x64, fixtures CTest incluindo dois PRX simultâneos
e recusa de entrada inválida no executável principal; testes Python de conversão,
identidades/versões, ambiguidades, imports e comparação ISS. O teste real com
os oito arquivos terminou com `Module set mapping successful: 8 images resident simultaneously`.
