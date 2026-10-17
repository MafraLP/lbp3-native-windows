#include "lbp3_host/event_queue.hpp"
#include <Windows.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <cstdio>
#include <stdexcept>
namespace lbp3::abi {
namespace {
using Clock=std::chrono::steady_clock;
using Key=std::pair<std::uint64_t,std::int16_t>;
constexpr std::int32_t error(unsigned code){return std::bit_cast<std::int32_t>(0x80020000u|code);}
struct Registration {KernelEvent event;bool ready{};std::chrono::microseconds period{};Clock::time_point next{};};
struct Queue {std::array<char,32> name{};bool deleted{};std::size_t waiters{};std::map<Key,Registration> events;std::condition_variable changed;};
std::mutex registry;EventQueueState* active{};std::uint64_t next_id{1};
bool wait_trace_enabled() noexcept {static const bool enabled=[] {wchar_t value[8]{};return GetEnvironmentVariableW(L"LBP3_EQUEUE_TRACE",value,8)!=0;}();return enabled;}
void trace_wait(const char* phase,EventQueueHandle id,int capacity,bool has_timeout,std::uint32_t timeout,std::size_t subscriptions,int result,int count) noexcept {
    if(!wait_trace_enabled())return;
    static std::atomic_uint sequence{};
    const auto n=sequence.fetch_add(1,std::memory_order_relaxed);
    if(n>=256)return;
    std::fprintf(stderr,"[LBP3_EQUEUE_TRACE] #%u %s queue=0x%llx capacity=%d timeout=%s%u subscriptions=%zu result=0x%08x events=%d\n",
        n,phase,static_cast<unsigned long long>(id),capacity,has_timeout?"":"infinite/",timeout,subscriptions,static_cast<unsigned>(result),count);
    std::fflush(stderr);
}
bool writable(const void* p,std::size_t length){
    auto at=reinterpret_cast<std::uintptr_t>(p);if(!at||length>UINTPTR_MAX-at)return false;
    const auto end=at+length;
    while(at<end){MEMORY_BASIC_INFORMATION info{};if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info))||info.State!=MEM_COMMIT||info.Protect&(PAGE_GUARD|PAGE_NOACCESS))return false;
        const auto prot=info.Protect&255;if(prot!=PAGE_READWRITE&&prot!=PAGE_WRITECOPY&&prot!=PAGE_EXECUTE_READWRITE&&prot!=PAGE_EXECUTE_WRITECOPY)return false;
        const auto base=reinterpret_cast<std::uintptr_t>(info.BaseAddress);if(info.RegionSize>UINTPTR_MAX-base||base+info.RegionSize<=at)return false;at=base+info.RegionSize;
    }return true;
}
bool write(void* p,const void* value,std::size_t length){SIZE_T n{};return writable(p,length)&&WriteProcessMemory(GetCurrentProcess(),p,value,length,&n)&&n==length;}
template<class T> bool read(const T* p,T& value){SIZE_T n{};return p&&ReadProcessMemory(GetCurrentProcess(),p,&value,sizeof(value),&n)&&n==sizeof(value);}
KernelEvent read_event(const KernelEvent* input){KernelEvent event{};
    if(!read(input,event)){std::fputs("Guest diagnostic stop: inaccessible kernel event accessor.\n",stderr);for(;;)TerminateProcess(GetCurrentProcess(),0xe04c4206u);}
    return event;
}
std::uint64_t LBP3_GUEST_ABI event_id(const KernelEvent* input) noexcept {return read_event(input).ident;}
std::int32_t LBP3_GUEST_ABI event_filter(const KernelEvent* input) noexcept {return read_event(input).filter;}
std::uint64_t LBP3_GUEST_ABI event_data(const KernelEvent* input) noexcept {return read_event(input).data;}
void* LBP3_GUEST_ABI event_userdata(const KernelEvent* input) noexcept {return read_event(input).userdata;}
std::uint32_t LBP3_GUEST_ABI event_fflags(const KernelEvent* input) noexcept {return read_event(input).fflags;}
}
struct EventQueueState {std::map<EventQueueHandle,std::shared_ptr<Queue>> queues;};
namespace {
std::shared_ptr<Queue> lookup(EventQueueHandle id){if(!active)return {};const auto it=active->queues.find(id);return it==active->queues.end()?nullptr:it->second;}
std::int32_t subscribe(EventQueueHandle id,Registration registration){
    auto queue=lookup(id);if(!queue)return error(9);
    const Key key{registration.event.ident,registration.event.filter};
    if(queue->events.size()>=256&&!queue->events.contains(key))return error(12);
    queue->events.insert_or_assign(key,registration);queue->changed.notify_all();return 0;
}
}
ScopedEventQueues::ScopedEventQueues():state_(std::make_unique<EventQueueState>()){
    std::lock_guard lock(registry);if(active)throw std::runtime_error("Event queue scope already active.");active=state_.get();
}
ScopedEventQueues::~ScopedEventQueues(){std::lock_guard lock(registry);
    for(auto& [id,queue]:state_->queues){(void)id;queue->deleted=true;queue->changed.notify_all();}active=nullptr;
}
std::int32_t LBP3_GUEST_ABI event_queue_create(EventQueueHandle* output,const char* name) noexcept {
    try{std::lock_guard lock(registry);if(!active||!output||!name)return error(22);
        if(!writable(output,8))return error(14);if(active->queues.size()>=64||next_id>0xffffffffull)return error(24);
        auto queue=std::make_shared<Queue>();bool terminated=false;
        for(unsigned i=0;i<32;++i){if(!read(name+i,queue->name[i]))return error(14);if(!queue->name[i]){terminated=true;break;}}
        if(!terminated)return error(63);
        const auto id=static_cast<EventQueueHandle>(0x4400000000000000ull|next_id++);active->queues.emplace(id,queue);
        if(!write(output,&id,8)){active->queues.erase(id);return error(14);}return 0;
    }catch(const std::bad_alloc&){return error(12);}catch(...){return error(5);}
}
std::int32_t LBP3_GUEST_ABI event_queue_delete(EventQueueHandle id) noexcept {
    try{std::lock_guard lock(registry);auto queue=lookup(id);if(!queue)return error(9);
        queue->deleted=true;queue->changed.notify_all();active->queues.erase(id);return 0;
    }catch(...){return error(5);}
}
std::int32_t LBP3_GUEST_ABI event_queue_wait(EventQueueHandle id,KernelEvent* output,int capacity,int* number,const std::uint32_t* timeout) noexcept {
    try{std::unique_lock lock(registry);auto queue=lookup(id);if(!queue)return error(9);
        if(capacity<1||capacity>128)return error(22);
        const auto bytes=static_cast<std::size_t>(capacity)*sizeof(KernelEvent);
        if(!writable(output,bytes)||!writable(number,4))return error(14);
        const auto first=reinterpret_cast<std::uintptr_t>(output),last=first+bytes,count_address=reinterpret_cast<std::uintptr_t>(number);
        if(count_address<last&&first<count_address+4)return error(22);
        std::uint32_t usec{};if(timeout&&!read(timeout,usec))return error(14);
        const int zero{};if(!write(number,&zero,4))return error(14);
        const auto deadline=timeout?Clock::now()+std::chrono::microseconds(usec):Clock::time_point::max();
        trace_wait("enter",id,capacity,timeout!=nullptr,usec,queue->events.size(),0,0);
        ++queue->waiters;struct Waiting {Queue& queue;~Waiting(){--queue.waiters;}} waiting{*queue};
        for(;;){
            if(queue->deleted){const auto result=error(9);trace_wait("deleted",id,capacity,timeout!=nullptr,usec,queue->events.size(),result,0);return result;}
            const auto now=Clock::now();auto wake=deadline;
            std::array<KernelEvent,128> events{};std::array<Key,128> keys{};int count{};
            for(auto& [key,item]:queue->events){
                if(item.period.count()>0){
                    if(now>=item.next){const auto lag=std::chrono::duration_cast<std::chrono::microseconds>(now-item.next);
                        const auto expirations=static_cast<std::uint64_t>(lag.count()/item.period.count()+1);
                        item.event.data=expirations>UINT64_MAX-item.event.data?UINT64_MAX:item.event.data+expirations;
                        item.ready=true;item.next=now+item.period-std::chrono::microseconds(lag.count()%item.period.count());
                    }wake=std::min(wake,item.next);
                }
                if(item.ready&&count<capacity){events[count]=item.event;keys[count]=key;++count;}
            }
            if(count){
                if(!write(output,events.data(),static_cast<std::size_t>(count)*sizeof(KernelEvent))||!write(number,&count,4))return error(14);
                for(int i=0;i<count;++i){auto it=queue->events.find(keys[i]);
                    if(it->second.event.flags&0x10)queue->events.erase(it);
                    else if(it->second.event.flags&0x20){it->second.ready=false;it->second.event.data=0;it->second.event.fflags=0;}
                }trace_wait("ready",id,capacity,timeout!=nullptr,usec,queue->events.size(),0,count);return 0;
            }
            if(timeout&&now>=deadline){const auto result=error(60);trace_wait("timeout",id,capacity,true,usec,queue->events.size(),result,0);return result;}
            if(wake==Clock::time_point::max())queue->changed.wait(lock);else queue->changed.wait_until(lock,wake);
        }
    }catch(const std::bad_alloc&){return error(12);}catch(...){return error(5);}
}
std::int32_t event_subscribe(EventQueueHandle id,const KernelEvent& event) noexcept {
    try{std::lock_guard lock(registry);if(event.filter!=-11&&event.filter!=-13&&event.filter!=-14)return error(22);
        if(event.flags&~0x31)return error(45);
        Registration item{event};item.event.flags&=~std::uint16_t{1};if(event.filter!=-11)item.event.flags|=0x20;
        return subscribe(id,item);
    }catch(...){return error(12);}
}
std::int32_t event_remove(EventQueueHandle id,std::uint64_t ident,std::int16_t filter) noexcept {
    try{std::lock_guard lock(registry);auto queue=lookup(id);if(!queue)return error(9);
        return queue->events.erase({ident,filter})?0:error(2);
    }catch(...){return error(5);}
}
std::int32_t event_signal(EventQueueHandle id,std::uint64_t ident,std::int16_t filter,std::uint64_t data,void* userdata) noexcept {
    try{std::lock_guard lock(registry);auto queue=lookup(id);if(!queue)return error(9);
        const auto found=queue->events.find({ident,filter});if(found==queue->events.end())return error(2);
        auto& item=found->second;item.ready=true;if(filter==-11)item.event.userdata=userdata;else item.event.data=data;
        queue->changed.notify_all();return 0;
    }catch(...){return error(5);}
}
std::int32_t LBP3_GUEST_ABI event_add_user(EventQueueHandle id,int ident) noexcept {return event_subscribe(id,{static_cast<std::uint64_t>(ident),-11,0,0,0,nullptr});}
std::int32_t LBP3_GUEST_ABI event_add_user_edge(EventQueueHandle id,int ident) noexcept {return event_subscribe(id,{static_cast<std::uint64_t>(ident),-11,0x20,0,0,nullptr});}
std::int32_t LBP3_GUEST_ABI event_trigger_user(EventQueueHandle id,int ident,void* userdata) noexcept {return event_signal(id,static_cast<std::uint64_t>(ident),-11,0,userdata);}
std::int32_t LBP3_GUEST_ABI event_delete_user(EventQueueHandle id,int ident) noexcept {return event_remove(id,static_cast<std::uint64_t>(ident),-11);}
std::int32_t LBP3_GUEST_ABI event_delete_timer(EventQueueHandle id,int ident) noexcept {return event_remove(id,static_cast<std::uint64_t>(ident),-7);}
std::int32_t LBP3_GUEST_ABI event_add_timer(EventQueueHandle id,int ident,std::uint32_t usec,void* userdata) noexcept {
    if(!usec)return error(22);
    try{std::lock_guard lock(registry);Registration item{{static_cast<std::uint64_t>(ident),-7,0x20,0,0,userdata}};
        item.period=std::chrono::microseconds(usec);item.next=Clock::now()+item.period;return subscribe(id,item);
    }catch(...){return error(12);}
}
std::optional<std::size_t> event_waiter_count(EventQueueHandle id){std::lock_guard lock(registry);const auto queue=lookup(id);return queue?std::optional(queue->waiters):std::nullopt;}
std::optional<std::uintptr_t> resolve_event_queue(const HostImport& r){
    if(r.type!=2||r.library!="libkernel"||r.module!="libkernel"||r.library_version!=1||r.module_version!=0x0101)return std::nullopt;
    if(r.nid=="D0OdFMjp46I")return reinterpret_cast<std::uintptr_t>(&event_queue_create);
    if(r.nid=="jpFjmgAC5AE")return reinterpret_cast<std::uintptr_t>(&event_queue_delete);
    if(r.nid=="fzyMKs9kim0")return reinterpret_cast<std::uintptr_t>(&event_queue_wait);
    if(r.nid=="4R6-OvI2cEA")return reinterpret_cast<std::uintptr_t>(&event_add_user);
    if(r.nid=="WDszmSbWuDk")return reinterpret_cast<std::uintptr_t>(&event_add_user_edge);
    if(r.nid=="F6e0kwo4cnk")return reinterpret_cast<std::uintptr_t>(&event_trigger_user);
    if(r.nid=="LJDwdSNTnDg")return reinterpret_cast<std::uintptr_t>(&event_delete_user);
    if(r.nid=="57ZK+ODEXWY")return reinterpret_cast<std::uintptr_t>(&event_add_timer);
    if(r.nid=="YWQFUyXIVdU")return reinterpret_cast<std::uintptr_t>(&event_delete_timer);
    if(r.nid=="mJ7aghmgvfc")return reinterpret_cast<std::uintptr_t>(&event_id);
    if(r.nid=="23CPPI1tyBY")return reinterpret_cast<std::uintptr_t>(&event_filter);
    if(r.nid=="kwGyyjohI50")return reinterpret_cast<std::uintptr_t>(&event_data);
    if(r.nid=="vz+pg2zdopI")return reinterpret_cast<std::uintptr_t>(&event_userdata);
    if(r.nid=="Q0qr9AyqJSk")return reinterpret_cast<std::uintptr_t>(&event_fflags);
    return std::nullopt;
}
}
