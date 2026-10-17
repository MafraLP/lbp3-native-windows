#include "lbp3_host/thread_services.hpp"
#include "lbp3_host/module_registry.hpp"
#include <Windows.h>
#include <array>
#include <bit>
#include <condition_variable>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <algorithm>
#include <cstdio>

namespace lbp3::abi {
namespace {
struct Key {bool live{};std::uint64_t generation{};KeyDestructor destructor{};};
struct Value {std::uint64_t generation{};const void* pointer{};};
struct OnceRun {std::thread::id owner;bool finished{};std::condition_variable changed;};
std::mutex services_mutex;
std::uint64_t next_context{};
ThreadServiceState* active{};
thread_local std::uint64_t local_context{};
thread_local std::array<Value,256> local_values{};
constexpr std::int32_t error(unsigned e){return std::bit_cast<std::int32_t>(0x80020000u|e);}
template<class T> bool read(const T* source,T& target){SIZE_T n{};
    return source && ReadProcessMemory(GetCurrentProcess(),source,&target,sizeof(T),&n) && n==sizeof(T);}
bool writable(const void* pointer,std::size_t size){
    auto at=reinterpret_cast<std::uintptr_t>(pointer);
    if(!at || at>UINTPTR_MAX-size)return false;
    const auto end=at+size;
    while(at<end){MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info)) || info.State!=MEM_COMMIT ||
           info.Protect&(PAGE_GUARD|PAGE_NOACCESS))return false;
        const auto p=info.Protect&255;
        if(p!=PAGE_READWRITE && p!=PAGE_WRITECOPY && p!=PAGE_EXECUTE_READWRITE && p!=PAGE_EXECUTE_WRITECOPY)return false;
        const auto base=reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if(info.RegionSize>UINTPTR_MAX-base || base+info.RegionSize<=at)return false;
        at=base+info.RegionSize;
    }return true;
}
template<class T> bool write(T* target,const T& value){SIZE_T n{};
    return writable(target,sizeof(T)) && WriteProcessMemory(GetCurrentProcess(),target,&value,sizeof(T),&n) && n==sizeof(T);}
