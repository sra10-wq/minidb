#pragma once
#include <cstdint>
#include <string>
#include <variant>
#include <vector>
#include "config.h"

enum class Type : uint8_t { INT = 0, TEXT = 1 };
struct Column {
    std::string name;
    Type type;
};
using Schema = std::vector<Column>;
using Value = std::variant<int64_t, std::string>;
using Row = std::vector<Value>;

// Record ID: where a row lives (page + slot). Stable for the row's lifetime.
struct RID {
    page_id_t page_id = INVALID_PAGE_ID;
    uint16_t slot = 0;
    bool operator==(const RID&) const = default;
};

std::string serialize_row(const Schema& schema, const Row& row);
Row deserialize_row(const Schema& schema, const char* data, std::size_t len);
std::string value_to_string(const Value& v);
