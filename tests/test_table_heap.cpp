#include <cstdio>
#include <iostream>
#include <vector>
#include "table_heap.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::cerr << "FAILED: " #cond " (line " << __LINE__ << ")\n"; return 1; } } while (0)

int main() {
    const char* path = "test_table.db";
    std::remove(path);
    Schema schema = {{"id", Type::INT}, {"name", Type::TEXT}, {"age", Type::INT}};
    page_id_t first;

    {
        DiskManager dm(path);
        BufferPool bp(4, dm);  // tiny pool: forces evictions during the test
        first = TableHeap::create(bp);
        TableHeap t(bp, first, schema);

        // 1. insert 1000 rows -> spans many pages
        std::vector<RID> rids;
        for (int64_t i = 0; i < 1000; i++)
            rids.push_back(t.insert({i, "user-" + std::to_string(i), i % 90}));
        CHECK(dm.num_pages() > 5);

        // 2. point lookups by RID
        for (int64_t i = 0; i < 1000; i += 97) {
            auto row = t.get(rids[i]);
            CHECK(row && std::get<int64_t>((*row)[0]) == i);
            CHECK(std::get<std::string>((*row)[1]) == "user-" + std::to_string(i));
        }

        // 3. full scan sees every row
        int count = 0;
        t.scan([&](const RID&, const Row&) { count++; return true; });
        CHECK(count == 1000);

        // 4. delete even ids
        for (int64_t i = 0; i < 1000; i += 2) CHECK(t.remove(rids[i]));
        CHECK(!t.get(rids[0]));
        CHECK(!t.remove(rids[0]));  // double delete fails
        count = 0;
        bool all_odd = true;
        t.scan([&](const RID&, const Row& r) {
            if (std::get<int64_t>(r[0]) % 2 == 0) all_odd = false;
            count++;
            return true;
        });
        CHECK(all_odd && count == 500);

        // 5. update: smaller row stays in place, larger row moves
        CHECK(t.update(rids[1], {int64_t(1), std::string("x"), int64_t(5)}) == rids[1]);
        RID moved = t.update(rids[3], {int64_t(3), std::string(200, 'z'), int64_t(7)});
        CHECK(!(moved == rids[3]));
        auto row = t.get(moved);
        CHECK(row && std::get<std::string>((*row)[1]).size() == 200);
        CHECK(!t.get(rids[3]));

        // 6. type mismatches are rejected
        bool threw = false;
        try { t.insert({std::string("oops"), std::string("a"), int64_t(1)}); } catch (...) { threw = true; }
        CHECK(threw);
    }

    {
        // 7. everything persists after restart
        DiskManager dm(path);
        BufferPool bp(4, dm);
        TableHeap t(bp, first, schema);
        int count = 0;
        t.scan([&](const RID&, const Row&) { count++; return true; });
        CHECK(count == 500);
    }

    std::remove(path);
    std::cout << "table heap tests passed\n";
    return 0;
}
