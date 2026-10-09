#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/light/light_state.h"
#include "esphome/components/fastcon/fastcon_controller.h"

namespace esphome {
namespace fastcon_broadcast_light {

// Controls all lamps in the mesh at once (target address 0x00), the same way the
// brMesh app does when controlling a group:
//   off:            43 2A A8 00 00
//   color / white:  93 2A A8 00 <0x80|brightness> <blue> <red> <green> <warm> <cold>
class FastconBroadcastLight : public Component, public light::LightOutput {
 public:
  void set_controller(fastcon::FastconController *controller) { controller_ = controller; }
  void set_mesh_key(std::array<uint8_t, 4> key) { mesh_key_ = key; }
  void set_device_type(uint16_t device_type) { device_type_ = device_type; }
  void set_color_interlock(bool interlock) { color_interlock_ = interlock; }

  void dump_config() override;
  light::LightTraits get_traits() override;
  void write_state(light::LightState *state) override;

 protected:
  std::vector<uint8_t> build_encrypted_body_(uint8_t n, const std::vector<uint8_t> &data, bool forward = true, uint8_t lightness = 100);
  void queue_broadcast_(const std::vector<uint8_t> &light_data);

  fastcon::FastconController *controller_{nullptr};
  std::array<uint8_t, 4> mesh_key_{};
  uint16_t device_type_{43050};
  uint8_t sequence_{1};
  bool first_write_{true};
  bool was_on_{false};
  bool color_interlock_{false};
};

}  // namespace fastcon_broadcast_light
}  // namespace esphome
