#pragma once
#include <string>
#include <vector>

enum class TokType { IDENT, NUMBER, STRING, SYMBOL, END };
struct Token {
    TokType type;
    std::string text;  // identifiers are lowercased; strings have quotes removed
};

std::vector<Token> tokenize(const std::string& sql);
