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

static inline uint8_t clamp_u8_(float value) {
  if (value <= 0.0f) return 0;
  if (value >= 255.0f) return 255;
  return static_cast<uint8_t>(value + 0.5f);
}

void FastconGroupLight::dump_config() {
  ESP_LOGCONFIG(TAG, "FastCon Group Light:");
  ESP_LOGCONFIG(TAG, "  Start light ID: %u", start_light_id_);
  ESP_LOGCONFIG(TAG, "  Mask: 0x%02X", mask_);
  ESP_LOGCONFIG(TAG, "  Color temperature: 153-500 mired");
}

light::LightTraits FastconGroupLight::get_traits() {
  light::LightTraits traits;
  traits.set_supported_color_modes({light::ColorMode::COLOR_TEMPERATURE});
  traits.set_min_mireds(153.0f);
  traits.set_max_mireds(500.0f);
  return traits;
}

std::vector<uint8_t> FastconGroupLight::build_encrypted_body_(
    uint8_t n, const std::vector<uint8_t> &data, bool forward) {
  std::vector<uint8_t> body(data.size() + 4, 0);

  body[0] = ((n & 0x07) << 4) | (forward ? 0x80 : 0);
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
  control[1] = 0x2A;
  control[2] = 0xA8;
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
    ESP_LOGD(TAG, "Group OFF start=%u mask=0x%02X",
             start_light_id_, mask_);
    return;
  }

  float brightness = values.get_brightness();
  if (brightness < 0.0f) brightness = 0.0f;
  if (brightness > 1.0f) brightness = 1.0f;
  uint8_t bri7 = static_cast<uint8_t>(brightness * 127.0f);
  if (bri7 == 0)
    bri7 = 1;

  float mired = values.get_color_temperature();
  if (mired < 153.0f) mired = 153.0f;
  if (mired > 500.0f) mired = 500.0f;

  const float warm_ratio = (mired - 153.0f) / (500.0f - 153.0f);
  const float cold_ratio = 1.0f - warm_ratio;

  const uint8_t warm = clamp_u8_(warm_ratio * 255.0f);
  const uint8_t cold = clamp_u8_(cold_ratio * 255.0f);

  std::vector<uint8_t> light_data{
      static_cast<uint8_t>(0x80 | bri7),
      0x00,
      0x00,
      0x00,
      warm,
      cold,
  };

  queue_group_control_(light_data);

  ESP_LOGD(TAG,
           "Group ON start=%u mask=0x%02X bri=%u ct=%.1f warm=%u cold=%u",
           start_light_id_, mask_, bri7, mired, warm, cold);
}

}  // namespace fastcon_group_light
}  // namespace esphome
