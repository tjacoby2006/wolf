#include <streaming/gst_bus.hpp>

#include <helpers/logger.hpp>
#include <streaming/streaming.hpp>

namespace streaming::gst_bus {

namespace {

gboolean structure_each(GQuark field_id, const GValue *value, gpointer user_data) {
  auto field_str = std::string(g_quark_to_string(field_id));
  if (!G_VALUE_HOLDS_STRING(value)) {
    logs::log(logs::warning, "Wayland source message: {} = {}", field_str, "not a string");
    return FALSE;
  }
  auto value_str = g_value_get_string(value);
  logs::log(logs::debug, "Wayland source message: {} = {}", field_str, value_str);

  if (field_str == "WAYLAND_DISPLAY") {
    logs::log(logs::info, "Wayland display ready, listening on: {}", value_str);
    auto bus_data = static_cast<WaylandBusData *>(user_data);
    bus_data->on_ready->set_value(
        WaylandDisplayReady{.wayland_socket_name = value_str, .wayland_plugin = bus_data->wayland_plugin});
  }

  return TRUE;
}

} // namespace

void application_message_handler(GstBus *bus, GstMessage *msg, gpointer data) {
  auto structure = gst_message_get_structure(msg);
  if (gst_structure_has_name(structure, "wayland.src")) {
    gst_structure_foreach(structure, structure_each, data);
  }
}

void need_context_handler(GstBus *bus, GstMessage *msg, gpointer data) {
  auto ctx_data = static_cast<ContextBusData *>(data);
  // A CUDA context is bound to a single GPU, so look up (and cache) the context for this render node.
  if (auto gst_context = ctx_data->gst_contexts->load()->find(ctx_data->device_path)) {
    logs::log(logs::debug, "Context already set for {}, passing it to the pipeline.", ctx_data->device_path);
    gst_video_context::set_context(*gst_context, msg);
  } else if (auto video_context = gst_video_context::need_context_for_device(ctx_data->device_path, msg)) {
    ctx_data->gst_contexts->update([device_path = ctx_data->device_path, video_context](const auto &contexts) {
      return contexts.set(device_path, video_context);
    });
  }
}

GstBusSyncReply bus_sync_handler(GstBus *bus, GstMessage *msg, gpointer data) {
  if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_NEED_CONTEXT) {
    need_context_handler(bus, msg, data);
  }
  return GST_BUS_PASS;
}

} // namespace streaming::gst_bus
