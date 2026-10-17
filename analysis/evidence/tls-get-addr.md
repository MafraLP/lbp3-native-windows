# Acesso ao TLS da thread atual

Implementado `__tls_get_addr` por função host SysV, registrada sob NID
`vNe1w4diLCs`, libkernel/biblioteca versão 1, módulo libkernel versão 0x0101,
tipo STT_FUNC. A assinatura usa ponteiro para dois uint64: module_id e offset,
conforme contrato observado na referência ISS e nas estruturas DTPMOD64 locais.

`ScopedThreadTls` anexa blocos a um ponteiro C++ thread_local. Ao sair do
escopo, restaura o contexto anterior, permitindo escopos aninhados. O storage
precisa durar mais que o escopo, que é criado/destruído na mesma thread.
O loader anexa seus templates já relocados à thread de diagnóstico e verifica
o acesso de cada um dos quatro módulos. A destruição destaca o contexto antes
de liberar os blocos. Não há alteração de FS/GS ou TEB por código manual.

O descritor de 16 bytes é copiado com ReadProcessMemory antes de consultar os
blocos, evitando dereferenciar um descritor inacessível. Módulos e offsets são
validados pelo storage, exigindo pelo menos um byte válido. A entrada não deixa
exceções C++ atravessarem a ABI. No modo de diagnóstico atual, retorna nullptr
para thread sem contexto, ponteiro nulo/inacessível, ID desconhecido ou offset
inválido. Não é uma política final de recuperação para executar o jogo.

Validação:

- Duas threads chamam o getter por ponteiros tipados SysV, modificam seus blocos
  simultaneamente e mantêm valores distintos, sem alterar os templates.
- Offset zero e último byte válido, offsets além do fim/overflow, ID ausente,
  descritor nulo/inacessível e thread sem contexto.
- Escopo aninhado restaura o storage anterior; sair do último escopo remove o acesso.
- `--hle-self-test` chama o endereço fornecido pelo resolver e confere o byte inicial.
- Fixture do linker conecta o NID real em destinos sintéticos sem executar seus módulos.
- Seis conjuntos CTest passam.

Resultado com LBP3: quatro novas referências conectadas, em eboot, Fios2, libc
e SPU. `host_bindings=8`, `pending=579` (573 símbolos e seis RELATIVE especiais),
`tls_relocations=27`. A consulta dos quatro módulos foi verificada na thread
de diagnóstico; nenhuma instrução do LBP3 foi chamada.

Limites: ainda não existem threads guest gerenciadas, TCB/DTV Orbis completo,
reescrita de acessos diretos a FS/GS, TLS dinâmico ou finalizadores TLS.
O descritor é lido do processo atual; ainda não há política de isolamento de
ponteiros/arena guest para uma execução geral. As funções e o contexto vivem
apenas durante o teste de ligação.
