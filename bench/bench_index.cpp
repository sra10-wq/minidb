#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>
#include "database.h"

using Clock = std::chrono::steady_clock;

template <class F>
double time_ms(F&& f) {
    auto start = Clock::now();
    f();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

int main(int argc, char** argv) {
    const int N = argc > 1 ? std::atoi(argv[1]) : 100000;
    const int QUERIES = 200;
    const char* path = "bench.db";
    std::remove(path);
    Database db(path, 4096);  // 16 MB pool: the whole table fits in memory (warm cache)
    std::mt19937 rng(7);

    db.execute("CREATE TABLE t (id INT, val INT, name TEXT)");
    double load_ms = time_ms([&] {
        for (int i = 0; i < N; i += 1000) {
            std::string sql = "INSERT INTO t VALUES ";
            for (int j = i; j < std::min(N, i + 1000); j++)
                sql += (j > i ? "," : "") + std::string("(") + std::to_string(j) + "," +
                       std::to_string(rng() % N) + ",'name-" + std::to_string(j) + "')";
            db.execute(sql);
        }
    });

    std::vector<int64_t> keys;
    for (int q = 0; q < QUERIES; q++) keys.push_back(rng() % N);
    const int64_t width = N / 100;  // range queries cover ~1% of the key space

    auto point = [&](std::size_t& rows) {
        rows = 0;
        return time_ms([&] {
            for (int64_t k : keys)
                rows += db.execute("SELECT * FROM t WHERE val = " + std::to_string(k)).rows.size();
        }) / QUERIES;
    };
    auto range = [&](std::size_t& rows) {
        rows = 0;
        return time_ms([&] {
            for (int64_t k : keys)
                rows += db.execute("SELECT * FROM t WHERE val >= " + std::to_string(k) +
                                   " AND val < " + std::to_string(k + width)).rows.size();
        }) / QUERIES;
    };

    std::size_t scan_pt_rows, scan_rg_rows, idx_pt_rows, idx_rg_rows;
    double scan_pt = point(scan_pt_rows), scan_rg = range(scan_rg_rows);
    double build_ms = time_ms([&] { db.execute("CREATE INDEX idx_val ON t (val)"); });
    double idx_pt = point(idx_pt_rows), idx_rg = range(idx_rg_rows);

    if (scan_pt_rows != idx_pt_rows || scan_rg_rows != idx_rg_rows) {
        std::cerr << "MISMATCH: index and scan returned different rows\n";
        return 1;
    }

    auto& bp = db.buffer_pool();
    double hit_rate = 100.0 * bp.hits() / static_cast<double>(bp.hits() + bp.misses());
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "rows: " << N << "   load: " << load_ms / 1000 << " s   index build: " << build_ms << " ms\n\n";
    std::cout << "query (avg of " << QUERIES << ")      full scan     index scan    speedup\n";
    std::cout << "point  (val = k)          " << std::setw(9) << scan_pt << " ms  " << std::setw(9) << idx_pt
              << " ms  " << std::setprecision(0) << std::setw(6) << scan_pt / idx_pt << "x\n" << std::setprecision(3);
    std::cout << "range  (~1% of rows)      " << std::setw(9) << scan_rg << " ms  " << std::setw(9) << idx_rg
              << " ms  " << std::setprecision(0) << std::setw(6) << scan_rg / idx_rg << "x\n";
    std::cout << std::setprecision(1) << "\nbuffer pool hit rate: " << hit_rate << "%\n";
    std::remove(path);
    return 0;
}
