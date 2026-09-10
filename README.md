# ESPHome Fastcon BLE Light Component

This is a custom component for ESPHome that allows you to control Broadlink Fastcon BLE lights, also known as brMesh. It should work with any light that can be controlled by brMesh or Broadlink BLE mobile apps.

Be warned - there is also a brLight app, which might look like brMesh, but the protocol is different.

## Requirements

- ESP32 board
- ESPHome 2023.12.0 or newer

## Supported Features

- On/Off control
- Brightness control
- RGB color control
- White mode
- Experimental group on/off, brightness and cold/warm white control

## Configuration

Add the following to your ESPHome configuration:

```yaml
# ESP32 is required
esp32:
  board: esp32-s3-devkitc-1
  framework:
    type: arduino

esp32_ble_tracker:
esp32_ble_server:

# Source configuration
external_components:
  - source: github://scross01/esphome-fastcon@dev

# Controller configuration
fastcon:
  mesh_key: "12345678"    # Your mesh key in hex format

  # Optional parameters to control the advertisdement protocol with their defaults:
  adv_interval_min: 0x20  # Minimum advertisement interval
  adv_interval_max: 0x40  # Maximum advertisement interval
  adv_duration: 50        # Advertisement duration in milliseconds
  adv_gap: 10             # Gap between advertisements in milliseconds
  max_queue_size: 100     # Maximum number of queued commands

# Light configuration (add an entry for each light)
light:
  - platform: fastcon
    id: living_room_light
    name: "Living Room Light"
    light_id: 1           # ID of the light (1-255)
    supports_cwww: true   # Optional: Set to true if the light supports cold/warm white
    color_interlock: true # Optional: Set to true to prevent RGB and white LEDs from being on at the same time
```

### Configuration Variables

#### Fastcon Controller

- **mesh_key** (*Required*, string): The mesh key for your Fastcon lights in hexadecimal format (8 characters/4 bytes)
- **id** (*Optional*, ID): The ID to use for this controller component. Defaults to "fastcon_controller"
- **adv_interval_min** (*Optional*, int): Minimum advertisement interval. Defaults to 0x20
- **adv_interval_max** (*Optional*, int): Maximum advertisement interval. Defaults to 0x40
- **adv_duration** (*Optional*, int): Duration of each advertisement in milliseconds. Defaults to 50
- **adv_gap** (*Optional*, int): Gap between advertisements in milliseconds. Defaults to 10
- **max_queue_size** (*Optional*, int): Maximum number of commands that can be queued. Defaults to 100

#### Fastcon Light

- **light_id** (*Required*, int): The ID of the light (1-255)
- **name** (*Required*, string): The name for the light entity
- **id** (*Optional*, ID): The ID to use for this light component
- **controller_id** (*Optional*, ID): The ID of the controller to use. Defaults to "fastcon_controller"
- **supports_cwww** (*Optional*, boolean): Set to `true` if the light supports cold/warm white channels. Defaults to `false`.
- **color_interlock** (*Optional*, boolean): Set to `true` to prevent RGB and white LEDs from being on at the same time. Defaults to `false`.

## Experimental group light control

The `fastcon_group_light` platform sends a temporary brMesh/FastCon group selector followed by one group control command. This avoids queuing one command per lamp and gives near-simultaneous switching for a group of consecutive light IDs.

Supported group features:

- On/off
- Brightness
- Cold/warm white color temperature
- Native ESPHome/Home Assistant light entity

Example for six consecutive lamps with IDs 12 through 17:

```yaml
fastcon:
  id: fastcon_controller
  mesh_key: "12345678"

light:
  - platform: fastcon_group_light
    id: living_room_group
    name: "Living Room"
    controller_id: fastcon_controller
    mesh_key: "12345678"
    start_light_id: 12
    mask: 0x3F
    default_transition_length: 0s
    restore_mode: ALWAYS_OFF
```

`start_light_id` is the first lamp ID. `mask` selects consecutive IDs, with bit 0 selecting `start_light_id`, bit 1 selecting the next ID, and so on.

Examples:

- `start_light_id: 18`, `mask: 0x07` -> IDs 18..20
- `start_light_id: 7`, `mask: 0x1F` -> IDs 7..11
- `start_light_id: 12`, `mask: 0x3F` -> IDs 12..17

The implementation currently exposes a 153-500 mired color-temperature range. It has been tested with ESPHome 2026.8.2 on an ESP32-S3 with 23 brMesh/FastCon lamps split into five groups. Group sizes of 3, 5 and 6 lamps were tested for on/off, brightness and color temperature.

Current limitations:

- Lamp IDs in a group must be contiguous.
- RGB group control is not implemented.
- The bulbs do not acknowledge commands or report state back.
- The 8-bit mask implies up to 8 consecutive IDs, but the 8-lamp case has not been tested.
- The group protocol is reverse engineered and may differ across app/firmware variants.

See `docs/GROUP_PROTOCOL.md` for the reverse-engineered protocol notes and `examples/group_lights.yaml` for more examples.

## Finding Your Mesh Key

The mesh key is crucial for controlling your Fastcon BLE lights. To find your light's mesh key, you first need to setup your devices using an Android device. The app generates a unique mesh key that will be used with all lights that are set up in the app.

Once the lights are setup, you can use ADB to connect to your phone and you may use the following command to extract the mesh key.

```bash
adb logcat | { grep -m 1 -o 'jyq_helper: .* payload:.\{24\},[[:space:]]*key:[[:space:]]*.\{8\}' | awk '{print $NF}'; kill -2 $(pgrep -P $$ adb); }
```

While running the above, open the app and toggle a light on and off. The command should then output your mesh key.

## Acknowledgments

This component builds upon the reverse engineering and hard work of several others who must be acknowledged and thanked:

### Protocol Reverse Engineering

The foundational protocol reverse engineering work was done by [Mooody](https://mooody.me/posts/2023-04/reverse-the-fastcon-ble-protocol/), who provided detailed analysis of the Fastcon BLE protocol, including packet structure and encryption methods. https://mooody.me/posts/2023-04/reverse-the-fastcon-ble-protocol/

### Implementation References

- [ArcadeMachinist's brMeshMQTT](https://github.com/ArcadeMachinist/brMeshMQTT) - This work was crucial in helping me understand the practical implementation details of the protocol. https://github.com/ArcadeMachinist/brMeshMQTT

### Community Resources

- [Home Assistant Community Thread](https://community.home-assistant.io/t/brmesh-app-bluetooth-lights/473486/102)

This ESPHome component adapts and/or takes heavy inspiration from all of these works to run directly on ESP32 devices, allowing for native integration with Home Assistant without requiring additional bridges or MQTT brokers. A huge thank you to all those who contributed to my understanding of the Fastcon BLE protocol.

## License

This project is licensed under the MIT License - see the LICENSE file for details.
