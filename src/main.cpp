#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include "database.h"

static void print_result(const QueryResult& r, double ms) {
    std::cout << std::fixed << std::setprecision(2);
    if (r.columns.empty()) {
        std::cout << r.message << " (" << ms << " ms)\n";
        return;
    }
    const std::size_t MAX_WIDTH = 40;
    auto cell = [&](const Value& v) {
        std::string s = value_to_string(v);
        return s.size() > MAX_WIDTH ? s.substr(0, MAX_WIDTH - 3) + "..." : s;
    };
    std::vector<std::size_t> w;
    for (const std::string& c : r.columns) w.push_back(c.size());
    for (const Row& row : r.rows)
        for (std::size_t i = 0; i < row.size(); i++) w[i] = std::max(w[i], cell(row[i]).size());

    auto rule = [&] {
        for (std::size_t x : w) std::cout << '+' << std::string(x + 2, '-');
        std::cout << "+\n";
    };
    rule();
    for (std::size_t i = 0; i < w.size(); i++) std::cout << "| " << std::left << std::setw(w[i]) << r.columns[i] << ' ';
    std::cout << "|\n";
    rule();
    for (const Row& row : r.rows) {
        for (std::size_t i = 0; i < row.size(); i++) std::cout << "| " << std::left << std::setw(w[i]) << cell(row[i]) << ' ';
        std::cout << "|\n";
    }
    if (!r.rows.empty()) rule();
    std::cout << r.rows.size() << " row(s) (" << ms << " ms)\n";
}

int main(int argc, char** argv) {
    std::string path = argc > 1 ? argv[1] : "minidb.db";
    Database db(path);
    std::cout << "minidb (" << path << "). End statements with ';'. Commands: .tables .exit\n";

    std::string buffer, line;
    while (true) {
        std::cout << (buffer.empty() ? "minidb> " : "   ...> ") << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (buffer.empty() && !line.empty() && line[0] == '.') {
            if (line == ".exit") break;
            if (line == ".tables") {
                for (const std::string& name : db.table_names()) std::cout << name << "\n";
            } else {
                std::cout << "unknown command " << line << "\n";
            }
            continue;
        }
        buffer += line + "\n";
        auto end = line.find_last_not_of(" \t\r");
        if (end == std::string::npos || line[end] != ';') continue;  // keep reading until ';'

        auto start = std::chrono::steady_clock::now();
        try {
            QueryResult r = db.execute(buffer);
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            print_result(r, ms);
        } catch (const std::exception& e) {
            std::cout << "Error: " << e.what() << "\n";
        }
        buffer.clear();
    }
    return 0;
}
