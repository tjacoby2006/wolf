#include <boost/endian/conversion.hpp>
#include <control/control.hpp>
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

std::shared_ptr<events::JoypadTypes> create_new_joypad(const events::StreamSession &session,
                                                       immer::box<std::shared_ptr<ENetPeer>> connected_client,
                                                       int controller_number,
                                                       CONTROLLER_TYPE requested_type,
                                                       uint8_t capabilities) {

  auto on_rumble_fn = ([connected_client, controller_number, aes_key = session.aes_key](int low_freq, int high_freq) {
    auto rumble_pkt = ControlRumblePacket{
        .header = {.type = RUMBLE_DATA, .length = sizeof(ControlRumblePacket) - sizeof(ControlPacket)},
        .controller_number = boost::endian::native_to_little((uint16_t)controller_number),
        .low_freq = boost::endian::native_to_little((uint16_t)low_freq),
        .high_freq = boost::endian::native_to_little((uint16_t)high_freq)};
    std::string plaintext = {(char *)&rumble_pkt, sizeof(rumble_pkt)};
    encrypt_and_send(plaintext, aes_key, connected_client);
  });

  auto on_led_fn = ([connected_client, controller_number, aes_key = session.aes_key](int r, int g, int b) {
    auto led_pkt = ControlRGBLedPacket{
        .header{.type = RGB_LED_EVENT, .length = sizeof(ControlRGBLedPacket) - sizeof(ControlPacket)},
        .controller_number = boost::endian::native_to_little((uint16_t)controller_number),
        .r = static_cast<uint8_t>(r),
        .g = static_cast<uint8_t>(g),
        .b = static_cast<uint8_t>(b)};
    std::string plaintext = {(char *)&led_pkt, sizeof(led_pkt)};
    encrypt_and_send(plaintext, aes_key, connected_client);
  });

  auto on_adaptive_trigger_fn = ([connected_client, controller_number, aes_key = session.aes_key](
                                     const inputtino::PS5Joypad::TriggerEffect &effect) {
    auto rumble_pkt = ControlAdaptiveTriggerPacket{
        .header{.type = ADAPTIVE_TRIGGER_EVENT, .length = sizeof(ControlAdaptiveTriggerPacket) - sizeof(ControlPacket)},
        .controller_number = boost::endian::native_to_little((uint16_t)controller_number),
        .effect = effect};
    std::string plaintext = {(char *)&rumble_pkt, sizeof(rumble_pkt)};
    encrypt_and_send(plaintext, aes_key, connected_client);
  });

  std::shared_ptr<events::JoypadTypes> new_pad;
  auto controllers_override = session.client_settings->controllers_override;
  auto final_type = controllers_override.size() > controller_number ? controllers_override[controller_number]
                                                                    : wolf::config::ControllerType::AUTO;
  if (final_type == wolf::config::ControllerType::AUTO) {
    // Motion-gated per-client override. When this slot has no
    // `controllers_override`, the client advertises GYRO or
    // ACCELEROMETER, AND `motion_controller_override` is set to a
    // specific type, route to that type. Sibling to
    // `controllers_override` (unconditional force, per-slot), but
    // gated on the client actually having motion to forward so
    // non-motion clients are unaffected.
    if ((capabilities & (ACCELEROMETER | GYRO)) &&
        session.client_settings->motion_controller_override != wolf::config::ControllerType::AUTO) {
      final_type = session.client_settings->motion_controller_override;
    } else {
      switch (requested_type) {
      case XBOX:
        final_type = wolf::config::ControllerType::XBOX;
        break;
      case PS:
        final_type = wolf::config::ControllerType::PS;
        break;
      case NINTENDO:
        final_type = wolf::config::ControllerType::NINTENDO;
        break;
      default:
        // Client reported UNKNOWN. If it advertises GYRO or
        // ACCELEROMETER (typically a phone with built-in sensors
        // overlaying any underlying pad — see
        // moonlight-android ControllerHandler.java:3148) promote to
        // PS so the motion-request blocks below actually fire and the
        // gyro/accel data the client is ready to send gets requested
        // and forwarded. Without this it stays AUTO → XBOX and the
        // motion is silently dropped.
        if (capabilities & (ACCELEROMETER | GYRO)) {
          final_type = wolf::config::ControllerType::PS;
        } else {
          final_type = wolf::config::ControllerType::AUTO;
        }
        break;
      }
    }
  }
  switch (final_type) {
  case wolf::config::ControllerType::AUTO:
  case wolf::config::ControllerType::XBOX: {
    logs::log(logs::info,
              "Creating Xbox joypad for controller {} in session {}",
              controller_number,
              session.session_id);
    auto result =
        XboxOneJoypad::create({.name = "Wolf X-Box One (virtual) pad",
                               // https://github.com/torvalds/linux/blob/master/drivers/input/joystick/xpad.c#L147
                               .vendor_id = 0x045E,
                               .product_id = 0x02EA,
                               .version = 0x0408});
    if (!result) {
      logs::log(logs::error, "Failed to create Xbox One joypad: {}", result.getErrorMessage());
      return {};
    } else {
      (*result).set_on_rumble(on_rumble_fn);
      new_pad = std::make_shared<events::JoypadTypes>(std::move(*result));
    }
    break;
  }
  case wolf::config::ControllerType::PS: {
    logs::log(logs::info, "Creating PS joypad for controller {}", controller_number);
    auto result = PS5Joypad::create(
        {.name = "Wolf DualSense (virtual) pad", .vendor_id = 0x054C, .product_id = 0x0CE6, .version = 0x8111});
    if (!result) {
      logs::log(logs::error, "Failed to create PS5 joypad: {}", result.getErrorMessage());
      return {};
    } else {
      (*result).set_on_rumble(on_rumble_fn);
      (*result).set_on_led(on_led_fn);
      (*result).set_on_trigger_effect(on_adaptive_trigger_fn);
      new_pad = std::make_shared<events::JoypadTypes>(std::move(*result));

      // Let's wait for the kernel to pick it up and mount the /dev/ devices
      std::this_thread::sleep_for(std::chrono::milliseconds(50));

      std::visit(
          [&session](auto &pad) {
            if (auto wl = *session.wayland_display->load()) {
              for (const auto node : pad.get_udev_events()) {
                if (node.find("ID_INPUT_TOUCHPAD") != node.end()) {
                  add_input_device(*wl, node.at("DEVNAME"));
                }
              }
            }
          },
          *new_pad);
    }
    break;
  }
  case wolf::config::ControllerType::NINTENDO:
    logs::log(logs::info, "Creating Nintendo joypad for controller {}", controller_number);
    auto result = SwitchJoypad::create({.name = "Wolf Nintendo (virtual) pad",
                                        // https://github.com/torvalds/linux/blob/master/drivers/hid/hid-ids.h#L981
                                        .vendor_id = 0x057e,
                                        .product_id = 0x2009,
                                        .version = 0x8111});
    if (!result) {
      logs::log(logs::error, "Failed to create Switch joypad: {}", result.getErrorMessage());
      return {};
    } else {
      (*result).set_on_rumble(on_rumble_fn);
      new_pad = std::make_shared<events::JoypadTypes>(std::move(*result));
    }
    break;
  }

  if (capabilities & ACCELEROMETER && final_type == wolf::config::ControllerType::PS) {
    // Request acceleromenter events from the client at 100 Hz
    logs::log(logs::info, "Requesting accelerometer events for controller {}", controller_number);
    auto accelerometer_pkt = ControlMotionEventPacket{
        .header{.type = MOTION_EVENT, .length = sizeof(ControlMotionEventPacket) - sizeof(ControlPacket)},
        .controller_number = static_cast<uint16_t>(controller_number),
        .reportrate = 100,
        .type = ACCELERATION};
    std::string plaintext = {(char *)&accelerometer_pkt, sizeof(accelerometer_pkt)};
    encrypt_and_send(plaintext, session.aes_key, connected_client);
  }

  if (capabilities & GYRO && final_type == wolf::config::ControllerType::PS) {
    // Request gyroscope events from the client at 100 Hz
    logs::log(logs::info, "Requesting gyroscope events for controller {}", controller_number);
    auto gyro_pkt = ControlMotionEventPacket{
        .header{.type = MOTION_EVENT, .length = sizeof(ControlMotionEventPacket) - sizeof(ControlPacket)},
        .controller_number = static_cast<uint16_t>(controller_number),
        .reportrate = 100,
        .type = GYROSCOPE};
    std::string plaintext = {(char *)&gyro_pkt, sizeof(gyro_pkt)};
    encrypt_and_send(plaintext, session.aes_key, connected_client);
  }

  session.joypads->update([&](events::JoypadList joypads) {
    logs::log(logs::debug,
              "[INPUT] Sending PlugDeviceEvent for joypad {} of type: {}",
              controller_number,
              (int)final_type);

    events::PlugDeviceEvent plug_ev{.session_id = std::to_string(session.session_id)};
    std::visit(
        [&plug_ev](auto &pad) {
          plug_ev.udev_events = pad.get_udev_events();
          plug_ev.udev_hw_db_entries = pad.get_udev_hw_db_entries();
        },
        *new_pad);
    session.event_bus->fire_event(immer::box<events::PlugDeviceEvent>(plug_ev));
    return joypads.set(controller_number, new_pad);
  });
  return new_pad;
}

