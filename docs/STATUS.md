# Status do projeto / Project status

Última atualização: 2026-10-17. Este é um snapshot de desenvolvimento, não uma declaração de compatibilidade de release.

## O que foi observado

- O host Windows x64 compila com `clang-cl` e CMake; os testes automatizados cobrem loader ELF, ABI guest/host, memória, TLS, threads e serviços HLE em fixtures.
- Em uma configuração local com o backend GPU Vulkan experimental do ISS, o entry do eboot apresentou “Health Warning”, termos de uso, calibração de brilho, informação de salvamento e “Connecting to LittleBigPlanet 3 Online…”.
- `scePadReadState` recebeu Espaço como Cross (`buttons=0x4000`) com o controle virtual de teclado conectado em repouso. Confirmações enviadas após cada tela avançaram o fluxo até a conexão online; menu principal, fallback offline e gameplay ainda não foram confirmados.
- A execução limitada mais recente passou pelos imports `sceNpGetState`, POSIX `socket` e `sceNetEpollCreate`; o restante da sequência de rede/offline permanece em investigação.
- A verificação mais recente deste workspace passou 49/49 testes CTest. São testes de componentes e fixtures, não um teste de compatibilidade do jogo completo.

## O que ainda não foi demonstrado

- Sair da tela “Connecting to LittleBigPlanet 3 Online…”, chegar ao menu principal ou jogar uma fase.
- Inicialização completa e estável em builds de usuário, múltiplas GPUs ou outra instalação do jogo.
- Cobertura total dos imports Orbis, execução de todas as rotinas indiretas, áudio, saves ou rede.
- Build gráfico autocontido a partir deste repositório. O backend Vulkan usado na captura é opcional e vem de uma árvore externa do ISS, com dependências próprias.

O projeto não oferece downloads de jogo/firmware, instalador para jogadores nem instruções de aquisição de cópias. Registre qualquer nova afirmação de compatibilidade com configuração, commit, evidência reproduzível e limitações.

For details in English, see the same verified scope above; this document intentionally records milestones conservatively.

## Latest first-use diagnostic (2026-10-17)

A run with a separate, initially empty save directory stopped at `Important Information`. Its gated trace observed `sceNpGetNpId`, `sceNpSetNpTitleId`, and `sceNpRegisterStateCallback`, followed by repeated `sceNpCheckCallback` polling and another NP ID query. It did not call `sceNpGetState` or the network APIs during the captured interval. A separate 30-second filesystem trace found `/app0/gamedata/data/craftworld.ini` missing, then successfully opened `ratings.xml` and streamed `intro_001.farc`; the title continued rendering, so the missing INI is not yet proven fatal. The earlier run using persisted save data reached the online-connection screen; these are distinct startup states, so no specific network shim is identified as the cause of the first-use screen yet.
