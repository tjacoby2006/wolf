#include <streaming/udp_sink.hpp>

#include <chrono>
#include <thread>
#include <vector>

#include <helpers/logger.hpp>

namespace streaming::udp_sink {

namespace {

void ensure_socket_open(UDPSink *udp_sink, bool is_video) {
  if (!udp_sink->socket->is_open()) {
    logs::log(logs::warning, "UDP Socket is not open");
    udp_sink->socket->open(udp::v4());
    wolf::platform::configure_socket_for_streaming(*udp_sink->socket, is_video);
    wolf::platform::enable_socket_qos(udp_sink->socket->native_handle(), is_video);
  }
}

GstFlowReturn send_buffer_batched(GstBufferList *buffer_list, UDPSink *udp_sink) {
  guint num_buffers = gst_buffer_list_length(buffer_list);
  if (num_buffers == 0) {
    return GST_FLOW_OK;
  }

  ensure_socket_open(udp_sink, true);

  std::vector<std::pair<GstBuffer *, GstMapInfo>> mapped_buffers;
  udp_sink->send_info.payload_buffers.resize(num_buffers);
  mapped_buffers.reserve(num_buffers);

  for (guint i = 0; i < num_buffers; i++) {
    GstBuffer *buffer = gst_buffer_list_get(buffer_list, i);

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
      logs::log(logs::error, "Failed to map buffer {} in batch", i);
      for (auto &[mapped_buffer, m] : mapped_buffers) {
        gst_buffer_unmap(mapped_buffer, &m);
      }
      return GST_FLOW_ERROR;
    }
    mapped_buffers.emplace_back(buffer, map);
    udp_sink->send_info.payload_buffers[i] =
        wolf::platform::buffer_descriptor_t(reinterpret_cast<const char *>(map.data), map.size);
  }

  udp_sink->send_info.native_socket = udp_sink->socket->native_handle();
  udp_sink->send_info.target_address = udp_sink->client_endpoint->address();
  udp_sink->send_info.target_port = udp_sink->client_endpoint->port();

  bool success = true;

  if (!udp_sink->pacing.enabled || num_buffers <= udp_sink->pacing.max_batch_size) {
    udp_sink->send_info.block_offset = 0;
    udp_sink->send_info.block_count = num_buffers;
    success = wolf::platform::send_batch(udp_sink->send_info);
  } else {
    // The schedule is computed by a pure function (see streaming/pacing.hpp); this loop only
    // performs the syscalls and the sleeps.
    auto plan =
        pacing::plan_frame(udp_sink->pacing, udp_sink->pacing_state, num_buffers, std::chrono::steady_clock::now());
    for (const auto &batch : plan.batches) {
      if (batch.count == 0) {
        // Window boundary: wait until the next window opens.
        auto now = std::chrono::steady_clock::now();
        if (now < batch.due) {
          std::this_thread::sleep_until(batch.due);
        }
        continue;
      }

      udp_sink->send_info.block_offset = batch.offset;
      udp_sink->send_info.block_count = batch.count;
      if (!wolf::platform::send_batch(udp_sink->send_info)) {
        success = false;
        break;
      }
    }
    udp_sink->pacing_state = plan.next_state;
  }

  for (auto &[buffer, m] : mapped_buffers) {
    gst_buffer_unmap(buffer, &m);
  }

  if (!success) {
    logs::log(logs::warning, "Failed to send batch of {} packets", num_buffers);
    return GST_FLOW_ERROR;
  }

  return GST_FLOW_OK;
}

GstFlowReturn send_buffer_single(GstBuffer *buffer, UDPSink *udp_sink) {
  GstMapInfo map;
  if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    ensure_socket_open(udp_sink, false);

    wolf::platform::batched_send_info_t send_info;
    send_info.payload_buffers.emplace_back(reinterpret_cast<const char *>(map.data), map.size);
    send_info.block_offset = 0;
    send_info.block_count = 1;
    send_info.native_socket = udp_sink->socket->native_handle();
    send_info.target_address = udp_sink->client_endpoint->address();
    send_info.target_port = udp_sink->client_endpoint->port();

    bool success = wolf::platform::send_batch(send_info);
    gst_buffer_unmap(buffer, &map);

    if (!success) {
      logs::log(logs::error, "Error sending UDP packet");
      return GST_FLOW_ERROR;
    }
    return GST_FLOW_OK;
  } else {
    logs::log(logs::error, "Failed to map buffer");
    return GST_FLOW_ERROR;
  }
}

GstFlowReturn on_new_sample(GstAppSink *appsink, gpointer user_data) {
  std::shared_ptr<GstSample> sample(gst_app_sink_pull_sample(appsink), gst_sample_unref);
  if (!sample) {
    logs::log(logs::warning, "Custom sink: failed to create sample");
    return GST_FLOW_ERROR;
  }

  UDPSink *udp_sink = static_cast<UDPSink *>(user_data);

  if (GstBufferList *buffer_list = gst_sample_get_buffer_list(sample.get())) {
    return send_buffer_batched(buffer_list, udp_sink);
  } else if (GstBuffer *buffer = gst_sample_get_buffer(sample.get())) {
    return send_buffer_single(buffer, udp_sink);
  } else {
    logs::log(logs::warning, "Custom sink: failed to get buffer");
    return GST_FLOW_ERROR;
  }
}

} // namespace

void configure_appsink(GstElement *appsink, UDPSink *udp_sink) {
  g_object_set(appsink, "emit-signals", FALSE, NULL);
  g_object_set(appsink, "buffer-list", TRUE, NULL);

  GstAppSinkCallbacks callbacks = {nullptr};
  callbacks.new_sample = on_new_sample;
  gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &callbacks, udp_sink, nullptr);
}

} // namespace streaming::udp_sink
