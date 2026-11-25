#pragma once
#include <compare>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>
#include "buffer_pool.h"
#include "types.h"

// Index entry: ordered by key, then by RID. Duplicate keys are allowed
// (different rows), but every (key, RID) entry is unique.
struct IndexKey {
    int64_t key;
    int32_t page;
    uint16_t slot;
    auto operator<=>(const IndexKey&) const = default;
};

// Disk-based B+ tree on INT keys. Node layout (one node per page):
// [is_leaf 1B | pad 1B | num_keys 2B | next_leaf 4B | keys (16B each) | children (4B each, internal only)]
class BPlusTree {
public:
    static constexpr int LEAF_CAPACITY = 255;      // 8 + 16n <= 4096
    static constexpr int INTERNAL_CAPACITY = 204;  // 8 + 16n + 4(n+1) <= 4096

    static page_id_t create(BufferPool& bp);  // returns the header page id
    BPlusTree(BufferPool& bp, page_id_t header_page,
              int leaf_max = LEAF_CAPACITY, int internal_max = INTERNAL_CAPACITY);

    void insert(int64_t key, const RID& rid);
    bool remove(int64_t key, const RID& rid);
    // Visit entries with lo <= key <= hi in sorted order; return false to stop.
    void scan_range(int64_t lo, int64_t hi, const std::function<bool(int64_t, const RID&)>& fn);
    std::vector<RID> lookup(int64_t key);
    int height();
    page_id_t header_page() const { return header_; }

private:
    struct Node {
        bool leaf = true;
        std::vector<IndexKey> keys;
        std::vector<page_id_t> children;  // internal nodes: keys.size() + 1 children
        page_id_t next = INVALID_PAGE_ID; // leaves: right sibling
    };

    Node load(page_id_t id);
    void store(page_id_t id, const Node& n);
    page_id_t new_node_page();
    page_id_t root();
    void set_root(page_id_t id);
    page_id_t find_leaf(const IndexKey& target);
    // Inserts into the subtree; if the node split, returns (separator, new right page).
    std::optional<std::pair<IndexKey, page_id_t>> insert_into(page_id_t id, const IndexKey& key);

    BufferPool& bp_;
    page_id_t header_;
    int leaf_max_, internal_max_;
};
