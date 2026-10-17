# Live pad input check

Date: 2026-10-17

## Setup

- Windows x64 debug build with the optional ISS Vulkan backend enabled.
- The pinned module set and a locally supplied game application root.
- `LBP3_PAD_TRACE=1` while the isolated guest-entry probe ran.
- A neutral keyboard state was observed first; then a Space key event was sent to the host desktop.

## Observed

- Before input, `scePadReadState` returned `buttons=0`, `connected=1`, and neutral axes (`128,128,128,128`).
- While Space was held, it returned `buttons=0x4000` (Cross), `connected=1`, and neutral axes.
- After release, it returned to `buttons=0` while remaining connected.
- The GPU backend continued submitting frames successfully (`submit_result=0`).

## Conclusion and limit

The host keyboard fallback and pad ABI deliver a pressed Cross state to the guest. This does not establish that the game accepted the button or advanced beyond the Health Warning screen; the bounded diagnostic session did not capture a confirmed screen transition. The generated logs and game files remain local under ignored `out/` paths and are not part of this repository.
