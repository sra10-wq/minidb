#pragma once
#include <cstddef>
#include <cstdint>

constexpr std::size_t PAGE_SIZE = 4096;   // bytes per page, matches the OS page size
using page_id_t = int32_t;
constexpr page_id_t INVALID_PAGE_ID = -1;
