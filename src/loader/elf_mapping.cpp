#include "lbp3_host/elf_mapping.hpp"
#include "lbp3_host/elf_reader.hpp"
#include "lbp3_host/elf_dynamic.hpp"
#include "lbp3_host/tls.hpp"
#include "lbp3_host/pthread.hpp"
#include "lbp3_host/guest_execution.hpp"
#include "lbp3_host/guest_fs.hpp"
#include "lbp3_host/synthetic_elf.hpp"
#include "lbp3_host/process_param.hpp"
#include "lbp3_host/heap_api.hpp"
#include "lbp3_host/flexible_memory.hpp"
#include "lbp3_host/direct_memory.hpp"
#include "lbp3_host/video_out.hpp"
#include "lbp3_host/compute_queue.hpp"
#include "lbp3_host/low_memory.hpp"
#include "lbp3_host/gpu_memory.hpp"
#include "lbp3_host/event_queue.hpp"
#include "lbp3_host/thread_services.hpp"
#include "lbp3_host/module_registry.hpp"
#include "lbp3_host/win32_file.hpp"
#include "lbp3_host/iss_backend.hpp"
#include "lbp3_host/guest_filesystem.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <cstring>
#include <set>
#include <fstream>
#include <string_view>
#include <ostream>
#include <stdexcept>
#include <system_error>
#include <vector>
#include <bcrypt.h>

