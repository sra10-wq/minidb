#include <algorithm>
#include <cstdio>
#include <iostream>
#include <random>
#include <string>
#include <vector>
#include "database.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::cerr << "FAILED: " #cond " (line " << __LINE__ << ")\n"; return 1; } } while (0)

// Sorted ids returned by a query, so index and scan results can be compared.
static std::vector<int64_t> ids(Database& db, const std::string& sql) {
    std::vector<int64_t> out;
    for (const Row& r : db.execute(sql).rows) out.push_back(std::get<int64_t>(r[0]));
    std::sort(out.begin(), out.end());
    return out;
}

// Same condition on indexed column a vs unindexed column b must give identical results.
static bool same(Database& db, const std::string& cond) {
    std::string ca = cond, cb = cond;
    for (auto& c : cb) if (c == '#') c = 'b';
    for (auto& c : ca) if (c == '#') c = 'a';
    return ids(db, "SELECT id FROM t WHERE " + ca) == ids(db, "SELECT id FROM t WHERE " + cb);
}

int main() {
    const char* path = "test_index.db";
    std::remove(path);
    std::vector<std::string> conds = {"# = 42", "# >= 100 AND # < 150", "# > 990", "# <= 3",
                                      "# = 5 AND id > 1000", "# < -1", "# >= 0 AND # <= 999"};
    {
        Database db(path);
        auto fails = [&](const std::string& sql) {
            try { db.execute(sql); } catch (const std::exception&) { return true; }
            return false;
        };
        db.execute("CREATE TABLE t (id INT, a INT, b INT, note TEXT)");
        std::mt19937 rng(1);
        for (int batch = 0; batch < 20; batch++) {  // 2000 rows, a == b, many duplicates
            std::string sql = "INSERT INTO t VALUES ";
            for (int j = 0; j < 100; j++) {
                int id = batch * 100 + j, v = static_cast<int>(rng() % 1000);
                sql += (j ? "," : "") + std::string("(") + std::to_string(id) + "," + std::to_string(v) + "," +
                       std::to_string(v) + ",'n')";
            }
            db.execute(sql);
        }

        CHECK(db.execute("SELECT id FROM t WHERE a = 42").plan == "full table scan");
        CHECK(db.execute("CREATE INDEX idx_a ON t (a)").message == "index 'idx_a' created (2000 rows)");
        CHECK(db.execute("SELECT id FROM t WHERE a = 42").plan == "index scan using idx_a");
        CHECK(db.execute("SELECT id FROM t WHERE b = 42").plan == "full table scan");
        CHECK(db.execute("SELECT id FROM t WHERE a != 42").plan == "full table scan");
        for (const auto& c : conds) CHECK(same(db, c));

        // writes keep the index in sync (including rows that move on update)
        db.execute("UPDATE t SET a = 7, b = 7 WHERE a >= 500 AND a < 520");
        db.execute("UPDATE t SET note = 'a much longer note that forces the row to move', a = 8, b = 8 WHERE id < 50");
        db.execute("DELETE FROM t WHERE a > 900");
        db.execute("INSERT INTO t VALUES (5000, 42, 42, 'new')");
        for (const auto& c : conds) CHECK(same(db, c));
        CHECK(same(db, "# = 7") && same(db, "# = 8"));
        CHECK(db.execute("DELETE FROM t WHERE a = 8").plan == "index scan using idx_a");

        CHECK(fails("CREATE INDEX idx_a ON t (b)"));     // duplicate name
        CHECK(fails("CREATE INDEX idx_n ON t (note)"));  // TEXT column
        CHECK(fails("CREATE INDEX idx_a2 ON t (a)"));    // already indexed
    }
    {
        // index survives restart and is still used and correct
        Database db(path);
        CHECK(db.execute("SELECT id FROM t WHERE a = 42").plan == "index scan using idx_a");
        for (const auto& c : conds) CHECK(same(db, c));
    }
    std::remove(path);
    std::cout << "index tests passed\n";
    return 0;
}
