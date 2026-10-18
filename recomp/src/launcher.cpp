// MHFU recomp launcher and entry point (Linux).
//
// Finds the player's own disc image (disc.hpp), checks it is the EU release this build was
// made from, lays its files out for refract, builds the game's memory image from the disc's
// unencrypted BOOT.BIN, and starts the recompiled game. Nothing from the game ships with the
// build itself.
//
//   mhfu                  use the remembered ISO, or find or ask for one
//   mhfu PATH             use (and remember) this ISO, or the one in this folder
//   mhfu --choose-iso     ask for the ISO even if one is remembered
//   mhfu --play           start straight away, without the settings screen
//
// Data (the remembered ISO, extracted disc files, saves) lives in $XDG_DATA_HOME/mhfu-recomp
// (~/.local/share/mhfu-recomp by default). REFRACT_VERBOSE=1 logs what the game does.
#include "generated.hpp"
#include "launcher_config.hpp"  // written by build_linux.sh: the build's revision
#include "camera.hpp"
#include "disc.hpp"
#include "fps60.hpp"
#include "features.hpp"
#include "cheats.hpp"

#include <platform/platform.h>
#include <refract/adhoc.hpp>
#include <refract/refract.hpp>

#if defined(MHFU_HAS_SDL)
#include <SDL.h>
#include <refract/frontend.hpp>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;

namespace {

using mhfu::Feature;
using mhfu::features;

constexpr std::uint32_t guest_base = 0x08000000U;
constexpr std::uint32_t guest_ram_end = 0x0a000000U;
constexpr std::uint32_t return_address = 0xfffffff0U;
using mhfu::disc::sector_size;
constexpr const char* window_title = "Monster Hunter Freedom Unite (recomp)";

// Messages -------------------------------------------------------------------

void show_error(const std::string& message) {
  std::fprintf(stderr, "mhfu: %s\n", message.c_str());
#if defined(__ANDROID__)
  // The app shows it again when the game has closed, and offers to choose another ISO.
  std::ofstream(mhfu::data_directory() / "launch-problem.txt") << message << '\n';
#endif
#if defined(MHFU_HAS_SDL)
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, window_title, message.c_str(), nullptr);
#endif
}

// Paths and settings ---------------------------------------------------------

fs::path executable_directory(const char* argv0) {
  std::error_code error;
  auto path = fs::read_symlink("/proc/self/exe", error);
  if (error) path = fs::weakly_canonical(argv0);
  return path.parent_path();
}

// Kept in config.txt in the data directory, one key=value per line.
struct Settings {
  std::optional<fs::path> iso;
  int fps = 30;           // 30 (the game's own) or 60 (the community 60 fps code)
  int resolution = 720;   // 720 or 1080: the window size and how sharply the game is drawn
  bool fullscreen = false;
#if defined(__ANDROID__)
  bool widescreen = true;  // fill a screen wider than the PSP's with more of the game's world
#else
  bool widescreen = false;
#endif
  bool fps_counter = false;  // show the frame rate in a corner
  int frame_interpolation = 0;  // frames shown a second, with ones in between the game's; 0 off
  int right_stick_camera = 100;  // the right stick turns the camera: % of the d-pad's speed, 0 off
  std::map<std::string, bool> features;  // the gameplay features (features.hpp) chosen, by key
  std::string multiplayer_server;  // the ad hoc server to play with others through; empty: off
  std::string player_address;      // this player's address on the ad hoc network, made once
  std::string nickname;            // this player's name on the server
};

bool feature_on(const Settings& settings, const Feature& feature) {
  const auto found = settings.features.find(feature.key);
  return found != settings.features.end() ? found->second : feature.on_by_default;
}

// The features in the order of their names, as the settings screen lists them.
std::vector<const Feature*> features_by_name() {
  auto list = features();
  std::sort(list.begin(), list.end(),
            [](const Feature* a, const Feature* b) { return std::string(a->name) < std::string(b->name); });
  return list;
}