namespace lbp3::loader {
namespace {
struct Reservation {
    void* base{};
    std::uint64_t load_bias{};
    ElfImageInfo image;
    DynamicImage dynamic;
    std::string name;
    std::uint64_t tls_id{};
    std::unique_ptr<platform::Win32File> file;
    ~Reservation() {
        if (!base) return;
        for (const auto& ph : image.program_headers)
            if (ph.type == 1 && ph.memory_size)
                memory::gpu_notify_unmap(static_cast<std::uintptr_t>(load_bias + ph.virtual_address), ph.memory_size);
        VirtualFree(base, 0, MEM_RELEASE);
    }
    bool contains(std::uint64_t va, std::uint64_t size) const {
        for (const auto& ph : image.program_headers)
            if (ph.type == 1 && va >= ph.virtual_address && va-ph.virtual_address <= ph.memory_size &&
                size <= ph.memory_size-(va-ph.virtual_address)) return true;
        return false;
    }
    std::uint64_t host(std::uint64_t va, std::uint64_t size) const {
        if (!contains(va, size)) throw std::runtime_error("Relocation address is outside PT_LOAD.");
        if (va > std::numeric_limits<std::uint64_t>::max()-load_bias)
            throw std::runtime_error("Relocation address overflow.");
        return load_bias + va;
    }
};
} // namespace

struct MappedModuleSession::State {
    std::vector<std::unique_ptr<Reservation>> modules;
    std::shared_ptr<void> services;
    std::unique_ptr<io::ScopedFileSystem> guest_filesystem;
    bool iss_gpu_lifetime_pinned{};
    bool iss_gpu_backend_active{};
};

namespace {
[[noreturn]] void fail(const char* operation) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
}
std::uint64_t round_up(std::uint64_t n, std::uint64_t alignment) {
    if (n > std::numeric_limits<std::uint64_t>::max() - (alignment - 1))
        throw std::runtime_error("Mapping address overflow.");
    return (n + alignment - 1) & ~(alignment - 1);
}
DWORD protection(std::uint32_t flags) {
    switch (flags) {
    case 0: return PAGE_NOACCESS;
    case 4: return PAGE_READONLY;
    case 6: return PAGE_READWRITE;
    case 1: return PAGE_EXECUTE;
    case 5: return PAGE_EXECUTE_READ;
    default: throw std::runtime_error("Unsupported PT_LOAD permissions (including RWX).");
    }
}

struct ProbeHeapWorker {
    std::uintptr_t allocate{},release{};
    unsigned seed{};
    bool passed{};
};
void LBP3_GUEST_ABI probe_process_finalizer() noexcept {
    constexpr char message[]="PROBE_PROCESS_FINALIZER_REQUESTED: main startup reached the loader exit callback; full process shutdown is not enabled.\n";
    DWORD written{};WriteFile(GetStdHandle(STD_ERROR_HANDLE),message,sizeof(message)-1,&written,nullptr);
    for(;;)TerminateProcess(GetCurrentProcess(),0xe04c4304u);
}
void* LBP3_GUEST_ABI probe_heap_worker(void* argument) noexcept {
    auto& worker=*static_cast<ProbeHeapWorker*>(argument);
    using Allocate=void* (LBP3_GUEST_ABI *)(std::uint64_t);
    using Release=void (LBP3_GUEST_ABI *)(void*);
    Allocate volatile allocate=reinterpret_cast<Allocate>(worker.allocate);
    Release volatile release=reinterpret_cast<Release>(worker.release);
    for(unsigned round=0;round<128;++round){
        const std::size_t size=1+((round*7919+worker.seed)%65536);
        auto* block=static_cast<std::byte*>(allocate(size));
        if(!block||!memory::owns_writable_range(reinterpret_cast<std::uintptr_t>(block),size))return nullptr;
        const auto pattern=static_cast<std::byte>(round+worker.seed);
        std::fill_n(block,size,pattern);
        SwitchToThread();
        const bool valid=std::all_of(block,block+size,[&](std::byte value){return value==pattern;});
        release(block);
        if(!valid)return nullptr;
    }
    worker.passed=true;
    return argument;
}

abi::ModuleInfoEx module_record(const Reservation& module,std::int32_t id){
    abi::ModuleInfoEx record{};record.id=id;record.tls_index=static_cast<std::uint32_t>(module.tls_id);
    if(module.name.size()>=record.name.size())throw std::runtime_error("Module name exceeds query ABI.");
    std::copy(module.name.begin(),module.name.end(),record.name.begin());
    const auto narrow=[](std::uint64_t n){if(n>UINT32_MAX)throw std::runtime_error("Module field exceeds query ABI.");return static_cast<std::uint32_t>(n);};
    for(const auto& ph:module.image.program_headers){
        if(ph.type==1&&ph.memory_size){
            if(record.segment_count==4)throw std::runtime_error("Too many module query segments.");
            record.segments[record.segment_count++]={module.host(ph.virtual_address,ph.memory_size),narrow(ph.memory_size),static_cast<std::int32_t>(ph.flags)};
        }
        if(ph.type==7&&ph.memory_size){record.tls_initial=module.host(ph.virtual_address,std::max<std::uint64_t>(1,ph.file_size));
            record.tls_initial_size=narrow(ph.file_size);record.tls_size=narrow(ph.memory_size);record.tls_alignment=narrow(ph.alignment);}
    }
    const auto entry=[&](std::uint64_t va){
        for(const auto& ph:module.image.program_headers)if(ph.type==1&&(ph.flags&1)&&va>=ph.virtual_address&&va-ph.virtual_address<ph.file_size)return module.host(va,1);
        throw std::runtime_error("Lifecycle query entry outside executable bytes.");};
    if(module.dynamic.init)record.init=entry(*module.dynamic.init);
    if(module.dynamic.fini)record.fini=entry(*module.dynamic.fini);
    for(const auto& ph:module.image.program_headers)if(ph.type==0x6474e550&&ph.file_size){
        if(ph.file_size<8)throw std::runtime_error("Truncated EH frame header.");
        const auto header=module.host(ph.virtual_address,ph.file_size);
        std::array<std::byte,8> bytes{};module.file->read_at(ph.offset,bytes);
        if(bytes[0]!=std::byte{1}||bytes[1]!=std::byte{0x1b})throw std::runtime_error("Unsupported EH frame pointer encoding.");
        std::int32_t displacement{};std::memcpy(&displacement,bytes.data()+4,4);
        const auto frame=add_signed(header+4,displacement);
        if(frame<module.load_bias)throw std::runtime_error("EH frame pointer precedes module.");
        const auto va=frame-module.load_bias;
        const ProgramHeader* backing=nullptr;
        for(const auto& load:module.image.program_headers)if(load.type==1&&(load.flags&4)&&va>=load.virtual_address&&va-load.virtual_address<load.file_size)backing=&load;
        if(!backing)throw std::runtime_error("EH frame pointer outside readable file bytes.");
        auto end=backing->virtual_address+backing->file_size;
        if(ph.virtual_address>va)end=std::min(end,ph.virtual_address);
        auto cursor=va;unsigned entries{};
        while(cursor<end){
            if(end-cursor<4||++entries>100000)throw std::runtime_error("Invalid EH frame record range.");
            std::uint32_t length{};std::memcpy(&length,reinterpret_cast<const void*>(module.host(cursor,4)),4);
            cursor+=4;if(!length)break;
            std::uint64_t size=length;
            if(length==UINT32_MAX){if(end-cursor<8)throw std::runtime_error("Truncated extended EH frame.");
                std::memcpy(&size,reinterpret_cast<const void*>(module.host(cursor,8)),8);cursor+=8;}
            if(size<4||size>end-cursor)throw std::runtime_error("Invalid EH frame record length.");
            cursor+=size;
        }
        record.eh_header=header;record.eh_header_size=narrow(ph.file_size);
        record.eh_frame=frame;record.eh_frame_size=narrow(cursor-va);
    }
    return record;
}

// Owns the host-side service contexts whose pointers refer into guest images.
// Member declaration order is intentional: thread attachments/services unwind
// before the TLS storage and module metadata they depend on.
struct RuntimeServices {
    std::unique_ptr<abi::ThreadTls> tls;
    std::unique_ptr<abi::ScopedThreadTls> tls_attachment;
    std::optional<abi::ScopedProcessParam> process_parameters;
    std::vector<abi::ModuleInfoEx> module_records;
    std::unique_ptr<abi::ScopedModuleRegistry> module_registry;
    std::unique_ptr<memory::ScopedDirectMemory> direct_memory;
    std::unique_ptr<gpu::ScopedVideoOut> video_out;
    std::unique_ptr<gpu::ScopedComputeQueues> compute_queues;
    std::unique_ptr<memory::ScopedFlexibleMemory> flexible_memory;
    std::unique_ptr<abi::ScopedHeapApi> heap_api;
    std::unique_ptr<abi::ScopedThreadServices> thread_services;
    std::unique_ptr<abi::PthreadRuntime> pthread_runtime;
    std::unique_ptr<abi::ScopedPthreadRuntime> pthread_attachment;
    std::unique_ptr<abi::ScopedEventQueues> event_queues;
    std::unique_ptr<abi::ScopedGuestFs> guest_fs;
    std::unique_ptr<memory::ScopedGpuMemoryTracking> gpu_memory_tracking;
};

std::unique_ptr<Reservation> map_image(const std::filesystem::path& path, std::ostream& output,
                                      bool allow_shared) {
    // Hold a read-only handle throughout inspection and mapping to prevent writes.
    auto reservation = std::make_unique<Reservation>();
    reservation->file = std::make_unique<platform::Win32File>(path);
    auto& file = *reservation->file;
    const auto image = inspect_elf(path, allow_shared);
    const bool shared = image.header.type == 0xfe18;
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const std::uint64_t page = system.dwPageSize;
    const std::uint64_t granularity = system.dwAllocationGranularity;
    std::vector<ProgramHeader> loads;
    for (const auto& ph : image.program_headers) {
        if (ph.type == 0x61000010 && ph.memory_size)
            throw std::runtime_error("PT_SCE_RELRO mapping is not supported yet.");
        if (ph.type == 1 && ph.memory_size) loads.push_back(ph);
    }
    if (loads.empty()) throw std::runtime_error("No nonempty PT_LOAD segments.");
    std::sort(loads.begin(), loads.end(), [](const auto& a, const auto& b) {
        return a.virtual_address < b.virtual_address;
    });
    const auto minimum = loads.front().virtual_address & ~(granularity - 1);
    std::uint64_t end = minimum;
    std::uint64_t alignment = granularity;
    bool entry_valid = false;
    for (const auto& ph : loads) {
        (void)protection(ph.flags);
        if ((ph.virtual_address & ~(page - 1)) < end)
            throw std::runtime_error("PT_LOAD segments overlap or share host pages.");
        end = round_up(ph.virtual_address + ph.memory_size, page);
        alignment = std::max(alignment, ph.alignment);
        if ((ph.flags & 1) && image.header.entry >= ph.virtual_address &&
            image.header.entry - ph.virtual_address < ph.file_size) entry_valid = true;
    }
    if (!shared && !entry_valid) throw std::runtime_error("Entry is outside file-backed executable PT_LOAD data.");
    const auto size = end - minimum;
    // Initial diagnostic mapper has an explicit resource budget.
    constexpr std::uint64_t budget = 1024ull * 1024 * 1024;
    if (size > budget || alignment > budget || minimum % alignment != 0)
        throw std::runtime_error("Image exceeds mapping budget or has unsupported base alignment.");
    const bool fixed = image.header.type == 2 || image.header.type == 0xfe00;
    if (fixed && minimum == 0) throw std::runtime_error("Cannot map a fixed image at address zero.");
    if(fixed&&(minimum>=memory::guest_address_limit||size>memory::guest_address_limit-minimum))
        throw std::runtime_error("Fixed ELF exceeds 40-bit guest pointer range.");
    reservation->base = fixed?VirtualAlloc(reinterpret_cast<void*>(minimum),static_cast<SIZE_T>(size),MEM_RESERVE,PAGE_NOACCESS):
        memory::allocate_low(size+alignment,MEM_RESERVE,PAGE_NOACCESS);
    if (!reservation->base) fail("VirtualAlloc reserve");
    const auto raw = reinterpret_cast<std::uintptr_t>(reservation->base);
    const auto base = fixed ? raw : round_up(raw, alignment);
    if (base < minimum) throw std::runtime_error("Negative load bias is unsupported.");
    reservation->load_bias = base - minimum;
    reservation->name = path.filename().string();
    reservation->image = image;
    output << "Load bias: 0x" << std::hex << reservation->load_bias << std::dec << '\n';
    if (fixed && base != minimum) throw std::runtime_error("Fixed image address unavailable.");
    const auto address = [base, minimum](std::uint64_t va) {
        return reinterpret_cast<std::byte*>(base + (va - minimum));
    };
    for (const auto& ph : loads) {
        auto* destination = address(ph.virtual_address);
        const auto start = ph.virtual_address & ~(page - 1);
        const auto length = round_up(ph.virtual_address + ph.memory_size, page) - start;
        if (!VirtualAlloc(address(start), static_cast<SIZE_T>(length), MEM_COMMIT, PAGE_READWRITE))
            fail("VirtualAlloc commit");
        file.read_at(ph.offset, {destination, static_cast<std::size_t>(ph.file_size)});
        // Fresh committed Windows pages are zeroed; verify BSS before final protection.
        if (!std::all_of(destination + ph.file_size, destination + ph.memory_size,
                        [](std::byte b) { return b == std::byte{}; }))
            throw std::runtime_error("BSS verification failed.");
        std::array<std::byte, 65536> bytes{};
        for (std::uint64_t offset = 0; offset < ph.file_size;) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(), ph.file_size - offset));
            file.read_at(ph.offset + offset, std::span(bytes).first(count));
            if (!std::equal(bytes.begin(), bytes.begin() + count, destination + offset))
                throw std::runtime_error("Mapped bytes differ from file.");
            offset += count;
        }
        DWORD previous{};
        if (!VirtualProtect(address(start), static_cast<SIZE_T>(length), protection(ph.flags), &previous))
            fail("VirtualProtect");
        if ((ph.flags & 1) && !FlushInstructionCache(GetCurrentProcess(), destination, static_cast<SIZE_T>(ph.memory_size)))
            fail("FlushInstructionCache");
        output << "PT_LOAD guest=0x" << std::hex << ph.virtual_address
               << " host=0x" << reinterpret_cast<std::uintptr_t>(destination)
               << " flags=0x" << ph.flags << std::dec
               << " copied=" << ph.file_size << " BSS=" << ph.memory_size - ph.file_size << " bytes; verified\n";
    }
    if (!shared) {
        output << "Mapped entry: 0x" << std::hex << reinterpret_cast<std::uintptr_t>(address(image.header.entry))
               << std::dec << '\n';
    } else {
        output << "Shared module mapped. Entry/constructors were not called.\n";
    }
    for (const auto& ph : loads)
        memory::gpu_notify_map(static_cast<std::uintptr_t>(reservation->load_bias + ph.virtual_address),
                               ph.memory_size);
    return reservation;
}

