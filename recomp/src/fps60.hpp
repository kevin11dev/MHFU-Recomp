#pragma once

// Real 60 fps: fixes for the parts of the game the community 60 fps code leaves running at twice
// the speed (fps60.cpp).

namespace mhfu {

// Turns the fixes on, for the game running with the 60 fps code. Call it before the game starts.
void set_real_60fps(bool enabled);

}  // namespace mhfu
