#include "backstage/backstage_host.h"
#include "backstage/backstage_browser_host.h"
#include "util/log.h"

#include <string>

namespace hi5 {
    int RunStreamerMain(int argc, char** argv);
    int RunMediaHostMain(int argc, char** argv);
    int MaybeRunCefSubprocess(int argc, char** argv);
}

int main(int argc, char** argv) {
    const int cefRc = hi5::MaybeRunCefSubprocess(argc, argv);
    if (cefRc >= 0) return cefRc;

    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) != "--mode") continue;
        const std::string mode = argv[i + 1];
        if (mode == "streamer") return hi5::RunStreamerMain(argc, argv);
        if (mode == "media-host") return hi5::RunMediaHostMain(argc, argv);
        if (mode == "backstage-host") return hi5::RunBackstageHostMain(argc, argv);
        if (mode == "backstage-browser") return hi5::RunBackstageBrowserMain(argc, argv);
    }

    LogError("[remote-host] missing or unsupported --mode");
    return 2;
}
