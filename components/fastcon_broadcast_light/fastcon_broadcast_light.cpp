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

static inline bool all_zero_(float r, float g, float b) {
  return r == 0.0f && g == 0.0f && b == 0.0f;
}

void FastconBroadcastLight::dump_config() { ESP_LOGCONFIG(TAG, "FastCon Broadcast Light (all lamps at once)"); }

light::LightTraits FastconBroadcastLight::get_traits() {
  light::LightTraits traits;
  traits.set_supported_color_modes({light::ColorMode::RGB, light::ColorMode::COLOR_TEMPERATURE});
  traits.set_min_mireds(153.0f);
  traits.set_max_mireds(500.0f);
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

  // Header: high nibble = data length + 3 (0x43 for off, 0x93 for color/white), then 2A A8, target 00 = all lamps
  control[0] = static_cast<uint8_t>((((light_data.size() + 3) & 0x0F) << 4) | 0x03);
  control[1] = 0x2A;
  control[2] = 0xA8;
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

  // Do not send anything for the restored state at boot, so the lamps do not
  // switch unexpectedly when the ESP reboots.
  if (first_write_) {
    first_write_ = false;
    return;
  }

  const auto &values = state->current_values;

  if (!values.is_on()) {
    queue_broadcast_({0x00});
    ESP_LOGD(TAG, "All lamps OFF");
    return;
  }

  // Power on: the lamps only transition OFF->ON via a 1-byte power
  // command (0x80). The 6-byte color/white command below adjusts
  // color/brightness but does not turn the lamp on.
  queue_broadcast_({0x80});
  ESP_LOGD(TAG, "All lamps ON (power)");

  const float brightness = std::min(std::max(values.get_brightness(), 0.0f), 1.0f);
  uint8_t bri7 = static_cast<uint8_t>(brightness * 127.0f + 0.5f);
  if (bri7 == 0)
    bri7 = 1;
  const uint8_t on_bri = static_cast<uint8_t>(0x80 | bri7);

  if (values.get_color_mode() == light::ColorMode::COLOR_TEMPERATURE) {
    const float mired = std::min(std::max(values.get_color_temperature(), 153.0f), 500.0f);
    const float warm_ratio = (mired - 153.0f) / (500.0f - 153.0f);
    const uint8_t warm = to_u8_(warm_ratio);
    const uint8_t cold = to_u8_(1.0f - warm_ratio);
    queue_broadcast_({on_bri, 0x00, 0x00, 0x00, warm, cold});
    ESP_LOGD(TAG, "All lamps WHITE: bri=%u/127 warm=%u cold=%u", bri7, warm, cold);
    return;
  }

  float r = values.get_red();
  float g = values.get_green();
  float b = values.get_blue();

  // Fallback for zeroed channels (no color set): light a neutral
  // white so the lamp is visible, matching the single-light
  // fallback in fastcon_controller.cpp. The broadcast light always
  // supports CW/WW, so prefer warm white.
  if (all_zero_(r, g, b)) {
    const float mired = std::min(std::max(values.get_color_temperature(), 153.0f), 500.0f);
    const float warm_ratio = (mired - 153.0f) / (500.0f - 153.0f);
    const uint8_t warm = to_u8_(warm_ratio);
    const uint8_t cold = to_u8_(1.0f - warm_ratio);
    queue_broadcast_({on_bri, 0x00, 0x00, 0x00, warm, cold});
    ESP_LOGD(TAG, "All lamps WHITE (fallback): bri=%u/127 warm=%u cold=%u", bri7, warm, cold);
    return;
  }

  const uint8_t r8 = to_u8_(r);
  const uint8_t g8 = to_u8_(g);
  const uint8_t b8 = to_u8_(b);
  queue_broadcast_({on_bri, b8, r8, g8, 0x00, 0x00});
  ESP_LOGD(TAG, "All lamps COLOR: bri=%u/127 R=%u G=%u B=%u", bri7, r8, g8, b8);
}

}  // namespace fastcon_broadcast_light
}  // namespace esphome