// Frame interpolation's settings: off, then the frames shown a second.
constexpr std::array<int, 4> interpolation_rates = {0, 60, 120, 144};

// The right-stick camera's settings: off, then slow, normal and fast (% of the d-pad's speed).
constexpr std::array<int, 4> right_stick_speeds = {0, 60, 100, 160};

// Multiplayer's settings: off, then public ad hoc servers with a relay (PPSSPP's list), where
// players on PPSSPP and PSPs play too. Everyone playing together picks the same one; another can
// be set in config.txt (multiplayer_server=host).
struct Server {
  const char* name;
  const char* host;
};
constexpr std::array<Server, 5> multiplayer_servers = {{
    {"Off", ""},
    {"MGN (USA)", "psp.mgn.pub"},
    {"Socom (France)", "socom.cc"},
    {"ArenaAnywhere (EU)", "relay.arenaanywhere.site"},
    {"Psi-Hate (USA)", "psi-hate.com"},
}};

// A choice of the Multiplayer row.
struct ServerChoice {
  std::string name;
  std::string host;  // empty: off
};

// The Multiplayer row's choices: off, the public servers, and one set in config.txt if it isn't
// one of them.
std::vector<ServerChoice> server_choices(const std::string& chosen) {
  std::vector<ServerChoice> choices;
  for (const auto& server : multiplayer_servers) choices.push_back({server.name, server.host});
  if (std::none_of(choices.begin(), choices.end(), [&](const auto& choice) { return choice.host == chosen; }))
    choices.push_back({chosen, chosen});
  return choices;
}

// Whether each server answers, found on threads of their own from the launcher's start (looking a
// name up can take a while), for the settings screen to show.
class ServerCheck {
 public:
  enum class Result { checking, online, offline };

  explicit ServerCheck(const std::vector<ServerChoice>& servers) {
    for (const auto& server : servers) {
      auto result = std::make_shared<std::atomic<Result>>(Result::checking);
      if (!server.host.empty())
        std::thread([host = server.host, result] {
          const bool answers = refract::adhoc::server_answers(host, std::chrono::seconds(2));
          result->store(answers ? Result::online : Result::offline);
        }).detach();
      results_.push_back(std::move(result));
    }
  }

