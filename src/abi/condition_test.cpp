#include "lbp3_host/condition.hpp"
#include "lbp3_host/mutex.hpp"
#include "lbp3_host/abi.hpp"
#include <Windows.h>
#include <array>
#include <bit>
#include <future>
#include <ostream>
#include <stdexcept>
#include <thread>

namespace lbp3::abi {
void condition_self_test(std::ostream& output){
    using Handle=std::uint64_t;
    using Unary=std::int32_t (LBP3_GUEST_ABI *)(Handle*) noexcept;
    using Wait=std::int32_t (LBP3_GUEST_ABI *)(Handle*,Handle*) noexcept;
    using Timed=std::int32_t (LBP3_GUEST_ABI *)(Handle*,Handle*,std::uint64_t) noexcept;
    using Init=std::int32_t (LBP3_GUEST_ABI *)(Handle*,const void*,const char*) noexcept;
    using Setting=std::int32_t (LBP3_GUEST_ABI *)(Handle*,int) noexcept;
    const auto error=[](unsigned e){return std::bit_cast<std::int32_t>(0x80020000u|e);};
    const auto get=[](const char* nid,bool mutex=false){const HostImport request{nid,"libkernel","libkernel",1,0x0101,2};
        auto found=mutex?resolve_mutex(request):resolve_condition(request);
        if(!found)throw std::runtime_error("Condition test export missing.");return *found;};
    Init volatile init=reinterpret_cast<Init>(get("2Tb92quprl0"));
    Unary volatile destroy=reinterpret_cast<Unary>(get("g+PZd2hiacg"));
    Unary volatile signal=reinterpret_cast<Unary>(get("kDh-NfxgMtE"));
    Unary volatile broadcast=reinterpret_cast<Unary>(get("JGgj7Uvrl+A"));
    Wait volatile wait=reinterpret_cast<Wait>(get("WKAXJ4XBPQ4"));
    Timed volatile timed=reinterpret_cast<Timed>(get("BmMjYxmew1w"));
    Init volatile mutex_init=reinterpret_cast<Init>(get("cmo1RIYva9o",true));
    Unary volatile mutex_destroy=reinterpret_cast<Unary>(get("2Of0f+3mhhE",true));
    Unary volatile lock=reinterpret_cast<Unary>(get("9UK1vLZQft4",true));
    Unary volatile unlock=reinterpret_cast<Unary>(get("tn3VlD0hG60",true));
    Unary volatile attr_init=reinterpret_cast<Unary>(get("F8bUHwAG284",true));
    Unary volatile attr_destroy=reinterpret_cast<Unary>(get("smWEktiyyG0",true));
    Setting volatile attr_type=reinterpret_cast<Setting>(get("iMp8QpE+XO4",true));
    Handle condition{},mutex{},attr{};
    if(attr_init(&attr)||attr_type(&attr,2)||mutex_init(&mutex,&attr,"cond-recursive")||attr_destroy(&attr)||init(&condition,nullptr,"cond"))
        throw std::runtime_error("Condition setup failed.");
    if(wait(&condition,&mutex)!=error(1))throw std::runtime_error("Condition wait accepted unowned mutex.");
    std::promise<void> ready;auto future=ready.get_future();int predicate{};bool valid=false;
    std::thread worker([&]{const bool acquired=!lock(&mutex)&&!lock(&mutex);ready.set_value();
        const auto result=acquired?timed(&condition,&mutex,UINT64_MAX):error(5);
        valid=acquired&&!result&&predicate==42&&!unlock(&mutex)&&!unlock(&mutex)&&unlock(&mutex)==error(1);
    });
    future.wait();
    // Acquiring the mutex proves the waiter completed enqueue+release.
    const auto acquired=lock(&mutex);const auto released=acquired?error(5):unlock(&mutex);
    const auto busy_mutex=mutex_destroy(&mutex),busy_cond=destroy(&condition);
    const auto acquired_again=lock(&mutex);predicate=42;
    const auto notified=signal(&condition);const auto released_again=unlock(&mutex);worker.join();
    if(acquired||released||busy_mutex!=error(16)||busy_cond!=error(16)||acquired_again||notified||released_again||!valid)
        throw std::runtime_error("Condition signal lost wakeup, lease or recursive ownership.");
    if(lock(&mutex)||lock(&mutex)||timed(&condition,&mutex,1000)!=error(60)||timed(&condition,&mutex,0)!=error(60)||
       unlock(&mutex)||unlock(&mutex)||unlock(&mutex)!=error(1))throw std::runtime_error("Timed condition failed to restore recursive ownership.");
    if(destroy(&condition)||mutex_destroy(&mutex))throw std::runtime_error("Condition cleanup failed.");
    const auto stale=condition; // Tombstone must not materialize again.
    if(signal(&condition)!=error(22))throw std::runtime_error("Destroyed condition accepted.");
    condition=0;mutex=0;predicate=0;
    std::array<std::promise<void>,2> started;
    std::array<std::future<void>,2> futures{started[0].get_future(),started[1].get_future()};
    std::array<bool,2> results{};
    std::array<std::thread,2> workers;
    for(unsigned i=0;i<2;++i)workers[i]=std::thread([&,i]{const bool acquired_worker=!lock(&mutex);started[i].set_value();
        const auto result=acquired_worker?wait(&condition,&mutex):error(5);
        results[i]=acquired_worker&&!result&&predicate==99&&!unlock(&mutex);
    });
    for(auto& pending:futures)pending.wait();
    const auto held=lock(&mutex);predicate=99;
    const auto woke=broadcast(&condition),unheld=unlock(&mutex);
    for(auto& thread:workers)thread.join();
    if(held||woke||unheld||!results[0]||!results[1]||destroy(&condition)||mutex_destroy(&mutex))
        throw std::runtime_error("Lazy condition broadcast/concurrent wait failed.");
    Handle invalid=stale;
    if(signal(&invalid)!=error(22)||init(nullptr,nullptr,nullptr)!=error(22)||
       init(&invalid,reinterpret_cast<void*>(1),nullptr)!=error(45)||signal(reinterpret_cast<Handle*>(8))!=error(14))
        throw std::runtime_error("Condition invalid input accepted.");
    auto* readonly=static_cast<Handle*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READONLY));
    if(!readonly)throw std::runtime_error("Condition readonly fixture failed.");
    const auto denied=init(readonly,nullptr,nullptr);VirtualFree(readonly,0,MEM_RELEASE);
    if(denied!=error(14))throw std::runtime_error("Read-only condition output accepted.");
    std::array<Handle,128> all{};
    for(auto& value:all)if(init(&value,nullptr,nullptr))throw std::runtime_error("Condition capacity fixture failed.");
    Handle extra{};
    if(init(&extra,nullptr,nullptr)!=error(35))throw std::runtime_error("Condition capacity ignored.");
    const auto old=all[0];
    for(auto& value:all)if(destroy(&value))throw std::runtime_error("Condition capacity cleanup failed.");
    if(init(&extra,nullptr,nullptr)||extra==old)throw std::runtime_error("Condition generation not advanced.");
    invalid=old;if(signal(&invalid)!=error(22)||destroy(&extra))throw std::runtime_error("Stale condition handle accepted.");
    output << "Condition self-test passed: signal/broadcast, lazy initialization, atomic enqueue/release and recursive mutex reacquisition.\n"
              "Relative timeout, zero timeout, saturated deadline, busy leases, ownership, stale handles and 128-object capacity verified.\n";
}
}
