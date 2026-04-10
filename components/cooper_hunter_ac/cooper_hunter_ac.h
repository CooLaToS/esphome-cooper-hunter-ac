#pragma once
#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/climate/climate.h"

namespace esphome {
namespace cooper_hunter {

// ─── Protocol constants ───────────────────────────────────────────────────────
// Packet: AA AA 12 [cmd] [16 data bytes] [checksum]  — 21 bytes total
// cmd:  A0 = ESP→AC status query  |  00 = AC→ESP status  |  01 = ESP→AC control
// Checksum: sum of bytes [0..19] mod 256
//
// Byte map (confirmed by sniffing, indices into the 21-byte packet):
//
// pkt[7]  d_[3]:  bit7=cpmode  bits[6:4]=fan  bit3=POWER  bits[2:0]=mode
//   mode: 0=auto  1=cool  2=dry  3=fan_only  4=heat
//   fan:  0=auto  1=low   2=med  3..6=high
//
// pkt[8]  d_[4]:  bit6=mute  bit5=tempUnit(1=°F display)  bits[4:0]=temp_encoded
//   temp_encoded = (setpoint_°C − 16)  range 0–14 (16–30°C)
//
// pkt[9]  d_[5]:  bit4=horizontal_swing  bit0=vertical_swing
//   0x00=off  0x01=vertical  0x10=horizontal  0x11=both
//   Set entire byte when changing swing — partial mask leaves auto-swing bits active
//
// pkt[10] d_[6]:  bit7=display  bit6=health/ionizer  bit4=heat_active(read-only)  bit1=sleep
//
// pkt[11] d_[7]:  bit7=off-timer active  bit3=on-timer active
// pkt[12] d_[8]:  timer minutes byte A
// pkt[13] d_[9]:  timer minutes byte B
//   off-timer: big-endian    (pkt[12]<<8)|pkt[13]
//   on-timer:  little-endian  pkt[12]|(pkt[13]<<8)
//   range 0–1440 min, 1-min resolution (confirmed by hardware test)
//
// pkt[15] d_[11]: indoor temperature °C (raw)
// pkt[16] d_[12]: auxiliary field — value 0x05 observed when indoor temp = 20°C

static const uint8_t QUERY_PKT[21] = {
    0xAA, 0xAA, 0x12, 0xA0, 0x0A, 0x0A, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1A
};

class CooperHunterAC : public climate::Climate, public PollingComponent, public uart::UARTDevice {
 public:
  CooperHunterAC() : PollingComponent(5000) {}

  bool display_on()        { return display_on_; }
  bool health_on()         { return health_on_; }
  uint16_t timer_minutes() { return timer_minutes_; }
  bool timer_on_mode()     { return timer_on_mode_; }
  climate::ClimateMode last_active_mode() { return last_active_mode_; }

  // Enable raw packet dumps to the log — useful for sniffing unknown bytes.
  void set_debug_raw(bool on) { debug_raw_ = on; }

  void set_display(bool on) {
    if (!got_status_) { ESP_LOGW("ch_ac", "set_display: no status yet, ignoring"); return; }
    if (on) d_[6] |=  0x80;
    else    d_[6] &= ~0x80;
    display_on_ = on;
    send_control_();
    ESP_LOGI("ch_ac", "Display %s", on ? "ON" : "OFF");
  }

  void set_health(bool on) {
    if (!got_status_) { ESP_LOGW("ch_ac", "set_health: no status yet, ignoring"); return; }
    if (on) d_[6] |=  0x40;
    else    d_[6] &= ~0x40;
    health_on_ = on;
    send_control_();
    ESP_LOGI("ch_ac", "Health %s", on ? "ON" : "OFF");
  }

