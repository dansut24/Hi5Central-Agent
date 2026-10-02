#include "http_client.h"

#include <curl/curl.h>

#include <stdexcept>

namespace hi5 {

HttpClient::HttpClient() {
    const auto rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (rc != CURLE_OK) {
        throw std::runtime_error("curl_global_init failed");
    }
}

HttpClient::~HttpClient() {
    curl_global_cleanup();
}

size_t HttpClient::writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t bytes = size * nmemb;
    if (!userdata || !ptr || bytes == 0) return bytes;
    static_cast<std::string*>(userdata)->append(ptr, bytes);
    return bytes;
}

HttpResponse HttpClient::postJson(
    const std::string& url,
    const std::string& body,
    const std::map<std::string, std::string>& headers) const {

    HttpResponse result;
    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error = "curl_easy_init failed";
        return result;
    }

    struct curl_slist* headerList = nullptr;
    headerList = curl_slist_append(headerList, "Content-Type: application/json");
    headerList = curl_slist_append(headerList, "Accept: application/json");
    for (const auto& [key, value] : headers) {
        headerList = curl_slist_append(headerList, (key + ": " + value).c_str());
    }

    char errorBuffer[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Hi5CentralAgent/portable");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &HttpClient::writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);

    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        result.error = errorBuffer[0] ? errorBuffer : curl_easy_strerror(rc);
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    }

    curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);
    return result;
}

} // namespace hi5
