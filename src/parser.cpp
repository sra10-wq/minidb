#include <stdexcept>
#include "sql_ast.h"
#include "tokenizer.h"

namespace {

// Recursive-descent parser: one method per grammar rule.
class Parser {
public:
    explicit Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}

    Statement parse() {
        Statement st;
        if (accept_kw("create")) {
            if (accept_kw("table")) st = create_table();
            else if (accept_kw("index")) st = create_index();
            else fail("expected TABLE or INDEX after CREATE");
        } else if (accept_kw("insert")) st = insert();
        else if (accept_kw("select")) st = select();
        else if (accept_kw("update")) st = update();
        else if (accept_kw("delete")) st = del();
        else fail("expected CREATE, INSERT, SELECT, UPDATE, or DELETE");
        accept_sym(";");
        if (peek().type != TokType::END) fail("unexpected input after statement");
        return st;
    }

private:
    // ---- token helpers ----
    const Token& peek() const { return toks_[pos_]; }
    [[noreturn]] void fail(const std::string& msg) const {
        std::string where = peek().type == TokType::END ? " at end of input" : " near '" + peek().text + "'";
        throw std::runtime_error("parse error: " + msg + where);
    }
    bool accept_kw(const std::string& kw) {
        if (peek().type == TokType::IDENT && peek().text == kw) { pos_++; return true; }
        return false;
    }
    void expect_kw(const std::string& kw) { if (!accept_kw(kw)) fail("expected " + kw); }
    bool accept_sym(const std::string& sym) {
        if (peek().type == TokType::SYMBOL && peek().text == sym) { pos_++; return true; }
        return false;
    }
    void expect_sym(const std::string& sym) { if (!accept_sym(sym)) fail("expected '" + sym + "'"); }
    std::string ident() {
        if (peek().type != TokType::IDENT) fail("expected a name");
        return toks_[pos_++].text;
    }
    Value literal() {
        const Token& t = peek();
        if (t.type == TokType::NUMBER) { pos_++; return Value(static_cast<int64_t>(std::stoll(t.text))); }
        if (t.type == TokType::STRING) { pos_++; return Value(t.text); }
        fail("expected a number or 'string'");
    }
    ColumnRef column_ref() {
        std::string first = ident();
        if (accept_sym(".")) return {first, ident()};
        return {"", first};
    }

    // ---- grammar rules ----
    CreateTable create_table() {  // CREATE TABLE t (col INT, col TEXT, ...)
        CreateTable ct;
        ct.table = ident();
        expect_sym("(");
        do {
            std::string col = ident();
            if (accept_kw("int") || accept_kw("integer")) ct.columns.push_back({col, Type::INT});
            else if (accept_kw("text") || accept_kw("varchar")) ct.columns.push_back({col, Type::TEXT});
            else fail("expected column type INT or TEXT");
        } while (accept_sym(","));
        expect_sym(")");
        return ct;
    }

    CreateIndex create_index() {  // CREATE INDEX name ON t (col)
        CreateIndex ci;
        ci.name = ident();
        expect_kw("on");
        ci.table = ident();
        expect_sym("(");
        ci.column = ident();
        expect_sym(")");
        return ci;
    }

    Insert insert() {  // INSERT INTO t VALUES (...), (...)
        Insert ins;
        expect_kw("into");
        ins.table = ident();
        expect_kw("values");
        do {
            expect_sym("(");
            Row row;
            do { row.push_back(literal()); } while (accept_sym(","));
            expect_sym(")");
            ins.rows.push_back(std::move(row));
        } while (accept_sym(","));
        return ins;
    }

    Select select() {  // SELECT cols FROM t [JOIN t2 ON a = b] [WHERE ...] [LIMIT n]
        Select sel;
        if (accept_sym("*")) sel.star = true;
        else do { sel.columns.push_back(column_ref()); } while (accept_sym(","));
        expect_kw("from");
        sel.table = ident();
        if (accept_kw("join")) {
            Join j;
            j.table = ident();
            expect_kw("on");
            j.left = column_ref();
            expect_sym("=");
            j.right = column_ref();
            sel.join = j;
        }
        if (accept_kw("where")) sel.where = where();
        if (accept_kw("limit")) {
            if (peek().type != TokType::NUMBER) fail("expected a number after LIMIT");
            sel.limit = std::stoll(toks_[pos_++].text);
        }
        return sel;
    }

    Update update() {  // UPDATE t SET col = val, ... [WHERE ...]
        Update up;
        up.table = ident();
        expect_kw("set");
        do {
            std::string col = ident();
            expect_sym("=");
            up.sets.push_back({col, literal()});
        } while (accept_sym(","));
        if (accept_kw("where")) up.where = where();
        return up;
    }

    Delete del() {  // DELETE FROM t [WHERE ...]
        Delete d;
        expect_kw("from");
        d.table = ident();
        if (accept_kw("where")) d.where = where();
        return d;
    }

    Where where() {  // cond AND cond AND ...
        Where w;
        do {
            Condition c;
            c.left = column_ref();
            c.op = cmp_op();
            if (peek().type == TokType::IDENT) {
                c.rhs_is_column = true;
                c.rhs_column = column_ref();
            } else {
                c.rhs_value = literal();
            }
            w.push_back(std::move(c));
        } while (accept_kw("and"));
        return w;
    }

    CmpOp cmp_op() {
        if (accept_sym("=")) return CmpOp::EQ;
        if (accept_sym("!=")) return CmpOp::NE;
        if (accept_sym("<=")) return CmpOp::LE;
        if (accept_sym(">=")) return CmpOp::GE;
        if (accept_sym("<")) return CmpOp::LT;
        if (accept_sym(">")) return CmpOp::GT;
        fail("expected a comparison operator");
    }

    std::vector<Token> toks_;
    std::size_t pos_ = 0;
};

}  // namespace

Statement parse_sql(const std::string& sql) {
    return Parser(tokenize(sql)).parse();
}
