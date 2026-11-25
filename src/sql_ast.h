#pragma once
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include "types.h"

enum class CmpOp { EQ, NE, LT, LE, GT, GE };

struct ColumnRef {
    std::string table;   // empty unless written as table.column
    std::string column;
};

// left <op> right, where right is either a literal or another column (for joins)
struct Condition {
    ColumnRef left;
    CmpOp op;
    bool rhs_is_column = false;
    ColumnRef rhs_column;
    Value rhs_value;
};
using Where = std::vector<Condition>;  // all conditions ANDed together

struct Join {
    std::string table;
    ColumnRef left, right;  // ON left = right
};

struct CreateTable { std::string table; Schema columns; };
struct CreateIndex { std::string name, table, column; };
struct Insert { std::string table; std::vector<Row> rows; };
struct Select {
    bool star = false;
    std::vector<ColumnRef> columns;
    std::string table;
    std::optional<Join> join;
    Where where;
    std::optional<int64_t> limit;
};
struct Update { std::string table; std::vector<std::pair<std::string, Value>> sets; Where where; };
struct Delete { std::string table; Where where; };

using Statement = std::variant<CreateTable, CreateIndex, Insert, Select, Update, Delete>;

Statement parse_sql(const std::string& sql);  // throws std::runtime_error on bad SQL
