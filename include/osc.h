#pragma once
// Minimal OSC 1.0 encoder/decoder covering what VRChat uses: messages and bundles whose
// first argument is T/F (bool), i (int32), f (float32) or s (string).

#include <stddef.h>
#include <stdint.h>

namespace osc {

struct Value {
  enum class Type : uint8_t { None, Bool, Int, Float, String };
  Type type = Type::None;
  bool b = false;
  int32_t i = 0;
  float f = 0.0f;
  const char* s = nullptr;

  // Bool -> 0/1, Int/Float as-is, anything else -> 0.
  float asFloat() const;

  static Value fromBool(bool v);
  static Value fromFloat(float v);
};

using MessageHandler = void (*)(const char* address, const Value& firstArg);

// Parses a UDP payload (message or bundle, bundles may nest) and calls `handler` once per
// message. Strings passed to the handler point into `buf` and are only valid during the call.
// Returns false if the packet is malformed; messages before the bad part are still delivered.
bool parsePacket(const uint8_t* buf, size_t len, MessageHandler handler);

// Encodes a single-argument message. Returns the byte count, or 0 if `cap` is too small.
size_t buildMessage(uint8_t* out, size_t cap, const char* address, const Value& value);

}  // namespace osc
