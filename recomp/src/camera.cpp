// Right-stick camera: a free camera like Monster Hunter 4 Ultimate's, for the hunting areas.
//
// The hunting areas' camera is updated once a game frame by field_camera_update, as one of the
// camera manager's six cameras; the manager shows it while its shown_camera is 0, and copies its
// eye and target to its own. Each frame the manager copies the pad's buttons to its held_buttons
// and pressed_buttons, and the camera takes them from there (field_camera_read_pad) while they are
// its own (field_camera_takes_pad: not while a message or the menu is up, the d-pad working it):
//   - left or right held: the camera's yaw (a 16-bit angle) turns by a fixed step, 0x47e a frame
//     (the 60 fps code halves it, the game's reverse-camera option flips it), and the camera
//     doesn't swing back behind the hunter that frame;
//   - up or down pressed: the camera steps between five heights (up: lower, looking up);
//   - L tapped (let go within 10 frames, hunter_tapped_l): the camera turns back behind the
//     hunter. Held, L only shows the item bar.
// Its shown_yaw follows the yaw a quarter of the way each frame. A height (CameraHeight) holds the
// eye's height above the hunter, its distance and the height of the point looked at. The eye's
// height eases towards the height's an eighth of the way a frame, is kept off the ground under it, is raised by up to 300 over high ground, and
// the point looked at moves to keep the hunter's head in view; so, tilted any way but the game's,
// the camera floats and jumps. The house and the village have other cameras, which the d-pad
// doesn't turn.
//
// This camera turns with the stick's or the d-pad's left and right (the d-pad as the stick pushed
// all the way): it holds left or right for the game's camera, so it takes its own d-pad path, and
// sets the yaw shown and the yaw beforehand so that the game's step lands where the stick says (the
// step scaled by how far the stick is pushed, learned from the frame before; on the first frame of
// a turn the yaw is put right afterwards). It tilts with the stick's or the d-pad's up and down,
// smoothly; the game's camera doesn't see up and down, so it keeps its height. While the game's
// camera doesn't take the pad, neither turns nor tilts it; L tapped tilts it back as the game turns
// it back. After the game's camera has updated, the eye is put at the height's distance from the
// point the game looks at, in the direction of the yaw shown, at the height's angle plus the tilt;
// then brought in front of walls with the game's own test for its camera (past trees), and in
// towards the hunter until it is a little above the floor (so looking up near the ground brings it
// in low).
// While the game's camera does something of its own (blending from another camera, the hunter
// carted or carving), it shows instead, faded to and from.
#include "camera.hpp"

#include "game.hpp"

