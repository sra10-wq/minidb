#pragma once
#include <list>
#include <optional>
#include <unordered_map>
#include <vector>
#include "disk_manager.h"
#include "lru_replacer.h"
#include "page.h"

// Caches disk pages in a fixed number of in-memory frames.
// Every fetch_page/new_page must be matched by an unpin_page.
class BufferPool {
public:
    BufferPool(std::size_t pool_size, DiskManager& disk);
    ~BufferPool();

    Page* fetch_page(page_id_t id);  // pinned page, or nullptr if every frame is pinned
    Page* new_page();                // allocates a new disk page and pins it
    bool unpin_page(page_id_t id, bool is_dirty);
    void flush_page(page_id_t id);
    void flush_all();

    std::size_t hits() const { return hits_; }
    std::size_t misses() const { return misses_; }

private:
    std::optional<int> get_free_frame();  // empty frame, or evict the LRU page

    DiskManager& disk_;
    std::vector<Page> frames_;
    std::list<int> free_list_;
    std::unordered_map<page_id_t, int> page_table_;  // page id -> frame index
    LRUReplacer replacer_;
    std::size_t hits_ = 0, misses_ = 0;
};
