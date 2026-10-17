#include "lbp3_host/elf_reader.hpp"
#include "lbp3_host/elf_mapping.hpp"
#include "lbp3_host/abi.hpp"
#include "lbp3_host/hle.hpp"
#include "lbp3_host/tls.hpp"
#include "lbp3_host/guest_thread.hpp"
#include "lbp3_host/pthread.hpp"
#include "lbp3_host/libc_probe.hpp"
#include "lbp3_host/game_metadata.hpp"
#include <string_view>

#include <exception>
#include <filesystem>
#include <iostream>

int wmain(int argc, wchar_t* argv[]) {
    const bool abi_test = argc == 2 && std::wstring_view(argv[1]) == L"--abi-self-test";
    const bool hle_test = argc == 2 && std::wstring_view(argv[1]) == L"--hle-self-test";
    const bool tls_test = argc == 2 && std::wstring_view(argv[1]) == L"--tls-self-test";
    const bool thread_test = argc == 2 && std::wstring_view(argv[1]) == L"--thread-self-test";
    const bool pthread_test = argc == 2 && std::wstring_view(argv[1]) == L"--pthread-self-test";
    const bool map = argc == 3 && std::wstring_view(argv[1]) == L"--map";
    const bool modules = argc == 3 && std::wstring_view(argv[1]) == L"--map-set";
    const bool link = (argc == 3 || argc == 4) && std::wstring_view(argv[1]) == L"--link-set";
    const bool execution = argc == 3 && std::wstring_view(argv[1]) == L"--elf-exec-self-test";
    const bool libc_test=argc==2 && std::wstring_view(argv[1])==L"--libc-services-self-test";
    const bool fatal_test=argc==3 && std::wstring_view(argv[1])==L"--fatal-service-fixture";
    const bool filesystem_test=argc==3 && std::wstring_view(argv[1])==L"--filesystem-self-test";
    const bool gpu_entry=argc==5 && std::wstring_view(argv[1])==L"--probe-pinned-entry-gpu";
    const bool entry=gpu_entry || (argc==5 && std::wstring_view(argv[1])==L"--probe-pinned-entry");
    const bool constructors=entry||(argc==3 && std::wstring_view(argv[1])==L"--probe-pinned-constructors");
    const bool dependencies=entry||(argc==3 && (std::wstring_view(argv[1])==L"--probe-pinned-modules"||constructors));
    const bool probe=entry||(argc==3 && (std::wstring_view(argv[1])==L"--probe-pinned-libc"||dependencies));
    const bool probe_test=argc==3 && std::wstring_view(argv[1])==L"--probe-fixture";
    const bool game_metadata = argc == 4 && std::wstring_view(argv[1]) == L"--game-metadata";
    const bool iss_link = (argc == 5 || argc == 6) && std::wstring_view(argv[1]) == L"--iss-link-set";
    if (argc != 2 && !map && !modules && !link && !execution && !fatal_test && !filesystem_test && !probe && !probe_test && !game_metadata && !iss_link) {
        std::cerr << "Usage: lbp3_host.exe [--map] <image.elf|eboot.bin>\n"
                     "       lbp3_host.exe --map-set|--link-set <prepared-module-directory> [pending.csv]\n"
                     "       lbp3_host.exe --game-metadata <app-root> <prepared-module-directory>\n"
                     "       lbp3_host.exe --iss-link-set <prepared-module-directory> <app-root> <user-data-directory>\n"
                     "       lbp3_host.exe --abi-self-test|--hle-self-test|--tls-self-test|--thread-self-test|--pthread-self-test\n"
                     "       lbp3_host.exe --elf-exec-self-test <artifact-directory>\n"
                     "       lbp3_host.exe --libc-services-self-test|--heap-api-self-test|--memory-self-test\n"
                     "       lbp3_host.exe --thread-services-self-test|--condition-self-test|--posix-memory-self-test\n"
                     "       lbp3_host.exe --libc-environment-self-test\n";
        return 2;
    }
    try {
        if(probe_test){lbp3::diagnostics::probe_fixture(argv[2]);return 1;}
        if (game_metadata || iss_link) {
            const std::filesystem::path app_root(argv[game_metadata ? 2 : 3]);
            const std::filesystem::path module_root(argv[game_metadata ? 3 : 2]);
            const auto metadata = lbp3::loader::read_game_metadata(
                app_root / L"sce_sys" / L"param.sfo", module_root / L"eboot.elf");
            std::cout << "Game metadata verified: title=" << metadata.title
                      << " title_id=" << metadata.title_id << " sdk=0x" << std::hex
                      << metadata.sdk_version << " attributes=0x" << metadata.psf_attributes
                      << std::dec << '\n';
            if (game_metadata) return 0;

            lbp3::hle::initialize();
            const std::filesystem::path user_root(argv[4]);
            const auto save_root = user_root / L"savedata";
            const auto temp_root = user_root / L"temp";
            std::filesystem::create_directories(save_root);
            std::filesystem::create_directories(temp_root);
            const auto user_utf8 = user_root.u8string();
            const std::string user_directory(user_utf8.begin(), user_utf8.end());
            lbp3::loader::MappedModuleSession session(module_root, std::cout);
            session.mount_guest_filesystem(app_root, save_root, temp_root);
            session.initialize_iss_gpu_backend(metadata.title.c_str(), metadata.title_id.c_str(),
                user_directory.c_str(), metadata.sdk_version, metadata.psf_attributes, 1280, 720);
            session.link(std::cout, lbp3::hle::resolve,
                         argc == 6 ? std::filesystem::path(argv[5]) : std::filesystem::path{});
            std::cout << "ISS backend and module imports prepared. Constructors and eboot entry were not called.\n";
            std::cout.flush();
            // tlggpu starts process-global detached threads and intentionally
            // has no shutdown API. This one-shot diagnostic must end the
            // process without running incompatible backend static teardown.
            TerminateProcess(GetCurrentProcess(), 0);
            return 1;
        }
        if(probe){lbp3::diagnostics::libc_probe(argv[2],dependencies,constructors,entry,entry?argv[3]:L"",entry?argv[4]:L"",gpu_entry);return 0;}
        if(filesystem_test){lbp3::hle::filesystem_self_test(argv[2],std::cout);return 0;}
        if(argc==2 && std::wstring_view(argv[1])==L"--module-registry-self-test") {
            lbp3::hle::module_registry_self_test(std::cout);return 0;
        }
        if(argc==2 && std::wstring_view(argv[1])==L"--signal-mask-self-test") {
            lbp3::hle::signal_mask_self_test(std::cout);return 0;
        }
        if(fatal_test){lbp3::hle::fatal_service_fixture(argv[2]);return 1;}
        if(argc==2 && std::wstring_view(argv[1])==L"--condition-self-test") {
            lbp3::hle::condition_self_test(std::cout);return 0;
        }
        if(argc==2 && std::wstring_view(argv[1])==L"--posix-memory-self-test") {
            lbp3::hle::posix_memory_self_test(std::cout);return 0;
        }
        if(argc==2 && std::wstring_view(argv[1])==L"--thread-services-self-test") {
            lbp3::hle::thread_services_self_test(std::cout);return 0;
        }
        if(argc==2 && std::wstring_view(argv[1])==L"--libc-environment-self-test") {
            lbp3::hle::libc_environment_self_test(std::cout);return 0;
        }
        if(argc==2 && std::wstring_view(argv[1])==L"--memory-self-test") {
            lbp3::hle::memory_self_test(std::cout);return 0;
        }
        if(argc==2 && std::wstring_view(argv[1])==L"--heap-api-self-test") {
            lbp3::hle::heap_api_self_test(std::cout);return 0;
        }
        if(argc==2 && std::wstring_view(argv[1])==L"--stack-failure-fixture") {
            lbp3::hle::stack_failure_fixture();return 1;
        }
        if(libc_test){lbp3::hle::libc_services_self_test(std::cout);return 0;}
        if (execution) {
            lbp3::hle::initialize();
            lbp3::loader::execution_self_test(std::filesystem::path(argv[2]),std::cout,lbp3::hle::resolve);
            return 0;
        }
        if (pthread_test) {
            lbp3::hle::pthread_self_test(std::cout);
            return 0;
        }
        if (thread_test) {
            lbp3::abi::thread_self_test(std::cout);
            return 0;
        }
        if (tls_test) {
            lbp3::abi::tls_self_test(std::cout);
            return 0;
        }
        if (hle_test) {
            lbp3::hle::self_test(std::cout);
            return 0;
        }
        if (abi_test) {
            lbp3::abi::self_test(std::cout);
            return 0;
        }
        if (link) {
            lbp3::hle::initialize();
            lbp3::loader::check_link_set(std::filesystem::path(argv[2]), std::cout,
                argc == 4 ? std::filesystem::path(argv[3]) : std::filesystem::path{}, lbp3::hle::resolve);
            return 0;
        }
        if (modules) {
            lbp3::loader::check_module_set(std::filesystem::path(argv[2]), std::cout);
            return 0;
        }
        if (map) {
            lbp3::loader::check_mapping(std::filesystem::path(argv[2]), std::cout);
            return 0;
        }
        const auto image = lbp3::loader::inspect_elf(std::filesystem::path(argv[1]));
        lbp3::loader::print_elf(image, std::cout);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ELF inspection failed: " << error.what() << '\n';
        return 1;
    }
}