struct Patch { Reservation* module; std::uint64_t target, value; std::uint32_t type; };

std::vector<abi::TlsTemplate> relocate(std::vector<std::unique_ptr<Reservation>>& loaded, std::ostream& output,
               const std::filesystem::path& pending_report, HostResolver resolver, bool require_complete=false, bool libc_probe=false,bool dependencies=false,bool constructors=false,bool run_entry=false,
               std::shared_ptr<void>* retained_services=nullptr) {
    struct Export { Reservation* module; const DynamicSymbol* symbol; };
    std::map<SymbolIdentity, std::vector<Export>> exports;
    struct TlsSource { Reservation* module; ProgramHeader header; };
    std::vector<TlsSource> tls_sources;
    for (const auto& module : loaded) {
        bool found_tls = false;
        for (const auto& ph : module->image.program_headers) if (ph.type == 7) {
            if (found_tls) throw std::runtime_error("Duplicate PT_TLS header.");
            found_tls = true;
            if (ph.memory_size) {
                module->tls_id = tls_sources.size()+1;
                if (ph.file_size) {
                    bool backed = false;
                    for (const auto& load : module->image.program_headers)
                        if (load.type == 1 && (load.flags & 4) && ph.virtual_address >= load.virtual_address &&
                            ph.virtual_address-load.virtual_address <= load.file_size &&
                            ph.file_size <= load.file_size-(ph.virtual_address-load.virtual_address) &&
                            ph.offset == load.offset+(ph.virtual_address-load.virtual_address)) backed = true;
                    if (!backed) throw std::runtime_error("TLS initializer is not backed by readable PT_LOAD bytes.");
                }
                tls_sources.push_back({module.get(), ph});
            }
        }
        module->dynamic = read_dynamic(*module->file, module->image);
        for (const auto& symbol : module->dynamic.symbols) {
            const auto bind = symbol.info >> 4;
            if (symbol.identity && symbol.section && symbol.section < 0xff00 &&
                (bind == 1 || bind == 2) && (symbol.visibility == 0 || symbol.visibility == 3))
                exports[*symbol.identity].push_back({module.get(), &symbol});
        }
    }
    std::vector<Patch> patches;
    std::size_t pending{}, relative{}, local{}, bound{}, host_bound{}, tls_applied{}, weak_zero{}, compatibility_bound{};
    struct Pending { std::string module, name, reason; Relocation rel; };
    std::vector<Pending> pending_rows;
    std::map<std::string, std::size_t> reasons;
    for (const auto& module : loaded) {
        std::set<std::uint64_t> targets;
        std::set<std::string> missing;
        std::size_t module_pending{}, module_applied{};
        for (const auto& rel : module->dynamic.relocations) {
            const auto pend = [&](const char* reason) {
                ++pending; ++module_pending; ++reasons[reason];
                pending_rows.push_back({module->name, module->dynamic.symbols[rel.symbol].name, reason, rel});
            };
            if (rel.type == 0) continue;
            // Validate even unsupported relocations; never accept a write into a gap.
            (void)module->host(rel.target, 8);
            const auto next = targets.lower_bound(rel.target);
            if ((next != targets.end() && *next - rel.target < 8) ||
                (next != targets.begin() && rel.target - *std::prev(next) < 8))
                throw std::runtime_error("Overlapping relocation targets.");
            targets.insert(rel.target);
            if (rel.type == 16) { // R_X86_64_DTPMOD64: store a module ID, not a pointer.
                const auto& symbol = module->dynamic.symbols[rel.symbol];
                const auto type = symbol.info & 15;
                // Observed Orbis marker: unnamed LOCAL SECTION, SHN_UNDEF,
                // zero value/size. It denotes this module's TLS for DTPMOD64.
                const bool orbis_marker = (module->image.header.type == 0xfe18 || module->image.header.type == 0xfe10) &&
                    !symbol.section && !symbol.value && !symbol.size && !symbol.visibility;
                const bool local_section = symbol.name.empty() && type == 3 &&
                    ((symbol.section && symbol.section < 0xff00) || orbis_marker) && (symbol.info >> 4) == 0;
                const bool local_tls = type == 6 && symbol.section && symbol.section < 0xff00 &&
                    (symbol.info >> 4) == 0;
                if (module->tls_id && !rel.addend && (!rel.symbol || local_section || local_tls)) {
                    patches.push_back({module.get(), rel.target, module->tls_id, rel.type});
                    ++tls_applied; ++module_applied;
                } else pend("tls_relocation");
                continue;
            }
            if (rel.type == 8) {
                // RELATIVE always uses B+A. Orbis libc may retain a nonzero
                // symbol index, but that symbol does not participate in the value.
                patches.push_back({module.get(), rel.target, add_signed(module->load_bias, rel.addend), rel.type});
                ++relative; ++module_applied;
                continue;
            }
            if (rel.type != 1 && rel.type != 6 && rel.type != 7) {
                pend((rel.type == 16 || rel.type == 17 || rel.type == 18) ? "tls_relocation" : "unsupported_type");
                continue;
            }
            if ((rel.type == 6 || rel.type == 7) && rel.addend != 0) {
                pend("nonzero_addend");
                continue;
            }
            const auto& symbol = module->dynamic.symbols[rel.symbol];
            if ((symbol.info & 15) == 6) { pend("tls_symbol"); continue; }
            std::uint64_t address{};
            bool resolved = false, external = false, host = false, weak = false;
            if (!rel.symbol) { resolved = true; }
            else if (symbol.section == 0xfff1) { address = symbol.value; resolved = true; }
            else if (symbol.section && symbol.section < 0xff00) {
                address = module->host(symbol.value, std::max<std::uint64_t>(1, symbol.size));
                resolved = true;
            } else if (!symbol.section && symbol.identity &&
                       ((symbol.info >> 4) == 1 || (symbol.info >> 4) == 2)) {
                const auto it = exports.find(*symbol.identity);
                if (it != exports.end()) {
                    std::vector<Export> compatible;
                    for (const auto& entry : it->second) {
                        const auto from = symbol.info & 15, to = entry.symbol->info & 15;
                        if (to != 6 && (!from || !to || from == to)) compatible.push_back(entry);
                    }
                    if (compatible.size() > 1) throw std::runtime_error("Ambiguous guest export binding.");
                    if (compatible.size() == 1) {
                        const auto& entry = compatible.front();
                        address = entry.module->host(entry.symbol->value, std::max<std::uint64_t>(1, entry.symbol->size));
                        resolved = true; external = true;
                    }
                }
            }
            // Explicit internal-libc bridges backed by exact NID matches in
            // the resident libc. These rules are per-function; there is no
            // library-wide version or namespace alias.
            if(!resolved&&!symbol.section&&symbol.identity&&(symbol.info&15)==2){
                const auto& [nid,library,version,provider,provider_version]=*symbol.identity;
                const bool internal_libc=library=="libSceLibcInternal"&&provider=="libSceLibcInternal"&&version==1&&provider_version==0x0101;
                const bool supported_nid=nid=="H2e8t5ScQGc"||nid=="Q3VBxCXhUHs"||nid=="8zTFvBIAIN8";
                if(internal_libc&&supported_nid){
                    const auto found=exports.find(SymbolIdentity{nid,"libc",1,"libc",0x0101});
                    if(found!=exports.end()){
                        const Export* selected{};
                        for(const auto& entry:found->second)if((entry.symbol->info&15)==2){
                            if(selected)throw std::runtime_error("Ambiguous internal-libc compatibility bridge.");selected=&entry;
                        }
                        if(selected){address=selected->module->host(selected->symbol->value,std::max<std::uint64_t>(1,selected->symbol->size));
                            resolved=true;external=true;++compatibility_bound;
                        }
                    }
                }
            }
            // Guest exports retain precedence. The application injects only tested,
            // typed host services; an unknown import remains pending.
            if (!resolved && !symbol.section && symbol.identity && resolver &&
                ((symbol.info >> 4) == 1 || (symbol.info >> 4) == 2)) {
                const auto& [nid, library, library_version, mod, module_version] = *symbol.identity;
                const auto target = resolver({nid, library, mod, library_version, module_version,
                                              static_cast<std::uint8_t>(symbol.info & 15)});
                if (target && *target) { address = *target; resolved = true; host = true; }
                if (module->name == "eboot.elf" && library == "libSceGnmDriver") {
                    output << "Runtime GNM import [" << symbol.name << "] slot=0x" << std::hex
                           << rel.target << " target=0x" << (target ? *target : 0) << std::dec
                           << (target && *target ? " resolved" : " unresolved") << '\n';
                }
            }
            // ELF undefined weak symbols have value zero after normal lookup.
            // This is not a success stub: callers must handle absent optional symbols.
            if(!resolved && !symbol.section && (symbol.info>>4)==2){resolved=true;weak=true;}
            if (!resolved) {
                missing.insert(symbol.name);
                if (module->name == "eboot.elf" && symbol.identity &&
                    std::get<1>(*symbol.identity) == "libSceGnmDriver")
                    output << "Runtime GNM pending [" << symbol.name << "] slot=0x" << std::hex
                           << rel.target << std::dec << " reason=unresolved\n";
                pend("unresolved_symbol");
                continue;
            }
            const auto value = rel.type == 1 ? add_signed(address, rel.addend) : address;
            patches.push_back({module.get(), rel.target, value, rel.type});
            if(weak)++weak_zero;
            else if (host) ++host_bound;
            else if (external) ++bound; else ++local;
            ++module_applied;
        }
        output << "Relocation plan [" << module->name << "]: ready=" << module_applied << " pending=" << module_pending
               << " unresolved_symbols=" << missing.size() << '\n';
    }
    // Validate resource requirements before any writes. Templates are snapshotted
    // after relocations so initialized TLS includes any relocated pointer values.
    std::uint64_t tls_total{};
    for (const auto& source : tls_sources) {
        if (source.header.memory_size > 16ull*1024*1024 || source.header.alignment > 1024*1024)
            throw std::runtime_error("TLS template exceeds budget.");
        tls_total += source.header.memory_size;
        if (tls_total > 64ull*1024*1024) throw std::runtime_error("Thread TLS exceeds budget.");
    }
    if (require_complete && pending) throw std::runtime_error("Synthetic fixture has unresolved relocations; refusing execution.");
    if (!pending_report.empty()) {
        std::ofstream report(pending_report, std::ios::binary);
        report.exceptions(std::ios::badbit | std::ios::failbit);
        const auto quoted = [](const std::string& value) {
            std::string result = "\"";
            for (char c : value) { if (c == '"') result += '"'; result += c; }
            return result + '"';
        };
        report << "requester,target,type,symbol_index,raw_name,reason\n";
        for (const auto& row : pending_rows)
            report << quoted(row.module) << ",0x" << std::hex << row.rel.target << std::dec
                   << ',' << row.rel.type << ',' << row.rel.symbol << ',' << quoted(row.name)
                   << ',' << row.reason << '\n';
        report.close();
    }
    // Batch relocation writes by host page. The former per-relocation
    // VirtualProtect/cache-flush sequence made a 30k-relocation image take
    // minutes to prepare, despite all targets being known before this point.
    SYSTEM_INFO relocation_page_info{};
    GetSystemInfo(&relocation_page_info);
    const auto relocation_page_size = static_cast<std::uintptr_t>(relocation_page_info.dwPageSize);
    std::map<std::uintptr_t, std::vector<const Patch*>> relocation_pages;
    for (const auto& patch : patches) {
        const auto address = static_cast<std::uintptr_t>(patch.module->host(patch.target, 8));
        const auto page = address & ~(relocation_page_size - 1);
        if (address - page > relocation_page_size - sizeof(patch.value))
            throw std::runtime_error("Relocation crosses a host page boundary.");
        relocation_pages[page].push_back(&patch);
    }
    std::set<std::pair<Reservation*, std::uint32_t>> samples;
    for (const auto& [page, page_patches] : relocation_pages) {
        auto* destination = reinterpret_cast<void*>(page);
        DWORD previous{};
        if (!VirtualProtect(destination, relocation_page_size, PAGE_READWRITE, &previous))
            fail("VirtualProtect relocation page");
        bool verified = true;
        for (const auto* patch : page_patches) {
            auto* target = reinterpret_cast<void*>(patch->module->host(patch->target, 8));
            std::memcpy(target, &patch->value, 8);
            std::uint64_t actual{};
            std::memcpy(&actual, target, 8);
            verified = verified && actual == patch->value;
        }
        const bool flushed = FlushInstructionCache(GetCurrentProcess(), destination, relocation_page_size) != 0;
        DWORD ignored{};
        if (!VirtualProtect(destination, relocation_page_size, previous, &ignored))
            fail("VirtualProtect restore relocation page");
        if (!flushed) fail("FlushInstructionCache relocation page");
        if (!verified) throw std::runtime_error("Relocation write verification failed.");
        for (const auto* patch : page_patches) {
            if (samples.emplace(patch->module, patch->type).second)
                output << "Verified relocation [" << patch->module->name << "]: target=0x"
                       << std::hex << patch->target << " value=0x" << patch->value << std::dec << '\n';
        }
    }
    std::vector<abi::TlsTemplate> templates;
    for (const auto& source : tls_sources) {
        abi::TlsTemplate item{source.module->tls_id, source.header.memory_size,
                             std::max<std::uint64_t>(1, source.header.alignment), {}};
        if (source.header.file_size) {
            const auto* data = reinterpret_cast<const std::byte*>(source.module->host(source.header.virtual_address, source.header.file_size));
            item.initial.assign(data, data+source.header.file_size);
        }
        output << "TLS template [" << source.module->name << "]: module_id=" << item.module_id
               << " initialized=" << item.initial.size() << " total=" << item.memory_size << '\n';
        templates.push_back(std::move(item));
    }
    auto services=std::make_shared<RuntimeServices>();
    services->tls=std::make_unique<abi::ThreadTls>(templates);
    services->tls_attachment=std::make_unique<abi::ScopedThreadTls>(*services->tls);
    for(const auto& module:loaded) if(module->image.header.type!=0xfe18) {
        for(const auto& ph:module->image.program_headers) if(ph.type==0x61000001) {
            if(services->process_parameters)throw std::runtime_error("Multiple process parameter blocks in module set.");
            bool backed=false;
            for(const auto& load:module->image.program_headers)
                if(load.type==1 && (load.flags&4) && ph.virtual_address>=load.virtual_address &&
                   ph.virtual_address-load.virtual_address<=load.file_size &&
                   ph.file_size<=load.file_size-(ph.virtual_address-load.virtual_address) &&
                   ph.offset==load.offset+(ph.virtual_address-load.virtual_address))backed=true;
            if(!backed)throw std::runtime_error("Process parameter is not backed by readable PT_LOAD.");
            const auto* address=reinterpret_cast<const std::byte*>(module->host(ph.virtual_address,ph.file_size));
            services->process_parameters.emplace(std::span<const std::byte>(address,static_cast<std::size_t>(ph.file_size)));
            if(abi::get_proc_param()!=address)throw std::runtime_error("Process parameter resolver pointer mismatch.");
            output << "Process parameters verified [" << module->name << "]: guest_va=0x" << std::hex
                   << ph.virtual_address << std::dec << " bytes=" << ph.file_size << '\n';
        }
    }
    for(const auto& module:loaded)services->module_records.push_back(module_record(*module,static_cast<std::int32_t>(services->module_records.size()+1)));
    services->module_registry=std::make_unique<abi::ScopedModuleRegistry>(services->module_records);
    for(const auto& record:services->module_records){abi::ModuleInfoEx queried{};
        if(abi::module_from_address(record.segments[0].address,2,&queried)||queried.id!=record.id||queried.eh_frame_size!=record.eh_frame_size)
            throw std::runtime_error("Module query diagnostic mismatch.");
    }
    output << "Module address registry verified: " << services->module_records.size() << " images.\n";
    services->direct_memory=std::make_unique<memory::ScopedDirectMemory>();
    services->video_out=std::make_unique<gpu::ScopedVideoOut>();
    services->compute_queues=std::make_unique<gpu::ScopedComputeQueues>();
    services->flexible_memory=std::make_unique<memory::ScopedFlexibleMemory>();
    services->heap_api=std::make_unique<abi::ScopedHeapApi>();
    services->thread_services=std::make_unique<abi::ScopedThreadServices>();
    services->pthread_runtime=std::make_unique<abi::PthreadRuntime>(templates);
    services->pthread_attachment=std::make_unique<abi::ScopedPthreadRuntime>(*services->pthread_runtime);
    services->event_queues=std::make_unique<abi::ScopedEventQueues>();
    for (const auto& item : templates) {
        const abi::TlsIndex index{item.module_id,0};
        auto* address = abi::tls_get_addr(&index);
        const auto data = services->tls->resolve(item.module_id,0,static_cast<std::size_t>(item.memory_size));
        if (address != data.data() || !std::equal(item.initial.begin(), item.initial.end(), data.begin()) ||
            !std::all_of(data.begin()+item.initial.size(),data.end(),[](auto b){return b==std::byte{};}))
            throw std::runtime_error("Attached module TLS verification failed.");
    }
    output << "__tls_get_addr verified for " << templates.size() << " modules on the diagnostic thread.\n";
    output << "Thread TLS prepared: " << templates.size() << " modules, " << tls_total << " bytes.\n";
    output << "Relocations applied and verified: relative=" << relative << " local=" << local
           << " guest_bindings=" << bound << " pending=" << pending << " host_bindings=" << host_bound
           << " tls_relocations=" << tls_applied << " weak_zero=" << weak_zero << " compatibility_bindings=" << compatibility_bound << '\n';
    for (const auto& [reason, count] : reasons) output << "Pending " << reason << ": " << count << '\n';
    if (pending) output << "Partial link only: unresolved host/TLS/unsupported relocations remain untouched.\n";
    else output << "All relocation records resolved for this module set.\n";
    if(libc_probe){
        const Reservation* libc=nullptr;
        for(const auto& module:loaded)if(module->name=="libc.elf")libc=module.get();
        if(!libc||!libc->dynamic.init)throw std::runtime_error("Pinned libc has no DT_INIT.");
        using Entry=std::int32_t (LBP3_GUEST_ABI *)(std::uint64_t,const void*,void*);
        Entry volatile entry=reinterpret_cast<Entry>(libc->host(*libc->dynamic.init,1));
        output << "PROBE_CALL: pinned libc DT_INIT(0, nullptr, nullptr); eboot entry is not called.\n" << std::flush;
        const auto result=entry(0,nullptr,nullptr);
        output << "PROBE_RETURN: libc DT_INIT=" << result << '\n' << std::flush;
        if(result)throw std::runtime_error("Controlled libc DT_INIT returned failure.");
        const auto heap=abi::heap_api_snapshot();
        if(!heap||(*heap)[0]!=libc->host(0x950,1)||(*heap)[1]!=libc->host(0x970,1)||(*heap)[6]!=libc->host(0x9d0,1))
            throw std::runtime_error("Real libc heap registration differs from audited callbacks.");
        output << "PROBE_HEAP: audited malloc/free callbacks registered; testing 64 bytes.\n" << std::flush;
        using Allocate=void* (LBP3_GUEST_ABI *)(std::uint64_t);
        using Release=void (LBP3_GUEST_ABI *)(void*);
        Allocate volatile allocate=reinterpret_cast<Allocate>((*heap)[0]);
        Release volatile release=reinterpret_cast<Release>((*heap)[1]);
        constexpr std::array<std::size_t,12> sizes{1,8,15,16,63,64,255,4096,16384,65536,262144,1048576};
        std::array<std::byte*,sizes.size()> allocations{};
        for(unsigned round=0;round<3;++round){
            for(std::size_t i=0;i<sizes.size();++i){
                output << "PROBE_ALLOCATE: round=" << round << " size=" << sizes[i] << '\n' << std::flush;
                auto* allocation=static_cast<std::byte*>(allocate(sizes[i]));
                if(!allocation||!memory::owns_writable_range(reinterpret_cast<std::uintptr_t>(allocation),sizes[i]))
                    throw std::runtime_error("Libc allocation is not within owned writable guest memory.");
                if(reinterpret_cast<std::uintptr_t>(allocation)%16)
                    throw std::runtime_error("Libc malloc returned an unaligned allocation.");
                for(std::size_t j=0;j<i;++j){
                    const auto a=reinterpret_cast<std::uintptr_t>(allocation),b=reinterpret_cast<std::uintptr_t>(allocations[j]);
                    if(a<b+sizes[j]&&b<a+sizes[i])throw std::runtime_error("Live libc allocations overlap.");
                }
                allocations[i]=allocation;
                std::fill_n(allocation,sizes[i],static_cast<std::byte>(i+round+1));
            }
            for(std::size_t i=sizes.size();i-->0;){
                const auto expected=static_cast<std::byte>(i+round+1);
                if(!std::all_of(allocations[i],allocations[i]+sizes[i],[&](std::byte value){return value==expected;}))
                    throw std::runtime_error("Libc allocation write/read verification failed.");
                release(allocations[i]);
            }
        }
        release(nullptr);
        output << "PROBE_HEAP_RETURN: 36 real libc allocations, alignment, isolation, contents, reuse and free(nullptr) verified.\n" << std::flush;
        output << "PROBE_HEAP_THREADS: two guest pthread workers, 128 allocations each.\n" << std::flush;
        std::array<ProbeHeapWorker,2> workers{{{(*heap)[0],(*heap)[1],17,false},{(*heap)[0],(*heap)[1],91,false}}};
        std::array<abi::GuestHandle,2> handles{};
        std::size_t started{};bool passed=true;
        for(std::size_t i=0;i<workers.size();++i){
            if(abi::sce_pthread_create(&handles[i],nullptr,&probe_heap_worker,&workers[i],"libc-heap-probe")){passed=false;break;}
            ++started;
        }
        for(std::size_t i=0;i<started;++i){void* result{};
            if(abi::sce_pthread_join(handles[i],&result)||result!=&workers[i]||!workers[i].passed)passed=false;
        }
        if(!passed)throw std::runtime_error("Concurrent real libc heap verification failed.");
        output << "PROBE_HEAP_THREADS_RETURN: 256 concurrent allocations and guest thread TLS lifecycle verified.\n" << std::flush;
        if(dependencies){
            const std::array<std::string_view,6> order{"libSceFios2.elf","fmodextinput.elf","fmodsmsreverb.elf","fmodsmswavehammer.elf","fmodvoipmixer.elf","spu.elf"};
            std::vector<Reservation*> started_modules;
            using Lifecycle=std::int32_t (LBP3_GUEST_ABI *)(std::uint64_t,const void*,void*);
            for(const auto name:order){
                Reservation* target{};for(const auto& module:loaded)if(module->name==name)target=module.get();
                if(!target||!target->dynamic.init||*target->dynamic.init!=0||!target->dynamic.fini)
                    throw std::runtime_error("Pinned shared-module lifecycle differs from audit.");
                output << "PROBE_MODULE_INIT: " << name << "(0, nullptr, nullptr).\n" << std::flush;
                Lifecycle volatile initialize=reinterpret_cast<Lifecycle>(target->host(*target->dynamic.init,1));
                const auto status=initialize(0,nullptr,nullptr);
                output << "PROBE_MODULE_INIT_RETURN: " << name << '=' << status << '\n' << std::flush;
                if(status)throw std::runtime_error("Shared dependency initialization returned error.");
                started_modules.push_back(target);
            }
            if(constructors){
                Reservation* eboot{};for(const auto& module:loaded)if(module->name=="eboot.elf")eboot=module.get();
                if(!eboot||eboot->dynamic.init!=0x20||eboot->dynamic.fini!=0xd18330)
                    throw std::runtime_error("Pinned eboot constructor wrapper differs from audit.");
                using ConstructorWrapper=void (LBP3_GUEST_ABI *)();
                if(run_entry){
                    if(eboot->image.header.entry!=0xcab0)throw std::runtime_error("Pinned eboot entry differs from audit.");
                    struct EntryParameters {std::int32_t argc{1};std::uint32_t padding{};const char* argv[33]{"/app0/eboot.bin"};std::uintptr_t entry{};};
                    static_assert(offsetof(EntryParameters,argv)==8&&sizeof(EntryParameters)==280);
                    EntryParameters parameters;parameters.entry=eboot->host(eboot->image.header.entry,1);
                    // This executable's audited static offset -0x1a0 denotes
                    // TLS byte 0x60 in the 0x200-byte main executable block.
                    const auto main_tls=std::find_if(templates.begin(),templates.end(),[&](const auto& item){return item.module_id==eboot->tls_id;});
                    if(main_tls==templates.end()||main_tls->memory_size!=0x200)
                        throw std::runtime_error("Pinned eboot static TLS contract differs from audit.");
                    std::uint64_t static_offset{};std::memcpy(&static_offset,reinterpret_cast<void*>(eboot->host(0xfa8178,8)),8);
                    if(static_offset!=0xfffffffffffffe60ull)throw std::runtime_error("Pinned eboot static TLS offset changed.");
                    services->guest_fs=std::make_unique<abi::ScopedGuestFs>(eboot->load_bias,eboot->load_bias+0xf420ac,eboot->tls_id);
                    using ExitCallback=void (LBP3_GUEST_ABI *)() noexcept;
                    using Entry=void (LBP3_GUEST_ABI *)(EntryParameters*,ExitCallback);
                    Entry volatile invoke=reinterpret_cast<Entry>(parameters.entry);
                    output << "PROBE_EBOOT_ENTRY: audited SysV entry(&parameters, exit_callback); argc=1, readonly /app0.\n" << std::flush;
                    invoke(&parameters,&probe_process_finalizer);
                    throw std::runtime_error("Pinned eboot entry unexpectedly returned.");
                }
                output << "PROBE_EBOOT_CONSTRUCTORS: DT_INIT void(void); entry 0xcab0 is not called.\n" << std::flush;
                ConstructorWrapper volatile initialize=reinterpret_cast<ConstructorWrapper>(eboot->host(*eboot->dynamic.init,1));
                initialize();
                output << "PROBE_EBOOT_CONSTRUCTORS_RETURN: constructor wrapper returned.\n" << std::flush;
                output << "PROBE_EBOOT_DESTRUCTORS: DT_FINI void(void).\n" << std::flush;
                ConstructorWrapper volatile finalize=reinterpret_cast<ConstructorWrapper>(eboot->host(*eboot->dynamic.fini,1));
                finalize();
                output << "PROBE_EBOOT_DESTRUCTORS_RETURN: destructor wrapper returned.\n" << std::flush;
            }
            for(auto i=started_modules.rbegin();i!=started_modules.rend();++i){auto& target=**i;
                output << "PROBE_MODULE_FINI: " << target.name << '\n' << std::flush;
                Lifecycle volatile finalize=reinterpret_cast<Lifecycle>(target.host(*target.dynamic.fini,1));
                const auto status=finalize(0,nullptr,nullptr);
                output << "PROBE_MODULE_FINI_RETURN: " << target.name << '=' << status << '\n' << std::flush;
                if(status)throw std::runtime_error("Shared dependency finalization returned error.");
            }
        }
        if(!libc->dynamic.fini||*libc->dynamic.fini!=0x7fca0)
            throw std::runtime_error("Pinned libc finalization contract differs from audited wrapper.");
        abi::cleanup_thread_specific();
        output << "PROBE_FINI: audited libc DT_FINI(0, nullptr, nullptr).\n" << std::flush;
        using Finalize=std::int32_t (LBP3_GUEST_ABI *)(std::uint64_t,const void*,void*);
        Finalize volatile finalize=reinterpret_cast<Finalize>(libc->host(*libc->dynamic.fini,1));
        const auto final_result=finalize(0,nullptr,nullptr);
        output << "PROBE_FINI_RETURN: libc DT_FINI=" << final_result << '\n' << std::flush;
        if(final_result)throw std::runtime_error("Pinned libc DT_FINI returned an error.");
    }
    if(retained_services)*retained_services=std::move(services);
    return templates;
}
}

