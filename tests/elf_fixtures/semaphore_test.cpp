#include "lbp3_host/semaphore.hpp"
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace lbp3::abi;
constexpr std::int32_t err(unsigned e){return std::bit_cast<std::int32_t>(0x80020000u|e);}
void check(bool value){if(!value)throw std::runtime_error("semaphore assertion failed");}
int main(){try{
    std::uint32_t id{};
    check(sem_create(&id,"test",1,2,4,nullptr)==0&&id);
    check(sem_poll(id,0)==err(22)&&sem_poll(id,5)==err(22));
    check(sem_poll(id,2)==0&&sem_poll(id,1)==err(16));
    check(sem_signal(id,5)==err(22)&&sem_signal(id,-1)==err(22));
    std::uint32_t timeout=1000;
    check(sem_wait(id,1,&timeout)==err(60)&&timeout==0);
    check(sem_wait(id,1,reinterpret_cast<std::uint32_t*>(1))==err(14));
    check(sem_signal(id,1)==0);
    timeout=100000;check(sem_wait(id,1,&timeout)==0&&timeout>0&&timeout<=100000);
    std::atomic<unsigned> acquired{};
    std::array<std::thread,4> workers;
    for(auto& thread:workers)thread=std::thread([&]{if(!sem_wait(id,1,nullptr))++acquired;});
    check(sem_signal(id,4)==0);
    for(auto& thread:workers)thread.join();
    check(acquired==4&&sem_poll(id,1)==err(16));
    std::int32_t canceled{},wait_result{};
    std::thread waiter([&]{wait_result=sem_wait(id,1,nullptr);});
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    do {check(sem_cancel(id,0,&canceled)==0);if(canceled)break;std::this_thread::yield();}
    while(std::chrono::steady_clock::now()<deadline);
    if(!canceled)sem_delete(id);
    waiter.join();check(canceled==1&&wait_result==err(85));
    check(sem_cancel(id,5,nullptr)==err(22));
    check(sem_cancel(id,-1,nullptr)==0&&sem_poll(id,2)==0);
    const auto old=id;check(sem_delete(id)==0&&sem_delete(id)==err(3)&&sem_signal(id,1)==err(3));
    check(sem_create(&id,"new",1,0,1,nullptr)==0&&id!=old);
    check(sem_poll(old,1)==err(3));check(sem_delete(id)==0);
    check(sem_create(&id,"delete-waiting",1,0,1,nullptr)==0);
    std::thread deleted_waiter([&]{wait_result=sem_wait(id,1,nullptr);});
    const auto delete_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(semaphore_waiter_count(id).value_or(0)!=1&&std::chrono::steady_clock::now()<delete_deadline)std::this_thread::yield();
    const bool queued=semaphore_waiter_count(id)==1;
    const auto deleted=sem_delete(id);deleted_waiter.join();
    check(queued&&deleted==0&&wait_result==err(13)&&!semaphore_waiter_count(id));
    std::array<std::uint32_t,256> full{};
    for(auto& handle:full)check(sem_create(&handle,"capacity",1,0,1,nullptr)==0);
    check(sem_create(&id,"overflow",1,0,1,nullptr)==err(28));
    for(auto handle:full)check(sem_delete(handle)==0);
    check(sem_create(&id,"priority",2,0,1,nullptr)==err(45));
    check(sem_create(reinterpret_cast<std::uint32_t*>(1),"bad",1,0,1,nullptr)==err(14));
    check(sem_create(&id,reinterpret_cast<const char*>(1),1,0,1,nullptr)==err(14));
    check(sem_create(&id,"options",1,0,1,&id)==err(45));
    for(const auto nid:{"188x57JYp0g","R1Jvn8bSCW8","4czppHBiriw","12wOHk8ywb0","Zxa0VhQVTsk","4DM06U2BNEY"}){
        lbp3::HostImport r{nid,"libkernel","libkernel",1,0x0101,2};check(resolve_semaphore(r).has_value());
        r.type=1;check(!resolve_semaphore(r));r.type=2;r.library="wrong";check(!resolve_semaphore(r));
    }
    std::cout<<"Semaphore tests passed: tokens, blocking workers, timeout, cancellation, stale handles, pointers and resolver. FIFO only; priority scheduling remains unsupported.\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
