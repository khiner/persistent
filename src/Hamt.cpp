#include "Hamt.h"

namespace hamt {
Map Set(Map m, uint64_t, uint64_t) { return m; }
Map Erase(Map m, uint64_t) { return m; }
std::optional<uint64_t> Get(Map, uint64_t) { return {}; }
} // namespace hamt
