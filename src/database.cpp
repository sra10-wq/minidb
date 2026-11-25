#include "database.h"
#include <cstring>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {

// A column visible to a query; joins combine both tables' columns.
struct BoundCol {
    std::string table, column;
    Type type;
};

// A WHERE condition with column names already resolved to row positions.
struct BoundCond {
    std::size_t left;
    CmpOp op;
    bool rhs_is_column;
    std::size_t right;
    Value value;
};

Type type_of(const Value& v) { return std::holds_alternative<int64_t>(v) ? Type::INT : Type::TEXT; }

std::size_t resolve(const std::vector<BoundCol>& cols, const ColumnRef& ref) {
    std::optional<std::size_t> found;
    for (std::size_t i = 0; i < cols.size(); i++) {
        if (cols[i].column != ref.column) continue;
        if (!ref.table.empty() && cols[i].table != ref.table) continue;
        if (found) throw std::runtime_error("ambiguous column '" + ref.column + "' (use table.column)");
        found = i;
    }
    if (!found) {
        std::string name = ref.table.empty() ? ref.column : ref.table + "." + ref.column;
        throw std::runtime_error("unknown column '" + name + "'");
    }
    return *found;
}

int compare(const Value& a, const Value& b) {
    if (a.index() != b.index()) throw std::runtime_error("cannot compare INT with TEXT");
    if (auto* x = std::get_if<int64_t>(&a)) {
        int64_t y = std::get<int64_t>(b);
        return *x < y ? -1 : (*x > y ? 1 : 0);
    }
    int c = std::get<std::string>(a).compare(std::get<std::string>(b));
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

bool holds(int c, CmpOp op) {
    switch (op) {
        case CmpOp::EQ: return c == 0;
        case CmpOp::NE: return c != 0;
        case CmpOp::LT: return c < 0;
        case CmpOp::LE: return c <= 0;
        case CmpOp::GT: return c > 0;
        case CmpOp::GE: return c >= 0;
    }
    return false;
}

std::vector<BoundCol> columns_of(const TableInfo& t) {
    std::vector<BoundCol> cols;
    for (const Column& c : t.schema) cols.push_back({t.name, c.name, c.type});
    return cols;
}

// Resolve names and check types once, before touching any rows.
std::vector<BoundCond> bind_where(const Where& where, const std::vector<BoundCol>& cols) {
    std::vector<BoundCond> out;
    for (const Condition& c : where) {
        BoundCond b{resolve(cols, c.left), c.op, c.rhs_is_column, 0, c.rhs_value};
        if (c.rhs_is_column) {
            b.right = resolve(cols, c.rhs_column);
            if (cols[b.left].type != cols[b.right].type)
                throw std::runtime_error("type mismatch comparing '" + cols[b.left].column + "' and '" + cols[b.right].column + "'");
        } else if (type_of(c.rhs_value) != cols[b.left].type) {
            throw std::runtime_error("type mismatch in WHERE on column '" + cols[b.left].column + "'");
        }
        out.push_back(std::move(b));
    }
    return out;
}

bool matches(const std::vector<BoundCond>& conds, const Row& row) {
    for (const BoundCond& c : conds) {
        const Value& rhs = c.rhs_is_column ? row[c.right] : c.value;
        if (!holds(compare(row[c.left], rhs), c.op)) return false;
    }
    return true;
}

}  // namespace

// ---------------- setup and catalog ----------------

Database::Database(const std::string& path, std::size_t pool_size)
    : disk_(path), bp_(pool_size, disk_) {
    if (disk_.num_pages() == 0) {  // new file: reserve page 0 for the catalog
        Page* p = bp_.new_page();
        bp_.unpin_page(p->id, true);
    } else {
        load_catalog();
    }
}

Database::~Database() { bp_.flush_all(); }

// Catalog format (text in page 0), one table per line:
//   name first_page num_cols col1 i|t col2 i|t ...
void Database::load_catalog() {
    Page* p = bp_.fetch_page(0);
    std::string text(p->data, strnlen(p->data, PAGE_SIZE));
    bp_.unpin_page(0, false);
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ls(line);
        TableInfo t;
        page_id_t first;
        std::size_t n;
        ls >> t.name >> first >> n;
        for (std::size_t i = 0; i < n; i++) {
            std::string col;
            char type;
            ls >> col >> type;
            t.schema.push_back({col, type == 'i' ? Type::INT : Type::TEXT});
        }
        t.heap = std::make_unique<TableHeap>(bp_, first, t.schema);
        std::string key = t.name;
        tables_.emplace(key, std::move(t));
    }
}

void Database::save_catalog() {
    std::ostringstream out;
    for (const auto& [name, t] : tables_) {
        out << name << ' ' << t.heap->first_page() << ' ' << t.schema.size();
        for (const Column& c : t.schema) out << ' ' << c.name << ' ' << (c.type == Type::INT ? 'i' : 't');
        out << '\n';
    }
    std::string s = out.str();
    if (s.size() >= PAGE_SIZE) throw std::runtime_error("catalog full");
    Page* p = bp_.fetch_page(0);
    if (!p) throw std::runtime_error("buffer pool exhausted");
    std::memset(p->data, 0, PAGE_SIZE);
    std::memcpy(p->data, s.data(), s.size());
    bp_.unpin_page(0, true);
    bp_.flush_page(0);  // catalog changes are written immediately
}

