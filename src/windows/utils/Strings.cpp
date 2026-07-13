#pragma once

#include <locale>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gh {
using std::string;

[[nodiscard]] auto ToWstring(std::string_view str) -> std::optional<std::wstring> {
  if (str.empty()) {
    return {};
  }

  std::locale loc("en_US.UTF-8");
  const auto& facet = std::use_facet<std::codecvt<wchar_t, char, std::mbstate_t>>(loc);

  std::mbstate_t state{};
  std::vector<wchar_t> dest(str.size() * facet.max_length());

  const char* from_next = nullptr;
  wchar_t* to_next = nullptr;

  auto result = facet.in(state,
                         // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                         str.data(), str.data() + str.size(), from_next,
                         // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                         dest.data(), dest.data() + dest.size(), to_next);

  if (result != std::codecvt_base::ok) {
  }

  return std::wstring(dest.data(), to_next);
}

[[nodiscard]] auto ToString(std::wstring_view wstr) -> std::optional<std::string> {
  if (wstr.empty()) {
    return {};
  }

  std::locale loc("en_US.UTF-8");
  const auto& facet = std::use_facet<std::codecvt<wchar_t, char, std::mbstate_t>>(loc);

  std::mbstate_t state{};
  std::vector<char> dest(wstr.size() * facet.max_length());

  const wchar_t* from_next = nullptr;
  char* to_next = nullptr;

  auto result = facet.out(state,
                          // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                          wstr.data(), wstr.data() + wstr.size(), from_next,
                          // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                          dest.data(), dest.data() + dest.size(), to_next);

  if (result != std::codecvt_base::ok) {
  }

  return std::string(dest.data(), to_next);
}

} // namespace gh
