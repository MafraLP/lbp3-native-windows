# GNM initialization packet data

`gnm_initial_state.hpp` contains the base PS4 `InitSequence` and
`ClearStateSequence` packet constants from the shadPS4 sources vendored in
[iss_pc at a065f561aef8544df2d0cf711dea93881afd04c3](https://github.com/HayatoG/iss_pc/tree/a065f561aef8544df2d0cf711dea93881afd04c3/port/gpu/shadps4/core/libraries/gnmdriver).
Original files: `gnmdriver_init.h` and `gnmdriver.cpp`. Copyright shadPS4
Emulator Project, GPL-2.0-or-later; license text is preserved alongside this file.

Only packet data is reused here. This does not integrate shadPS4's GPU executor,
shader translator, Vulkan renderer or the rest of the ISS runtime.