  // Waits until the servers have all answered or not, but not past `deadline`.
  void wait(std::chrono::steady_clock::time_point deadline) const {
    for (const auto& result : results_)
      while (result->load() == Result::checking && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  Result result(std::size_t server) const { return results_.at(server)->load(); }

 private:
  std::vector<std::shared_ptr<std::atomic<Result>>> results_;
};

// A player's address as config.txt keeps it (02:1f:...), or nothing if it isn't one.
std::optional<std::array<std::uint8_t, 6>> parse_address(const std::string& text) {
  std::array<unsigned, 6> parts{};
  if (std::sscanf(text.c_str(), "%x:%x:%x:%x:%x:%x", &parts[0], &parts[1], &parts[2], &parts[3], &parts[4],
                  &parts[5]) != 6)
    return std::nullopt;
  std::array<std::uint8_t, 6> address{};
  for (std::size_t i = 0; i < address.size(); ++i) address[i] = static_cast<std::uint8_t>(parts[i]);
  return address;
}

// A new address for this player: random, marked as locally made (not a network card's) and not a
// group address, as PPSSPP makes them.
std::string new_address() {
  std::random_device random;
  std::array<std::uint8_t, 6> address{};
  for (auto& byte : address) byte = static_cast<std::uint8_t>(random());
  address[0] = static_cast<std::uint8_t>((address[0] & 0xfcU) | 0x02U);
  char text[18];
  std::snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", address[0], address[1], address[2],
                address[3], address[4], address[5]);
  return text;
}

Settings load_settings(const fs::path& data) {
  Settings settings;
  std::ifstream config(data / "config.txt");
  std::string line;
  while (std::getline(config, line)) {
    const auto separator = line.find('=');
    if (separator == std::string::npos) continue;
    const auto key = line.substr(0, separator);
    const auto value = line.substr(separator + 1U);
    if (key == "iso" && !value.empty()) settings.iso = fs::path(value);
    else if (key == "fps") settings.fps = value == "60" ? 60 : 30;
    else if (key == "resolution") settings.resolution = value == "1080" ? 1080 : 720;
    else if (key == "fullscreen") settings.fullscreen = value == "1";
    else if (key == "fps_counter") settings.fps_counter = value == "1";
    else if (key == "widescreen") settings.widescreen = value == "1";
    else if (key == "frame_interpolation") {
      const auto rate = std::atoi(value.c_str());
      if (std::find(interpolation_rates.begin(), interpolation_rates.end(), rate) != interpolation_rates.end())
        settings.frame_interpolation = rate;
    } else if (key == "smooth_motion" && value == "1") {
      settings.frame_interpolation = 60;  // the setting's earlier name, on or off
    }
    else if (key == "right_stick_camera") {
      const auto speed = std::atoi(value.c_str());
      if (std::find(right_stick_speeds.begin(), right_stick_speeds.end(), speed) != right_stick_speeds.end())
        settings.right_stick_camera = speed;
    } else if (key.rfind("feature.", 0) == 0) {
      settings.features[key.substr(8)] = value == "1";  // feature.<key>
    } else if (key == "multiplayer_server") {
      settings.multiplayer_server = value;
    } else if (key == "player_address" && parse_address(value)) {
      settings.player_address = value;
    } else if (key == "nickname") {
      settings.nickname = value;
    }
  }
  return settings;
}

void save_settings(const fs::path& data, const Settings& settings) {
  fs::create_directories(data);
  std::ofstream config(data / "config.txt");
  if (settings.iso) config << "iso=" << settings.iso->string() << '\n';
  config << "fps=" << settings.fps << "\nresolution=" << settings.resolution
         << "\nfullscreen=" << (settings.fullscreen ? 1 : 0)
         << "\nwidescreen=" << (settings.widescreen ? 1 : 0)
         << "\nfps_counter=" << (settings.fps_counter ? 1 : 0)
         << "\nframe_interpolation=" << settings.frame_interpolation
         << "\nright_stick_camera=" << settings.right_stick_camera << '\n';
  for (const auto* feature : features_by_name())
    config << "feature." << feature->key << '=' << (feature_on(settings, *feature) ? 1 : 0) << '\n';
  config << "multiplayer_server=" << settings.multiplayer_server << "\nplayer_address=" << settings.player_address
         << "\nnickname=" << settings.nickname << '\n';
}

// The disc image ---------------------------------------------------------------

struct Disc {
  fs::path iso;
  std::vector<mhfu::disc::File> files;
  std::vector<std::uint8_t> memory;  // the guest image built from BOOT.BIN
  std::size_t image_size{};
};

// Opens and checks a disc image; on failure returns nullopt with the reason in `problem`.
std::optional<Disc> open_disc(const fs::path& path, std::string& problem) {
  const auto checked = mhfu::disc::check(path);
  if (checked.result != mhfu::disc::Check::Result::game) {
    problem = checked.problem(path);
    return std::nullopt;
  }
  Disc disc;
  disc.iso = path;
  const auto iso = mhfu::disc::open_iso(path);
  if (!mhfu::disc::list_files(*iso, disc.files)) {
    problem = "Can't read the files on " + path.filename().string() + ".";
    return std::nullopt;
  }
  const auto& elf = checked.program;
  const auto segment = mhfu::disc::loaded_segment(elf);
  if (!segment || segment->address < guest_base || segment->file_size > segment->memory_size ||
      segment->address - guest_base + segment->memory_size > guest_ram_end - guest_base) {
    problem = "The disc's game program (BOOT.BIN) is damaged.";
    return std::nullopt;
  }
  disc.image_size = segment->address - guest_base + segment->memory_size;
  disc.memory.assign(guest_ram_end - guest_base, 0U);
  std::memcpy(&disc.memory[segment->address - guest_base], &elf[segment->offset], segment->file_size);
  return disc;
}

// Copies the disc's files out for refract (once) and writes the sector index it uses for raw
// sector reads.
bool prepare_disc(const Disc& disc, const fs::path& data) {
  const auto disc_root = data / "disc";
  const auto original = data / "original";
  fs::create_directories(original);
  const auto stream = mhfu::disc::open_iso(disc.iso);
  auto& iso = *stream;
  std::ofstream sectors(original / "disc-sectors.tsv", std::ios::trunc);
  std::vector<char> buffer(1U << 20U);
  for (const auto& file : disc.files) {
    sectors << file.sector << '\t' << file.path << '\n';
    const auto target = disc_root / file.path;
    std::error_code error;
    if (fs::is_regular_file(target, error) && fs::file_size(target, error) == file.size) continue;
    fs::create_directories(target.parent_path());
    if (file.size >= (16U << 20U))
      std::fprintf(stderr, "mhfu: first run, copying %s from the disc (%u MB)...\n", file.path.c_str(),
                   file.size >> 20U);
    const auto partial = fs::path(target.string() + ".partial");
    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    iso.seekg(static_cast<std::streamoff>(file.sector) * sector_size);
    for (std::uint32_t left = file.size; left > 0U;) {
      const auto chunk = std::min<std::uint32_t>(left, static_cast<std::uint32_t>(buffer.size()));
      if (!iso.read(buffer.data(), chunk) || !out.write(buffer.data(), chunk)) return false;
      left -= chunk;
    }
    out.close();
    fs::rename(partial, target, error);
    if (error) return false;
  }
  // refract finds disc-sectors.tsv next to the image it reads raw sectors from.
  const auto link = original / "disc.iso";
  std::error_code error;
  fs::remove(link, error);
  fs::create_symlink(fs::absolute(disc.iso), link, error);
  return !error && static_cast<bool>(sectors);
}

}  // namespace

