#pragma once

#include <atomic>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace hi5 {

    struct PatchWorkerConfig {
        std::string apiBaseUrl = "https://hi5tech-software-intelligence.vercel.app";
        std::string apiKey;
        std::string deviceId;
        int pollSeconds = 30;
        bool enabled = true;
    };

    class PatchWorker {
    public:
        PatchWorker();
        ~PatchWorker();

        PatchWorker(const PatchWorker&) = delete;
        PatchWorker& operator=(const PatchWorker&) = delete;

        void Start(PatchWorkerConfig config);
        void Stop();

    private:
        struct CommandResult {
            int exitCode = 1;
            std::string output;
            std::wstring scriptPath;
            std::wstring outputPath;
        };

        void Run();
        void PollOnce();
        void ExecuteTask(const nlohmann::json& task);
        bool DownloadFile(const std::string& url, const std::wstring& outputPath);
        CommandResult RunCommand(const std::string& taskId, const std::string& command, const std::wstring& workingDir);
        void Report(
            const std::string& taskId,
            const std::string& status,
            const std::string& currentStep,
            int progress,
            const std::string& message,
            int exitCode = -1,
            bool rebootRequired = false
        );

        std::atomic<bool> stop_{ false };
        std::thread thread_;
        PatchWorkerConfig config_;
    };

} // namespace hi5
