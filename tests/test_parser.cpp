#include <iostream>
#include "sql_ast.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::cerr << "FAILED: " #cond " (line " << __LINE__ << ")\n"; return 1; } } while (0)

static bool fails(const std::string& sql) {
    try { parse_sql(sql); } catch (const std::exception&) { return true; }
    return false;
}

int main() {
    // CREATE TABLE
    auto ct = std::get<CreateTable>(parse_sql("CREATE TABLE users (id INT, name TEXT, age int);"));
    CHECK(ct.table == "users" && ct.columns.size() == 3);
    CHECK(ct.columns[1].name == "name" && ct.columns[1].type == Type::TEXT);

    // CREATE INDEX
    auto ci = std::get<CreateIndex>(parse_sql("create index idx_age on users (age)"));
    CHECK(ci.name == "idx_age" && ci.table == "users" && ci.column == "age");

    // INSERT with multiple rows, negative numbers, escaped quotes
    auto ins = std::get<Insert>(parse_sql("INSERT INTO users VALUES (1, 'O''Brien', 30), (2, 'bo', -5)"));
    CHECK(ins.rows.size() == 2);
    CHECK(std::get<std::string>(ins.rows[0][1]) == "O'Brien");
    CHECK(std::get<int64_t>(ins.rows[1][2]) == -5);

    // SELECT with WHERE ... AND ... and LIMIT
    auto sel = std::get<Select>(parse_sql("SELECT name, age FROM users WHERE age >= 21 AND name != 'x' LIMIT 5"));
    CHECK(!sel.star && sel.columns.size() == 2 && sel.table == "users");
    CHECK(sel.where.size() == 2 && sel.where[0].op == CmpOp::GE);
    CHECK(std::get<int64_t>(sel.where[0].rhs_value) == 21);
    CHECK(sel.where[1].op == CmpOp::NE && sel.limit && *sel.limit == 5);

    // SELECT * with JOIN
    auto j = std::get<Select>(parse_sql("select * from users join orders on users.id = orders.user_id where orders.total > 100"));
    CHECK(j.star && j.join && j.join->table == "orders");
    CHECK(j.join->left.table == "users" && j.join->right.column == "user_id");
    CHECK(j.where[0].left.table == "orders");

    // UPDATE and DELETE
    auto up = std::get<Update>(parse_sql("UPDATE users SET age = 31, name = 'al' WHERE id = 1"));
    CHECK(up.sets.size() == 2 && up.sets[0].first == "age" && up.where.size() == 1);
    auto del = std::get<Delete>(parse_sql("DELETE FROM users"));
    CHECK(del.table == "users" && del.where.empty());

    // errors are reported, not crashes
    CHECK(fails("SELEC * FROM users"));
    CHECK(fails("SELECT * FROM"));
    CHECK(fails("INSERT INTO users VALUES (1, 'unterminated)"));
    CHECK(fails("CREATE TABLE t (id FLOAT)"));
    CHECK(fails("SELECT * FROM users WHERE age ~ 3"));
    CHECK(fails("DELETE FROM users extra"));

    std::cout << "parser tests passed\n";
    return 0;
}
