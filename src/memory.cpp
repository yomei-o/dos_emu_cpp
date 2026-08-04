// Out-of-range access reporting for Memory (see the comment on Memory::check).
#include "memory.h"
#include <cstdio>
#include <cstdlib>

namespace dosemu {

bool Memory::check = getenv("DOSEMU_MEMCHK") != nullptr;

void Memory::oob(uint32_t a, const char* what) const {
    static int n = 0;
    if (++n > 20) return;   // one wild pointer produces thousands; the first few are enough
    std::fprintf(stderr, "[mem] %s past end of RAM: %08X (RAM is %08X)\n", what, a, kSize);
}

}  // namespace dosemu