void controller_arrival(const CONTROLLER_ARRIVAL_PACKET &pkt,
                        events::StreamSession &session,
                        immer::box<std::shared_ptr<ENetPeer>> connected_client) {
  auto joypads = session.joypads->load();
  if (joypads->find(pkt.controller_number)) {
    // TODO: should we replace it instead?
    logs::log(logs::debug,
              "[INPUT] Received CONTROLLER_ARRIVAL for controller {} which is already present; skipping...",
              pkt.controller_number);
  } else {
    create_new_joypad(session,
                      connected_client,
                      pkt.controller_number,
                      (CONTROLLER_TYPE)pkt.controller_type,
                      pkt.capabilities);
  }
}

void controller_multi(const CONTROLLER_MULTI_PACKET &pkt,
                      events::StreamSession &session,
                      immer::box<std::shared_ptr<ENetPeer>> connected_client) {
  auto joypads = session.joypads->load();
  std::shared_ptr<events::JoypadTypes> selected_pad;
  if (auto joypad = joypads->find(pkt.controller_number)) {
    selected_pad = std::move(*joypad);

    // Check if Moonlight is sending the final packet for this pad
    if (!(pkt.active_gamepad_mask & (1 << pkt.controller_number))) {
      logs::log(logs::debug, "Removing joypad {}", pkt.controller_number);
      // Send the event downstream, Docker will pick it up and remove the device
      events::UnplugDeviceEvent unplug_ev{.session_id = std::to_string(session.session_id)};
      std::visit(
          [&unplug_ev](auto &pad) {
            unplug_ev.udev_events = pad.get_udev_events();
            unplug_ev.udev_hw_db_entries = pad.get_udev_hw_db_entries();
          },
          *selected_pad);
      session.event_bus->fire_event(immer::box<events::UnplugDeviceEvent>(unplug_ev));

      // Remove the joypad, this will delete the last reference
      session.joypads->update([&](events::JoypadList joypads) { return joypads.erase(pkt.controller_number); });
    }
  } else {
    // Old Moonlight doesn't support CONTROLLER_ARRIVAL, we create a default pad when it's first mentioned
    selected_pad = create_new_joypad(session, connected_client, pkt.controller_number, XBOX, ANALOG_TRIGGERS | RUMBLE);
  }
  if (selected_pad) {
    std::visit(
        [pkt, session](inputtino::Joypad &pad) {
          std::uint16_t bf = pkt.button_flags;
          std::uint32_t bf2 = pkt.buttonFlags2;
          auto pressed_buttons = bf | (bf2 << 16);
          // Check for our special WOLF-UI combo (START + UP + RB)
          if (pressed_buttons & inputtino::Joypad::START && pressed_buttons & inputtino::Joypad::DPAD_UP &&
              pressed_buttons & inputtino::Joypad::RIGHT_BUTTON) {
            session.event_bus->fire_event(immer::box<events::ClientWolfUIComboEvent>{
                events::ClientWolfUIComboEvent{.session_id = session.session_id}});
          }
          pad.set_pressed_buttons(pressed_buttons);
          pad.set_stick(inputtino::Joypad::LS, pkt.left_stick_x, pkt.left_stick_y);
          pad.set_stick(inputtino::Joypad::RS, pkt.right_stick_x, pkt.right_stick_y);
          pad.set_triggers(pkt.left_trigger, pkt.right_trigger);
        },
        *selected_pad);
  }
}

