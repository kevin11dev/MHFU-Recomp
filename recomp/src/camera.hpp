#pragma once

// Right-stick camera: a free camera like Monster Hunter 4 Ultimate's in the hunting areas. A
// controller's right stick, or the d-pad, turns it by as much as it is pushed and tilts it up and
// down smoothly.

namespace mhfu {

// Turns the right-stick camera on, turning at `speed` times the d-pad's original speed with the
// stick pushed all the way, or off (0), for the game running at `game_fps` (30 or 60). Call it
// before the game starts.
void set_right_stick_camera(float speed, int game_fps);

}  // namespace mhfu
