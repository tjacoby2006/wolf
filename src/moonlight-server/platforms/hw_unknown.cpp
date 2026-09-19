#include "hw.hpp"

std::vector<std::string> linked_devices(std::string_view gpu) {
  return {};
}

std::vector<std::string> discover_dri_render_nodes() {
  return {};
}

std::string get_render_node_name(std::string_view render_node) {
  return std::string(render_node);
}

bool is_render_node_available(std::string_view render_node) {
  return !render_node.empty();
}

bool probe_render_node(std::string_view render_node) {
  return false; // No DRM on non-Linux platforms.
}

GPU_VENDOR get_vendor(std::string_view gpu) {
  return UNKNOWN;
}

std::optional<std::string> get_nvidia_device_index(std::string_view render_node) {
  return std::nullopt;
}

std::string get_mac_address(std::string_view local_ip) {
  return "00:00:00:00:00:00";
}