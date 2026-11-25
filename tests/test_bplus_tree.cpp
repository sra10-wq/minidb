#include <climits>
#include <cstdio>
#include <iostream>
#include <map>
#include <random>
#include <vector>
#include "bplus_tree.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::cerr << "FAILED: " #cond " (line " << __LINE__ << ")\n"; return 1; } } while (0)

// Collect (key, page) pairs from a range scan.
static std::vector<std::pair<int64_t, int>> scan(BPlusTree& t, int64_t lo, int64_t hi) {
    std::vector<std::pair<int64_t, int>> out;
    t.scan_range(lo, hi, [&](int64_t k, const RID& r) { out.push_back({k, r.page_id}); return true; });
    return out;
}

int main() {
    const char* path = "test_bptree.db";
    std::remove(path);
    page_id_t header;
    std::multimap<int64_t, int> ref;  // reference answer: key -> row number

    {
        DiskManager dm(path);
        BufferPool bp(16, dm);
        header = BPlusTree::create(bp);
        BPlusTree t(bp, header, 4, 4);  // tiny nodes -> many splits, deep tree
        CHECK(t.height() == 0 && t.lookup(1).empty());

        // 1. 3000 random keys with many duplicates and negatives
        std::mt19937 rng(42);
        for (int i = 0; i < 3000; i++) {
            int64_t key = static_cast<int64_t>(rng() % 1000) - 500;
            t.insert(key, RID{i, static_cast<uint16_t>(i % 10)});
            ref.insert({key, i});
        }
        CHECK(t.height() >= 4);

        // 2. point lookups return every duplicate
        for (int64_t key = -500; key < 500; key += 37)
            CHECK(t.lookup(key).size() == ref.count(key));

        // 3. range scan: correct count, sorted, within bounds
        auto r = scan(t, -100, 100);
        std::size_t expected = 0;
        for (auto it = ref.lower_bound(-100); it != ref.upper_bound(100); ++it) expected++;
        CHECK(r.size() == expected);
        for (std::size_t i = 1; i < r.size(); i++) CHECK(r[i - 1].first <= r[i].first);
        CHECK(r.front().first >= -100 && r.back().first <= 100);

        // 4. delete every entry whose key is divisible by 3
        std::vector<std::pair<int64_t, int>> to_remove;
        for (auto& [k, i] : ref) if (k % 3 == 0) to_remove.push_back({k, i});
        for (auto& [k, i] : to_remove) CHECK(t.remove(k, RID{i, static_cast<uint16_t>(i % 10)}));
        for (auto it = ref.begin(); it != ref.end();) it = (it->first % 3 == 0) ? ref.erase(it) : std::next(it);
        CHECK(!t.remove(9999, RID{1, 1}));  // missing entry
        CHECK(t.lookup(0).empty() && t.lookup(3).empty());

        auto all = scan(t, INT64_MIN, INT64_MAX);
        CHECK(all.size() == ref.size());
        for (std::size_t i = 1; i < all.size(); i++) CHECK(all[i - 1].first <= all[i].first);
    }

    {
        // 5. the tree persists across restarts
        DiskManager dm(path);
        BufferPool bp(16, dm);
        BPlusTree t(bp, header, 4, 4);
        CHECK(scan(t, INT64_MIN, INT64_MAX).size() == ref.size());
        CHECK(t.lookup(1).size() == ref.count(1));
    }
    std::remove(path);

    {
        // 6. full-size nodes: 100K sequential keys stay shallow
        DiskManager dm(path);
        BufferPool bp(64, dm);
        BPlusTree t(bp, BPlusTree::create(bp));
        for (int i = 0; i < 100000; i++) t.insert(i, RID{i, 0});
        CHECK(t.height() <= 3);
        CHECK(t.lookup(77777).size() == 1 && t.lookup(77777)[0].page_id == 77777);
        CHECK(scan(t, 500, 1499).size() == 1000);
        std::cout << "100K keys -> height " << t.height() << ", " << dm.num_pages() << " pages\n";
    }
    std::remove(path);

    std::cout << "b+ tree tests passed\n";
    return 0;
}
