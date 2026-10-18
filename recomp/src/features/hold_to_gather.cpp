// Hold to gather, like the later Monster Hunter games: hold the button at a gathering spot and the
// hunter goes on gathering until the spot runs out. Herbs, mushrooms, bones and the like are
// gathered with Circle; mining and catching bugs use Square, with a pickaxe or bug net chosen in
// the item bar.
//
// The game copies the pad into the hunter each frame (hunter_read_pad), and the hunter acts on the
// buttons pressed that frame (hunter_sheathed_input: Circle gathers, Square uses the item). While
// the button stays held, with the hunter standing at a gather point that has gathers left, this
// counts it as pressed again. So the game starts the next gather itself, by its own rules
// (hunter_try_gather), as soon as the last one is over and the hunter stands again.

#include "../features.hpp"
#include "../game.hpp"
#include "../hook.hpp"

#include <cstdint>
#include <optional>

namespace mhfu {
namespace {

using namespace game;

constexpr std::uint32_t circle = 0x2000U;
constexpr std::uint32_t square = 0x8000U;

constexpr std::int16_t kind_gather = 0;
constexpr std::int16_t kind_mine = 3;
constexpr std::int16_t kind_bugs = 4;

bool is_pickaxe(std::int16_t item) { return item == 0x90 || item == 0x91 || item == 0x92 || item == 0x4c9; }
bool is_bug_net(std::int16_t item) { return item == 0x94 || item == 0x95 || item == 0x96 || item == 0x4ca; }

// The gather point the hunter stands at: the first of the area's in reach, as gather_point_at
// finds it.
std::optional<Ref<GatherPoint>> gather_point_at(psprecomp::State& state, const Ref<Hunter>& hunter) {
  const auto quest = deref(state, global::quest);
  if (!quest.valid()) return std::nullopt;
  const auto list = psprecomp::patch::call_game_function<Address>(
      eboot::quest_gather_points, quest.address(), static_cast<std::uint32_t>(hunter.get(Character::area)));
  if (psprecomp::patch::last_call_error() != psprecomp::patch::CallError::none || list == 0U) return std::nullopt;
  const auto at = hunter.get(Character::position);
  for (Ref<GatherPoint> point{state, list}; point.valid(); point = {state, point.address() + size_of<GatherPoint>}) {
    const auto position = point.get(GatherPoint::position);
    if (position.x == -1.0F) break;  // the end of the list
    const float dx = at.x - position.x, dz = at.z - position.z, radius = point.get(GatherPoint::radius);
    if (at.y >= position.y - 200.0F && at.y < position.y + 100.0F && dx * dx + dz * dz <= radius * radius)
      return point;
  }
  return std::nullopt;
}

// The button that gathers at the point, if the hunter can: Circle, or Square with the right tool.
std::uint32_t gather_button(psprecomp::State& state, const Ref<Hunter>& hunter, const Ref<GatherPoint>& point) {
  const auto kind = point.get(GatherPoint::kind);
  if (kind == kind_gather) return circle;
  if (kind != kind_mine && kind != kind_bugs) return 0U;
  const auto slot = hunter.get(Hunter::selected_slot);
  if (slot < 0 || slot >= 25) return 0U;
  const auto item = deref(state, global::game_data).inner(at(GameData::item_pouch, static_cast<std::uint32_t>(slot)));
  if (!item.valid()) return 0U;
  const auto id = item.get(PouchSlot::item);
  return (kind == kind_mine ? is_pickaxe(id) : is_bug_net(id)) ? square : 0U;
}

void keep_gathering(psprecomp::State& state, const Ref<Hunter>& hunter) {
  const auto data = deref(state, global::game_data);
  if (!data.valid() || hunter.get(Character::player) != data.get(GameData::local_player)) return;
  // Standing with the weapon sheathed: what a gather goes back to when it's over.
  if (hunter.get(Character::action_group) != 0U || hunter.get(Character::action_id) != 0U) return;
  const auto held = hunter.get(Hunter::pad_held);
  if ((held & (circle | square)) == 0U) return;
  const auto point = gather_point_at(state, hunter);
  if (!point || point->get(GatherPoint::gathers_left) == 0) return;  // nothing left: stop
  const auto button = gather_button(state, hunter, *point);
  const auto pressed = hunter.get(Hunter::pad_pressed);
  if (button == 0U || (held & button) == 0U || (pressed & button) != 0U) return;
  hunter.set(Hunter::pad_pressed, pressed | button);
}

psprecomp::patch::PatchNode read_pad_node;

bool read_pad_hook(psprecomp::State& state) {
  const Ref<Hunter> hunter{state, state.gpr[4]};
  psprecomp::patch::call_original<void>(hunter.address());
  // Gather points are a hunt's (game_task); the village has none.
  if (hunter.reaches(Hunter::selected_slot) && loaded(state, overlay::game_task)) keep_gathering(state, hunter);
  return return_from_hook(state);
}

void start(int) { hook(read_pad_node, eboot::hunter_read_pad, "hold_to_gather", &read_pad_hook); }

}  // namespace

MHFU_FEATURE(hold_to_gather,
             {"hold_to_gather", "Hold to gather",
              "Hold the button at a gathering spot on a hunt and the hunter keeps gathering until the spot runs "
              "out or you let go, as in the later games (Circle for herbs and bones; Square with a pickaxe or bug "
              "net chosen). Off is the original: one press, one gather.",
              false, &start});

}  // namespace mhfu
