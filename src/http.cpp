#include "ccs.hpp"

#include <cctype>
#include <mutex>

#include <curl/curl.h>

namespace {

std::once_flag g_curlInit;
void ensureCurlGlobal() {
  std::call_once(g_curlInit, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t writeBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

size_t writeHeader(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
  size_t len = size * nmemb;
  std::string line(ptr, len);
  size_t colon = line.find(':');
  if (colon != std::string::npos) {
    std::string name = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    for (char& c : name) c = (char)std::tolower((unsigned char)c);
    value = jx::trim(value);
    (*headers)[jx::trim(name)] = value;
  }
  return len;
}

HttpResponse perform(CURL* curl, HttpResponse& res, curl_slist* headerList,
                     const std::string* body) {
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res.body);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, writeHeader);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &res.headers);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "claude-cli/claude-code-multi-accounts");
  if (headerList) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
  if (body) {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body->size());
  }

  CURLcode rc = curl_easy_perform(curl);
  if (rc != CURLE_OK) {
    res.networkError = true;
    res.errorMessage = curl_easy_strerror(rc);
  } else {
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    res.status = code;
  }
  return res;
}

}  // namespace

HttpResponse httpPostJson(const std::string& url, const json& body,
                          const std::vector<std::string>& extraHeaders) {
  ensureCurlGlobal();
  HttpResponse res;
  CURL* curl = curl_easy_init();
  if (!curl) {
    res.networkError = true;
    res.errorMessage = "curl init failed";
    return res;
  }
  std::string payload = body.dump();
  curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  for (const auto& h : extraHeaders) headers = curl_slist_append(headers, h.c_str());

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  perform(curl, res, headers, &payload);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return res;
}

HttpResponse httpGet(const std::string& url, const std::vector<std::string>& headerLines) {
  ensureCurlGlobal();
  HttpResponse res;
  CURL* curl = curl_easy_init();
  if (!curl) {
    res.networkError = true;
    res.errorMessage = "curl init failed";
    return res;
  }
  curl_slist* headers = nullptr;
  for (const auto& h : headerLines) headers = curl_slist_append(headers, h.c_str());

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  perform(curl, res, headers, nullptr);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return res;
}
