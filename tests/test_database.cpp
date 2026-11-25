#include <cstdio>
#include <iostream>
#include "database.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::cerr << "FAILED: " #cond " (line " << __LINE__ << ")\n"; return 1; } } while (0)

static int64_t as_int(const Value& v) { return std::get<int64_t>(v); }
static std::string as_text(const Value& v) { return std::get<std::string>(v); }

int main() {
    const char* path = "test_database.db";
    std::remove(path);

    {
        Database db(path);
        auto fails = [&](const std::string& sql) {
            try { db.execute(sql); } catch (const std::exception&) { return true; }
            return false;
        };

        db.execute("CREATE TABLE users (id INT, name TEXT, age INT)");
        db.execute("INSERT INTO users VALUES (1, 'ann', 30), (2, 'bob', 17), (3, 'cat', 45), (4, 'dan', 22)");

        // filter + projection
        auto r = db.execute("SELECT name FROM users WHERE age >= 21 AND age < 40");
        CHECK(r.columns.size() == 1 && r.columns[0] == "name");
        CHECK(r.rows.size() == 2 && as_text(r.rows[0][0]) == "ann" && as_text(r.rows[1][0]) == "dan");

        // SELECT * and LIMIT
        r = db.execute("SELECT * FROM users LIMIT 2");
        CHECK(r.rows.size() == 2 && r.columns.size() == 3);

        // JOIN
        db.execute("CREATE TABLE orders (oid INT, user_id INT, total INT)");
        db.execute("INSERT INTO orders VALUES (10, 1, 50), (11, 1, 200), (12, 3, 120), (13, 9, 5)");
        r = db.execute("SELECT users.name, orders.total FROM users JOIN orders ON users.id = orders.user_id WHERE orders.total > 100");
        CHECK(r.rows.size() == 2);
        CHECK(as_text(r.rows[0][0]) == "ann" && as_int(r.rows[0][1]) == 200);
        CHECK(as_text(r.rows[1][0]) == "cat" && as_int(r.rows[1][1]) == 120);

        // UPDATE
        CHECK(db.execute("UPDATE users SET age = 18 WHERE name = 'bob'").message == "1 row(s) updated");
        r = db.execute("SELECT age FROM users WHERE id = 2");
        CHECK(r.rows.size() == 1 && as_int(r.rows[0][0]) == 18);

        // DELETE (removes bob 18 and dan 22)
        CHECK(db.execute("DELETE FROM users WHERE age < 25").message == "2 row(s) deleted");
        CHECK(db.execute("SELECT * FROM users").rows.size() == 2);

        // errors are caught before any rows change
        CHECK(fails("SELECT * FROM nope"));
        CHECK(fails("SELECT nope FROM users"));
        CHECK(fails("INSERT INTO users VALUES (5, 'x')"));
        CHECK(fails("INSERT INTO users VALUES (5, 'ok', 1), ('bad', 'x', 1)"));
        CHECK(db.execute("SELECT * FROM users").rows.size() == 2);  // first row of bad insert not kept
        CHECK(fails("SELECT * FROM users WHERE age = 'old'"));
        CHECK(fails("CREATE TABLE users (a INT)"));
        CHECK(fails("CREATE TABLE t2 (a INT, a TEXT)"));
        CHECK(fails("UPDATE users SET age = 'x'"));
    }

    {
        // tables and data survive a restart
        Database db(path);
        CHECK(db.table_names().size() == 2);
        auto r = db.execute("SELECT name FROM users WHERE id = 3");
        CHECK(r.rows.size() == 1 && as_text(r.rows[0][0]) == "cat");
    }

    std::remove(path);
    std::cout << "database tests passed\n";
    return 0;
}
