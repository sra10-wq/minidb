#pragma once
#include <fstream>
#include <string>
#include "config.h"

// Reads and writes fixed-size pages in a single database file.
// Page N lives at byte offset N * PAGE_SIZE.
class DiskManager {
public:
    explicit DiskManager(const std::string& path);
    ~DiskManager();

    void read_page(page_id_t id, char* out);         // fills out[0..PAGE_SIZE)
    void write_page(page_id_t id, const char* data); // writes PAGE_SIZE bytes
    page_id_t allocate_page();                       // appends a zeroed page, returns its id
    page_id_t num_pages() const { return num_pages_; }
    void flush();

private:
    std::string path_;
    std::fstream file_;
    page_id_t num_pages_ = 0;
};
