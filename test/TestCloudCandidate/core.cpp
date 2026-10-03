#include "CloudCandidate.h"
#include <atomic>
#include <cassert>
#include <iostream>
#include <stdexcept>
#undef assert
#define assert(x)                   \
  do {                              \
    if (!(x))                       \
      throw std::runtime_error(#x); \
  } while (false)
using namespace cloud;
std::string Hex(const std::string& text) {
  std::string bytes;
  for (size_t i = 0; i < text.size(); i += 2)
    bytes.push_back(
        static_cast<char>(std::stoi(text.substr(i, 2), nullptr, 16)));
  return bytes;
}
CloudCandidateResult Await(CloudCandidateManager& manager,
                           const CloudQueryContext& ctx) {
  for (int i = 0; i < 2000; ++i) {
    auto result = manager.Take(ctx);
    if (result)
      return *result;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  throw std::runtime_error("completion missing");
}
int main() {
  auto packet =
      Hex("4b0005000000000000000000000900000000030004002d4efd5604008b"
          "0170000400050008000104002d4ec78f04008b01700004000500080001"
          "04002d4e9c6704008b01700004000500080001");
  assert(BuildSogouRequest("zhongguo") ==
         Hex("1200050000000001087a686f6e6767756f17"));
  assert(BuildSogouRequest("x&y").empty());
  auto parsed = ParseSogouResponse(packet, "zhongguo");
  assert(parsed.success && parsed.candidates.size() == 3 &&
         parsed.candidates[0].text == "中国");
  for (size_t n = 0; n < packet.size(); ++n)
    assert(!ParseSogouResponse(packet.substr(0, n), "zhongguo").success);
  assert(ParseSogouResponse(packet, "zhongguofalv").candidates.empty());
  assert(!Utf16LeToUtf8(Hex("00d8")));
  assert(!Utf16LeToUtf8(Hex("00dc")));
  assert(*Utf16LeToUtf8(Hex("3dd800de")) == "😀");
  CloudCandidateCache cache;
  auto now = Clock::now();
  cache.Put("sogou:a", parsed, now, 1, 2);
  assert(cache.Get("sogou:a", now));
  assert(!cache.Get("sogou:a", now + std::chrono::seconds(1)));
  cache.Put("sogou:a", parsed, now, 10, 2);
  cache.Put("sogou:b", parsed, now, 10, 2);
  assert(cache.Get("sogou:a", now));
  cache.Put("sogou:c", parsed, now, 10, 2);
  assert(!cache.Get("sogou:b", now));
  assert(!cache.Get("google:a", now));
  CloudSettings settings;
  settings.enabled = true;
  assert(CloudPrivacyPolicy::Allows(settings, "xnhc", false, false));
  assert(!CloudPrivacyPolicy::Allows(settings, "xnhc", true, false));
  assert(!CloudPrivacyPolicy::Allows(settings, "Nfoo", false, false));
  assert(!CloudPrivacyPolicy::Allows(settings, "xnhc", false, true));
  settings.enabled = false;
  assert(!CloudPrivacyPolicy::Allows(settings, "xnhc", false, false));
  settings.enabled = true;
  for (HttpResponse response : std::vector<HttpResponse>{
           {500, {}, {}}, {200, "bad", {}}, {0, {}, "timeout"}}) {
    SogouProvider provider([&](const auto&, const auto&) { return response; });
    assert(!provider.Query("zhongguo", {}).success);
  }
  std::mutex mutex;
  std::condition_variable cv;
  bool started = false, release = false;
  std::atomic<int> calls{0};
  auto provider =
      std::make_shared<SogouProvider>([&](const auto&, const auto& ctx) {
        ++calls;
        if (ctx.query_id == "A") {
          std::unique_lock<std::mutex> lock(mutex);
          started = true;
          cv.notify_one();
          cv.wait(lock, [&] { return release; });
        }
        return HttpResponse{200, packet, {}};
      });
  CloudCandidateManager manager;
  manager.Register(provider);
  CloudQueryContext a;
  a.session = 1;
  a.query_id = "A";
  a.input_snapshot = "old";
  a.composition_revision = "1";
  auto b = a;
  b.query_id = "B";
  b.input_snapshot = "new";
  b.composition_revision = "2";
  manager.Submit("zhongguo", a, settings);
  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return started; });
  }
  manager.Submit("zhongguo", b, settings);
  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  cv.notify_one();
  assert(Await(manager, b).success);
  assert(!manager.Take(a));
  auto c = b;
  c.query_id = "C";
  manager.Submit("zhongguo", c, settings);
  assert(manager.Take(c)->success &&
         calls == 2);  // cache does not contact provider
  settings.cache_enabled = false;
  settings.timeout_ms = 1;
  auto slow = std::make_shared<SogouProvider>([&](const auto&, const auto&) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return HttpResponse{200, packet, {}};
  });
  manager.Register(slow);
  c.query_id = "slow";
  manager.Submit("zhongguo", c, settings);
  assert(Await(manager, c).error == "timeout");
  manager.Cancel(1);
  assert(!manager.Take(c));
  std::cout << "PASS protocol, UTF16, cache/TTL/LRU, privacy, failures, stale "
               "A/B, deadline\n";
}
