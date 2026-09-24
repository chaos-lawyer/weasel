#pragma once

#include <string>
#include <vector>

namespace weasel_llm {
struct CandidateItem {
  std::wstring text;
  std::wstring comment;
};

struct LlmResponse {
  bool success = false;
  std::wstring error_message;
  std::vector<CandidateItem> candidates;
};

struct InputPaths {
  std::string schema_id;
  std::string raw_input;
  std::string phonetic;
  std::string initials;
  bool has_phonetic = false;
};

InputPaths ParseInput(const std::string& raw_input,
                      const std::string& schema_id,
                      const std::string& configured_scheme);
LlmResponse RequestCandidates(const std::wstring& base_url,
                              const std::wstring& model,
                              const std::wstring& api_key,
                              const std::wstring& prompt_both,
                              const std::wstring& prompt_initials_only,
                              const std::wstring& default_predict_comment,
                              const std::wstring& default_continuation_comment,
                              int continuation_count,
                              const std::wstring& context,
                              const InputPaths& input,
                              int timeout_ms,
                              double temperature,
                              int candidate_count,
                              bool cache_enabled,
                              int cache_ttl_seconds,
                              int cache_max_entries,
                              const std::wstring& debug_log_path,
                              const std::string& debug_context_mode,
                              int debug_context_preview_chars);
}  // namespace weasel_llm
