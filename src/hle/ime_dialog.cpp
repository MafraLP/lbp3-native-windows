#include "lbp3_host/ime_dialog.hpp"
#include "lbp3_host/abi.hpp"
#include <iostream>
#include <stdexcept>

namespace lbp3::hle {
namespace {
std::int32_t LBP3_GUEST_ABI get_status() noexcept { return 0; }
}
std::optional<std::uintptr_t> resolve_ime_dialog(const HostImport& r) {
    if (r.type != 2 || r.library != "libSceImeDialog" || r.module != "libSceImeDialog" ||
        r.library_version != 1 || r.module_version != 0x0101 || r.nid != "IADmD4tScBY") return std::nullopt;
    return reinterpret_cast<std::uintptr_t>(&get_status);
}
void ime_dialog_self_test(std::ostream& out) {
    using Status = std::int32_t (LBP3_GUEST_ABI *)() noexcept;
    const auto fn = resolve_ime_dialog({"IADmD4tScBY", "libSceImeDialog", "libSceImeDialog", 1, 0x0101, 2});
    if (!fn || reinterpret_cast<Status>(*fn)() != 0 ||
        resolve_ime_dialog({"IADmD4tScBY", "libSceUserService", "libSceImeDialog", 1, 0x0101, 2}) ||
        resolve_ime_dialog({"IADmD4tScBY", "libSceImeDialog", "libSceImeDialog", 1, 0x0100, 2}))
        throw std::runtime_error("IME dialog offline-state resolver fixture failed.");
    out << "IME dialog tests passed: exact status import reports inactive dialog and skips virtual keyboard flow.\n";
}
}