void check_mapping(const std::filesystem::path& path, std::ostream& output) {
    const auto reservation = map_image(path, output, false);
    output << "Mapping successful. Guest code was not executed. Memory released on return.\n";
}

void probe_lbp3_libc(const std::filesystem::path& directory,std::ostream& output,HostResolver resolver,bool dependencies,bool constructors,bool entry){
    const std::map<std::string,std::string> pins{
        {"eboot.elf","c5387ed91227a406e230351539107e81ff507f84f230df8db79d4d2438b7b86e"},
        {"fmodextinput.elf","24944290c49358f8e64215945d780907c0f913e78c1164ab0af4b0d331ab6776"},
        {"fmodsmsreverb.elf","06b30072c2f4903694eb0d207d430a701b5d4c7ec8ce3fd9d854001d0865abfa"},
        {"fmodsmswavehammer.elf","f1f8591532e8f9b652f87460b13ff29a7eb70bb469a9114947c9a8309296a150"},
        {"fmodvoipmixer.elf","ecf7f8f68745058555b56b30fc7befaee1d29089fd7d69e1a0ba48be0a745204"},
        {"libSceFios2.elf","63e0253b9b3411a100b1aa33323a1e36ca56b93d01f5743108f3104321016542"},
        {"libc.elf","9811633cd5d49ae461ec0550468db5ef037d576abe1d732348844b930f397566"},
        {"spu.elf","698563ceae0b48a0c7f90984e43cd4bab89ba7728b1d06b5d77e1a5ef9849cb5"}};
    for(const auto& entry:std::filesystem::directory_iterator(directory))
        if(entry.path().extension()==L".elf"&&!pins.contains(entry.path().filename().string()))throw std::runtime_error("Unpinned ELF in libc probe set.");
    std::vector<std::unique_ptr<Reservation>> loaded;
    for(const auto& [name,expected]:pins){
        output << "Module: " << name << '\n';auto module=map_image(directory/name,output,true);
        if(module->file->size()>64*1024*1024)throw std::runtime_error("Pinned probe image exceeds budget.");
        std::vector<std::byte> bytes(static_cast<std::size_t>(module->file->size()));module->file->read_at(0,bytes);
        BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};std::array<unsigned char,32> digest{};
        auto status=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0);
        if(status>=0)status=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0);
        if(status>=0)status=BCryptHashData(hash,reinterpret_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),0);
        if(status>=0)status=BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0);
        if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);
        if(status<0)throw std::runtime_error("Cannot hash pinned probe ELF.");
        std::string actual;constexpr char hex[]="0123456789abcdef";
        for(auto value:digest){actual+=hex[value>>4];actual+=hex[value&15];}
        if(actual!=expected)throw std::runtime_error("SHA-256 mismatch in pinned libc probe set.");
        loaded.push_back(std::move(module));
    }
    (void)relocate(loaded,output,{},resolver,true,true,dependencies,constructors,entry);
}

