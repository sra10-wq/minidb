#include "database.h"
#include <climits>
#include <cstring>
#include <functional>
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

// Inclusive key range for an index scan.
struct KeyRange {
    int64_t lo = INT64_MIN, hi = INT64_MAX;
    bool equality = false;
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

// Planner: find an index whose column the WHERE clause constrains, and turn those
// conditions into one key range. Prefers an equality match.
std::optional<std::pair<const IndexInfo*, KeyRange>> choose_index(const TableInfo& t,
                                                                   const std::vector<BoundCond>& conds) {
    std::optional<std::pair<const IndexInfo*, KeyRange>> best;
    for (const IndexInfo& ix : t.indexes) {
        KeyRange r;
        bool used = false;
        for (const BoundCond& c : conds) {
            if (c.rhs_is_column || c.left != ix.column) continue;
            int64_t v = std::get<int64_t>(c.value);
            switch (c.op) {
                case CmpOp::EQ: r.lo = std::max(r.lo, v); r.hi = std::min(r.hi, v); r.equality = true; break;
                case CmpOp::GE: r.lo = std::max(r.lo, v); break;
                case CmpOp::LE: r.hi = std::min(r.hi, v); break;
                case CmpOp::GT: if (v == INT64_MAX) r.lo = 1, r.hi = 0; else r.lo = std::max(r.lo, v + 1); break;
                case CmpOp::LT: if (v == INT64_MIN) r.lo = 1, r.hi = 0; else r.hi = std::min(r.hi, v - 1); break;
                case CmpOp::NE: continue;  // can't narrow a range
            }
            used = true;
        }
        if (!used) continue;
        if (!best || (r.equality && !best->second.equality)) best = std::make_pair(&ix, r);
    }
    return best;
}

// Visit every row of t matching conds (using an index if possible). Returns the plan used.
// fn returns false to stop early.
std::string for_each_match(TableInfo& t, const std::vector<BoundCond>& conds,
                           const std::function<bool(const RID&, const Row&)>& fn) {
    if (auto choice = choose_index(t, conds)) {
        const auto& [ix, range] = *choice;
        std::vector<RID> rids;
        ix->tree->scan_range(range.lo, range.hi, [&](int64_t, const RID& rid) {
            rids.push_back(rid);
            return true;
        });
        for (const RID& rid : rids) {
            auto row = t.heap->get(rid);
            if (row && matches(conds, *row) && !fn(rid, *row)) break;  // recheck all conditions
        }
        return "index scan using " + ix->name;
    }
    t.heap->scan([&](const RID& rid, const Row& r) { return !matches(conds, r) || fn(rid, r); });
    return "full table scan";
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

// Catalog format (text in page 0), one entry per line:
//   table:  name first_page num_cols col1 i|t col2 i|t ...
//   index:  @ index_name table_name column_name header_page
void Database::load_catalog() {
    Page* p = bp_.fetch_page(0);
    std::string text(p->data, strnlen(p->data, PAGE_SIZE));
    bp_.unpin_page(0, false);
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ls(line);
        if (line[0] == '@') {
            std::string at, name, table_name, column;
            page_id_t header;
            ls >> at >> name >> table_name >> column >> header;
            TableInfo& t = table(table_name);
            std::size_t col = resolve(columns_of(t), {"", column});
            t.indexes.push_back({name, col, std::make_unique<BPlusTree>(bp_, header)});
            continue;
        }
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
    for (const auto& [name, t] : tables_) {  // tables first...
        out << name << ' ' << t.heap->first_page() << ' ' << t.schema.size();
        for (const Column& c : t.schema) out << ' ' << c.name << ' ' << (c.type == Type::INT ? 'i' : 't');
        out << '\n';
    }
    for (const auto& [name, t] : tables_)  // ...then indexes, which refer to tables
        for (const IndexInfo& ix : t.indexes)
            out << "@ " << ix.name << ' ' << name << ' ' << t.schema[ix.column].name << ' '
                << ix.tree->header_page() << '\n';
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
    return {{}, {}, "table '" + s.table + "' created", ""};
}

QueryResult Database::run(const CreateIndex& s) {
    TableInfo& t = table(s.table);
    for (const auto& [name, other] : tables_)
        for (const IndexInfo& ix : other.indexes)
            if (ix.name == s.name) throw std::runtime_error("index '" + s.name + "' already exists");
    std::size_t col = resolve(columns_of(t), {"", s.column});
    if (t.schema[col].type != Type::INT) throw std::runtime_error("only INT columns can be indexed");
    for (const IndexInfo& ix : t.indexes)
        if (ix.column == col) throw std::runtime_error("column '" + s.column + "' is already indexed");

    IndexInfo ix{s.name, col, std::make_unique<BPlusTree>(bp_, BPlusTree::create(bp_))};
    std::size_t n = 0;
    t.heap->scan([&](const RID& rid, const Row& r) {  // index every existing row
        ix.tree->insert(std::get<int64_t>(r[col]), rid);
        n++;
        return true;
    });
    t.indexes.push_back(std::move(ix));
    save_catalog();
    return {{}, {}, "index '" + s.name + "' created (" + std::to_string(n) + " rows)", ""};
}

QueryResult Database::run(const Insert& s) {
    TableInfo& t = table(s.table);
    for (const Row& row : s.rows) serialize_row(t.schema, row);  // validate all rows before inserting any
    for (const Row& row : s.rows) {
        RID rid = t.heap->insert(row);
        for (IndexInfo& ix : t.indexes) ix.tree->insert(std::get<int64_t>(row[ix.column]), rid);
    }
    return {{}, {}, std::to_string(s.rows.size()) + " row(s) inserted", ""};
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
        Row out;
        for (std::size_t i : proj) out.push_back(full[i]);
        res.rows.push_back(std::move(out));
        return limit < 0 || static_cast<int64_t>(res.rows.size()) < limit;
    };

    if (!jt) {
        res.plan = for_each_match(t, conds, [&](const RID&, const Row& r) { return emit(r); });
        return res;
    }

    std::size_t li = resolve(cols, s.join->left), ri = resolve(cols, s.join->right);
    if (cols[li].type != cols[ri].type) throw std::runtime_error("JOIN columns have different types");
    const std::size_t nt = t.schema.size();  // positions [0, nt) come from t, the rest from jt
    if ((li < nt) == (ri < nt))
        throw std::runtime_error("JOIN condition must compare one column from each table");
    const std::size_t outer_col = li < nt ? li : ri;         // join column in t
    const std::size_t inner_col = (li < nt ? ri : li) - nt;  // join column in jt

    auto find_index = [](const TableInfo& tbl, std::size_t col) -> const IndexInfo* {
        for (const IndexInfo& ix : tbl.indexes)
            if (ix.column == col) return &ix;
        return nullptr;
    };
    // combine one row from each side; emit it if it passes WHERE. false = LIMIT reached
    auto output = [&](const Row& outer, const Row& inner) {
        Row full = outer;
        full.insert(full.end(), inner.begin(), inner.end());
        return !matches(conds, full) || emit(full);
    };

    if (const IndexInfo* inner_ix = find_index(*jt, inner_col)) {
        // index nested loop join: for each t row, probe jt's index for matching rows
        t.heap->scan([&](const RID&, const Row& outer) {
            for (const RID& rid : inner_ix->tree->lookup(std::get<int64_t>(outer[outer_col]))) {
                auto inner = jt->heap->get(rid);
                if (inner && !output(outer, *inner)) return false;
            }
            return true;
        });
        res.plan = "index nested loop join using " + inner_ix->name;
    } else if (const IndexInfo* outer_ix = find_index(t, outer_col)) {
        // same idea with roles flipped: for each jt row, probe t's index
        jt->heap->scan([&](const RID&, const Row& inner) {
            for (const RID& rid : outer_ix->tree->lookup(std::get<int64_t>(inner[inner_col]))) {
                auto outer = t.heap->get(rid);
                if (outer && !output(*outer, inner)) return false;
            }
            return true;
        });
        res.plan = "index nested loop join using " + outer_ix->name;
    } else {
        // nested loop join: load jt once, compare join keys for every pair
        std::vector<Row> inner_rows;
        jt->heap->scan([&](const RID&, const Row& r) { inner_rows.push_back(r); return true; });
        t.heap->scan([&](const RID&, const Row& outer) {
            for (const Row& inner : inner_rows) {
                if (compare(outer[outer_col], inner[inner_col]) != 0) continue;  // check key before copying
                if (!output(outer, inner)) return false;
            }
            return true;
        });
        res.plan = "nested loop join";
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
    std::string plan = for_each_match(t, conds, [&](const RID& rid, const Row& r) {
        todo.push_back({rid, r});
        return true;
    });
    for (const auto& [rid, old_row] : todo) {
        Row row = old_row;
        for (const auto& [i, v] : sets) row[i] = v;
        RID new_rid = t.heap->update(rid, row);
        for (IndexInfo& ix : t.indexes) {  // keep indexes in sync (key or location may change)
            int64_t old_key = std::get<int64_t>(old_row[ix.column]);
            int64_t new_key = std::get<int64_t>(row[ix.column]);
            if (old_key != new_key || !(new_rid == rid)) {
                ix.tree->remove(old_key, rid);
                ix.tree->insert(new_key, new_rid);
            }
        }
    }
    return {{}, {}, std::to_string(todo.size()) + " row(s) updated", plan};
}

QueryResult Database::run(const Delete& s) {
    TableInfo& t = table(s.table);
    std::vector<BoundCond> conds = bind_where(s.where, columns_of(t));
    std::vector<std::pair<RID, Row>> todo;
    std::string plan = for_each_match(t, conds, [&](const RID& rid, const Row& r) {
        todo.push_back({rid, r});
        return true;
    });
    for (const auto& [rid, row] : todo) {
        t.heap->remove(rid);
        for (IndexInfo& ix : t.indexes) ix.tree->remove(std::get<int64_t>(row[ix.column]), rid);
    }
    return {{}, {}, std::to_string(todo.size()) + " row(s) deleted", plan};
}
