#pragma once

#include <rest/endpoints/common.hpp>

/**
 * The Hercules pairing workflow: the four-phase handshake driven over HTTP (client cert ->
 * AES key -> server challenge -> client hash -> pairing secret) plus the final HTTPS pin
 * confirmation. State between phases lives in `AppState::pairing_cache`, keyed by
 * `<uniqueid>@<client_ip>` so concurrent clients can pair at the same time.
 */
namespace endpoints {

using namespace control;
using namespace wolf::core;

void remove_pair_session(const immer::box<state::AppState> &state, const std::string &cache_key) {
  state->pairing_cache->update([&cache_key](const immer::map<std::string, state::PairCache> &pairing_cache) {
    return pairing_cache.erase(cache_key);
  });
}

XML fail_pair(const std::string &status_msg) {
  logs::log(logs::warning, "Failed pairing: {}", status_msg);

  XML tree;
  tree.put("root.paired", 0);
  tree.put("root.<xmlattr>.status_code", 400);
  tree.put("root.<xmlattr>.status_message", status_msg);

  return tree;
}

struct XMLResult {
  SimpleWeb::StatusCode status;
  XML xml;
};

std::shared_ptr<boost::promise<XMLResult>> pair_phase1(const immer::box<state::AppState> &state,
                                                       const std::string &client_ip,
                                                       const std::string &host_ip,
                                                       const std::string &client_cert_str,
                                                       const std::string &salt,
                                                       const std::string &cache_key) {
  auto future_result = std::make_shared<boost::promise<XMLResult>>();
  if (state->pairing_cache->load()->find(cache_key)) {
    future_result->set_value(
        {SimpleWeb::StatusCode::client_error_bad_request, fail_pair("Out of order pair request (phase 1)")});
    remove_pair_session(state, cache_key);
    return future_result;
  }

  auto future_pin = std::make_shared<boost::promise<std::string>>();
  state->event_bus->fire_event( // Emit a signal and wait for the promise to be fulfilled
      immer::box<events::PairSignal>(
          events::PairSignal{.client_ip = client_ip, .host_ip = host_ip, .user_pin = future_pin}));

  future_pin->get_future().then(
      [state, salt, client_cert_str, cache_key, future_result](boost::future<std::string> fut_pin) {
        auto server_pem = x509::get_cert_pem(state->host->server_cert);
        auto result = moonlight::pair::get_server_cert(fut_pin.get(), salt, server_pem);

        auto client_cert_parsed = crypto::hex_to_str(client_cert_str, true);

        state->pairing_cache->update([&](const immer::map<std::string, state::PairCache> &pairing_cache) {
          return pairing_cache.set(cache_key,
                                   {.client_cert = client_cert_parsed,
                                    .aes_key = result.second,
                                    .last_phase = state::PAIR_PHASE::GETSERVERCERT});
        });

        future_result->set_value({SimpleWeb::StatusCode::success_ok, result.first});
      });

  return future_result;
}

XMLResult pair_phase2(const immer::box<state::AppState> &state,
                      state::PairCache &client_cache,
                      const std::string &client_challenge,
                      const std::string &cache_key) {
  if (client_cache.last_phase != state::PAIR_PHASE::GETSERVERCERT) {
    return {SimpleWeb::StatusCode::client_error_bad_request, fail_pair("Out of order pair request (phase 2)")};
  }
  client_cache.last_phase = state::PAIR_PHASE::CLIENTCHALLENGE;

  auto server_cert_signature = x509::get_cert_signature(state->host->server_cert);
  auto [xml, server_secret_pair] =
      moonlight::pair::send_server_challenge(client_cache.aes_key, client_challenge, server_cert_signature);

  auto [server_secret, server_challenge] = server_secret_pair;
  client_cache.server_secret = server_secret;
  client_cache.server_challenge = server_challenge;
  state->pairing_cache->update([&](const immer::map<std::string, state::PairCache> &pairing_cache) {
    return pairing_cache.set(cache_key, client_cache);
  });

  return {SimpleWeb::StatusCode::success_ok, xml};
}

XMLResult pair_phase3(const immer::box<state::AppState> &state,
                      state::PairCache &client_cache,
                      const std::string &server_challenge,
                      const std::string &cache_key) {
  if (client_cache.last_phase != state::PAIR_PHASE::CLIENTCHALLENGE) {
    return {SimpleWeb::StatusCode::client_error_bad_request, fail_pair("Out of order pair request (phase 3)")};
  }
  client_cache.last_phase = state::PAIR_PHASE::SERVERCHALLENGERESP;

  auto [xml, client_hash] = moonlight::pair::get_client_hash(client_cache.aes_key,
                                                             client_cache.server_secret.value(),
                                                             server_challenge,
                                                             x509::get_pkey_content(state->host->server_pkey));
  client_cache.client_hash = client_hash;
  state->pairing_cache->update([&](const immer::map<std::string, state::PairCache> &pairing_cache) {
    return pairing_cache.set(cache_key, client_cache);
  });
  return {SimpleWeb::StatusCode::success_ok, xml};
}

XMLResult pair_phase4(state::PairCache &client_cache, const std::string &client_secret) {
  if (client_cache.last_phase != state::PAIR_PHASE::SERVERCHALLENGERESP) {
    return {SimpleWeb::StatusCode::client_error_bad_request, fail_pair("Out of order pair request (phase 4)")};
  }
  client_cache.last_phase = state::PAIR_PHASE::CLIENTPAIRINGSECRET;

  auto client_cert = x509::cert_from_string(client_cache.client_cert);

  if (!client_cert) {
    return {SimpleWeb::StatusCode::client_error_bad_request, fail_pair("Unable to parse client certificate")};
  }

  auto client_sig = x509::get_cert_signature(client_cert);
  auto public_key = x509::get_cert_public_key(client_cert);
  auto xml = moonlight::pair::client_pair(client_cache.aes_key,
                                          client_cache.server_challenge.value(),
                                          client_cache.client_hash.value(),
                                          client_secret,
                                          client_sig,
                                          public_key);

  auto is_paired = xml.get<int>("root.paired") == 1;
  return {is_paired ? SimpleWeb::StatusCode::success_ok : SimpleWeb::StatusCode::client_error_bad_request, xml};
}

/**
 * The unauthenticated HTTP endpoint multiplexing the pairing phases: it inspects the query
 * headers to decide which phase the client is in (each phase is attempted only when its
 * header is present, so a client can retry from any point).
 */
void pair(const std::shared_ptr<typename SimpleWeb::Server<SimpleWeb::HTTP>::Response> &response,
          const std::shared_ptr<typename SimpleWeb::Server<SimpleWeb::HTTP>::Request> &request,
          const immer::box<state::AppState> &state) {
  log_req<SimpleWeb::HTTP>(request);

  SimpleWeb::CaseInsensitiveMultimap headers = request->parse_query_string();
  auto salt = get_header(headers, "salt");
  auto client_cert_str = get_header(headers, "clientcert");
  auto client_id = get_header(headers, "uniqueid");
  auto client_ip = request->remote_endpoint().address().to_string();

  if (!client_id) {
    send_xml<SimpleWeb::HTTP>(response,
                              SimpleWeb::StatusCode::client_error_bad_request,
                              fail_pair("Received pair request without uniqueid, stopping."));
    return;
  }

  /* client_id is hardcoded in Moonlight, we add the IP so that different users can pair at the same time */
  auto cache_key = client_id.value() + "@" + client_ip;

  // PHASE 1
  if (client_id && salt && client_cert_str) {
    auto future_result = pair_phase1(state,
                                     client_ip,
                                     get_host_ip<SimpleWeb::HTTP>(request, state),
                                     client_cert_str.value(),
                                     salt.value(),
                                     cache_key);
    future_result->get_future().then([response](boost::future<XMLResult> result) {
      auto [status, xml] = result.get();
      send_xml<SimpleWeb::HTTP>(response, status, xml);
    });
    return;
  }

  auto client_cache_it = state->pairing_cache->load()->find(cache_key);
  if (client_cache_it == nullptr) {
    send_xml<SimpleWeb::HTTP>(
        response,
        SimpleWeb::StatusCode::client_error_bad_request,
        fail_pair(fmt::format("Unable to find {} {} in the pairing cache", client_id.value(), client_ip)));
    return;
  }
  auto client_cache = *client_cache_it;

  // PHASE 2
  auto client_challenge = get_header(headers, "clientchallenge");
  if (client_challenge) {
    auto [status, xml] = pair_phase2(state, client_cache, client_challenge.value(), cache_key);
    send_xml<SimpleWeb::HTTP>(response, status, xml);
    if (status != SimpleWeb::StatusCode::success_ok) {
      remove_pair_session(state, cache_key); // security measure, remove the session if the pairing failed
    }
    return;
  }

  // PHASE 3
  auto server_challenge = get_header(headers, "serverchallengeresp");
  if (server_challenge && client_cache.server_secret) {
    auto [status, xml] = pair_phase3(state, client_cache, server_challenge.value(), cache_key);
    send_xml<SimpleWeb::HTTP>(response, status, xml);
    if (status != SimpleWeb::StatusCode::success_ok) {
      remove_pair_session(state, cache_key); // security measure, remove the session if the pairing failed
    }
    return;
  }

  // PHASE 4
  auto client_secret = get_header(headers, "clientpairingsecret");
  if (client_secret && client_cache.server_challenge && client_cache.client_hash) {
    auto [status, xml] = pair_phase4(client_cache, client_secret.value());
    send_xml<SimpleWeb::HTTP>(response, status, xml);

    if (status == SimpleWeb::StatusCode::success_ok) {
      state::pair(
          state->config,
          state::PairedClient{.client_cert = client_cache.client_cert,
                              .app_state_folder = std::to_string(std::hash<std::string>{}(client_cache.client_cert))});
      logs::log(logs::info, "Succesfully paired {}", client_ip);
    } else {
      logs::log(logs::warning, "Failed pairing with {}", client_ip);
    }

    remove_pair_session(state, cache_key); // Either case, this session is done
    return;
  }

  logs::log(logs::warning, "Unable to match pair with any phase, you can retry pairing from Moonlight");
}

namespace https {

/**
 * The last pairing step. The check here is implicit: by running over HTTPS
 * the client certificate has already been verified.
 */
void pair(const std::shared_ptr<typename SimpleWeb::Server<SimpleWeb::HTTPS>::Response> &response,
          const std::shared_ptr<typename SimpleWeb::Server<SimpleWeb::HTTPS>::Request> &request) {
  SimpleWeb::CaseInsensitiveMultimap headers = request->parse_query_string();
  auto phrase = get_header(headers, "phrase");
  // PHASE 5 (over HTTPS)
  if (phrase && phrase.value() == "pairchallenge") {
    XML xml;

    xml.put("root.paired", 1);
    xml.put("root.<xmlattr>.status_code", 200);

    send_xml<SimpleWeb::HTTPS>(response, SimpleWeb::StatusCode::success_ok, xml);
  }
}

} // namespace https

} // namespace endpoints