#if defined(MHFU_HAS_SDL)
// The game's logo, cut out of the picture the PSP's menu shows over the game's background
// (PIC0.PNG): it stands opaque on a see-through panel at the top left. Empty if the picture
// isn't the expected one.
refract::frontend::Image disc_logo(const refract::frontend::Image& overlay) {
  refract::frontend::Image logo;
  constexpr int left = 10, top = 6, right = 146, bottom = 80;
  if (overlay.width != 310 || overlay.height != 180) return logo;
  const auto alpha = [&](int x, int y) {
    return overlay.rgba[(static_cast<std::size_t>(y) * 310U + static_cast<std::size_t>(x)) * 4U + 3U];
  };
  int x0 = right, y0 = bottom, x1 = left - 1, y1 = top - 1;  // the opaque part
  for (int y = top; y < bottom; ++y)
    for (int x = left; x < right; ++x)
      if (alpha(x, y) >= 250U) {
        x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
      }
  if (x1 < x0) return logo;
  // A pixel's margin all round, where the cut is.
  logo.width = x1 - x0 + 3;
  logo.height = y1 - y0 + 3;
  std::vector<std::uint8_t> opaque(static_cast<std::size_t>(logo.width) * static_cast<std::size_t>(logo.height));
  logo.rgba.assign(opaque.size() * 4U, 0U);
  for (int y = 0; y < logo.height; ++y)
    for (int x = 0; x < logo.width; ++x) {
      const int sx = x0 - 1 + x, sy = y0 - 1 + y;
      if (sx < left || sy < top || sx >= right || sy >= bottom || alpha(sx, sy) < 250U) continue;
      const auto index = static_cast<std::size_t>(y) * static_cast<std::size_t>(logo.width) + static_cast<std::size_t>(x);
      opaque[index] = 1U;
      std::memcpy(&logo.rgba[index * 4U], &overlay.rgba[(static_cast<std::size_t>(sy) * 310U + static_cast<std::size_t>(sx)) * 4U], 3U);
    }
  // The lettering: its bright pixels, without the specks of the smoky background round it.
  const auto at = [&](int x, int y) { return static_cast<std::size_t>(y) * static_cast<std::size_t>(logo.width) + static_cast<std::size_t>(x); };
  std::vector<std::uint8_t> core(opaque.size());
  for (std::size_t index = 0; index < opaque.size(); ++index) {
    const auto* pixel = &logo.rgba[index * 4U];
    core[index] = opaque[index] != 0U && 0.3F * pixel[0] + 0.59F * pixel[1] + 0.11F * pixel[2] > 0.3F * 255.0F;
  }
  const auto neighbours = [&](std::size_t index, auto&& visit) {
    const int x = static_cast<int>(index % static_cast<std::size_t>(logo.width));
    const int y = static_cast<int>(index / static_cast<std::size_t>(logo.width));
    for (int dy = -1; dy <= 1; ++dy)
      for (int dx = -1; dx <= 1; ++dx)
        if ((dx != 0 || dy != 0) && x + dx >= 0 && y + dy >= 0 && x + dx < logo.width && y + dy < logo.height)
          visit(at(x + dx, y + dy));
  };
  std::vector<std::uint8_t> seen(core.size());
  for (std::size_t first = 0; first < core.size(); ++first) {
    if (core[first] == 0U || seen[first] != 0U) continue;
    std::vector<std::size_t> island{first};
    seen[first] = 1U;
    for (std::size_t i = 0; i < island.size(); ++i)
      neighbours(island[i], [&](std::size_t next) {
        if (core[next] != 0U && seen[next] == 0U) {
          seen[next] = 1U;
          island.push_back(next);
        }
      });
    if (island.size() < 12U)
      for (const auto index : island) core[index] = 0U;
  }
  // Kept: the lettering and two pixels round it (its dark rim), then fading out over two more.
  std::vector<int> distance(core.size(), 1 << 20);
  std::vector<std::size_t> frontier;
  for (std::size_t index = 0; index < core.size(); ++index)
    if (core[index] != 0U) {
      distance[index] = 0;
      frontier.push_back(index);
    }
  for (std::size_t i = 0; i < frontier.size(); ++i)
    neighbours(frontier[i], [&](std::size_t next) {
      if (distance[next] > distance[frontier[i]] + 1) {
        distance[next] = distance[frontier[i]] + 1;
        frontier.push_back(next);
      }
    });
  for (std::size_t index = 0; index < opaque.size(); ++index) {
    const auto shown = opaque[index] == 0U ? 0.0F : std::clamp(1.0F - static_cast<float>(distance[index] - 2) / 2.0F, 0.0F, 1.0F);
    logo.rgba[index * 4U + 3U] = static_cast<std::uint8_t>(std::lround(255.0F * shown));
  }
  return logo;
}