#include <host/host.hpp>
#include <psprecomp/patch.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace mhfu {
namespace {

using namespace game;
using Vector = std::array<float, 3>;

// collision_sweep_sphere with the game's radius and flags for its camera.
constexpr float sweep_radius = 28.0F;
constexpr std::uint32_t sweep_flags = 0x8009U;

constexpr std::uint16_t pad_up = 0x10;
constexpr std::uint16_t pad_right = 0x20;
constexpr std::uint16_t pad_down = 0x40;
constexpr std::uint16_t pad_left = 0x80;
constexpr std::uint16_t pad_l = 0x100;

constexpr float pi = 3.14159265F;
constexpr float tilt_speed = 100.0F * pi / 180.0F;  // radians a second, the stick all the way
constexpr float lowest_view = -35.0F * pi / 180.0F;  // the eye's angle above the point looked at
constexpr float highest_view = 70.0F * pi / 180.0F;
constexpr float nearest_eye = 60.0F;  // walls bring the eye no nearer the point looked at
constexpr float floor_clearance = 40.0F;  // the eye's least height above the floor (the game's, if less)
constexpr float thin_obstacle = 120.0F;  // thinner than this, something in the way is looked past
constexpr float open_above = 200.0F;  // the eye's place must be open this far above
constexpr float fade_seconds = 0.25F;  // between the game's camera and this one

std::atomic<float> turn_speed{};
std::atomic<float> frame_seconds{1.0F / 30.0F};
psprecomp::patch::PatchNode hook_node;

// The game's thread only:
int left_step{};           // the game's turn a frame holding left (0: not known)
bool turned_last_frame{};  // the camera took the held direction the frame before
float tilt{};              // radians, added to the height's angle
bool untilting{};          // easing the tilt back to 0 (after L tapped, which turns the camera back)
float weight{};            // 0: the game's camera shows, 1: this one
bool following{};          // the eased values below are this camera's
float radius{}, angle{};   // the height's distance from the point looked at and angle, eased
float target_height{};     // the point looked at, eased
float distance{};          // the eye's distance from the point looked at, after walls

Vector vector(const Vec3& value) { return {value.x, value.y, value.z}; }

Vec3 vec3(const Vector& value) { return {value[0], value[1], value[2]}; }

// Moves `value` towards `goal` by `share` of the way a 30 fps frame.
void ease(float& value, float goal, float share) {
  value += (goal - value) * (1.0F - std::pow(1.0F - share, frame_seconds.load() * 30.0F));
}

// How far from `from` towards `to` (0 to 1) the game's camera test lets the eye go.
float clear_share(psprecomp::State& state, const Vector& from, const Vector& to) {
  using namespace psprecomp;
  const auto world = get(state, global::collision_world).address();
  const auto stack = state.gpr[29];
  const auto scratch = (stack - 0x40U) & ~0xfU;  // three vectors, below the stack, aligned
  if (world == 0U || !in_ram(scratch, 0x40U)) return 1.0F;
  store<Vec3>(state, scratch, vec3(from));
  store<float>(state, scratch + 0xcU, 0.0F);
  store<Vec3>(state, scratch + 0x10U, vec3(to));
  store<float>(state, scratch + 0x1cU, 0.0F);
  store<Vec3>(state, scratch + 0x20U, vec3(to));
  state.gpr[29] = scratch;
  const auto hit = patch::call_game_function<std::uint32_t>(game_sub::collision_sweep_sphere.address, sweep_radius,
                                                              world, scratch, scratch + 0x10U, scratch + 0x20U,
                                                              sweep_flags);
  const bool called = patch::last_call_error() == patch::CallError::none;
  const auto stop = vector(load<Vec3>(state, scratch + 0x20U));
  state.gpr[29] = stack;
  if (!called || (hit & 0xffU) == 0U) return 1.0F;
  const Vector way{to[0] - from[0], to[1] - from[1], to[2] - from[2]};
  const auto length_squared = way[0] * way[0] + way[1] * way[1] + way[2] * way[2];
  if (!(length_squared > 0.0F)) return 1.0F;
  const auto share = ((stop[0] - from[0]) * way[0] + (stop[1] - from[1]) * way[1] + (stop[2] - from[2]) * way[2]) /
                     length_squared;
  return std::isfinite(share) ? std::clamp(share, 0.0F, 1.0F) : 1.0F;
}

// The height of the floor under `point`, as the game's camera finds it: from a little higher if
// the floor found is below the hunter's feet (the point may be under the ground).
float floor_under(psprecomp::State& state, const Vector& point, float feet) {
  using namespace psprecomp;
  const auto world = get(state, global::collision_world).address();
  const auto stack = state.gpr[29];
  const auto scratch = (stack - 0x20U) & ~0xfU;
  if (world == 0U || !in_ram(scratch, 0x20U)) return -INFINITY;
  const auto query = [&](float height) {
    store<Vec3>(state, scratch, Vec3{point[0], height, point[2]});
    store<float>(state, scratch + 0xcU, 0.0F);
    state.gpr[29] = scratch;
    const auto found = patch::call_game_function<float>(game_sub::collision_floor_height.address, world, scratch);
    const bool called = patch::last_call_error() == patch::CallError::none;
    state.gpr[29] = stack;
    return called && std::isfinite(found) ? found : -INFINITY;
  };
  const auto found = query(point[1]);
  return found < feet ? query(std::max(point[1], feet) + 150.0F) : found;
}

// How far along `way` (a unit vector) from `look` the eye can go, up to `reach`, keeping `clearance`
// above the floor: what's under the eye comes nearer the hunter's as it comes in.
float floor_distance(psprecomp::State& state, const Vector& look, const Vector& way, float reach, float feet,
                     float clearance) {
  const auto clear_at = [&](float along) {
    const Vector eye{look[0] + way[0] * along, look[1] + way[1] * along, look[2] + way[2] * along};
    return eye[1] >= floor_under(state, eye, feet) + clearance;
  };
  if (clear_at(reach)) return reach;
  float clear = std::min(nearest_eye, reach), blocked = reach;
  if (way[1] < -0.01F) {
    // Looking up: first where the eye would be over a floor level with the hunter's feet.
    const auto level = (look[1] - (feet + clearance)) / -way[1];
    if (level > clear && level < blocked) {
      if (clear_at(level)) return level;
      blocked = level;
    }
  }
  for (int step = 0; step < 3; ++step) {
    const auto middle = (clear + blocked) * 0.5F;
    (clear_at(middle) ? clear : blocked) = middle;
  }
  return clear;
}

// How far along `way` (a unit vector) from `look` the eye can go, up to `reach`, in front of walls.
// Something thin in between (a tree) is looked past: the eye's place is open from above, and the
// test backwards from there stops just behind where it stopped forwards (it doesn't stop at all
// behind a wall, whose surfaces only face out).
float clear_distance(psprecomp::State& state, const Vector& look, const Vector& way, float reach) {
  const Vector eye{look[0] + way[0] * reach, look[1] + way[1] * reach, look[2] + way[2] * reach};
  const auto near = clear_share(state, look, eye) * reach;
  if (near >= reach) return reach;
  const auto back = clear_share(state, eye, look);
  if (back >= 1.0F || (1.0F - back) * reach - near > thin_obstacle) return near;
  const Vector above{eye[0], std::max(look[1], eye[1]) + open_above, eye[2]};
  return clear_share(state, above, eye) < 1.0F ? near : reach;
}

// Whether the game's camera takes the pad this frame, as field_camera_read_pad has it: not while a
// message or the menu is up (the d-pad then works the menu), in some of the hunter's actions, or
// while camera 5 runs.
bool takes_pad(Ref<CameraManager> manager, Ref<FieldCamera> camera) {
  using namespace psprecomp;
  if (!manager.get(CameraManager::hunter).valid()) return false;
  if (manager.inner(at(CameraManager::cameras, 5)).get(Camera::running) != 0U) return false;
  const auto takes = patch::call_game_function<std::uint32_t>(eboot::field_camera_takes_pad, camera.address());
  return patch::last_call_error() == patch::CallError::none && (takes & 0xffU) != 0U;
}

// Whether L was let go and the game's camera turns back behind the hunter for it: a tap
// (field_camera_should_recenter, with game_task's test); held longer, L only showed the item bar.
bool tapped_l(psprecomp::State& state, Ref<CameraManager> manager, Ref<FieldCamera> camera) {
  using namespace psprecomp;
  const auto hunter = manager.get(CameraManager::hunter);
  if (!hunter.valid() || (hunter.get(Hunter::pad_released) & pad_l) == 0U) return false;
  if (!loaded(state, game_task::hunter_tapped_l, 0x18, 0x8042ade5U, 0x1043000dU)) return false;
  const auto turns = patch::call_game_function<std::uint32_t>(eboot::field_camera_should_recenter, camera.address());
  return patch::last_call_error() == patch::CallError::none && (turns & 0xffU) != 0U;
}

// Turns the camera by `amount` (-1 to 1, right positive) of its step, through the game's d-pad
// path; the game's camera updates in the middle. The held buttons are the camera's, up and down
// already taken out.
void turn_and_update(Ref<CameraManager> manager, Ref<FieldCamera> camera, float amount, std::uint16_t held) {
  using namespace psprecomp;
  const std::uint16_t turn = amount > 0.0F ? pad_right : amount < 0.0F ? pad_left : 0U;
  if (turn == 0U) {
    turned_last_frame = false;
    manager.set(CameraManager::held_buttons, held);
    patch::call_original<void>(camera.address());
    return;
  }

  // How much of the game's step to turn by (a gentle curve, for small turns near the middle).
  const auto magnitude = std::min(std::fabs(amount), 1.0F);
  const auto share = magnitude * std::sqrt(magnitude) * turn_speed.load();
  const auto yaw_before = static_cast<std::int16_t>(camera.get(FieldCamera::yaw));
  const auto shown_before = static_cast<std::int16_t>(camera.get(FieldCamera::shown_yaw));
  int expected_turn = 0;  // the game's step this frame, if known
  int set_yaw = yaw_before;
  if (left_step != 0 && turned_last_frame) {
    // Turn on from the yaw shown, so that the game's step lands on the yaw shown plus the
    // stick's share of it, and the yaw shown stays with it.
    expected_turn = turn == pad_left ? left_step : -left_step;
    const auto turned = shown_before + static_cast<int>(std::lround(expected_turn * share));
    set_yaw = turned - expected_turn;
    camera.set(FieldCamera::yaw, set_yaw);
    camera.set(FieldCamera::shown_yaw, turned);
  }
  manager.set(CameraManager::held_buttons, (held & ~(pad_left | pad_right)) | turn);
  patch::call_original<void>(camera.address());
  turned_last_frame = (camera.get(FieldCamera::buttons_used) & turn) != 0U;
  if (!turned_last_frame) {
    // The camera didn't take the turn (it isn't the player's to turn this frame); if the game
    // left the yaw alone, take back what was set.
    if (camera.get(FieldCamera::yaw) == static_cast<std::uint16_t>(set_yaw)) {
      camera.set(FieldCamera::yaw, yaw_before);
      camera.set(FieldCamera::shown_yaw, shown_before);
    }
    return;
  }

  // The game turned by its d-pad step (unless it set the yaw some other way). Learn the step;
  // if it wasn't the one expected, turn by the stick's share of it from the yaw shown now.
  const auto game_turn = static_cast<std::int16_t>(camera.get(FieldCamera::yaw) - set_yaw);
  if (std::abs(game_turn) < 0x40 || std::abs(game_turn) > 0x2000) return;  // not the d-pad's
  left_step = turn == pad_left ? game_turn : -game_turn;
  if (game_turn == expected_turn) return;
  const auto turned = static_cast<std::uint16_t>(shown_before + std::lround(game_turn * share));
  camera.set(FieldCamera::yaw, turned);
  camera.set(FieldCamera::shown_yaw, turned);
}

// Puts the eye where this camera has it, faded with the game's.
void place_eye(psprecomp::State& state, Ref<CameraManager> manager, Ref<FieldCamera> camera) {
  const auto hunter = manager.get(CameraManager::hunter);
  if (!hunter.reaches(Character::position)) {
    weight = 0.0F;
    following = false;
    return;
  }
  const auto feet = vector(hunter.get(Character::position));
  const auto height = camera.get(FieldCamera::height);
  bool own = height.valid() && camera.get(FieldCamera::blending) == 0U;
  float eye_height = 0.0F, eye_distance = 0.0F, look_height = 0.0F;
  if (own) {
    eye_height = height.get(CameraHeight::eye_height);
    eye_distance = height.get(CameraHeight::distance);
    look_height = height.get(CameraHeight::look_height);
    // Only while the game's camera follows the height (not while the hunter is carted, carves...).
    const auto wanted = vector(camera.get(FieldCamera::wanted_eye));
    own = eye_distance > 1.0F && std::isfinite(eye_height) && std::isfinite(look_height) &&
          std::fabs(camera.get(FieldCamera::wanted_target_height) - (feet[1] + look_height)) < 1.0F &&
          std::fabs(std::hypot(wanted[0] - feet[0], wanted[2] - feet[2]) - eye_distance) < 1.0F;
  }
  const auto fade = frame_seconds.load() / fade_seconds;
  weight = own ? std::min(weight + fade, 1.0F) : std::max(weight - fade, 0.0F);
  if (weight == 0.0F) {
    following = false;
    return;
  }

  if (own) {
    const auto above = eye_height - look_height;
    const auto height_radius = std::hypot(above, eye_distance);
    const auto height_angle = std::atan2(above, eye_distance);
    if (!following) {
      radius = height_radius;
      angle = height_angle;
      target_height = camera.get(FieldCamera::follow).y;
      distance = height_radius;
      following = true;
    } else {
      ease(radius, height_radius, 0.25F);
      ease(angle, height_angle, 0.25F);
    }
    ease(target_height, feet[1] + look_height, 0.5F);
  } else {
    // Fading out: the game's point looked at, as the game eases it.
    target_height = camera.get(FieldCamera::follow).y;
  }
  tilt = std::clamp(tilt, lowest_view - angle, highest_view - angle);

  const auto view = angle + tilt;
  const auto heading = static_cast<float>(static_cast<std::int16_t>(camera.get(FieldCamera::shown_yaw))) *
                       (2.0F * pi / 65536.0F);
  const Vector way{std::cos(view) * std::sin(heading), std::sin(view), std::cos(view) * std::cos(heading)};
  const Vector look{feet[0], target_height, feet[2]};
  auto clearance = height.valid() ? height.get(CameraHeight::clearance) : floor_clearance;
  if (!(clearance > 0.0F && clearance < floor_clearance)) clearance = floor_clearance;
  const auto nearest = std::min(nearest_eye, radius);
  const auto wall_clear = std::max(clear_distance(state, look, way, radius), nearest);
  const auto clear = std::max(floor_distance(state, look, way, wall_clear, feet[1], clearance), nearest);
  if (wall_clear < distance) distance = wall_clear;  // in front of a wall at once
  // Over the floor quickly (the eye keeps well off it), back out gently.
  ease(distance, clear, clear < distance ? 0.5F : 0.2F);
  const Vector eye{look[0] + way[0] * distance, look[1] + way[1] * distance, look[2] + way[2] * distance};

  auto shown = vector(camera.get(Camera::eye));
  auto looked_at = vector(camera.get(Camera::target));
  for (std::size_t i = 0; i < 3U; ++i) {
    shown[i] += (eye[i] - shown[i]) * weight;
    looked_at[i] += (look[i] - looked_at[i]) * weight;
  }
  camera.set(Camera::eye, vec3(shown));
  camera.set(Camera::target, vec3(looked_at));
  camera.set(Camera::direction, Vec3{looked_at[0] - shown[0], looked_at[1] - shown[1], looked_at[2] - shown[2]});
}

void update_field_camera(psprecomp::State& state, Ref<FieldCamera> camera) {
  using namespace psprecomp;
  const auto manager = deref(state, global::camera_manager);
  if (manager.address() == 0U || manager.get(CameraManager::shown_camera) != 0U) {
    weight = 0.0F;
    following = false;
    patch::call_original<void>(camera.address());
    return;
  }
  const auto held = manager.get(CameraManager::held_buttons);
  const auto pressed = manager.get(CameraManager::pressed_buttons);

  // The d-pad counts as the stick pushed all the way; neither moves the camera while the game's
  // doesn't take the pad.
  float across = 0.0F, upright = 0.0F;
  if (takes_pad(manager, camera)) {
    const auto input = refract::host::controller_state();
    const auto pad_x = ((held & pad_right) != 0U ? 1 : 0) - ((held & pad_left) != 0U ? 1 : 0);
    const auto pad_y = ((held & pad_down) != 0U ? 1 : 0) - ((held & pad_up) != 0U ? 1 : 0);
    across = pad_x != 0 ? static_cast<float>(pad_x) : std::clamp(input.right_x, -1.0F, 1.0F);
    upright = pad_y != 0 ? static_cast<float>(pad_y) : std::clamp(input.right_y, -1.0F, 1.0F);
  }
  if (state.stop_reason != StopReason::running) return;

  // Tilt: up to look up, as the d-pad's heights go (the game's reverse option turns it round); a
  // gentle curve, for small moves near the middle.
  const auto game_data = deref(state, global::game_data);
  if (game_data.reaches(GameData::camera_controls) && game_data.get(GameData::camera_controls) != 0U) upright = -upright;
  tilt += upright * std::sqrt(std::fabs(upright)) * tilt_speed * turn_speed.load() * frame_seconds.load();

  // The game's camera doesn't see up and down (its heights), and turns as the stick says.
  const auto camera_held = static_cast<std::uint16_t>(held & ~(pad_up | pad_down));
  manager.set(CameraManager::pressed_buttons, pressed & ~(pad_up | pad_down));
  turn_and_update(manager, camera, across, camera_held);
  manager.set(CameraManager::held_buttons, held);
  manager.set(CameraManager::pressed_buttons, pressed);
  if (state.stop_reason != StopReason::running) return;

  // L tapped turns the game's camera back behind the hunter, and this one's tilt goes back with it.
  if (upright != 0.0F) untilting = false;
  else if (tapped_l(state, manager, camera)) untilting = true;
  if (untilting) ease(tilt, 0.0F, 0.25F);
  if (state.stop_reason == StopReason::running) place_eye(state, manager, camera);
}

bool field_camera_hook(psprecomp::State& state) {
  update_field_camera(state, {state, state.gpr[4]});
  if (state.stop_reason == psprecomp::StopReason::running) {
    state.pc = state.gpr[31];
    state.branch_pending = false;
  }
  return true;
}

}  // namespace

void set_right_stick_camera(float speed, int game_fps) {
  turn_speed.store(speed);
  frame_seconds.store(1.0F / static_cast<float>(game_fps > 0 ? game_fps : 30));
  static bool registered = false;
  if (speed > 0.0F && !registered) {
    psprecomp::patch::register_raw_patch(hook_node, eboot::field_camera_update, "right_stick_camera",
                                         &field_camera_hook);
    registered = true;
  }
}

}  // namespace mhfu
