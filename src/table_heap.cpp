#include "table_heap.h"
#include <stdexcept>
#include "slotted_page.h"

Page* TableHeap::fetch(page_id_t id) {
    Page* p = bp_.fetch_page(id);
    if (!p) throw std::runtime_error("buffer pool exhausted (all frames pinned)");
    return p;
}

page_id_t TableHeap::create(BufferPool& bp) {
    Page* p = bp.new_page();
    if (!p) throw std::runtime_error("buffer pool exhausted");
    SlottedPage(p->data).init();
    page_id_t id = p->id;
    bp.unpin_page(id, true);
    return id;
}

TableHeap::TableHeap(BufferPool& bp, page_id_t first_page, Schema schema)
    : bp_(bp), first_page_(first_page), last_page_(first_page), schema_(std::move(schema)) {
    // walk the chain once to find the last page (where inserts go)
    while (true) {
        Page* p = fetch(last_page_);
        page_id_t next = SlottedPage(p->data).next();
        bp_.unpin_page(last_page_, false);
        if (next == INVALID_PAGE_ID) break;
        last_page_ = next;
    }
}

RID TableHeap::insert(const Row& row) {
    std::string rec = serialize_row(schema_, row);
    if (rec.size() > SlottedPage::MAX_RECORD) throw std::runtime_error("row too large for a page");
    auto len = static_cast<uint16_t>(rec.size());

    Page* last = fetch(last_page_);
    if (auto slot = SlottedPage(last->data).insert(rec.data(), len)) {
        bp_.unpin_page(last_page_, true);
        return {last_page_, *slot};
    }
    // last page is full: allocate a new one and link it in
    Page* fresh = bp_.new_page();
    if (!fresh) { bp_.unpin_page(last_page_, false); throw std::runtime_error("buffer pool exhausted"); }
    SlottedPage sp(fresh->data);
    sp.init();
    SlottedPage(last->data).set_next(fresh->id);
    bp_.unpin_page(last_page_, true);
    last_page_ = fresh->id;
    uint16_t slot = *sp.insert(rec.data(), len);
    bp_.unpin_page(last_page_, true);
    return {last_page_, slot};
}

std::optional<Row> TableHeap::get(const RID& rid) {
    Page* p = fetch(rid.page_id);
    const char* rec;
    uint16_t len;
    std::optional<Row> row;
    if (SlottedPage(p->data).get(rid.slot, rec, len)) row = deserialize_row(schema_, rec, len);
    bp_.unpin_page(rid.page_id, false);
    return row;
}

bool TableHeap::remove(const RID& rid) {
    Page* p = fetch(rid.page_id);
    bool ok = SlottedPage(p->data).remove(rid.slot);
    bp_.unpin_page(rid.page_id, ok);
    return ok;
}

RID TableHeap::update(const RID& rid, const Row& row) {
    std::string rec = serialize_row(schema_, row);
    Page* p = fetch(rid.page_id);
    bool in_place = rec.size() <= SlottedPage::MAX_RECORD &&
        SlottedPage(p->data).update_in_place(rid.slot, rec.data(), static_cast<uint16_t>(rec.size()));
    bp_.unpin_page(rid.page_id, in_place);
    if (in_place) return rid;
    if (!remove(rid)) throw std::runtime_error("update: row not found");
    return insert(row);  // grew too large for its spot: move it
}

void TableHeap::scan(const std::function<bool(const RID&, const Row&)>& fn) {
    page_id_t pid = first_page_;
    while (pid != INVALID_PAGE_ID) {
        Page* p = fetch(pid);
        SlottedPage sp(p->data);
        for (uint16_t s = 0; s < sp.num_slots(); s++) {
            const char* rec;
            uint16_t len;
            if (!sp.get(s, rec, len)) continue;
            if (!fn({pid, s}, deserialize_row(schema_, rec, len))) {
                bp_.unpin_page(pid, false);
                return;
            }
        }
        page_id_t next = sp.next();
        bp_.unpin_page(pid, false);
        pid = next;
    }
}
