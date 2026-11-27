# Hardware Documentation

Hardware reference for Blinky Time controllers. The canonical list of supported devices is the device registry, [`devices/registry/*.json`](https://github.com/Jdubz/blinky_time/blob/master/devices/registry) in blinky_time; this doc summarises the recurring patterns.

## Boards: XIAO nRF52840 Sense and Sense Plus

The legacy fleet is the plain **XIAO nRF52840 Sense**; new devices (and the test box chip) are the **Sense Plus**. One bootloader and one firmware image run on both: every pin they use (QSPI flash, PDM mic, IMU, LEDs, reset/DFU, D0–D10) is identical in Seeed's netlists. The Plus adds 9 castellated edge pads (D11–D19). A fresh Plus enumerates as `2886:8065` with its factory app and `2886:0065` in its factory bootloader. Once our bootloader and app are on it, it reports `0x8045`/`0x0045` like a Sense, and the tools accept both. Details: `docs/BOOTLOADER_WORKFLOW.md` §5a.

## Device Configurations (XIAO nRF52840 Sense / Sense Plus)

Current registered devices (see blinky_time's [`devices/registry/`](https://github.com/Jdubz/blinky_time/blob/master/devices/registry)):

| Device | Layout | Notes |
|--------|--------|-------|
| `hat_v1` | 89×1 string | Wearable, battery. The release test device |
| `sourpuss_hat` | 70×1 string | Wearable, battery |
| `shannon_hat` | 79×1 string | Wearable, battery, `ledPin=0` |
| `scarf` | 141×1 string | Wearable, battery, `ledPin=0` |
| `tube_v2` | 4×15 zigzag matrix | Standalone tube |
| `long_tube_v1` | 4×60 zigzag matrix | Sealed sculpture |
| `bucket_v3` | 16×8 matrix | Standalone bucket |
| `big_bucket_v1` | 14×8 matrix | `ledPin=0`, `ledType=6` (RGB), sealed |
| `cart_inner` | 104×1 string | Lemon-cart, two channels (`ledPin=10`, `ledPin2=9`) |
| `cart_outer` | 96×1 string | Lemon-cart |
| `cart_umbrella_v1` | 8×13 matrix | Lemon-cart umbrella |
| `testbox_grid_v1` | 8×8 matrix, orientation 5 (`HORIZONTAL_ROTATED_180`) | Test box chip (Sense Plus 87DE9B58C90AFE2D); a test device, see [Test box LED grid](#test-box-led-grid) |
| `display_v1` | 32×32 matrix | Bench display; ESP32-S3 hardware with no firmware path today |

Default LED data pin is **D10**. `big_bucket_v1`, `scarf` and `shannon_hat` use D0. `cart_inner` adds a second channel on D9 for its split topology.

## Bootloader and flashing

Every fleet device runs bootloader **0.8.0-10** (UF2 drive over USB plus BLE DFU). Firmware goes on only through blinky_time's `./scripts/deploy.sh` (fleet), `tools/deploy_local.py` (a device on your USB) or the browser updater (`docs/WEB_FLASHER.md`). Bootloader changes go only through `./scripts/deploy-bootloader.sh`, on a sacrificial device first. Never use `arduino-cli upload` or `adafruit-nrfutil dfu serial`. See CLAUDE.md and blinky_time's [`docs/SAFETY.md`](https://github.com/Jdubz/blinky_time/blob/master/docs/SAFETY.md).

The nRF52840 Sense also has 2 MB of external QSPI flash. Signed OTA updates stage the new image there while the old firmware keeps running (`blinky-things/ota/QspiOtaStaging.h`).

## Wiring Diagrams

### Basic Connections
```
nRF52840 XIAO Sense Pinout:
├── D10 (GPIO) → LED Data Pin
├── 3V3 → LED Power (VCC)   (small battery strings only; see below)
├── GND → LED Ground
├── Built-in Mic → Audio input
└── Built-in IMU → Motion sensing
```

The XIAO's 3.3 V regulator supplies about 250 mA, enough only for a short, dim string. Larger layouts power the LEDs from their own 5 V supply sized for the LED count, join that supply's ground to the XIAO's GND at one point, and keep only data (with a ~330 Ω series resistor at the strip) and the ground reference on the XIAO.

### Test box LED grid

The test box chip (a XIAO nRF52840 Sense Plus, 87DE9B58C90AFE2D; device config `testbox_grid_v1`, listed in `devices/test_devices.json`) drives an 8x8 WS2812 grid. As wired and checked on the rig (2026-11-23):
- The chip sits in a XIAO Debug Mate with its own USB-C on the hub. Data comes from D10 through the Debug Mate's D10 header pin. In Seeed's Debug Mate V1.0 KiCad netlist every D0–D10 header pin is a direct trace to the socket (nothing else on the net) except D6/D7, which go through the UART analog switch. The header's GND pin is the XIAO's GND net (`XIAO_5V-`, the low side of the Debug Mate's power-meter shunt).
- The grid runs from its own external 5 V supply, not from the XIAO or the Debug Mate.
- 3.3 V data drives this grid directly; no level shifter is needed.
- Wiring order, seen from the front: data enters at the bottom-right LED; each row runs right to left, rows go bottom to top, no serpentine. That is orientation 5, `HORIZONTAL_ROTATED_180` (#46): `HORIZONTAL` (orientation 0) rotated 180°.
- Its device config has brightness 0: the grid is dark at boot and while idle. blinky-server lights it with the runtime `leds` override only while a test runs (`docs/GENERATOR_EFFECT_ARCHITECTURE.md`, "LED brightness override"; blinky_time `docs/TEST_BOX.md`).

### Power Requirements
- **LED Current**: ~60mA per LED at full brightness
- **Controller Current**: ~50mA during operation
- **Battery Recommendation**: 3.7V Li-Po, 1000mAh+ for portable use

## LED Strip Specifications

### Supported Types
- **WS2812B** (NeoPixel) - Most common, NEO_GRB color order
- **SK6812** - Similar to WS2812B with white channel option
- **WS2813** - Backup data line for improved reliability

### Color Order Configuration
Set by `ledType` in the device config (a number; see blinky_time's [`devices/registry/README.md`](https://github.com/Jdubz/blinky_time/blob/master/devices/registry/README.md)):
- `82` = `NEO_GRB + NEO_KHZ800`: standard WS2812B, the fleet default
- `6` = `NEO_RGB`: `big_bucket_v1`, whose LEDs are wired native RGB

RGBW types are rejected: the driver uses 3 bytes per pixel.

### LED output (nRF52840)

`hal/hardware/Nrf52PwmLedStrip` drives each strand from a PWM peripheral over EasyDMA: one 16-bit duty value per LED bit, 1.25 µs each, so a frame takes **30 µs per LED** on the wire (1.9 ms at 64 LEDs, 11.5 ms at 384, 30.7 ms at 1024), plus a 300 µs latch. One pin can't go faster.

- **The loop never waits on the wire (#14).** `show()` copies the frame to a staged buffer (3 B/LED) and wakes a transmit task above the loop. With the wire idle, the task encodes the frame and starts it before `show()` returns. While the previous frame is still going out, the frame waits and the PWM's SEQEND interrupt starts it the moment the wire frees. A newer `show()` replaces a waiting frame, so the wire always carries the newest frame, back to back. `json info`: `showUs`/`showMaxUs` is loop time in `show()`, `ledFps` the frames that began transmitting per second, `ledSuperseded` the frames replaced before reaching the wire (since boot).
- **Strands:** `ledPin2` splits the pixels across two PWM instances that transmit together, halving the wire time. The chip has four PWM instances, one per strand.
- **RAM is the limit on long strips.** The heap costs about **71 B per LED** on top of a fixed base: the 48 B/LED duty pattern, 3 B each for the render buffer, staged copy and pixel matrix, 4 B for the LED map, plus the rest. On the box, `heapUsed` is 157,944 B at 64 LEDs and 180,556 B at 384, against a `heapTotal` of 182,984 B. That puts the ceiling at **about 420 LEDs per chip** today. A 1024-LED strand doesn't fit: the duty pattern alone is 48 KB. Options, outstanding: I2S output at 4 bits per LED bit (12 B/LED, as in Zephyr's `ws2812_i2s`); the renderer writing straight into the output buffer, so the render buffer and pixel matrix stop being duplicates; or a smaller audio footprint.
- **The render is the next limit on long strips.** On the box, loop frames average 5.5 ms at 64 LEDs and 15.7 ms at 384, so each LED costs about 30 µs (generators, effects, mapping, encode). That projects to roughly 30 fps at 1024 LEDs, even though the loop no longer waits on the wire.

## Installation Notes

### Zigzag Matrix Wiring
For tube lights and matrix displays, LEDs are wired in a zigzag pattern:
```
Column 0: LEDs 0-14 (bottom to top)
Column 1: LEDs 29-15 (top to bottom) 
Column 2: LEDs 30-44 (bottom to top)
Column 3: LEDs 59-45 (top to bottom)
```

### Enclosure Considerations
- **Waterproofing**: IP65 rating recommended for outdoor use
- **Heat Dissipation**: Provide ventilation for high LED counts
- **Access Ports**: Programming connector and charging port access

## Troubleshooting

### Common Issues
1. **Wrong Color Order** - Check `ledType` (82 = GRB, 6 = RGB) in the device config
2. **Power Supply** - Insufficient current causes dim/flickering LEDs
3. **Data Signal** - Use proper gauge wire for data connections
4. **Ground Loops** - Ensure common ground between controller and LEDs

### Testing Procedures
1. **Single LED Test** - Verify basic connectivity
2. **Color Order Test** - Check red/green/blue display correctly  
3. **Full Matrix Test** - Validate zigzag mapping pattern
4. **Audio Test** - Confirm microphone responsiveness
