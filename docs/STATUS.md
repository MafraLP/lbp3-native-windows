# Status do projeto / Project status

Última atualização: 2026-10-17. Este é um snapshot de desenvolvimento, não uma declaração de compatibilidade de release.

## O que foi observado

- O host Windows x64 compila com `clang-cl` e CMake; os testes automatizados cobrem loader ELF, ABI guest/host, memória, TLS, threads e serviços HLE em fixtures.
- Em uma configuração local com o backend GPU Vulkan experimental do ISS, o entry do eboot chegou à apresentação gráfica e uma captura mostrou o texto “Health Warning”. Isso é evidência de que um caminho de renderização executou; não prova que a entrada de controle avance a tela.
- `scePadReadState` foi observado em chamadas repetidas durante essa tela. Ainda não confirmamos entrada pressionada chegando ao guest nem a transição para o menu.
- A verificação mais recente deste workspace passou 49/49 testes CTest. São testes de componentes e fixtures, não um teste de compatibilidade do jogo completo.

## O que ainda não foi demonstrado

- Avançar o aviso, chegar ao menu ou jogar uma fase.
- Inicialização completa e estável em builds de usuário, múltiplas GPUs ou outra instalação do jogo.
- Cobertura total dos imports Orbis, execução de todas as rotinas indiretas, áudio, saves ou rede.
- Build gráfico autocontido a partir deste repositório. O backend Vulkan usado na captura é opcional e vem de uma árvore externa do ISS, com dependências próprias.

O projeto não oferece downloads de jogo/firmware, instalador para jogadores nem instruções de aquisição de cópias. Registre qualquer nova afirmação de compatibilidade com configuração, commit, evidência reproduzível e limitações.

For details in English, see the same verified scope above; this document intentionally records milestones conservatively.