namespace {
void module_set(const std::filesystem::path& directory, std::ostream& output, bool link,
                const std::filesystem::path& pending_report = {}, HostResolver resolver = nullptr) {
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == L".elf") {
            paths.push_back(entry.path());
            if (paths.size() > 64) throw std::runtime_error("Module set exceeds 64 images.");
        }
    }
    if (paths.empty()) throw std::runtime_error("Module directory has no .elf images.");
    std::sort(paths.begin(), paths.end());
    std::vector<std::unique_ptr<Reservation>> reservations;
    for (const auto& path : paths) {
        output << "\nModule: " << path.filename().string() << '\n';
        reservations.push_back(map_image(path, output, true));
    }
    output << "\nModule set mapping successful: " << reservations.size()
           << " images resident simultaneously.\n";
    if (link) relocate(reservations, output, pending_report, resolver);
    else output << "No relocations or bindings applied. ";
    output << "Guest code was not executed. Memory released on return.\n";
}
}
void check_module_set(const std::filesystem::path& directory, std::ostream& output) {
    module_set(directory, output, false);
}
void check_link_set(const std::filesystem::path& directory, std::ostream& output,
                    const std::filesystem::path& pending_report, HostResolver resolver) {
    module_set(directory, output, true, pending_report, resolver);
}

