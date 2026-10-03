#include "execution_context.h"

#include "platform_info.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <grp.h>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

namespace hi5 {
namespace {

std::string normalizeRunAs(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (value == "system" || value == "admin" || value == "root") return "root";
    if (value == "user" || value == "current_user" || value == "current-user") return "user";
    return value;
}

std::string firstExistingShell(const std::vector<std::string>& candidates) {
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && !ec) return candidate;
    }
    return "/bin/sh";
}

ExecutionContext contextForUser(const std::string& username, const std::string& mode) {
    ExecutionContext out;
    out.mode = mode;
    out.username = username;

    if (username.empty()) {
        out.error = "No signed-in user is available.";
        return out;
    }

    long size = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (size < 1024) size = 16384;
    std::vector<char> buffer(static_cast<std::size_t>(size));
    struct passwd pwd {};
    struct passwd* result = nullptr;

    const int rc = getpwnam_r(
        username.c_str(),
        &pwd,
        buffer.data(),
        buffer.size(),
        &result);

    if (rc != 0 || !result) {
        out.error = "Could not resolve user account '" + username + "'.";
        return out;
    }

    out.uid = pwd.pw_uid;
    out.gid = pwd.pw_gid;
    out.home = pwd.pw_dir && *pwd.pw_dir ? pwd.pw_dir : "/";
    out.shell = pwd.pw_shell && *pwd.pw_shell ? pwd.pw_shell : "/bin/sh";

    int groupCount = 16;
#if defined(__APPLE__)
    std::vector<int> nativeGroups(static_cast<std::size_t>(groupCount));
    int groupsRc = getgrouplist(
        username.c_str(),
        static_cast<int>(out.gid),
        nativeGroups.data(),
        &groupCount);

    if (groupsRc == -1 && groupCount > 0) {
        nativeGroups.resize(static_cast<std::size_t>(groupCount));
        groupsRc = getgrouplist(
            username.c_str(),
            static_cast<int>(out.gid),
            nativeGroups.data(),
            &groupCount);
    }

    if (groupsRc >= 0 && groupCount >= 0) {
        nativeGroups.resize(static_cast<std::size_t>(groupCount));
        out.groups.clear();
        out.groups.reserve(nativeGroups.size());
        for (const int group : nativeGroups) {
            if (group >= 0) out.groups.push_back(static_cast<gid_t>(group));
        }
    }
#else
    out.groups.resize(static_cast<std::size_t>(groupCount));
    int groupsRc = getgrouplist(
        username.c_str(),
        out.gid,
        out.groups.data(),
        &groupCount);

    if (groupsRc == -1 && groupCount > 0) {
        out.groups.resize(static_cast<std::size_t>(groupCount));
        groupsRc = getgrouplist(
            username.c_str(),
            out.gid,
            out.groups.data(),
            &groupCount);
    }

    if (groupsRc >= 0 && groupCount >= 0) {
        out.groups.resize(static_cast<std::size_t>(groupCount));
    } else {
        out.groups.clear();
    }
#endif

    if (out.groups.empty()) out.groups.push_back(out.gid);

    if (out.groups.empty()) out.groups.push_back(out.gid);
    out.valid = true;
    return out;
}

} // namespace

std::string defaultRootShell() {
#if defined(__APPLE__)
    return firstExistingShell({"/bin/zsh", "/bin/bash", "/bin/sh"});
#else
    return firstExistingShell({"/bin/bash", "/usr/bin/bash", "/bin/zsh", "/bin/sh"});
#endif
}

ExecutionContext resolveExecutionContext(const std::string& runAs) {
    const std::string mode = normalizeRunAs(runAs.empty() ? "root" : runAs);

    if (mode == "current") {
        long size = sysconf(_SC_GETPW_R_SIZE_MAX);
        if (size < 1024) size = 16384;
        std::vector<char> buffer(static_cast<std::size_t>(size));
        struct passwd pwd {};
        struct passwd* result = nullptr;
        if (getpwuid_r(geteuid(), &pwd, buffer.data(), buffer.size(), &result) != 0 || !result) {
            ExecutionContext out;
            out.mode = "current";
            out.error = "Could not resolve current process identity.";
            return out;
        }
        ExecutionContext out = contextForUser(pwd.pw_name ? pwd.pw_name : "", "current");
        out.uid = geteuid();
        out.gid = getegid();
        out.groups.clear();
        int count = getgroups(0, nullptr);
        if (count > 0) {
            out.groups.resize(static_cast<std::size_t>(count));
            if (getgroups(count, out.groups.data()) < 0) out.groups.clear();
        }
        if (out.groups.empty()) out.groups.push_back(out.gid);
        out.valid = true;
        return out;
    }

    if (mode == "root") {
        ExecutionContext out;
        out.mode = "root";
        out.username = "root";
        out.uid = 0;
        out.gid = 0;
#if defined(__APPLE__)
        out.home = "/var/root";
#else
        out.home = "/root";
#endif
        out.shell = defaultRootShell();
        out.groups = {0};
        out.valid = true;
        return out;
    }

    if (mode == "user") {
        const auto username = activeUser();
        if (username.empty()) {
            ExecutionContext out;
            out.mode = "user";
            out.error = "No signed-in user is available on this endpoint.";
            return out;
        }
        return contextForUser(username, "user");
    }

    ExecutionContext out;
    out.mode = mode;
    out.error = "Execution context must be user or root.";
    return out;
}

bool applyExecutionContext(const ExecutionContext& context, std::string* error) {
    auto fail = [&](const std::string& prefix) {
        if (error) *error = prefix + ": " + std::strerror(errno);
        return false;
    };

    if (!context.valid) {
        if (error) *error = context.error.empty() ? "Invalid execution context." : context.error;
        return false;
    }

    if (geteuid() != 0 && context.uid != geteuid()) {
        if (error) *error = "Agent does not have permission to switch execution identity.";
        return false;
    }

    if (geteuid() == 0) {
        if (!context.groups.empty()) {
            if (setgroups(context.groups.size(), context.groups.data()) != 0) {
                return fail("setgroups failed");
            }
        }

        if (setgid(context.gid) != 0) return fail("setgid failed");
        if (setuid(context.uid) != 0) return fail("setuid failed");
    }

    if (!context.home.empty()) {
        (void)chdir(context.home.c_str());
    }

    return true;
}

} // namespace hi5
