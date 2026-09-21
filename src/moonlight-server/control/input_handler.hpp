#pragma once

#include <control/control.hpp>
#include <events/events.hpp>
#include <moonlight/control.hpp>

namespace control {

using namespace moonlight::control::pkts;
using namespace wolf::core;

/**
 * Side effect: session devices might be updated when hotplugging
 */
void handle_input(events::StreamSession &session,
                  immer::box<std::shared_ptr<ENetPeer>> connected_client,
                  INPUT_PKT *pkt);

void mouse_move_rel(const MOUSE_MOVE_REL_PACKET &pkt, events::StreamSession &session);

void mouse_move_abs(const MOUSE_MOVE_ABS_PACKET &pkt, events::StreamSession &session);

void mouse_button(const MOUSE_BUTTON_PACKET &pkt, events::StreamSession &session);

void mouse_scroll(const MOUSE_SCROLL_PACKET &pkt, events::StreamSession &session);

void mouse_h_scroll(const MOUSE_HSCROLL_PACKET &pkt, events::StreamSession &session);

void keyboard_key(const KEYBOARD_PACKET &pkt, events::StreamSession &session);

void utf8_text(const UTF8_TEXT_PACKET &pkt, events::StreamSession &session);

void touch(const TOUCH_PACKET &pkt, events::StreamSession &session);

void pen(const PEN_PACKET &pkt, events::StreamSession &session);

void controller_arrival(const CONTROLLER_ARRIVAL_PACKET &pkt,
                        events::StreamSession &session,
                        immer::box<std::shared_ptr<ENetPeer>> connected_client);

void controller_multi(const CONTROLLER_MULTI_PACKET &pkt,
                      events::StreamSession &session,
                      immer::box<std::shared_ptr<ENetPeer>> connected_client);

void controller_touch(const CONTROLLER_TOUCH_PACKET &pkt, events::StreamSession &session);

void controller_motion(const CONTROLLER_MOTION_PACKET &pkt, events::StreamSession &session);

void controller_battery(const CONTROLLER_BATTERY_PACKET &pkt, events::StreamSession &session);

/**
 * Create a virtual joypad of the type the client asked for (or the configured override) and record
 * it in the session, firing a PlugDeviceEvent. Returns the new pad, or null on failure.
 */
std::shared_ptr<events::JoypadTypes> create_new_joypad(const events::StreamSession &session,
                                                       immer::box<std::shared_ptr<ENetPeer>> connected_client,
                                                       int controller_number,
                                                       CONTROLLER_TYPE requested_type,
                                                       uint8_t capabilities);

/**
 * Create a virtual pen tablet / touch screen and record it in the session, firing a PlugDeviceEvent.
 * Returns false if the device could not be created.
 */
bool create_pen_tablet(events::StreamSession &session);
bool create_touch_screen(events::StreamSession &session);

} // namespace control