MappedModuleSession::MappedModuleSession(const std::filesystem::path& directory,
                                         std::ostream& output)
    : state_(std::make_unique<State>()) {
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == L".elf") {
            paths.push_back(entry.path());
            if (paths.size() > 64) throw std::runtime_error("Module set exceeds 64 images.");
        }
    }
    if (paths.empty()) throw std::runtime_error("Module directory has no .elf images.");
    std::sort(paths.begin(), paths.end());
    for (const auto& path : paths) {
        output << "\nModule: " << path.filename().string() << '\n';
        state_->modules.push_back(map_image(path, output, true));
    }
    output << "\nResident module mapping successful: " << state_->modules.size()
           << " images remain mapped until the session is destroyed. No relocations or guest code executed.\n";
}

MappedModuleSession::~MappedModuleSession() {
    // The ISS rasterizer has process-global hooks and a detached window thread,
    // but no shutdown API. Keep callback targets and guest images valid until
    // process termination instead of leaving dangling global callbacks.
    if (state_ && state_->iss_gpu_lifetime_pinned) {
        OutputDebugStringW(L"LBP3: retaining ISS GPU session until process exit; backend has no shutdown API.\n");
        (void)state_.release();
    }
}

std::uintptr_t MappedModuleSession::entry(std::string_view name) const noexcept {
    if (!state_) return 0;
    for (const auto& module : state_->modules)
        if (module->name == name && module->image.header.type != 0xfe18)
            return static_cast<std::uintptr_t>(module->host(module->image.header.entry, 1));
    return 0;
}