void controller_touch(const CONTROLLER_TOUCH_PACKET &pkt, events::StreamSession &session) {
  auto joypads = session.joypads->load();
  std::shared_ptr<events::JoypadTypes> selected_pad;
  if (auto joypad = joypads->find(pkt.controller_number)) {
    selected_pad = std::move(*joypad);
    auto pointer_id = boost::endian::little_to_native(pkt.pointer_id);
    switch (pkt.event_type) {
    case TOUCH_EVENT_DOWN:
    case TOUCH_EVENT_HOVER:
    case TOUCH_EVENT_MOVE: {
      if (std::holds_alternative<PS5Joypad>(*selected_pad)) {
        std::get<PS5Joypad>(*selected_pad)
            .place_finger(pointer_id,
                          netfloat_to_0_1(pkt.x) * (uint16_t)inputtino::PS5Joypad::touchpad_width,
                          netfloat_to_0_1(pkt.y) * (uint16_t)inputtino::PS5Joypad::touchpad_height);
      }
      break;
    }
    case TOUCH_EVENT_UP:
    case TOUCH_EVENT_HOVER_LEAVE:
    case TOUCH_EVENT_CANCEL: {
      if (std::holds_alternative<PS5Joypad>(*selected_pad)) {
        std::get<PS5Joypad>(*selected_pad).release_finger(pointer_id);
      }
      break;
    }
    case TOUCH_EVENT_CANCEL_ALL:
      logs::log(logs::warning, "Received TOUCH_EVENT_CANCEL_ALL which isn't supported");
      break;                      // TODO: remove all fingers
    case TOUCH_EVENT_BUTTON_ONLY: // TODO: ???
      logs::log(logs::warning, "Received TOUCH_EVENT_BUTTON_ONLY which isn't supported");
      break;
    }
  } else {
    logs::log(logs::warning, "Received controller touch for unknown controller {}", pkt.controller_number);
  }
}

