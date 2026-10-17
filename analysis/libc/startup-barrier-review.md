# Review of terminal libc startup barriers

These barriers are reachable in the conservative graph. Both sides of every
conditional are followed, so reachability does not prove the failure condition
will occur. None of the barriers is silently removed from the startup gate.

| VA | Root | Evidence in the path | Current interpretation |
|---|---|---|---|
| `0x58d2` | legacy constructor `0x5d040` | A lock operation returns nonzero after allocator/arena metadata checks. | Fatal lock/invariant path; runtime lock behavior and object state must be validated. |
| `0x99ff` | legacy constructor `0x5d040` | Canary comparison after stack-protected work. | Stack-protector failure trap. |
| `0x42fe` | legacy constructor `0x5d040` | Internal freelist/chunk metadata comparison fails. | Allocator consistency trap. |
| `0x8307`, `0x8309` | legacy constructor `0x5d040` | Two lock-operation return checks branch to `ud2`. | Fatal lock/runtime error paths. |
| `0x1cb5` | preinit `0xa100` | `testb $0x1f, %dil` branches to `ud2` for a non-32-byte-aligned input. | Alignment contract failure in a memory routine. |
| `0x42cb` | legacy constructor `0x5d040` | A small-size allocator path reaches the same internal metadata check as `0x42fe`. | Allocator consistency trap. |
| `0x74fd` | legacy constructor `0x5d040` | A malformed/unsupported encoded-data path branches to `ud2`. | Fatal parser/decoder error path. |

These are explicit guest traps, not host imports. The key unresolved contracts
are successful guest mutex operations during libc initialization, valid heap
and arena metadata, stack-canary setup, and valid static data. Since the graph
does not prove those predicates from initialized runtime state, these traps
remain blockers. Do not execute libc initialization until that state is
validated or the traps are handled by a controlled guest-fault boundary.

## Indirect callback example

One frequent `call *0x18(%rdi)` site is `0x54b71`. Its incoming `%rdi` is
loaded from the RELATIVE-relocated global slot `0xb72d8`, whose addend points
to zero-filled BSS object `0xc02a0`. The object field at `0xc02b8` (offset
`0x18`) is written by initializer code at `0x54757` with callback VA
`0x547f0`. This gives a possible callback target, but not a safe static
resolution: the initializer at `0x54706` is not one of the 21 audited legacy
constructor roots, and the current analysis does not establish when or whether
that object initializer runs relative to the call at `0x54b71`. Its target and
lifetime must be resolved through constructor/callback ordering before the
graph may follow it as a guaranteed edge.
