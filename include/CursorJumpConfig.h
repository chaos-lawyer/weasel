#pragma once

#include <optional>
#include <string>

namespace weasel {
struct CursorJumpConfig {
  bool enabled = false;
  std::wstring punct_keys;
  bool punct_any_field = false;
};

// Absent values inherit; an explicit false or empty whitelist overrides.
struct CursorJumpOverrides {
  std::optional<bool> enabled;
  std::optional<std::wstring> punct_keys;
  std::optional<bool> punct_any_field;

  void ApplyTo(CursorJumpConfig& config) const {
    if (enabled.has_value())
      config.enabled = *enabled;
    if (punct_keys.has_value())
      config.punct_keys = *punct_keys;
    if (punct_any_field.has_value())
      config.punct_any_field = *punct_any_field;
  }
};

inline std::wstring FilterCursorPunctKeys(const std::wstring& keys) {
  std::wstring filtered;
  for (wchar_t c : keys) {
    if ((c >= L'!' && c <= L'/') || (c >= L':' && c <= L'@') ||
        (c >= L'[' && c <= L'`') || (c >= L'{' && c <= L'~'))
      filtered.push_back(c);
  }
  return filtered;
}
}  // namespace weasel
