// Real 60 fps: what the community 60 fps code leaves running at twice the speed.
//
// MHFU counts time in 1/60 s: at 30 fps everything steps by 2 a frame. The community code
// (cheats.hpp) halves those steps and switches the game to 60 frames a second, but some counters
// still step as at 30 fps, so what they time happens twice as fast at 60. Each fix here runs the
// game's function as it is and adjusts its counters around it, so that over two 60 fps frames
// they move as they did over one 30 fps frame. The names are those of re/ (game.hpp).
//
// - main_loop adds 1 to Game.frame_count each frame. The game uses it for things that pulse,
//   blink and turn by themselves (icons, effects): at 60 it now advances every other frame.
// - character_update, every character's update on a hunt, adds 2 to its action_time and its
//   held_time, and counts its countdown down by 1: now 1, 1 and every other frame. In the village
//   the update is villager_update: the same, and its cycle (a count to 100), now every other
//   frame; villager_motion_update counts a villager's motion_time, now every other frame.
// - play_time_update counts frames and adds a second of play time every 30: it now runs every
//   other frame. Effects that pulse or turn by themselves, and timers that act every so many
//   frames, read its frame count too.
// - What animates by itself: texture animations (texture_animation_update) and flipbooks
//   (lobby_flipbook_update, hunt_flipbook_update) now step every other frame; the marker bobbing
//   over a character (marker_update) and an area's effect timers and turn (stage_update) at half
//   the step.
// - On a hunt, the hunter's timers (hunter_update_timers: how long items and ailments last,
//   hunger, health recovering every so many frames) now run every other frame. Its stamina
//   changes go through the 60 fps code, which adds small changes only every other call (they come
//   every frame): there they are all added.
// - The village's task (lobby_task_update) waits in some of its states by counting its wait down
//   a frame at a time (the opening's view of the village lasts 599 frames): at 60 the count goes
//   down every other frame.
// - Monsters on a hunt: monster_update (with each species' code) counts timers a frame at a time:
//   how long ailments and traps hold the monster, rage, and the steps of its actions
//   (action_timer: how long it stays put, runs, waits before attacking...). At 60 they count
//   every other frame. Some steps move the monster by a speed (and an acceleration) a frame
//   (monster_fall, monster_move_facing, ...): at 60 those take half a step each frame.
// - A hunter's actions count their steps the same way (action_timer, action_countdown: drawing
//   and sheathing the weapon...): at 60 they count every other frame.
// - Cutscene cameras follow a script (event_camera_run_script): each step lasts step_length
//   frames, counted down in step_left, and the camera is placed by how much of the count is left.
//   At 60 a step is made to last twice as many frames, so the camera moves at the same speed and
//   still every frame. Other cutscene cameras follow a track of keys
//   (event_camera_place_on_track) at times in 1/60 s, at twice the frames counted in step_left:
//   while it runs at 60, the keys' times are doubled.
// - Text types out by the time since it started (message_type_out's frames, +1 a frame, each
//   letter taking 3): at 60 it gets half that time.
// - motion_start blends a character's new motion in over a number of frames (blend + 1): at 60 it
//   is given twice as many, so motions blend into each other as smoothly and for as long.
// - pad_read repeats held buttons for menus (pad_repeat) after 13 frames of holding them, then
//   every 3, counting in the pad's repeat_count: at 60 it counts every other frame, so a held
//   direction moves a menu's cursor as often as at 30.
// - On a hunt, L let go within 10 frames of pressing it (hunter_tapped_l, by the hunter's
//   l_held_frames) turns the camera back; held longer, it only showed the item bar. At 60 the test
//   is for 20 frames, so a tap is as long as at 30.
// Overlays share addresses, so a hook in one checks that the overlay is loaded and the code is as
// expected (game::loaded), and otherwise lets the instruction run.
#include "fps60.hpp"

#include "game.hpp"

#include <psprecomp/patch.hpp>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <type_traits>

