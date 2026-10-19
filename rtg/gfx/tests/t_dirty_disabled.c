/* DM_DIRTYOFF leaves CPU pages writable. Consumers must therefore re-upload
 * them even after a cached upload, including texture queries and partial spans. */
#include "../nvn/nvn_dirty.h"
#include "../../host/dm.h"
#include <sys/mman.h>

module_t g_mod[MAX_MOD];
int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }
void crash_install(void);

static int failures;
#define CHECK(label, condition) do { \
    int pass = !!(condition); \
    printf("%s: %s\n", label, pass ? "PASS" : "FAIL"); \
    failures += !pass; \
} while (0)

int main(void) {
    setenv("DM_DIRTYOFF", "1", 1);
    crash_install();
    const size_t bytes = 65536;
    unsigned char *guest = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANON, -1, 0);
    if (guest == MAP_FAILED) return 2;
    nvn_dirty_add_range(guest, bytes);
    nvn_dirty_protect(guest, bytes);
    uint64_t stamp = nvn_dirty_now();
    /* Same color update seen on the Mate 60 Pro: black -> white after upload. */
    ((float *)guest)[0] = 1.0f;
    ((float *)guest)[1] = 1.0f;
    ((float *)guest)[2] = 1.0f;
    CHECK("cached uniform upload cannot treat writable pages as tracked",
          !nvn_dirty_tracked(guest, bytes));
    CHECK("texture query sees CPU changes after upload stamp",
          nvn_dirty_since(guest, bytes, stamp));
    CHECK("nonempty writable range stays dirty at current stamp",
          nvn_dirty_since(guest + 17, 31, nvn_dirty_now()));
    CHECK("empty range is clean", !nvn_dirty_since(guest, 0, stamp));
    size_t cursor = 11, offset = 0, length = 0;
    CHECK("dirty iterator returns the remaining requested span",
          nvn_dirty_next_range(guest + 3, 1024, stamp, &cursor, &offset, &length)
          && offset == 11 && length == 1013 && cursor == 1024);
    CHECK("dirty iterator terminates",
          !nvn_dirty_next_range(guest + 3, 1024, stamp, &cursor, &offset, &length));
    nvn_dirty_remove_range(guest);
    munmap(guest, bytes);
    return failures != 0;
}
