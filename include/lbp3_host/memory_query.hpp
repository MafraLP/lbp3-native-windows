#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <cstddef>
namespace lbp3::memory {
struct QueryInfo {
    std::uint64_t start{},end{},offset{};std::int32_t protection{},type{};
    std::uint8_t flags{};std::array<char,32> name{};
};
static_assert(sizeof(QueryInfo)==72&&offsetof(QueryInfo,name)==33);
std::optional<QueryInfo> query_owned(std::uintptr_t,bool next);
}