  void set_timer(uint16_t minutes) {
    if (!got_status_) { ESP_LOGW("ch_ac", "set_timer: no status yet, ignoring"); return; }
    if (minutes == 0) {
      d_[7] &= ~0x88;  // pkt[11]: clear both on-timer (bit3) and off-timer (bit7)
      d_[8]  = 0x00;
      d_[9]  = 0x00;
    } else {
      if (minutes > 1440) minutes = 1440;
      d_[7] &= ~0x88;  // clear both bits first
      if (timer_on_mode_) {
        d_[7] |= 0x08;
        d_[8]  = (uint8_t)(minutes & 0xFF);         // low byte first (little-endian)
        d_[9]  = (uint8_t)((minutes >> 8) & 0xFF);  // high byte second
      } else {
        d_[7] |= 0x80;
        d_[8]  = (uint8_t)((minutes >> 8) & 0xFF);  // high byte first (big-endian)
        d_[9]  = (uint8_t)(minutes & 0xFF);          // low byte second
      }
    }
    timer_minutes_ = minutes;
    send_control_();
    publish_state();
    ESP_LOGI("ch_ac", "Timer (%s) set to %d min", timer_on_mode_ ? "ON" : "OFF", minutes);
  }

  void set_timer_on_mode(bool on_timer) {
    timer_on_mode_ = on_timer;
    // If a timer is already active, resend with the new mode bit
    if (timer_minutes_ > 0 && got_status_) {
      set_timer(timer_minutes_);
    }
    ESP_LOGI("ch_ac", "Timer mode: %s", on_timer ? "on-timer" : "off-timer");
  }

  // ── ESPHome hooks ──────────────────────────────────────────────────────────
  void setup() override {
    // Defaults matching observed AC state
    d_[0] = 0x0A; d_[1] = 0x0A; d_[2] = 0x00; d_[3] = 0x12;
    d_[4] = 0x08; d_[5] = 0x00; d_[6] = 0xC0; d_[7] = 0x00;
    d_[8] = 0x00; d_[9] = 0x00; d_[10] = 0x00; d_[11] = 0x14;
    d_[12] = 0x00; d_[13] = 0x00; d_[14] = 0x00; d_[15] = 0x00;
  }

  void update() override {
    // Send AA AA 12 A0 status query every poll interval
    write_array(QUERY_PKT, 21);
    ESP_LOGD("ch_ac", "Sent status query");
  }

  void loop() override {
    // Read available bytes into ring buffer
    while (available()) {
      uint8_t b;
      read_byte(&b);
      if (buf_len_ < sizeof(buf_)) {
        buf_[buf_len_++] = b;
      }
    }

    // Try to find and consume a valid 21-byte AA AA 12 packet
    while (buf_len_ >= 21) {
      // Scan for AA AA 12 header
      if (buf_[0] != 0xAA || buf_[1] != 0xAA || buf_[2] != 0x12) {
        memmove(buf_, buf_ + 1, --buf_len_);
        continue;
      }
      // Validate checksum
      uint32_t csum = 0;
      for (int i = 0; i < 20; i++) csum += buf_[i];
      if ((csum & 0xFF) != buf_[20]) {
        ESP_LOGW("ch_ac", "Bad checksum: calc=%02X got=%02X", csum & 0xFF, buf_[20]);
        memmove(buf_, buf_ + 1, --buf_len_);
        continue;
      }
      // Good packet — parse if it's a status response (cmd=00)
      if (buf_[3] == 0x00) {
        parse_status_(buf_);
      } else if (buf_[3] != 0xA0) {
        // Unknown command byte — log raw packet so we can reverse-engineer it
        ESP_LOGI("ch_ac", "Unknown cmd=0x%02X: %s", buf_[3], format_hex_(buf_, 21));
      }
      // Consume 21 bytes
      buf_len_ -= 21;
      if (buf_len_ > 0) memmove(buf_, buf_ + 21, buf_len_);
    }
  }

