# brMesh / FastCon broadcast (all lamps) protocol notes

These notes document commands captured from the Android brMesh app while controlling
a group of lamps. They are observations, not an official specification.

## How it was captured

The brMesh app logs every command before encryption. With USB debugging enabled:

```bash
adb logcat -c && adb logcat | grep --line-buffered -iE "jyq_helper|getPayloadWithInner"
```

Group actions in the app then show up as `getPayloadWithInnerRetry---> payload:...` lines.

## Observed commands

Unlike the temporary group selector used by `fastcon_group_light` (`45 FD ...` followed by a
control command to target `FD`), the app sent a **single command** to target `00`, without
any selector. All lamps in the mesh reacted.

```text
43 2A A8 00 00                       off
43 2A A8 00 80                       on (previous state)
43 2A A8 00 0A                       brightness 10/127
43 2A A8 00 7F                       brightness 127/127
93 2A A8 00 FF 00 FF 04 00 00        red    (brightness 0x7F, B=00, R=FF, G=04)
93 2A A8 00 FF 00 00 FF 00 00        green  (B=00, R=00, G=FF)
93 2A A8 00 FF FF 00 00 00 00        blue   (B=FF, R=00, G=00)
93 2A A8 00 FF 00 00 00 7F 7F        white  (warm=7F, cold=7F)
```

Interpretation:

- Byte 0: high nibble = number of data bytes + 3 (`0x4_` for one data byte, `0x9_` for six), low nibble `3`.
- Bytes 1-2: `2A A8`, same as in the group control command.
- Byte 3: target. `00` addressed every lamp in the mesh in our test.
- Color/white command data: `<0x80 | brightness(7 bit)> <blue> <red> <green> <warm> <cold>`.
  This is the same layout as the CWWW group command documented in `GROUP_PROTOCOL.md`,
  with the three color bytes filled in.

The outer packet (type 5, sequence number, last mesh-key byte, checksum, encryption and BLE wrapper)
is identical to single-light commands; the `send--->` log lines confirm this.

## Implementation notes

- **Power-on transition:** the lamps switch OFF→ON only via the 1-byte
  power command (`43 2A A8 00 80`). The 6-byte color/white command
  (`93 ...`) adjusts color/brightness but does not trigger the
  power-on transition. `fastcon_broadcast_light` therefore sends the
  1-byte power command when turning on, then the color/white command.
- **All-zero fallback:** when turning on with no color set (all RGB
  channels zero), the implementation sends a warm-white command
  (non-zero warm/cold) so the lamp is visible, mirroring the
  single-light fallback in `fastcon_controller.cpp`.
- **Outer body[0] lightness nibble:** the low 4 bits of the outer
  `body[0]` byte carry a lightness value. The app uses lightness=100
  (0x64), so a forward color/white command has `body[0] = 0xD4`
  (`0xD0 | 0x04`). The implementation matches this.
- **Device type:** the type bytes (`2A A8` = RGBCW, 43050) are
  configurable via the optional `device_type` key. Other FastCon
  types: RGB=43168 (0xA8A0), RGBW=43169 (0xA8A1), CCT=43051 (0xA82B).

## Test setup

- 5 brMesh RGB + warm/cold white ceiling lamps, all in one mesh (IDs 1-5)
- ESP32-S3 (N16R8), ESPHome 2026.9.1, Arduino framework
- `adv_interval_min: 0x20`, `adv_interval_max: 0x30`, `adv_duration: 350`, `adv_gap: 10`

## How this was made

The commands were captured and analysed, and the `fastcon_broadcast_light` platform was implemented,
with the help of Claude Code. The contributor is not very experienced with ESP32/ESPHome and shares these
findings so others can build on them.

## Known limitations / open questions

- No acknowledgement or state feedback from the lamps.
- It is not verified whether target `00` always means "all lamps", or whether it refers to an
  app-side group that happened to contain all lamps. With all lamps in one mesh it addressed all of them.
- Only tested with one mesh of 5 lamps.