std::size_t MappedModuleSession::module_count() const noexcept {
    return state_ ? state_->modules.size() : 0;
}

void MappedModuleSession::link(std::ostream& output, HostResolver resolver,
                               const std::filesystem::path& pending_report) {
    if (!state_) throw std::runtime_error("Module session is not initialized.");
    if (state_->services) throw std::runtime_error("Module session has already been linked.");
    (void)relocate(state_->modules, output, pending_report, resolver, false, false, false, false, false,
                   &state_->services);
    if (state_->iss_gpu_backend_active)
        enable_gpu_fault_dispatch(&gpu::iss_backend_handle_fault);
    output << "Resident link contexts active: TLS, process parameters, module registry, memory and "
              "guest service scopes remain active with the mapped images. Guest code was not executed.\n";
}

void MappedModuleSession::initialize_iss_gpu_backend(const char* title, const char* serial,
                                                      const char* user_directory,
                                                      std::uint32_t sdk_version,
                                                      std::uint32_t psf_attributes,
                                                      std::int32_t width,
                                                      std::int32_t height) {
    if (!state_) throw std::runtime_error("Module session is not initialized.");
    if (state_->iss_gpu_backend_active)
        throw std::runtime_error("ISS GPU backend is already active for this session.");
    if (state_->services)
        throw std::runtime_error("Initialize the ISS GPU backend before linking guest imports.");
    // tlggpu_init may register process-global hooks before it reports failure;
    // pin the session before entering it so a partial failure cannot dangle them.
    state_->iss_gpu_lifetime_pinned = true;
    const int status = gpu::initialize_iss_backend(title, serial, user_directory,
                                                   sdk_version, psf_attributes, width, height);
    if (status != 0)
        throw std::system_error(status, std::system_category(), "initialize ISS GPU backend");
    state_->iss_gpu_backend_active = true;
}

