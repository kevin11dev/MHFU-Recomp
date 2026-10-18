// A minimal CWCheat engine, and the community "60 FPS" code for MHFU (ULES01213) from the
// CWCheat Database Plus (github.com/Saramagrean/CWCheat-Database-Plus-), as used in PPSSPP.
//
// The code switches the game's logic and frame timing from 30 to 60 updates per second. It
// rewrites a few instructions of the game program (jumping into new code it writes at
// 0x08802000) and of the gameplay overlays. Overlays run in refract's interpreter, which reads
// code from memory, so writing the code is enough for them. The game program is translated to
// C++ ahead of time, so the functions the code changes are also run by the interpreter while
// it is on (fps60_functions). Overlays are loaded and replaced while the game runs, so, like a
// cheat engine, the code is applied again every so often.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace mhfu {

inline constexpr std::pair<std::uint32_t, std::uint32_t> fps60_code[] = {
    {0xE0182C48, 0x000591A0}, {0x200591A0, 0x0A20080B}, {0x200591A4, 0x3C0808B6}, {0x2000202C, 0x8D099914},
    {0x20002030, 0xAC642C48}, {0x20002034, 0x51200002}, {0x20002038, 0x34030000}, {0x2000203C, 0x28830002},
    {0x20002040, 0x0A21646A}, {0x2006DFCC, 0x0A200800}, {0x2006DFD0, 0x00651823}, {0x20002000, 0x3C170880},
    {0x20002004, 0x8EFB1FFC}, {0x20002008, 0x13600003}, {0x2000200C, 0x277B0001}, {0x20002010, 0x00651821},
    {0x20002014, 0x277BFFFE}, {0x20002018, 0x04610002}, {0x2000201C, 0xAC830010}, {0x20002020, 0xAC800010},
    {0x20002024, 0x0A21B7F7}, {0x20002028, 0xAEFB1FFC}, {0x200D5DF8, 0x3C023F80}, {0x2008749C, 0x2403FDC1},
    {0x200874A0, 0x2403023F}, {0xE0054000, 0x012AEDD0}, {0x212AEDD0, 0x3C023F80}, {0x212AB030, 0x3C033FA0},
    {0x212AB044, 0x3C033FA0}, {0x212AF030, 0x3C033F80}, {0x212BC4C8, 0x3C023F80}, {0xE00E4000, 0x012A31F0},
    {0x212A31F0, 0x3C033F80}, {0x212A31CC, 0x3C033F80}, {0x212A4ACC, 0x3C033F80}, {0x212A31F4, 0x3C033FD9},
    {0x212A4AD0, 0x3C033FD9}, {0x21265FB0, 0x3C023F80}, {0x2126767C, 0x3C033F80}, {0x2129E208, 0x3C023FA0},
    {0x2129E234, 0x3C023FA0}, {0x2129E260, 0x3C023FA0}, {0x2126867C, 0x3C033F80}, {0x212759D4, 0x3C023F80},
    {0x212759B4, 0x3C023F80}, {0x2127599C, 0x3C023F80}, {0xE0054000, 0x012C6698}, {0x212C6698, 0x3C033F80},
    {0x212C6964, 0x3C023F80}, {0x212C6AF4, 0x3C023F80}, {0x212CB7B8, 0x3C023F00}, {0x212CB7E0, 0x3C023F80},
    {0xE0101821, 0x0126A22C}, {0x2126A22C, 0x0A200813}, {0x2126A230, 0x00701821}, {0x2000204C, 0x3C1F0880},
    {0x20002050, 0x8FFB2048}, {0x20002054, 0x2E1E0004}, {0x20002058, 0x17C00005}, {0x2000205C, 0x2E1EFFFC},
    {0x20002060, 0x13C00003}, {0x20002064, 0xA623055A}, {0x20002068, 0x0A69A88D}, {0x2000206C, 0xAFFB2048},
    {0x20002070, 0x1360FFFC}, {0x20002074, 0x277B0001}, {0x20002078, 0x00701823}, {0x2000207C, 0x277BFFFE},
    {0x20002080, 0x1000FFF8}, {0xE00A0362, 0x012A25CC}, {0x212A25CC, 0x0A200823}, {0x2000208C, 0x3C020880},
    {0x20002090, 0x8C5B2088}, {0x20002094, 0x13600003}, {0x20002098, 0x277B0001}, {0x2000209C, 0x2463FFFF},
    {0x200020A0, 0x277BFFFE}, {0x200020A4, 0xAC5B2088}, {0x200020A8, 0x0A6A8975}, {0x200020AC, 0x34020362},
    {0xE005400C, 0x012B235C}, {0x212B235C, 0x3C033F8C}, {0x212B24DC, 0x3C033F8C}, {0x212B25B0, 0x3C023F80},
    {0x212B31E8, 0x3C023F80}, {0x212B34DC, 0x3C043F80}, {0xE0034000, 0x01307C9A}, {0x21307C98, 0x3F800000},
    {0x21307C9C, 0x3FC00000}, {0x21307CA0, 0x40000000}, {0xE0023FCC, 0x0129ECFC}, {0x2129ECFC, 0x3C023F4C},
    {0x2129ED10, 0x3C023FA0}, {0xE00D082C, 0x100D5E1C}, {0x200D5E1C, 0x0A20082C}, {0x200D5E20, 0x3C023F80},
    {0x200020B0, 0x3C1B090B}, {0x200020B4, 0x8F63F21C}, {0x200020B8, 0x3C1B43FA}, {0x200020BC, 0x147B0002},
    {0x200020C4, 0x3C024000}, {0x200020C8, 0x0A235789}, {0x200020CC, 0x44820800}, {0x200D1C3C, 0x2841003C},
    {0x200D1C44, 0x28410078}, {0x200D1C54, 0x284100A0}, {0x2006B3B4, 0x240204B0}, {0xE001005A, 0x013A1FC4},
    {0x213A1FC4, 0x240200B4}, {0xE00C0005, 0x012DD930}, {0x212DD930, 0x0A200835}, {0x212DD934, 0x3C1B0880},
    {0x200020D4, 0x8F6420D0}, {0x200020D8, 0x10800003}, {0x200020DC, 0x24840001}, {0x200020E0, 0x24420001},
    {0x200020E4, 0x2484FFFE}, {0x200020E8, 0xAF6420D0}, {0x200020EC, 0x18400003}, {0x200020F0, 0xAE220414},
    {0x200020F4, 0x0A6B764E}, {0x200020FC, 0x0A6B7652},
};

