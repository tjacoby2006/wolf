#include <api/api.hpp>

namespace wolf::api {

/*
 * The HTTP route table for the Wolf control API.
 *
 * Extracted from the `UnixSocketServer` constructor so the constructor stays a short bootstrap and
 * the route table (which is long but purely declarative) lives on its own.
 */

void UnixSocketServer::register_routes() {
  state_->http.add(HTTPMethod::GET,
                   "/api/v1/events",
                   {.summary = "Subscribe to events",
                    .description = "This endpoint allows clients to subscribe to events using SSE",
                    // TODO: json_schema = rfl::json::to_schema<EventsVariant>()
                    .handler = [this](auto req, auto socket) { endpoint_Events(req, socket); }});

  /**
   * Pairing API
   */

  state_->http.add(
      HTTPMethod::GET,
      "/api/v1/pair/pending",
      {
          .summary = "Get pending pair requests",
          .description = "This endpoint returns a list of Moonlight clients that are currently waiting to be paired.",
          .response_description = {{200, {.json_schema = rfl::json::to_schema<PendingPairRequestsResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_PendingPairRequest(req, socket); },
      });

  state_->http.add(HTTPMethod::POST,
                   "/api/v1/pair/client",
                   {
                       .summary = "Pair a client",
                       .request_description = APIDescription{.json_schema = rfl::json::to_schema<PairRequest>()},
                       .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                                {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
                       .handler = [this](auto req, auto socket) { endpoint_Pair(req, socket); },
                   });

  state_->http.add(
      HTTPMethod::POST,
      "/api/v1/unpair/client",
      {
          .summary = "Unpair a client",
          .request_description = APIDescription{.json_schema = rfl::json::to_schema<UnpairClientRequest>()},
          .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                   {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_UnpairClient(req, socket); },
      });

  state_->http.add(HTTPMethod::GET,
                   "/api/v1/clients",
                   {
                       .summary = "Get paired clients",
                       .description = "This endpoint returns a list of all paired clients.",
                       .response_description = {{200, {.json_schema = rfl::json::to_schema<PairedClientsResponse>()}}},
                       .handler = [this](auto req, auto socket) { endpoint_PairedClients(req, socket); },
                   });

  state_->http.add(
      HTTPMethod::POST,
      "/api/v1/clients/settings",
      {
          .summary = "Update client settings",
          .description = "Update a client's settings including app state folder and client-specific settings",
          .request_description = APIDescription{.json_schema = rfl::json::to_schema<UpdateClientSettingsRequest>()},
          .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                   {400, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                   {404, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                   {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_UpdateClientSettings(req, socket); },
      });

  /**
   * Apps API
   */

  state_->http.add(
      HTTPMethod::GET,
      "/api/v1/apps",
      {
          .summary = "Get all Moonlight apps",
          .description = "This endpoint returns a list of all apps that will be shown in the Moonlight client.",
          .response_description = {{200, {.json_schema = rfl::json::to_schema<AppListResponse>()}},
                                   {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_Apps(req, socket); },
      });

  state_->http.add(HTTPMethod::POST,
                   "/api/v1/apps/add",
                   {
                       .summary = "Add a Moonlight app",
                       .request_description =
                           APIDescription{.json_schema = rfl::json::to_schema<rfl::Reflector<events::App>::ReflType>()},
                       .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                                {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
                       .handler = [this](auto req, auto socket) { endpoint_AddApp(req, socket); },
                   });

  state_->http.add(HTTPMethod::POST,
                   "/api/v1/apps/delete",
                   {.summary = "Remove a Moonlight app",
                    .request_description = APIDescription{.json_schema = rfl::json::to_schema<AppDeleteRequest>()},
                    .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                             {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
                    .handler = [this](auto req, auto socket) { endpoint_RemoveApp(req, socket); }});

  /**
   * Profiles API
   */

  state_->http.add(HTTPMethod::GET,
                   "/api/v1/profiles",
                   {
                       .summary = "Get all profiles",
                       .description = "This endpoint returns a list of all profiles.",
                       .response_description = {{200, {.json_schema = rfl::json::to_schema<ProfileListResponse>()}}},
                       .handler = [this](auto req, auto socket) { endpoint_Profiles(req, socket); },
                   });

  state_->http.add(
      HTTPMethod::POST,
      "/api/v1/profiles/add",
      {
          .summary = "Create a new profile",
          .request_description =
              APIDescription{.json_schema = rfl::json::to_schema<rfl::Reflector<events::Profile>::ReflType>()},
          .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                   {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_AddProfile(req, socket); },
      });

  state_->http.add(
      HTTPMethod::POST,
      "/api/v1/profiles/remove",
      {
          .summary = "Remove a profile",
          .request_description = APIDescription{.json_schema = rfl::json::to_schema<ProfileRemoveRequest>()},
          .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                   {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_RemoveProfile(req, socket); },
      });

  /**
   * Stream session API
   */

  state_->http.add(
      HTTPMethod::GET,
      "/api/v1/sessions",
      {
          .summary = "Get all stream sessions",
          .description = "This endpoint returns a list of all active stream sessions.",
          .response_description = {{200, {.json_schema = rfl::json::to_schema<StreamSessionListResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_StreamSessions(req, socket); },
      });

  state_->http.add(
      HTTPMethod::POST,
      "/api/v1/sessions/add",
      {
          .summary = "Create a new stream session",
          .request_description =
              APIDescription{.json_schema = rfl::json::to_schema<rfl::Reflector<events::StreamSession>::ReflType>()},
          .response_description = {{200, {.json_schema = rfl::json::to_schema<StreamSessionCreated>()}},
                                   {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_StreamSessionAdd(req, socket); },
      });

  state_->http.add(
      HTTPMethod::POST,
      "/api/v1/sessions/start",
      {
          .summary = "Start a stream session",
          .request_description = APIDescription{.json_schema = rfl::json::to_schema<StreamSessionStartRequest>()},
          .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                   {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_StreamSessionStart(req, socket); },
      });

  state_->http.add(
      HTTPMethod::POST,
      "/api/v1/sessions/pause",
      {
          .summary = "Pause a stream session",
          .request_description = APIDescription{.json_schema = rfl::json::to_schema<StreamSessionPauseRequest>()},
          .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                   {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_StreamSessionPause(req, socket); },
      });

  state_->http.add(
      HTTPMethod::POST,
      "/api/v1/sessions/stop",
      {
          .summary = "Stop a stream session",
          .request_description = APIDescription{.json_schema = rfl::json::to_schema<StreamSessionStopRequest>()},
          .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                   {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_StreamSessionStop(req, socket); },
      });

  state_->http.add(
      HTTPMethod::POST,
      "/api/v1/sessions/input",
      {
          .summary = "Handle input for a stream session",
          .request_description = APIDescription{.json_schema = rfl::json::to_schema<StreamSessionHandleInputRequest>()},
          .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                   {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_StreamSessionHandleInput(req, socket); },
      });

  state_->http.add(HTTPMethod::POST,
                   "/api/v1/runners/start",
                   {
                       .summary = "Start a runner in a given session",
                       .request_description = APIDescription{.json_schema = rfl::json::to_schema<RunnerStartRequest>()},
                       .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                                {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
                       .handler = [this](auto req, auto socket) { endpoint_RunnerStart(req, socket); },
                   });

  /**
   * Lobbies API
   */

  state_->http.add(HTTPMethod::GET,
                   "/api/v1/lobbies",
                   {
                       .summary = "List all lobbies",
                       .response_description = {{200, {.json_schema = rfl::json::to_schema<LobbiesResponse>()}},
                                                {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
                       .handler = [this](auto req, auto socket) { endpoint_Lobbies(req, socket); },
                   });

  state_->http.add(HTTPMethod::POST,
                   "/api/v1/lobbies/create",
                   {
                       .summary = "Create a new lobby",
                       .request_description = APIDescription{.json_schema = rfl::json::to_schema<CreateLobbyRequest>()},
                       .response_description = {{200, {.json_schema = rfl::json::to_schema<LobbyCreateResponse>()}},
                                                {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
                       .handler = [this](auto req, auto socket) { endpoint_LobbyCreate(req, socket); },
                   });

  state_->http.add(
      HTTPMethod::POST,
      "/api/v1/lobbies/join",
      {
          .summary = "Join a lobby",
          .request_description = APIDescription{.json_schema = rfl::json::to_schema<events::JoinLobbyEvent>()},
          .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                   {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_LobbyJoin(req, socket); },
      });

  state_->http.add(
      HTTPMethod::POST,
      "/api/v1/lobbies/leave",
      {
          .summary = "Leave a lobby",
          .request_description = APIDescription{.json_schema = rfl::json::to_schema<events::LeaveLobbyEvent>()},
          .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                   {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_LobbyLeave(req, socket); },
      });

  state_->http.add(
      HTTPMethod::POST,
      "/api/v1/lobbies/stop",
      {
          .summary = "Stop a lobby",
          .request_description = APIDescription{.json_schema = rfl::json::to_schema<events::StopLobbyEvent>()},
          .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                   {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_LobbyStop(req, socket); },
      });

  /**
   * Utils
   */
  state_->http.add(HTTPMethod::GET,
                   "/api/v1/utils/get-icon",
                   {.summary = "Get the icon for a given app",
                    .description = "Get the icon for a given app, pass the icon_path as a query parameter ex: "
                                   "/api/v1/utils/get-icon?icon_path=/etc/wolf/icons/steam.png",
                    .handler = [this](auto req, auto socket) { endpoint_GetIcon(req, socket); }});

  state_->http.add(
      HTTPMethod::GET,
      "/api/v1/docker/images/inspect",
      {.summary = "Inspect a Docker image",
       .description = "Inspect a Docker image and returns the full JSON response as is from the Docker APIs at "
                      "/images/{image_name}/json expects image_name as a query parameter.",
       .handler = [this](auto req, auto socket) { endpoint_DockerInspectImage(req, socket); }});

  state_->http.add(
      HTTPMethod::POST,
      "/api/v1/docker/images/pull",
      {.summary = "Pull a Docker image",
       .description = "Pull a Docker image, will keep the connection open to send back progress updates. Each "
                      "progress event will be a single line encoded as JSON.",
       .request_description = APIDescription{.json_schema = rfl::json::to_schema<DockerPullImageRequest>()},
       .response_description = {{200, {.json_schema = rfl::json::to_schema<DockerPullImageResponse>()}},
                                {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
       .handler = [this](auto req, auto socket) { endpoint_DockerPullImage(req, socket); }});

  /**
   * OpenAPI schema
   */

  state_->http.add(HTTPMethod::GET,
                   "/api/v1/openapi-schema",
                   {.summary = "Return this OpenAPI schema as JSON", .handler = [this](auto req, auto socket) {
                      send_http(socket, 200, state_->http.openapi_schema());
                    }});
}

} // namespace wolf::api