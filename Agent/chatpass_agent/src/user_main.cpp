#include "ui/native_banner.h"
#include "ui/native_tray.h"
#include "util/log.h"

#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace hi5 {
    int RunNativeChatMain(int argc, char** argv);
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif

    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) != "--mode") continue;
        const std::string mode = argv[i + 1];
        if (mode == "tray") return hi5::RunNativeTrayMain(argc, argv);
        if (mode == "banner") return hi5::RunNativeBannerMain(argc, argv);
        if (mode == "native-chat") return hi5::RunNativeChatMain(argc, argv);
    }

    LogError("[user-host] missing or unsupported --mode");
    return 2;
}
