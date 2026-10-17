#include "lbp3_host/companion_httpd.hpp"
#include "lbp3_host/hle.hpp"
#include "lbp3_host/abi.hpp"
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {
constexpr std::int32_t invalid_param = static_cast<std::int32_t>(0x80e40004u);
constexpr std::int32_t not_initialized = static_cast<std::int32_t>(0x80e40006u);
constexpr std::int32_t already_initialized = static_cast<std::int32_t>(0x80e40007u);
constexpr std::int32_t no_event = static_cast<std::int32_t>(0x80e40008u);
constexpr std::int32_t already_started = static_cast<std::int32_t>(0x80e4000au);
#define CHECK(ok) do { if (!(ok)) throw std::runtime_error("CompanionHttpd fixture assertion failed at line " + std::to_string(__LINE__)); } while (false)
auto resolve(const char* nid) {
    return lbp3::hle::resolve({nid, "libSceCompanionHttpd", "libSceCompanionHttpd", 1, 0x0101, 2});
}
std::int32_t LBP3_GUEST_ABI callback() { return 0; }
}

int main() {
    try {
        lbp3::hle::initialize();
        using Options = std::int32_t (LBP3_GUEST_ABI *)(void*);
        using Initialize = std::int32_t (LBP3_GUEST_ABI *)(const void*);
        using Register = std::int32_t (LBP3_GUEST_ABI *)(void*, void*);
        using Simple = std::int32_t (LBP3_GUEST_ABI *)();
        using AddHeader = std::int32_t (LBP3_GUEST_ABI *)(const char*, const char*);
        using SetStatus = std::int32_t (LBP3_GUEST_ABI *)(std::int32_t, void*);
        auto options_init = reinterpret_cast<Options>(resolve("r-2-a0c7Kfc").value());
        auto initialize = reinterpret_cast<Initialize>(resolve("ykNpWs3ktLY").value());
        auto register_callback = reinterpret_cast<Register>(resolve("OaWw+IVEdbI").value());
        auto start = reinterpret_cast<Simple>(resolve("k7F0FcDM-Xc").value());
        auto stop = reinterpret_cast<Simple>(resolve("0SCgzfVQHpo").value());
        auto add_header = reinterpret_cast<AddHeader>(resolve("8pWltDG7h6A").value());
        auto set_status = reinterpret_cast<SetStatus>(resolve("w7oz0AWHpT4").value());

        std::array<std::uint8_t, 0x68> options{};
        options.fill(0xA5);
        CHECK(options_init(options.data()) == 0);
        for (std::size_t i = 0; i < 0x60; ++i) CHECK(options[i] == 0);
        for (std::size_t i = 0x60; i < options.size(); ++i) CHECK(options[i] == 0xA5);
        CHECK(options_init(nullptr) == invalid_param);
        CHECK(start() == not_initialized);
        CHECK(register_callback(reinterpret_cast<void*>(&callback), options.data()) == not_initialized);
        CHECK(initialize(options.data()) == 0);
        CHECK(initialize(options.data()) == already_initialized);
        CHECK(register_callback(nullptr, options.data()) == invalid_param);
        CHECK(register_callback(reinterpret_cast<void*>(&callback), options.data()) == 0);
        CHECK(start() == 0 && start() == already_started);
        CHECK(add_header("Location", "https://example.invalid/") == no_event);
        CHECK(add_header(nullptr, "x") == invalid_param);
        CHECK(set_status(302, options.data()) == no_event);
        CHECK(set_status(99, options.data()) == invalid_param);
        CHECK(stop() == 0 && stop() == not_initialized);
        CHECK(!lbp3::hle::resolve_companion_httpd({"k7F0FcDM-Xc", "libkernel", "libSceCompanionHttpd", 1, 0x0101, 2}));
        CHECK(!lbp3::hle::resolve_companion_httpd({"k7F0FcDM-Xc", "libSceCompanionHttpd", "libSceCompanionHttpd", 2, 0x0101, 2}));
        CHECK(!lbp3::hle::resolve_companion_httpd({"k7F0FcDM-Xc", "libSceCompanionHttpd", "libSceCompanionHttpd", 1, 0x0100, 2}));
        CHECK(!lbp3::hle::resolve_companion_httpd({"unknown", "libSceCompanionHttpd", "libSceCompanionHttpd", 1, 0x0101, 2}));
        std::cout << "CompanionHttpd offline startup lifecycle and strict import identities passed; no network listener is exposed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
