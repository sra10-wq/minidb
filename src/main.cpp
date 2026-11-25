#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::string db_file = argc > 1 ? argv[1] : "test.db";
    std::cout << "minidb (file: " << db_file << "). Type .exit to quit.\n";
    std::string line;
    while (true) {
        std::cout << "minidb> " << std::flush;
        if (!std::getline(std::cin, line) || line == ".exit") break;
        if (line.empty()) continue;
        std::cout << "you typed: " << line << "\n";
    }
    return 0;
}
