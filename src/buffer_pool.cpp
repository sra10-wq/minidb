#include "buffer_pool.h"

BufferPool::BufferPool(std::size_t pool_size, DiskManager& disk)
    : disk_(disk), frames_(pool_size) {
    for (std::size_t i = 0; i < pool_size; i++) free_list_.push_back(static_cast<int>(i));
}

BufferPool::~BufferPool() { flush_all(); }

std::optional<int> BufferPool::get_free_frame() {
    if (!free_list_.empty()) {
        int frame = free_list_.front();
        free_list_.pop_front();
        return frame;
    }
    auto victim = replacer_.victim();
    if (!victim) return std::nullopt;  // every frame is pinned
    Page& page = frames_[*victim];
    if (page.dirty) disk_.write_page(page.id, page.data);  // write back before reuse
    page_table_.erase(page.id);
    page.reset();
    return victim;
}

Page* BufferPool::fetch_page(page_id_t id) {
    auto it = page_table_.find(id);
    if (it != page_table_.end()) {  // hit
        hits_++;
        Page& page = frames_[it->second];
        if (page.pin_count++ == 0) replacer_.pin(it->second);
        return &page;
    }
    misses_++;  // miss: load from disk
    auto frame = get_free_frame();
    if (!frame) return nullptr;
    Page& page = frames_[*frame];
    disk_.read_page(id, page.data);
    page.id = id;
    page.pin_count = 1;
    page.dirty = false;
    page_table_[id] = *frame;
    return &page;
}

Page* BufferPool::new_page() {
    auto frame = get_free_frame();
    if (!frame) return nullptr;
    page_id_t id = disk_.allocate_page();
    Page& page = frames_[*frame];
    page.reset();
    page.id = id;
    page.pin_count = 1;
    page_table_[id] = *frame;
    return &page;
}

bool BufferPool::unpin_page(page_id_t id, bool is_dirty) {
    auto it = page_table_.find(id);
    if (it == page_table_.end()) return false;
    Page& page = frames_[it->second];
    if (page.pin_count <= 0) return false;
    if (is_dirty) page.dirty = true;
    if (--page.pin_count == 0) replacer_.unpin(it->second);
    return true;
}

void BufferPool::flush_page(page_id_t id) {
    auto it = page_table_.find(id);
    if (it == page_table_.end()) return;
    Page& page = frames_[it->second];
    if (page.dirty) {
        disk_.write_page(page.id, page.data);
        page.dirty = false;
    }
}

void BufferPool::flush_all() {
    for (auto& [id, frame] : page_table_) flush_page(id);
    disk_.flush();
}
