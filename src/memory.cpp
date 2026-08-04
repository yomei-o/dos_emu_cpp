// High-page reporting for Memory (see the comment on Memory::check).
#include "memory.h"
#include <cstdio>
#include <cstdlib>

namespace dosemu {

bool Memory::check = getenv("DOSEMU_MEMCHK") != nullptr;

void Memory::note_page(uint32_t a) const {
    static int n = 0;
    if (++n > 20) return;   // a guest working up there touches many pages; a few tell the story
    std::fprintf(stderr, "[mem] page at %08X (above the %08X RAM budget)\n",
                 a & ~kPageMask, kSize);
}

}  // namespace dosemu
