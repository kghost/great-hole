#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace gh {

[[nodiscard]] auto ToWstring(std::string_view str) -> std::optional<std::wstring>;
[[nodiscard]] auto ToString(std::wstring_view wstr) -> std::optional<std::string>;

} // namespace gh
