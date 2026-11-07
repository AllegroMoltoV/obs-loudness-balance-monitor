#include "loopback-probe.h"
#include <Windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <util/platform.h>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <future>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
struct Resources {
    HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    IMMDeviceEnumerator *enumerator=nullptr;
    IMMDevice *device=nullptr;
    IAudioClient *client=nullptr;
    IAudioCaptureClient *capture=nullptr;
    WAVEFORMATEX *format=nullptr;
    bool running=false;
    ~Resources() {
        if(running) client->Stop();
        if(capture) capture->Release();
        if(client) client->Release();
        if(device) device->Release();
        if(enumerator) enumerator->Release();
        if(format) CoTaskMemFree(format);
        if(SUCCEEDED(com)) CoUninitialize();
    }
};
struct NewFile {
    HANDLE file=INVALID_HANDLE_VALUE;
    explicit NewFile(const std::string &path) {
        file=CreateFileW(std::filesystem::u8path(path).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot exclusively create "+path+" error="+std::to_string(GetLastError()));
    }
    ~NewFile(){if(file!=INVALID_HANDLE_VALUE) CloseHandle(file);}
    void write(const void *data,size_t bytes) {
        const char *cursor=static_cast<const char*>(data);
        while(bytes) {DWORD take=DWORD(bytes>1048576?1048576:bytes),written=0;
            if(!WriteFile(file,cursor,take,&written,nullptr)||written!=take) throw std::runtime_error("loopback file write failed");
            cursor+=take; bytes-=take;}
    }
    void text(const std::string &s){write(s.data(),s.size());}
};
}
struct LoopbackProbe::Impl {
    struct Event {uint64_t clock_ns,device_position,qpc_position_100ns,first_float; uint32_t frames,flags; int32_t release_hresult;};
    std::string id;
    Stats result;
    std::vector<float> pcm;
    std::vector<Event> events;
    std::atomic<bool> stopping{false};
    std::promise<bool> initialized;
    std::thread worker;
    HANDLE wake=nullptr;
    bool attempted=false,joined=false;
    Impl(const std::string &endpoint,uint32_t seconds):id(endpoint) {
        if(id.rfind("{0.0.0.",0)!=0 || seconds<1 || seconds>310)
            throw std::invalid_argument("Explicit render endpoint and 1..310 capacity seconds required");
        result.capacity_seconds=seconds;
        wake=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!wake) throw std::runtime_error("loopback wake creation failed");
    }
    ~Impl(){stop();if(wake) CloseHandle(wake);}
    void check(HRESULT hr,const char *operation) {
        result.last_hresult=static_cast<int32_t>(hr);
        if(FAILED(hr)) {std::ostringstream s; s<<operation<<" HRESULT=0x"<<std::hex<<uint32_t(hr); throw std::runtime_error(s.str());}
    }
    void drain(Resources &r, bool final_drain=false) {
        for(;;) {
            if(!final_drain && stopping.load(std::memory_order_acquire)) return;
            UINT32 available=0;
            check(r.capture->GetNextPacketSize(&available),"GetNextPacketSize");
            if(!available) return;
            BYTE *data=nullptr; UINT32 frames=0; DWORD flags=0; UINT64 device=0,qpc=0;
            HRESULT get=r.capture->GetBuffer(&data,&frames,&flags,&device,&qpc);
            const uint64_t received=os_gettime_ns();
            check(get,"GetBuffer");
            if(get==AUDCLNT_S_BUFFER_EMPTY) return;
            ++result.packets; result.captured_frames+=frames;
            if(flags&AUDCLNT_BUFFERFLAGS_SILENT) ++result.silent_packets;
            if(flags&AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++result.discontinuity_packets;
            if(flags&AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) ++result.timestamp_error_packets;
            const bool fits=frames<=result.pcm_capacity_frames-result.stored_frames && result.stored_packets<events.size();
            const bool valid=data || !frames || (flags&AUDCLNT_BUFFERFLAGS_SILENT);
            if(fits && valid) {
                float *dest=pcm.data()+result.stored_frames*2;
                if(flags&AUDCLNT_BUFFERFLAGS_SILENT) std::memset(dest,0,size_t(frames)*8);
                else if(frames) std::memcpy(dest,data,size_t(frames)*8);
                events[size_t(result.stored_packets)]={received,device,qpc,result.stored_frames*2,frames,flags,0};
            } else {result.dropped_frames+=frames; ++result.dropped_packets;}
            HRESULT release=r.capture->ReleaseBuffer(frames);
            if(fits && valid) {
                events[size_t(result.stored_packets)].release_hresult=static_cast<int32_t>(release);
                ++result.stored_packets; result.stored_frames+=frames;
            }
            check(release,"ReleaseBuffer");
            if(!valid) throw std::runtime_error("non-silent loopback packet has null PCM");
        }
    }
    void run() noexcept {
        bool notified=false;
        try {
            Resources r;
            check(r.com,"CoInitializeEx");
            check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(&r.enumerator)),"CoCreateInstance");
            const std::wstring wide(id.begin(),id.end());
            check(r.enumerator->GetDevice(wide.c_str(),&r.device),"GetDevice");
            check(r.device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(&r.client)),"Activate");
            check(r.client->GetMixFormat(&r.format),"GetMixFormat");
            const auto &f=*r.format;
            bool floating=f.wFormatTag==WAVE_FORMAT_IEEE_FLOAT;
            if(f.wFormatTag==WAVE_FORMAT_EXTENSIBLE && f.cbSize>=22) {
                const auto &x=*reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(r.format);
                const GUID ieee={3,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
                floating=IsEqualGUID(x.SubFormat,ieee)&&x.Samples.wValidBitsPerSample==32&&x.dwChannelMask==3;
            }
            if(!floating||f.nChannels!=2||f.wBitsPerSample!=32||f.nBlockAlign!=8||
               (f.nSamplesPerSec!=44100&&f.nSamplesPerSec!=48000))
                throw std::runtime_error("loopback requires stereo float 44100/48000 endpoint mix format");
            result.rate=f.nSamplesPerSec; result.channels=2;
            result.pcm_capacity_frames=uint64_t(result.rate)*result.capacity_seconds;
            // Bounded metadata budget, independent of packet size. Exhaustion is recorded.
            result.event_capacity=uint64_t(result.capacity_seconds)*1000+1024;
            pcm.resize(size_t(result.pcm_capacity_frames)*2);
            events.resize(size_t(result.event_capacity));
            result.allocated_bytes=pcm.size()*sizeof(float)+events.size()*sizeof(Event);
            check(r.client->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_LOOPBACK,1000000,0,r.format,nullptr),"Initialize LOOPBACK");
            check(r.client->GetBufferSize(&result.endpoint_buffer_frames),"GetBufferSize");
            check(r.client->GetService(__uuidof(IAudioCaptureClient),reinterpret_cast<void**>(&r.capture)),"GetService capture");
            check(r.client->Start(),"Start capture"); r.running=true; result.start_clock_ns=os_gettime_ns();
            initialized.set_value(true); notified=true;
            while(!stopping.load(std::memory_order_acquire)) {drain(r); WaitForSingleObject(wake,1);}
            check(r.client->Stop(),"Stop capture"); r.running=false; result.stop_clock_ns=os_gettime_ns();
            drain(r,true); // Stop freezes production; preserve packets already captured.
        } catch(const std::exception &e) {
            result.failure=e.what(); ++result.failures;
            if(!notified) initialized.set_value(false);
        } catch(...) {
            result.failure="unknown loopback capture failure"; ++result.failures;
            if(!notified) initialized.set_value(false);
        }
    }
    void stop() noexcept {stopping.store(true,std::memory_order_release);SetEvent(wake);if(worker.joinable()) worker.join(); joined=true;}
};
LoopbackProbe::LoopbackProbe(const std::string &id,uint32_t seconds):impl_(std::make_unique<Impl>(id,seconds)){}
LoopbackProbe::~LoopbackProbe()=default;
bool LoopbackProbe::start(){auto &p=*impl_;if(p.attempted||p.joined) throw std::logic_error("LoopbackProbe starts once");p.attempted=true;auto future=p.initialized.get_future();p.worker=std::thread([&p]{p.run();});bool ok=future.get();if(!ok)p.stop();return ok;}
void LoopbackProbe::stop() noexcept {impl_->stop();}
const LoopbackProbe::Stats &LoopbackProbe::stats() const noexcept {return impl_->result;}
void LoopbackProbe::save(const std::string &prefix) const {
    const auto &p=*impl_;const auto &s=p.result;
    if(!p.joined) throw std::logic_error("Stop loopback before save");
    NewFile binary(prefix+"-loopback.f32"),csv(prefix+"-loopback.csv"),meta(prefix+"-loopback-meta.txt");
    binary.write(p.pcm.data(),size_t(s.stored_frames)*8);
    csv.text("clock_ns,device_position_frames,qpc_position_100ns,first_float,frames,flags,release_hresult\n");
    for(size_t i=0;i<s.stored_packets;++i){const auto &e=p.events[i];std::ostringstream row;row<<e.clock_ns<<','<<e.device_position<<','<<e.qpc_position_100ns<<','<<e.first_float<<','<<e.frames<<','<<e.flags<<','<<e.release_hresult<<'\n';csv.text(row.str());}
    std::ostringstream m;
    m<<"device="<<p.id<<"\nrate="<<s.rate<<"\nchannels="<<s.channels<<"\ncapacity_seconds="<<s.capacity_seconds
     <<"\npcm_capacity_frames="<<s.pcm_capacity_frames<<"\nevent_capacity="<<s.event_capacity<<"\nallocated_bytes="<<s.allocated_bytes
     <<"\nendpoint_buffer_frames="<<s.endpoint_buffer_frames<<"\npackets="<<s.packets<<"\ncaptured_frames="<<s.captured_frames
     <<"\nstored_frames="<<s.stored_frames<<"\nstored_packets="<<s.stored_packets<<"\ndropped_frames="<<s.dropped_frames<<"\ndropped_packets="<<s.dropped_packets
     <<"\nsilent_packets="<<s.silent_packets<<"\ndiscontinuity_packets="<<s.discontinuity_packets<<"\ntimestamp_error_packets="<<s.timestamp_error_packets
     <<"\nstart_clock_ns="<<s.start_clock_ns<<"\nstop_clock_ns="<<s.stop_clock_ns<<"\nfailures="<<s.failures<<"\nlast_hresult="<<s.last_hresult
     <<"\nfailure="<<s.failure<<"\npcm=float32-le-interleaved\nclock=os_gettime_ns_after_GetBuffer\nqpc_position_unit=100ns\n";
    meta.text(m.str());
}


std::string default_render_device_id()
{
    return std::async(std::launch::async, [] {
        Resources r;
        if (FAILED(r.com) || FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&r.enumerator))) ||
            FAILED(r.enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &r.device)))
            throw std::runtime_error("Cannot resolve default render endpoint");
        LPWSTR raw = nullptr;
        if (FAILED(r.device->GetId(&raw))) throw std::runtime_error("Cannot read default render endpoint ID");
        std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(raw, CoTaskMemFree);
        const std::wstring wide(raw);
        std::string id;
        for (const wchar_t ch : wide) {
            if (ch > 127) throw std::runtime_error("Non-ASCII endpoint ID");
            id.push_back(static_cast<char>(ch));
        }
        return id;
    }).get();
}