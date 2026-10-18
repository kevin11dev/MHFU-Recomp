#pragma once

// Gameplay features a player turns on or off on the settings screen. Each one is a file in
// features/ that describes itself with MHFU_FEATURE and registers its hooks when it's turned on.
// The launcher shows a choice for each (in the order of their names), keeps the choices in
// config.txt (feature.<key>=1 or 0) and starts the ones turned on. features/hold_to_gather.cpp
// is an example; docs/features.md says how to write one.

#include <vector>

namespace mhfu {

struct Feature {
  const char* key;          // in config.txt, as feature.<key>
  const char* name;         // on the settings screen
  const char* description;  // what it does, shown there while it's chosen
  bool on_by_default;       // the choice before the player makes one
  void (*start)(int game_fps);  // registers its hooks, once, before the game starts (30 or 60 fps)
};

// Every feature, in no particular order.
inline std::vector<const Feature*>& features() {
  static std::vector<const Feature*> list;
  return list;
}

struct FeatureRegistration {
  explicit FeatureRegistration(const Feature& feature) { features().push_back(&feature); }
};

}  // namespace mhfu

// Adds a feature: MHFU_FEATURE(hold_to_gather, {"hold_to_gather", "Hold to gather", "Hold the
// button at a gathering spot to keep gathering.", false, &start});
#define MHFU_FEATURE(id, ...)                     \
  const ::mhfu::Feature id##_feature __VA_ARGS__; \
  const ::mhfu::FeatureRegistration id##_registration { id##_feature }
