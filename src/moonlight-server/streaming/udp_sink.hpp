#pragma once

#include <memory>

#include <boost/asio.hpp>
#include <core/batched_send.hpp>
#include <gst/gst.h>
#include <gstreamer-1.0/gst/app/gstappsink.h>
#include <streaming/pacing.hpp>

namespace streaming::udp_sink {

using boost::asio::ip::udp;

/**
 * The custom UDP sink used by the video/audio streaming pipelines.
 *
 * It pulls samples from a GStreamer appsink and forwards them to the client over UDP, batching
 * packets per frame and optionally pacing them (see `streaming/pacing.hpp`).
 */
struct UDPSink {
  std::shared_ptr<udp::socket> socket;
  std::shared_ptr<udp::endpoint> client_endpoint;
  pacing::Config pacing;
  pacing::State pacing_state;
  wolf::platform::batched_send_info_t send_info;
};

/**
 * Wire `udp_sink` up as the appsink callbacks for `appsink`.
 *
 * The sink is passed as user data, so it must outlive the pipeline.
 */
void configure_appsink(GstElement *appsink, UDPSink *udp_sink);

} // namespace streaming::udp_sink
