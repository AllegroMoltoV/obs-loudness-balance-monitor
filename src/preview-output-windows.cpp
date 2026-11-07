#ifdef _WIN32
#include "preview-output.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <media-io/audio-resampler.h>
#include <util/platform.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <vector>
namespace lbm {
namespace {
static void require(bool ok,const char *s){if(!ok)throw std::runtime_error(s);}
static void hrcheck(HRESULT hr,const char *s){if(FAILED(hr)){std::ostringstream o;o<<s<<" HRESULT=0x"<<std::hex<<uint32_t(hr);throw std::runtime_error(o.str());}}
// WASAPI ownership, format validation and cleanup follow the existing FinalPlayer.
struct Resources {
 IMMDeviceEnumerator *enumerator=nullptr; IMMDevice *device=nullptr;
 IAudioClient *client=nullptr;IAudioRenderClient *render=nullptr;IAudioClock *clock=nullptr;WAVEFORMATEX *format=nullptr;
 bool playing=false;
 ~Resources(){if(playing)client->Stop();if(client)client->Reset();if(clock)clock->Release();if(render)render->Release();if(client)client->Release();if(device)device->Release();if(enumerator)enumerator->Release();if(format)CoTaskMemFree(format);}
};
static std::vector<float> convert(uint32_t from_rate,uint32_t to_rate,const std::vector<float>* captured_left=nullptr,const std::vector<float>* captured_right=nullptr){
 std::vector<float> pcm;
 require(captured_left && captured_right && !captured_left->empty() && captured_left->size()==captured_right->size(), "captured size mismatch");
 const auto& left=*captured_left;
 const auto& right=*captured_right;
 resample_info from{from_rate,AUDIO_FORMAT_FLOAT_PLANAR,SPEAKERS_STEREO},to{to_rate,AUDIO_FORMAT_FLOAT,SPEAKERS_STEREO};
 auto rs=audio_resampler_create(&to,&from);require(rs!=nullptr,"resampler init");
 try{
  uint8_t *out[MAX_AV_PLANES]{};uint32_t n=0;uint64_t offset=0;
  const uint8_t *in[MAX_AV_PLANES]={reinterpret_cast<const uint8_t*>(left.data()),reinterpret_cast<const uint8_t*>(right.data())};
  require(audio_resampler_resample(rs,out,&n,&offset,in,uint32_t(left.size())),"convert");
  pcm.insert(pcm.end(),reinterpret_cast<float*>(out[0]),reinterpret_cast<float*>(out[0])+n*2);
  for(unsigned flush=0;;++flush){require(flush<16,"flush did not terminate");require(audio_resampler_resample(rs,out,&n,&offset,nullptr,0),"flush");if(!n)break;pcm.insert(pcm.end(),reinterpret_cast<float*>(out[0]),reinterpret_cast<float*>(out[0])+n*2);}
  const uint64_t scaled=uint64_t(left.size())*to_rate;
  require(pcm.size()/2>=scaled/from_rate && pcm.size()/2<=(scaled+from_rate-1)/from_rate,"finite conversion length mismatch");
  if(from_rate==to_rate)for(size_t i=0;i<left.size();++i)require(pcm[i*2]==left[i]&&pcm[i*2+1]==right[i],"same-rate PCM mismatch");

 }catch(...){audio_resampler_destroy(rs);throw;}
 audio_resampler_destroy(rs);return pcm;
}
struct Com {
 Com(){hrcheck(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"CoInitializeEx");}
 ~Com(){CoUninitialize();}
};
} // namespace
bool play_preview_on_device(const std::vector<float>& input,uint32_t input_rate,const std::string& id,const std::atomic<bool>& cancel,std::string&,std::atomic<double>* position){
 if(position)position->store(-1.0);
 Com com;
 Resources r;hrcheck(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(&r.enumerator)),"CoCreateInstance");
 if(id=="default"){
  hrcheck(r.enumerator->GetDefaultAudioEndpoint(eRender,eMultimedia,&r.device),"GetDefaultAudioEndpoint");
 }else{
  std::wstring wide(id.begin(),id.end());hrcheck(r.enumerator->GetDevice(wide.c_str(),&r.device),"GetDevice");
 }
 hrcheck(r.device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(&r.client)),"Activate");
 hrcheck(r.client->GetMixFormat(&r.format),"GetMixFormat");
 const auto&f=*r.format;bool floating=f.wFormatTag==WAVE_FORMAT_IEEE_FLOAT;
 if(f.wFormatTag==WAVE_FORMAT_EXTENSIBLE&&f.cbSize>=22){const auto&x=*reinterpret_cast<WAVEFORMATEXTENSIBLE*>(r.format);const GUID g={3,0,0x0010,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};floating=IsEqualGUID(x.SubFormat,g)&&x.Samples.wValidBitsPerSample==32;require(x.dwChannelMask==3,"channel mask");}
 require(floating&&f.nChannels==2&&f.wBitsPerSample==32&&f.nBlockAlign==8&&(f.nSamplesPerSec==44100||f.nSamplesPerSec==48000),"unsupported endpoint format");
 std::vector<float> converted;
 if(input_rate!=f.nSamplesPerSec){
  std::vector<float> left(input.size()/2),right(input.size()/2);
  for(size_t i=0;i<left.size();++i){left[i]=input[i*2];right[i]=input[i*2+1];}
  converted=convert(input_rate,f.nSamplesPerSec,&left,&right);
 }
 const auto& pcm=converted.empty()?input:converted;
 if(cancel.load())return false;
 hrcheck(r.client->Initialize(AUDCLNT_SHAREMODE_SHARED,0,1000000,0,r.format,nullptr),"Initialize");
 UINT32 capacity=0;hrcheck(r.client->GetBufferSize(&capacity),"GetBufferSize");
 hrcheck(r.client->GetService(__uuidof(IAudioRenderClient),reinterpret_cast<void**>(&r.render)),"GetService");
 UINT64 frequency=0;
 if(position && SUCCEEDED(r.client->GetService(__uuidof(IAudioClock),reinterpret_cast<void**>(&r.clock)))) {
  if(FAILED(r.clock->GetFrequency(&frequency)))frequency=0;
 }
 const double duration=double(input.size()/2)/input_rate;
 double last_position=0.0;
 size_t cursor=0;uint64_t drain_deadline=0;UINT32 final_padding=0;
 const uint64_t deadline=os_gettime_ns()+uint64_t(pcm.size()/2)*1000000000ULL/f.nSamplesPerSec+10000000000ULL;
 while(true){
  UINT32 padding=0;hrcheck(r.client->GetCurrentPadding(&padding),"GetCurrentPadding");require(padding<=capacity,"invalid padding");
  const uint64_t now=os_gettime_ns();
  if(cancel.load())break;
  if(position && r.playing && r.clock && frequency){
   UINT64 ticks=0;
   const HRESULT status=r.clock->GetPosition(&ticks,nullptr);
   if(status==S_OK){last_position=std::clamp(double(ticks)/double(frequency),last_position,duration);position->store(last_position);}
   else if(FAILED(status))position->store(-1.0);
  }
  require(now<deadline,"playback timeout");
  if(cursor==pcm.size()/2){
   if(!drain_deadline)drain_deadline=now+2000000000ULL;
   if(!padding)break;
   require(now<drain_deadline,"drain timeout");Sleep(1);continue;
  }
  require(!r.playing||padding!=0,"finite playback starvation");
  const auto take=uint32_t(std::min(pcm.size()/2-cursor,size_t(capacity-padding)));
  if(!take){Sleep(1);continue;}
  BYTE *buffer=nullptr;hrcheck(r.render->GetBuffer(take,&buffer),"GetBuffer");std::memcpy(buffer,pcm.data()+cursor*2,size_t(take)*8);hrcheck(r.render->ReleaseBuffer(take,0),"ReleaseBuffer");cursor+=take;
  if(!r.playing){hrcheck(r.client->Start(),"Start");r.playing=true;}
 }
 hrcheck(r.client->Stop(),"Stop");r.playing=false;hrcheck(r.client->GetCurrentPadding(&final_padding),"stop padding");hrcheck(r.client->Reset(),"Reset");UINT32 reset_padding=1;hrcheck(r.client->GetCurrentPadding(&reset_padding),"reset padding");require(reset_padding==0,"reset retained frames");
 const bool completed=!cancel.load();
 if(completed && position)position->store(duration);
 return completed;
}
} // namespace lbm
#endif
