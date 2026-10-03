#pragma once
// Portable cloud core. No Rime/UI calls are permitted on its worker thread.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace cloud {
using Clock = std::chrono::steady_clock;
struct CloudCandidate {
  std::string text, provider;
  double score = 0;
};
struct CloudCandidateResult {
  bool success = false;
  std::vector<CloudCandidate> candidates;
  std::string error;
};
struct CloudQueryContext {
  uint64_t session = 0;
  std::string query_id, composition_revision, input_snapshot;
  int timeout_ms = 250;
  Clock::time_point deadline;
  bool operator==(const CloudQueryContext& other) const {
    return session == other.session && query_id == other.query_id &&
           composition_revision == other.composition_revision &&
           input_snapshot == other.input_snapshot;
  }
};
struct CloudSettings {
  bool enabled = false, sogou_enabled = true, cache_enabled = true;
  bool privacy_enabled = true, disable_in_password_field = true;
  std::string provider = "sogou";
  int timeout_ms = 250, ttl_seconds = 3600, max_entries = 1000;
};
class CloudPrivacyPolicy {
 public:
  static bool Allows(const CloudSettings& settings,
                     const std::string& input,
                     bool password,
                     bool special_mode) {
    if (!settings.enabled || special_mode || input.empty())
      return false;
    if (settings.privacy_enabled && settings.disable_in_password_field &&
        password)
      return false;
    // Raw special-mode prefixes are blocked even before their segment tags
    // exist.
    const auto code = input[0] == '\\' ? input.substr(1) : input;
    return code.find_first_of("NRDVLAZHI\\;") == std::string::npos;
  }
};
inline std::string BuildSogouRequest(const std::string& pinyin) {
  if (pinyin.empty() || pinyin.size() > 245 ||
      pinyin.find_first_not_of("abcdefghijklmnopqrstuvwxyz") !=
          std::string::npos)
    return {};
  std::string data(1, static_cast<char>(pinyin.size() + 10));
  data.append("\0\5\0\0\0\0\1", 7);
  data.push_back(static_cast<char>(pinyin.size()));
  data += pinyin;
  unsigned char checksum = 0;
  for (unsigned char byte : data)
    checksum ^= byte;
  data.push_back(static_cast<char>(checksum));
  return data;
}
inline std::optional<std::string> Utf16LeToUtf8(const std::string& data) {
  if (data.empty() || data.size() % 2)
    return std::nullopt;
  std::string text;
  auto unit = [&](size_t i) {
    return static_cast<unsigned char>(data[i]) |
           (static_cast<unsigned char>(data[i + 1]) << 8);
  };
  for (size_t i = 0; i < data.size(); i += 2) {
    uint32_t cp = unit(i);
    if (cp >= 0xd800 && cp <= 0xdbff) {
      if (i + 3 >= data.size())
        return std::nullopt;
      uint32_t low = unit(i + 2);
      if (low < 0xdc00 || low > 0xdfff)
        return std::nullopt;
      cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
      i += 2;
    } else if (cp >= 0xdc00 && cp <= 0xdfff)
      return std::nullopt;
    if (cp < 0x20 || (cp >= 0x7f && cp <= 0x9f))
      return std::nullopt;
    if (cp < 0x80)
      text.push_back(static_cast<char>(cp));
    else {
      if (cp >= 0x10000)
        text.push_back(static_cast<char>(0xf0 | (cp >> 18)));
      if (cp >= 0x800)
        text.push_back(
            static_cast<char>((cp >= 0x10000 ? 0x80 : 0xe0) |
                              ((cp >> 12) & (cp >= 0x10000 ? 0x3f : 0x0f))));
      text.push_back(
          static_cast<char>((cp >= 0x800 ? 0x80 : 0xc0) |
                            ((cp >> 6) & (cp >= 0x800 ? 0x3f : 0x1f))));
      text.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
  }
  return text;
}
inline CloudCandidateResult ParseSogouResponse(const std::string& packet,
                                               const std::string& pinyin) {
  auto failure = [] {
    return CloudCandidateResult{false, {}, "invalid_binary"};
  };
  if (packet.size() < 20 || packet.size() > 65536)
    return failure();
  auto u16 = [&](size_t i) {
    return static_cast<unsigned char>(packet[i]) |
           (static_cast<unsigned char>(packet[i + 1]) << 8);
  };
  // Legacy length is a single byte. Reject truncation; records supply further
  // bounds.
  if (static_cast<unsigned char>(packet[0]) + 2u > packet.size())
    return failure();
  int count = u16(18);
  if (count > 32)
    return failure();
  size_t pos = 20;
  auto field = [&]() -> std::optional<std::string> {
    if (pos + 2 > packet.size())
      return std::nullopt;
    size_t length = u16(pos);
    pos += 2;
    if (length > packet.size() - pos)
      return std::nullopt;
    std::string data = packet.substr(pos, length);
    pos += length;
    return data;
  };
  CloudCandidateResult result{true, {}, {}};
  for (int i = 0; i < count; ++i) {
    auto encoded = field(), metadata = field(), boundaries = field();
    if (!encoded || !metadata || !boundaries || pos >= packet.size())
      return failure();
    // Unknown field observed in legacy Sogou mobile protocol.
    // Preserve parsing behavior from librime-cloud (metadata and trailing
    // flag).
    ++pos;
    auto text = Utf16LeToUtf8(*encoded);
    if (!text)
      return failure();
    bool complete = !boundaries->empty() && boundaries->size() % 2 == 0;
    size_t previous = 0;
    for (size_t j = 0; complete && j < boundaries->size(); j += 2) {
      size_t end = static_cast<unsigned char>((*boundaries)[j]) |
                   (static_cast<unsigned char>((*boundaries)[j + 1]) << 8);
      if (end <= previous || end > pinyin.size())
        complete = false;
      previous = end;
    }
    if (complete && previous == pinyin.size() && result.candidates.size() < 5 &&
        std::none_of(result.candidates.begin(), result.candidates.end(),
                     [&](const CloudCandidate& c) { return c.text == *text; }))
      result.candidates.push_back({*text, "sogou", 1.0 / (i + 1)});
  }
  return result;
}
struct HttpResponse {
  int status = 0;
  std::string body, error;
};
using HttpTransport =
    std::function<HttpResponse(const std::string&, const CloudQueryContext&)>;
HttpResponse QuerySogou(const std::string& payload,
                        const CloudQueryContext& context);
class ICloudCandidateProvider {
 public:
  virtual ~ICloudCandidateProvider() = default;
  virtual std::string Name() const = 0;
  // Synchronous provider contract, invoked exclusively by manager worker.
  virtual CloudCandidateResult Query(const std::string&,
                                     const CloudQueryContext&) = 0;
};
class SogouProvider : public ICloudCandidateProvider {
 public:
  explicit SogouProvider(HttpTransport transport)
      : transport_(std::move(transport)) {}
  std::string Name() const override { return "sogou"; }
  CloudCandidateResult Query(const std::string& pinyin,
                             const CloudQueryContext& context) override {
    auto payload = BuildSogouRequest(pinyin);
    if (payload.empty())
      return {false, {}, "invalid_pinyin"};
    auto response = transport_(payload, context);
    if (!response.error.empty())
      return {false, {}, response.error};
    if (response.status != 200)
      return {false, {}, "http_" + std::to_string(response.status)};
    return ParseSogouResponse(response.body, pinyin);
  }

 private:
  HttpTransport transport_;
};
// Future providers implement the same contract and register with the manager.
class GoogleInputToolsProvider : public ICloudCandidateProvider {
 public:
  std::string Name() const override { return "google"; }
  // Query remains abstract until a concrete Google transport is implemented.
};
class LlmCandidateProvider : public ICloudCandidateProvider {
 public:
  std::string Name() const override { return "llm"; }
  // Query remains abstract; no LLM API is called in this phase.
};
class CloudCandidateCache {
 public:
  std::optional<CloudCandidateResult> Get(const std::string& key,
                                          Clock::time_point now) {
    auto it = entries_.find(key);
    if (it == entries_.end())
      return std::nullopt;
    if (now >= it->second.expires) {
      order_.erase(it->second.order);
      entries_.erase(it);
      return std::nullopt;
    }
    order_.splice(order_.end(), order_, it->second.order);
    return it->second.result;
  }
  void Put(const std::string& key,
           const CloudCandidateResult& result,
           Clock::time_point now,
           int ttl,
           int capacity) {
    if (!result.success || result.candidates.empty() || ttl <= 0 ||
        capacity <= 0)
      return;
    auto old = entries_.find(key);
    if (old != entries_.end()) {
      order_.erase(old->second.order);
      entries_.erase(old);
    }
    while (entries_.size() >= static_cast<size_t>(capacity)) {
      entries_.erase(order_.front());
      order_.pop_front();
    }
    order_.push_back(key);
    entries_[key] = {result, now + std::chrono::seconds(ttl),
                     std::prev(order_.end())};
  }

 private:
  struct Entry {
    CloudCandidateResult result;
    Clock::time_point expires;
    std::list<std::string>::iterator order;
  };
  std::list<std::string> order_;
  std::map<std::string, Entry> entries_;
};
class CloudCandidateManager {
 public:
  using Logger = std::function<void(const std::string&)>;
  explicit CloudCandidateManager(Logger logger = {})
      : logger_(std::move(logger)), worker_([this] { Run(); }) {}
  ~CloudCandidateManager() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
      jobs_.clear();
    }
    cv_.notify_one();
    worker_.join();
  }
  void Register(std::shared_ptr<ICloudCandidateProvider> provider) {
    std::lock_guard<std::mutex> lock(mutex_);
    providers_[provider->Name()] = std::move(provider);
  }
  // Caller is the owner thread. Bounded queue, at most one pending job per
  // session.
  void Submit(std::string pinyin,
              CloudQueryContext context,
              CloudSettings settings) {
    std::lock_guard<std::mutex> lock(mutex_);
    context.timeout_ms = std::clamp(settings.timeout_ms, 1, 5000);
    context.deadline =
        Clock::now() + std::chrono::milliseconds(context.timeout_ms);
    active_[context.session] = context;
    ready_.erase(context.session);
    jobs_.erase(context.session);
    auto it = providers_.find(settings.provider);
    if (it == providers_.end() || !settings.enabled ||
        (settings.provider == "sogou" && !settings.sogou_enabled)) {
      ready_[context.session] = {context, {false, {}, "provider_disabled"}};
      return;
    }
    auto key = settings.provider + ":" + pinyin;
    if (settings.cache_enabled) {
      auto cached = cache_.Get(key, Clock::now());
      if (cached) {
        Log("cache hit");
        ready_[context.session] = {context, *cached};
        return;
      }
    }
    if (jobs_.size() >= 64) {
      ready_[context.session] = {context, {false, {}, "busy"}};
      return;
    }
    jobs_[context.session] = {std::move(pinyin), context, settings, it->second};
    cv_.notify_one();
  }
  std::optional<CloudCandidateResult> Take(const CloudQueryContext& context) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = ready_.find(context.session);
    if (it == ready_.end())
      return std::nullopt;
    auto item = std::move(it->second);
    ready_.erase(it);
    if (!(item.context == context)) {
      Log("stale response ignored");
      return std::nullopt;
    }
    return item.result;
  }
  void Cancel(uint64_t session) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_.erase(session);
    ready_.erase(session);
    jobs_.erase(session);
  }

 private:
  struct Job {
    std::string pinyin;
    CloudQueryContext context;
    CloudSettings settings;
    std::shared_ptr<ICloudCandidateProvider> provider;
  };
  struct Ready {
    CloudQueryContext context;
    CloudCandidateResult result;
  };
  void Log(const std::string& text) {
    if (logger_)
      logger_("[CloudCandidate] " + text);
  }
  void Run() {
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
        if (stopping_)
          return;
        auto it = jobs_.begin();
        job = std::move(it->second);
        jobs_.erase(it);
      }
      auto start = Clock::now();
      std::string description = "query start provider=" + job.settings.provider;
