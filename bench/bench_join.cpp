#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include "database.h"

using Clock = std::chrono::steady_clock;

template <class F>
double time_ms(F&& f) {
    auto start = Clock::now();
    f();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

int main(int argc, char** argv) {
    const int USERS = argc > 1 ? std::atoi(argv[1]) : 2000;
    const int ORDERS = argc > 2 ? std::atoi(argv[2]) : 20000;
    const int RUNS = 5;
    const char* path = "bench_join.db";
    std::remove(path);
    Database db(path, 4096);
    std::mt19937 rng(11);

    db.execute("CREATE TABLE users (id INT, name TEXT)");
    db.execute("CREATE TABLE orders (oid INT, user_id INT, total INT)");
    for (int i = 0; i < USERS; i += 1000) {
        std::string sql = "INSERT INTO users VALUES ";
        for (int j = i; j < std::min(USERS, i + 1000); j++)
            sql += (j > i ? "," : "") + std::string("(") + std::to_string(j) + ",'user-" + std::to_string(j) + "')";
        db.execute(sql);
    }
    for (int i = 0; i < ORDERS; i += 1000) {
        std::string sql = "INSERT INTO orders VALUES ";
        for (int j = i; j < std::min(ORDERS, i + 1000); j++)
            sql += (j > i ? "," : "") + std::string("(") + std::to_string(j) + "," + std::to_string(rng() % USERS) +
                   "," + std::to_string(rng() % 1000) + ")";
        db.execute(sql);
    }

    const std::string query = "SELECT users.name, orders.total FROM users JOIN orders ON users.id = orders.user_id";
    QueryResult nl;
    double nl_ms = time_ms([&] { nl = db.execute(query); });  // one run: it's slow

    db.execute("CREATE INDEX idx_orders_user ON orders (user_id)");
    QueryResult inl;
    double inl_ms = time_ms([&] { for (int r = 0; r < RUNS; r++) inl = db.execute(query); }) / RUNS;

    if (nl.rows.size() != inl.rows.size()) {
        std::cerr << "MISMATCH: strategies returned different row counts\n";
        return 1;
    }
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "users: " << USERS << "   orders: " << ORDERS << "   result rows: " << inl.rows.size() << "\n\n";
    std::cout << "nested loop join        " << std::setw(10) << nl_ms << " ms   (" << nl.plan << ")\n";
    std::cout << "index nested loop join  " << std::setw(10) << inl_ms << " ms   (" << inl.plan << ")\n";
    std::cout << std::setprecision(0) << "speedup: " << nl_ms / inl_ms << "x\n";
    std::remove(path);
    return 0;
}