namespace mhfu {
namespace {

using namespace game;

// The 60 fps code is on while it has written this instruction (lui v0, 1.0: the hunter's motion
// speed, in hunter_motion_update), and the game runs at 60 while no video plays.
constexpr Address frame_step_instruction = 0x088d5df8;
constexpr std::uint32_t frame_step_at_60 = 0x3c023f80U;

// A count an update steps by 1 a frame: down while it is over 0, or up.
struct Count {
  std::uint32_t offset;
  std::uint8_t size;
  bool is_signed;
  std::int32_t most;
};

template <typename O, typename T>
constexpr Count count(Field<O, T> field) {
  return {field.offset, sizeof(T), std::is_signed_v<T>, static_cast<std::int32_t>(std::numeric_limits<T>::max())};
}

// A monster's: how long ailments and traps hold it, rage, others, and the step of its action.
constexpr Count monster_counts[] = {
    count(Monster::timer_4b4),       count(Monster::timer_4b6),       count(Monster::timer_4c0),
    count(Monster::timer_512),       count(Monster::timer_529),       count(Monster::timer_575),
    count(Monster::timer_57a),       count(Monster::timer_5f0),       count(Monster::timer_5f2),
    count(Monster::timer_634),       count(Monster::timer_635),       count(Monster::timer_4c4),
    count(Monster::timer_54e),       count(Monster::timer_550),       count(Monster::rage_timer),
    count(Monster::ailment_timer_2), count(Monster::ailment_timer_1), count(Monster::ailment_timer_4),
    count(Monster::ailment_timer_5), count(Character::action_timer),
};

// A hunter's: the steps of its actions (drawing and sheathing the weapon...).
constexpr Count hunter_counts[] = {count(Character::action_timer), count(Character::action_countdown)};

constexpr std::int32_t longest_step = 0x3fff;  // doubled, it still fits

// A camera track's keys: 48 bytes each, starting with their time; one under 0 ends them.
constexpr std::uint32_t track_key_size = 48;
constexpr std::uint32_t most_track_keys = 4096;

// message_type_out's frames: this and over (-1) shows all of the message.
constexpr std::uint32_t whole_message = 0x80000000U;

psprecomp::patch::PatchNode end_of_frame_node;
psprecomp::patch::PatchNode character_update_node;
psprecomp::patch::PatchNode villager_update_node;
psprecomp::patch::PatchNode villager_motion_update_node;
psprecomp::patch::PatchNode play_time_update_node;
psprecomp::patch::PatchNode texture_animation_update_node;
psprecomp::patch::PatchNode lobby_flipbook_update_node;
psprecomp::patch::PatchNode hunt_flipbook_update_node;
psprecomp::patch::PatchNode marker_update_node;
psprecomp::patch::PatchNode stage_update_node;
psprecomp::patch::PatchNode message_type_out_node;
psprecomp::patch::PatchNode event_camera_run_script_node;
psprecomp::patch::PatchNode lobby_task_update_node;
psprecomp::patch::PatchNode event_camera_place_on_track_node;
psprecomp::patch::PatchNode hunter_update_timers_node;
psprecomp::patch::PatchNode hunter_add_stamina_node;
psprecomp::patch::PatchNode monster_update_node;
psprecomp::patch::PatchNode pad_read_node;
psprecomp::patch::PatchNode motion_start_node;
psprecomp::patch::PatchNode hunter_frame_node;
psprecomp::patch::PatchNode monster_fall_node;
psprecomp::patch::PatchNode monster_move_facing_node;
psprecomp::patch::PatchNode monster_move_facing_2_node;
psprecomp::patch::PatchNode monster_timed_moves_node;
psprecomp::patch::PatchNode monster_flying_action_node;
psprecomp::patch::PatchNode monster_sliding_action_node;

// The game's thread only: true on the frames where a 30 fps frame's worth of time ends.
bool second_half{};
// While the hunter's timers run.
bool in_hunter_timers{};

// The step each cutscene camera was last made twice as long in.
struct LongerStep {
  Address camera;
  Address commands;
  std::int32_t length;
};
LongerStep longer_steps[4]{};

bool at_60(psprecomp::State& state) {
  return psprecomp::load32(state, frame_step_instruction) == frame_step_at_60 &&
         get(state, global::video_playing) == 0U;
}

// Makes a 16-bit count the function adds 1 to stay put this frame, every other frame.
template <typename S, typename O>
void hold_every_other_frame(Ref<S> object, Field<O, std::uint16_t> count) {
  if (second_half) return;
  object.set(count, object.get(count) - 1U);
}

// Returns from the hooked function as the original would have.
bool finish(psprecomp::State& state) {
  if (state.stop_reason == psprecomp::StopReason::running) {
    state.pc = state.gpr[31];
    state.branch_pending = false;
  }
  return true;
}

bool end_of_frame_hook(psprecomp::State& state) {
  psprecomp::patch::call_original<void>(state.gpr[4]);
  if (!at_60(state)) return finish(state);
  second_half = !second_half;
  // The main loop adds 1 to the frame count next: take it back every other frame.
  const auto the_game = deref(state, global::game);
  if (!second_half && the_game.reaches(Game::frame_count))
    the_game.set(Game::frame_count, the_game.get(Game::frame_count) - 1U);
  return finish(state);
}

// What character_update and villager_update add to a character, at half the step.
template <typename S>
void half_character_step(Ref<S> character) {
  // The update adds 2 to the action's time and counts the countdown down by 1.
  character.set(Character::action_time, character.get(Character::action_time) - 1);
  const auto count = character.get(Character::countdown);
  if (count != 0U && !second_half) character.set(Character::countdown, count + 1U);
}

std::int32_t load_count(psprecomp::State& state, Address object, const Count& count) {
  const auto address = object + count.offset;
  if (count.size == 1U) return count.is_signed ? load<std::int8_t>(state, address) : load<std::uint8_t>(state, address);
  if (count.size == 2U) return count.is_signed ? load<std::int16_t>(state, address) : load<std::uint16_t>(state, address);
  return load<std::int32_t>(state, address);
}

void store_count(psprecomp::State& state, Address object, const Count& count, std::int32_t value) {
  const auto address = object + count.offset;
  if (count.size == 1U) store<std::uint8_t>(state, address, value);
  else if (count.size == 2U) store<std::uint16_t>(state, address, value);
  else store<std::int32_t>(state, address, value);
}

// Makes counts an update steps by 1 a frame step every other frame. On the frames in between,
// each over 0 is raised by 1 first, so that counting down leaves it where it was (and it can't
// reach 0 and act on that twice); afterwards one left alone or counted up is put back. One the
// update sets is kept.
template <std::size_t N>
class HeldCounts {
 public:
  HeldCounts(psprecomp::State& state, Address object, const Count (&counts)[N])
      : state_(state), object_(object), counts_(counts) {
    for (std::size_t i = 0; i < N; ++i) {
      before_[i] = load_count(state_, object_, counts_[i]);
      raised_[i] = before_[i] > 0 && before_[i] < counts_[i].most - 1;
      if (raised_[i]) store_count(state_, object_, counts_[i], before_[i] + 1);
    }
  }

