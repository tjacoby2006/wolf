#pragma once

#include <crypto/crypto.hpp>
#include <curl/curl.h>
#include <curl/easy.h>
#include <events/events.hpp>
#include <filesystem>
#include <functional>
#include <helpers/utils.hpp>
#include <immer/vector_transient.hpp>
#include <moonlight/control.hpp>
#include <moonlight/protocol.hpp>
#include <platforms/hw.hpp>
#include <range/v3/view.hpp>
#include <rest/helpers.hpp>
#include <rest/rest.hpp>
#include <rtp/udp-ping.hpp>
#include <state/config.hpp>
#include <state/sessions.hpp>
#include <state/utils.hpp>
#include <utility>

/**
 * Shared responses used across the REST workflows: the generic error/not-found writers,
 * the caller's host IP resolution and the `/serverinfo` payload (HTTP and HTTPS).
 */
namespace endpoints {

using namespace control;
using namespace wolf::core;

template <class T> void server_error(const std::shared_ptr<typename SimpleWeb::Server<T>::Response> &response) {
  XML xml;
  xml.put("root.<xmlattr>.status_code", 400);
  send_xml<T>(response, SimpleWeb::StatusCode::client_error_bad_request, xml);
}

template <class T>
void not_found(const std::shared_ptr<typename SimpleWeb::Server<T>::Response> &response,
               const std::shared_ptr<typename SimpleWeb::Server<T>::Request> &request) {
  log_req<T>(request);

  XML xml;
  xml.put("root.<xmlattr>.status_code", 404);
  send_xml<T>(response, SimpleWeb::StatusCode::client_error_not_found, xml);
}

template <class T>
std::string get_host_ip(const std::shared_ptr<typename SimpleWeb::Server<T>::Request> &request,
                        const immer::box<state::AppState> &state) {
  return state->host->internal_ip.value_or(request->local_endpoint().address().to_string());
}

template <class T>
void serverinfo(const std::shared_ptr<typename SimpleWeb::Server<T>::Response> &response,
                const std::shared_ptr<typename SimpleWeb::Server<T>::Request> &request,
                std::optional<events::StreamSession> stream_session,
                const immer::box<state::AppState> &state) {
  log_req<T>(request);

  SimpleWeb::CaseInsensitiveMultimap headers = request->parse_query_string();

  auto cfg = state->config;
  auto host = state->host;
  bool is_https = std::is_same_v<SimpleWeb::HTTPS, T>;

  bool is_busy = stream_session.has_value();
  int app_id = stream_session.has_value() ? std::stoi(stream_session->app->base.id) : 0;

  auto local_ip = get_host_ip<T>(request, state);

  auto xml = moonlight::serverinfo(is_busy,
                                   app_id,
                                   get_port(state::HTTPS_PORT),
                                   get_port(state::HTTP_PORT),
                                   cfg->uuid,
                                   cfg->hostname,
                                   utils::lazy_value_or(host->mac_address, [&]() { return get_mac_address(local_ip); }),
                                   local_ip,
                                   host->display_modes,
                                   is_https,
                                   cfg->support_hevc,
                                   cfg->support_av1);

  send_xml<T>(response, SimpleWeb::StatusCode::success_ok, xml);
}

} // namespace endpoints