#include "bplus_tree.h"
#include <algorithm>
#include <climits>
#include <cstring>
#include <stdexcept>

static constexpr std::size_t NODE_HEADER = 8;
static constexpr std::size_t KEY_SIZE = 16;

static Page* fetch_or_throw(BufferPool& bp, page_id_t id) {
    Page* p = bp.fetch_page(id);
    if (!p) throw std::runtime_error("buffer pool exhausted");
    return p;
}

page_id_t BPlusTree::create(BufferPool& bp) {
    Page* p = bp.new_page();
    if (!p) throw std::runtime_error("buffer pool exhausted");
    page_id_t root = INVALID_PAGE_ID;  // empty tree
    std::memcpy(p->data, &root, 4);
    page_id_t id = p->id;
    bp.unpin_page(id, true);
    return id;
}

BPlusTree::BPlusTree(BufferPool& bp, page_id_t header_page, int leaf_max, int internal_max)
    : bp_(bp), header_(header_page), leaf_max_(leaf_max), internal_max_(internal_max) {
    if (leaf_max < 3 || leaf_max > LEAF_CAPACITY || internal_max < 3 || internal_max > INTERNAL_CAPACITY)
        throw std::invalid_argument("bad B+ tree node capacity");
}

// ---------------- page <-> node ----------------

BPlusTree::Node BPlusTree::load(page_id_t id) {
    Page* p = fetch_or_throw(bp_, id);
    const char* d = p->data;
    Node n;
    n.leaf = d[0] == 1;
    uint16_t count;
    std::memcpy(&count, d + 2, 2);
    std::memcpy(&n.next, d + 4, 4);
    n.keys.resize(count);
    for (uint16_t i = 0; i < count; i++) {
        const char* e = d + NODE_HEADER + i * KEY_SIZE;
        std::memcpy(&n.keys[i].key, e, 8);
        std::memcpy(&n.keys[i].page, e + 8, 4);
        std::memcpy(&n.keys[i].slot, e + 12, 2);
    }
    if (!n.leaf) {
        n.children.resize(count + 1);
        std::memcpy(n.children.data(), d + NODE_HEADER + count * KEY_SIZE, 4 * (count + 1));
    }
    bp_.unpin_page(id, false);
    return n;
}

void BPlusTree::store(page_id_t id, const Node& n) {
    Page* p = fetch_or_throw(bp_, id);
    char* d = p->data;
    std::memset(d, 0, PAGE_SIZE);
    d[0] = n.leaf ? 1 : 0;
    uint16_t count = static_cast<uint16_t>(n.keys.size());
    std::memcpy(d + 2, &count, 2);
    std::memcpy(d + 4, &n.next, 4);
    for (uint16_t i = 0; i < count; i++) {
        char* e = d + NODE_HEADER + i * KEY_SIZE;
        std::memcpy(e, &n.keys[i].key, 8);
        std::memcpy(e + 8, &n.keys[i].page, 4);
        std::memcpy(e + 12, &n.keys[i].slot, 2);
    }
    if (!n.leaf) std::memcpy(d + NODE_HEADER + count * KEY_SIZE, n.children.data(), 4 * n.children.size());
    bp_.unpin_page(id, true);
}

page_id_t BPlusTree::new_node_page() {
    Page* p = bp_.new_page();
    if (!p) throw std::runtime_error("buffer pool exhausted");
    page_id_t id = p->id;
    bp_.unpin_page(id, true);
    return id;
}

page_id_t BPlusTree::root() {
    Page* p = fetch_or_throw(bp_, header_);
    page_id_t r;
    std::memcpy(&r, p->data, 4);
    bp_.unpin_page(header_, false);
    return r;
}

void BPlusTree::set_root(page_id_t id) {
    Page* p = fetch_or_throw(bp_, header_);
    std::memcpy(p->data, &id, 4);
    bp_.unpin_page(header_, true);
}

// ---------------- search ----------------

// Internal node: child i holds entries in [keys[i-1], keys[i]).
// So follow child number = (how many separators are <= target).
page_id_t BPlusTree::find_leaf(const IndexKey& target) {
    page_id_t id = root();
    while (true) {
        Node n = load(id);
        if (n.leaf) return id;
        std::size_t i = std::upper_bound(n.keys.begin(), n.keys.end(), target) - n.keys.begin();
        id = n.children[i];
    }
}

