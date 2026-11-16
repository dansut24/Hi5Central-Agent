// Hi5Central Streaming Lab: standalone localhost-only VP8/WebRTC diagnostics.
// No RMM APIs, service registration, unattended access, or remote control.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <rtc/rtc.hpp>
#include <vpx/vpx_encoder.h>
#include <vpx/vp8cx.h>
#include <libyuv/convert.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <variant>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
static std::atomic<bool> gRunning{true};
static BOOL WINAPI onConsole(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        gRunning = false;
        return TRUE;
    }
    return FALSE;
}
static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b-a).count();
}
static std::string env(const char* key) {
    const char* value = std::getenv(key);
    return value ? std::string(value) : std::string();
}
static std::string percentEncode(const std::string& input) {
    std::ostringstream out;
    const char* hex = "0123456789ABCDEF";
    for (unsigned char c : input) {
        if (std::isalnum(c) || c=='-' || c=='_' || c=='.' || c=='~') out << c;
        else out << '%' << hex[c >> 4] << hex[c & 15];
    }
    return out.str();
}
static std::string randomToken() {
    std::array<unsigned char, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        throw std::runtime_error("Unable to generate secure lab token");
    }
    std::ostringstream out;
    for (auto c : bytes) out << std::hex << std::setw(2) << std::setfill('0') << int(c);
    return out.str();
}
struct TurnConfig {
    std::string url, username, password;
    bool configured() const { return !url.empty() && !username.empty() && !password.empty(); }
    std::string hostUri() const {
        if (!configured()) return {};
        auto i = url.find(':');
        if (i==std::string::npos) return {};
        return url.substr(0,i+1) + percentEncode(username) + ":" +
               percentEncode(password) + "@" + url.substr(i+1);
    }
};
struct GdiCapture {
    HDC desktop = nullptr, dc = nullptr;
    HBITMAP bitmap = nullptr, original = nullptr;
    uint8_t* bgra = nullptr;
    int w=0,h=0;
    GdiCapture() {
        SetProcessDPIAware();
        w = GetSystemMetrics(SM_CXSCREEN);
        h = GetSystemMetrics(SM_CYSCREEN);
        if (w < 2 || h < 2) throw std::runtime_error("Invalid screen dimensions");
        w &= ~1; h &= ~1;
        desktop = GetDC(nullptr);
        if (!desktop) throw std::runtime_error("GetDC failed");
        dc = CreateCompatibleDC(desktop);
        if (!dc) throw std::runtime_error("CreateCompatibleDC failed");
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(desktop, &bi, DIB_RGB_COLORS,
                                  reinterpret_cast<void**>(&bgra), nullptr, 0);
        if (!bitmap || !bgra) throw std::runtime_error("CreateDIBSection failed");
        original = static_cast<HBITMAP>(SelectObject(dc,bitmap));
    }
    ~GdiCapture() {
        if (dc && original) SelectObject(dc,original);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        if (desktop) ReleaseDC(nullptr,desktop);
    }
    bool capture() { return BitBlt(dc,0,0,w,h,desktop,0,0,SRCCOPY|CAPTUREBLT) != FALSE; }
};
struct I420 {
    int w=0,h=0;
    std::vector<uint8_t> pixels;
    uint8_t *y=nullptr,*u=nullptr,*v=nullptr;
    explicit I420(int width, int height):w(width),h(height) {
        const size_t planeY=size_t(w)*h;
        const size_t planeUV=size_t(w/2)*(h/2);
        pixels.resize(planeY+2*planeUV);
        y=pixels.data(); u=y+planeY; v=u+planeUV;
    }
    bool convert(const GdiCapture& src) {
        return libyuv::ARGBToI420(src.bgra, src.w*4, y,w,u,w/2,v,w/2,w,h)==0;
    }
};
class Vp8 {
    vpx_codec_ctx_t ctx{};
    bool initialized=false;
    uint64_t frameNumber=0;
    uint64_t lastPts=0;
    int fps=30;
public:
    Vp8(int w,int h,int rate,int kbps):fps(rate) {
        vpx_codec_enc_cfg_t cfg{};
        if (vpx_codec_enc_config_default(vpx_codec_vp8_cx(),&cfg,0)!=VPX_CODEC_OK)
            throw std::runtime_error("VP8 default config unavailable");
        cfg.g_w=w; cfg.g_h=h; cfg.g_threads=4;
        cfg.g_timebase.num=1; cfg.g_timebase.den=90000;
        cfg.g_lag_in_frames=0; cfg.g_error_resilient=1;
        cfg.rc_end_usage=VPX_CBR; cfg.rc_target_bitrate=kbps;
        cfg.rc_resize_allowed=0; cfg.rc_dropframe_thresh=0;
        cfg.rc_min_quantizer=4; cfg.rc_max_quantizer=40;
        cfg.kf_mode=VPX_KF_AUTO; cfg.kf_max_dist=rate*4;
        if (vpx_codec_enc_init(&ctx,vpx_codec_vp8_cx(),&cfg,0)!=VPX_CODEC_OK)
            throw std::runtime_error("VP8 initialisation failed");
        initialized=true;
        vpx_codec_control(&ctx,VP8E_SET_CPUUSED,8);
    }
    ~Vp8(){if(initialized) vpx_codec_destroy(&ctx);}
    struct Output {std::vector<uint8_t> data;uint32_t timestamp=0;bool key=false;};
    Output encode(const I420& input, uint64_t requestedPts, bool forceKeyframe=false) {
        vpx_image_t img{};
        if (!vpx_img_wrap(&img,VPX_IMG_FMT_I420,input.w,input.h,1,nullptr))
            throw std::runtime_error("VP8 image wrap failed");
        img.planes[0]=input.y; img.planes[1]=input.u; img.planes[2]=input.v;
        img.stride[0]=input.w; img.stride[1]=input.w/2; img.stride[2]=input.w/2;
        const uint64_t ts=frameNumber==0 ? 0 : std::max(lastPts+1,requestedPts);
        lastPts=ts;
        ++frameNumber;
        const int flags=(frameNumber==1 || forceKeyframe)?VPX_EFLAG_FORCE_KF:0;
        if (vpx_codec_encode(&ctx,&img,static_cast<vpx_codec_pts_t>(ts),
            static_cast<unsigned long>(90000/fps),flags,VPX_DL_REALTIME)!=VPX_CODEC_OK)
            throw std::runtime_error("VP8 encoding failed");
        Output out; out.timestamp=static_cast<uint32_t>(ts);
        vpx_codec_iter_t it=nullptr;
        const vpx_codec_cx_pkt_t* pkt=nullptr;
        while ((pkt=vpx_codec_get_cx_data(&ctx,&it))) {
            if (pkt->kind!=VPX_CODEC_CX_FRAME_PKT) continue;
            const auto* ptr=static_cast<const uint8_t*>(pkt->data.frame.buf);
            out.data.insert(out.data.end(),ptr,ptr+pkt->data.frame.sz);
            out.key = (pkt->data.frame.flags & VPX_FRAME_IS_KEY)!=0;
        }
        return out;
    }
};
struct AccumStats {
    uint64_t captured=0, encoded=0, sent=0, dropped=0, missedDeadlines=0;
    double captureSum=0,convertSum=0,encodeSum=0,sendSum=0;
    double lastAge=0,maxAge=0,lastCapture=0,lastConvert=0,lastEncode=0;
    Clock::time_point start=Clock::now();
    int width=0,height=0,fps=30,kbps=8000;
};
class Session {
    std::mutex eventMu, statsMu;
    std::deque<std::pair<uint64_t,json>> events;
    uint64_t eventId=0;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> track;
    std::thread worker;
    std::atomic<bool> running{true}, ready{false};
    uint32_t ssrc=0;
    uint16_t sequence=0;
    std::atomic<bool> forceKeyframe{true};
    int fps=30,kbps=8000;
    TurnConfig turn;
    AccumStats stats;
    void push(const json& event) {
        std::lock_guard<std::mutex> lock(eventMu);
        events.emplace_back(++eventId,event);
        if (events.size()>200) events.pop_front();
    }
    void pushVp8(const Vp8::Output& frame) {
        // VP8 payload descriptor (RFC 7741), 12-byte RTP header, 1200-byte MTU.
        const size_t maxChunk=1199;
        for(size_t off=0;off<frame.data.size();) {
            const size_t n=std::min(maxChunk,frame.data.size()-off);
            const bool first=(off==0),last=(off+n==frame.data.size());
            rtc::binary pkt(13+n);
            auto set=[&](size_t i,uint8_t b){pkt[i]=static_cast<std::byte>(b);};
            set(0,0x80);set(1,uint8_t((last?0x80:0)|96));
            set(2,uint8_t(sequence>>8));set(3,uint8_t(sequence));
            set(4,uint8_t(frame.timestamp>>24));set(5,uint8_t(frame.timestamp>>16));
            set(6,uint8_t(frame.timestamp>>8));set(7,uint8_t(frame.timestamp));
            set(8,uint8_t(ssrc>>24));set(9,uint8_t(ssrc>>16));
            set(10,uint8_t(ssrc>>8));set(11,uint8_t(ssrc));
            set(12,first?0x10:0x00);
            for(size_t j=0;j<n;++j)set(13+j,frame.data[off+j]);
            track->send(pkt);
            ++sequence;off+=n;
        }
    }
    void stream() {
        try {
            GdiCapture capture;
            I420 i420(capture.w,capture.h);
            Vp8 encoder(capture.w,capture.h,fps,kbps);
            {
                std::lock_guard<std::mutex> lock(statsMu);
                stats.width=capture.w;stats.height=capture.h;stats.start=Clock::now();
            }
            push({{"type","capture_started"},{"width",capture.w},{"height",capture.h}});
            const auto frameDuration=std::chrono::nanoseconds(1000000000LL/fps);
            auto deadline=Clock::now();
            uint64_t missed=0;
            const auto epoch=Clock::now();
            while(running && ready && gRunning) {
                auto now=Clock::now();
                if(now<deadline) std::this_thread::sleep_until(deadline);
                else if(now>deadline+frameDuration) {
                    const auto late=std::chrono::duration_cast<std::chrono::nanoseconds>(now-deadline).count();
                    missed+=static_cast<uint64_t>(late/frameDuration.count());
                    deadline=now;
                }
                const auto t0=Clock::now();
                if(!capture.capture()) {std::lock_guard<std::mutex> lock(statsMu);++stats.dropped;deadline+=frameDuration;continue;}
                const auto t1=Clock::now();
                if(!i420.convert(capture)) {std::lock_guard<std::mutex> lock(statsMu);++stats.dropped;deadline+=frameDuration;continue;}
                const auto t2=Clock::now();
                const auto elapsedUs=std::chrono::duration_cast<std::chrono::microseconds>(t0-epoch).count();
                const auto pts=static_cast<uint64_t>(std::max<int64_t>(0,elapsedUs))*90ULL/1000ULL;
                auto encoded=encoder.encode(i420,pts,forceKeyframe.exchange(false));
                const auto t3=Clock::now();
                bool delivered=false;
                if(!encoded.data.empty() && ready && track && track->isOpen()) {
                    try {pushVp8(encoded);delivered=true;} catch (const std::exception&) {}
                }
                const auto t4=Clock::now();
                {
                    std::lock_guard<std::mutex> lock(statsMu);
                    ++stats.captured;
                    if(!encoded.data.empty()) ++stats.encoded;
                    if(delivered) ++stats.sent; else ++stats.dropped;
                    stats.missedDeadlines=missed;
                    stats.lastCapture=ms(t0,t1);stats.lastConvert=ms(t1,t2);stats.lastEncode=ms(t2,t3);
                    stats.captureSum+=stats.lastCapture;stats.convertSum+=stats.lastConvert;
                    stats.encodeSum+=stats.lastEncode;stats.sendSum+=ms(t3,t4);
                    stats.lastAge=ms(t0,t4);stats.maxAge=std::max(stats.maxAge,stats.lastAge);
                }
                deadline+=frameDuration;
            }
        } catch(const std::exception& ex) {
            push({{"type","error"},{"message",ex.what()}});
            std::cerr << "[lab] stream error: " << ex.what() << "\n";
        }
    }
public:
    Session(int rate,int bitrate,TurnConfig t):fps(rate),kbps(bitrate),turn(std::move(t)){
        std::random_device rd;ssrc=(uint32_t(rd())<<16)^uint32_t(rd());
        stats.fps=fps;stats.kbps=kbps;
    }
    ~Session(){ stop(); }
    void stop(){
        running=false;ready=false;
        if(worker.joinable())worker.join();
        if(pc){try{pc->close();}catch(...){}}
        track.reset();pc.reset();
    }
    void start(){
        rtc::Configuration c;
        c.forceMediaTransport=true;
        if(turn.configured())c.iceServers.emplace_back(turn.hostUri());
        pc=std::make_shared<rtc::PeerConnection>(c);
        pc->onLocalDescription([this](rtc::Description d){
            push({{"type","webrtc_offer"},{"sdp",std::string(d)},{"sdp_type",d.typeString()}});
        });
        pc->onLocalCandidate([this](rtc::Candidate c){
            push({{"type","ice_candidate"},{"candidate",std::string(c)},{"sdpMid",c.mid()}});
        });
        pc->onStateChange([this](rtc::PeerConnection::State s){
            push({{"type","peer_state"},{"state",static_cast<int>(s)}});
            if(s==rtc::PeerConnection::State::Disconnected ||
               s==rtc::PeerConnection::State::Failed ||
               s==rtc::PeerConnection::State::Closed) ready=false;
        });
        rtc::Description::Video m("video",rtc::Description::Direction::SendOnly);
        m.addVP8Codec(96);m.addSSRC(ssrc,"lab-video");m.setBitrate(kbps*1000);
        track=pc->addTrack(m);
        // Keep RTP packets for NACK retransmission and request encoder keyframes on PLI.
        try {
            auto nack=std::make_shared<rtc::RtcpNackResponder>(2048);
            nack->addToChain(std::make_shared<rtc::PliHandler>(
                [this](){forceKeyframe=true;}));
            track->setMediaHandler(nack);
        } catch(const std::exception& e) {
            push({{"type","error"},{"message",std::string("RTCP recovery disabled: ")+e.what()}});
        }
        track->onOpen([this]{
            ready=true;
            if(!worker.joinable())worker=std::thread([this]{stream();});
        });
        track->onClosed([this]{ready=false;});
        pc->setLocalDescription();
    }
    void signal(const json& message){
        if(!pc)throw std::runtime_error("No active peer");
        const auto type=message.value("type",std::string());
        if(type=="answer") {
            pc->setRemoteDescription(rtc::Description(message.at("sdp").get<std::string>(),"answer"));
        } else if(type=="ice_candidate") {
            pc->addRemoteCandidate(rtc::Candidate(message.at("candidate").get<std::string>(),
                 message.value("sdpMid",std::string("video"))));
        }
    }
    json poll(uint64_t after){
        json list=json::array();
        std::lock_guard<std::mutex> lock(eventMu);
        for(const auto& p:events)if(p.first>after)list.push_back({{"id",p.first},{"event",p.second}});
        return {{"events",list},{"last",eventId}};
    }
    json snapshot(){
        std::lock_guard<std::mutex> lock(statsMu);
        auto n=std::max<uint64_t>(stats.captured,1);
        auto elapsed=std::max(0.001,ms(stats.start,Clock::now())/1000.);
        return {
            {"active",ready.load()},{"codec","VP8 software"},{"configuredFps",stats.fps},
            {"bitrateKbps",stats.kbps},{"width",stats.width},{"height",stats.height},
            {"seconds",elapsed},{"captured",stats.captured},{"encoded",stats.encoded},
            {"sent",stats.sent},{"dropped",stats.dropped},{"missedDeadlines",stats.missedDeadlines},
            {"fpsMeasured",stats.sent/elapsed},
            {"captureMsAvg",stats.captureSum/n},{"convertMsAvg",stats.convertSum/n},
            {"encodeMsAvg",stats.encodeSum/n},{"sendMsAvg",stats.sendSum/n},
            {"captureMsLast",stats.lastCapture},{"convertMsLast",stats.lastConvert},
            {"encodeMsLast",stats.lastEncode},{"lastFrameAgeMs",stats.lastAge},
            {"maxFrameAgeMs",stats.maxAge}
        };
    }
};
static std::mutex gSessionMu;
static std::shared_ptr<Session> gSession;
static std::string gToken;
static TurnConfig gTurn;
static int gDefaultFps=30;
static int gDefaultKbps=8000;
static std::filesystem::path executableDirectory() {
    std::array<wchar_t,32768> p{};
    auto n=GetModuleFileNameW(nullptr,p.data(),static_cast<DWORD>(p.size()));
    if(!n || n>=p.size())throw std::runtime_error("GetModuleFileNameW failed");
    return std::filesystem::path(std::wstring(p.data(),n)).parent_path();
}
static std::string readViewer() {
    auto file=executableDirectory()/L"viewer.html";
    std::ifstream stream(file,std::ios::binary);
    if(!stream)throw std::runtime_error("viewer.html must be beside Hi5StreamingLab.exe");
    return std::string(std::istreambuf_iterator<char>(stream),{});
}
struct Request {std::string method,path,body,token,origin;};
static std::string headerValue(const std::string& header,const std::string& key){
    auto text=header;auto needle=key;
    std::transform(text.begin(),text.end(),text.begin(),[](unsigned char c){return char(std::tolower(c));});
    std::transform(needle.begin(),needle.end(),needle.begin(),[](unsigned char c){return char(std::tolower(c));});
    auto p=text.find("\r\n"+needle+":");
    if(p==std::string::npos)return {};
    p+=needle.size()+3;
    while(p<header.size() && header[p]==' ')++p;
    auto end=header.find("\r\n",p);
    return header.substr(p,end==std::string::npos?std::string::npos:end-p);
}
static Request recvRequest(SOCKET sock) {
    std::string raw;
    char buf[8192];
    size_t headerEnd=std::string::npos;
    while(raw.size()<131072) {
        int n=recv(sock,buf,sizeof(buf),0);
        if(n<=0)break;
        raw.append(buf,n);
        headerEnd=raw.find("\r\n\r\n");
        if(headerEnd!=std::string::npos)break;
    }
    if(headerEnd==std::string::npos)throw std::runtime_error("Invalid HTTP headers");
    auto headers=raw.substr(0,headerEnd+4);
    auto lenStr=headerValue(headers,"Content-Length");
    const size_t len=lenStr.empty()?0:static_cast<size_t>(std::stoul(lenStr));
    if(len>65536)throw std::runtime_error("HTTP body too large");
    while(raw.size()<headerEnd+4+len){
        int n=recv(sock,buf,int(std::min<size_t>(sizeof(buf),headerEnd+4+len-raw.size())),0);
        if(n<=0)throw std::runtime_error("Truncated HTTP body");
        raw.append(buf,n);
    }
    std::istringstream line(headers.substr(0,headers.find("\r\n")));
    Request r;line>>r.method>>r.path;
    r.token=headerValue(headers,"X-Lab-Token");
    r.origin=headerValue(headers,"Origin");
    r.body=raw.substr(headerEnd+4,len);
    return r;
}
static void respond(SOCKET sock,int status,const std::string& payload,const std::string& mime="application/json") {
    std::ostringstream header;
    header << "HTTP/1.1 " << status << (status==200?" OK":status==403?" Forbidden":status==404?" Not Found":" Bad Request") << "\r\n"
           << "Content-Type: " << mime << "; charset=utf-8\r\n"
           << "Content-Length: " << payload.size() << "\r\n"
           << "Cache-Control: no-store\r\n"
           << "X-Content-Type-Options: nosniff\r\n"
           << "X-Frame-Options: DENY\r\n"
           << "Referrer-Policy: no-referrer\r\n"
           << "Connection: close\r\n\r\n";
    auto h=header.str();
    send(sock,h.data(),static_cast<int>(h.size()),0);
    size_t off=0;
    while(off<payload.size()){
        int n=send(sock,payload.data()+off,static_cast<int>(std::min<size_t>(16384,payload.size()-off)),0);
        if(n<=0)break;off+=n;
    }
}
static void handle(SOCKET client) {
    try{
        const auto r=recvRequest(client);
        if(r.method=="GET" && r.path=="/"){
            auto html=readViewer();
            const auto marker=html.find("__LAB_TOKEN__");
            if(marker==std::string::npos)throw std::runtime_error("Viewer template missing token placeholder");
            html.replace(marker,strlen("__LAB_TOKEN__"),gToken);
            respond(client,200,html,"text/html");return;
        }
        if(r.token!=gToken ||
            (!r.origin.empty() && r.origin!="http://127.0.0.1:8787")) {
            respond(client,403,R"({"error":"unauthorised"})");return;
        }
        if(r.method=="GET" && r.path=="/config"){
            json j={{"fps",gDefaultFps},{"bitrateKbps",gDefaultKbps},{"turnConfigured",gTurn.configured()}};
            j["iceServers"]=json::array();
            if(gTurn.configured())j["iceServers"].push_back({
                {"urls",gTurn.url},{"username",gTurn.username},{"credential",gTurn.password}});
            respond(client,200,j.dump());return;
        }
        if(r.method=="POST" && r.path=="/start"){
            auto j=json::parse(r.body);
            const int fps=j.value("fps",gDefaultFps);
            const int kbps=j.value("bitrateKbps",gDefaultKbps);
            if((fps!=15 && fps!=30 && fps!=45 && fps!=60) || kbps<500 || kbps>30000){
                respond(client,400,R"({"error":"invalid fps or bitrate"})");return;
            }
            auto replacement=std::make_shared<Session>(fps,kbps,gTurn);
            std::lock_guard<std::mutex> lock(gSessionMu);
            if(gSession)gSession->stop();
            gSession=replacement;
            gSession->start();
            respond(client,200,R"({"ok":true,"codec":"VP8"})");return;
        }
        if(r.method=="POST" && r.path=="/stop"){
            std::lock_guard<std::mutex> lock(gSessionMu);
            if(gSession){gSession->stop();gSession.reset();}
            respond(client,200,R"({"ok":true})");return;
        }
        std::shared_ptr<Session> s;
        {std::lock_guard<std::mutex> lock(gSessionMu);s=gSession;}
        if(!s){respond(client,200,R"({"active":false})");return;}
        if(r.method=="POST" && r.path=="/signal"){
            s->signal(json::parse(r.body));
            respond(client,200,R"({"ok":true})");return;
        }
        if(r.method=="GET" && r.path=="/stats"){respond(client,200,s->snapshot().dump());return;}
        if(r.method=="GET" && r.path.rfind("/events?after=",0)==0){
            auto after=std::stoull(r.path.substr(strlen("/events?after=")));
            respond(client,200,s->poll(after).dump());return;
        }
        respond(client,404,R"({"error":"not found"})");
    }catch(const std::exception& e){
        respond(client,400,json({{"error",e.what()}}).dump());
    }
}

