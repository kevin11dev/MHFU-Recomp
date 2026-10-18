// The game's memory by the names in re/: game_layout.hpp (made from re/ by tools/gen_layout.py)
// has the structures' fields, the functions, the globals and the overlays; this reads and
// changes them.
//
//   game::Ref<game::Monster> monster{state, state.gpr[4]};
//   if (monster.get(game::Monster::rage_timer) > 0) ...
//   const auto hunter = game::deref(state, game::global::camera_manager).get(game::CameraManager::hunter);
#pragma once

#include <game_layout.hpp>
#include <psprecomp/patch.hpp>

#include <bit>
#include <cstdint>
#include <type_traits>

namespace mhfu::game {

// Whether `size` bytes at `address` are in the game's memory.
inline bool in_ram(Address address, std::uint32_t size) {
  return address >= 0x08800000U && address <= 0x0a000000U - size;
}

template <typename S>
class Ref;

namespace detail {
template <typename T>
struct IsPointer : std::false_type {};
template <typename T>
struct IsPointer<Pointer<T>> : std::true_type {
  using to = T;
};
}  // namespace detail

// A value of type T at `address`; a Pointer<S> reads as a Ref<S>.
template <typename T>
auto load(psprecomp::State& state, Address address) {
  using psprecomp::load16, psprecomp::load32, psprecomp::load8;
  if constexpr (detail::IsPointer<T>::value) {
    return Ref<typename detail::IsPointer<T>::to>{state, load32(state, address)};
  } else if constexpr (std::is_same_v<T, Vec3>) {
    return Vec3{load<float>(state, address), load<float>(state, address + 4U), load<float>(state, address + 8U)};
  } else if constexpr (std::is_same_v<T, float>) {
    return std::bit_cast<float>(load32(state, address));
  } else if constexpr (sizeof(T) == 1) {
    return static_cast<T>(load8(state, address));
  } else if constexpr (sizeof(T) == 2) {
    return static_cast<T>(load16(state, address));
  } else {
    static_assert(sizeof(T) == 4 && std::is_integral_v<T>, "can't load this type");
    return static_cast<T>(load32(state, address));
  }
}

template <typename T, typename V>
void store(psprecomp::State& state, Address address, V value) {
  using psprecomp::store16, psprecomp::store32, psprecomp::store8;
  if constexpr (detail::IsPointer<T>::value) {
    store32(state, address, static_cast<Address>(value));
  } else if constexpr (std::is_same_v<T, Vec3>) {
    store<float>(state, address, value.x);
    store<float>(state, address + 4U, value.y);
    store<float>(state, address + 8U, value.z);
  } else if constexpr (std::is_same_v<T, float>) {
    store32(state, address, std::bit_cast<std::uint32_t>(static_cast<float>(value)));
  } else if constexpr (sizeof(T) == 1) {
    store8(state, address, static_cast<std::uint8_t>(static_cast<T>(value)));
  } else if constexpr (sizeof(T) == 2) {
    store16(state, address, static_cast<std::uint16_t>(static_cast<T>(value)));
  } else {
    static_assert(sizeof(T) == 4 && std::is_integral_v<T>, "can't store this type");
    store32(state, address, static_cast<std::uint32_t>(static_cast<T>(value)));
  }
}

// A structure of type S in the game's memory (S void: of a type not known).
template <typename S>
class Ref {
 public:
  Ref(psprecomp::State& state, Address address) : state_(&state), address_(address) {}

  Address address() const { return address_; }
  explicit operator Address() const { return address_; }

  // Whether the field is in the game's memory: a structure past its end can't be read.
  template <typename O, typename T>
  bool reaches(Field<O, T> field) const {
    check<O>();
    return in_ram(field.in(address_), size_of<T>);
  }
  // Whether all of the structure is.
  bool valid() const { return in_ram(address_, size_of<S>); }

  template <typename O, typename T>
  auto get(Field<O, T> field) const {
    check<O>();
    return load<T>(*state_, field.in(address_));
  }
  template <typename O, typename T, typename V>
  void set(Field<O, T> field, V value) const {
    check<O>();
    store<T>(*state_, field.in(address_), value);
  }
  // A structure inside this one.
  template <typename O, typename T>
  Ref<T> inner(Field<O, T> field) const {
    check<O>();
    return {*state_, field.in(address_)};
  }

 private:
  template <typename O>
  static constexpr void check() {
    static_assert(std::is_base_of_v<O, S>, "a field of another structure");
  }

  psprecomp::State* state_;
  Address address_;
};

template <typename T>
auto get(psprecomp::State& state, Global<T> global) {
  return load<T>(state, global.address);
}

template <typename T, typename V>
void set(psprecomp::State& state, Global<T> global, V value) {
  store<T>(state, global.address, value);
}

// The structure a global points to.
template <typename S>
Ref<S> deref(psprecomp::State& state, Global<Pointer<S>> global) {
  return get(state, global);
}

// Whether the overlay is loaded: its header, "MWo3" and then its ID, is at its address.
inline bool loaded(psprecomp::State& state, Overlay overlay) {
  constexpr std::uint32_t magic = 0x336f574dU;  // "MWo3"
  return psprecomp::load32(state, overlay.address) == magic && psprecomp::load32(state, overlay.address + 4U) == overlay.id;
}

// Whether the function's overlay is loaded, with `first` and `second` as the instructions at
// `offset` into the function (overlays share addresses: this tells the function from others').
inline bool loaded(psprecomp::State& state, OverlayFunction function, std::uint32_t offset, std::uint32_t first,
                   std::uint32_t second) {
  return loaded(state, function.overlay) && psprecomp::load32(state, function.address + offset) == first &&
         psprecomp::load32(state, function.address + offset + 4U) == second;
}

}  // namespace mhfu::game