  // ── Climate traits ─────────────────────────────────────────────────────────
  climate::ClimateTraits traits() override {
    auto t = climate::ClimateTraits();
    t.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
    t.set_supported_modes({
        climate::CLIMATE_MODE_OFF,
        climate::CLIMATE_MODE_COOL,
        climate::CLIMATE_MODE_HEAT,
        climate::CLIMATE_MODE_DRY,
        climate::CLIMATE_MODE_FAN_ONLY,
        climate::CLIMATE_MODE_AUTO,
    });
    t.set_supported_fan_modes({
        climate::CLIMATE_FAN_AUTO,
        climate::CLIMATE_FAN_LOW,
        climate::CLIMATE_FAN_MEDIUM,
        climate::CLIMATE_FAN_HIGH,
    });
    t.set_supported_swing_modes({
        climate::CLIMATE_SWING_OFF,
        climate::CLIMATE_SWING_VERTICAL,
        climate::CLIMATE_SWING_HORIZONTAL,
        climate::CLIMATE_SWING_BOTH,
    });
    t.set_supported_presets({
        climate::CLIMATE_PRESET_NONE,
        climate::CLIMATE_PRESET_SLEEP,
    });
    t.set_visual_min_temperature(16);
    t.set_visual_max_temperature(30);
    t.set_visual_temperature_step(1);
    return t;
  }

  // ── Control handler ────────────────────────────────────────────────────────
  void control(const climate::ClimateCall &call) override {
    if (!got_status_ || millis() < boot_grace_ms_) {
      ESP_LOGW("ch_ac", "Ignoring control call during boot grace period");
      return;
    }
    bool changed = false;

    if (call.get_mode().has_value()) {
      auto mode = call.get_mode().value();
      if (mode != this->mode) {
        changed = true;
        if (mode == climate::CLIMATE_MODE_OFF) {
          set_power_(false);
        } else {
          set_power_(true);
          set_mode_(mode);
        }
      }
    }
    if (call.get_target_temperature().has_value()) {
      if (call.get_target_temperature().value() != this->target_temperature) {
        changed = true;
        set_temp_(call.get_target_temperature().value());
      }
    }
    if (call.get_fan_mode().has_value()) {
      if (call.get_fan_mode().value() != this->fan_mode) {
        changed = true;
        set_fan_(call.get_fan_mode().value());
      }
    }
    if (call.get_swing_mode().has_value()) {
      if (call.get_swing_mode().value() != this->swing_mode) {
        changed = true;
        set_swing_(call.get_swing_mode().value());
      }
    }
    if (call.get_preset().has_value()) {
      if (call.get_preset().value() != this->preset) {
        changed = true;
        bool sleep = (call.get_preset().value() == climate::CLIMATE_PRESET_SLEEP);
        if (sleep) d_[6] |=  0x02;
        else       d_[6] &= ~0x02;
        this->preset = call.get_preset().value();
      }
    }

    if (!changed) {
      ESP_LOGD("ch_ac", "Control call with no state change, ignoring");
      return;
    }
    send_control_();
    publish_state();
  }

 protected:
  // Last received payload bytes [4..19] (indices into full 21-byte packet)
  uint8_t d_[16];
  uint8_t buf_[128];
  int buf_len_ = 0;
  bool     display_on_    = true;
  bool     health_on_     = true;    // health/ionizer ON by default
  uint16_t timer_minutes_ = 0;
  bool     timer_on_mode_ = false;   // false=off-timer (bit7), true=on-timer (bit3)
  bool     debug_raw_     = false;
  bool got_status_ = false;
  uint32_t boot_grace_ms_ = 0;
  climate::ClimateMode last_active_mode_ = climate::CLIMATE_MODE_COOL;