TableInfo& Database::table(const std::string& name) {
    auto it = tables_.find(name);
    if (it == tables_.end()) throw std::runtime_error("no such table '" + name + "'");
    return it->second;
}

std::vector<std::string> Database::table_names() const {
    std::vector<std::string> names;
    for (const auto& [name, t] : tables_) names.push_back(name);
    return names;
}

QueryResult Database::execute(const std::string& sql) {
    Statement st = parse_sql(sql);
    return std::visit([this](const auto& s) { return run(s); }, st);
}

// ---------------- statements ----------------

QueryResult Database::run(const CreateTable& s) {
    if (tables_.count(s.table)) throw std::runtime_error("table '" + s.table + "' already exists");
    std::set<std::string> seen;
    for (const Column& c : s.columns)
        if (!seen.insert(c.name).second) throw std::runtime_error("duplicate column '" + c.name + "'");
    TableInfo t;
    t.name = s.table;
    t.schema = s.columns;
    t.heap = std::make_unique<TableHeap>(bp_, TableHeap::create(bp_), t.schema);
    tables_.emplace(s.table, std::move(t));
    save_catalog();
    return {{}, {}, "table '" + s.table + "' created"};
}

QueryResult Database::run(const CreateIndex&) {
    throw std::runtime_error("CREATE INDEX is not supported yet");
}

QueryResult Database::run(const Insert& s) {
    TableInfo& t = table(s.table);
    for (const Row& row : s.rows) serialize_row(t.schema, row);  // validate all rows before inserting any
    for (const Row& row : s.rows) t.heap->insert(row);
    return {{}, {}, std::to_string(s.rows.size()) + " row(s) inserted"};
}

QueryResult Database::run(const Select& s) {
    TableInfo& t = table(s.table);
    std::vector<BoundCol> cols = columns_of(t);
    TableInfo* jt = nullptr;
    if (s.join) {
        if (s.join->table == s.table) throw std::runtime_error("self-joins are not supported");
        jt = &table(s.join->table);
        for (const BoundCol& c : columns_of(*jt)) cols.push_back(c);
    }
    std::vector<BoundCond> conds = bind_where(s.where, cols);

    // which columns to output
    QueryResult res;
    std::vector<std::size_t> proj;
    if (s.star) {
        for (std::size_t i = 0; i < cols.size(); i++) {
            proj.push_back(i);
            res.columns.push_back(jt ? cols[i].table + "." + cols[i].column : cols[i].column);
        }
    } else {
        for (const ColumnRef& ref : s.columns) {
            proj.push_back(resolve(cols, ref));
            res.columns.push_back(ref.table.empty() ? ref.column : ref.table + "." + ref.column);
        }
    }

    int64_t limit = s.limit.value_or(-1);
    if (limit == 0) return res;
    // returns false once LIMIT is reached, which stops the scan early
    auto emit = [&](const Row& full) {
        if (!matches(conds, full)) return true;
        Row out;
        for (std::size_t i : proj) out.push_back(full[i]);
        res.rows.push_back(std::move(out));
        return limit < 0 || static_cast<int64_t>(res.rows.size()) < limit;
    };

    if (!jt) {
        t.heap->scan([&](const RID&, const Row& r) { return emit(r); });
    } else {
        std::size_t li = resolve(cols, s.join->left), ri = resolve(cols, s.join->right);
        if (cols[li].type != cols[ri].type) throw std::runtime_error("JOIN columns have different types");
        // nested-loop join: load the inner table once, then pair it with each outer row
        std::vector<Row> inner;
        jt->heap->scan([&](const RID&, const Row& r) { inner.push_back(r); return true; });
        t.heap->scan([&](const RID&, const Row& outer) {
            for (const Row& r : inner) {
                Row full = outer;
                full.insert(full.end(), r.begin(), r.end());
                if (compare(full[li], full[ri]) != 0) continue;
                if (!emit(full)) return false;
            }
            return true;
        });
    }
    return res;
}

QueryResult Database::run(const Update& s) {
    TableInfo& t = table(s.table);
    std::vector<BoundCol> cols = columns_of(t);
    std::vector<BoundCond> conds = bind_where(s.where, cols);
    std::vector<std::pair<std::size_t, Value>> sets;
    for (const auto& [name, value] : s.sets) {
        std::size_t i = resolve(cols, {"", name});
        if (type_of(value) != cols[i].type) throw std::runtime_error("type mismatch setting '" + name + "'");
        sets.push_back({i, value});
    }
    // collect first, then modify, so the scan never sees its own changes
    std::vector<std::pair<RID, Row>> todo;
    t.heap->scan([&](const RID& rid, const Row& r) {
        if (matches(conds, r)) todo.push_back({rid, r});
        return true;
    });
    for (auto& [rid, row] : todo) {
        for (const auto& [i, v] : sets) row[i] = v;
        t.heap->update(rid, row);
    }
    return {{}, {}, std::to_string(todo.size()) + " row(s) updated"};
}

QueryResult Database::run(const Delete& s) {
    TableInfo& t = table(s.table);
    std::vector<BoundCond> conds = bind_where(s.where, columns_of(t));
    std::vector<RID> todo;
    t.heap->scan([&](const RID& rid, const Row& r) {
        if (matches(conds, r)) todo.push_back(rid);
        return true;
    });
    for (const RID& rid : todo) t.heap->remove(rid);
    return {{}, {}, std::to_string(todo.size()) + " row(s) deleted"};
}
