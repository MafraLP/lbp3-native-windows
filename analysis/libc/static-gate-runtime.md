# Static libc initialization gate

ELF SHA-256: `9811633cd5d49ae461ec0550468db5ef037d576abe1d732348844b930f397566`.

Result: **BLOCKED**.

- 75 control-flow barriers remain unresolved

## Unbound imports in the reviewed graph

| Import | NID | Call site |
|---|---|---|

The graph explores both conditional branches. These are potential paths, not observed calls.
Indirect callbacks/jumps still prevent a complete proof. No real libc/game instructions were executed.
Binding an export or modeling a signal mask does not validate signal delivery or exception unwinding.

A passed static gate alone must never enable execution automatically.
