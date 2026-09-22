#pragma once

#include <string>
#include <vector>

namespace weasel_llm {
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
std::vector<std::wstring> RequestCandidates(const std::wstring& base_url,
                                            const std::wstring& model,
                                            const std::wstring& api_key_env,
                                            const std::wstring& context,
                                            const InputPaths& input,
                                            int timeout_ms,
                                            int candidate_count,
                                            bool cache_enabled,
                                            int cache_ttl_seconds,
                                            int cache_max_entries);
}  // namespace weasel_llm
