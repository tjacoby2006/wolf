#include <boost/endian/conversion.hpp>
#include <boost/locale.hpp>
#include <control/input_handler.hpp>
#include <events/events.hpp>
#include <helpers/logger.hpp>
#include <platforms/input.hpp>

namespace control {

using namespace wolf::core::virtual_display;
using namespace wolf::core::input;
using namespace wolf::core;
using namespace moonlight::control;

namespace {

char modifier_bit(short moonlight_key) {
  switch (moonlight_key) {
  case M_SHIFT:
  case M_LSHIFT:
  case M_RSHIFT:
    return KEYBOARD_MODIFIERS::SHIFT;
  case M_CTRL:
  case M_LCTRL:
  case M_RCTRL:
    return KEYBOARD_MODIFIERS::CTRL;
  case M_MENU:
  case M_ALT:
  case M_RALT:
    return KEYBOARD_MODIFIERS::ALT;
  case M_META:
  case M_RMETA:
    return KEYBOARD_MODIFIERS::META;
  default:
    return KEYBOARD_MODIFIERS::NONE;
  }
}

} // namespace

void keyboard_key(const KEYBOARD_PACKET &pkt, events::StreamSession &session) {
  // moonlight always sets the high bit; not sure why but mask it off here
  short moonlight_key = (short)boost::endian::little_to_native(pkt.key_code) & (short)0x7fff;

  if (!session.keyboard->has_value()) {
    logs::log(logs::warning, "Received KEYBOARD_PACKET but no keyboard device is present");
    return;
  }

  const char key_mod = modifier_bit(moonlight_key);
  char held = 0;
  if (key_mod != KEYBOARD_MODIFIERS::NONE) {
    held = session.held_modifiers->update(
        [&](char h) -> char { return pkt.type == KEY_PRESS ? (h | key_mod) : (h & ~key_mod); });
  } else {
    held = session.held_modifiers->load();
  }

  if (pkt.type == KEY_PRESS) {
    int wolf_ui_combo_pressed = 0;
    if (pkt.modifiers & KEYBOARD_MODIFIERS::SHIFT && !(key_mod & KEYBOARD_MODIFIERS::SHIFT))
      wolf_ui_combo_pressed++;
    if (pkt.modifiers & KEYBOARD_MODIFIERS::CTRL && !(key_mod & KEYBOARD_MODIFIERS::CTRL))
      wolf_ui_combo_pressed++;
    if (pkt.modifiers & KEYBOARD_MODIFIERS::ALT && !(key_mod & KEYBOARD_MODIFIERS::ALT))
      wolf_ui_combo_pressed++;

    // CTRL + ALT + SHIFT + W
    const bool wolf_ui_combo = (wolf_ui_combo_pressed == 3 && moonlight_key == 0x57);
    if (wolf_ui_combo) {
      // Ensure modifiers are released before we return to overlay
      std::visit(
          [](auto &keyboard) {
            keyboard.release(M_SHIFT);
            keyboard.release(M_CTRL);
            keyboard.release(M_ALT);
          },
          session.keyboard->value());
      session.held_modifiers->store(0);
      session.event_bus->fire_event(
          immer::box<events::ClientWolfUIComboEvent>{events::ClientWolfUIComboEvent{.session_id = session.session_id}});
      return;
    }

    // Press the virtual modifiers
    const char virtual_mods = pkt.modifiers & ~held & ~key_mod;
    if (virtual_mods & KEYBOARD_MODIFIERS::SHIFT)
      std::visit([](auto &keyboard) { keyboard.press(M_SHIFT); }, session.keyboard->value());
    if (virtual_mods & KEYBOARD_MODIFIERS::CTRL)
      std::visit([](auto &keyboard) { keyboard.press(M_CTRL); }, session.keyboard->value());
    if (virtual_mods & KEYBOARD_MODIFIERS::ALT)
      std::visit([](auto &keyboard) { keyboard.press(M_ALT); }, session.keyboard->value());
    if (virtual_mods & KEYBOARD_MODIFIERS::META)
      std::visit([](auto &keyboard) { keyboard.press(M_META); }, session.keyboard->value());

    // Press the actual key
    std::visit([moonlight_key](auto &keyboard) { keyboard.press(moonlight_key); }, session.keyboard->value());

    // Release the virtual modifiers
    if (virtual_mods & KEYBOARD_MODIFIERS::SHIFT)
      std::visit([](auto &keyboard) { keyboard.release(M_SHIFT); }, session.keyboard->value());
    if (virtual_mods & KEYBOARD_MODIFIERS::CTRL)
      std::visit([](auto &keyboard) { keyboard.release(M_CTRL); }, session.keyboard->value());
    if (virtual_mods & KEYBOARD_MODIFIERS::ALT)
      std::visit([](auto &keyboard) { keyboard.release(M_ALT); }, session.keyboard->value());
    if (virtual_mods & KEYBOARD_MODIFIERS::META)
      std::visit([](auto &keyboard) { keyboard.release(M_META); }, session.keyboard->value());

  } else {
    std::visit([moonlight_key](auto &keyboard) { keyboard.release(moonlight_key); }, session.keyboard->value());
  }
}

void utf8_text(const UTF8_TEXT_PACKET &pkt, events::StreamSession &session) {
  if (session.keyboard->has_value()) {
    /* Here we receive a single UTF-8 encoded char at a time,
     * the trick is to convert it to UTF-32 then send CTRL+SHIFT+U+<HEXCODE> in order to produce any
     * unicode character, see: https://en.wikipedia.org/wiki/Unicode_input
     *
     * ex:
     * - when receiving UTF-8 [0xF0 0x9F 0x92 0xA9] (which is '💩')
     * - we'll convert it to UTF-32 [0x1F4A9]
     * - then type: CTRL+SHIFT+U+1F4A9
     * see the conversion at: https://www.compart.com/en/unicode/U+1F4A9
     */
    auto size = boost::endian::big_to_native(pkt.data_size) - sizeof(pkt.packet_type) - 2;
    /* Reading input text as UTF-8 */
    auto utf8 = boost::locale::conv::to_utf<wchar_t>(pkt.text, pkt.text + size, "UTF-8");
    /* Converting to UTF-32 */
    auto utf32 = boost::locale::conv::utf_to_utf<char32_t>(utf8);
    wolf::platforms::input::paste_utf(session.keyboard->value(), utf32);
  } else {
    logs::log(logs::warning, "Received UTF8_TEXT_PACKET but no keyboard device is present");
  }
}

} // namespace control