#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace hi5 {

struct MemoryStats {
    std::uint64_t totalBytes = 0;
    std::uint64_t availableBytes = 0;
    std::uint64_t usedBytes = 0;
    double usedPercent = 0.0;
};

struct DiskStats {
    std::uint64_t totalBytes = 0;
    std::uint64_t freeBytes = 0;
    std::uint64_t usedBytes = 0;
    double usedPercent = 0.0;
};

std::string platformId();
std::string platformDisplayName();
std::string architecture();
std::string hostname();
std::string machineId();
std::string osVersion();
std::string osBuild();
std::string osDistributionId();
std::string osDistributionVersion();
std::string osDistributionName();
std::string osDistributionLike();
std::string osDistributionCodename();
std::string osUbuntuCodename();
std::string manufacturer();
std::string model();
std::string serialNumber();
std::string cpuModel();
std::string activeUser();
std::uint64_t uptimeSeconds();
double cpuPercent();
MemoryStats memoryStats();
DiskStats rootDiskStats();
nlohmann::json networkInfo();
std::string nowIsoUtc();
std::filesystem::path defaultStateDirectory();

} // namespace hi5