  // ── Parsing ────────────────────────────────────────────────────────────────
  void parse_status_(const uint8_t *pkt) {
    if (!got_status_) {
      got_status_ = true;
      boot_grace_ms_ = millis() + 15000;
    }
    // Store payload bytes for building control packets
    for (int i = 0; i < 16; i++) d_[i] = pkt[4 + i];
    // Clamp stored setpoint to valid range (16–30°C = encoded 0–14).
    // Prevents echoing an out-of-range byte back to the AC in control packets.
    if ((d_[4] & 0x1F) > 14) d_[4] = (d_[4] & 0xE0) | 14;

    uint8_t d7 = pkt[7];
    uint8_t d8 = pkt[8];

    bool power_on = (d7 >> 3) & 0x01;
    uint8_t mode  = d7 & 0x07;
    // fan: bits[6:4] of d7
    uint8_t fan_raw = (d7 >> 4) & 0x07;

    // Temperature: bits[4:0] of d8 are already encoded as (°C − 16)
    float set_temp = 16.0f + (float)(d8 & 0x1F);

    // Indoor temp: pkt[15]
    float indoor = (float) pkt[15];

    // Update climate state
    current_temperature = indoor;
    target_temperature  = set_temp;

    if (!power_on) {
      this->mode = climate::CLIMATE_MODE_OFF;
    } else {
      this->mode = ac_mode_to_climate_(mode);
      last_active_mode_ = this->mode;
    }
    this->fan_mode = ac_fan_to_climate_(fan_raw);

    // Timer: pkt[11] bit7=off-timer active, bit3=on-timer active
    // Off-timer: big-endian    (pkt[12]=high, pkt[13]=low)  — confirmed by hardware
    // On-timer:  little-endian (pkt[12]=low,  pkt[13]=high) — confirmed by sniffing IR remote
    if (pkt[11] & 0x80) {
      timer_minutes_ = ((uint16_t)pkt[12] << 8) | pkt[13];
      timer_on_mode_ = false;
    } else if (pkt[11] & 0x08) {
      timer_minutes_ = (uint16_t)pkt[12] | ((uint16_t)pkt[13] << 8);
      timer_on_mode_ = true;
    } else {
      timer_minutes_ = 0;
    }

    // pkt[10] (d_[6]): bit7=display  bit6=health  bit1=sleep
    display_on_ = (pkt[10] >> 7) & 0x01;
    health_on_  = (pkt[10] >> 6) & 0x01;
    this->preset = ((pkt[10] >> 1) & 0x01)
                   ? climate::CLIMATE_PRESET_SLEEP
                   : climate::CLIMATE_PRESET_NONE;

    // Swing: pkt[9] (d_[5]) — bit0=vertical, bit4=horizontal
    uint8_t swing_raw = pkt[9];
    bool sv = swing_raw & 0x01;
    bool sh = swing_raw & 0x10;
    if (sv && sh)   this->swing_mode = climate::CLIMATE_SWING_BOTH;
    else if (sv)    this->swing_mode = climate::CLIMATE_SWING_VERTICAL;
    else if (sh)    this->swing_mode = climate::CLIMATE_SWING_HORIZONTAL;
    else            this->swing_mode = climate::CLIMATE_SWING_OFF;

    ESP_LOGI("ch_ac", "pwr=%d mode=%d fan=%d set=%.0f°C in=%.0f°C disp=%d health=%d sleep=%d swing=%02X timer=%d",
             (int)power_on, mode, fan_raw, set_temp, indoor,
             (int)display_on_, (int)health_on_,
             (int)(this->preset == climate::CLIMATE_PRESET_SLEEP), swing_raw,
             (int)timer_minutes_);

    // Raw dump for sniffing unknown bytes (swing, turbo, eco, sleep, quiet).
    // Enable with set_debug_raw(true) or the "Cooper Hunter Debug Raw" switch.
    if (debug_raw_) {
      ESP_LOGI("ch_ac", "RAW status: %s", format_hex_(pkt, 21));
    }

    publish_state();
  }

  // ── Mode / fan conversion ──────────────────────────────────────────────────
  climate::ClimateMode ac_mode_to_climate_(uint8_t m) {
    switch (m) {
      case 0: return climate::CLIMATE_MODE_AUTO;
      case 1: return climate::CLIMATE_MODE_COOL;
      case 2: return climate::CLIMATE_MODE_DRY;
      case 3: return climate::CLIMATE_MODE_FAN_ONLY;
      case 4: return climate::CLIMATE_MODE_HEAT;
      default: return climate::CLIMATE_MODE_AUTO;
    }
  }

  uint8_t climate_mode_to_ac_(climate::ClimateMode m) {
    switch (m) {
      case climate::CLIMATE_MODE_AUTO:     return 0;
      case climate::CLIMATE_MODE_COOL:     return 1;
      case climate::CLIMATE_MODE_DRY:      return 2;
      case climate::CLIMATE_MODE_FAN_ONLY: return 3;
      case climate::CLIMATE_MODE_HEAT:     return 4;
      default:                             return 1;
    }
  }

