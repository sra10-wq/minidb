#pragma once
#include <cstring>
#include "config.h"

// One in-memory frame holding a copy of a disk page.
struct Page {
    char data[PAGE_SIZE];
    page_id_t id = INVALID_PAGE_ID;
    int pin_count = 0;   // >0 means someone is using it; cannot be evicted
    bool dirty = false;  // modified since it was read; must be written back before eviction

    void reset() {
        std::memset(data, 0, PAGE_SIZE);
        id = INVALID_PAGE_ID;
        pin_count = 0;
        dirty = false;
    }
};
