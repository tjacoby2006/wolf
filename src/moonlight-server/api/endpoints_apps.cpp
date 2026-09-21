#include <api/api.hpp>
#include <state/config.hpp>
#include <state/sessions.hpp>
#include <state/utils.hpp>

namespace wolf::api {

/*
 * App and profile endpoints.
 *
 * Split out of the single `endpoints.cpp` so each API domain lives in its own translation unit.
 */

void UnixSocketServer::endpoint_Apps(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto res = AppListResponse{.success = true};
  auto moonlight_profile = state::get_moonlight_profile(state_->app_state->config);
  if (!moonlight_profile) {
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Moonlight profile not found"}));
    return;
  }
  immer::vector<immer::box<events::App>> app_list = moonlight_profile.value()->apps->load();
  for (const immer::box<events::App> &app : app_list) {
    res.apps.push_back(rfl::Reflector<events::App>::from(app));
  }
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_AddApp(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto app = rfl::json::read<rfl::Reflector<events::App>::ReflType>(req.body);
  if (app) {
    auto profiles = state_->app_state->config->profiles->load().get();
    state::update_profiles(
        state_->app_state->config,
        profiles | //
            ranges::views::transform([app = app.value(), this](const immer::box<events::Profile> &profile) {
              if (profile->id == events::MOONLIGHT_PROFILE_ID) {
                profile->apps->update([app, this](auto &apps) {
                  return apps.push_back(rfl::Reflector<events::App>::to(app, this->state_->app_state->event_bus));
                });
              }
              return profile;
            }) |
            ranges::to<state::ProfilesList>());

    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, app.error().what());
    auto res = GenericErrorResponse{.error = app.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_RemoveApp(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto app = rfl::json::read<AppDeleteRequest>(req.body);
  if (app) {
    auto profiles = state_->app_state->config->profiles->load().get();
    state::update_profiles(
        state_->app_state->config,
        profiles | //
            ranges::views::transform([app = app.value(), this](const immer::box<events::Profile> &profile) {
              if (profile->id == events::MOONLIGHT_PROFILE_ID) {
                profile->apps->update([app, this](auto &apps) {
                  return apps | //
                         ranges::views::filter(
                             [&app](const immer::box<events::App> &a) { return a->base.id != app.id; }) | //
                         ranges::to<immer::vector<immer::box<events::App>>>();
                });
              }
              return profile;
            }) |
            ranges::to<state::ProfilesList>());

    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, app.error().what());
    auto res = GenericErrorResponse{.error = app.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_Profiles(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto profiles = state_->app_state->config->profiles->load().get();
  auto res = ProfileListResponse{.success = true,
                                 .profiles = profiles | //
                                             ranges::views::filter([](const immer::box<events::Profile> &p) {
                                               return p->id != events::MOONLIGHT_PROFILE_ID;
                                             }) |                                                              //
                                             ranges::views::transform(rfl::Reflector<events::Profile>::from) | //
                                             ranges::to_vector};
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_AddProfile(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto profile_req = rfl::json::read<rfl::Reflector<events::Profile>::ReflType>(req.body);
  if (profile_req) {
    auto p = profile_req.value();

    auto profiles = state_->app_state->config->profiles->load().get();
    state::update_profiles(
        state_->app_state->config,
        profiles.push_back(rfl::Reflector<events::Profile>::to(p, this->state_->app_state->event_bus)));
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, profile_req.error().what());
    auto res = GenericErrorResponse{.error = profile_req.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_RemoveProfile(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto profile_req = rfl::json::read<ProfileRemoveRequest>(req.body);
  if (profile_req) {
    auto p = profile_req.value();

    auto profiles = state_->app_state->config->profiles->load().get();
    state::update_profiles(state_->app_state->config,
                           profiles | //
                               ranges::views::remove_if([&p](const immer::box<events::Profile> &profile) {
                                 return profile.get().id == p.id;
                               }) | //
                               ranges::to<state::ProfilesList>());
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, profile_req.error().what());
    auto res = GenericErrorResponse{.error = profile_req.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

} // namespace wolf::api
