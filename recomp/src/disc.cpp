#include "disc.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fcntl.h>
#include <spawn.h>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace fs = std::filesystem;

namespace mhfu {
namespace disc {
namespace {

// A folder's entry on the disc.
struct Entry {
  std::string name;
  std::uint32_t sector{};
  std::uint32_t size{};
  bool folder{};
};

bool read_folder(std::ifstream& iso, std::uint32_t sector, std::uint32_t size, std::vector<Entry>& entries) {
  std::vector<std::uint8_t> records;
  if (!read_sectors(iso, sector, size, records)) return false;
  for (std::size_t offset = 0; offset < records.size();) {
    const auto length = records[offset];
    if (length == 0U) {  // records don't cross sector boundaries
      offset = (offset / sector_size + 1U) * sector_size;
      continue;
    }
    if (offset + length > records.size() || length < 34U) return false;
    const auto* record = &records[offset];
    const auto name_length = record[32];
    if (33U + name_length > length) return false;
    std::string name(reinterpret_cast<const char*>(record + 33), name_length);
    offset += length;
    if (name == std::string(1, '\0') || name == std::string(1, '\1')) continue;  // . and ..
    if (const auto version = name.find(';'); version != std::string::npos) name.resize(version);
    entries.push_back({name, le32(record + 2), le32(record + 10), (record[25] & 2U) != 0U});
  }
  return true;
}

bool list_folder(std::ifstream& iso, const Entry& folder, const std::string& prefix, std::vector<File>& files,
                 int depth) {
  std::vector<Entry> entries;
  if (depth > 8 || !read_folder(iso, folder.sector, folder.size, entries)) return false;
  for (const auto& entry : entries) {
    if (!entry.folder)
      files.push_back({prefix + entry.name, entry.sector, entry.size});
    else if (!list_folder(iso, entry, prefix + entry.name + "/", files, depth + 1))
      return false;
  }
  return true;
}

// The disc's root folder, from its primary volume descriptor.
std::optional<Entry> root_folder(std::ifstream& iso) {
  std::vector<std::uint8_t> descriptor;
  if (!read_sectors(iso, 16U, sector_size, descriptor) || descriptor[0] != 1U ||
      std::memcmp(&descriptor[1], "CD001", 5) != 0)
    return std::nullopt;
  return Entry{"", le32(&descriptor[156 + 2]), le32(&descriptor[156 + 10]), true};
}

// The file at `path` (e.g. PSP_GAME/PARAM.SFO) on the disc.
std::optional<Entry> look_up(std::ifstream& iso, Entry folder, std::string_view path) {
  while (!path.empty()) {
    const auto slash = path.find('/');
    const auto name = path.substr(0, slash);
    path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1U);
    std::vector<Entry> entries;
    if (!folder.folder || !read_folder(iso, folder.sector, folder.size, entries)) return std::nullopt;
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) { return entry.name == name; });
    if (found == entries.end()) return std::nullopt;
    folder = *found;
  }
  return folder;
}

std::string lower(std::string text) {
  for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

bool is_compressed_image(const fs::path& path) {
  const auto extension = lower(path.extension().string());
  return extension == ".cso" || extension == ".zso" || extension == ".chd";
}

// What else a folder holds, for the message when the game isn't in it.
std::string described(const fs::path& path, const Check& result) {
  if (result.result == Check::Result::other_game)
    return path.filename().string() + ": " + (result.title.empty() ? "a PSP game" : result.title) + " (" +
           result.disc_id + ")";
  if (result.result == Check::Result::other_version)
    return path.filename().string() + ": " + result.disc_id + ", but another version or a changed copy";
  return {};
}

}  // namespace

std::uint32_t le32(const std::uint8_t* bytes) {
  return bytes[0] | (bytes[1] << 8U) | (bytes[2] << 16U) | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

bool read_sectors(std::ifstream& iso, std::uint32_t sector, std::size_t size, std::vector<std::uint8_t>& out) {
  out.resize(size);
  iso.clear();
  iso.seekg(static_cast<std::streamoff>(sector) * sector_size);
  return static_cast<bool>(iso.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size)));
}

