#pragma once

#include <concepts>
#include <cstdint>
#include <type_traits>
#include <utility>

// Persistent box: one value of any type, held on the heap behind a reference count. Copying bumps the
// counter, and two boxes that never diverged compare equal without reading a byte of `T`. Header-only and
// a template, unlike the tries, and a cell is one `new` and not a slab node -- so no audit build.
namespace box {
// A box always holds a value: there is no empty box, so a read is a load and never a test.
template<typename T> struct Box {
    // `Refs` is not atomic, so a cell must not be shared across threads. Built from the constructor
    // arguments, so a `T` that can be neither copied nor moved still boxes.
    struct Cell {
        uint32_t Refs = 1;
        T Value;

        template<typename... Args> explicit Cell(Args &&...args) : Value(std::forward<Args>(args)...) {}
    };

    // Null only in a box moved from. Public like the tries' roots, so a test can ask if two boxes share it.
    Cell *Held;

    Box() : Held(new Cell()) {}
    // Implicit at one argument: `Box<std::string> b = "x"`. `Box` is excluded, or it would beat the copy constructor.
    template<typename Arg, typename... Rest>
        requires(!std::same_as<Box, std::remove_cvref_t<Arg>>) && std::constructible_from<T, Arg, Rest...>
    Box(Arg &&arg, Rest &&...rest) : Held(new Cell(std::forward<Arg>(arg), std::forward<Rest>(rest)...)) {}

    Box(const Box &o) : Held(o.Held) { ++Held->Refs; }
    Box(Box &&o) noexcept : Held(o.Held) { o.Held = nullptr; }
    ~Box() {
        if (Held && !--Held->Refs) delete Held;
    }
    // By value, so that both a copy and a move reach it, as the tries' assignment does.
    Box &operator=(Box o) noexcept {
        std::swap(Held, o.Held);
        return *this;
    }

    // The value, which lasts as long as some box holds the cell -- a reference into a temporary dangles.
    const T &operator*() const { return Held->Value; }
    const T *operator->() const { return &Held->Value; }
    // So a box passes wherever the value is read.
    operator const T &() const { return Held->Value; }
};

// The box holding `fn` of the value this one holds. Both forms, as the tries' writes have: given one to
// give up it writes through the cell it alone holds.
template<typename T, typename F> Box<T> Update(const Box<T> &b, F &&fn) { return Box<T>{fn(*b)}; }
template<typename T, typename F> Box<T> &&Update(Box<T> &&b, F &&fn) {
    if (b.Held->Refs == 1) b.Held->Value = fn(std::move(b.Held->Value));
    else b = Box<T>{fn(*b)};
    return std::move(b);
}

// Whether the two boxes hold equal values, which two holding the same cell settle without reading `T`.
template<typename T>
    requires std::equality_comparable<T>
bool operator==(const Box<T> &a, const Box<T> &b) { return a.Held == b.Held || *a == *b; }
// Against a bare value: `T` converts to `Box<T>` and back, so the overload above is ambiguous here. `U` is
// deduced, so a value that only converts to `T` -- `b == "x"` -- lands here too.
template<typename T, typename U>
    requires(!std::same_as<Box<T>, std::remove_cvref_t<U>>) && requires(const T &t, const U &u) { t == u; }
bool operator==(const Box<T> &a, const U &b) { return *a == b; }
} // namespace box
