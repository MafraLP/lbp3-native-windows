# Arquivos do jogo e dados locais

Este port não contém nem distribui conteúdo de *LittleBigPlanet 3*. Ele também não inclui downloader, descriptografador, firmware, chaves, executável do jogo, bibliotecas proprietárias ou conversor de pacotes.

## O que o host pode ler

Os diagnósticos atuais trabalham com módulos ELF do jogo e uma raiz de arquivos que representa `/app0`. Os formatos e o conjunto de arquivos necessários dependem do build/região do jogo e ainda não são uma especificação de release. Não trate os nomes ou caminhos de um diagnóstico local como uma lista de aquisição ou garantia de compatibilidade.

Use apenas arquivos provenientes de uma cópia que você esteja legalmente autorizado a usar, respeitando as leis e os termos aplicáveis na sua região. Este projeto não orienta como contornar DRM, obter chaves ou baixar jogos. Consulte fontes oficiais para dúvidas sobre seus direitos e a plataforma.

## Proteção de dados

- Nunca faça commit de `eboot.bin`, ELF extraído, SELF, PRX, pacote, firmware, chaves, dumps de memória, saves ou conteúdo extraído.
- Não anexe esses arquivos a issues, pull requests, releases, chats ou serviços de compartilhamento.
- Antes de enviar logs, confira se não contêm caminhos pessoais, nomes de usuário, tokens ou conteúdo do jogo. Remova dados privados e compartilhe somente trechos mínimos de texto técnico.
- Os diretórios de build e diagnóstico são locais. O `.gitignore` exclui `out/` e extensões comuns de binários, mas não substitui a revisão manual do que será publicado.
- Use saves de teste que possa restaurar; não aponte ferramentas experimentais a dados importantes.

## Se você não tem os arquivos

É possível compilar o host e executar as fixtures sintéticas sem uma cópia do jogo. Não baixe dumps de terceiros e não peça a outros usuários que os enviem. A documentação e o código devem continuar úteis sem distribuir material protegido.