bool list_files(std::ifstream& iso, std::vector<File>& files) {
  const auto root = root_folder(iso);
  return root && list_folder(iso, *root, "", files, 0);
}

std::optional<std::string> sfo_string(const std::vector<std::uint8_t>& sfo, const std::string& wanted) {
  if (sfo.size() < 20U || std::memcmp(sfo.data(), "\0PSF", 4) != 0) return std::nullopt;
  const auto keys = le32(&sfo[8]), values = le32(&sfo[12]), count = le32(&sfo[16]);
  for (std::uint32_t i = 0; i < count && 20U + (i + 1U) * 16U <= sfo.size(); ++i) {
    const auto* entry = &sfo[20U + i * 16U];
    const auto key_offset = keys + (entry[0] | (entry[1] << 8U));
    const auto length = le32(entry + 4), value_offset = values + le32(entry + 12);
    if (key_offset >= sfo.size() || value_offset + length > sfo.size()) continue;
    if (std::strncmp(reinterpret_cast<const char*>(&sfo[key_offset]), wanted.c_str(), sfo.size() - key_offset) != 0)
      continue;
    std::string value(reinterpret_cast<const char*>(&sfo[value_offset]), length);
    value.resize(std::strlen(value.c_str()));
    return value;
  }
  return std::nullopt;
}

