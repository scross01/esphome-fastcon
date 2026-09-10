# brMesh / FastCon temporary group protocol notes

These notes document behavior observed while reverse engineering group control from the Android brMesh app. They are implementation notes, not an official protocol specification.

## Temporary group selector

Observed inner payload form:

```text
45 FD <start-id> <nonce-low> <nonce-high> <mask> 00 ...
```

The selector payload is 18 bytes long.

Interpretation used by the implementation:

- `FD`: temporary group ID
- `start-id`: first lamp ID in a contiguous run
- `nonce`: low 16 bits of a millisecond counter
- `mask`: one bit per consecutive lamp
  - bit 0 = `start-id`
  - bit 1 = `start-id + 1`
  - ...
  - bit 7 = `start-id + 7`

Observed examples:

```text
start 1,  mask 3F -> six lamps: 1..6
start 7,  mask 1F -> five lamps: 7..11
start 12, mask 3F -> six lamps: 12..17
start 18, mask 07 -> three lamps: 18..20
```

### Long selector BLE wrapper

The 18-byte selector cannot use the normal FastCon RF/CRC wrapper without exceeding the 31-byte legacy BLE advertising buffer used by the controller.

The Android HCI trace showed a separate encoding path. The implementation uses this conceptual buffer before whitening:

```text
[15 x 00] [A5 5A] [encrypted FastCon body]
```

The entire buffer is whitened using seed `0x25`, then only bytes from offset 15 onward are transmitted. With the 22-byte encrypted selector body, the resulting advertised FastCon data is 24 bytes.

## Group on / off / brightness

Observed inner payload:

```text
43 2A A8 FD <value> 00 00 00 00 00 00 00
```

Observed values:

```text
80 = on
00 = off
3E = approximately 50% brightness
```

Brightness uses the low 7 bits, mapped to approximately 1..127.

## Group cold / warm white

Observed inner payload form:

```text
93 2A A8 FD <on|brightness> 00 00 00 <warm> <cold> 00 00
```

Examples captured around 50% brightness:

```text
Warm:   93 2A A8 FD BE 00 00 00 FF 00 00 00
Middle: 93 2A A8 FD BE 00 00 00 7F 80 00 00
Cold:   93 2A A8 FD BE 00 00 00 00 FF 00 00
```

`BE = 0x80 | 0x3E`, supporting the interpretation:

- high bit: light on
- low 7 bits: brightness
- warm/cold bytes: linear CWWW mix

The implementation maps 153–500 mired linearly to cold/warm byte pairs.

## Transmission sequence

Each group state change sends:

1. temporary group selector
2. group control command

This is substantially faster than queuing individual commands for every lamp.

## Known limitations

- No acknowledgement/state feedback from the bulbs.
- RGB group control has not been implemented.
- Non-contiguous lamp IDs are not supported by this selector model.
- An 8-bit mask implies up to 8 consecutive IDs, but only 3-, 5- and 6-lamp groups have been tested.
- This protocol is reverse engineered and may differ across firmware/app variants.