  climate::ClimateFanMode ac_fan_to_climate_(uint8_t f) {
    switch (f) {
      case 0: return climate::CLIMATE_FAN_AUTO;
      case 1: return climate::CLIMATE_FAN_LOW;
      case 2: return climate::CLIMATE_FAN_MEDIUM;
      case 3:
      case 4:
      case 5:
      case 6: return climate::CLIMATE_FAN_HIGH;
      default: return climate::CLIMATE_FAN_AUTO;
    }
  }

  uint8_t climate_fan_to_ac_(climate::ClimateFanMode f) {
    switch (f) {
      case climate::CLIMATE_FAN_AUTO:   return 0;
      case climate::CLIMATE_FAN_LOW:    return 1;
      case climate::CLIMATE_FAN_MEDIUM: return 2;
      case climate::CLIMATE_FAN_HIGH:   return 4;
      default:                          return 0;
    }
  }

  // ── State setters (modify d_ in place) ────────────────────────────────────
  void set_power_(bool on) {
    // d_[3] = d7 (offset 3 in d_ array since d_[0]=pkt[4], d_[3]=pkt[7])
    if (on) d_[3] |=  0x08;
    else    d_[3] &= ~0x08;
  }

  void set_mode_(climate::ClimateMode m) {
    uint8_t ac_m = climate_mode_to_ac_(m);
    d_[3] = (d_[3] & 0xF8) | (ac_m & 0x07);  // preserve upper bits, set bits[2:0]
  }

  void set_temp_(float temp_c) {
    uint8_t t = (uint8_t) temp_c;
    if (t < 16) t = 16;
    if (t > 30) t = 30;
    uint8_t enc = (t - 16) & 0x1F;
    d_[4] = (d_[4] & 0xE0) | enc;  // d_[4]=d8: preserve upper 3 bits (mute, tempUnit, bit7)
  }

  void set_fan_(climate::ClimateFanMode f) {
    uint8_t fan = climate_fan_to_ac_(f);
    d_[3] = (d_[3] & 0x8F) | ((fan & 0x07) << 4);  // bits[6:4] of d7
  }

  void set_swing_(climate::ClimateSwingMode s) {
    // d_[5] = pkt[9]: bit0=vertical, bit4=horizontal.
    // Zero the entire byte — the IR remote does the same, and partial masking
    // leaves auto-swing bits (1,3,5) active which keeps the louvers moving.
    d_[5] = 0x00;
    if (s == climate::CLIMATE_SWING_VERTICAL || s == climate::CLIMATE_SWING_BOTH)
      d_[5] |= 0x01;
    if (s == climate::CLIMATE_SWING_HORIZONTAL || s == climate::CLIMATE_SWING_BOTH)
      d_[5] |= 0x10;
  }

  // ── Send control command ───────────────────────────────────────────────────
  void send_control_() {
    uint8_t pkt[21];
    pkt[0] = 0xAA; pkt[1] = 0xAA; pkt[2] = 0x12; pkt[3] = 0x01;
    for (int i = 0; i < 16; i++) pkt[4 + i] = d_[i];

    uint32_t csum = 0;
    for (int i = 0; i < 20; i++) csum += pkt[i];
    pkt[20] = csum & 0xFF;

    write_array(pkt, 21);
    ESP_LOGI("ch_ac", "Control cmd: %s", format_hex_(pkt, 21));
  }

  // ── Helpers ────────────────────────────────────────────────────────────────
  // Format up to 21 bytes as a hex string into a static buffer.
  // Not reentrant, but only called from the main ESPHome loop.
  const char *format_hex_(const uint8_t *data, size_t len) {
    // 21 bytes × 3 chars ("XX ") + null = 64 bytes minimum; 96 gives headroom.
    static char hex[96];
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 3 < sizeof(hex); i++) {
      pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", data[i]);
    }
    if (pos > 0) hex[pos - 1] = '\0';  // trim trailing space
    return hex;
  }
};

}  // namespace cooper_hunter
}  // namespace esphome
