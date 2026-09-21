#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <curl/curl.h>

namespace wolf::core::docker {

/*
 * Internal curl plumbing shared by the Docker API implementation files.
 *
 * These were previously file-local to `docker.cpp`; they live here so the API methods can be split
 * across translation units while sharing one HTTP client.
 */

enum METHOD : int {
  GET,
  POST,
  DELETE
};

using curl_ptr = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;

/**
 * Initialise the curl handle and connect it to the docker socket.
 */
std::optional<curl_ptr> docker_connect(const std::string &socket_path, bool debug = false);

/**
 * Perform an HTTP request using curl.
 * Returns the response code and raw body, or nullopt on a transport error.
 */
std::optional<std::pair<long /* response_code */, std::string /* raw message */>>
req(CURL *handle,
    METHOD method,
    std::string_view target,
    std::string_view post_body = {},
    const std::vector<std::string> &header_params = {});

} // namespace wolf::core::docker