template<class T> bool executable(T fn){MEMORY_BASIC_INFORMATION info{};
    if(!fn || !VirtualQuery(reinterpret_cast<void*>(fn),&info,sizeof(info)) || info.State!=MEM_COMMIT ||
       info.Protect&(PAGE_GUARD|PAGE_NOACCESS))return false;
    const auto p=info.Protect&255;
    return p==PAGE_EXECUTE || p==PAGE_EXECUTE_READ || p==PAGE_EXECUTE_READWRITE || p==PAGE_EXECUTE_WRITECOPY;
}
}
struct ThreadServiceState {
    std::uint64_t id{};std::array<Key,256> keys{};
    std::map<GuestOnce*,std::shared_ptr<OnceRun>> running;
    std::vector<std::array<ForkCallback,3>> atfork;
};
std::int32_t register_atfork(ForkCallback prepare,ForkCallback parent,ForkCallback child) noexcept {
    try{std::lock_guard lock(services_mutex);if(!active)return 22;
        for(const auto callback:{prepare,parent,child})if(callback&&!executable(callback))return 14;
        if(active->atfork.size()>=256)return 12;
        active->atfork.push_back({prepare,parent,child});return 0;
    }catch(...){return 12;}
}
std::size_t atfork_handler_count(){std::lock_guard lock(services_mutex);return active?active->atfork.size():0;}
void LBP3_GUEST_ABI pthread_cxa_finalize(const ModuleInfoEx* input) noexcept {
    try{std::lock_guard lock(services_mutex);
        if(!active)throw std::runtime_error("No thread services scope for cxa finalization.");
        // There is nothing to unregister when no atfork handler was installed.
        if(active->atfork.empty())return;
        ModuleInfoEx module{};
        if(!read(input,module)||!module.segment_count||module.segment_count>4||
           !module_matches_address(input,module.segments[0].address))
            throw std::runtime_error("Unknown module in pthread cxa finalization.");
        std::erase_if(active->atfork,[&](const auto& callbacks){
            for(const auto callback:callbacks)if(callback&&module_matches_address(input,reinterpret_cast<std::uintptr_t>(callback)))return true;
            return false;
        });
    }catch(...){
        std::fputs("Guest diagnostic stop: invalid pthread cxa finalization context.\n",stderr);
        TerminateProcess(GetCurrentProcess(),0xe04c4204u);
        std::terminate();
    }
}
namespace {
void attach_local(){if(local_context!=active->id){local_values={};local_context=active->id;}}
bool valid_key(std::int32_t key){return active && key>=0 && key<256 && active->keys[key].live;}
}
ScopedThreadServices::ScopedThreadServices():state_(std::make_unique<ThreadServiceState>()){
    std::lock_guard lock(services_mutex);
    if(active || next_context==UINT64_MAX)throw std::runtime_error("Thread services context unavailable.");
    state_->id=++next_context;active=state_.get();
}
ScopedThreadServices::~ScopedThreadServices(){
    cleanup_thread_specific();
    std::lock_guard lock(services_mutex);active=nullptr;
}
std::int32_t LBP3_GUEST_ABI pthread_once(GuestOnce* control,OnceRoutine routine) noexcept {
    try{
        std::unique_lock lock(services_mutex);
        if(!active || !control || reinterpret_cast<std::uintptr_t>(control)%alignof(GuestOnce) || !routine)return error(22);
        if(!executable(routine))return error(14);
        for(;;){GuestOnce value{};
            if(!read(control,value)||!writable(control,sizeof(GuestOnce)))return error(14);
            if(value.state==1)return 0;
            const auto found=active->running.find(control);
            if(found!=active->running.end()){
                auto run=found->second;
                if(run->owner==std::this_thread::get_id())return error(11);
                run->changed.wait(lock,[&]{return run->finished;});continue;
            }
            if(value.state!=0)return error(22);
            if(active->running.size()>=1024)return error(35);
            auto run=std::make_shared<OnceRun>();run->owner=std::this_thread::get_id();
            active->running.emplace(control,run);
            if(!write(&control->state,std::uint32_t{2})){active->running.erase(control);return error(14);}
            lock.unlock();routine();lock.lock();
            const bool completed=write(&control->state,std::uint32_t{1});
            run->finished=true;active->running.erase(control);run->changed.notify_all();
            return completed ? 0 : error(14);
        }
    }catch(const std::bad_alloc&){return error(12);}catch(...){return error(5);}
}
std::int32_t LBP3_GUEST_ABI key_create(std::int32_t* output,KeyDestructor destructor) noexcept {
    try{std::lock_guard lock(services_mutex);
        if(!active || !output)return error(22);
        if(!writable(output,sizeof(*output)))return error(14);
        if(destructor && !executable(destructor))return error(14);
        for(std::int32_t i=0;i<256;++i){auto& key=active->keys[i];
            if(!key.live && key.generation!=UINT64_MAX){
                if(!write(output,i))return error(14);
                ++key.generation;key.live=true;key.destructor=destructor;return 0;
            }}return error(35);
    }catch(...){return error(5);}
}
std::int32_t LBP3_GUEST_ABI key_delete(std::int32_t index) noexcept {
    try{std::lock_guard lock(services_mutex);if(!valid_key(index))return error(22);
        active->keys[index].live=false;active->keys[index].destructor=nullptr;return 0;
    }catch(...){return error(5);}
}
std::int32_t LBP3_GUEST_ABI set_specific(std::int32_t index,const void* value) noexcept {
    try{std::lock_guard lock(services_mutex);if(!valid_key(index))return error(22);
        attach_local();local_values[index]={active->keys[index].generation,value};return 0;
    }catch(...){return error(5);}
}
const void* LBP3_GUEST_ABI get_specific(std::int32_t index) noexcept {
    try{std::lock_guard lock(services_mutex);if(!valid_key(index))return nullptr;
        attach_local();return local_values[index].generation==active->keys[index].generation ? local_values[index].pointer : nullptr;
    }catch(...){return nullptr;}
}
void cleanup_thread_specific() noexcept {
    try{
        std::unique_lock lock(services_mutex);
        if(!active || local_context!=active->id)return;
        for(unsigned pass=0;pass<4;++pass){bool invoked=false;
            for(unsigned i=0;i<256;++i){const auto value=local_values[i];local_values[i]={};
                const auto key=active->keys[i];
                if(value.pointer && key.live && value.generation==key.generation && key.destructor){
                    invoked=true;lock.unlock();key.destructor(value.pointer);lock.lock();
                }}
            if(!invoked)break;
        }local_values={};local_context=0;
    }catch(...){std::terminate();} // Never silently skip guest destructors after an internal failure.
}
std::optional<std::uintptr_t> resolve_thread_services(const HostImport& request){
    if(request.type!=2)return std::nullopt;
    if(request.nid=="kbw4UHHSYy0")return reinterpret_cast<std::uintptr_t>(&pthread_cxa_finalize);
    if(request.nid=="14bOACANTBo")return reinterpret_cast<std::uintptr_t>(&pthread_once);
    if(request.nid=="geDaqgH9lTg")return reinterpret_cast<std::uintptr_t>(&key_create);
    if(request.nid=="PrdHuuDekhY")return reinterpret_cast<std::uintptr_t>(&key_delete);
    if(request.nid=="+BzXYkqYeLE")return reinterpret_cast<std::uintptr_t>(&set_specific);
    if(request.nid=="eoht7mQOCmo")return reinterpret_cast<std::uintptr_t>(&get_specific);
    return std::nullopt;
}
}