std::uint64_t fnv1a64(const std::uint8_t* bytes, std::size_t size) {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (std::size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 0x100000001b3ULL;
  return hash;
}

std::optional<Segment> loaded_segment(const std::vector<std::uint8_t>& elf) {
  if (elf.size() < 0x54U || std::memcmp(elf.data(), "\x7f" "ELF", 4) != 0) return std::nullopt;
  const auto header = le32(&elf[0x1c]);
  if (header > elf.size() || elf.size() - header < 32U) return std::nullopt;
  const Segment segment{le32(&elf[header + 4]), le32(&elf[header + 8]), le32(&elf[header + 16]),
                        le32(&elf[header + 20])};
  if (segment.offset > elf.size() || elf.size() - segment.offset < segment.file_size) return std::nullopt;
  return segment;
}

std::string Check::problem(const fs::path& path) const {
  const auto name = path.filename().string();
  const std::string wanted = std::string("Monster Hunter Freedom Unite's European release (") + expected_disc_id +
                             " v" + expected_version + ")";
  switch (result) {
    case Result::game: return {};
    case Result::other_version:
      if (version == expected_version)
        return name + " is " + expected_disc_id + " v" + version +
               ", but its game program has been changed (a patched or modded copy?). This build needs the "
               "original disc's.";
      return name + " is " + expected_disc_id + (version.empty() ? "" : " v" + version) + ". This build needs v" +
             expected_version + ".";
    case Result::other_game:
      return name + " is " + (title.empty() ? "a PSP game" : title) + " (" + disc_id + "), not " + wanted + ".";
    case Result::compressed:
      return name + " is compressed (.cso, .zso or .chd), which can't be read yet: the game needs to be an .iso.";
    case Result::not_a_disc: return name + " isn't a PSP disc image (ISO).";
    case Result::unreadable: return "Can't read " + path.string() + ".";
  }
  return {};
}

Check check(const fs::path& path) {
  Check result;
  std::ifstream iso(path, std::ios::binary);
  if (!iso) return result;
  std::array<char, 8> magic{};
  if (!iso.read(magic.data(), magic.size())) {
    result.result = Check::Result::not_a_disc;
    return result;
  }
  const std::string_view head(magic.data(), magic.size());
  if (head.starts_with("CISO") || head.starts_with("ZISO") || head.starts_with(std::string_view("DAX\0", 4)) ||
      head == "MComprHD") {
    result.result = Check::Result::compressed;
    return result;
  }
  const auto root = root_folder(iso);
  const auto param = root ? look_up(iso, *root, "PSP_GAME/PARAM.SFO") : std::nullopt;
  std::vector<std::uint8_t> sfo;
  if (!param || param->folder || !read_sectors(iso, param->sector, param->size, sfo)) {
    result.result = Check::Result::not_a_disc;
    return result;
  }
  result.disc_id = sfo_string(sfo, "DISC_ID").value_or("");
  result.title = sfo_string(sfo, "TITLE").value_or("");
  result.version = sfo_string(sfo, "DISC_VERSION").value_or("");
  if (result.disc_id != expected_disc_id) {
    result.result = Check::Result::other_game;
    return result;
  }
  result.result = Check::Result::other_version;
  const auto boot = look_up(iso, *root, "PSP_GAME/SYSDIR/BOOT.BIN");
  std::vector<std::uint8_t> program;
  if (!boot || boot->folder || !read_sectors(iso, boot->sector, boot->size, program)) return result;
  const auto segment = loaded_segment(program);
  if (!segment || fnv1a64(&program[segment->offset], segment->file_size) != expected_image_hash) return result;
  result.result = Check::Result::game;
  result.program = std::move(program);
  return result;
}

std::optional<fs::path> find(const fs::path& path, std::string& problem) {
  std::error_code error;
  if (!fs::is_directory(path, error)) {
    if (!fs::exists(path, error)) {
      problem = "Can't find " + path.string() + ".";
      return std::nullopt;
    }
    const auto result = check(path);
    if (result.result == Check::Result::game) return path;
    problem = result.problem(path);
    return std::nullopt;
  }
  // Disc images in the folder and the folders in it, a few deep (ROMs/PSP/Some folder/game.iso),
  // known by what's in them rather than their names.
  constexpr int deepest = 3;
  std::vector<std::string> others;
  std::size_t more = 0;
  bool compressed = false;
  fs::recursive_directory_iterator entry(
      path, fs::directory_options::skip_permission_denied | fs::directory_options::follow_directory_symlink, error);
  for (const fs::recursive_directory_iterator end; !error && entry != end; entry.increment(error)) {
    const auto& file = entry->path();
    std::error_code ignored;
    if (entry->is_directory(ignored)) {
      if (file.filename().string().starts_with('.') || entry.depth() >= deepest) entry.disable_recursion_pending();
      continue;
    }
    if (is_compressed_image(file)) {
      compressed = true;
      continue;
    }
    if (lower(file.extension().string()) != ".iso") continue;
    const auto result = check(file);
    if (result.result == Check::Result::game) return file;
    const auto description = described(file, result);
    if (description.empty()) continue;
    if (others.size() < 6U) others.push_back("  " + description);
    else ++more;
  }
  problem = "There's no copy of Monster Hunter Freedom Unite's European release (" + std::string(expected_disc_id) +
            " v" + expected_version + ") in " + path.string() + " or the folders in it.";
  if (!others.empty()) {
    problem += "\n\nThere is:";
    for (const auto& other : others) problem += "\n" + other;
    if (more > 0U) problem += "\n  and " + std::to_string(more) + " more";
  }
  if (compressed)
    problem += "\n\nCompressed disc images (.cso, .zso, .chd) can't be read yet: the game needs to be an .iso.";
  return std::nullopt;
}

namespace {

std::optional<fs::path> look_in_usual_places() {
  const auto* home_value = std::getenv("HOME");
  if (home_value == nullptr || *home_value == '\0') return std::nullopt;
  const fs::path home = home_value;
  std::vector<fs::path> games, folders;
  // PPSSPP's settings: its recent games (FileName0 = ...) and the folder it showed last.
  const auto* config_value = std::getenv("XDG_CONFIG_HOME");
  const fs::path config = config_value != nullptr && *config_value != '\0' ? fs::path(config_value) : home / ".config";
  const auto trim = [](const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) return std::string();
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1U);
  };
  for (const auto& settings : {config / "ppsspp/PSP/SYSTEM/ppsspp.ini",
                               home / ".var/app/org.ppsspp.PPSSPP/config/ppsspp/PSP/SYSTEM/ppsspp.ini"}) {
    std::ifstream in(settings);
    std::string line;
    while (std::getline(in, line)) {
      const auto equals = line.find('=');
      if (equals == std::string::npos) continue;
      const auto key = trim(line.substr(0, equals)), value = trim(line.substr(equals + 1U));
      if (value.empty()) continue;
      if (key.starts_with("FileName")) games.emplace_back(value);
      else if (key == "CurrentDirectory") folders.emplace_back(value);
    }
  }
  // ROM folders: in the home folder, and on memory cards and drives (/run/media/<user>/<card> on a
  // Steam Deck, /media/<user>/<drive> on Ubuntu; a PSP's memory stick keeps games in ISO).
  std::vector<fs::path> roots{home};
  std::error_code error;
  for (const auto* media : {"/run/media", "/media"})
    for (const auto& mount : fs::directory_iterator(media, error)) {
      roots.push_back(mount.path());
      std::error_code inner;
      for (const auto& drive : fs::directory_iterator(mount.path(), inner)) roots.push_back(drive.path());
    }
  for (const auto& root : roots)
    for (const auto& folder : fs::directory_iterator(root, error)) {
      const auto name = lower(folder.path().filename().string());
      if ((name == "roms" || name == "games" || name == "emulation" || name == "retrodeck" || name == "downloads" ||
           name == "iso") &&
          folder.is_directory(error))
        folders.push_back(folder.path());
    }
  for (const auto& game : games)
    if (lower(game.extension().string()) == ".iso" && check(game).result == Check::Result::game) return game;
  for (const auto& folder : folders) {
    std::string ignored;
    if (auto found = find(folder, ignored)) return found;
  }
  return std::nullopt;
}

}  // namespace

