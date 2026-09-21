#include <algorithm>
#include <cctype>
#include <boost/json.hpp>
#include <core/docker.hpp>
#include <curl/curl.h>
#include <docker/docker_client.hpp>
#include <helpers/logger.hpp>

namespace wolf::core::docker {
namespace json = boost::json;

void init() {
  curl_global_init(CURL_GLOBAL_ALL);
}

namespace {

std::string lower_copy(std::string_view value) {
  std::string lowered(value);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return lowered;
}

bool has_name_conflict_text(std::string_view value) {
  const auto lowered = lower_copy(value);
  return lowered.find("already in use") != std::string::npos && lowered.find("name") != std::string::npos;
}

} // namespace

bool is_container_name_conflict_response(long status_code, std::string_view response_body) {
  if (status_code == 409) {
    return true;
  }

  if (status_code != 500) {
    return false;
  }

  boost::system::error_code ec;
  const auto json = json::parse({response_body.data(), response_body.size()}, ec);
  if (!ec) {
    if (const auto obj = json.if_object()) {
      for (const auto field : {"message", "cause"}) {
        if (const auto value = obj->if_contains(field);
            value && value->is_string() && has_name_conflict_text(value->as_string())) {
          return true;
        }
      }
    }
  }

  return has_name_conflict_text(response_body);
}

/**
 * Initialise the curl handle and connects it to the docker socket
 */
std::optional<curl_ptr> docker_connect(const std::string &socket_path, bool debug) {
  if (auto curl = curl_easy_init()) {
    curl_easy_setopt(curl, CURLOPT_UNIX_SOCKET_PATH, socket_path.c_str()); // TODO: support also tcp://
    if (debug)
      curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    return curl_ptr(curl, ::curl_easy_cleanup);
  } else {
    return {};
  }
}

/**
 * Perform a HTTP request using curl
 */
std::optional<std::pair<long /* response_code */, std::string /* raw message */>>
req(CURL *handle,
    METHOD method,
    std::string_view target,
    std::string_view post_body,
    const std::vector<std::string> &header_params) {
  logs::log(logs::trace, "[CURL] Sending [{}] -> {}", (int)method, target);
  curl_easy_setopt(handle, CURLOPT_URL, target.data());

  /* Set method */
  switch (method) {
  case GET:
    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "GET");
    break;
  case POST:
    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "POST");
    break;
  case DELETE:
    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "DELETE");
    break;
  }
  curl_easy_setopt(handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

  struct curl_slist *headers = nullptr;
  for (const auto &header : header_params) {
    headers = curl_slist_append(headers, header.c_str());
  }

  /* Pass POST params (if present) */
  if (method == POST && !post_body.empty()) {
    logs::log(logs::trace, "[CURL] POST: {}", post_body);

    curl_easy_setopt(handle, CURLOPT_POST, 1L);
    headers = curl_slist_append(headers, "Transfer-Encoding: chunked");
    headers = curl_slist_append(headers, "Content-type: application/json");
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, post_body.data());
  }
  curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);

  /* Set custom writer (in order to receive back the response) */
  curl_easy_setopt(
      handle,
      CURLOPT_WRITEFUNCTION,
      static_cast<size_t (*)(char *, size_t, size_t, void *)>([](char *ptr, size_t size, size_t nmemb, void *read_buf) {
        *(static_cast<std::string *>(read_buf)) += std::string{ptr, size * nmemb};
        return size * nmemb;
      }));
  std::string read_buf;
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, &read_buf);

  /* Run! */
  auto res = curl_easy_perform(handle);
  curl_slist_free_all(headers);
  if (res != CURLE_OK) {
    logs::log(logs::warning, "[CURL] Request failed with error: {}", curl_easy_strerror(res));
    return {};
  } else {
    long response_code;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response_code);
    logs::log(logs::trace, "[CURL] Received {} - {}", response_code, read_buf);
    return {{response_code, read_buf}};
  }
}

} // namespace wolf::core::docker
