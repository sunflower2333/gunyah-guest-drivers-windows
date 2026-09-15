// SPDX-License-Identifier: BSD-3-Clause
#include "video_client.h"
#include <cstdio>

// Query the actual host formats, with a finite enumeration limit and explicit errors.
static void Probe(VideoClient &client)
{
    auto config=client.Op(VV_OP_CONFIG).config;
    std::printf("card: %.*s\ncaps: 0x%08x\n",32,reinterpret_cast<const char *>(config.card),config.device_caps);
    client.Op(VV_OP_OPEN);
    const uint32_t queues[]={VV_OUTPUT,VV_CAPTURE,VV_OUTPUT_MPLANE,VV_CAPTURE_MPLANE};
    for (auto queue:queues) {
        bool ended=false;
        for (uint32_t index=0;index<64;++index) {
            VV_FMTDESC format={}; format.type=queue; format.index=index;
            VV_REQUEST request={}; request.operation=VV_OP_IOCTL; request.code=VV_IOCTL_ENUM_FMT;
            request.payload_size=sizeof(format); std::memcpy(request.payload,&format,sizeof(format));
            auto reply=client.Exec(request,true);
            if (reply.error==22) { ended=true; break; }
            if (reply.error || reply.payload_size!=sizeof(format)) throw std::runtime_error("ENUM_FMT failed");
            std::memcpy(&format,reply.payload,sizeof(format));
            char fourcc[5]={}; std::memcpy(fourcc,&format.fourcc,4);
            std::printf("queue: %u\nindex: %u\nfourcc: %s\ndescription: %.*s\n",
                        queue,index,fourcc,32,reinterpret_cast<const char *>(format.description));
        }
        if (!ended) throw std::runtime_error("ENUM_FMT exceeded 64 entries");
    }
    client.Op(VV_OP_CLOSE);
}

// List endpoints or probe one; no success is synthesized when no device is present.
int main(int argc, char **argv)
{
    try {
        auto paths=VideoPaths();
        if (paths.empty()) throw std::runtime_error("No VioGPU Video media function found");
        if (argc==1) {
            for (size_t i=0;i<paths.size();++i) std::wprintf(L"index: %zu\npath: %ls\n",i,paths[i].c_str());
            std::puts("Usage: video_probe.exe <index>");
            return 0;
        }
        if (argc!=2) throw std::runtime_error("Usage: video_probe.exe <index>");
        size_t consumed=0;
        auto index=std::stoul(argv[1],&consumed);
        if (consumed!=std::strlen(argv[1]) || index>=paths.size()) throw std::runtime_error("Invalid device index");
        VideoClient client(paths[index]); Probe(client);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr,"FAIL: %s\n",error.what());
        return 1;
    }
}
