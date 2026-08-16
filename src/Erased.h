#pragma once

#include <type_traits>

// How a callable reaches a compiled walk: a function pointer and a `void *` standing for it, so nothing
// is allocated and the walk stays out of the header that offers it. `const` is lost only for the hop.
namespace erased {
template<typename F> void *Context(F &&fn) { return const_cast<std::remove_const_t<std::remove_reference_t<F>> *>(&fn); }
template<typename R, typename F, typename... Args> R Call(void *context, Args... args) { return (*static_cast<std::remove_reference_t<F> *>(context))(args...); }
} // namespace erased
