#include <cstdio>
#include <cstring>
#include <iostream>
#include "buffer_pool.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::cerr << "FAILED: " #cond " (line " << __LINE__ << ")\n"; return 1; } } while (0)

static bool has_text(Page* p, int i) {
    char expected[16];
    std::snprintf(expected, sizeof(expected), "page-%d", i);
    return p && std::strcmp(p->data, expected) == 0;
}

int main() {
    const char* path = "test_bp.db";
    std::remove(path);

    {
        DiskManager dm(path);
        BufferPool bp(3, dm);  // only 3 frames

        // 1. Write 5 pages through 3 frames: forces evictions and dirty write-back.
        for (int i = 0; i < 5; i++) {
            Page* p = bp.new_page();
            CHECK(p && p->id == i);
            std::snprintf(p->data, PAGE_SIZE, "page-%d", i);
            CHECK(bp.unpin_page(p->id, true));
        }
        for (int i = 0; i < 5; i++) {
            CHECK(has_text(bp.fetch_page(i), i));
            bp.unpin_page(i, false);
        }
        // Cache now holds pages 2, 3, 4 (least to most recently used).

        // 2. LRU order: touch 2, so 3 becomes the least recently used.
        std::size_t m = bp.misses();
        CHECK(bp.fetch_page(2)); bp.unpin_page(2, false);
        CHECK(bp.misses() == m);        // hit
        CHECK(bp.fetch_page(0)); bp.unpin_page(0, false);
        CHECK(bp.misses() == m + 1);    // miss: should evict page 3
        CHECK(bp.fetch_page(2)); bp.unpin_page(2, false);
        CHECK(bp.misses() == m + 1);    // 2 survived
        CHECK(bp.fetch_page(3)); bp.unpin_page(3, false);
        CHECK(bp.misses() == m + 2);    // 3 was the one evicted

        // 3. Pinned pages are never evicted.
        CHECK(bp.fetch_page(0) && bp.fetch_page(1) && bp.fetch_page(2));
        CHECK(bp.fetch_page(4) == nullptr);  // all 3 frames pinned
        bp.unpin_page(0, false); bp.unpin_page(1, false); bp.unpin_page(2, false);
        CHECK(bp.fetch_page(4) != nullptr);
        bp.unpin_page(4, false);
    }  // destructor flushes dirty pages

    {
        // 4. Everything persists after restart.
        DiskManager dm(path);
        BufferPool bp(2, dm);
        CHECK(dm.num_pages() == 5);
        for (int i = 0; i < 5; i++) {
            CHECK(has_text(bp.fetch_page(i), i));
            bp.unpin_page(i, false);
        }
    }

    std::remove(path);
    std::cout << "buffer pool tests passed\n";
    return 0;
}
