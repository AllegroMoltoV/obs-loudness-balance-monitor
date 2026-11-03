#include "recording.h"
#include <array>
#include <iostream>
static void check(bool ok,const char *what) { if(!ok) throw std::runtime_error(what); }
static float sample(uint64_t i,bool right) { return float((i%65521)+1)*(right?-1.0f:1.0f)/8388608.0f; }
static void core(uint32_t rate) {
 const uint32_t total=rate*5;
 Recording r(rate,total); check(r.begin(),"begin");
 std::array<float,1024> l{},rr{};
 for(uint32_t pos=0;pos<total;pos+=1024) {
  for(uint32_t i=0;i<1024;++i) { l[i]=sample(pos+i,false); rr[i]=sample(pos+i,true); }
  r.append(l.data(),rr.data(),1024,1000000000ULL+uint64_t(pos)*1000000000ULL/rate);
 }
 check(r.done()&&r.freeze()&&r.frames()==total,"exact capture size");
 for(uint32_t i=0;i<total;++i) check(r.left()[i]==sample(i,false)&&r.right()[i]==sample(i,true),"PCM boundary or channel mismatch");
 const auto saved=r.left();
 for(int trial=0;trial<3;++trial) {
  check(r.play(),"replay"); check(!r.begin(),"record/play exclusion");
  r.append(l.data(),rr.data(),1024,0); check(r.left()==saved,"frozen PCM changed"); r.stop();
 }
 check(r.begin(),"rerecord"); r.append(l.data(),rr.data(),1024,1000000000ULL);
 r.append(l.data(),rr.data(),1024,1000000000ULL); check(r.done()&&!r.freeze(),"duplicate timestamp accepted");
 check(r.begin(),"restart after error"); r.append(l.data(),rr.data(),1024,1000000000ULL);
 r.append(l.data(),rr.data(),1024,2000000000ULL); check(r.done()&&!r.freeze(),"gap accepted");
 check(r.begin(),"timeout restart"); check(!r.freeze(),"empty timeout accepted");
 check(r.begin(),"cancel restart"); r.cancel(); check(!r.play()&&r.frames()==0,"cancelled playback");
 Recording silent(rate,rate); check(silent.begin(),"silent begin"); l.fill(0);rr.fill(0);
 for(uint32_t pos=0;pos<rate;pos+=1024) silent.append(l.data(),rr.data(),1024,1000000000ULL+uint64_t(pos)*1000000000ULL/rate);
 check(silent.freeze(),"valid silence rejected");
 Recording manual(rate,rate*30);check(manual.begin(),"manual begin");
 manual.append(l.data(),rr.data(),1024,1000000000ULL);check(!manual.done(),"premature automatic limit");
 check(manual.freeze(true)&&manual.frames()==1024,"manual end length");check(manual.play(),"manual replay");manual.stop();
 check(manual.begin(),"empty begin");check(!manual.freeze(true),"empty recording accepted");
 check(manual.begin(),"error begin");manual.append(nullptr,nullptr,1,0);check(!manual.freeze(true),"manual finish hid input failure");
 Recording cap(rate,1050);check(cap.begin(),"cap begin");cap.append(l.data(),rr.data(),1024,1000000000ULL);cap.append(l.data(),rr.data(),1024,1000000000ULL+1024ULL*1000000000ULL/rate);check(cap.done()&&cap.freeze()&&cap.frames()==1050,"cap crossing");
 std::cout<<"PASS manual_limit rate="<<rate<<" manual_frames=1024 automatic_limit_frames=1050 empty_and_error_rejected=1\n";
 std::cout<<"PASS core rate="<<rate<<" frames="<<total<<" bytes="<<total*8<<" exact_pcm replay=3 gap_duplicate_timeout_cancel_silence=pass\n";
}

int main(){try{core(44100);core(48000);}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}return 0;}
