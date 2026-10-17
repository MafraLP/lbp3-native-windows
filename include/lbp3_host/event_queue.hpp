#pragma once
#include "lbp3_host/abi.hpp"
#include "lbp3_host/host_resolver.hpp"
#include <memory>
namespace lbp3::abi {
using EventQueueHandle=std::int64_t;
struct KernelEvent {std::uint64_t ident{};std::int16_t filter{};std::uint16_t flags{};std::uint32_t fflags{};std::uint64_t data{};void* userdata{};};
static_assert(sizeof(KernelEvent)==32&&offsetof(KernelEvent,userdata)==24);
struct EventQueueState;
class ScopedEventQueues {
public:
    ScopedEventQueues();~ScopedEventQueues();
    ScopedEventQueues(const ScopedEventQueues&)=delete;
    ScopedEventQueues& operator=(const ScopedEventQueues&)=delete;
private:std::unique_ptr<EventQueueState> state_;
};
std::int32_t LBP3_GUEST_ABI event_queue_create(EventQueueHandle*,const char*) noexcept;
std::int32_t LBP3_GUEST_ABI event_queue_delete(EventQueueHandle) noexcept;
std::int32_t LBP3_GUEST_ABI event_queue_wait(EventQueueHandle,KernelEvent*,int,int*,const std::uint32_t*) noexcept;
std::int32_t LBP3_GUEST_ABI event_add_user(EventQueueHandle,int) noexcept;
std::int32_t LBP3_GUEST_ABI event_add_user_edge(EventQueueHandle,int) noexcept;
std::int32_t LBP3_GUEST_ABI event_trigger_user(EventQueueHandle,int,void*) noexcept;
std::int32_t LBP3_GUEST_ABI event_delete_user(EventQueueHandle,int) noexcept;
std::int32_t LBP3_GUEST_ABI event_add_timer(EventQueueHandle,int,std::uint32_t,void*) noexcept;
std::int32_t LBP3_GUEST_ABI event_delete_timer(EventQueueHandle,int) noexcept;
// Host producers register exact filters, then signal matching subscriptions.
std::int32_t event_subscribe(EventQueueHandle,const KernelEvent&) noexcept;
std::int32_t event_remove(EventQueueHandle,std::uint64_t,std::int16_t) noexcept;
std::int32_t event_signal(EventQueueHandle,std::uint64_t,std::int16_t,std::uint64_t,void*) noexcept;
std::optional<std::size_t> event_waiter_count(EventQueueHandle);
std::optional<std::uintptr_t> resolve_event_queue(const HostImport&);
}
