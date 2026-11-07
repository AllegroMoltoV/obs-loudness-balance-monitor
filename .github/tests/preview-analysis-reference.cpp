// Offline reference uses only libebur128 primitives, never product analysis/VAD.
#include <ebur128.h>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iomanip>
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
int main(int argc,char **argv) {
 try {
    if (argc!=3) return 2;
    const auto v=read(argv[1]), b=read(argv[2]);
    if (v.size()!=b.size()) return 2;
    auto *vs=ebur128_init(2,48000,EBUR128_MODE_S), *bs=ebur128_init(2,48000,EBUR128_MODE_S);
    if (!vs || !bs) throw std::runtime_error("ebur128 init failed");
    bool active=false;
    unsigned attack=0,release=0,valid=0;
    int color=-1;
    double sumv=0,sumb=0;
    std::cout << "frame,voice,bgm,active,status\n" << std::setprecision(17);
    const size_t frames=v.size()/2;
    for (size_t offset=0;offset+480<=frames;offset+=480) {
        if (ebur128_add_frames_float(vs,v.data()+offset*2,480) || ebur128_add_frames_float(bs,b.data()+offset*2,480))
            throw std::runtime_error("ebur128 input failed");
        double power=0;
        for(size_t i=offset*2;i<(offset+480)*2;++i) power+=double(v[i])*v[i];
        const bool above=power/960 >= std::pow(10.0,-35.0/10);
        if(above){release=0;if(!active && ++attack>=15){active=true;attack=0;}}
        else {attack=0;if(active && ++release>=60){active=false;release=0;}}
        const size_t end=offset+480;
        if(end%4800) continue;
        double vl=-HUGE_VAL,bl=-HUGE_VAL;
        if(end>=19200){ebur128_loudness_momentary(vs,&vl);ebur128_loudness_momentary(bs,&bl);}
        int rating=-1;
        if(active && std::isfinite(vl) && std::isfinite(bl)){
            double delta=vl-bl;
            if(color<0) color=delta>=8 ? 0 : delta>=5 ? 1 : 2;
            else if(delta>=8.5) color=0;
            else if(delta<4.5) color=2;
            else if(delta<7.5 && delta>=5.5) color=1;
            rating=color;
        }
        if(active && end>=144000){
            double sv=-HUGE_VAL,sb=-HUGE_VAL;
            ebur128_loudness_shortterm(vs,&sv);ebur128_loudness_shortterm(bs,&sb);
            if(std::isfinite(sv) && std::isfinite(sb)){sumv+=sv;sumb+=sb;++valid;}
        }
        std::cout<<end<<','<<vl<<','<<bl<<','<<active<<','<<rating<<'\n';
    }
    if(!valid) throw std::runtime_error("no valid points");
    const double mv=sumv/valid, mb=sumb/valid;
    std::cout<<"mean,"<<mv<<','<<mb<<','<<valid<<','<<(mv-mb>=8 ? 0 : mv-mb>=5 ? 1 : 2)<<'\n';
    ebur128_destroy(&vs);ebur128_destroy(&bs);
    return 0;
 } catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
