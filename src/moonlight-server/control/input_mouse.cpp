#include <boost/endian/conversion.hpp>
#include <control/input_handler.hpp>
#include <control/input_handler_internal.hpp>
#include <events/events.hpp>
#include <helpers/logger.hpp>

namespace control {

using namespace wolf::core::virtual_display;
using namespace wolf::core::input;
using namespace wolf::core;
using namespace moonlight::control;

void mouse_move_rel(const MOUSE_MOVE_REL_PACKET &pkt, events::StreamSession &session) {
  if (session.mouse->has_value()) {
    auto pointer_acceleration = session.client_settings->mouse_acceleration;
    auto delta_x = static_cast<float>(boost::endian::big_to_native(pkt.delta_x)) * pointer_acceleration;
    auto delta_y = static_cast<float>(boost::endian::big_to_native(pkt.delta_y)) * pointer_acceleration;
    std::visit([delta_x, delta_y](auto &mouse) { mouse.move(delta_x, delta_y); }, session.mouse->value());
  } else {
    logs::log(logs::warning, "Received MOUSE_MOVE_REL_PACKET but no mouse device is present");
  }
}

void mouse_move_abs(const MOUSE_MOVE_ABS_PACKET &pkt, events::StreamSession &session) {
  if (session.mouse->has_value()) {
    auto pointer_acceleration = session.client_settings->mouse_acceleration;
    float x = boost::endian::big_to_native(pkt.x);
    float y = boost::endian::big_to_native(pkt.y);
    float window_width = boost::endian::big_to_native(pkt.width);
    float window_height = boost::endian::big_to_native(pkt.height);

    auto absolute_x = (x / window_width) * static_cast<float>(session.display_mode.width) * pointer_acceleration;
    auto absolute_y = (y / window_height) * static_cast<float>(session.display_mode.height) * pointer_acceleration;

    std::visit([absolute_x,
                absolute_y,
                screen_width = session.display_mode.width,
                screen_height = session.display_mode.height](
                   auto &mouse) { mouse.move_abs(absolute_x, absolute_y, screen_width, screen_height); },
               session.mouse->value());
  } else {
    logs::log(logs::warning, "Received MOUSE_MOVE_ABS_PACKET but no mouse device is present");
  }
}

void mouse_button(const MOUSE_BUTTON_PACKET &pkt, events::StreamSession &session) {
  if (session.mouse->has_value()) {
    if (std::holds_alternative<state::input::Mouse>(session.mouse->value())) {
      Mouse::MOUSE_BUTTON btn_type;

      switch (pkt.button) {
      case 1:
        btn_type = Mouse::LEFT;
        break;
      case 2:
        btn_type = Mouse::MIDDLE;
        break;
      case 3:
        btn_type = Mouse::RIGHT;
        break;
      case 4:
        btn_type = Mouse::SIDE;
        break;
      default:
        btn_type = Mouse::EXTRA;
        break;
      }
      if (pkt.type == MOUSE_BUTTON_PRESS) {
        std::get<state::input::Mouse>(session.mouse->value()).press(btn_type);
      } else {
        std::get<state::input::Mouse>(session.mouse->value()).release(btn_type);
      }
    } else if (std::holds_alternative<wolf::core::virtual_display::WaylandMouse>(session.mouse->value())) {
      if (pkt.type == MOUSE_BUTTON_PRESS) {
        std::get<wolf::core::virtual_display::WaylandMouse>(session.mouse->value()).press(pkt.button);
      } else {
        std::get<wolf::core::virtual_display::WaylandMouse>(session.mouse->value()).release(pkt.button);
      }
    }
  } else {
    logs::log(logs::warning, "Received MOUSE_BUTTON_PACKET but no mouse device is present");
  }
}

void mouse_scroll(const MOUSE_SCROLL_PACKET &pkt, events::StreamSession &session) {
  if (session.mouse->has_value()) {
    std::visit(
        [session, scroll_amount = boost::endian::big_to_native(pkt.scroll_amt1)](auto &mouse) {
          auto scroll_acceleration = session.client_settings->v_scroll_acceleration;
          mouse.vertical_scroll(scroll_amount * scroll_acceleration);
        },
        session.mouse->value());
  } else {
    logs::log(logs::warning, "Received MOUSE_SCROLL_PACKET but no mouse device is present");
  }
}

void mouse_h_scroll(const MOUSE_HSCROLL_PACKET &pkt, events::StreamSession &session) {
  if (session.mouse->has_value()) {
    std::visit(
        [session, scroll_amount = boost::endian::big_to_native(pkt.scroll_amount)](auto &mouse) {
          auto scroll_acceleration = session.client_settings->h_scroll_acceleration;
          mouse.horizontal_scroll(scroll_amount * scroll_acceleration);
        },
        session.mouse->value());
  } else {
    logs::log(logs::warning, "Received MOUSE_HSCROLL_PACKET but no mouse device is present");
  }
}

} // namespace control
