#pragma once
#include <functional>
#include <optional>
#include "buffer_pool.h"
#include "types.h"

// A table stored as a linked list of slotted pages.
class TableHeap {
public:
    static page_id_t create(BufferPool& bp);  // allocate an empty first page
    TableHeap(BufferPool& bp, page_id_t first_page, Schema schema);

    RID insert(const Row& row);
    std::optional<Row> get(const RID& rid);
    bool remove(const RID& rid);
    RID update(const RID& rid, const Row& row);  // may move the row; returns its RID
    // Visit every live row in order; return false from fn to stop early.
    void scan(const std::function<bool(const RID&, const Row&)>& fn);

    const Schema& schema() const { return schema_; }
    page_id_t first_page() const { return first_page_; }

private:
    Page* fetch(page_id_t id);
    BufferPool& bp_;
    page_id_t first_page_;
    page_id_t last_page_;
    Schema schema_;
};