void BPlusTree::scan_range(int64_t lo, int64_t hi, const std::function<bool(int64_t, const RID&)>& fn) {
    if (root() == INVALID_PAGE_ID || lo > hi) return;
    page_id_t id = find_leaf({lo, INT32_MIN, 0});  // smallest possible entry with key lo
    while (id != INVALID_PAGE_ID) {
        Node n = load(id);
        for (const IndexKey& k : n.keys) {
            if (k.key < lo) continue;
            if (k.key > hi) return;
            if (!fn(k.key, RID{k.page, k.slot})) return;
        }
        id = n.next;  // walk the leaf chain
    }
}

std::vector<RID> BPlusTree::lookup(int64_t key) {
    std::vector<RID> out;
    scan_range(key, key, [&](int64_t, const RID& rid) { out.push_back(rid); return true; });
    return out;
}

int BPlusTree::height() {
    page_id_t id = root();
    if (id == INVALID_PAGE_ID) return 0;
    int h = 1;
    while (true) {
        Node n = load(id);
        if (n.leaf) return h;
        id = n.children[0];
        h++;
    }
}

// ---------------- insert ----------------

std::optional<std::pair<IndexKey, page_id_t>> BPlusTree::insert_into(page_id_t id, const IndexKey& key) {
    Node n = load(id);

    if (n.leaf) {
        auto it = std::lower_bound(n.keys.begin(), n.keys.end(), key);
        if (it != n.keys.end() && *it == key) return std::nullopt;  // exact entry already present
        n.keys.insert(it, key);
        if (static_cast<int>(n.keys.size()) <= leaf_max_) {
            store(id, n);
            return std::nullopt;
        }
        // split leaf: upper half moves right; first right key is copied up as the separator
        Node right;
        right.leaf = true;
        std::size_t mid = n.keys.size() / 2;
        right.keys.assign(n.keys.begin() + mid, n.keys.end());
        n.keys.resize(mid);
        page_id_t right_id = new_node_page();
        right.next = n.next;
        n.next = right_id;
        store(id, n);
        store(right_id, right);
        return std::make_pair(right.keys.front(), right_id);
    }

    std::size_t i = std::upper_bound(n.keys.begin(), n.keys.end(), key) - n.keys.begin();
    auto split = insert_into(n.children[i], key);
    if (!split) return std::nullopt;

    // child split: add its separator and new right child here
    n.keys.insert(n.keys.begin() + i, split->first);
    n.children.insert(n.children.begin() + i + 1, split->second);
    if (static_cast<int>(n.keys.size()) <= internal_max_) {
        store(id, n);
        return std::nullopt;
    }
    // split internal node: the middle key moves up (it is not kept in either half)
    std::size_t mid = n.keys.size() / 2;
    IndexKey up = n.keys[mid];
    Node right;
    right.leaf = false;
    right.keys.assign(n.keys.begin() + mid + 1, n.keys.end());
    right.children.assign(n.children.begin() + mid + 1, n.children.end());
    n.keys.resize(mid);
    n.children.resize(mid + 1);
    page_id_t right_id = new_node_page();
    store(id, n);
    store(right_id, right);
    return std::make_pair(up, right_id);
}

void BPlusTree::insert(int64_t key, const RID& rid) {
    IndexKey k{key, rid.page_id, rid.slot};
    page_id_t r = root();
    if (r == INVALID_PAGE_ID) {  // first entry: a single leaf is the root
        Node leaf;
        leaf.keys.push_back(k);
        page_id_t id = new_node_page();
        store(id, leaf);
        set_root(id);
        return;
    }
    auto split = insert_into(r, k);
    if (split) {  // root split: tree grows one level
        Node new_root;
        new_root.leaf = false;
        new_root.keys = {split->first};
        new_root.children = {r, split->second};
        page_id_t id = new_node_page();
        store(id, new_root);
        set_root(id);
    }
}

// ---------------- delete ----------------

bool BPlusTree::remove(int64_t key, const RID& rid) {
    if (root() == INVALID_PAGE_ID) return false;
    IndexKey k{key, rid.page_id, rid.slot};
    page_id_t id = find_leaf(k);
    Node n = load(id);
    auto it = std::lower_bound(n.keys.begin(), n.keys.end(), k);
    if (it == n.keys.end() || !(*it == k)) return false;
    n.keys.erase(it);  // lazy deletion: no merging/rebalancing
    store(id, n);
    return true;
}
