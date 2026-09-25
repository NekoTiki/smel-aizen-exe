#pragma once
#include <stddef.h>
#include <stdint.h>

enum class RelayMode : uint8_t { Follow, Pulse, Toggle };

struct RelayConfig {
  const char* param;
  uint8_t pin;
  RelayMode mode;
  bool activeLow;
  float threshold;
  uint32_t pulseMs;
  uint32_t maxOnMs;
  const char* feedbackParam;
};

// Owns the relay GPIOs and turns avatar parameter values into relay states.
class RelayBank {
 public:
  using ChangeCallback = void (*)(size_t index, bool on);

  RelayBank(const RelayConfig* configs, size_t count);

  // Drives every relay to its OFF level, then configures the pins as outputs.
  void begin(ChangeCallback onChange);

  // Feeds a parameter value to every relay mapped to `name`. Returns true if any matched.
  bool handleParameter(const char* name, float value);

  // Manual override (web page). The next parameter update may change it again.
  void set(size_t index, bool on);

  // Opens every relay and forgets the last input state (avatar change, WiFi loss).
  void allOff();

  // Call from loop(): ends pulses and enforces maxOnMs.
  void update();

  size_t size() const { return count_; }
  const RelayConfig& config(size_t index) const { return configs_[index]; }
  bool isOn(size_t index) const { return states_[index].on; }
  bool inputActive(size_t index) const { return states_[index].inputActive; }

 private:
  struct State {
    bool on;
    bool inputActive;
    uint32_t onSince;
  };

  static constexpr size_t kMaxRelays = 16;
  // Float inputs must fall this far below the threshold to deactivate, so a Proximity
  // contact hovering right at the threshold doesn't chatter the relay.
  static constexpr float kHysteresis = 0.05f;

  void drive(size_t index, bool on);

  const RelayConfig* configs_;
  size_t count_;
  State states_[kMaxRelays] = {};
  ChangeCallback onChange_ = nullptr;
};
