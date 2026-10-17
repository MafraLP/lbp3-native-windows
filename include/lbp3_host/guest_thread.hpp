#pragma once
#include "lbp3_host/tls.hpp"
#include <thread>

namespace lbp3::abi {
using GuestThreadEntry = void* (LBP3_GUEST_ABI *)(void*) noexcept;
// Host-side lifecycle primitive. Not yet an implementation of scePthreadCreate.
// Argument lifetime must cover the thread. Entry cannot throw across the ABI.
// Join or destroy on the controlling thread; destruction waits, never detaches.
class GuestThread {
public:
    GuestThread(std::span<const TlsTemplate> templates, GuestThreadEntry entry, void* argument,std::uint64_t stack_size=1024*1024);
    ~GuestThread();
    GuestThread(const GuestThread&) = delete;
    GuestThread& operator=(const GuestThread&) = delete;
    void* join();
    void* native_handle() noexcept {return worker_;}
private:
    struct State;
    struct Start;
    std::shared_ptr<State> state_;
    void* worker_{};unsigned thread_id_{};
};
void thread_self_test(std::ostream& output);
}
