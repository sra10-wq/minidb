#include <cstring>
#include <stdexcept>
#include "types.h"

// INT  -> 8 bytes
// TEXT -> 2-byte length + bytes
std::string serialize_row(const Schema& schema, const Row& row) {
    if (row.size() != schema.size())
        throw std::runtime_error("expected " + std::to_string(schema.size()) +
                                 " values, got " + std::to_string(row.size()));
    std::string out;
    for (std::size_t i = 0; i < schema.size(); i++) {
        if (schema[i].type == Type::INT) {
            if (!std::holds_alternative<int64_t>(row[i]))
                throw std::runtime_error("column '" + schema[i].name + "' expects INT");
            int64_t v = std::get<int64_t>(row[i]);
            out.append(reinterpret_cast<const char*>(&v), sizeof(v));
        } else {
            if (!std::holds_alternative<std::string>(row[i]))
                throw std::runtime_error("column '" + schema[i].name + "' expects TEXT");
            const std::string& s = std::get<std::string>(row[i]);
            if (s.size() > UINT16_MAX) throw std::runtime_error("TEXT value too long");
            uint16_t len = static_cast<uint16_t>(s.size());
            out.append(reinterpret_cast<const char*>(&len), sizeof(len));
            out.append(s);
        }
    }
    return out;
}

Row deserialize_row(const Schema& schema, const char* data, std::size_t len) {
    Row row;
    std::size_t pos = 0;
    for (const Column& col : schema) {
        if (col.type == Type::INT) {
            if (pos + 8 > len) throw std::runtime_error("corrupt row");
            int64_t v;
            std::memcpy(&v, data + pos, 8);
            pos += 8;
            row.emplace_back(v);
        } else {
            if (pos + 2 > len) throw std::runtime_error("corrupt row");
            uint16_t n;
            std::memcpy(&n, data + pos, 2);
            pos += 2;
            if (pos + n > len) throw std::runtime_error("corrupt row");
            row.emplace_back(std::string(data + pos, n));
            pos += n;
        }
    }
    return row;
}

std::string value_to_string(const Value& v) {
    if (std::holds_alternative<int64_t>(v)) return std::to_string(std::get<int64_t>(v));
    return std::get<std::string>(v);
}
