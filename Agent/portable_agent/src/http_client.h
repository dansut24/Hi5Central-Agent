#pragma once

#include <map>
#include <string>

namespace hi5 {

struct HttpResponse {
    long status = 0;
    std::string body;
    std::string error;

    bool ok() const { return status >= 200 && status < 300; }
};

class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    HttpResponse postJson(
        const std::string& url,
        const std::string& body,
        const std::map<std::string, std::string>& headers = {}) const;

    HttpResponse getJson(
        const std::string& url,
        const std::map<std::string, std::string>& headers = {}) const;

private:
    static size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata);
};

} // namespace hi5
