# GPU Load Balancing (per-app)

## Goal
Let Wolf spread app containers across multiple GPUs instead of pinning the whole server to one.
Requirements:
- Per-app GPU assignment (not global).
- Users can **exclude** GPUs from the pool (e.g. an iGPU used for the display).
- Track **how many containers are using each GPU** and never send a new one to an already-full GPU when a
  lighter one is free (the weight decides how many a GPU counts as "full").
- Optional user-supplied **relative performance weight** per GPU (RTX 3090 vs 1660 → a 4:1 weight sends the
  first four apps to the 3090 and then keeps them at that 4:1 ratio).

> **Note (removed feature):** a per-app hard *pin* (`gpu_pin`) existed briefly but was removed. It was only
> useful at the top-level app and a session is stuck with its assigned GPU for its whole lifetime anyway,
> so load balancing + `excluded_gpus` covers the same ground without it. `GpuBalancer::pick()` therefore
> takes no pin argument; only the lobby's *soft* preference (`pick_preferring`) remains.

## Confirmed design decisions
- Weighting format: top-level `[gpus]` table mapping render-node path -> integer weight.
  Apps with no pin are balanced by `current_load / weight`.
- Lobbies: assign the GPU **once at lobby creation** (driven by the creating session's app); all
  joining clients share it; release when the lobby stops.

## Current behavior (what we're changing)
- `WOLF_RENDER_NODE` (default `/dev/dri/renderD128`) is the single global GPU, read in
  [`load_or_default`](src/moonlight-server/state/configTOML.cpp:268).
- Each app may override via `render_node` ([`BaseApp::render_node`](src/moonlight-server/state/serialised_config.hpp:122)),
  resolved in [`parse_apps`](src/moonlight-server/state/configTOML.cpp:144) and stored on
  [`events::App::render_node`](src/moonlight-server/events/events.hpp:72).
- That one node flows to the Wayland producer, the GStreamer encode pipeline, and the container
  (`linked_devices` + NVIDIA `--gpus all` / `NVIDIA_VISIBLE_DEVICES=all`) in
  [`start_runner`](src/moonlight-server/sessions/common.cpp:51) and [`RunDocker::run`](src/moonlight-server/runners/docker.cpp:86).
- Nothing counts per-GPU usage; nothing frees a GPU when an app exits.

## Architecture

```mermaid
flowchart TD
  C[config.toml] -->|gpus table + excluded_gpus| P[parse config]
  D[discover /dev/dri pool] --> BAL[GpuBalancer atom]
  P --> BAL
  SS[StreamSession event] -->|acquire app pin| BAL
  CL[CreateLobbyEvent] -->|acquire creating app pin| BAL
  BAL -->|chosen render_node| RUN[start_runner / docker runner]
  RUN -->|NVIDIA_VISIBLE_DEVICES + DeviceIDs scoped to chosen GPU| CTR[container]
  STOP[StopStreamEvent / StopLobbyEvent] -->|release| BAL
```

### New component: `GpuBalancer` (pure, testable)
Location: [`src/moonlight-server/state/gpu_balancer.hpp`](src/moonlight-server/state/gpu_balancer.hpp)
(auto-globbed by `state/*.cpp` / `*/*.hpp`, no CMake edit needed).

Held in `AppState` as `std::shared_ptr<immer::atom<GpuBalancer>>` (matches the immer/atom style used
by `running_sessions`, `lobbies`).

Data:
- `pool`: `map<render_node_path, GpuInfo{ weight (default 1), excluded (bool) }>`
- `usage`: `map<render_node_path, int /* active container count */>`

API (pure functions over an immutable snapshot; caller swaps in the new one via `atom->update`):
- `pick() -> optional<render_node>`
  - Among non-excluded GPUs, minimize the *result* of the assignment, `(usage[node] + 1) / weight[node]`;
    tie-break by lowest usage, then by highest weight, then stable path order. Return best.
  - Scoring the post-assignment load (`usage + 1`) rather than the current load is what makes the weight
    mean anything on an idle pool: `usage / weight` is `0` for every idle GPU, so the ratio could only
    ever show up after the fact and a burst of simultaneous starts always spilled one container onto the
    weak GPU immediately. With `(usage + 1) / weight` the choice reflects what the GPU would then be
    carrying, so a 4:1 pair really does take 4:1 and a 3–4× stronger GPU absorbs the first several apps.
  - The highest-weight tie-break still matters for the *first* pick, where every score is `1 / weight`:
    without it `renderD128` (weight 9) would beat `renderD129` (weight 10) purely because the path sorts first.
  - Trade-off worth knowing: `usage + 1` means an idle GPU no longer automatically wins, so a GPU that was
    just freed is not unconditionally preferred over an idle one — the weights can send the next container
    to the stronger GPU instead of back to the just-freed one. That is the intended price of honouring weights.
- `acquire(snapshot, node) -> snapshot'` (increments usage)
- `release(snapshot, node) -> snapshot'` (decrements usage, floor 0)

Construction: from the discovered `/dev/dri/renderD*` pool (reusing the iteration pattern in
[`get_nvidia_render_device`](tests/platforms/linux/nvidia.cpp:16)) merged with the `[gpus]` table for
weights/exclusions. The default `WOLF_RENDER_NODE` is always included so single-GPU setups keep working.

### Config model changes
- [`WolfConfig`](src/moonlight-server/state/serialised_config.hpp:140): add `std::map<std::string,int> gpus = {}`
  (render node -> weight). A GPU listed with a special sentinel or a parallel `excluded_gpus` list is
  excluded — simplest: keep `[gpus]` for weights and add `excluded_gpus = [...]` array.
- [`BaseApp`](src/moonlight-server/state/serialised_config.hpp:119): `render_node` is the only GPU-ish field.
- Bump `config_version` to 8 and extend the migration in
  [`load_or_default`](src/moonlight-server/state/configTOML.cpp:218) (new keys are optional/DefaultIfMissing, so old files load fine).
- Update the generated default at `state/default/config.v7.toml` with a commented `[gpus]` example.

### Wiring acquire/release
- **StreamSession**: in the [`StreamSession` handler](src/moonlight-server/sessions/moonlight.cpp:89),
  before starting the producer/runner, call `acquire(chosen)` for the node from
  `GpuBalancer::pick()`, store the chosen node on the
  session (new field `assigned_render_node`), and use it for both the Wayland producer and the runner
  args. On [`StopStreamEvent`](src/moonlight-server/sessions/moonlight.cpp:60), call `release`.
- **Lobby**: in the [`CreateLobbyEvent` handler](src/moonlight-server/sessions/lobbies.cpp:74), acquire once
  using the creating app's pin; store on the `Lobby`. Release in the
  [`StopLobbyEvent` handler](src/moonlight-server/sessions/lobbies.cpp:266). Joining clients do not re-acquire.
  - **GPU affinity (implemented)**: a lobby must render on the same GPU as the session that is driving it,
    otherwise the session's *already-created* encoder is fed frames from a different GPU and fails. The
    `CreateLobbyRequest` therefore carries an optional `session_id`; the API resolves that session's
    `assigned_render_node` (falling back to `app->render_node`) into a new
    `CreateLobbyEvent.preferred_render_node`. `MoonlightLobbyRuntime::assign_gpu` calls
    `GpuBalancer::pick_preferring(preferred_render_node)`, which uses the preferred node when it is
    available and otherwise logs a warning and falls back to normal load balancing.
  - **Inferring the driving session**: clients don't reliably send `session_id` — wolf-ui creates the
    lobby from inside the session it is running in, but only sends the id on *join*/`runners/start`, not
    on `/lobbies/create`. So when `session_id` is absent, `endpoint_LobbyCreate` falls back to
    `state::get_launcher_session_id`, which returns the **unique** running session whose app has
    `start_virtual_compositor` (i.e. the compositor-running session a lobby is created from). That session
    is then resolved exactly like an explicit `session_id`. If zero or more than one such session exists
    the request is genuinely ambiguous and the lobby load-balances as before.
  - **A shared desktop cannot emit device-local memory (multi-GPU)**: `preferred_render_node` only keeps
    the *creator* on the lobby's GPU. A *joining* session gets its own `assigned_render_node` from the
    balancer, and its encoder pipeline is built once at RTSP ANNOUNCE scoped to that node
    (`rtsp/commands.hpp::announce` -> `start_streaming_video` -> `scope_pipeline_to_node`). There is no
    re-negotiation on join, so a joiner on another GPU would read `CUDAMemory`/`DMABuf` produced by the
    lobby's compositor on the creator's GPU and fail (black screen). Since the pool can place joiners
    anywhere, the lobby's producer instead emits GPU-agnostic system memory when the pool has more than
    one *usable* node: `GpuBalancer::is_multi_gpu()` -> `MoonlightLobbyRuntime::producer_buffer_caps()`
    -> `wolf::platform::shared_desktop_producer_buffer_caps()`. The same caps are put in `RunnerArgs`
    (i.e. `WOLF_VIDEO_BUFFER_CAPS`) so the container agrees with the compositor. A session's *own*
    desktop keeps its configured caps (same node as its encoder, so still zero-copy).

### Per-GPU container scoping (the correctness crux)
Today [`RunDocker::run`](src/moonlight-server/runners/docker.cpp:86) sets `NVIDIA_VISIBLE_DEVICES=all` and
`DeviceIDs: all`. To actually isolate per GPU, when the assigned node is NVIDIA scope it to that one device:
- `NVIDIA_VISIBLE_DEVICES=<uuid-or-index>` derived from the chosen render node.
- `DeviceRequests[0].DeviceIDs = [<that device>]` instead of `"all"`.
For non-NVIDIA (AMD/Intel) the existing `linked_devices(node)` already passes only that GPU's nodes, so no change.
Deriving the NVIDIA index/uuid from a render node is a small helper in `platforms/hw_linux.cpp`
(`/proc/driver/nvidia/gpus/<bus>/information` already parsed by [`get_nvidia_node`](src/moonlight-server/platforms/hw_linux.cpp:31)).

## Testing
- New Catch2 file `tests/testGpuBalancer.cpp` (pure logic, no GPU needed):
  - weight-aware pick (3090 w=4 vs 1660 w=1 → the first three apps land on the 3090, the fourth spills to
    the 1660 because 4/4 ties with 1/1 and the least-loaded GPU wins the tie, then it settles at 4:1),
  - exclusion honored, pin override, pin to excluded errors,
  - acquire/release refcount reuse (app quits → next app reuses the freed GPU).
- Add `[gpus]` entries to [`tests/assets/config.test.toml`](tests/assets/config.test.toml) and assert parse.

## Docs
- [`docs/modules/user/pages/configuration.adoc`](docs/modules/user/pages/configuration.adoc): document `[gpus]`,
  `excluded_gpus`.