  void settle() {
    for (std::size_t i = 0; i < N; ++i) {
      const std::int64_t after = load_count(state_, object_, counts_[i]);
      const std::int64_t before = before_[i];
      if (after == before + 1 || (raised_[i] && after == before + 2)) store_count(state_, object_, counts_[i], before_[i]);
    }
  }

 private:
  psprecomp::State& state_;
  Address object_;
  const Count (&counts_)[N];
  std::int32_t before_[N]{};
  bool raised_[N]{};
};

bool character_update_hook(psprecomp::State& state) {
  const Ref<Character> character{state, state.gpr[4]};
  if (character.valid() && character.get(Character::paused) == 0U && at_60(state)) {
    half_character_step(character);
    character.set(Character::held_time, character.get(Character::held_time) - 1U);  // +2 too
  }
  psprecomp::patch::call_original<void>(character.address());
  return finish(state);
}

// Speeds some of the hunter's actions give their motions themselves (frames of the motion a
// 30 fps frame), which the 60 fps code doesn't halve as it does the others: gathering, mining
// and catching bugs (hunter_gather_action and FUN_09aa14c0: 2.2, 2.6 and 2.8), FUN_09aa2f78's
// 3.6 (with skill 0xad) and FUN_09a9eef8's 1.6 and 2.6 (its twin FUN_09a9ecd8's are halved).
// Each is the lui of the speed's upper half: one off the immediate's exponent halves it, like the
// 60 fps code's own. Overlays run in the interpreter, so the changed code runs; it's changed
// again each time game_task is loaded.
struct ActionSpeed {
  Address address;
  std::uint32_t lui;  // the instruction as it is in game_task
};
constexpr ActionSpeed action_speeds[] = {
    {0x09aa0df8U, 0x3c03400cU}, {0x09aa0dfcU, 0x3c034026U}, {0x09aa0e88U, 0x3c03400cU},
    {0x09aa0e8cU, 0x3c034026U}, {0x09aa0ef8U, 0x3c03400cU}, {0x09aa0f04U, 0x3c034033U},
    {0x09aa0f54U, 0x3c03400cU}, {0x09aa0f60U, 0x3c034033U}, {0x09aa161cU, 0x3c03400cU},
    {0x09aa1620U, 0x3c034026U}, {0x09aa31b8U, 0x3c034066U}, {0x09a9ef1cU, 0x3c023fccU},
    {0x09a9ef30U, 0x3c024026U},
};

// hunter_tapped_l's test of the frames L was held, changed with the speeds.
constexpr Address tap_test = game_task::hunter_tapped_l.address + 0x60U;
constexpr std::uint32_t tap_test_at_30 = 0x2841000aU;  // slti at, v0, 10
constexpr std::uint32_t tap_test_at_60 = 0x28410014U;  // slti at, v0, 20

void change_code_for_60(psprecomp::State& state) {
  for (const auto& speed : action_speeds)
    if (psprecomp::load32(state, speed.address) == speed.lui) psprecomp::store32(state, speed.address, speed.lui - 0x80U);
  if (psprecomp::load32(state, tap_test) == tap_test_at_30) psprecomp::store32(state, tap_test, tap_test_at_60);
}

// Runs a hunter's frame with the counts of its actions (hunter_counts) stepping every other frame.
bool hunter_frame_hook(psprecomp::State& state) {
  if (!loaded(state, game_task::hunter_frame, 0xc, 0x0e69a1b0U, 0x00808021U)) return false;
  if (at_60(state)) change_code_for_60(state);
  const Ref<Hunter> hunter{state, state.gpr[4]};
  if (!hunter.reaches(Character::action_timer) || !at_60(state) || second_half) return false;  // run it
  HeldCounts held{state, hunter.address(), hunter_counts};
  psprecomp::patch::call_original<void>(hunter.address());
  held.settle();
  return finish(state);
}

bool villager_update_hook(psprecomp::State& state) {
  if (!loaded(state, lobby_task::villager_update, 0, 0x9083033cU, 0x14600017U)) return false;  // not this code
  const Ref<Villager> villager{state, state.gpr[4]};
  if (villager.valid() && villager.get(Character::paused) == 0U && at_60(state)) {
    half_character_step(villager);
    hold_every_other_frame(villager, Villager::cycle);
  }
  psprecomp::patch::call_original<void>(villager.address());
  return finish(state);
}

bool villager_motion_update_hook(psprecomp::State& state) {
  if (!loaded(state, lobby_task::villager_motion_update, 0, 0x27bdfff0U, 0xafbf000cU)) return false;
  const Ref<Villager> villager{state, state.gpr[4]};
  if (villager.valid() && at_60(state)) hold_every_other_frame(villager, Villager::motion_time);
  psprecomp::patch::call_original<void>(villager.address());
  return finish(state);
}

bool play_time_update_hook(psprecomp::State& state) {
  // It adds a second when the frame count reaches a multiple of 30: it runs every other frame.
  if (at_60(state) && !second_half) return finish(state);
  psprecomp::patch::call_original<void>(state.gpr[4]);
  return finish(state);
}

// For functions that only step a count: at 60 they run every other frame.
bool every_other_frame(psprecomp::State& state) {
  if (second_half || !at_60(state)) return false;  // run it
  return finish(state);
}

bool texture_animation_update_hook(psprecomp::State& state) {
  if (!loaded(state, game_sub::texture_animation_update, 0, 0x8c850028U, 0x30a30038U)) return false;
  return every_other_frame(state);
}

bool lobby_flipbook_update_hook(psprecomp::State& state) {
  if (!loaded(state, lobby_task::lobby_flipbook_update, 0, 0x8c830034U, 0x94850038U)) return false;
  return every_other_frame(state);
}

bool hunt_flipbook_update_hook(psprecomp::State& state) {
  if (!loaded(state, game_sub::hunt_flipbook_update, 0, 0x8c830034U, 0x94850038U)) return false;
  return every_other_frame(state);
}

bool marker_update_hook(psprecomp::State& state) {
  if (!loaded(state, game_sub::marker_update, 0x10, 0x8c850048U, 0x2403ffefU)) return false;
  const Ref<Marker> marker{state, state.gpr[4]};
  if (!marker.reaches(Marker::phase) || !at_60(state)) return false;
  const auto phase = marker.get(Marker::phase);
  psprecomp::patch::call_original<void>(marker.address());
  // It adds 1 when the marker shows: take it back every other frame.
  if (!second_half && marker.get(Marker::phase) == static_cast<std::int16_t>(phase + 1)) marker.set(Marker::phase, phase);
  return finish(state);
}

bool stage_update_hook(psprecomp::State& state) {
  const Ref<Stage> stage{state, state.gpr[4]};
  if (!stage.valid() || !at_60(state)) {
    psprecomp::patch::call_original<void>(stage.address());
    return finish(state);
  }
  const auto count = stage.get(Stage::effect_count);
  if (!second_half) {
    // The count stays put this frame. The update reads it before adding 1, and starts effects when
    // it is a multiple of 128: it read this count last frame, so here it must not start them again.
    const auto last = static_cast<std::uint16_t>(count - 1U);
    stage.set(Stage::effect_count, (last & 0x7fU) == 0U ? count : last);
    if (stage.get(Stage::effect_state) != 0) stage.set(Stage::effect_timer, stage.get(Stage::effect_timer) + 1);
  }
  stage.set(Stage::effect_turn, stage.get(Stage::effect_turn) - 0x2eU / 2U);  // it adds 0x2e
  psprecomp::patch::call_original<void>(stage.address());
  if (!second_half) stage.set(Stage::effect_count, count);
  return finish(state);
}

bool hunter_update_timers_hook(psprecomp::State& state) {
  if (!loaded(state, game_task::hunter_update_timers, 0x18, 0x0e235922U, 0x00808021U) || !at_60(state)) return false;
  if (!second_half) return finish(state);
  const auto every_other = get(state, global::fps60_stamina_every_other);
  in_hunter_timers = true;
  psprecomp::patch::call_original<void>(state.gpr[4]);
  in_hunter_timers = false;
  set(state, global::fps60_stamina_every_other, every_other);
  return finish(state);
}

bool hunter_add_stamina_hook(psprecomp::State& state) {
  // Called from the hunter's timers: the next change goes through.
  if (in_hunter_timers && loaded(state, game_task::hunter_add_stamina, 0x10, 0x3c03089dU, 0x8c63c438U))
    set(state, global::fps60_stamina_every_other, 0U);
  return false;  // run it
}

bool lobby_task_update_hook(psprecomp::State& state) {
  if (!loaded(state, lobby_task::lobby_task_update, 0x18, 0x00808821U, 0x3c0209a5U)) return false;
  const Ref<Task> task{state, state.gpr[4]};
  if (!task.valid() || !at_60(state) || second_half) return false;
  const auto task_state = task.get(Task::state);
  const auto wait = task.get(Task::wait);
  psprecomp::patch::call_original<void>(task.address());
  // Counted down in the same state: take it back.
  if (task.get(Task::state) == task_state && task.get(Task::wait) == wait - 1) task.set(Task::wait, wait);
  return finish(state);
}

bool event_camera_run_script_hook(psprecomp::State& state) {
  const Ref<EventCamera> camera{state, state.gpr[4]};
  psprecomp::patch::call_original<void>(camera.address());  // its result stays in v0
  if (!camera.reaches(EventCamera::step_commands) || !at_60(state)) return finish(state);
  const std::int32_t length = camera.get(EventCamera::step_length);
  const auto commands = camera.get(EventCamera::step_commands).address();
  auto* last = &longer_steps[0];
  for (auto& step : longer_steps)
    if (step.camera == camera.address()) last = &step;
  if (length <= 0 || length > longest_step ||
      (last->camera == camera.address() && length == last->length && commands == last->commands))
    return finish(state);
  // A step began this frame, and ran its first frame: from here it takes twice as many.
  const std::int32_t left = camera.get(EventCamera::step_left);
  camera.set(EventCamera::step_length, 2 * length);
  camera.set(EventCamera::step_left, 2 * left + 1);
  if (last->camera != camera.address()) {  // a camera not seen yet takes the oldest place
    for (std::size_t i = std::size(longer_steps) - 1U; i > 0U; --i) longer_steps[i] = longer_steps[i - 1U];
    last = &longer_steps[0];
  }
  *last = {camera.address(), commands, 2 * length};
  return finish(state);
}

// Multiplies (or divides) the times of a camera track's keys by 2.
void scale_track_times(psprecomp::State& state, Address keys, bool up) {
  for (std::uint32_t n = 0; n < most_track_keys && in_ram(keys, 4U); ++n, keys += track_key_size) {
    const auto time = load<std::int32_t>(state, keys);
    if (time < 0) return;
    store<std::int32_t>(state, keys, up ? time * 2 : time / 2);
  }
}

bool event_camera_place_on_track_hook(psprecomp::State& state) {
  const Ref<EventCamera> camera{state, state.gpr[4]};
  const auto keys = camera.reaches(EventCamera::commands) ? camera.get(EventCamera::commands).address() : 0U;
  const bool slower = keys != 0U && at_60(state);
  if (slower) scale_track_times(state, keys, true);
  psprecomp::patch::call_original<void>(camera.address());  // its result stays in v0
  if (slower) scale_track_times(state, keys, false);
  return finish(state);
}

// Runs a monster's update with its counts (monster_counts) stepping every other frame.
bool monster_update_hook(psprecomp::State& state) {
  if (!loaded(state, game_task::monster_update, 0x10, 0x00809021U, 0x0e6b1bbeU)) return false;
  const Ref<Monster> monster{state, state.gpr[4]};
  if (!monster.valid() || !at_60(state)) {
    psprecomp::patch::call_original<void>(monster.address());
    return finish(state);
  }
  // The 60 fps code makes one of the monster's countdowns count every other call, by a word it
  // flips: from 0 it counts, so that the count here alone makes it every other frame.
  set(state, global::fps60_monster_count_every_other, 0U);
  if (second_half) {
    psprecomp::patch::call_original<void>(monster.address());
    return finish(state);
  }
  HeldCounts held{state, monster.address(), monster_counts};
  psprecomp::patch::call_original<void>(monster.address());
  held.settle();
  return finish(state);
}

// Runs a function that moves a character by its speed (position += speed, speed += acceleration,
// in either order) half a step: the speed halved and the acceleration quartered for the call,
// then the speed doubled back. Over two 60 fps frames it moves as over one 30 fps frame (the
// game's own steps are as long as an action's count, which now counts every other frame). A
// speed or acceleration it sets is kept.
bool half_speed_step(psprecomp::State& state) {
  const Ref<Character> character{state, state.gpr[4]};
  if (!character.valid() || !at_60(state)) return false;  // run it
  const auto speed = character.get(Character::speed);
  const auto acceleration = character.get(Character::acceleration);
  const Vec3 half_speed{speed.x * 0.5F, speed.y * 0.5F, speed.z * 0.5F};
  const Vec3 quarter_acceleration{acceleration.x * 0.25F, acceleration.y * 0.25F, acceleration.z * 0.25F};
  character.set(Character::speed, half_speed);
  character.set(Character::acceleration, quarter_acceleration);
  psprecomp::patch::call_original<void>(state.gpr[4], state.gpr[5], state.gpr[6], state.gpr[7]);  // v0 kept
  const auto stepped = [](float in, float added, float out) { return out == in || out == in + added; };
  auto new_speed = character.get(Character::speed);
  if (stepped(half_speed.x, quarter_acceleration.x, new_speed.x)) new_speed.x *= 2.0F;
  if (stepped(half_speed.y, quarter_acceleration.y, new_speed.y)) new_speed.y *= 2.0F;
  if (stepped(half_speed.z, quarter_acceleration.z, new_speed.z)) new_speed.z *= 2.0F;
  character.set(Character::speed, new_speed);
  auto new_acceleration = character.get(Character::acceleration);
  if (new_acceleration.x == quarter_acceleration.x) new_acceleration.x = acceleration.x;
  if (new_acceleration.y == quarter_acceleration.y) new_acceleration.y = acceleration.y;
  if (new_acceleration.z == quarter_acceleration.z) new_acceleration.z = acceleration.z;
  character.set(Character::acceleration, new_acceleration);
  return finish(state);
}

bool monster_fall_hook(psprecomp::State& state) {
  if (!loaded(state, game_task::monster_fall, 0xc, 0xc4810250U, 0xc4800240U)) return false;
  return half_speed_step(state);
}

bool monster_move_facing_hook(psprecomp::State& state) {
  if (!loaded(state, game_task::monster_move_facing, 0x14, 0xc48101f4U, 0x3c0343b4U)) return false;
  return half_speed_step(state);
}

bool monster_move_facing_2_hook(psprecomp::State& state) {
  if (!loaded(state, game_task::monster_move_facing_2, 0x14, 0xc48101f4U, 0x3c0343b4U)) return false;
  return half_speed_step(state);
}

bool monster_timed_moves_hook(psprecomp::State& state) {
  if (!loaded(state, game_task::monster_timed_moves, 0xc, 0x24050005U, 0xa08504b4U)) return false;
  return half_speed_step(state);
}

bool monster_flying_action_hook(psprecomp::State& state) {
  if (!loaded(state, game_task::monster_flying_action, 0x14, 0x00a08821U, 0x908501e8U)) return false;
  return half_speed_step(state);
}

bool monster_sliding_action_hook(psprecomp::State& state) {
  if (!loaded(state, game_task::monster_sliding_action, 0x14, 0x24080002U, 0xa48802baU)) return false;
  return half_speed_step(state);
}

bool message_type_out_hook(psprecomp::State& state) {
  auto frames = state.gpr[6];
  if (frames < whole_message && at_60(state)) frames /= 2U;
  psprecomp::patch::call_original<void>(state.gpr[4], state.gpr[5], frames, state.gpr[7], state.gpr[8]);
  return finish(state);
}

bool pad_read_hook(psprecomp::State& state) {
  const Ref<Pad> pad{state, state.gpr[4]};
  // It adds 1 to the count while the buttons stay the same (and puts it at 0 when they change):
  // every other frame, 1 less first keeps it where it was. A count of 0 goes round to 255 and back.
  if (pad.valid() && at_60(state) && !second_half) pad.set(Pad::repeat_count, pad.get(Pad::repeat_count) - 1U);
  psprecomp::patch::call_original<void>(pad.address());
  return finish(state);
}

bool motion_start_hook(psprecomp::State& state) {
  // (motion, data, frame, blend, layer): blend + 1 frames of blending, 0 for none, under 0 the other way.
  auto blend = static_cast<std::int32_t>(state.gpr[7]);
  if (blend != 0 && blend > -0x4000 && blend < 0x4000 && at_60(state)) blend = blend > 0 ? 2 * blend + 1 : 2 * blend - 1;
  psprecomp::patch::call_original<void>(state.gpr[4], state.gpr[5], state.gpr[6], static_cast<std::uint32_t>(blend),
                                        state.gpr[8]);
  return finish(state);
}

void hook(psprecomp::patch::PatchNode& node, Address address, const char* name, bool (*function)(psprecomp::State&)) {
  psprecomp::patch::register_raw_patch(node, address, name, function);
}

void hook(psprecomp::patch::PatchNode& node, OverlayFunction function, const char* name,
          bool (*hook_function)(psprecomp::State&)) {
  hook(node, function.address, name, hook_function);
}

}  // namespace

void set_real_60fps(bool enabled) {
  static bool registered = false;
  if (!enabled || registered) return;
  hook(end_of_frame_node, eboot::end_of_frame, "fps60_end_of_frame", &end_of_frame_hook);
  hook(character_update_node, eboot::character_update, "fps60_character_update", &character_update_hook);
  hook(villager_update_node, lobby_task::villager_update, "fps60_villager_update", &villager_update_hook);
  hook(villager_motion_update_node, lobby_task::villager_motion_update, "fps60_villager_motion_update",
       &villager_motion_update_hook);
  hook(play_time_update_node, eboot::play_time_update, "fps60_play_time_update", &play_time_update_hook);
  hook(texture_animation_update_node, game_sub::texture_animation_update, "fps60_texture_animation_update",
       &texture_animation_update_hook);
  hook(lobby_flipbook_update_node, lobby_task::lobby_flipbook_update, "fps60_lobby_flipbook_update",
       &lobby_flipbook_update_hook);
  hook(hunt_flipbook_update_node, game_sub::hunt_flipbook_update, "fps60_hunt_flipbook_update",
       &hunt_flipbook_update_hook);
  hook(marker_update_node, game_sub::marker_update, "fps60_marker_update", &marker_update_hook);
  hook(stage_update_node, eboot::stage_update, "fps60_stage_update", &stage_update_hook);
  hook(message_type_out_node, eboot::message_type_out, "fps60_message_type_out", &message_type_out_hook);
  hook(hunter_update_timers_node, game_task::hunter_update_timers, "fps60_hunter_update_timers",
       &hunter_update_timers_hook);
  hook(hunter_add_stamina_node, game_task::hunter_add_stamina, "fps60_hunter_add_stamina", &hunter_add_stamina_hook);
  hook(event_camera_run_script_node, eboot::event_camera_run_script, "fps60_event_camera_run_script",
       &event_camera_run_script_hook);
  hook(lobby_task_update_node, lobby_task::lobby_task_update, "fps60_lobby_task_update", &lobby_task_update_hook);
  hook(event_camera_place_on_track_node, eboot::event_camera_place_on_track, "fps60_event_camera_place_on_track",
       &event_camera_place_on_track_hook);
  hook(monster_update_node, game_task::monster_update, "fps60_monster_update", &monster_update_hook);
  hook(pad_read_node, eboot::pad_read, "fps60_pad_read", &pad_read_hook);
  hook(motion_start_node, eboot::motion_start, "fps60_motion_start", &motion_start_hook);
  hook(hunter_frame_node, game_task::hunter_frame, "fps60_hunter_frame", &hunter_frame_hook);
  hook(monster_fall_node, game_task::monster_fall, "fps60_monster_fall", &monster_fall_hook);
  hook(monster_move_facing_node, game_task::monster_move_facing, "fps60_monster_move_facing", &monster_move_facing_hook);
  hook(monster_move_facing_2_node, game_task::monster_move_facing_2, "fps60_monster_move_facing_2",
       &monster_move_facing_2_hook);
  hook(monster_timed_moves_node, game_task::monster_timed_moves, "fps60_monster_timed_moves",
       &monster_timed_moves_hook);
  hook(monster_flying_action_node, game_task::monster_flying_action, "fps60_monster_flying_action",
       &monster_flying_action_hook);
  hook(monster_sliding_action_node, game_task::monster_sliding_action, "fps60_monster_sliding_action",
       &monster_sliding_action_hook);
  registered = true;
}

}  // namespace mhfu
