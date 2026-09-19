#include <streaming/pipeline_runner.hpp>

#include <events/events.hpp>
#include <helpers/logger.hpp>

namespace streaming {

bool run_pipeline(
    const std::string &pipeline_desc,
    const std::function<immer::array<immer::box<wolf::core::events::EventBusHandlers>>(
        wolf::core::gstreamer::gst_element_ptr)>
        &on_pipeline_ready) {
  GError *error = nullptr;
  wolf::core::gstreamer::gst_element_ptr pipeline(
      gst_parse_launch(pipeline_desc.c_str(), &error), [](const auto &pipeline) {
        logs::log(logs::trace, "~pipeline");
        gst_object_unref(pipeline);
      });

  if (!pipeline) {
    logs::log(logs::error, "[GSTREAMER] Pipeline parse error: {}", error->message);
    g_error_free(error);
    return false;
  } else if (error) { // Please note that you might get a return value that is not NULL even though the error is set. In
                      // this case there was a recoverable parsing error and you can try to play the pipeline.
    logs::log(logs::warning, "[GSTREAMER] Pipeline parse error (recovered): {}", error->message);
    g_error_free(error);
  }

  wolf::core::gstreamer::gst_main_context_ptr context = {g_main_context_new(), ::g_main_context_unref};
  g_main_context_push_thread_default(context.get());
  wolf::core::gstreamer::gst_main_loop_ptr loop(g_main_loop_new(context.get(), FALSE), ::g_main_loop_unref);

  /* Let the calling thread set extra things */
  auto handlers = on_pipeline_ready(pipeline);

  /*
   * adds a watch for new message on our pipeline's message bus to
   * the default GLib main context, which is the main context that our
   * GLib main loop is attached to below
   */
  auto bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline.get()));
  gst_bus_add_signal_watch(bus);
  g_signal_connect(bus, "message::error", G_CALLBACK(wolf::core::gstreamer::pipeline_error_handler), loop.get());
  g_signal_connect(bus, "message::eos", G_CALLBACK(wolf::core::gstreamer::pipeline_eos_handler), loop.get());
  gst_object_unref(bus);

  /* Set the pipeline to "playing" state*/
  gst_element_set_state(pipeline.get(), GST_STATE_PLAYING);
  GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(reinterpret_cast<GstBin *>(pipeline.get()),
                                    GST_DEBUG_GRAPH_SHOW_ALL,
                                    "pipeline-start");

  /* The main loop will be run until someone calls g_main_loop_quit() */
  g_main_loop_run(loop.get());

  /* Out of the main loop, clean up nicely */
  gst_element_set_state(pipeline.get(), GST_STATE_PAUSED);
  gst_element_set_state(pipeline.get(), GST_STATE_READY);
  gst_element_set_state(pipeline.get(), GST_STATE_NULL);

  return true;
}

} // namespace streaming