/* Public lab relay mode. All session signalling is outbound over authenticated TLS.
   Pairing requires an eight-digit code displayed locally on the Windows console.
   No Windows input channel, service, RMM API or persistent host identity. */
static int runRemote(const std::string& endpoint) {
    std::cout << "\n=== Hi5Central Streaming Lab / Web Viewer ===\n"
              << "Webpage: https://hi5central.com/streamlab\n"
              << "Only accept a pairing code while YOU are testing.\n"
              << "Press Ctrl+C to terminate desktop sharing and pairing.\n\n";
    while (gRunning) {
        std::atomic<bool> connected{false}, disconnected{false};
        std::mutex inboxMu;
        std::deque<json> inbox;
        std::shared_ptr<rtc::WebSocket> ws=std::make_shared<rtc::WebSocket>();
        std::shared_ptr<Session> session;
        bool paired=false;
        uint64_t lastEvent=0;
        TurnConfig turn;
        auto nextStats=Clock::now();
        auto sendJson=[&](const json& data) {
            if (!connected.load()) return;
            try { ws->send(data.dump()); }
            catch(const std::exception& e) {
                std::cerr << "[lab] websocket send error: " << e.what() << "\n";
            }
        };
        ws->onOpen([&]{
            connected=true;
            std::cout << "[lab] Secure relay connected. Requesting pairing code...\n";
            sendJson({{"type","hello"}});
        });
        ws->onClosed([&]{disconnected=true;connected=false;});
        ws->onError([&](std::string error){
            std::cerr << "[lab] Signalling error: " << error << "\n";
            disconnected=true;connected=false;
        });
        ws->onMessage([&](rtc::message_variant msg){
            if(const auto* text=std::get_if<std::string>(&msg)) {
                const auto parsed=json::parse(*text,nullptr,false);
                if(!parsed.is_discarded()) {
                    std::lock_guard<std::mutex> lock(inboxMu);
                    inbox.push_back(parsed);
                    if(inbox.size()>300)inbox.pop_front();
                }
            }
        });
        try { ws->open(endpoint); }
        catch(const std::exception& e) {
            std::cerr << "[lab] Unable to open relay: " << e.what() << "\n";
            disconnected=true;
        }
        while(gRunning && !disconnected.load()) {
            std::deque<json> localMessages;
            {
                std::lock_guard<std::mutex> lock(inboxMu);
                localMessages.swap(inbox);
            }
            for(const auto& msg:localMessages) {
                if(!msg.is_object()) continue;
                const std::string type=msg.value("type",std::string());
                if(type=="registered") {
                    std::cout << "\n--------------------------------------\n"
                              << "  WEB VIEWER: https://hi5central.com/streamlab\n"
                              << "  ONE-TIME PAIRING CODE: "<<msg.value("code",std::string("unknown"))
                              << "\n  Code valid for 10 minutes.\n"
                              << "--------------------------------------\n\n";
                } else if(type=="paired") {
                    paired=true;
                    if(session) {session->stop();session.reset();}
                    turn={};
                    const auto ice=msg.value("iceServers",json::array());
                    if(ice.is_array())for(const auto& item:ice) {
                        if(!item.is_object())continue;
                        const std::string url=item.value("urls",std::string());
                        if(url.rfind("turn:",0)==0 && item.contains("credential")) {
                            turn.url=url;
                            turn.username=item.value("username",std::string());
                            turn.password=item.value("credential",std::string());
                            break;
                        }
                    }
                    if(!turn.configured()){
                        std::cerr << "[lab] TURN credentials unavailable. Refusing remote stream.\n";
                        sendJson({{"type","error"},{"message","TURN unavailable"}});
                        paired=false;
                    } else std::cout << "[lab] Browser paired; waiting for Start on web page.\n";
                } else if(type=="start" && paired) {
                    const int fps=msg.value("fps",30);
                    const int kbps=msg.value("bitrateKbps",8000);
                    if((fps!=15 && fps!=30 && fps!=45 && fps!=60) || kbps<500 || kbps>30000){
                        sendJson({{"type","error"},{"message","Invalid frame rate or bitrate"}});
                        continue;
                    }
                    if(session){session->stop();session.reset();}
                    lastEvent=0;
                    try {
                        session=std::make_shared<Session>(fps,kbps,turn);
                        session->start();
                        nextStats=Clock::now();
                        std::cout << "[lab] Started independent VP8 session " << fps
                                  << " FPS @ " << kbps << " kbps. Awaiting browser WebRTC answer.\n";
                    }catch(const std::exception& e){
                        session.reset();
                        sendJson({{"type","error"},{"message",std::string("Host start failed: ")+e.what()}});
                    }
                } else if(type=="webrtc_answer" && session) {
                    try{
                        session->signal({{"type","answer"},{"sdp",msg.at("sdp")}});
                        std::cout << "[lab] Applied browser SDP answer.\n";
                    }catch(const std::exception& e){
                        sendJson({{"type","error"},{"message",std::string("SDP rejected: ")+e.what()}});
                    }
                } else if(type=="ice_candidate" && session) {
                    try{session->signal(msg);}
                    catch(const std::exception& e){
                        std::cerr << "[lab] Candidate error: " << e.what() << "\n";
                    }
                } else if(type=="stop" || type=="viewer_left") {
                    if(session){session->stop();session.reset();}
                    if(type=="viewer_left")paired=false;
                    else sendJson({{"type","stopped"}});
                    std::cout << "[lab] Stream stopped: " << type << "\n";
                } else if(type=="error") {
                    std::cout << "[lab] Signalling: "<<msg.value("message",std::string("error"))<<"\n";
                }
            }
            if(session){
                auto polling=session->poll(lastEvent);
                lastEvent=polling.value("last",lastEvent);
                for(const auto& item:polling.value("events",json::array())){
                    if(!item.is_object() || !item.contains("event"))continue;
                    json event=item["event"];
                    if(event.is_object())sendJson(event);
                }
                if(Clock::now()>=nextStats){
                    sendJson({{"type","stats"},{"stats",session->snapshot()}});
                    nextStats=Clock::now()+std::chrono::seconds(1);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
        if(session){session->stop();session.reset();}
        connected=false;
        try{ws->close();}catch(...){}
        // Give outstanding libdatachannel callbacks time to finish before their captures expire.
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if(gRunning){
            std::cout << "[lab] Relay disconnected. Retrying in three seconds...\n";
            for(int i=0;i<30 && gRunning;++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    return 0;
}

int main(int argc,char** argv){
    WSADATA wsa{};
    if(WSAStartup(MAKEWORD(2,2),&wsa)!=0)return 1;
    SetConsoleCtrlHandler(onConsole,TRUE);
    if(argc==1 || std::string(argv[1])!="--local") {
        const auto url=env("HI5_LAB_RELAY_URL").empty() ?
            "wss://hi5central.com/streamlab/ws?role=host" : env("HI5_LAB_RELAY_URL");
        const int code=runRemote(url);
        WSACleanup();
        return code;
    }
    try {
        gToken=randomToken();
        gTurn={env("HI5_LAB_TURN_URL"),env("HI5_LAB_TURN_USER"),env("HI5_LAB_TURN_PASS")};
        if(!gTurn.url.empty() && !gTurn.configured())
            std::cout << "TURN ignored: both HI5_LAB_TURN_USER and HI5_LAB_TURN_PASS required.\n";
        // Fail early if viewer file was not distributed beside executable.
        (void)readViewer();
        SOCKET listenSock=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        if(listenSock==INVALID_SOCKET)throw std::runtime_error("socket failed");
        sockaddr_in bindAddr{};bindAddr.sin_family=AF_INET;
        bindAddr.sin_port=htons(8787);bindAddr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if(bind(listenSock,reinterpret_cast<sockaddr*>(&bindAddr),sizeof(bindAddr))==SOCKET_ERROR ||
           listen(listenSock,4)==SOCKET_ERROR){
            closesocket(listenSock);throw std::runtime_error("Cannot bind 127.0.0.1:8787");
        }
        std::cout << "\nHi5Central Streaming Lab — isolated diagnostic build\n"
                  << "Open http://127.0.0.1:8787 on THIS Windows desktop.\n"
                  << "VP8 baseline | FPS 15/30/45/60 | TURN "
                  << (gTurn.configured()?"configured":"not configured") << "\n"
                  << "Local diagnostic only. Press Ctrl+C to stop.\n\n";
        while(gRunning){
            fd_set readSet;FD_ZERO(&readSet);FD_SET(listenSock,&readSet);
            timeval wait{};wait.tv_sec=0;wait.tv_usec=250000;
            int selected=select(0,&readSet,nullptr,nullptr,&wait);
            if(selected<=0)continue;
            SOCKET c=accept(listenSock,nullptr,nullptr);
            if(c==INVALID_SOCKET)continue;
            DWORD timeout=2000;
            setsockopt(c,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
            setsockopt(c,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
            handle(c);closesocket(c);
        }
        closesocket(listenSock);
        {std::lock_guard<std::mutex> lock(gSessionMu);if(gSession)gSession->stop();gSession.reset();}
    } catch(const std::exception& e){
        std::cerr << "[lab] fatal: "<<e.what()<<"\n";
        WSACleanup();return 1;
    }
    WSACleanup();return 0;
}
