// Offline reference uses only libebur128 primitives, never product analysis/VAD.
#include <ebur128.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <memory>
#include <vector>
#include <stdexcept>
std::vector<float> read(const char *path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("PCM open failed");
    auto bytes=f.tellg();
    if (bytes <= 0 || bytes % 8) throw std::runtime_error("invalid stereo PCM size");
    std::vector<float> pcm(size_t(bytes)/4);
    f.seekg(0); f.read(reinterpret_cast<char*>(pcm.data()),bytes);
    if (!f) throw std::runtime_error("PCM read failed");
    return pcm;
}
struct StateDelete {
    void operator()(ebur128_state *state) const { if (state) ebur128_destroy(&state); }
};
using State = std::unique_ptr<ebur128_state, StateDelete>;
double peak_db(const std::vector<float> &pcm, size_t begin, size_t end) {
    double peak=0;
    for(size_t i=begin*2;i<end*2;++i) peak=std::max(peak,std::abs(double(pcm[i])));
    return peak>0 ? 20.0*std::log10(peak) : -HUGE_VAL;
}
int clip(double voice,double bgm,double mix) {
    const double peak=std::max({voice,bgm,mix});
    return peak>=0 ? 2 : peak>=-1 ? 1 : 0;
}
int main(int argc,char **argv) {
 try {
    if (argc!=3) return 2;
    const auto v=read(argv[1]), b=read(argv[2]);
    if (v.size()!=b.size()) return 2;
    std::vector<float> m(v.size());
    for(size_t i=0;i<m.size();++i) {
        m[i]=v[i]+b[i];
        if(!std::isfinite(v[i]) || !std::isfinite(b[i]) || !std::isfinite(m[i]))
            throw std::runtime_error("nonfinite input or mix");
    }
    State vs(ebur128_init(2,48000,EBUR128_MODE_S));
    State bs(ebur128_init(2,48000,EBUR128_MODE_S));
    State ms(ebur128_init(2,48000,EBUR128_MODE_S));
    if (!vs || !bs || !ms) throw std::runtime_error("ebur128 init failed");
    bool active=false;
    unsigned attack=0,release=0,valid=0,mix_valid=0;
    int color=-1,mix_color=-1;
    double sumv=0,sumb=0,summ=0;
    std::cout << "frame,voice,bgm,mix,voice_peak,bgm_peak,mix_peak,active,status,mix_status,clip_status,valid_points,mix_valid_points\n" << std::setprecision(17);
    const size_t frames=v.size()/2;
    for (size_t offset=0;offset+480<=frames;offset+=480) {
        if (ebur128_add_frames_float(vs.get(),v.data()+offset*2,480) ||
            ebur128_add_frames_float(bs.get(),b.data()+offset*2,480) ||
            ebur128_add_frames_float(ms.get(),m.data()+offset*2,480))
            throw std::runtime_error("ebur128 input failed");
        double power=0;
        for(size_t i=offset*2;i<(offset+480)*2;++i) power+=double(v[i])*v[i];
        const bool above=power/960 >= std::pow(10.0,-35.0/10);
        if(above){release=0;if(!active && ++attack>=15){active=true;attack=0;}}
        else {attack=0;if(active && ++release>=60){active=false;release=0;}}
        const size_t end=offset+480;
        if(end%4800) continue;
        double vl=-HUGE_VAL,bl=-HUGE_VAL,ml=-HUGE_VAL;
        if(end>=19200){
            if(ebur128_loudness_momentary(vs.get(),&vl) || ebur128_loudness_momentary(bs.get(),&bl) ||
               ebur128_loudness_momentary(ms.get(),&ml)) throw std::runtime_error("Momentary measurement failed");
        }
        int rating=-1,mix_rating=-1;
        if(active && std::isfinite(vl) && std::isfinite(bl)){
            double delta=vl-bl;
            if(color<0) color=delta>=8 ? 0 : delta>=5 ? 1 : 2;
            else if(delta>=8.5) color=0;
            else if(delta<4.5) color=2;
            else if(delta<7.5 && delta>=5.5) color=1;
            rating=color;
        }
        if(active && std::isfinite(ml)){
            if(mix_color<0) mix_color=ml>=-20 ? 0 : ml>=-24 ? 1 : 2;
            else if(ml>=-19.5) mix_color=0;
            else if(ml< -24.5) mix_color=2;
            else if(ml< -20.5 && ml>=-23.5) mix_color=1;
            mix_rating=mix_color;
        }
        if(active && end>=144000){
            double sv=-HUGE_VAL,sb=-HUGE_VAL,sm=-HUGE_VAL;
            if(ebur128_loudness_shortterm(vs.get(),&sv) || ebur128_loudness_shortterm(bs.get(),&sb) ||
               ebur128_loudness_shortterm(ms.get(),&sm)) throw std::runtime_error("Short-term measurement failed");
            if(std::isfinite(sv) && std::isfinite(sb)){
                sumv+=sv;sumb+=sb;++valid;
                if(std::isfinite(sm)){summ+=sm;++mix_valid;}
            }
        }
        const double pv=peak_db(v,end-4800,end),pb=peak_db(b,end-4800,end),pm=peak_db(m,end-4800,end);
        std::cout<<end<<','<<vl<<','<<bl<<','<<ml<<','<<pv<<','<<pb<<','<<pm<<','<<active<<','
                 <<rating<<','<<mix_rating<<','<<clip(pv,pb,pm)<<",0,0\n";
    }
    if(!valid) throw std::runtime_error("no valid points");
    const double mv=sumv/valid,mb=sumb/valid,mm=mix_valid ? summ/mix_valid : -HUGE_VAL;
    const double pv=peak_db(v,0,frames),pb=peak_db(b,0,frames),pm=peak_db(m,0,frames);
    const int mix_rating=mix_valid ? (mm>=-20 ? 0 : mm>=-24 ? 1 : 2) : -1;
    std::cout<<"mean,"<<mv<<','<<mb<<','<<mm<<','<<pv<<','<<pb<<','<<pm<<",0,"
             <<(mv-mb>=8 ? 0 : mv-mb>=5 ? 1 : 2)<<','<<mix_rating<<','<<clip(pv,pb,pm)<<','<<valid<<','<<mix_valid<<'\n';
    return 0;
 } catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
