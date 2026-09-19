#pragma once

#include <cstdint>

namespace weasel {

// Some TSF hosts test an event more than once, or omit its OnKeyDown callback.
// A handled test belongs only to that exact event, never to the next key.
class KeyEventCache {
 public:
  bool Matches(std::uintptr_t key, std::intptr_t info) const {
    return pending_ && key_ == key && info_ == info;
  }

  void Remember(std::uintptr_t key, std::intptr_t info) {
    key_ = key;
    info_ = info;
    pending_ = true;
  }

  void Clear() { pending_ = false; }

 private:
  bool pending_ = false;
  std::uintptr_t key_ = 0;
  std::intptr_t info_ = 0;
};

}  // namespace weasel
