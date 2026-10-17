# Live pad input and startup progression

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
- Delayed Cross input advanced the rendered startup flow through the “Health Warning” and software-terms screens to the brightness calibration screen. A later capture showed the calibration instructions and controller image.

## Conclusion and limit

The host keyboard fallback and pad ABI deliver a pressed Cross state to the guest, and the game advanced to brightness calibration. The attempt to adjust the slider and confirm “Done” did not yield a later frame, so the title menu and gameplay remain unverified. The generated logs, frame dumps, and game files remain local under ignored `out/` paths and are not part of this repository.
