#pragma once
#include "lbp3_host/host_resolver.hpp"

namespace lbp3::hle {
std::optional<std::uintptr_t> resolve_audio_out(const HostImport&);
void audio_out_self_test();
}