// What the launcher shows: the project's logo (recomp/art; without it, the game's own from the
// player's disc, captioned), the PSP menu's background from the disc, and the controls.
refract::frontend::Launcher launcher_screen(const fs::path& data, const fs::path& resources, const Disc& disc) {
  refract::frontend::Launcher launcher;
  const auto game = data / "disc" / "PSP_GAME";
  launcher.title = "Monster Hunter Freedom Unite";
  launcher.logo = refract::frontend::load_image((resources / "art" / "logo.png").string());
  if (launcher.logo.width == 0) {
    launcher.logo = disc_logo(refract::frontend::load_image((game / "PIC0.PNG").string()));
    launcher.caption = "RECOMPILED";
  }
  launcher.background = refract::frontend::load_image((game / "PIC1.PNG").string());
  launcher.play = "Start Game";
  launcher.version = std::string("Build ") + mhfu::build_revision;
  launcher.details = "European release (ULES01213, v1.01)  \u00b7  " + disc.iso.filename().string();
  launcher.controls = {
      {"D-pad", "Arrow keys", "D-pad"},
      {"Analog stick", "W A S D", "Left stick"},
      {"Camera (hunts)", "Arrow keys", "Right stick, or d-pad"},
      {"Cross", "K", "A"},
      {"Circle", "L", "B"},
      {"Square", "J", "X"},
      {"Triangle", "I", "Y"},
      {"L / R", "Q / E", "Shoulder buttons"},
      {"Start", "Enter", "Start"},
      {"Select", "Right Shift", "Back"},
  };
  return launcher;
}
#endif