// The translated functions the code changes: [start, end) of each.
inline constexpr std::pair<std::uint32_t, std::uint32_t> fps60_functions[] = {
    {0x08859174U, 0x08859218U}, {0x0886B0E0U, 0x0886B7C0U}, {0x0886DFACU, 0x0886E000U},
    {0x08886E50U, 0x088883B8U}, {0x088D1BA8U, 0x088D1CA8U}, {0x088D5C34U, 0x088D5F30U},
};

// Applies CWCheat codes to guest memory (base: the host address of guest 0x08000000). Supports
// 8/16/32-bit writes and the E0/E1 tests that skip the next lines unless a value matches. Within
// each run, writes below the game program (code caves) come first, so a jump into a cave never
// lands on code that isn't written yet.
class CheatEngine {
 public:
  CheatEngine(std::uint8_t* memory, std::size_t size) : memory_(memory), size_(size) {}

  template <std::size_t N>
  void apply(const std::pair<std::uint32_t, std::uint32_t> (&code)[N]) {
    run(code, N, true);
    run(code, N, false);
  }

 private:
  static constexpr std::uint32_t user_memory = 0x08800000U;
  static constexpr std::uint32_t memory_base = 0x08000000U;
  static constexpr std::uint32_t program_start = 0x08804000U;

  std::uint8_t* at(std::uint32_t address, std::size_t bytes) const {
    if (address < memory_base || address - memory_base + bytes > size_) return nullptr;
    return memory_ + (address - memory_base);
  }

  void run(const std::pair<std::uint32_t, std::uint32_t>* code, std::size_t count, bool caves) {
    for (std::size_t line = 0; line < count; ++line) {
      const auto [first, second] = code[line];
      const auto type = first >> 28U;
      const auto address = user_memory + (first & 0x0fffffffU);
      if (type == 0xeU) {
        const bool byte_test = ((first >> 24U) & 0xfU) == 1U;
        const auto lines = (first >> 16U) & 0xffU;
        const auto expected = byte_test ? first & 0xffU : first & 0xffffU;
        const auto* value = at(user_memory + (second & 0x0fffffffU), byte_test ? 1U : 2U);
        std::uint32_t actual{};
        if (value != nullptr) std::memcpy(&actual, value, byte_test ? 1U : 2U);
        bool pass{};
        switch (second >> 28U) {
          case 0U: pass = actual == expected; break;
          case 1U: pass = actual != expected; break;
          case 2U: pass = actual < expected; break;
          case 3U: pass = actual > expected; break;
          default: break;
        }
        if (value == nullptr || !pass) line += lines;
        continue;
      }
      if (type > 2U || (address < program_start) != caves) continue;
      const auto bytes = type == 0U ? 1U : type == 1U ? 2U : 4U;
      if (auto* target = at(address, bytes)) std::memcpy(target, &second, bytes);
    }
  }

  std::uint8_t* memory_;
  std::size_t size_;
};

}  // namespace mhfu
