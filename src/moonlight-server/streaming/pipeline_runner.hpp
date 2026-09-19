#pragma once

#include <functional>
#include <string>

#include <core/gstreamer.hpp>
#include <events/events.hpp>
#include <immer/array.hpp>
#include <immer/box.hpp>

namespace streaming {

/**
 * Parse, run and tear down a GStreamer pipeline on the calling thread.
 *
 * `on_pipeline_ready` is invoked once the pipeline is built but before it is set to PLAYING, so the
 * caller can attach bus handlers, look up elements by name, and register event-bus handlers. The
 * returned handlers are kept alive for the lifetime of the pipeline.
 *
 * Blocks until the pipeline reaches EOS or errors out, then tears it down. Returns false if the
 * pipeline could not be parsed.
 */
bool run_pipeline(
    const std::string &pipeline_desc,
    const std::function<immer::array<immer::box<wolf::core::events::EventBusHandlers>>(
        wolf::core::gstreamer::gst_element_ptr)>
        &on_pipeline_ready);

} // namespace streaming