void controller_motion(const CONTROLLER_MOTION_PACKET &pkt, events::StreamSession &session) {
  auto joypads = session.joypads->load();
  std::shared_ptr<events::JoypadTypes> selected_pad;
  if (auto joypad = joypads->find(pkt.controller_number)) {
    selected_pad = std::move(*joypad);
    if (std::holds_alternative<PS5Joypad>(*selected_pad)) {
      auto x = utils::from_netfloat(pkt.x);
      auto y = utils::from_netfloat(pkt.y);
      auto z = utils::from_netfloat(pkt.z);

      if (pkt.motion_type == ACCELERATION) {
        std::get<PS5Joypad>(*selected_pad).set_motion(inputtino::PS5Joypad::ACCELERATION, x, y, z);
      } else if (pkt.motion_type == GYROSCOPE) {
        std::get<PS5Joypad>(*selected_pad)
            .set_motion(inputtino::PS5Joypad::GYROSCOPE, deg2rad(x), deg2rad(y), deg2rad(z));
      }
    }
  }
}

void controller_battery(const CONTROLLER_BATTERY_PACKET &pkt, events::StreamSession &session) {
  auto joypads = session.joypads->load();
  std::shared_ptr<events::JoypadTypes> selected_pad;
  if (auto joypad = joypads->find(pkt.controller_number)) {
    selected_pad = std::move(*joypad);
    if (std::holds_alternative<PS5Joypad>(*selected_pad)) {
      inputtino::PS5Joypad::BATTERY_STATE state;
      switch (pkt.battery_state) {
      case BATTERY_STATE_UNKNOWN:
      case BATTERY_NOT_PRESENT:
        return; // We can't set it, let's return
      case BATTERY_DISCHARGHING:
        state = inputtino::PS5Joypad::BATTERY_DISCHARGING;
        break;
      case BATTERY_CHARGING:
        state = inputtino::PS5Joypad::BATTERY_CHARGHING;
        break;
      case BATTERY_NOT_CHARGING:
        state = inputtino::PS5Joypad::CHARGHING_ERROR;
        break;
      case BATTERY_FULL:
        state = inputtino::PS5Joypad::BATTERY_FULL;
        break;
      }
      if (pkt.battery_percentage != BATTERY_PERCENTAGE_UNKNOWN) {
        std::get<PS5Joypad>(*selected_pad).set_battery(state, pkt.battery_percentage);
      }
    }
  }
}

} // namespace control