void MappedModuleSession::mount_guest_filesystem(const std::filesystem::path& app_root,
                                                  const std::filesystem::path& savedata_root,
                                                  const std::filesystem::path& temp_root) {
    if (!state_) throw std::runtime_error("Module session is not initialized.");
    if (state_->services || state_->iss_gpu_lifetime_pinned)
        throw std::runtime_error("Mount the guest filesystem before backend initialization and linking.");
    if (state_->guest_filesystem) throw std::runtime_error("Guest filesystem is already mounted.");
    state_->guest_filesystem = std::make_unique<io::ScopedFileSystem>(app_root, savedata_root, temp_root);
}

void MappedModuleSession::enable_gpu_fault_dispatch(memory::GpuFaultHandler handler) {
    if (!state_ || !state_->services)
        throw std::runtime_error("Link the resident module session before installing GPU fault dispatch.");
    auto services = std::static_pointer_cast<RuntimeServices>(state_->services);
    if (services->gpu_memory_tracking)
        throw std::runtime_error("GPU fault dispatch is already active for this session.");
    services->gpu_memory_tracking =
        std::make_unique<memory::ScopedGpuMemoryTracking>(handler);
}

namespace {
struct FixtureRun {
    abi::GuestThreadEntry entry{};
    std::uintptr_t low{}, high{};
    abi::ExecutionResult result;
    unsigned tls_after{};
};
void* LBP3_GUEST_ABI run_fixture(void* argument) noexcept {
    auto& run=*static_cast<FixtureRun*>(argument);
    abi::execute_fixture(run.entry,nullptr,run.low,run.high,&run.result);
    const abi::TlsIndex index{1,0};
    auto* ptr=abi::tls_get_addr(&index);
    run.tls_after=ptr ? std::to_integer<unsigned>(*ptr) : UINT_MAX;
    return argument;
}
}

void execution_self_test(const std::filesystem::path& artifacts,std::ostream& output,HostResolver resolver) {
    std::filesystem::create_directories(artifacts);
    for(bool fault : {false,true}) {
        const auto path=artifacts/(fault ? "synthetic-fault.elf" : "synthetic-success.elf");
        const auto bytes=synthetic_elf(fault);
        {
            std::ofstream file(path,std::ios::binary);
            file.exceptions(std::ios::badbit|std::ios::failbit);
            file.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
            file.close();
        }
        std::vector<std::unique_ptr<Reservation>> loaded;
        loaded.push_back(map_image(path,output,false));
        auto templates=relocate(loaded,output,{},resolver,true);
        auto& module=*loaded.front();
        FixtureRun run;
        run.low=module.host(0x1000,0x100);
        run.high=run.low+0x100;
        run.entry=reinterpret_cast<abi::GuestThreadEntry>(module.host(module.image.header.entry,1));
        abi::PthreadRuntime runtime(templates);
        abi::ScopedPthreadRuntime scope(runtime);
        abi::GuestHandle handle{};void* value{};
        if(abi::sce_pthread_create(&handle,nullptr,&run_fixture,&run,"synthetic-elf") ||
            abi::sce_pthread_join(handle,&value) || value!=&run)
            throw std::runtime_error("Synthetic ELF thread creation/join failed.");
        if(!fault) {
            if(run.result.fault || reinterpret_cast<std::uintptr_t>(run.result.value)!=73 || run.tls_after!=0x55)
                throw std::runtime_error("Synthetic ELF return/TLS/host call verification failed.");
            output << "Synthetic ELF executed: result=73, TLS=0x55, imported host calls verified.\n";
        } else {
            if(!run.result.fault || run.result.code!=EXCEPTION_ACCESS_VIOLATION ||
                run.result.rip-module.load_bias!=0x1002 || run.result.address || run.result.access || run.tls_after!=0x42)
                throw std::runtime_error("Synthetic ELF fault diagnostic verification failed.");
            std::ofstream report(artifacts/"synthetic-fault.json",std::ios::binary);
            report.exceptions(std::ios::badbit|std::ios::failbit);
            report << "{\n  \"fixture\": \"synthetic-fault.elf\",\n  \"expected_fault\": true,\n"
                   << "  \"exception_code\": \"0x" << std::hex << run.result.code << "\",\n"
                   << "  \"runtime_rip\": \"0x" << run.result.rip << "\",\n"
                   << "  \"guest_va\": \"0x" << run.result.rip-module.load_bias << "\",\n"
                   << "  \"access_address\": \"0x" << run.result.address << "\",\n"
                   << "  \"access\": \"read\",\n  \"thread_id\": " << std::dec << run.result.thread_id << "\n}\n";
            report.close();
            output << "Synthetic fault captured: code=0xc0000005 guest_va=0x1002 access=read address=0.\n";
        }
    }
    // A separate negative fixture must not reach the entry while imports remain.
    {
        auto bytes=synthetic_elf(false);
        const std::string name="6XG4B33N09g#B#C";
        const auto found=std::search(bytes.begin(),bytes.end(),name.begin(),name.end(),
            [](std::byte a,char b){ return std::to_integer<unsigned char>(a)==static_cast<unsigned char>(b); });
        if(found==bytes.end()) throw std::runtime_error("Synthetic unresolved import fixture is missing its symbol.");
        *found=std::byte{'?'};
        const auto path=artifacts/"synthetic-unresolved.elf";
        {
            std::ofstream file(path,std::ios::binary);
            file.exceptions(std::ios::badbit|std::ios::failbit);
            file.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
            file.close();
        }
        std::vector<std::unique_ptr<Reservation>> loaded;
        loaded.push_back(map_image(path,output,false));
        bool rejected=false;
        try { (void)relocate(loaded,output,{},resolver,true); }
        catch(const std::runtime_error& error) {
            if(std::string_view(error.what()).find("has unresolved relocations")==std::string_view::npos) throw;
            rejected=true;
        }
        if(!rejected) throw std::runtime_error("Synthetic unresolved fixture was accepted for execution.");
        output << "Unresolved synthetic ELF rejected before execution and relocation writes.\n";
    }
    output << "ELF execution self-test passed. Fixed synthetic ELF instructions executed; no game code was executed.\n";
}
}
