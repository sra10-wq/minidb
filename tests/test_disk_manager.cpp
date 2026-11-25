#include <cstdio>
#include <cstring>
#include <iostream>
#include "disk_manager.h"

// assert() is disabled in Release builds, so use our own check
#define CHECK(cond) do { if (!(cond)) { \
    std::cerr << "FAILED: " #cond " (line " << __LINE__ << ")\n"; return 1; } } while (0)

int main() {
    const char* path = "test_disk.db";
    std::remove(path);

    {
        DiskManager dm(path);
        CHECK(dm.num_pages() == 0);
        for (int i = 0; i < 3; i++) {
            page_id_t id = dm.allocate_page();
            CHECK(id == i);
            char buf[PAGE_SIZE];
            std::memset(buf, 'A' + i, PAGE_SIZE);  // page 0 = all 'A', page 1 = all 'B', ...
            dm.write_page(id, buf);
        }
    }  // destructor flushes and closes the file

    {
        DiskManager dm(path);  // reopen: data must persist
        CHECK(dm.num_pages() == 3);
        for (int i = 0; i < 3; i++) {
            char buf[PAGE_SIZE];
            dm.read_page(i, buf);
            CHECK(buf[0] == 'A' + i && buf[PAGE_SIZE - 1] == 'A' + i);
        }
        bool threw = false;
        try { char buf[PAGE_SIZE]; dm.read_page(5, buf); } catch (...) { threw = true; }
        CHECK(threw);  // reading past the end must fail loudly
    }

    std::remove(path);
    std::cout << "disk manager tests passed\n";
    return 0;
}
