#include "osc.h"

#include <string.h>

namespace osc {
namespace {

constexpr int kMaxBundleDepth = 4;

size_t pad4(size_t n) { return (n + 3) & ~static_cast<size_t>(3); }

uint32_t readU32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void writeU32(uint8_t* p, uint32_t v) {
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}

// Reads a NUL-terminated string padded to 4 bytes, advancing `pos` past the padding.
bool readString(const uint8_t* buf, size_t len, size_t& pos, const char*& out) {
  if (pos >= len) return false;
  const void* nul = memchr(buf + pos, 0, len - pos);
  if (!nul) return false;
  size_t strLen = static_cast<const uint8_t*>(nul) - (buf + pos);
  size_t next = pos + pad4(strLen + 1);
  if (next > len) return false;
  out = reinterpret_cast<const char*>(buf + pos);
  pos = next;
  return true;
}

bool parseMessage(const uint8_t* buf, size_t len, MessageHandler handler) {
  size_t pos = 0;
  const char* address;
  if (!readString(buf, len, pos, address) || address[0] != '/') return false;

  Value v;
  const char* tags;
  if (pos < len) {
    if (!readString(buf, len, pos, tags) || tags[0] != ',') return false;
    switch (tags[1]) {
      case 'T':
      case 'F':
        v.type = Value::Type::Bool;
        v.b = tags[1] == 'T';
        break;
      case 'i':
        if (pos + 4 > len) return false;
        v.type = Value::Type::Int;
        v.i = static_cast<int32_t>(readU32(buf + pos));
        break;
      case 'f': {
        if (pos + 4 > len) return false;
        uint32_t raw = readU32(buf + pos);
        v.type = Value::Type::Float;
        memcpy(&v.f, &raw, sizeof v.f);
        break;
      }
      case 's':
        if (!readString(buf, len, pos, v.s)) return false;
        v.type = Value::Type::String;
        break;
      default:
        break;  // no arguments, or a type we don't care about
    }
  }
  handler(address, v);
  return true;
}

bool parse(const uint8_t* buf, size_t len, MessageHandler handler, int depth) {
  if (len >= 8 && memcmp(buf, "#bundle", 8) == 0) {
    if (depth >= kMaxBundleDepth || len < 16) return false;
    size_t pos = 16;  // "#bundle\0" + 8-byte timetag, which we ignore and act immediately
    while (pos + 4 <= len) {
      uint32_t size = readU32(buf + pos);
      pos += 4;
      if (size > len - pos || size % 4 != 0) return false;
      if (!parse(buf + pos, size, handler, depth + 1)) return false;
      pos += size;
    }
    return pos == len;
  }
  return parseMessage(buf, len, handler);
}

}  // namespace

float Value::asFloat() const {
  switch (type) {
    case Type::Bool: return b ? 1.0f : 0.0f;
    case Type::Int: return static_cast<float>(i);
    case Type::Float: return f;
    default: return 0.0f;
  }
}

Value Value::fromBool(bool v) {
  Value out;
  out.type = Type::Bool;
  out.b = v;
  return out;
}

Value Value::fromFloat(float v) {
  Value out;
  out.type = Type::Float;
  out.f = v;
  return out;
}

bool parsePacket(const uint8_t* buf, size_t len, MessageHandler handler) {
  return parse(buf, len, handler, 0);
}

size_t buildMessage(uint8_t* out, size_t cap, const char* address, const Value& value) {
  char tags[3] = {',', 0, 0};
  size_t payload = 0;
  switch (value.type) {
    case Value::Type::Bool: tags[1] = value.b ? 'T' : 'F'; break;
    case Value::Type::Int: tags[1] = 'i'; payload = 4; break;
    case Value::Type::Float: tags[1] = 'f'; payload = 4; break;
    case Value::Type::String: tags[1] = 's'; payload = pad4(strlen(value.s) + 1); break;
    case Value::Type::None: break;
  }

  size_t addrLen = strlen(address);
  size_t tagLen = strlen(tags);
  size_t need = pad4(addrLen + 1) + pad4(tagLen + 1) + payload;
  if (need > cap) return 0;

  memset(out, 0, need);
  size_t pos = 0;
  memcpy(out + pos, address, addrLen);
  pos += pad4(addrLen + 1);
  memcpy(out + pos, tags, tagLen);
  pos += pad4(tagLen + 1);

  switch (value.type) {
    case Value::Type::Int: writeU32(out + pos, static_cast<uint32_t>(value.i)); break;
    case Value::Type::Float: {
      uint32_t raw;
      memcpy(&raw, &value.f, sizeof raw);
      writeU32(out + pos, raw);
      break;
    }
    case Value::Type::String: memcpy(out + pos, value.s, strlen(value.s)); break;
    default: break;
  }
  return need;
}

}  // namespace osc
