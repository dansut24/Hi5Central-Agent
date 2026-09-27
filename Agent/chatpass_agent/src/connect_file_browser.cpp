#include "connect_file_browser.h"

#include "util/log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace hi5 {
namespace fs = std::filesystem;
using json = nlohmann::json;

ConnectFileBrowser::ConnectFileBrowser(SendCallback send)
    : send_(std::move(send)) {}

void ConnectFileBrowser::Send(json payload) {
    if (send_) send_(payload);
}

std::string ConnectFileBrowser::ComputerName() {
#ifdef _WIN32
    char name[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD size = static_cast<DWORD>(sizeof(name));
    if (GetComputerNameA(name, &size) && size > 0) return std::string(name, size);
#endif
    return "Customer computer";
}

fs::path ConnectFileBrowser::ResolvePath(const std::string& rawPath) {
#ifdef _WIN32
    if (rawPath.empty() || rawPath == "/" || rawPath == "drives") return fs::path();
#endif
    return fs::path(rawPath);
}

bool ConnectFileBrowser::IsProtectedPath(const fs::path& target) {
#ifdef _WIN32
    std::string s = target.string();
    std::replace(s.begin(), s.end(), '/', '\\');
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lower.size() == 3 &&
        std::isalpha(static_cast<unsigned char>(lower[0])) &&
        lower[1] == ':' && lower[2] == '\\') return true;

    std::string name = target.filename().string();
    std::transform(name.begin(), name.end(), name.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (name == "$recycle.bin" || name == "system volume information" ||
        name == "recovery" || name == "windows" || name == "program files" ||
        name == "program files (x86)" || name == "programdata") return true;
    if (name == "pagefile.sys" || name == "hiberfil.sys" ||
        name == "swapfile.sys" || name == "bootmgr") return true;
    if (lower.find("\\$recycle.bin") != std::string::npos ||
        lower.find("\\system volume information") != std::string::npos) return true;
#else
    (void)target;
#endif
    return false;
}

std::string ConnectFileBrowser::Base64Encode(const std::vector<unsigned char>& data) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    int val = 0;
    int valb = -6;
    for (unsigned char c : data) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(table[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(table[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

std::vector<unsigned char> ConnectFileBrowser::Base64Decode(const std::string& input) {
    static const std::string table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<int> decode(256, -1);
    for (int i = 0; i < 64; ++i) {
        decode[static_cast<unsigned char>(table[static_cast<size_t>(i)])] = i;
    }
    std::vector<unsigned char> out;
    int val = 0;
    int valb = -8;
    for (unsigned char c : input) {
        if (c == '=') break;
        if (decode[c] == -1) continue;
        val = (val << 6) + decode[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<unsigned char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

std::string ConnectFileBrowser::UploadKey(
    const std::string& sessionId, const std::string& transferId) {
    return sessionId + "|" + transferId;
}

void ConnectFileBrowser::Error(const std::string& sessionId,
    const std::string& requestType, const std::string& path,
    const std::string& error) {
    Send(json{
        {"type", requestType + "_error"},
        {"session_id", sessionId},
        {"path", path},
        {"error", error},
    });
}
void ConnectFileBrowser::List(
    const std::string& sessionId, const std::string& rawPath) {
    const std::string path = rawPath.empty() ? "/" : rawPath;
    try {
        json entries = json::array();
#ifdef _WIN32
        const bool listingDrives =
            path == "/" || path == "drives" || path.empty();
        if (listingDrives) {
            const DWORD mask = GetLogicalDrives();
            for (char letter = 'A'; letter <= 'Z'; ++letter) {
                if ((mask & (1u << (letter - 'A'))) == 0) continue;
                std::string root;
                root.push_back(letter);
                root += ":\\";
                entries.push_back({
                    {"name", root},
                    {"path", root},
                    {"is_dir", true},
                    {"isDir", true},
                    {"size", 0},
                });
            }
        } else
#endif
        {
            const fs::path target(path);
            if (!fs::exists(target) || !fs::is_directory(target)) {
                Error(sessionId, "remote_file_list", path, "Folder does not exist");
                return;
            }
            std::error_code ec;
            const fs::path parent = target.parent_path();
            if (!parent.empty() && parent != target) {
                entries.push_back({
                    {"name", ".."},
                    {"path", parent.string()},
                    {"is_dir", true},
                    {"isDir", true},
                    {"size", 0},
                });
            }
            for (const auto& de :
                fs::directory_iterator(target, fs::directory_options::skip_permission_denied, ec)) {
                const bool isDir = de.is_directory(ec);
                std::uintmax_t size = 0;
                if (!isDir) size = de.file_size(ec);
                entries.push_back({
                    {"name", de.path().filename().string()},
                    {"path", de.path().string()},
                    {"is_dir", isDir},
                    {"isDir", isDir},
                    {"size", size},
                });
            }
        }

        Send(json{
            {"type", "remote_file_list"},
            {"session_id", sessionId},
            {"path", path},
            {"computer_name", ComputerName()},
            {"hostname", ComputerName()},
            {"entries", entries},
        });
    } catch (const std::exception& ex) {
        Error(sessionId, "remote_file_list", path, ex.what());
    }
}

void ConnectFileBrowser::Download(
    const std::string& sessionId, const json& message) {
    const std::string path = message.value("path", std::string());
    constexpr std::uintmax_t maxInlineBytes = 512ull * 1024ull;
    constexpr std::size_t chunkBytes = 32u * 1024u;
    try {
        const fs::path target = ResolvePath(path);
        if (target.empty() || !fs::exists(target)) {
            Error(sessionId, "remote_file_download", path, "File does not exist");
            return;
        }
        if (!fs::is_regular_file(target)) {
            Error(sessionId, "remote_file_download", path, "Target is not a regular file");
            return;
        }
        if (IsProtectedPath(target)) {
            Error(sessionId, "remote_file_download", path,
                "Protected/system path cannot be downloaded");
            return;
        }

        const auto size = fs::file_size(target);
        if (size <= maxInlineBytes) {
            std::ifstream in(target, std::ios::binary);
            if (!in) {
                Error(sessionId, "remote_file_download", path,
                    "Could not open file for reading");
                return;
            }
            std::vector<unsigned char> bytes(static_cast<size_t>(size));
            if (!bytes.empty()) {
                in.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
            }
            Send(json{
                {"type", "remote_file_download"},
                {"session_id", sessionId},
                {"path", path},
                {"name", target.filename().string()},
                {"size", bytes.size()},
                {"encoding", "base64"},
                {"data", Base64Encode(bytes)},
            });
            return;
        }

        const std::string transferId =
            "dl_" + std::to_string(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
        Send(json{
            {"type", "file_transfer_start"},
            {"session_id", sessionId},
            {"transfer_id", transferId},
            {"direction", "download"},
            {"path", path},
            {"name", target.filename().string()},
            {"size", size},
            {"chunk_size", chunkBytes},
        });

        std::ifstream in(target, std::ios::binary);
        if (!in) {
            Send(json{
                {"type", "file_transfer_error"},
                {"session_id", sessionId},
                {"transfer_id", transferId},
                {"path", path},
                {"error", "Could not open file for chunked download"},
            });
            return;
        }

        std::vector<unsigned char> buffer(chunkBytes);
        std::uintmax_t offset = 0;
        int chunkIndex = 0;
        while (in && offset < size) {
            const auto remaining = size - offset;
            const size_t want = static_cast<size_t>(
                std::min<std::uintmax_t>(remaining, chunkBytes));
            in.read(reinterpret_cast<char*>(buffer.data()),
                static_cast<std::streamsize>(want));
            const size_t got = static_cast<size_t>(in.gcount());
            if (!got) break;
            std::vector<unsigned char> chunk(
                buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(got));
            Send(json{
                {"type", "file_transfer_chunk"},
                {"session_id", sessionId},
                {"transfer_id", transferId},
                {"direction", "download"},
                {"path", path},
                {"offset", offset},
                {"chunk_index", chunkIndex++},
                {"size", got},
                {"data", Base64Encode(chunk)},
            });
            offset += got;
        }

        Send(json{
            {"type", "file_transfer_complete"},
            {"session_id", sessionId},
            {"transfer_id", transferId},
            {"direction", "download"},
            {"path", path},
            {"size", offset},
        });
    } catch (const std::exception& ex) {
        Error(sessionId, "remote_file_download", path, ex.what());
    }
}
void ConnectFileBrowser::UploadInline(
    const std::string& sessionId, const json& message) {
    const std::string path = message.value("path", std::string());
    try {
        const fs::path target = ResolvePath(path);
        if (target.empty()) {
            Error(sessionId, "remote_file_upload", path, "Invalid target path");
            return;
        }
        if (IsProtectedPath(target)) {
            Error(sessionId, "remote_file_upload", path,
                "Protected/system path cannot be overwritten");
            return;
        }
        if (!target.parent_path().empty()) {
            fs::create_directories(target.parent_path());
        }
        const auto bytes = Base64Decode(
            message.value("data", std::string()));
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) {
            Error(sessionId, "remote_file_upload", path,
                "Could not open file for writing");
            return;
        }
        if (!bytes.empty()) {
            out.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        }
        out.close();
        Send(json{
            {"type", "remote_file_upload_complete"},
            {"session_id", sessionId},
            {"path", path},
            {"size", bytes.size()},
        });
        List(sessionId, target.parent_path().string());
    } catch (const std::exception& ex) {
        Error(sessionId, "remote_file_upload", path, ex.what());
    }
}

void ConnectFileBrowser::UploadStart(
    const std::string& sessionId, const json& message) {
    const std::string transferId =
        message.value("transfer_id", std::string());
    const std::string path = message.value("path", std::string());
    if (transferId.empty() || path.empty()) {
        Error(sessionId, "remote_file_upload", path,
            "Missing upload transfer id or target path");
        return;
    }
    try {
        const fs::path target = ResolvePath(path);
        if (target.empty() || IsProtectedPath(target)) {
            Error(sessionId, "remote_file_upload", path,
                "Invalid or protected target path");
            return;
        }
        if (!target.parent_path().empty()) {
            fs::create_directories(target.parent_path());
        }
        auto stream = std::make_unique<std::ofstream>(
            target, std::ios::binary | std::ios::trunc);
        if (!*stream) {
            Error(sessionId, "remote_file_upload", path,
                "Could not open file for chunked upload");
            return;
        }

        auto upload = std::make_unique<IncomingUpload>();
        upload->target = target;
        upload->stream = std::move(stream);
        upload->expected =
            message.value("size", static_cast<std::uint64_t>(0));
        {
            std::lock_guard<std::mutex> lock(uploadsMu_);
            uploads_[UploadKey(sessionId, transferId)] = std::move(upload);
        }
        Send(json{
            {"type", "remote_file_upload_started"},
            {"session_id", sessionId},
            {"transfer_id", transferId},
            {"path", path},
        });
    } catch (const std::exception& ex) {
        Error(sessionId, "remote_file_upload", path, ex.what());
    }
}

void ConnectFileBrowser::UploadChunk(
    const std::string& sessionId, const json& message) {
    const std::string transferId =
        message.value("transfer_id", std::string());
    const std::string path = message.value("path", std::string());
    const auto bytes = Base64Decode(
        message.value("data", std::string()));

    std::lock_guard<std::mutex> lock(uploadsMu_);
    auto it = uploads_.find(UploadKey(sessionId, transferId));
    if (it == uploads_.end() || !it->second || !it->second->stream) return;

    if (!bytes.empty()) {
        it->second->stream->write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    if (!*it->second->stream) {
        uploads_.erase(it);
        Error(sessionId, "remote_file_upload", path,
            "Failed while writing upload chunk");
        return;
    }
    it->second->received += static_cast<std::uint64_t>(bytes.size());
}

void ConnectFileBrowser::UploadComplete(
    const std::string& sessionId, const json& message) {
    const std::string transferId =
        message.value("transfer_id", std::string());
    const std::string path = message.value("path", std::string());

    fs::path target;
    std::uint64_t expected = 0;
    std::uint64_t received = 0;
    {
        std::lock_guard<std::mutex> lock(uploadsMu_);
        auto it = uploads_.find(UploadKey(sessionId, transferId));
        if (it == uploads_.end() || !it->second) {
            Error(sessionId, "remote_file_upload", path,
                "Upload transfer was not found");
            return;
        }
        target = it->second->target;
        expected = it->second->expected;
        received = it->second->received;
        if (it->second->stream) {
            it->second->stream->flush();
            it->second->stream->close();
        }
        uploads_.erase(it);
    }

    if (expected && expected != received) {
        std::error_code ec;
        fs::remove(target, ec);
        Error(sessionId, "remote_file_upload", path,
            "Upload size mismatch");
        return;
    }

    Send(json{
        {"type", "remote_file_upload_complete"},
        {"session_id", sessionId},
        {"transfer_id", transferId},
        {"path", path},
        {"size", received},
    });
    List(sessionId, target.parent_path().string());
}

void ConnectFileBrowser::UploadCancel(
    const std::string& sessionId, const json& message) {
    const std::string transferId =
        message.value("transfer_id", std::string());
    const std::string path = message.value("path", std::string());
    fs::path target;
    {
        std::lock_guard<std::mutex> lock(uploadsMu_);
        auto it = uploads_.find(UploadKey(sessionId, transferId));
        if (it == uploads_.end() || !it->second) return;
        target = it->second->target;
        if (it->second->stream) it->second->stream->close();
        uploads_.erase(it);
    }
    std::error_code ec;
    if (!target.empty()) fs::remove(target, ec);
    Send(json{
        {"type", "remote_file_upload_cancelled"},
        {"session_id", sessionId},
        {"transfer_id", transferId},
        {"path", path},
    });
}
void ConnectFileBrowser::DeletePath(
    const std::string& sessionId, const json& message) {
    const std::string path = message.value("path", std::string());
    try {
        const fs::path target = ResolvePath(path);
        if (target.empty() || !fs::exists(target)) {
            Error(sessionId, "remote_file_delete", path,
                "Path does not exist");
            return;
        }
        if (IsProtectedPath(target)) {
            Error(sessionId, "remote_file_delete", path,
                "Protected/system path cannot be deleted");
            return;
        }
        const std::uintmax_t removed =
            fs::is_directory(target)
                ? fs::remove_all(target)
                : (fs::remove(target) ? 1 : 0);
        Send(json{
            {"type", "remote_file_delete_complete"},
            {"session_id", sessionId},
            {"path", path},
            {"removed", removed},
        });
        List(sessionId, target.parent_path().string());
    } catch (const std::exception& ex) {
        Error(sessionId, "remote_file_delete", path, ex.what());
    }
}

void ConnectFileBrowser::MakeDirectory(
    const std::string& sessionId, const json& message) {
    const std::string path = message.value("path", std::string());
    try {
        const fs::path target = ResolvePath(path);
        if (target.empty() || IsProtectedPath(target)) {
            Error(sessionId, "remote_file_mkdir", path,
                "Invalid or protected folder path");
            return;
        }
        fs::create_directories(target);
        Send(json{
            {"type", "remote_file_mkdir_complete"},
            {"session_id", sessionId},
            {"path", path},
        });
        List(sessionId, target.parent_path().string());
    } catch (const std::exception& ex) {
        Error(sessionId, "remote_file_mkdir", path, ex.what());
    }
}

void ConnectFileBrowser::RenamePath(
    const std::string& sessionId, const json& message) {
    const std::string from = message.value("from", std::string());
    const std::string to = message.value("to", std::string());
    try {
        const fs::path src = ResolvePath(from);
        const fs::path dst = ResolvePath(to);
        if (src.empty() || dst.empty() || !fs::exists(src)) {
            Error(sessionId, "remote_file_rename", from,
                "Invalid source or target path");
            return;
        }
        if (IsProtectedPath(src) || IsProtectedPath(dst)) {
            Error(sessionId, "remote_file_rename", from,
                "Protected/system path cannot be renamed");
            return;
        }
        fs::rename(src, dst);
        Send(json{
            {"type", "remote_file_rename_complete"},
            {"session_id", sessionId},
            {"from", from},
            {"to", to},
        });
        List(sessionId, dst.parent_path().string());
    } catch (const std::exception& ex) {
        Error(sessionId, "remote_file_rename", from, ex.what());
    }
}

bool ConnectFileBrowser::Handle(
    const std::string& sessionId, const json& message) {
    const std::string type = message.value("type", std::string());
    if (type == "remote_file_list_request") {
        List(sessionId, message.value("path", std::string("/")));
        return true;
    }
    if (type == "remote_file_download_request") {
        Download(sessionId, message);
        return true;
    }
    if (type == "remote_file_upload_request") {
        UploadInline(sessionId, message);
        return true;
    }
    if (type == "remote_file_upload_start") {
        UploadStart(sessionId, message);
        return true;
    }
    if (type == "remote_file_upload_chunk") {
        UploadChunk(sessionId, message);
        return true;
    }
    if (type == "remote_file_upload_complete_request") {
        UploadComplete(sessionId, message);
        return true;
    }
    if (type == "remote_file_upload_cancel") {
        UploadCancel(sessionId, message);
        return true;
    }
    if (type == "remote_file_delete_request") {
        DeletePath(sessionId, message);
        return true;
    }
    if (type == "remote_file_mkdir_request") {
        MakeDirectory(sessionId, message);
        return true;
    }
    if (type == "remote_file_rename_request") {
        RenamePath(sessionId, message);
        return true;
    }
    return false;
}

void ConnectFileBrowser::CancelAll() {
    std::vector<fs::path> partials;
    {
        std::lock_guard<std::mutex> lock(uploadsMu_);
        for (auto& [_, upload] : uploads_) {
            if (!upload) continue;
            if (upload->stream) upload->stream->close();
            if (!upload->target.empty()) partials.push_back(upload->target);
        }
        uploads_.clear();
    }
    for (const auto& target : partials) {
        std::error_code ec;
        fs::remove(target, ec);
    }
}

} // namespace hi5
