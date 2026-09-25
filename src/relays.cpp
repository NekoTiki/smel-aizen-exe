#include "relays.h"

#include <Arduino.h>
#include <string.h>

RelayBank::RelayBank(const RelayConfig* configs, size_t count)
    : configs_(configs), count_(count < kMaxRelays ? count : kMaxRelays) {}

void RelayBank::begin(ChangeCallback onChange) {
  onChange_ = onChange;
  for (size_t i = 0; i < count_; i++) {
    // Set the OFF level before enabling the output so active-low boards don't click on boot.
    drive(i, false);
    pinMode(configs_[i].pin, OUTPUT);
    drive(i, false);
  }
}

bool RelayBank::handleParameter(const char* name, float value) {
  bool matched = false;
  for (size_t i = 0; i < count_; i++) {
    const RelayConfig& cfg = configs_[i];
    if (strcmp(cfg.param, name) != 0) continue;
    matched = true;

    State& st = states_[i];
    bool active = st.inputActive ? value > cfg.threshold - kHysteresis : value >= cfg.threshold;
    bool rising = active && !st.inputActive;
    st.inputActive = active;

    switch (cfg.mode) {
      case RelayMode::Follow:
        set(i, active);
        break;
      case RelayMode::Pulse:
        if (rising) {
          set(i, true);
          st.onSince = millis();  // re-touching during a pulse extends it
        }
        break;
      case RelayMode::Toggle:
        if (rising) set(i, !st.on);
        break;
    }
  }
  return matched;
}

void RelayBank::set(size_t index, bool on) {
  if (index >= count_) return;
  State& st = states_[index];
  if (st.on == on) return;
  st.on = on;
  if (on) st.onSince = millis();
  drive(index, on);
  if (onChange_) onChange_(index, on);
}

void RelayBank::allOff() {
  for (size_t i = 0; i < count_; i++) {
    set(i, false);
    states_[i].inputActive = false;
  }
}

void RelayBank::update() {
  uint32_t now = millis();
  for (size_t i = 0; i < count_; i++) {
    const RelayConfig& cfg = configs_[i];
    State& st = states_[i];
    if (!st.on) continue;
    uint32_t elapsed = now - st.onSince;
    if (cfg.mode == RelayMode::Pulse && elapsed >= cfg.pulseMs) {
      set(i, false);
    } else if (cfg.maxOnMs > 0 && elapsed >= cfg.maxOnMs) {
      Serial.printf("[relay] %s hit maxOnMs (%u ms), forcing off\n", cfg.param, (unsigned)cfg.maxOnMs);
      set(i, false);
    }
  }
}

void RelayBank::drive(size_t index, bool on) {
  const RelayConfig& cfg = configs_[index];
  digitalWrite(cfg.pin, (on != cfg.activeLow) ? HIGH : LOW);
}
