# Plan: GPU Load Balancing for Wolf

## Goal

Add a GPU pool with weighted session assignment, keeping rendering + encoding on the same GPU.
TOML gets a `[[gpus]]` array (`render_node` + `weight`); if empty, auto-detect all GPUs and balance
by least-assigned. Includes an `excluded_gpus` config item, full mixed-vendor support, and removal of
the per-app `render_node` pin.

## Key findings (current code)

- `WOLF_RENDER_NODE` -> `default_app_render_node`; `WOLF_ENCODER_NODE` -> `default_gst_render_node`
  (`configTOML.cpp:268-269`)
- Encoder + vendor chosen **once globally** at config load (`configTOML.cpp:250-330`); `get_encoder()`
  per vendor.
- `parse_apps` sets `app.render_node = app.render_node.value_or(default_app_render_node)`
  (`configTOML.cpp:150-200`)
- `events::App` holds `render_node` + h264/hevc/av1 pipelines + `video_producer_buffer_caps`
  (`events.hpp:60-78`)
- `create_stream_session` copies `run_app` into `session->app` (`state/sessions.hpp:70-121`)
- `moonlight.cpp:106` `start_video_producer(app->render_node)`; `:214-215` StartRunner render nodes
- `rtsp/commands.hpp:222` `VideoSession.render_node = session.app->render_node`; picks
  `app->{h264,hevc,av1}_gst_pipeline`
- `AppState.gst_context` is a **single global atom** (`data-structures.hpp:191`) -> must become
  per-GPU map
- `AppState.running_sessions` atom; StopStreamEvent handler removes session (`moonlight.cpp:60-68`)
- `resume()` carries over `wayland_display` -> **must keep same GPU** (`rest/endpoints.hpp:449-470`)
- `hw.hpp`/`hw_linux.cpp`: `linked_devices`, `get_vendor`, `get_render_node_name`; `hw_unknown.cpp`
  stubs
- `config_version = 7`; migration `version <= 6` in `load_or_default`

## Decisions (from user)

- **Algorithm**: pick GPU with max `(weight - assigned)`. Tie-break: fewest assigned, then config
  order.
  -> 3:1 yields `3090, 3090, 3060, 3090, 3060, 3090` (matches the "first two, then third" example)
- **TOML**: new top-level `[[gpus]]` with `render_node` + `weight`
- **Auto-detect** ALL GPUs when list empty, plus `excluded_gpus` config item
- **Full mixed-vendor** support (per-GPU encoder selection + per-GPU contexts)
- **Remove** per-app `render_node` pin

## Implementation steps

1. **Config model**: add `GpuConfig{render_node, weight=1}`, `WolfConfig.gpus`,
   `WolfConfig.excluded_gpus`; remove `BaseApp.render_node`; bump `config_version` 7->8.
2. **New `state/gpu.hpp`**: `Gpu` (render_node, weight, vendor, per-GPU pipelines/params/buffer caps,
   support_hevc/av1, use_zero_copy), `GpuPool`, `GpuAssignments{counts, last_assigned}`, `pick_gpu`,
   `assign_gpu`, `release_gpu` (immer atom update returns new value -> safe).
3. **`platforms/hw.hpp` + `hw_linux.cpp` + `hw_unknown.cpp`**: add `list_render_nodes()` enumerating
   `/dev/dri/renderD*`.
4. **`configTOML.cpp`**: build pool (`cfg.gpus` or auto-detect minus `excluded_gpus`); per-GPU vendor
   + encoder selection + zero-copy caps; remove per-app `render_node`; migration v7->v8 that logs a
   warning listing dropped per-app `render_node` pins; drop `render_node` from `update_profiles`.
5. **`data-structures.hpp`**: `AppState` gains `immer::box<GpuPool> gpus` +
   `std::shared_ptr<immer::atom<GpuAssignments>> gpu_assignments`; `gst_context` ->
   `immer::atom<immer::map<std::string, gst_context_ptr>>`.
6. **`events.hpp`**: add `StreamSession.render_node`; add `App.support_hevc/support_av1`
   (per-session resolved). Update `reflectors.hpp`.
7. **`state/sessions.hpp`**: `create_stream_session` assigns GPU, overrides app
   pipelines/render_node/buffer caps/support flags from assigned `Gpu`.
8. **`streaming.cpp/.hpp`**: per-GPU context map lookup/create in `need_context_handler`; update
   `NeedContextData`.
9. **`moonlight.cpp`**: release GPU on StopStreamEvent; pass context map. **`lobbies.cpp`**:
   assign/release GPU for lobbies via the balancer (ignore client-supplied render nodes); context map.
10. **`rest/endpoints.hpp` `resume()`**: reuse old session's GPU (no re-balance); `api/endpoints.cpp`
    dummy app cleanup.
11. **`rtsp/commands.hpp`**: codec fallback if assigned GPU lacks HEVC/AV1.
12. **`wolf.cpp`**: init `AppState` gpus + gpu_assignments.
13. **Tests**: new balancer test (3:1 sequence), update `testMoonlight.cpp`, `config.test.toml`
    (v8, remove render_node), `testWolfAPI.cpp`.
14. **Docs**: `configuration.adoc`, `quickstart.adoc`, `spec.json`.

## Resolved (user chose Option A on all)

- **Lobbies**: route through the same balancer; ignore client-supplied wayland/runner render nodes.
- **Migration**: warn + drop per-app `render_node` pins (log a warning listing dropped pins).
- **spec.json**: regenerate from `openapi.cpp` (do not hand-edit).
