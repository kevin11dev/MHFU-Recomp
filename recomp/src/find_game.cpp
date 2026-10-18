// Finds the player's copy of the game before the game is built from it (play.sh runs this):
// the one given (a disc image, or a folder with it in), else the remembered one, else one where
// emulators keep games, else asks for the folder it's in. Prints its path; with --program, also
// writes the game's program (the disc's BOOT.BIN) to FILE, for the build to translate.
//
//   find_game [--choose-iso] [--program FILE] [PATH]
//
// Other options (the game's, like --play) are left alone.
#include "disc.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
  bool choose = false;
  std::optional<fs::path> given, program;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--choose-iso") == 0) choose = true;
    else if (std::strcmp(argv[i], "--program") == 0 && i + 1 < argc) program = argv[++i];
    else if (std::strncmp(argv[i], "--", 2) != 0) given = fs::path(argv[i]);
  }
  const auto game = mhfu::find_game(given, choose, mhfu::show_message);
  if (!game) return 1;
  if (program) {
    const auto checked = mhfu::disc::check(*game);
    std::error_code error;
    fs::create_directories(program->parent_path(), error);
    const auto partial = fs::path(program->string() + ".partial");
    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(checked.program.data()), static_cast<std::streamsize>(checked.program.size()));
    out.close();
    if (checked.program.empty() || !out) {
      mhfu::show_message("Can't write the game's program to " + program->string() + ".");
      return 1;
    }
    fs::rename(partial, *program, error);
    if (error) {
      mhfu::show_message("Can't write the game's program to " + program->string() + ".");
      return 1;
    }
  }
  std::printf("%s\n", game->c_str());
  return 0;
}
