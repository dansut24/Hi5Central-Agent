#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

namespace hi5 {

struct ExecutionContext {
    std::string mode;
    std::string username;
    std::string home;
    std::string shell;
    uid_t uid = 0;
    gid_t gid = 0;
    std::vector<gid_t> groups;
    bool valid = false;
    std::string error;
};

ExecutionContext resolveExecutionContext(const std::string& runAs);
bool applyExecutionContext(const ExecutionContext& context, std::string* error = nullptr);
std::string defaultRootShell();

} // namespace hi5