std::optional<fs::path> find_in_usual_places() {
  try {
    return look_in_usual_places();
  } catch (const std::exception&) {  // a folder that went away or can't be read while looking
    return std::nullopt;
  }
}

}  // namespace disc

fs::path data_directory() {
  if (const auto* value = std::getenv("MHFU_DATA_DIR")) return value;
  if (const auto* value = std::getenv("XDG_DATA_HOME"); value != nullptr && *value != '\0')
    return fs::path(value) / "mhfu-recomp";
  if (const auto* home = std::getenv("HOME")) return fs::path(home) / ".local" / "share" / "mhfu-recomp";
  return fs::current_path() / "mhfu-recomp";
}

namespace {

bool has_desktop() {
  const auto* x11 = std::getenv("DISPLAY");
  const auto* wayland = std::getenv("WAYLAND_DISPLAY");
  return (x11 != nullptr && *x11 != '\0') || (wayland != nullptr && *wayland != '\0');
}

// Runs a program (no shell, so nothing in the arguments is taken as a command) and returns its
// exit status, 127 if it couldn't be run; what it prints goes to `output`.
int run(const std::vector<std::string>& command, std::string& output) {
  std::vector<char*> arguments;
  for (const auto& argument : command) arguments.push_back(const_cast<char*>(argument.c_str()));
  arguments.push_back(nullptr);
  int pipe_ends[2];
  if (pipe(pipe_ends) != 0) return 127;
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, pipe_ends[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipe_ends[0]);
  posix_spawn_file_actions_addclose(&actions, pipe_ends[1]);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  pid_t child{};
  const auto spawned = posix_spawnp(&child, arguments[0], &actions, nullptr, arguments.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(pipe_ends[1]);
  if (spawned != 0) {
    close(pipe_ends[0]);
    return 127;
  }
  std::array<char, 4096> buffer{};
  for (ssize_t got; (got = read(pipe_ends[0], buffer.data(), buffer.size())) > 0;)
    output.append(buffer.data(), static_cast<std::size_t>(got));
  close(pipe_ends[0]);
  int status = 0;
  if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status)) return 127;
  while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
  return WEXITSTATUS(status);
}

