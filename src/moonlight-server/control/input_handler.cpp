#include <control/input_handler.hpp>
#include <events/events.hpp>
#include <helpers/logger.hpp>

namespace control {

using namespace wolf::core;
using namespace moonlight::control;

/**
 * Dispatch a Moonlight input packet to the handler for its device category.
 *
 * The handlers themselves live in focused translation units:
 *   - input_mouse.cpp      (mouse move/button/scroll)
 *   - input_keyboard.cpp   (keyboard + UTF-8 text)
 *   - input_touch_pen.cpp  (touch screen + pen tablet)
 *   - input_controller.cpp (joypads: arrival/multi/touch/motion/battery)
 */
void handle_input(events::StreamSession &session,
                  immer::box<std::shared_ptr<ENetPeer>> connected_client,
                  INPUT_PKT *pkt) {
  switch (pkt->type) {
  case MOUSE_MOVE_REL: {
    logs::log(logs::trace, "[INPUT] Received input of type: MOUSE_MOVE_REL");
    auto move_pkt = static_cast<MOUSE_MOVE_REL_PACKET *>(pkt);
    mouse_move_rel(*move_pkt, session);
    break;
  }
  case MOUSE_MOVE_ABS: {
    logs::log(logs::trace, "[INPUT] Received input of type: MOUSE_MOVE_ABS");
    auto move_pkt = static_cast<MOUSE_MOVE_ABS_PACKET *>(pkt);
    mouse_move_abs(*move_pkt, session);
    break;
  }
  case MOUSE_BUTTON_PRESS:
  case MOUSE_BUTTON_RELEASE: {
    logs::log(logs::trace, "[INPUT] Received input of type: MOUSE_BUTTON_PACKET");
    auto btn_pkt = static_cast<MOUSE_BUTTON_PACKET *>(pkt);
    mouse_button(*btn_pkt, session);
    break;
  }
  case MOUSE_SCROLL: {
    logs::log(logs::trace, "[INPUT] Received input of type: MOUSE_SCROLL_PACKET");
    auto scroll_pkt = (static_cast<MOUSE_SCROLL_PACKET *>(pkt));
    mouse_scroll(*scroll_pkt, session);
    break;
  }
  case MOUSE_HSCROLL: {
    logs::log(logs::trace, "[INPUT] Received input of type: MOUSE_HSCROLL_PACKET");
    auto scroll_pkt = (static_cast<MOUSE_HSCROLL_PACKET *>(pkt));
    mouse_h_scroll(*scroll_pkt, session);
    break;
  }
  case KEY_PRESS:
  case KEY_RELEASE: {
    logs::log(logs::trace, "[INPUT] Received input of type: KEYBOARD_PACKET");
    auto key_pkt = static_cast<KEYBOARD_PACKET *>(pkt);
    keyboard_key(*key_pkt, session);
    break;
  }
  case UTF8_TEXT: {
    logs::log(logs::trace, "[INPUT] Received input of type: UTF8_TEXT");
    auto txt_pkt = static_cast<UTF8_TEXT_PACKET *>(pkt);
    utf8_text(*txt_pkt, session);
    break;
  }
  case TOUCH: {
    logs::log(logs::trace, "[INPUT] Received input of type: TOUCH");
    auto touch_pkt = static_cast<TOUCH_PACKET *>(pkt);
    touch(*touch_pkt, session);
    break;
  }
  case PEN: {
    logs::log(logs::trace, "[INPUT] Received input of type: PEN");
    auto pen_pkt = static_cast<PEN_PACKET *>(pkt);
    pen(*pen_pkt, session);
    break;
  }
  case CONTROLLER_ARRIVAL: {
    logs::log(logs::trace, "[INPUT] Received input of type: CONTROLLER_ARRIVAL");
    auto new_controller = static_cast<CONTROLLER_ARRIVAL_PACKET *>(pkt);
    controller_arrival(*new_controller, session, connected_client);
    break;
  }
  case CONTROLLER_MULTI: {
    logs::log(logs::trace, "[INPUT] Received input of type: CONTROLLER_MULTI");
    auto controller_pkt = static_cast<CONTROLLER_MULTI_PACKET *>(pkt);
    controller_multi(*controller_pkt, session, connected_client);
    break;
  }
  case CONTROLLER_TOUCH: {
    logs::log(logs::trace, "[INPUT] Received input of type: CONTROLLER_TOUCH");
    auto touch_pkt = static_cast<CONTROLLER_TOUCH_PACKET *>(pkt);
    controller_touch(*touch_pkt, session);
    break;
  }
  case CONTROLLER_MOTION: {
    logs::log(logs::trace, "[INPUT] Received input of type: CONTROLLER_MOTION");
    auto motion_pkt = static_cast<CONTROLLER_MOTION_PACKET *>(pkt);
    controller_motion(*motion_pkt, session);
    break;
  }
  case CONTROLLER_BATTERY: {
    logs::log(logs::trace, "[INPUT] Received input of type: CONTROLLER_BATTERY");
    auto battery_pkt = static_cast<CONTROLLER_BATTERY_PACKET *>(pkt);
    controller_battery(*battery_pkt, session);
    break;
  }
  case HAPTICS:
    logs::log(logs::trace, "[INPUT] Received input of type: HAPTICS");
    break;
  }
}

} // namespace control