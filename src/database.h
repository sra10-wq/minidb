#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "bplus_tree.h"
#include "buffer_pool.h"
#include "sql_ast.h"
#include "table_heap.h"

struct QueryResult {
    std::vector<std::string> columns;  // empty for statements that don't return rows
    std::vector<Row> rows;
    std::string message;
    std::string plan;                  // how a query found its rows
};

struct IndexInfo {
    std::string name;
    std::size_t column;  // position in the table schema
    std::unique_ptr<BPlusTree> tree;
};

struct TableInfo {
    std::string name;
    Schema schema;
    std::unique_ptr<TableHeap> heap;
    std::vector<IndexInfo> indexes;
};

// Owns the file, buffer pool, and catalog. Page 0 of the file stores the catalog.
class Database {
public:
    explicit Database(const std::string& path, std::size_t pool_size = 1024);
    ~Database();

    QueryResult execute(const std::string& sql);
    std::vector<std::string> table_names() const;
    BufferPool& buffer_pool() { return bp_; }

private:
    QueryResult run(const CreateTable& s);
    QueryResult run(const CreateIndex& s);
    QueryResult run(const Insert& s);
    QueryResult run(const Select& s);
    QueryResult run(const Update& s);
    QueryResult run(const Delete& s);

    TableInfo& table(const std::string& name);
    void load_catalog();
    void save_catalog();

    DiskManager disk_;  // declared first so it is destroyed last
    BufferPool bp_;
    std::map<std::string, TableInfo> tables_;
};
