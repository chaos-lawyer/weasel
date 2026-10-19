#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace weasel::external_template {
inline constexpr wchar_t kWindowClass[] = L"Weasel.ExternalTemplate.v1";
inline constexpr uint32_t kCopyDataTag = 0x57535431;
inline constexpr uint32_t kStatusMessage = 0x8000 + 0x310;
inline constexpr size_t kHeaderSize = 24;
inline constexpr size_t kMaxUnits = 32768;
enum Status : uint32_t { Rejected = 0, Pending = 1, Applied = 2, Failed = 3 };

struct Request {
  uint32_t id = 0;
  uint64_t target = 0;
  std::u16string text;
};

template <typename Char>
bool HasMarker(std::basic_string_view<Char> text) {
  for (size_t i = 0; i + 1 < text.size(); ++i) {
    if (text[i] != '$')
      continue;
    const auto c = text[i + 1];
    if (c == '|' || (c >= '1' && c <= '9'))
      return true;
    if (c == '{' && text.find(static_cast<Char>('}'), i + 2) != text.npos)
      return true;
    const char cursor[] = "cursor";
    size_t n = 0;
    while (n < 6 && i + 1 + n < text.size() && text[i + 1 + n] == cursor[n])
      ++n;
    if (n == 6)
      return true;
  }
  return false;
}

inline uint64_t ReadNumber(const uint8_t* data, size_t count) {
  uint64_t value = 0;
  for (size_t i = 0; i < count; ++i)
    value |= static_cast<uint64_t>(data[i]) << (i * 8);
  return value;
}

inline void AppendNumber(std::vector<uint8_t>& data,
                         uint64_t value,
                         size_t count) {
  for (size_t i = 0; i < count; ++i)
    data.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

inline bool ValidText(std::u16string_view text) {
  if (text.empty() || text.size() > kMaxUnits)
    return false;
  for (size_t i = 0; i < text.size(); ++i) {
    const auto c = text[i];
    if (!c || (c >= 0xdc00 && c <= 0xdfff))
      return false;
    if (c >= 0xd800 && c <= 0xdbff) {
      if (++i == text.size() || text[i] < 0xdc00 || text[i] > 0xdfff)
        return false;
    }
  }
  return HasMarker(text);
}

inline std::vector<uint8_t> Encode(const Request& request) {
  if (!request.id || !request.target || !ValidText(request.text))
    return {};
  std::vector<uint8_t> data;
  AppendNumber(data, 1, 4);
  AppendNumber(data, request.id, 4);
  AppendNumber(data, request.target, 8);
  AppendNumber(data, request.text.size(), 4);
  AppendNumber(data, 0, 4);
  for (auto c : request.text)
    AppendNumber(data, c, 2);
  return data;
}

inline bool Decode(const void* buffer, size_t size, Request& request) {
  if (!buffer || size < kHeaderSize || size > kHeaderSize + 2 * kMaxUnits)
    return false;
  const auto* data = static_cast<const uint8_t*>(buffer);
  if (ReadNumber(data, 4) != 1 || ReadNumber(data + 20, 4) != 0)
    return false;
  const auto units = ReadNumber(data + 16, 4);
  if (size != kHeaderSize + units * 2)
    return false;
  Request parsed;
  parsed.id = static_cast<uint32_t>(ReadNumber(data + 4, 4));
  parsed.target = ReadNumber(data + 8, 8);
  for (size_t i = 0; i < units; ++i)
    parsed.text.push_back(
        static_cast<char16_t>(ReadNumber(data + kHeaderSize + i * 2, 2)));
  if (!parsed.id || !parsed.target || !ValidText(parsed.text))
    return false;
  request = std::move(parsed);
  return true;
}
}  // namespace weasel::external_template
