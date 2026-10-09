#include "fastcon_group_light.h"

#include <algorithm>
#include <cmath>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/fastcon/protocol.h"
#include "esphome/components/fastcon/utils.h"

namespace esphome {
namespace fastcon_group_light {

static const char *const TAG = "fastcon_group_light";

static inline uint8_t to_u8_(float value) {
  if (value <= 0.0f) return 0;
  if (value >= 1.0f) return 255;
  return static_cast<uint8_t>(value * 255.0f + 0.5f);
}

static inline bool all_zero_(float r, float g, float b, float cw, float ww) {
  return r == 0.0f && g == 0.0f && b == 0.0f && cw == 0.0f && ww == 0.0f;
}

void FastconGroupLight::dump_config() {
  ESP_LOGCONFIG(TAG, "FastCon Group Light:");
  ESP_LOGCONFIG(TAG, "  Start light ID: %u", start_light_id_);
  ESP_LOGCONFIG(TAG, "  Mask: 0x%02X", mask_);
  ESP_LOGCONFIG(TAG, "  Color temperature: 153-500 mired");
}

light::LightTraits FastconGroupLight::get_traits() {
  light::LightTraits traits;

  // Derive the color modes from the device type (the lamp's
  // hardware capability) and the interlock preference. Device
  // types are the brMesh constants (see FirstFragment.java:629-676):
  //   43049 PWR   43050 RGBCW   43051 CCT   43168 RGB   43169 RGBW
  switch (device_type_) {
    case 43050:  // RGBCW — RGB + cold/warm white
      if (color_interlock_) {
        traits.set_supported_color_modes({light::ColorMode::RGB, light::ColorMode::COLD_WARM_WHITE});
      } else {
        traits.set_supported_color_modes({light::ColorMode::RGB_COLD_WARM_WHITE});
      }
      traits.set_min_mireds(153.0f);
      traits.set_max_mireds(500.0f);
      break;
    case 43169:  // RGBW — RGB + single white
      if (color_interlock_) {
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
    default:  // unknown: assume RGBCW combined (backward compatible)
      if (color_interlock_) {
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

std::vector<uint8_t> FastconGroupLight::build_encrypted_body_(
    uint8_t n, const std::vector<uint8_t> &data, bool forward, uint8_t lightness) {
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

std::vector<uint8_t> FastconGroupLight::prepare_standard_payload_(
    const std::vector<uint8_t> &body) {
  std::vector<uint8_t> addr{
      fastcon::DEFAULT_BLE_FASTCON_ADDRESS.begin(),
      fastcon::DEFAULT_BLE_FASTCON_ADDRESS.end()};
  return fastcon::prepare_payload(addr, body);
}

std::vector<uint8_t> FastconGroupLight::prepare_long_ble_payload_(
    const std::vector<uint8_t> &body) {
  std::vector<uint8_t> tmp(body.size() + 17, 0x00);
  tmp[15] = 0xA5;
  tmp[16] = 0x5A;
  std::copy(body.begin(), body.end(), tmp.begin() + 17);

  fastcon::WhiteningContext ctx;
  fastcon::whitening_init(0x25, ctx);
  fastcon::whitening_encode(tmp, ctx);

  return std::vector<uint8_t>(tmp.begin() + 15, tmp.end());
}

void FastconGroupLight::queue_group_select_() {
  std::vector<uint8_t> select(18, 0x00);
  const uint16_t nonce = static_cast<uint16_t>(millis() & 0xFFFF);

  select[0] = 0x45;
  select[1] = 0xFD;
  select[2] = start_light_id_;
  select[3] = nonce & 0xFF;
  select[4] = (nonce >> 8) & 0xFF;
  select[5] = mask_;

  const auto body = build_encrypted_body_(5, select, true);
  const auto rf = prepare_long_ble_payload_(body);

  ESP_LOGV(TAG, "Group select start=%u mask=0x%02X nonce=0x%04X rf=%u",
           start_light_id_, mask_, nonce, static_cast<unsigned>(rf.size()));

  controller_->queueCommand(0xFD, rf);
}

void FastconGroupLight::queue_group_control_(
    const std::vector<uint8_t> &light_data) {
  std::vector<uint8_t> control(12, 0x00);

  const uint8_t high_nibble =
      static_cast<uint8_t>((light_data.size() + 3) & 0x0F);

  control[0] = static_cast<uint8_t>((high_nibble << 4) | 0x03);
  control[1] = static_cast<uint8_t>(device_type_ & 0xFF);
  control[2] = static_cast<uint8_t>((device_type_ >> 8) & 0xFF);
  control[3] = 0xFD;

  const size_t copy_len = std::min<size_t>(light_data.size(), 8);
  std::copy(light_data.begin(), light_data.begin() + copy_len,
            control.begin() + 4);

  const auto body = build_encrypted_body_(5, control, true);
  const auto rf = prepare_standard_payload_(body);

  controller_->queueCommand(0xFD, rf);
}

void FastconGroupLight::write_state(light::LightState *state) {
  if (controller_ == nullptr) {
    ESP_LOGE(TAG, "No FastCon controller configured");
    return;
  }

  const auto &values = state->current_values;
  const bool is_on = values.is_on();

  queue_group_select_();

  if (!is_on) {
    queue_group_control_({0x00});
    was_on_ = false;
    ESP_LOGD(TAG, "Group OFF start=%u mask=0x%02X",
             start_light_id_, mask_);
    return;
  }

  // Power on: the lamps only transition OFF->ON via a 1-byte power
  // command (0x80). Send it only on that transition — the app does
  // not repeat it for brightness/color/white changes while the lamp
  // is already on, and repeating it resets the lamp to its previous
  // state, which overrides the 6-byte command below.
  if (!was_on_) {
    queue_group_control_({0x80});
    ESP_LOGD(TAG, "Group ON (power) start=%u mask=0x%02X",
             start_light_id_, mask_);
  }
  was_on_ = true;

  const float brightness = std::min(std::max(values.get_brightness(), 0.0f), 1.0f);
  uint8_t bri7 = static_cast<uint8_t>(brightness * 127.0f + 0.5f);
  if (bri7 == 0)
    bri7 = 1;
  const uint8_t on_bri = static_cast<uint8_t>(0x80 | bri7);

  // White-only command. ESPHome's current_values_as_rgbww() returns
  // all zeros for WHITE mode — the white value lives in a separate
  // channel that as_rgb()/as_cwww() do not read for WHITE — so it
  // cannot be resolved that way. The app sends warm=cold=0x7F with
  // the brightness in byte 0 (docs/GROUP_PROTOCOL.md), matching the
  // single light's get_white_light_data().
  if (values.get_color_mode() == light::ColorMode::WHITE) {
    queue_group_control_({on_bri, 0x00, 0x00, 0x00, 0x7F, 0x7F});
    ESP_LOGD(TAG, "Group white bri=%u/127 start=%u mask=0x%02X",
             bri7, start_light_id_, mask_);
    return;
  }

  // Resolve the channel levels for the current color mode. This
  // handles RGB, COLD_WARM_WHITE and the combined RGB_COLD_WARM_WHITE
  // mode, so a color-temperature change is emitted as warm/cold bytes.
  float r = 0.0f, g = 0.0f, b = 0.0f, cw = 0.0f, ww = 0.0f;
  state->current_values_as_rgbww(&r, &g, &b, &cw, &ww, /*constant_brightness=*/false);

  // Fallback for zeroed channels (no color set): RGB/RGBW lamps
  // have no cold/warm white LEDs, so fall back to RGB white;
  // RGBCW/CCT lamps fall back to warm white, so the lamp is
  // visible.
  if (all_zero_(r, g, b, cw, ww)) {
    switch (device_type_) {
      case 43168:  // RGB only
      case 43169:  // RGBW (single white, no CW/WW)
        r = g = b = 1.0f;
        break;
      default:  // 43050 RGBCW, 43051 CCT, unknown (assume RGBCW)
        ww = 1.0f;
        break;
    }
  }

  std::vector<uint8_t> light_data{
      on_bri,
      to_u8_(b),   // Blue
      to_u8_(r),   // Red
      to_u8_(g),   // Green
      to_u8_(ww),  // Warm
      to_u8_(cw),  // Cold
  };
  queue_group_control_(light_data);

  ESP_LOGD(TAG,
           "Group ON start=%u mask=0x%02X bri=%u R=%u G=%u B=%u warm=%u cold=%u",
           start_light_id_, mask_, bri7, to_u8_(r), to_u8_(g), to_u8_(b),
           to_u8_(ww), to_u8_(cw));
}

}  // namespace fastcon_group_light
}  // namespace esphome
