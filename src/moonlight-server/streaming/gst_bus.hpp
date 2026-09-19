#pragma once

#include <memory>
#include <string>

#include <boost/thread/future.hpp>
#include <core/gstreamer.hpp>
#include <gst-video-context.hpp>
#include <gst/gst.h>
#include <immer/atom.hpp>
#include <immer/map.hpp>

namespace streaming {

struct WaylandDisplayReady;

namespace gst_bus {

/**
 * State passed to the `wayland.src` application-message handler.
 *
 * When the compositor reports its `WAYLAND_DISPLAY`, the promise is fulfilled with the socket
 * name and the plugin element (so input events can be sent to it directly).
 */
struct WaylandBusData {
  std::shared_ptr<boost::promise<WaylandDisplayReady>> on_ready;
  wolf::core::gstreamer::gst_element_ptr wayland_plugin;
};

/**
 * State passed to the NEED_CONTEXT sync handler.
 *
 * A CUDA context is bound to a single GPU, so the context is looked up (and cached) per render node.
 */
struct ContextBusData {
  std::string device_path;
  std::shared_ptr<immer::atom<immer::map<std::string, gst_video_context::gst_context_ptr>>> gst_contexts;
};

/**
 * Handle a `wayland.src` application message: fulfil `data->on_ready` when the compositor reports
 * its `WAYLAND_DISPLAY`.
 */
void application_message_handler(GstBus *bus, GstMessage *msg, gpointer data);

/**
 * Handle a NEED_CONTEXT message: attach the cached CUDA context for the render node, or create and
 * cache a new one.
 */
void need_context_handler(GstBus *bus, GstMessage *msg, gpointer data);

/**
 * Sync bus handler: routes NEED_CONTEXT messages to `need_context_handler`, passes everything else
 * through.
 */
GstBusSyncReply bus_sync_handler(GstBus *bus, GstMessage *msg, gpointer data);

} // namespace gst_bus
} // namespace streaming
