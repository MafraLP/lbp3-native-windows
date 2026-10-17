# Licenças, créditos e publicação

Este documento registra os fatos de proveniência conhecidos e não é aconselhamento jurídico. Antes de publicar código ou binários, confirme que você tem direito de redistribuí-los e que os avisos e fontes correspondentes acompanham cada componente.

## Conteúdo protegido do jogo

Não incluir no repositório, releases, CI ou documentação: executáveis e bibliotecas do jogo, firmware, chaves, pacotes, assets, áudio, vídeos, saves, dumps ou arquivos extraídos. O port deve exigir que cada usuário forneça localmente os arquivos que tem direito de usar. Não hospede links para cópias não autorizadas nem instruções de contorno de DRM.

## Componentes de terceiros identificados

- O port em [HayatoG/iss_pc](https://github.com/HayatoG/iss_pc) declara em [`port/UPSTREAM.md`](https://github.com/HayatoG/iss_pc/blob/main/port/UPSTREAM.md) que o código de `port/` é GNU GPL v2 ou posterior, com o texto da licença em [`port/LICENSE`](https://github.com/HayatoG/iss_pc/blob/main/port/LICENSE). Isso concede permissão para copiar, modificar e redistribuir esse código cumprindo a GPL; crédito isolado não substitui a licença. Ao distribuir modificações, preserve avisos/licença, marque arquivos modificados e cumpra os termos de distribuição e disponibilização do código-fonte aplicáveis.
- O `VENDOR.txt` do backend GPU lista trechos vindos de shadPS4, Shadow of the Colossus PC port, bloodborne_pc e outros projetos. A lista e o submódulo registram licenças diversas; mantenha cada aviso e não apague a proveniência. Veja [`port/gpu/VENDOR.txt`](https://github.com/HayatoG/iss_pc/blob/main/port/gpu/VENDOR.txt) e [`port/.gitmodules`](https://github.com/HayatoG/iss_pc/blob/main/.gitmodules).
- `third_party/shadps4/gnm_initial_state.hpp` declara `GPL-2.0-or-later`; a pasta local preserva a licença e a nota de origem.
- A árvore local `out/iss-audit` não é parte deste checkout e não deve ser enviada automaticamente. Se ela for copiada/vendida junto ao projeto, inclua o código-fonte correspondente, licenças e avisos das dependências conforme seus termos.
- Identifique a procedência e licença de todos os demais arquivos, scripts, patches, dados de referência, fontes, fixtures e capturas antes de os publicar.

O README do upstream `iss_pc` serve como referência estrutural e factual; não copie texto ou imagens sem verificar direitos e atribuição. O fato de um repositório ser público não significa que todos os seus arquivos estejam sob uma licença permissiva.

## Situação da licença deste repositório

Este checkout ainda não tem um `LICENSE` na raiz. A licença upstream do ISS resolve a permissão de reutilização para o código de `port/`, mas não escolhe automaticamente uma licença para as contribuições originais deste projeto nem substitui os avisos das dependências.

Para publicar o port integrado como software livre, a escolha natural é uma licença GPL compatível com o ISS e com os componentes derivados incluídos. Antes de marcar a caixa de licença no GitHub, finalize o inventário dos arquivos efetivamente copiados/adaptados e escolha a variante GPL que todos os contribuidores podem conceder. O ISS declara “GPL v2 or later”; alguns trechos listados no `VENDOR.txt` têm indicação GPL v2. Não aplique uma licença permissiva como MIT/Apache ao conjunto integrado. Até o `LICENSE` raiz ser adicionado, o GitHub não apresentará licença global para o projeto.

Não é necessário manter o repositório privado apenas por usar o código ISS: a licença publicada permite reutilização sob seus termos. Um repositório público sem `LICENSE` raiz, porém, não comunica uma licença geral para as partes originais do LBP3 port.

## Marcas e independência

Use “port/reimplementação não oficial, em desenvolvimento” e o aviso de não afiliação. *LittleBigPlanet 3*, PlayStation e nomes relacionados pertencem aos respectivos titulares. Não use logotipos oficiais, não apresente o projeto como produto da Sony/Media Molecule e não sugira aprovação ou suporte oficial.

## Checklist antes de tornar público

- Revise `git status --short` e a lista exata de arquivos staged.
- Confirme que `out/`, artefatos de build, dados de jogo, firmware, chaves, saves, dumps, caminhos pessoais e tokens não estão staged.
- Verifique origem/licença de cada arquivo e mantenha notices; para o código do ISS, cumpra a GPL indicada em `port/UPSTREAM.md` e as licenças de cada dependência.
- Não publique um instalador que contenha conteúdo de jogo.
- Descreva corretamente que o jogo não está confirmado como jogável e que o backend GPU é opcional/externo.
