#include "lbp3_host/guest_thread.hpp"
#include "lbp3_host/thread_services.hpp"
#include "lbp3_host/signal_mask.hpp"
#include <atomic>
#include <barrier>
#include <exception>
#include <future>
#include <ostream>
#include <stdexcept>
#include <Windows.h>
#include <process.h>
#include <system_error>

namespace lbp3::abi {
struct GuestThread::State { void* result{}; std::exception_ptr error;std::promise<void> startup;std::atomic<bool> completed{}; };
struct GuestThread::Start {
    std::shared_ptr<State> state;std::vector<TlsTemplate> templates;GuestThreadEntry entry{};void* argument{};SignalMask inherited_mask{};
    static unsigned __stdcall run(void* argument) noexcept {
        std::unique_ptr<Start> start(static_cast<Start*>(argument));const auto state=start->state;bool started=false;
        try{
            ULONG reserve=64*1024;if(!SetThreadStackGuarantee(&reserve))throw std::runtime_error("Cannot establish guest thread fault stack guarantee.");
            {
                ThreadTls storage(start->templates);ScopedThreadTls attached(storage);inherit_signal_mask(start->inherited_mask);
                state->startup.set_value();started=true;
                state->result=start->entry(start->argument);cleanup_thread_specific();
            }
            const TlsIndex probe{1,0};if(tls_get_addr(&probe))throw std::runtime_error("Guest thread TLS context leaked at completion.");
        }catch(...){state->error=std::current_exception();if(!started){try{state->startup.set_exception(state->error);}catch(...){}}}
        state->completed.store(true,std::memory_order_release);return 0;
    }
};

GuestThread::GuestThread(std::span<const TlsTemplate> templates, GuestThreadEntry entry, void* argument,std::uint64_t stack_size)
    : state_(std::make_shared<State>()) {
    if (!entry) throw std::invalid_argument("Guest thread requires an entry function.");
    if(stack_size<0x4000||stack_size>64ull*1024*1024)throw std::invalid_argument("Guest thread stack request exceeds supported limits.");
    auto start=std::make_unique<Start>();start->state=state_;start->templates.assign(templates.begin(),templates.end());
    start->entry=entry;start->argument=argument;start->inherited_mask=current_signal_mask();
    auto ready=state_->startup.get_future();
    // Preserve a separate host-runtime margin in addition to the requested
    // guest stack. _beginthreadex maintains Windows CRT thread-local state.
    const auto reservation=static_cast<unsigned>((stack_size+0x40000+0xffff)&~0xffffull);
    worker_=reinterpret_cast<void*>(_beginthreadex(nullptr,reservation,&Start::run,start.get(),STACK_SIZE_PARAM_IS_A_RESERVATION,&thread_id_));
    if(!worker_)throw std::runtime_error("Cannot create native guest thread.");
    (void)start.release();
    try { ready.get(); }
    catch (...) {WaitForSingleObject(worker_,INFINITE);CloseHandle(worker_);worker_=nullptr;throw;}
}

GuestThread::~GuestThread() {
    if (worker_) {
        if(thread_id_==GetCurrentThreadId()||WaitForSingleObject(worker_,INFINITE)!=WAIT_OBJECT_0)std::terminate();
        CloseHandle(worker_);worker_=nullptr;
    }
}

void* GuestThread::join() {
    if(!worker_)throw std::logic_error("Guest thread already joined.");
    if(thread_id_==GetCurrentThreadId())throw std::logic_error("Guest thread cannot join itself.");
    if(WaitForSingleObject(worker_,INFINITE)!=WAIT_OBJECT_0)throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"Guest thread wait");
    CloseHandle(worker_);worker_=nullptr;
    if(!state_->completed.load(std::memory_order_acquire))throw std::runtime_error("Guest thread completion was not published.");
    if (state_->error) std::rethrow_exception(state_->error);
    return state_->result;
}

namespace {
struct TestContext {
    std::barrier<>* sync{};
    std::atomic<unsigned>* complete{};
    unsigned value{};
    std::uintptr_t tls_address{};
    bool valid{};
};
__declspec(noinline) void* LBP3_GUEST_ABI synthetic_entry(void* argument) noexcept {
    auto& context = *static_cast<TestContext*>(argument);
    const TlsIndex first{1,0}, second{3,0};
    auto* bytes = tls_get_addr(&first);
    auto* zeros = tls_get_addr(&second);
    bool valid = bytes && zeros && bytes[0] == std::byte{0x42} && bytes[1] == std::byte{0x17} &&
        bytes[2] == std::byte{} && zeros[0] == std::byte{};
    context.tls_address = reinterpret_cast<std::uintptr_t>(bytes);
    if (bytes) bytes[0] = static_cast<std::byte>(context.value);
    context.sync->arrive_and_wait();
    context.valid = valid && bytes[0] == static_cast<std::byte>(context.value) && tls_get_addr(&first) == bytes;
    context.complete->fetch_add(1);
    return argument;
}
__declspec(noinline) void* LBP3_GUEST_ABI completion_entry(void* argument) noexcept {
    static_cast<std::atomic<unsigned>*>(argument)->fetch_add(1);
    return argument;
}
}

void thread_self_test(std::ostream& output) {
    std::vector<TlsTemplate> templates{{1,256,128,{std::byte{0x42},std::byte{0x17}}},{3,64,16,{}}};
    ThreadTls parent(templates);
    ScopedThreadTls attached(parent);
    const TlsIndex first{1,0};
    auto* parent_data = tls_get_addr(&first);
    parent_data[0] = std::byte{0x7f};
    std::barrier sync(2);
    std::atomic<unsigned> complete{};
    TestContext a{&sync,&complete,1}, b{&sync,&complete,2};
    GuestThread one(templates, &synthetic_entry, &a);
    GuestThread two(templates, &synthetic_entry, &b);
    // Each worker uses its snapshot, not these now-discarded initializer bytes.
    templates[0].initial.clear();
    if (one.join() != &a || two.join() != &b || complete.load() != 2 || !a.valid || !b.valid ||
        a.tls_address == b.tls_address || a.tls_address == reinterpret_cast<std::uintptr_t>(parent_data) ||
        tls_get_addr(&first) != parent_data || parent_data[0] != std::byte{0x7f})
        throw std::runtime_error("Managed guest thread lifecycle/isolation test failed.");
    bool rejected = false;
    try { (void)one.join(); } catch (const std::logic_error&) { rejected=true; }
    if (!rejected) throw std::runtime_error("Repeated thread join was accepted.");
    rejected = false;
    try { GuestThread invalid(templates,nullptr,nullptr); }
    catch (const std::invalid_argument&) { rejected=true; }
    if (!rejected) throw std::runtime_error("Null thread entry was accepted.");
    auto malformed = templates; malformed[0].alignment=3;
    rejected = false;
    try { GuestThread invalid(malformed,&completion_entry,&complete); }
    catch (const std::runtime_error&) { rejected=true; }
    if (!rejected || complete.load() != 2) throw std::runtime_error("Thread setup failure reached guest entry.");
    { GuestThread scoped(templates,&completion_entry,&complete); }
    if (complete.load() != 3) throw std::runtime_error("Thread destructor did not wait for completion.");
    output << "Thread self-test passed: create, TLS attach, SysV entry, result, detach and join.\n"
              "Two concurrent workers isolated from each other and parent; setup failure and repeated join checked.\n"
              "Synthetic functions only. No game code was executed.\n";
}
}
