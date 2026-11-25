#pragma once
#include <cstdint>
#include <cstring>
#include <optional>
#include "config.h"

// Page layout:
// [next_page_id 4B | num_slots 2B | free_end 2B | slot0 | slot1 | ...  free space  ... | rec1 | rec0]
// Each slot is {offset 2B, length 2B}. Slots grow forward, records grow backward from the end.
// length == 0 marks a deleted record (tombstone), so other rows' slot numbers never change.
class SlottedPage {
public:
    static constexpr std::size_t HEADER = 8;
    static constexpr std::size_t SLOT = 4;
    static constexpr std::size_t MAX_RECORD = PAGE_SIZE - HEADER - SLOT;

    explicit SlottedPage(char* data) : d_(data) {}

    void init() {
        set_next(INVALID_PAGE_ID);
        set_u16(4, 0);                                  // num_slots
        set_u16(6, static_cast<uint16_t>(PAGE_SIZE));   // free_end
    }

    page_id_t next() const { page_id_t v; std::memcpy(&v, d_, 4); return v; }
    void set_next(page_id_t id) { std::memcpy(d_, &id, 4); }
    uint16_t num_slots() const { return u16(4); }

    std::optional<uint16_t> insert(const char* rec, uint16_t len) {
        uint16_t n = num_slots(), free_end = u16(6);
        std::size_t slots_end = HEADER + (n + 1) * SLOT;  // after adding one slot
        if (len == 0 || slots_end + len > free_end) return std::nullopt;  // doesn't fit
        uint16_t offset = static_cast<uint16_t>(free_end - len);
        std::memcpy(d_ + offset, rec, len);
        set_u16(HEADER + n * SLOT, offset);
        set_u16(HEADER + n * SLOT + 2, len);
        set_u16(4, n + 1);
        set_u16(6, offset);
        return n;
    }

    bool get(uint16_t slot, const char*& rec, uint16_t& len) const {
        if (slot >= num_slots()) return false;
        len = u16(HEADER + slot * SLOT + 2);
        if (len == 0) return false;  // deleted
        rec = d_ + u16(HEADER + slot * SLOT);
        return true;
    }

    bool remove(uint16_t slot) {
        if (slot >= num_slots() || u16(HEADER + slot * SLOT + 2) == 0) return false;
        set_u16(HEADER + slot * SLOT + 2, 0);  // tombstone
        return true;
    }

    // Overwrite in place if the new record is no larger than the old one.
    bool update_in_place(uint16_t slot, const char* rec, uint16_t len) {
        if (slot >= num_slots()) return false;
        uint16_t old_len = u16(HEADER + slot * SLOT + 2);
        if (old_len == 0 || len == 0 || len > old_len) return false;
        std::memcpy(d_ + u16(HEADER + slot * SLOT), rec, len);
        set_u16(HEADER + slot * SLOT + 2, len);
        return true;
    }

private:
    uint16_t u16(std::size_t off) const { uint16_t v; std::memcpy(&v, d_ + off, 2); return v; }
    void set_u16(std::size_t off, uint16_t v) { std::memcpy(d_ + off, &v, 2); }
    char* d_;
};
