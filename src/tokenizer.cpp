#include "tokenizer.h"
#include <cctype>
#include <stdexcept>

static bool is_alpha(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
static bool is_alnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
static bool is_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)); }

std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> out;
    std::size_t i = 0, n = s.size();
    while (i < n) {
        char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c))) { i++; continue; }

        if (is_alpha(c)) {  // keyword or name (case-insensitive)
            std::size_t j = i;
            while (j < n && is_alnum(s[j])) j++;
            std::string word = s.substr(i, j - i);
            for (char& ch : word) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            out.push_back({TokType::IDENT, word});
            i = j;
            continue;
        }
        if (is_digit(c) || (c == '-' && i + 1 < n && is_digit(s[i + 1]))) {  // integer
            std::size_t j = i + 1;
            while (j < n && is_digit(s[j])) j++;
            out.push_back({TokType::NUMBER, s.substr(i, j - i)});
            i = j;
            continue;
        }
        if (c == '\'') {  // 'string', with '' as an escaped quote
            std::string v;
            std::size_t j = i + 1;
            while (true) {
                if (j >= n) throw std::runtime_error("unterminated string literal");
                if (s[j] == '\'') {
                    if (j + 1 < n && s[j + 1] == '\'') { v += '\''; j += 2; continue; }
                    break;
                }
                v += s[j++];
            }
            out.push_back({TokType::STRING, v});
            i = j + 1;
            continue;
        }
        if (i + 1 < n) {  // two-character operators
            std::string two = s.substr(i, 2);
            if (two == "<=" || two == ">=" || two == "!=" || two == "<>") {
                out.push_back({TokType::SYMBOL, two == "<>" ? "!=" : two});
                i += 2;
                continue;
            }
        }
        if (std::string("(),;*=<>.").find(c) != std::string::npos) {
            out.push_back({TokType::SYMBOL, std::string(1, c)});
            i++;
            continue;
        }
        throw std::runtime_error(std::string("unexpected character '") + c + "'");
    }
    out.push_back({TokType::END, ""});
    return out;
}
