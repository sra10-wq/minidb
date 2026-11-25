#include <algorithm>
#include <cstdio>
#include <iostream>
#include <random>
#include <string>
#include <vector>
#include "database.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::cerr << "FAILED: " #cond " (line " << __LINE__ << ")\n"; return 1; } } while (0)

// Rows as sorted strings, so results from different join strategies can be compared.
static std::vector<std::string> rows_of(const QueryResult& r) {
    std::vector<std::string> out;
    for (const Row& row : r.rows) {
        std::string line;
        for (const Value& v : row) line += value_to_string(v) + "|";
        out.push_back(line);
    }
    std::sort(out.begin(), out.end());
    return out;
}

int main() {
    const char* path = "test_join.db";
    std::remove(path);
    {
        Database db(path);
        auto fails = [&](const std::string& sql) {
            try { db.execute(sql); } catch (const std::exception&) { return true; }
            return false;
        };
        db.execute("CREATE TABLE users (id INT, name TEXT)");
        db.execute("CREATE TABLE orders (oid INT, user_id INT, total INT)");
        std::string sql = "INSERT INTO users VALUES ";
        for (int i = 0; i < 300; i++) sql += (i ? "," : "") + std::string("(") + std::to_string(i) + ",'u" + std::to_string(i) + "')";
        db.execute(sql);
        std::mt19937 rng(3);
        for (int b = 0; b < 30; b++) {  // 3000 orders; user_id up to 399 so some have no matching user
            sql = "INSERT INTO orders VALUES ";
            for (int j = 0; j < 100; j++) {
                int oid = b * 100 + j;
                sql += (j ? "," : "") + std::string("(") + std::to_string(oid) + "," + std::to_string(rng() % 400) + "," +
                       std::to_string(rng() % 1000) + ")";
            }
            db.execute(sql);
        }

        std::string q1 = "SELECT users.name, orders.total FROM users JOIN orders ON users.id = orders.user_id";
        std::string q2 = q1 + " WHERE orders.total > 500 AND users.id < 100";
        std::string q3 = "SELECT orders.oid, users.name FROM orders JOIN users ON orders.user_id = users.id";

        // baseline: no indexes -> nested loop
        auto r = db.execute(q1);
        CHECK(r.plan == "nested loop join");
        auto base1 = rows_of(r), base2 = rows_of(db.execute(q2)), base3 = rows_of(db.execute(q3));
        CHECK(!base1.empty() && base1.size() < 3000);  // orders with user_id >= 300 don't match

        db.execute("CREATE INDEX idx_order_user ON orders (user_id)");
        // q1: the joined table (orders) has the index
        r = db.execute(q1);
        CHECK(r.plan == "index nested loop join using idx_order_user");
        CHECK(rows_of(r) == base1);
        CHECK(rows_of(db.execute(q2)) == base2);
        // q3: the outer table (orders) has the index -> probes from the users side
        r = db.execute(q3);
        CHECK(r.plan == "index nested loop join using idx_order_user");
        CHECK(rows_of(r) == base3);

        CHECK(db.execute(q1 + " LIMIT 5").rows.size() == 5);
        CHECK(fails("SELECT * FROM users JOIN orders ON users.id = users.id"));     // same table both sides
        CHECK(fails("SELECT * FROM users JOIN orders ON users.name = orders.oid"));  // type mismatch
    }
    std::remove(path);
    std::cout << "join tests passed\n";
    return 0;
}
