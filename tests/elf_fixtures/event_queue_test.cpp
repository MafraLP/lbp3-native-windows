#include "lbp3_host/event_queue.hpp"
#include <Windows.h>
#include <array>
#include <bit>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace lbp3::abi;
constexpr std::int32_t err(unsigned e){return std::bit_cast<std::int32_t>(0x80020000u|e);}
void check(bool value){if(!value)throw std::runtime_error("event queue assertion failed");}
int main(int argc,char**){try{
    using Id=std::uint64_t (LBP3_GUEST_ABI *)(const KernelEvent*);
    const auto getter=reinterpret_cast<Id>(resolve_event_queue({"mJ7aghmgvfc","libkernel","libkernel",1,0x0101,2}).value());
    if(argc>1){(void)getter(reinterpret_cast<const KernelEvent*>(1));return 1;}
    EventQueueHandle id{};check(event_queue_create(&id,"outside")==err(22));
    {ScopedEventQueues scope;
        check(event_queue_create(&id,"events")==0&&id>0);
        KernelEvent output[2]{};int count=-1;const std::uint32_t poll=0,wait=100000;
        check(event_queue_wait(id,output,2,&count,&poll)==err(60)&&count==0);
        check(event_add_user(id,5)==0&&event_trigger_user(id,5,&count)==0);
        check(event_queue_wait(id,output,2,&count,&poll)==0&&count==1&&output[0].ident==5&&output[0].filter==-11&&output[0].userdata==&count&&getter(output)==5);
        check(event_queue_wait(id,output,2,&count,&poll)==0&&count==1); // Level persists.
        check(event_delete_user(id,5)==0&&event_delete_user(id,5)==err(2));
        check(event_add_user_edge(id,5)==0&&event_add_user_edge(id,6)==0);
        check(event_trigger_user(id,5,nullptr)==0&&event_trigger_user(id,6,nullptr)==0);
        check(event_queue_wait(id,reinterpret_cast<KernelEvent*>(1),1,&count,&poll)==err(14));
        check(event_queue_wait(id,output,1,&count,&poll)==0&&count==1&&output[0].ident==5);
        check(event_queue_wait(id,output,2,&count,&poll)==0&&count==1&&output[0].ident==6);
        check(event_queue_wait(id,output,2,&count,&poll)==err(60)&&count==0);
        check(event_add_timer(id,5,0,nullptr)==err(22));
        const auto timer_start=std::chrono::steady_clock::now();
        check(event_add_timer(id,5,1000,&count)==0);
        check(event_queue_wait(id,output,2,&count,&wait)==0&&count==1&&output[0].filter==-7&&output[0].ident==5&&output[0].data>=1&&output[0].userdata==&count);
        check(std::chrono::steady_clock::now()-timer_start>=std::chrono::milliseconds(1));
        check(event_delete_timer(id,5)==0&&event_queue_wait(id,output,2,&count,&poll)==err(60));
        int worker_status{};KernelEvent worker_event{};int worker_count{};
        std::thread worker([&]{worker_status=event_queue_wait(id,&worker_event,1,&worker_count,nullptr);});
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(event_waiter_count(id).value_or(0)!=1&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
        const bool waiting=event_waiter_count(id)==1;
        event_trigger_user(id,5,&worker_count);worker.join();check(waiting&&worker_status==0&&worker_count==1&&worker_event.userdata==&worker_count);
        std::thread deleted([&]{worker_status=event_queue_wait(id,&worker_event,1,&worker_count,nullptr);});
        const auto delete_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(event_waiter_count(id).value_or(0)!=1&&std::chrono::steady_clock::now()<delete_deadline)std::this_thread::yield();
        const bool blocked=event_waiter_count(id)==1;const auto deleted_status=event_queue_delete(id);deleted.join();
        check(blocked&&deleted_status==0&&worker_status==err(9)&&worker_count==0&&event_queue_delete(id)==err(9));
        const auto old=id;check(event_queue_create(&id,"reused")==0&&id!=old);
        check(event_queue_wait(old,output,1,&count,&poll)==err(9));
        check(event_queue_wait(id,output,0,&count,&poll)==err(22));
        check(event_queue_wait(id,output,1,reinterpret_cast<int*>(1),&poll)==err(14));
        check(event_queue_wait(id,output,1,&count,reinterpret_cast<const std::uint32_t*>(1))==err(14));
        check(event_subscribe(id,{10,-14,0,0,0,&count})==0&&event_signal(id,10,-14,99,nullptr)==0);
        check(event_queue_wait(id,output,1,&count,&poll)==0&&output[0].filter==-14&&output[0].data==99&&output[0].userdata==&count);
        check(event_remove(id,10,-14)==0);
        for(unsigned i=0;i<256;++i)check(event_add_user_edge(id,static_cast<int>(i))==0);
        check(event_add_user_edge(id,256)==err(12));
        check(event_add_user_edge(id,0)==0); // Replace an existing subscription at capacity.
        check(event_queue_create(nullptr,"bad")==err(22)&&event_queue_create(reinterpret_cast<EventQueueHandle*>(1),"bad")==err(14));
        std::array<char,32> long_name{};long_name.fill('a');EventQueueHandle ignored{};
        check(event_queue_create(&ignored,long_name.data())==err(63));
    }
    check(!event_waiter_count(id));
    std::cout<<"Event queue tests passed: level/edge user events, timers, batching, nonconsuming pointer errors, blocking wake, deletion during wait, stale IDs, external producers and capacity. File/socket/HRTimer filters remain unsupported.\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
