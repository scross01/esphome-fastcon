#include "fastcon_broadcast_light.h"

#include <algorithm>

#include "esphome/core/log.h"
#include "esphome/components/fastcon/protocol.h"

namespace esphome {
namespace fastcon_broadcast_light {

static const char *const TAG = "fastcon_broadcast_light";

static inline uint8_t to_u8_(float value) {
  if (value <= 0.0f) return 0;
  if (value >= 1.0f) return 255;
  return static_cast<uint8_t>(value * 255.0f + 0.5f);
}

static inline bool all_zero_(float r, float g, float b, float cw, float ww) {
  return r == 0.0f && g == 0.0f && b == 0.0f && cw == 0.0f && ww == 0.0f;
}

void FastconBroadcastLight::dump_config() { ESP_LOGCONFIG(TAG, "FastCon Broadcast Light (all lamps at once)"); }

light::LightTraits FastconBroadcastLight::get_traits() {
  light::LightTraits traits;
  const bool interlock = color_interlock_;

  // Derive the color modes from the device type (the lamp's hardware
  // capability) and the interlock preference. Device types are the
  // brMesh constants (see FirstFragment.java:629-676):
  //   43049 PWR   43050 RGBCW   43051 CCT   43168 RGB   43169 RGBW
  switch (device_type_) {
    case 43050:  // RGBCW — RGB + cold/warm white
      if (interlock) {
        traits.set_supported_color_modes({light::ColorMode::RGB, light::ColorMode::COLD_WARM_WHITE});
      } else {
        traits.set_supported_color_modes({light::ColorMode::RGB_COLD_WARM_WHITE});
      }
      traits.set_min_mireds(153.0f);
      traits.set_max_mireds(500.0f);
      break;
    case 43169:  // RGBW — RGB + single white
      if (interlock) {
        traits.set_supported_color_modes({light::ColorMode::RGB, light::ColorMode::WHITE});
      } else {
        traits.set_supported_color_modes({light::ColorMode::RGB_WHITE});
      }
      break;
    case 43168:  // RGB only
      traits.set_supported_color_modes({light::ColorMode::RGB});
      break;
    case 43051:  // CCT — cold/warm white only
      traits.set_supported_color_modes({light::ColorMode::COLD_WARM_WHITE});
      traits.set_min_mireds(153.0f);
      traits.set_max_mireds(500.0f);
      break;
    case 43049:  // PWR — power only, no color modes
      break;
    default:  // unknown: assume RGBCW combined (backward compatible with plan 026)
      if (interlock) {
        traits.set_supported_color_modes({light::ColorMode::RGB, light::ColorMode::COLD_WARM_WHITE});
      } else {
        traits.set_supported_color_modes({light::ColorMode::RGB_COLD_WARM_WHITE});
      }
      traits.set_min_mireds(153.0f);
      traits.set_max_mireds(500.0f);
      break;
  }

  return traits;
}

// Same body layout as the single-light and group commands (verified against the
// brMesh app's log: type 5, sequence number, last mesh-key byte, checksum).
std::vector<uint8_t> FastconBroadcastLight::build_encrypted_body_(uint8_t n, const std::vector<uint8_t> &data,
                                                                  bool forward, uint8_t lightness) {
  std::vector<uint8_t> body(data.size() + 4, 0);

  body[0] = static_cast<uint8_t>(((n & 0x07) << 4) | (forward ? 0x80 : 0) | (lightness & 0x0F));
  body[1] = sequence_++;
  if (sequence_ == 0 || sequence_ == 0xFF)
    sequence_ = 1;
  body[2] = mesh_key_[3];

  std::copy(data.begin(), data.end(), body.begin() + 4);

  uint8_t checksum = 0;
  for (size_t i = 0; i < body.size(); i++) {
    if (i != 3)
      checksum = static_cast<uint8_t>(checksum + body[i]);
  }
  body[3] = checksum;

  for (size_t i = 0; i < 4; i++)
    body[i] ^= fastcon::DEFAULT_ENCRYPT_KEY[i & 3];

  for (size_t i = 0; i < data.size(); i++)
    body[4 + i] ^= mesh_key_[i & 3];

  return body;
}

void FastconBroadcastLight::queue_broadcast_(const std::vector<uint8_t> &light_data) {
  std::vector<uint8_t> control(12, 0x00);

  // Header: high nibble = data length + 3 (0x43 for off, 0x93 for color/white), then device type (little-endian), target 00 = all lamps
  control[0] = static_cast<uint8_t>((((light_data.size() + 3) & 0x0F) << 4) | 0x03);
  control[1] = static_cast<uint8_t>(device_type_ & 0xFF);
  control[2] = static_cast<uint8_t>((device_type_ >> 8) & 0xFF);
  control[3] = 0x00;

  const size_t copy_len = std::min<size_t>(light_data.size(), 8);
  std::copy(light_data.begin(), light_data.begin() + copy_len, control.begin() + 4);

  const auto body = build_encrypted_body_(5, control, true);
  std::vector<uint8_t> addr{fastcon::DEFAULT_BLE_FASTCON_ADDRESS.begin(), fastcon::DEFAULT_BLE_FASTCON_ADDRESS.end()};
  controller_->queueCommand(0, fastcon::prepare_payload(addr, body));
}

void FastconBroadcastLight::write_state(light::LightState *state) {
  if (controller_ == nullptr) {
    ESP_LOGE(TAG, "No FastCon controller configured");
    return;
  }

  const auto &values = state->current_values;
  const bool is_on = values.is_on();

  // Do not send anything for the restored state at boot, so the lamps do
  // not switch unexpectedly when the ESP reboots.
  if (first_write_) {
    first_write_ = false;
    was_on_ = is_on;
    return;
  }

  if (!is_on) {
    queue_broadcast_({0x00});
    was_on_ = false;
    ESP_LOGD(TAG, "All lamps OFF");
    return;
  }

  // Power on: the lamps only transition OFF->ON via a 1-byte power
  // command (0x80). Send it only on that transition — the app does not
  // repeat it for brightness/color/white changes while the lamp is
  // already on, and repeating it resets the lamp to its previous state,
  // which overrides the 6-byte command below.
  if (!was_on_) {
    queue_broadcast_({0x80});
    ESP_LOGD(TAG, "All lamps ON (power)");
  }
  was_on_ = true;

  // Resolve the channel levels for the current color mode. This handles
  // RGB, COLD_WARM_WHITE and the combined RGB_COLD_WARM_WHITE mode, so a
  // color-temperature change is emitted as warm/cold bytes.
  float r = 0.0f, g = 0.0f, b = 0.0f, cw = 0.0f, ww = 0.0f;
  state->current_values_as_rgbww(&r, &g, &b, &cw, &ww, /*constant_brightness=*/false);

  const float brightness = std::min(std::max(values.get_brightness(), 0.0f), 1.0f);
  uint8_t bri7 = static_cast<uint8_t>(brightness * 127.0f + 0.5f);
  if (bri7 == 0)
    bri7 = 1;
  const uint8_t on_bri = static_cast<uint8_t>(0x80 | bri7);

  // Fallback for zeroed channels (no color set): warm white so the lamp
  // is visible, matching the single-light fallback.
  if (all_zero_(r, g, b, cw, ww)) {
    ww = 1.0f;
  }

  queue_broadcast_({on_bri, to_u8_(b), to_u8_(r), to_u8_(g), to_u8_(ww), to_u8_(cw)});
  ESP_LOGD(TAG, "All lamps: bri=%u/127 R=%u G=%u B=%u warm=%u cold=%u",
           bri7, to_u8_(r), to_u8_(g), to_u8_(b), to_u8_(ww), to_u8_(cw));
}

}  // namespace fastcon_broadcast_light
}  // namespace esphome
