# Status do projeto / Project status

Última atualização: 2026-10-17. Este é um snapshot de desenvolvimento, não uma declaração de compatibilidade de release.

## O que foi observado

- O host Windows x64 compila com `clang-cl` e CMake; os testes automatizados cobrem loader ELF, ABI guest/host, memória, TLS, threads e serviços HLE em fixtures.
- Em uma configuração local com o backend GPU Vulkan experimental do ISS, o entry do eboot apresentou as telas “Health Warning”, termos de uso e calibração de brilho.
- `scePadReadState` recebeu Espaço como Cross (`buttons=0x4000`) com o controle virtual de teclado conectado em repouso. Uma confirmação enviada após a tela estar pronta avançou o fluxo até a calibração; menu principal ou gameplay ainda não foram confirmados.
- A verificação mais recente deste workspace passou 49/49 testes CTest. São testes de componentes e fixtures, não um teste de compatibilidade do jogo completo.

## O que ainda não foi demonstrado

- Chegar ao menu principal ou jogar uma fase; a calibração de brilho ainda não foi concluída neste port.
- Inicialização completa e estável em builds de usuário, múltiplas GPUs ou outra instalação do jogo.
- Cobertura total dos imports Orbis, execução de todas as rotinas indiretas, áudio, saves ou rede.
- Build gráfico autocontido a partir deste repositório. O backend Vulkan usado na captura é opcional e vem de uma árvore externa do ISS, com dependências próprias.

O projeto não oferece downloads de jogo/firmware, instalador para jogadores nem instruções de aquisição de cópias. Registre qualquer nova afirmação de compatibilidade com configuração, commit, evidência reproduzível e limitações.

For details in English, see the same verified scope above; this document intentionally records milestones conservatively.