int main(int argc, char** argv) {
  bool choose = false;
  bool play = false;
  std::optional<fs::path> iso_argument;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--choose-iso") == 0) choose = true;
    else if (std::strcmp(argv[i], "--play") == 0) play = true;
    else iso_argument = fs::path(argv[i]);
  }
#if defined(__ANDROID__)
  // What the game and the launcher print goes to log.txt, which the app shows if the game stops
  // unexpectedly.
  if (std::freopen((mhfu::data_directory() / "log.txt").c_str(), "w", stderr) != nullptr) {
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    dup2(fileno(stderr), fileno(stdout));
  }
  // The app hands the ISO over as an open file (GameActivity); without one, the app has said why.
  if (!iso_argument) return 1;
  // Landscape whichever way the phone is turned, even with rotation locked (the window can be
  // resized, so without this SDL would follow the lock).
  SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
  const auto data = mhfu::data_directory();
  auto settings = load_settings(data);
#if defined(__ANDROID__)
  settings.fullscreen = true;  // the whole screen, without Android's bars
#endif
  if (settings.player_address.empty()) {
    settings.player_address = new_address();
    if (const char* user = std::getenv("USER"); settings.nickname.empty() && user != nullptr) settings.nickname = user;
  }
#if defined(MHFU_HAS_SDL)
  // Which multiplayer servers can be reached, for the settings screen: found meanwhile.
  const auto servers = server_choices(settings.multiplayer_server);
  const auto checks_started = std::chrono::steady_clock::now();
  std::optional<ServerCheck> server_check;
  if (!play) server_check.emplace(servers);
#endif
  const auto iso = mhfu::find_game(iso_argument, choose, show_error);
  if (!iso) return 1;
  std::string problem;
  auto disc = open_disc(*iso, problem);
  if (!disc) {
    show_error(problem);
    return 1;
  }
  settings.iso = fs::absolute(disc->iso);
  save_settings(data, settings);
  if (!prepare_disc(*disc, data)) {
    show_error("Can't copy the game's files to " + data.string() + ". Is there enough free space?");
    return 1;
  }

  // Fonts and the logo: in Resources next to the program's folder, or where MHFU_RESOURCES says (the
  // Android app unpacks them there).
  const auto* resources_setting = std::getenv("MHFU_RESOURCES");
  const auto resources = resources_setting != nullptr && *resources_setting != '\0'
                             ? fs::path(resources_setting)
                             : executable_directory(argv[0]).parent_path() / "Resources";
  setenv("REFRACT_DISC_ROOT", (data / "disc").c_str(), 1);
  setenv("REFRACT_DISC_IMAGE", (data / "original" / "disc.iso").c_str(), 1);
  setenv("REFRACT_WRITABLE_ROOT", (data / "ms0").c_str(), 0);
  setenv("REFRACT_FONT_DIR", (resources / "fonts").c_str(), 0);
  fs::create_directories(data / "ms0");

  bool verbose = false;
  if (const auto* setting = std::getenv("REFRACT_VERBOSE")) verbose = std::strcmp(setting, "0") != 0;
  refract::Runtime::instance().set_verbose(verbose);

  auto& memory = disc->memory;
  psprecomp::State state;
  state.memory = memory.data();
  state.memory_size = memory.size();
  state.memory_base = guest_base;
  state.direct_memory_access = false;
  state.pc = guest_base + psprecomp::generated::guest_entry;
  // Like the PSP loader: argv[0] goes just past the image, the stack at the top of RAM.
  const auto argument = std::string("disc0:/PSP_GAME/SYSDIR/EBOOT.BIN");
  const auto argument_offset = (disc->image_size + 15U) & ~std::size_t{15U};
  std::memcpy(memory.data() + argument_offset, argument.c_str(), argument.size() + 1U);
  state.gpr[4] = static_cast<std::uint32_t>(argument.size() + 1U);
  state.gpr[5] = guest_base + static_cast<std::uint32_t>(argument_offset);
  state.gpr[29] = guest_base + static_cast<std::uint32_t>(memory.size()) - 64U;
  state.gpr[31] = return_address;
  psprecomp::platform::configure_runtime(memory.data(), memory.size(), guest_base);
  state.gpr[28] = psprecomp::platform::guest_gp_value();
  if (!psprecomp::platform::patch_imports(state)) return 3;
  if (!psprecomp::patch::run_initializers(state)) return 4;