#ifndef NDEBUG
      description += " input=" + job.pinyin;
#else
      description += " length=" + std::to_string(job.pinyin.size());
#endif
      Log(description);
      CloudCandidateResult result;
      try {
        if (start < job.context.deadline)
          result = job.provider->Query(job.pinyin, job.context);
      } catch (...) {
        result = {false, {}, "provider_exception"};
      }
      auto now = Clock::now();
      if (now >= job.context.deadline)
        result = {false, {}, "timeout"};
      Log("response " +
          std::to_string(
              std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
                  .count()) +
          "ms candidates=" + std::to_string(result.candidates.size()) +
          " error=" + result.error);
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = active_.find(job.context.session);
      if (it == active_.end() || !(it->second == job.context)) {
        Log("stale response ignored");
        continue;
      }
      if (job.settings.cache_enabled)
        cache_.Put(job.settings.provider + ":" + job.pinyin, result, now,
                   job.settings.ttl_seconds, job.settings.max_entries);
      ready_[job.context.session] = {job.context, std::move(result)};
    }
  }
  Logger logger_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stopping_ = false;
  std::map<std::string, std::shared_ptr<ICloudCandidateProvider>> providers_;
  std::map<uint64_t, Job> jobs_;
  std::map<uint64_t, CloudQueryContext> active_;
  std::map<uint64_t, Ready> ready_;
  CloudCandidateCache cache_;
  std::thread worker_;
};
}  // namespace cloud
