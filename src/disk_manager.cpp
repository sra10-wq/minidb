#include "disk_manager.h"
#include <cstring>
#include <stdexcept>

DiskManager::DiskManager(const std::string& path) : path_(path) {
    file_.open(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_.is_open()) {
        // file doesn't exist yet: create it, then reopen for read/write
        std::ofstream create(path, std::ios::binary);
        create.close();
        file_.open(path, std::ios::in | std::ios::out | std::ios::binary);
        if (!file_.is_open()) throw std::runtime_error("cannot open " + path);
    }
    file_.seekg(0, std::ios::end);
    num_pages_ = static_cast<page_id_t>(static_cast<std::size_t>(file_.tellg()) / PAGE_SIZE);
}

DiskManager::~DiskManager() {
    flush();
    file_.close();
}

void DiskManager::read_page(page_id_t id, char* out) {
    if (id < 0 || id >= num_pages_) throw std::out_of_range("read_page: bad page id");
    file_.seekg(static_cast<std::streamoff>(id) * PAGE_SIZE);
    file_.read(out, PAGE_SIZE);
    auto got = file_.gcount();
    if (got < static_cast<std::streamsize>(PAGE_SIZE)) {
        std::memset(out + got, 0, PAGE_SIZE - got);  // short read: zero the rest
    }
    file_.clear();  // reset EOF flags so later operations work
}

void DiskManager::write_page(page_id_t id, const char* data) {
    if (id < 0 || id >= num_pages_) throw std::out_of_range("write_page: bad page id");
    file_.seekp(static_cast<std::streamoff>(id) * PAGE_SIZE);
    file_.write(data, PAGE_SIZE);
    if (!file_) throw std::runtime_error("write_page failed");
}

page_id_t DiskManager::allocate_page() {
    page_id_t id = num_pages_++;
    char zeros[PAGE_SIZE] = {};
    write_page(id, zeros);
    return id;
}

void DiskManager::flush() {
    file_.flush();
}
