#pragma once

#include <ExternalTemplate.h>

namespace weasel::external_caret {
inline constexpr uint32_t kRequestTag = 0x57534331;
inline constexpr uint32_t kReplyTag = 0x57534332;
inline constexpr size_t kRequestSize = 32;
inline constexpr size_t kReplySize = 40;
inline constexpr uint64_t kMaxLifetimeMs = 200;

struct Request {
  uint32_t id = 0;
  uint64_t target = 0;
  uint64_t reply = 0;
  uint64_t deadline = 0;
};

struct Reply {
  uint32_t id = 0;
  uint64_t target = 0;
  bool available = false;
  int32_t left = 0;
  int32_t top = 0;
  int32_t right = 0;
  int32_t bottom = 0;
};

inline bool Valid(const Request& request, uint64_t now) {
  return request.id && request.target && request.reply &&
         request.deadline > now && request.deadline - now <= kMaxLifetimeMs;
}

inline bool Decode(const void* buffer, size_t size, Request& request) {
  using external_template::ReadNumber;
  if (!buffer || size != kRequestSize)
    return false;
  const auto* data = static_cast<const uint8_t*>(buffer);
  if (ReadNumber(data, 4) != 1)
    return false;
  Request parsed;
  parsed.id = static_cast<uint32_t>(ReadNumber(data + 4, 4));
  parsed.target = ReadNumber(data + 8, 8);
  parsed.reply = ReadNumber(data + 16, 8);
  parsed.deadline = ReadNumber(data + 24, 8);
  if (!parsed.id || !parsed.target || !parsed.reply || !parsed.deadline)
    return false;
  request = parsed;
  return true;
}

inline std::vector<uint8_t> Encode(const Reply& reply) {
  using external_template::AppendNumber;
  if (!reply.id || !reply.target ||
      (reply.available &&
       (reply.right < reply.left || reply.bottom <= reply.top)))
    return {};
  std::vector<uint8_t> data;
  AppendNumber(data, 1, 4);
  AppendNumber(data, reply.id, 4);
  AppendNumber(data, reply.target, 8);
  AppendNumber(data, reply.available ? 1 : 0, 4);
  AppendNumber(data, 0, 4);
  AppendNumber(data, static_cast<uint32_t>(reply.left), 4);
  AppendNumber(data, static_cast<uint32_t>(reply.top), 4);
  AppendNumber(data, static_cast<uint32_t>(reply.right), 4);
  AppendNumber(data, static_cast<uint32_t>(reply.bottom), 4);
  return data;
}
}  // namespace weasel::external_caret
