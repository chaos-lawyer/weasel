#include <KeyEventCache.h>
#include <cassert>
#include <iostream>

int main() {
  weasel::KeyEventCache pending;
  constexpr std::intptr_t first = 0x00210001;
  constexpr std::intptr_t repeat = 0x40210001;
  constexpr std::intptr_t release = static_cast<std::intptr_t>(0xC0210001u);
  assert(!pending.Matches('F', first));
  pending.Remember('F', first);
  // Word can test a physical event repeatedly before delivering it.
  assert(pending.Matches('F', first));
  assert(pending.Matches('F', first));
  // An omitted delivery must not make the next key look already handled.
  assert(!pending.Matches('G', first));
  // A repeat or release is a different event, even with the same virtual key.
  assert(!pending.Matches('F', repeat));
  assert(!pending.Matches('F', release));
  pending.Clear();
  assert(!pending.Matches('F', first));
  pending.Remember('F', release);
  assert(pending.Matches('F', release));
  assert(!pending.Matches('F', first));
  pending.Clear();
  assert(!pending.Matches('F', release));
  std::cout << "PASS TSF repeated tests, missing delivery, repeat and release "
               "identity\n";
}