// The remembered disc image: iso= in config.txt.
std::optional<fs::path> remembered_game() {
  std::ifstream config(data_directory() / "config.txt");
  std::string line;
  while (std::getline(config, line))
    if (line.starts_with("iso=") && line.size() > 4U) return fs::path(line.substr(4));
  return std::nullopt;
}

// Asks where the game is: a folder, with the desktop's chooser, or a folder or file on the
// terminal. nullopt if the player cancels or there's no way to ask.
std::optional<fs::path> ask_where() {
  const std::string title = "Choose the folder with your Monster Hunter Freedom Unite (EU) disc image";
  const auto* home = std::getenv("HOME");
  const std::string start = home != nullptr ? std::string(home) + "/" : "./";
  if (has_desktop()) {
    const std::vector<std::vector<std::string>> choosers = {
        {"zenity", "--file-selection", "--directory", "--title=" + title, "--filename=" + start},
        {"kdialog", "--title", title, "--getexistingdirectory", start},
    };
    for (const auto& chooser : choosers) {
      std::string output;
      const auto status = run(chooser, output);
      if (status == 0 && !output.empty()) return fs::path(output);
      if (status == 1) return std::nullopt;  // cancelled
    }
  }
  if (isatty(STDIN_FILENO) == 0) return std::nullopt;
  std::fprintf(stderr, "Folder (or file) with your Monster Hunter Freedom Unite (EU) disc image: ");
  std::string line;
  if (!std::getline(std::cin, line)) return std::nullopt;
  // As pasted or dropped in: maybe quoted, maybe starting with ~.
  while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) line.pop_back();
  while (!line.empty() && line.front() == ' ') line.erase(line.begin());
  if (line.size() >= 2U && (line.front() == '\'' || line.front() == '"') && line.back() == line.front())
    line = line.substr(1, line.size() - 2U);
  if (line.empty()) return std::nullopt;
  if (line.starts_with("~/") && home != nullptr) line = std::string(home) + line.substr(1);
  return fs::path(line);
}

}  // namespace

void show_message(const std::string& message) {
  std::fprintf(stderr, "mhfu: %s\n", message.c_str());
  if (!has_desktop()) return;
  const std::vector<std::vector<std::string>> windows = {
      {"zenity", "--error", "--no-markup", "--width=480", "--title=Monster Hunter Freedom Unite", "--text=" + message},
      {"kdialog", "--title", "Monster Hunter Freedom Unite", "--error", message},
  };
  for (const auto& window : windows) {
    std::string output;
    if (run(window, output) != 127) return;
  }
}

std::optional<fs::path> find_game(const std::optional<fs::path>& given, bool choose,
                                  const std::function<void(const std::string&)>& tell) {
  std::string problem;
  if (given) {
    if (auto found = disc::find(*given, problem)) return fs::absolute(*found);
    tell(problem);
    return std::nullopt;  // what's given on the command line isn't asked for again
  }
  if (!choose) {
    if (const auto remembered = remembered_game();
        remembered && disc::check(*remembered).result == disc::Check::Result::game)
      return remembered;
    if (auto found = disc::find_in_usual_places()) {
      std::fprintf(stderr, "mhfu: found the game: %s\n", found->c_str());
      return fs::absolute(*found);
    }
  }
  for (;;) {
    const auto where = ask_where();
    if (!where) {
      std::fprintf(stderr,
                   "mhfu: to play, choose the folder with your Monster Hunter Freedom Unite (EU) disc image, "
                   "or give its path:\n  ./play.sh /path/to/your/roms\n");
      return std::nullopt;
    }
    if (auto found = disc::find(*where, problem)) return fs::absolute(*found);
    tell(problem);
  }
}

}  // namespace mhfu