#if defined(MHFU_HAS_SDL)
  // The launcher, in the window as it was last chosen; then the window and drawing resolution
  // chosen in it. 720p draws at 3x the PSP's 480x272 (1440x816), 1080p at 4x (1920x1088).
  const auto apply_display = [&] {
    const bool full_hd = settings.resolution == 1080;
    refract::frontend::set_display_settings(full_hd ? 4U : 3U, full_hd ? 1920 : 1280, full_hd ? 1080 : 720,
                                            settings.fullscreen);
  };
  if (!play) {
    apply_display();
    using refract::frontend::Choice;
    std::vector<Choice> choices = {
        {"Frame rate", {"30 fps (original)", "60 fps"}, settings.fps == 60 ? 1U : 0U, "Display",
         "30 is how the game was made. At 60 the game itself runs 60 times a second, with fixes for what would "
         "otherwise go at double speed. If something moves too fast, go back to 30."},
        {"Resolution", {"720p", "1080p"}, settings.resolution == 1080 ? 1U : 0U, "Display",
         "The game is drawn at this resolution (3 or 4 times the PSP's), so it's sharper, not just bigger."},
#if !defined(__ANDROID__)
        {"Display", {"Window", "Fullscreen"}, settings.fullscreen ? 1U : 0U, "Display",
         "In a window, or on the whole screen."},
#endif
        {"Widescreen", {"Off", "On"}, settings.widescreen ? 1U : 0U, "Display",
         "On a screen wider than the PSP's (most phones, ultrawide monitors), the game's world is drawn wider to "
         "fill it, at the same size, instead of black bars at the sides. The HUD and menus stay in the middle. "
         "Off shows the original picture."},
        {"Frame interpolation", {"Off", "60 fps", "120 fps", "144 fps"},
         static_cast<std::size_t>(std::find(interpolation_rates.begin(), interpolation_rates.end(),
                                            settings.frame_interpolation) -
                                  interpolation_rates.begin()),
         "Display",
         "Shows frames in between the game's own, so the picture moves at that many frames a second (up to your "
         "screen's refresh rate). The game still runs at its normal speed. Experimental: if something looks "
         "wrong, turn it off."},
        {"FPS counter", {"Off", "On"}, settings.fps_counter ? 1U : 0U, "Display",
         "Shows the frame rate in the top-left corner."},
        {"Right-stick camera", {"Off", "Slow", "Normal", "Fast"},
         static_cast<std::size_t>(std::find(right_stick_speeds.begin(), right_stick_speeds.end(),
                                            settings.right_stick_camera) -
                                  right_stick_speeds.begin()),
         "Gameplay",
         "A free camera in the hunting areas, like Monster Hunter 4 Ultimate's: the right stick turns and tilts "
         "it, faster the further you push it. Off keeps the game's original camera."},
    };
#if defined(__ANDROID__)
    constexpr std::size_t frame_rate = 0, resolution = 1, widescreen = 2, interpolation = 3, fps_counter = 4,
                          camera = 5;
#else
    constexpr std::size_t frame_rate = 0, resolution = 1, display = 2, widescreen = 3, interpolation = 4,
                          fps_counter = 5, camera = 6;
#endif
    const auto choosable = features_by_name();
    const auto first_feature = choices.size();
    for (const auto* feature : choosable)
      choices.push_back({feature->name, {"Off", "On"}, feature_on(settings, *feature) ? 1U : 0U, "Gameplay",
                         feature->description});
    // Multiplayer: the servers, each with whether it answers (so players can see which they can
    // play through), waiting a little for the ones still being checked.
    Choice multiplayer{"Multiplayer", {}, 0U, "Online",
                       "Hunt with others over the internet, players on PPSSPP and PSPs too. Everyone picks the same "
                       "server (online means it answers from here), then at the Gathering Hall's door presses Circle "
                       "and picks the same hall number."};
    server_check->wait(checks_started + std::chrono::milliseconds(2500));
    for (std::size_t index = 0; index < servers.size(); ++index) {
      if (servers[index].host == settings.multiplayer_server) multiplayer.selected = index;
      auto value = servers[index].name;
      if (server_check->result(index) == ServerCheck::Result::online) value += ": online";
      if (server_check->result(index) == ServerCheck::Result::offline) value += ": offline";
      multiplayer.values.push_back(value);
    }
    const auto multiplayer_choice = choices.size();
    choices.push_back(multiplayer);
    if (!refract::frontend::run_launcher(launcher_screen(data, resources, *disc), choices)) return 0;
    settings.fps = choices[frame_rate].selected == 1U ? 60 : 30;
    settings.resolution = choices[resolution].selected == 1U ? 1080 : 720;
#if !defined(__ANDROID__)
    settings.fullscreen = choices[display].selected == 1U;
#endif
    settings.widescreen = choices[widescreen].selected == 1U;
    settings.fps_counter = choices[fps_counter].selected == 1U;
    settings.frame_interpolation = interpolation_rates.at(choices[interpolation].selected);
    settings.right_stick_camera = right_stick_speeds.at(choices[camera].selected);
    settings.multiplayer_server = servers.at(choices[multiplayer_choice].selected).host;
    for (std::size_t index = 0; index < choosable.size(); ++index)
      settings.features[choosable[index]->key] = choices[first_feature + index].selected == 1U;
    save_settings(data, settings);
  }
  apply_display();
  refract::frontend::set_widescreen(settings.widescreen);
  refract::frontend::set_fps_counter(settings.fps_counter);
  refract::frontend::set_frame_interpolation(static_cast<std::uint32_t>(settings.frame_interpolation));
#endif

  mhfu::set_right_stick_camera(static_cast<float>(settings.right_stick_camera) / 100.0F, settings.fps);
  refract::adhoc::configure(
      {settings.multiplayer_server, *parse_address(settings.player_address), settings.nickname});
  for (const auto* feature : features_by_name())
    if (feature_on(settings, *feature)) feature->start(settings.fps);

  // 60 fps: the community code, applied before the game starts and again every 50 ms for the
  // overlays it loads; the translated functions it changes run in the interpreter.
  std::jthread cheat_thread;
  if (settings.fps == 60) {
    mhfu::set_real_60fps(true);
    for (const auto& [start, end] : mhfu::fps60_functions)
      psprecomp::patch::add_interpreted_range(start, end);
    mhfu::CheatEngine cheats(memory.data(), memory.size());
    cheats.apply(mhfu::fps60_code);
    cheat_thread = std::jthread([&memory](std::stop_token stop) {
      mhfu::CheatEngine engine(memory.data(), memory.size());
      while (!stop.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        engine.apply(mhfu::fps60_code);
      }
    });
  }

  refract::Runtime::instance().execute_guest(state);
  refract::Runtime::instance().run_host_loop();
  return static_cast<int>(state.gpr[2]);
}
