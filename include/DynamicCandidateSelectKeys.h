#pragma once

#include <cstdint>
#include <cwctype>
#include <string>
#include <vector>

namespace weasel {

namespace select_keys_ibus {
constexpr uint32_t SHIFT_MASK = 1 << 0;
constexpr uint32_t LOCK_MASK = 1 << 1;
constexpr uint32_t CONTROL_MASK = 1 << 2;
constexpr uint32_t ALT_MASK = 1 << 3;
constexpr uint32_t SUPER_MASK = 1 << 26;
constexpr uint32_t HYPER_MASK = 1 << 27;
constexpr uint32_t META_MASK = 1 << 28;
constexpr uint32_t RELEASE_MASK = 1 << 30;
}  // namespace select_keys_ibus

enum class DynamicCandidateSelectAction {
  PassThrough,
  SelectCandidate,
  Swallow
};

constexpr uint64_t kRemappedNavigationGuardMs = 80;

inline bool FollowsRemappedCandidateNavigation(uint64_t event_tick,
                                               uint64_t navigation_tick,
                                               bool is_navigation_key) {
  return !is_navigation_key && navigation_tick != 0 &&
         event_tick >= navigation_tick &&
         event_tick - navigation_tick <= kRemappedNavigationGuardMs;
}

// Platform-independent UTF-8 to UTF-16 wstring decoder
inline std::wstring DynamicUtf8ToWstring(const std::string& str) {
  std::wstring dest;
  size_t i = 0;
  while (i < str.size()) {
    uint32_t cp = 0;
    unsigned char c = static_cast<unsigned char>(str[i]);
    if (c < 0x80) {
      cp = c;
      i += 1;
    } else if ((c & 0xE0) == 0xC0) {
      if (i + 1 >= str.size())
        break;
      cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(str[i + 1]) & 0x3F);
      i += 2;
    } else if ((c & 0xF0) == 0xE0) {
      if (i + 2 >= str.size())
        break;
      cp = ((c & 0x0F) << 12) |
           ((static_cast<unsigned char>(str[i + 1]) & 0x3F) << 6) |
           (static_cast<unsigned char>(str[i + 2]) & 0x3F);
      i += 3;
    } else if ((c & 0xF8) == 0xF0) {
      if (i + 3 >= str.size())
        break;
      cp = ((c & 0x07) << 18) |
           ((static_cast<unsigned char>(str[i + 1]) & 0x3F) << 12) |
           ((static_cast<unsigned char>(str[i + 2]) & 0x3F) << 6) |
           (static_cast<unsigned char>(str[i + 3]) & 0x3F);
      i += 4;
    } else {
      i += 1;
      continue;
    }

    if (cp <= 0xFFFF) {
      dest.push_back(static_cast<wchar_t>(cp));
    } else if (cp <= 0x10FFFF) {
      cp -= 0x10000;
      dest.push_back(static_cast<wchar_t>((cp >> 10) + 0xD800));
      dest.push_back(static_cast<wchar_t>((cp & 0x3FF) + 0xDC00));
    }
  }
  return dest;
}

// Splits candidate select keys into individual characters/symbols
inline std::vector<std::wstring> ParseSelectKeys(const std::string& u8_keys) {
  std::vector<std::wstring> keys;
  if (u8_keys.empty())
    return keys;
  std::wstring wstr = DynamicUtf8ToWstring(u8_keys);
  for (size_t i = 0; i < wstr.size(); ++i) {
    wchar_t ch = wstr[i];
    if (ch >= 0xD800 && ch <= 0xDBFF && i + 1 < wstr.size() &&
        wstr[i + 1] >= 0xDC00 && wstr[i + 1] <= 0xDFFF) {
      keys.push_back(wstr.substr(i, 2));
      ++i;
    } else {
      keys.push_back(std::wstring(1, ch));
    }
  }
  return keys;
}

// Parses candidate select labels.
// If whitespace is present, splits by tokens; otherwise splits into individual
// code points.
inline std::vector<std::wstring> ParseSelectLabels(const std::string& u8_str) {
  std::vector<std::wstring> labels;
  if (u8_str.empty())
    return labels;

  std::wstring wstr = DynamicUtf8ToWstring(u8_str);
  bool has_space = false;
  for (wchar_t ch : wstr) {
    if (iswspace(ch)) {
      has_space = true;
      break;
    }
  }

  if (has_space) {
    size_t start = 0;
    while (start < wstr.size()) {
      while (start < wstr.size() && iswspace(wstr[start])) {
        ++start;
      }
      if (start >= wstr.size())
        break;
      size_t end = start;
      while (end < wstr.size() && !iswspace(wstr[end])) {
        ++end;
      }
      labels.push_back(wstr.substr(start, end - start));
      start = end;
    }
  } else {
    for (size_t i = 0; i < wstr.size(); ++i) {
      wchar_t ch = wstr[i];
      if (ch >= 0xD800 && ch <= 0xDBFF && i + 1 < wstr.size() &&
          wstr[i + 1] >= 0xDC00 && wstr[i + 1] <= 0xDFFF) {
        labels.push_back(wstr.substr(i, 2));
        ++i;
      } else {
        labels.push_back(std::wstring(1, ch));
      }
    }
  }
  return labels;
}

