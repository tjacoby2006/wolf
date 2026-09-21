#include <boost/endian/conversion.hpp>
#include <control/input_handler.hpp>
#include <control/input_handler_internal.hpp>
#include <events/events.hpp>
#include <helpers/logger.hpp>
#include <platforms/input.hpp>

namespace control {

using namespace wolf::core::virtual_display;
using namespace wolf::core::input;
using namespace wolf::core;
using namespace moonlight::control;

/**
 * Creates a new PenTablet and saves it into the session;
 * will also trigger a PlugDeviceEvent
 */
bool create_pen_tablet(events::StreamSession &session) {
  logs::log(logs::debug, "[INPUT] Creating new pen tablet");
  auto tablet = PenTablet::create();
  if (!tablet) {
    logs::log(logs::error, "Failed to create pen tablet: {}", tablet.getErrorMessage());
    return false;
  }
  auto tablet_ptr = std::make_shared<PenTablet>(std::move(*tablet));
  session.event_bus->fire_event(immer::box<events::PlugDeviceEvent>(
      events::PlugDeviceEvent{.session_id = std::to_string(session.session_id),
                              .udev_events = tablet_ptr->get_udev_events(),
                              .udev_hw_db_entries = tablet_ptr->get_udev_hw_db_entries()}));
  if (auto wl = *session.wayland_display->load()) {
    for (const auto node : tablet_ptr->get_nodes()) {
      add_input_device(*wl, node);
    }
  }
  session.pen_tablet->emplace(std::move(*tablet_ptr));
  return true;
}

/**
 * Creates a new Touch screen and saves it into the session;
 * will also trigger a PlugDeviceEvent
 */
bool create_touch_screen(events::StreamSession &session) {
  logs::log(logs::debug, "[INPUT] Creating new touch screen");
  auto touch = TouchScreen::create();
  if (!touch) {
    logs::log(logs::error, "Failed to create touch screen: {}", touch.getErrorMessage());
    return false;
  }
  auto touch_screen = std::make_shared<TouchScreen>(std::move(*touch));
  session.event_bus->fire_event(immer::box<events::PlugDeviceEvent>(
      events::PlugDeviceEvent{.session_id = std::to_string(session.session_id),
                              .udev_events = touch_screen->get_udev_events(),
                              .udev_hw_db_entries = touch_screen->get_udev_hw_db_entries()}));
  if (auto wl = *session.wayland_display->load()) {
    for (const auto node : touch_screen->get_nodes()) {
      add_input_device(*wl, node);
    }
  }
  session.touch_screen->emplace(std::move(*touch_screen));
  return true;
}

void touch(const TOUCH_PACKET &pkt, events::StreamSession &session) {
  bool has_touch_device = session.touch_screen->has_value();
  if (!has_touch_device) {
    has_touch_device = create_touch_screen(session);
  }
  if (has_touch_device) {
    auto finger_id = boost::endian::little_to_native(pkt.pointer_id);
    auto x = netfloat_to_0_1(pkt.x);
    auto y = netfloat_to_0_1(pkt.y);
    auto pressure_or_distance = netfloat_to_0_1(pkt.pressure_or_distance);
    std::visit(
        [&pkt, &session, &x, &y, &finger_id, &pressure_or_distance](auto &screen) {
          using T = std::decay_t<decltype(screen)>;
          if constexpr (std::is_same_v<T, input::TouchScreen>) {
            // Inputtino TouchScreen only defines place_finger and release_finger methods
            switch (pkt.event_type) {
            case pkts::TOUCH_EVENT_HOVER:
            case pkts::TOUCH_EVENT_DOWN:
            case pkts::TOUCH_EVENT_MOVE: {
              // Convert our 0..360 range to -90..90 relative to Y axis
              int adjusted_angle = pkt.rotation;

              if (adjusted_angle > 90 && adjusted_angle < 270) {
                // Lower hemisphere
                adjusted_angle = 180 - adjusted_angle;
              }

              // Wrap the value if it's out of range
              if (adjusted_angle > 90) {
                adjusted_angle -= 360;
              } else if (adjusted_angle < -90) {
                adjusted_angle += 360;
              }
              screen.place_finger(finger_id, x, y, pressure_or_distance, adjusted_angle);
              break;
            }
            case pkts::TOUCH_EVENT_UP:
            case pkts::TOUCH_EVENT_HOVER_LEAVE:
            case pkts::TOUCH_EVENT_CANCEL:
              screen.release_finger(finger_id);
              break;
            default:
              logs::log(logs::warning, "[INPUT] Unknown touch event type {}", (int)pkt.event_type);
            }
          } else if constexpr (std::is_same_v<T, virtual_display::WaylandTouchScreen>) {
            // For WaylandTouchScreen, we handle the events differently
            switch (pkt.event_type) {
            case pkts::TOUCH_EVENT_HOVER:
            case pkts::TOUCH_EVENT_DOWN:
              screen.down(finger_id, x, y);
              break;
            case pkts::TOUCH_EVENT_MOVE:
              screen.motion(finger_id, x, y);
              break;
            case pkts::TOUCH_EVENT_UP:
              screen.up(finger_id);
              break;
            case pkts::TOUCH_EVENT_HOVER_LEAVE:
            case pkts::TOUCH_EVENT_CANCEL:
              screen.cancel();
              break;
            default:
              logs::log(logs::warning, "[INPUT] Unknown touch event type {}", (int)pkt.event_type);
            }
            // Moonlight TOUCH_EVENT does not include TouchFrame events, so we trigger it manually every time
            screen.frame();
          }
        },
        **session.touch_screen);
  }
}

void pen(const PEN_PACKET &pkt, events::StreamSession &session) {
  bool has_pen_device = session.pen_tablet->has_value();
  if (!has_pen_device) {
    create_pen_tablet(session);
  }

  if (has_pen_device) {
    // First set the buttons
    session.pen_tablet->value().set_btn(PenTablet::PRIMARY, pkt.pen_buttons & PEN_BUTTON_TYPE_PRIMARY);
    session.pen_tablet->value().set_btn(PenTablet::SECONDARY, pkt.pen_buttons & PEN_BUTTON_TYPE_SECONDARY);
    session.pen_tablet->value().set_btn(PenTablet::TERTIARY, pkt.pen_buttons & PEN_BUTTON_TYPE_TERTIARY);

    // Set the tool
    PenTablet::TOOL_TYPE tool;
    switch (pkt.tool_type) {
    case moonlight::control::pkts::TOOL_TYPE_PEN:
      tool = PenTablet::PEN;
      break;
    case moonlight::control::pkts::TOOL_TYPE_ERASER:
      tool = PenTablet::ERASER;
      break;
    default:
      tool = PenTablet::SAME_AS_BEFORE;
      break;
    }

    auto pressure_or_distance = netfloat_to_0_1(pkt.pressure_or_distance);

    // Normalize rotation value to 0-359 degree range
    auto rotation = boost::endian::little_to_native(pkt.rotation);
    if (rotation != PEN_ROTATION_UNKNOWN) {
      rotation %= 360;
    }

    // Here we receive:
    //  - Rotation: degrees from vertical in Y dimension (parallel to screen, 0..360)
    //  - Tilt: degrees from vertical in Z dimension (perpendicular to screen, 0..90)
    float tilt_x = 0;
    float tilt_y = 0;
    // Convert polar coordinates into Y tilt angles
    if (pkt.tilt != PEN_TILT_UNKNOWN && rotation != PEN_ROTATION_UNKNOWN) {
      auto rotation_rads = deg2rad(rotation);
      auto tilt_rads = deg2rad(pkt.tilt);
      auto r = std::sin(tilt_rads);
      auto z = std::cos(tilt_rads);

      tilt_x = std::atan2(std::sin(-rotation_rads) * r, z) * 180.f / M_PI;
      tilt_y = std::atan2(std::cos(-rotation_rads) * r, z) * 180.f / M_PI;
    }

    bool is_touching = pkt.event_type == TOUCH_EVENT_DOWN || pkt.event_type == TOUCH_EVENT_MOVE;

    session.pen_tablet->value().place_tool(tool,
                                           netfloat_to_0_1(pkt.x),
                                           netfloat_to_0_1(pkt.y),
                                           is_touching ? pressure_or_distance : -1,
                                           is_touching ? -1 : pressure_or_distance,
                                           tilt_x,
                                           tilt_y);
  }
}

} // namespace control