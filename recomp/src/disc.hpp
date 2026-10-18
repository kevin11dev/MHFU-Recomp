// The player's copy of the game: reading a PSP disc image (ISO 9660), telling whether it's the
// game this build is made from, finding it in a folder or where emulators keep games, and asking
// for it. The launcher uses it, and so does find_game, which play.sh runs before the game is
// built, to take the game's program from the player's own disc.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <istream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mhfu {

// The game this build is made from: Monster Hunter Freedom Unite, European release, v1.01. Its
// program (the disc's unencrypted PSP_GAME/SYSDIR/BOOT.BIN) hashes to expected_image_hash
// (FNV-1a, 64-bit, over the part loaded into memory).
inline constexpr const char* expected_disc_id = "ULES01213";
inline constexpr const char* expected_version = "1.01";
inline constexpr std::uint64_t expected_image_hash = 0x6456188275dacf0bULL;

namespace disc {

inline constexpr std::uint32_t sector_size = 2048U;

// A file on the disc.
struct File {
  std::string path;  // relative, e.g. PSP_GAME/USRDIR/DATA.BIN
  std::uint32_t sector{};
  std::uint32_t size{};
};

// A disc image's contents. "/proc/self/fd/N" is read through the open file N: on Android the
// system's file picker gives the app an open file, not a path it may open again.
std::unique_ptr<std::istream> open_iso(const std::filesystem::path& path);

std::uint32_t le32(const std::uint8_t* bytes);
bool read_sectors(std::istream& iso, std::uint32_t sector, std::size_t size, std::vector<std::uint8_t>& out);
// Every file on the disc.
bool list_files(std::istream& iso, std::vector<File>& files);
std::optional<std::string> sfo_string(const std::vector<std::uint8_t>& sfo, const std::string& wanted);
std::uint64_t fnv1a64(const std::uint8_t* bytes, std::size_t size);

// The part of a game program (an ELF) loaded into memory: its first program header.
struct Segment {
  std::uint32_t offset{};
  std::uint32_t address{};
  std::uint32_t file_size{};
  std::uint32_t memory_size{};
};
std::optional<Segment> loaded_segment(const std::vector<std::uint8_t>& elf);

// What a file is, as far as this build goes.
struct Check {
  enum class Result { game, other_version, other_game, compressed, not_a_disc, unreadable };
  Result result{Result::unreadable};
  std::string disc_id, title, version;  // from the disc's PARAM.SFO
  std::vector<std::uint8_t> program;    // the game's program (BOOT.BIN), when it's the game
  // Why `path` won't do, for the player.
  std::string problem(const std::filesystem::path& path) const;
};
Check check(const std::filesystem::path& path);

// The game in `path`: the disc image itself, or one in the folder or the folders in it. Without
// it, nullopt and what's there instead in `problem`.
std::optional<std::filesystem::path> find(const std::filesystem::path& path, std::string& problem);

// The game where emulators keep games: PPSSPP's recent games and the folder it showed last, and
// the ROM folders (roms, games, Emulation, retrodeck, Downloads, and ISO on a PSP's memory stick) in
// the home folder and on memory cards and drives.
std::optional<std::filesystem::path> find_in_usual_places();

}  // namespace disc

// Where the game's files, settings and saves are kept: $MHFU_DATA_DIR, or mhfu-recomp in
// $XDG_DATA_HOME (~/.local/share).
std::filesystem::path data_directory();

// A message, in a window when there's a desktop (zenity or kdialog), and on the terminal.
void show_message(const std::string& message);

// The game to play: `given` (a disc image, or a folder with it in), else the one remembered in
// config.txt, else one found where emulators keep games, else asked for (a folder, with the
// desktop's chooser or on the terminal) until it's found or the player gives up. `choose` skips
// the remembered and usual places. What's wrong with what's given or chosen goes to `tell`.
std::optional<std::filesystem::path> find_game(const std::optional<std::filesystem::path>& given, bool choose,
                                               const std::function<void(const std::string&)>& tell);

}  // namespace mhfu
