#pragma once

#include <string>
#include <vector>
#include <functional>
#include <logging.h>
#include <WeaselIPCData.h>

namespace weasel {

enum class CandidateLayout {
  Horizontal,
  Vertical
};

enum class LayoutRuleType {
  Invalid,
  Option,
  CandidateMaxTextLengthGt,
  CandidateCountGt
};

struct DynamicLayoutRule {
  LayoutRuleType type = LayoutRuleType::Invalid;
  std::string option_name;
  bool option_value = true;
  int threshold = 0;
  CandidateLayout target_layout = CandidateLayout::Horizontal;
};

struct DynamicLayoutConfig {
  bool enabled = false;
  CandidateLayout default_layout = CandidateLayout::Horizontal;
  std::vector<DynamicLayoutRule> rules;
};

// Calculate Unicode code point count (surrogate pairs in UTF-16 counted as 1).
inline size_t CalculateUnicodeLength(const std::wstring& text) {
  size_t count = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    wchar_t ch = text[i];
    if (ch >= 0xD800 && ch <= 0xDBFF) {
      // High surrogate, check if paired with low surrogate
      if (i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
        ++i;
      }
    }
    ++count;
  }
  return count;
}

// Extension point: can be enhanced in the future for actual pixel width measurement.
inline size_t MeasureCandidateTextMetric(const std::wstring& text) {
  return CalculateUnicodeLength(text);
}

inline size_t GetMaxCandidateUnicodeLength(const CandidateInfo& cinfo) {
  size_t max_len = 0;
  for (const auto& cand : cinfo.candies) {
    size_t len = MeasureCandidateTextMetric(cand.str);
    if (len > max_len) {
      max_len = len;
    }
  }
  return max_len;
}

// Resolves candidate layout based on context, rules, and option values.
// Follows "first match wins" priority. Fallback to default_layout if no rule matches.
inline CandidateLayout ResolveCandidateLayout(
    const CandidateInfo& cinfo,
    CandidateLayout default_fallback,
    const DynamicLayoutConfig& config,
    const std::function<bool(const std::string&)>& option_getter) {
  if (!config.enabled) {
    return default_fallback;
  }

  for (const auto& rule : config.rules) {
    switch (rule.type) {
      case LayoutRuleType::Option: {
        if (!rule.option_name.empty() && option_getter) {
          bool actual_val = option_getter(rule.option_name);
          if (actual_val == rule.option_value) {
            DLOG(INFO) << "DynamicLayout: matched option " << rule.option_name
                       << "=" << (actual_val ? "true" : "false")
                       << ", resolved=" << (rule.target_layout == CandidateLayout::Vertical ? "vertical" : "horizontal");
            return rule.target_layout;
          }
        }
        break;
      }
      case LayoutRuleType::CandidateMaxTextLengthGt: {
        if (rule.threshold >= 0) {
          size_t max_len = GetMaxCandidateUnicodeLength(cinfo);
          if (max_len > static_cast<size_t>(rule.threshold)) {
            DLOG(INFO) << "DynamicLayout: matched candidate_max_text_length_gt "
                       << rule.threshold << " (actual " << max_len
                       << "), resolved=" << (rule.target_layout == CandidateLayout::Vertical ? "vertical" : "horizontal");
            return rule.target_layout;
          }
        }
        break;
      }
      case LayoutRuleType::CandidateCountGt: {
        if (rule.threshold >= 0) {
          size_t count = cinfo.candies.size();
          if (count > static_cast<size_t>(rule.threshold)) {
            DLOG(INFO) << "DynamicLayout: matched candidate_count_gt "
                       << rule.threshold << " (actual " << count
                       << "), resolved=" << (rule.target_layout == CandidateLayout::Vertical ? "vertical" : "horizontal");
            return rule.target_layout;
          }
        }
        break;
      }
      case LayoutRuleType::Invalid:
      default:
        // Ignore invalid rules safely without crashing
        break;
    }
  }

  DLOG(INFO) << "DynamicLayout: no rule matched, resolved to default="
             << (config.default_layout == CandidateLayout::Vertical ? "vertical" : "horizontal");
  return config.default_layout;
}

}  // namespace weasel