// Generates label string for candidate i on the current page
// Priority:
// 1. Runtime candidate_select_labels (if set)
// 2. Runtime candidate_select_keys (if set)
// 3. Schema ctx.select_labels (if set)
// 4. Schema ctx.menu.select_keys (if set)
// 5. Default: (i + 1) % 10
inline std::wstring FormatCandidateLabel(
    size_t i,
    const std::vector<std::wstring>& runtime_labels,
    const std::vector<std::wstring>& runtime_keys,
    const char* const* schema_select_labels,
    const char* schema_select_keys) {
  if (!runtime_labels.empty()) {
    if (i < runtime_labels.size()) {
      return runtime_labels[i];
    }
    return L"";
  }

  if (!runtime_keys.empty()) {
    if (i < runtime_keys.size()) {
      return runtime_keys[i];
    }
    return L"";
  }

  if (schema_select_labels && schema_select_labels[i]) {
    return DynamicUtf8ToWstring(schema_select_labels[i]);
  }

  if (schema_select_keys && schema_select_keys[i]) {
    return std::wstring(1, static_cast<wchar_t>(schema_select_keys[i]));
  }

  return std::to_wstring((i + 1) % 10);
}

// Resolves whether a key event triggers dynamic candidate selection
inline DynamicCandidateSelectAction ResolveDynamicCandidateSelection(
    uint32_t keycode,
    uint32_t mask,
    const std::vector<std::wstring>& runtime_keys,
    size_t num_candidates,
    size_t& selected_index,
    bool suppress_matching_key = false) {
  if (runtime_keys.empty() || num_candidates == 0) {
    return DynamicCandidateSelectAction::PassThrough;
  }

  // Ctrl / Alt / Super / Hyper / Meta modifiers bypass selection
  constexpr uint32_t kBypassModifiers =
      select_keys_ibus::CONTROL_MASK | select_keys_ibus::ALT_MASK |
      select_keys_ibus::SUPER_MASK | select_keys_ibus::HYPER_MASK |
      select_keys_ibus::META_MASK;
  if (mask & kBypassModifiers) {
    return DynamicCandidateSelectAction::PassThrough;
  }

  // Look for match in runtime_keys
  int match_index = -1;
  for (size_t idx = 0; idx < runtime_keys.size(); ++idx) {
    if (runtime_keys[idx].size() == 1 &&
        static_cast<uint32_t>(runtime_keys[idx][0]) == keycode) {
      match_index = static_cast<int>(idx);
      break;
    }
  }

  // A remapped chord may cause the leaked letter to be reported in uppercase
  // even though the configured runtime selection key is lowercase.  Selection
  // itself remains case-sensitive; only the suppression path folds case.
  if (match_index < 0 && suppress_matching_key) {
    const auto folded_key = std::towlower(static_cast<wchar_t>(keycode));
    for (size_t idx = 0; idx < runtime_keys.size(); ++idx) {
      if (runtime_keys[idx].size() == 1 &&
          std::towlower(runtime_keys[idx][0]) == folded_key) {
        match_index = static_cast<int>(idx);
        break;
      }
    }
  }

  if (match_index < 0) {
    return DynamicCandidateSelectAction::PassThrough;
  }

  // A remapping tool may inject a navigation key, then let the chord's letter
  // reach TSF as well.  When that letter is also a runtime selection key,
  // consume it without selecting a candidate.  The caller detects the event
  // sequence because remapper-specific state is unavailable in this helper.
  if (suppress_matching_key) {
    return DynamicCandidateSelectAction::Swallow;
  }

  // If this is a key release of a selection key, swallow it so target app
  // doesn't receive it
  if (mask & select_keys_ibus::RELEASE_MASK) {
    return DynamicCandidateSelectAction::Swallow;
  }

  // If candidate index is within visible candidate count on current page
  if (static_cast<size_t>(match_index) < num_candidates) {
    selected_index = static_cast<size_t>(match_index);
    return DynamicCandidateSelectAction::SelectCandidate;
  }

  // If key matches a selection key but exceeds candidate count,
  // swallow the key to prevent it from leaking into input.
  return DynamicCandidateSelectAction::Swallow;
}

}  // namespace weasel
