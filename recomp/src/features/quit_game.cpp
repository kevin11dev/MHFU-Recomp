// Quit Game: a row in the village's START menu, under Receive, that asks what the bed asks and then
// closes the game.
//
// village_menu_draw shows seven rows a page, their text numbers from two tables of lobby_task's;
// the second page's last is a placeholder ("－－－－－－－－", text 456), always grey, with no
// description under the menu, and choosing it (village_menu_choose) only buzzes. This puts text
// numbers of its own there, whose text (lobby_text) is "Quit Game" and a line about it in the
// game's language, the row drawn white rather than grey (render_text).
//
// Choosing it closes the menu and starts the hunter's bed action (hunter_bed, the village's action
// 9, which holds the hunter still), and an action started this way is this file's: the bed's
// prompts without the bed. "Would you like to save?" (save_prompt_update), saving as the bed saves
// (bed_save_update: saving, "Save completed.", then "Exit game?"), and where the bed would go back
// to the title screen, the game closes, as closing its window does. Saying no leaves the hunter
// standing where they were.

#include "../features.hpp"
#include "../game.hpp"
#include "../hook.hpp"

#include <refract/frontend.hpp>

#include <cstdint>
#include <string_view>

namespace mhfu {
namespace {

using namespace game;
using psprecomp::patch::call_game_function;

// The second page's last row in lobby_task's tables of the menu's text numbers: its label, and the
// description shown under the menu (the window's description kind 4, a row each).
constexpr std::uint8_t quit_row = 13;
constexpr Address placeholder_row = 0x09b051aeU + 6U * 2U;
constexpr Address placeholder_description = 0x09b051c8U + quit_row * 2U;
// Quit Game's text numbers, past the village's own, and where their text is kept: in the first
// 9 KB of user memory, which the runtime leaves free (the 60 fps code, cheats.hpp, is lower down).
constexpr std::uint16_t quit_text = 0x7f00U;
constexpr std::uint16_t quit_description = 0x7f01U;
constexpr Address quit_text_address = 0x08801e00U;
constexpr Address quit_description_address = 0x08801e40U;
constexpr std::uint32_t receive_text = 455;  // the row above, by which the language is told

// The village's state, the first word of lobby_task's lobby manager (pointed to from 0x09B22D70):
// the bed sets it to 0xf while it saves, and back to 0 after.
constexpr Address lobby_manager = 0x09b22d70U;
constexpr std::uint32_t lobby_saving = 0xfU;
constexpr std::uint32_t lobby_free = 0;

constexpr std::uint8_t action_bed = 9;
constexpr std::uint8_t action_stand = 6;
constexpr std::int32_t text_white = 0;

// Quit Game in each of the game's languages, by its word for Receive.
struct Label {
  std::string_view receive;
  std::string_view quit;
  std::string_view description;
};
constexpr Label labels[] = {
    {"Receive", "Quit Game", "Save if you like, then quit\nthe game."},
    {"Recevoir", "Quitter le jeu", "Sauvegarder au besoin, puis\nquitter le jeu."},
    {"Empfangen", "Spiel beenden", "Auf Wunsch speichern, dann\ndas Spiel beenden."},
    {"Ricevi", "Esci dal gioco", "Salva se vuoi, poi esci\ndal gioco."},
    {"Recibir", "Salir del juego", "Guarda si quieres y luego\nsal del juego."}};

// Where the action started from the menu is: asking whether to save, saving (and then asking
// whether to leave), or leaving (the game closes in a frame or two; until then, nothing more).
enum class Step { off, asking, saving, leaving };
Step step = Step::off;

std::string_view read_text(psprecomp::State& state, Address address) {
  constexpr std::uint32_t longest = 64;
  std::uint32_t length = 0;
  while (length < longest && in_ram(address + length, 1) && psprecomp::load8(state, address + length) != 0) ++length;
  static char text[longest];
  for (std::uint32_t i = 0; i < length; ++i) text[i] = static_cast<char>(psprecomp::load8(state, address + i));
  return {text, length};
}

bool draw_menu_hook(psprecomp::State& state) {
  if (loaded(state, overlay::lobby_task)) {
    psprecomp::store16(state, placeholder_row, quit_text);
    psprecomp::store16(state, placeholder_description, quit_description);
  }
  psprecomp::patch::call_original<void>(state.gpr[4], state.gpr[5]);
  return return_from_hook(state);
}

bool lobby_text_hook(psprecomp::State& state) {
  const auto number = state.gpr[6] & 0xffffU;
  if (!loaded(state, overlay::lobby_task) || state.gpr[5] != 0 || (number != quit_text && number != quit_description))
    return false;
  const auto receive = psprecomp::patch::call_original_game_function<std::uint32_t>(lobby_task::lobby_text.address,
                                                                                     state.gpr[4], 0U, receive_text);
  const Label* language = &labels[0];
  for (const auto& label : labels)
    if (read_text(state, receive) == label.receive) language = &label;
  const auto text = number == quit_text ? language->quit : language->description;
  const auto address = number == quit_text ? quit_text_address : quit_description_address;
  for (std::uint32_t i = 0; i < text.size(); ++i)
    psprecomp::store8(state, address + i, static_cast<std::uint8_t>(text[i]));
  psprecomp::store8(state, address + static_cast<std::uint32_t>(text.size()), 0);
  state.gpr[2] = address;
  return return_from_hook(state);
}

// render_text(renderer, x, y, color, format, text...): the row's text white, as a choice that can
// be made. The format's other values, if any, are in the rest of the registers and on the stack.
bool render_text_hook(psprecomp::State& state) {
  if (state.gpr[9] == quit_text_address) state.gpr[7] = text_white;
  psprecomp::patch::call_original<void>(state.gpr[4], state.gpr[5], state.gpr[6], state.gpr[7], state.gpr[8],
                                        state.gpr[9], state.gpr[10], state.gpr[11]);
  return return_from_hook(state);
}

Ref<Hunter> local_hunter(psprecomp::State& state) {
  const auto data = deref(state, global::game_data);
  const auto player = static_cast<std::int32_t>(data.get(GameData::local_player));
  return {state, call_game_function<std::uint32_t>(eboot::player_get, get(state, global::players).address(), player)};
}

bool choose_hook(psprecomp::State& state) {
  if ((state.gpr[5] & 0xffU) != quit_row || !loaded(state, overlay::lobby_task)) {
    state.gpr[2] = psprecomp::patch::call_original<std::uint32_t>(state.gpr[4], state.gpr[5]);
    return return_from_hook(state);
  }
  const auto window = state.gpr[4];
  const auto hunter = local_hunter(state);
  call_game_function(eboot::village_menu_close, window);
  call_game_function(lobby_task::hunter_set_action.address, hunter.address(), 0U, static_cast<std::uint32_t>(action_bed));
  step = Step::asking;
  state.gpr[2] = 0;  // chosen: the menu plays its sound, and finds itself closed next frame
  return return_from_hook(state);
}

void set_lobby_state(psprecomp::State& state, std::uint32_t value) {
  const auto manager = psprecomp::load32(state, lobby_manager);
  if (in_ram(manager, 4)) psprecomp::store32(state, manager, value);
}

void stand(psprecomp::State& state, const Ref<Hunter>& hunter) {
  step = Step::off;
  set_lobby_state(state, lobby_free);
  call_game_function(lobby_task::hunter_stand.address, hunter.address(), 0U, static_cast<std::uint32_t>(action_stand),
                     0U);
}

void leave() {
  step = Step::leaving;
  refract::frontend::request_exit();
}

bool bed_hook(psprecomp::State& state) {
  if (step == Step::off || !loaded(state, overlay::lobby_task)) return false;
  const Ref<Hunter> hunter{state, state.gpr[4]};
  if (hunter.address() != local_hunter(state).address()) return false;
  const auto save = get(state, global::save_manager).address();
  if (hunter.get(Character::action_phase) == 0) {
    call_game_function(eboot::save_prompt_start, save, 1U, 0U);
    hunter.set(Character::action_phase, 1);
  }
  if (step == Step::asking) {
    const auto answer = call_game_function<std::int32_t>(eboot::save_prompt_update, save);
    if (answer != 0) call_game_function(eboot::save_prompt_end);
    if (answer == 1) {
      hunter.set(Hunter::bed_save_step, 0);
      set_lobby_state(state, lobby_saving);
      step = Step::saving;
    } else if (answer == 2) {
      stand(state, hunter);
    } else if (answer == 3) {
      leave();
    }
  } else if (step == Step::saving) {
    hunter.set(Hunter::bed_no_exit_prompt, 0);
    const auto answer = call_game_function<std::int32_t>(lobby_task::bed_save_update.address, hunter.address());
    if (answer == 1) stand(state, hunter);
    else if (answer == 2) leave();
  }
  return return_from_hook(state);
}

psprecomp::patch::PatchNode draw_menu_node, lobby_text_node, render_text_node, choose_node, bed_node;

void start(int) {
  hook(draw_menu_node, eboot::village_menu_draw, "quit_game_menu", &draw_menu_hook);
  hook(lobby_text_node, lobby_task::lobby_text, "quit_game_text", &lobby_text_hook);
  hook(render_text_node, eboot::render_text, "quit_game_white", &render_text_hook);
  hook(choose_node, eboot::village_menu_choose, "quit_game_choose", &choose_hook);
  hook(bed_node, lobby_task::hunter_bed, "quit_game_bed", &bed_hook);
}

}  // namespace

MHFU_FEATURE(quit_game,
             {"quit_game", "Quit Game in the menu",
              "Adds Quit Game to the village's START menu, under Receive: it asks what the bed asks (whether to "
              "save, then whether to exit) and then closes the game. Off is the original menu, with an empty row "
              "there.",
              true, &start});

}  // namespace mhfu
