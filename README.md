# Xbox 360 PC Wireless Gaming Receiver for PS5 (`.elf`)

> **Built at home to get my wireless Xbox 360 guitars back to work on PlayStation 5 — free and open source for everyone to use!**

A jailbroken PlayStation 5 ELF payload (`Xbox360Receiver.elf`) that lets you plug a **Microsoft Xbox 360 PC Wireless Gaming Receiver** (and compatible third-party USB clones) into your PS5 to connect up to **4 wireless Xbox 360 controllers or guitars**.

You can use this `.elf` to play **Guitar Hero** with your wireless Xbox 360 guitar on **PS5X360** (Xbox 360 emulator for PS5) or **Porpoise / Dolphin** (Wii & GameCube emulator for PS5), as well as any native PS5 game!

Built with the actively maintained [`ps5-payload-dev/sdk`](https://github.com/ps5-payload-dev/sdk) and powered by [PoorDS4](https://github.com/ItsBlurf/PoorDS4)'s `libScePad` game-side import bridge.

---

## Features

- **Xbox 360 Wireless Guitars & Gamepads on PS5:** Connect up to **4 wireless Xbox 360 controllers/guitars** through a single PC wireless USB receiver (automatic Player 1–4 LED ring assignment).
- **Works with PS5X360 & Porpoise (Dolphin) & PS5 games:** Automatically attaches to the running PS5 app/emulator (`eboot.bin`) and bridges inputs cleanly.
- **Strum-Gated Guitar Hero Mode (`Xbox360Receiver-guitar.elf`):** Optional live-toggleable remap mode designed for playing Guitar Hero with an Xbox 360 wireless guitar in gamepad-mode emulators (such as `PS5X360`):
  - Holding frets + **Strum Up/Down** fires the remapped fret notes, holds sustains until you lift your fingers, and pulses cleanly on re-strum for repeated notes.
  - Built-in menu shortcuts so you don't need to switch back to the DualSense: **Back + Green** = Confirm (`Cross`/`A`), **Back + Red** = Cancel (`Circle`/`B`), **Strum Up/Down** = scroll menus.
- **Simultaneous DualSense + Xbox 360 support:** Your PS5 DualSense keeps working in-game alongside your Xbox 360 controller/guitar at all times.
- **Official & third-party clone receiver support:** Matches Microsoft `045e:0719`, `045e:0291`, `045e:02a9`, `045e:02aa`, `045e:02a1`, `045e:02a0`, plus automatic USB interface descriptor matching (`class 0xFF / subclass 0x5D / protocol 0x81`) for other clones.
- **Hot-plug & rumble support:** Plug/unplug the receiver or power guitars/controllers on/off anytime; forwards PS5 game vibration to Xbox 360 controller motors.

---

## Included `.elf` Payloads

Download prebuilt `.elf` files from the [Releases](../../releases) page (or compile from source):

| Payload | Purpose |
| --- | --- |
| `Xbox360Receiver.elf` | Main background payload — reads the USB wireless receiver and bridges up to 4 controllers/guitars into running PS5 games/emulators. |
| `Xbox360Receiver-guitar.elf` | Toggles **Strum-Gated Guitar Hero Mode** ON/OFF live (no restart needed). |
| `Xbox360Receiver-status.elf` | Prints current receiver, controller slots, guitar mode, and game-bridge status. |
| `Xbox360Receiver-stop.elf` | Cleanly detaches from the running game and stops the background payload. |

---

## Quick Start (Prebuilt `.elf`)

1. Plug your **Xbox 360 PC Wireless Gaming Receiver** into a PS5 USB port and sync your wireless Xbox 360 guitar/controller using the pairing buttons on the receiver and controller.
2. Send `Xbox360Receiver.elf` to your jailbroken PS5's `elfldr` (default port `9021`):
   ```bash
   nc -w 8 <your-ps5-ip> 9021 < Xbox360Receiver.elf
   ```
3. *(Optional, for Guitar Hero in PS5X360)* Enable **Guitar Hero Mode**:
   ```bash
   nc -w 8 <your-ps5-ip> 9021 < Xbox360Receiver-guitar.elf
   ```
4. Launch **PS5X360**, **Porpoise (Dolphin)**, or any native PS5 game using your DualSense (the PS5 Home Screen itself does not react to Xbox 360 controllers — input starts automatically once the game/emulator launches).
5. To stop the payload cleanly at any time, send `Xbox360Receiver-stop.elf`.

Runtime logs and live status are saved to `/data/x360w/bridge.log` and `/data/x360w/supervisor.txt` on the PS5.

---

## Button Mapping

### Standard Gamepad Mode (`guitar_mode=0`)

| Xbox 360 | PS5 (`ScePadData`) |
| --- | --- |
| A / B / X / Y | Cross / Circle / Square / Triangle |
| LB / RB | L1 / R1 |
| LT / RT | Analog L2 / R2 (+ digital L2 / R2) |
| Left / Right Stick | Left / Right Stick (10% deadzone, Y-axis inverted to PS5) |
| Left / Right Stick Click | L3 / R3 |
| D-Pad | D-Pad |
| Start | Options |
| Back | Touchpad Click |
| Guide + Back | Create / Share |

### Strum-Gated Guitar Hero Mode (`guitar_mode=1`)

| Xbox 360 Guitar | PS5 / Gamepad GH Output |
| --- | --- |
| **Green Fret (`A`) + Strum** | **`L2` (`LT`)** |
| **Red Fret (`B`) + Strum** | **`L1` (`LB`)** |
| **Yellow Fret (`Y`) + Strum** | **`R1` (`RB`)** |
| **Blue Fret (`X`) + Strum** | **`R2` (`RT`)** |
| **Orange Fret (`LB`) + Strum** | **`Cross` (`A`)** |
| **Strum Up / Down** | **Strum trigger + D-Pad Up / Down** |
| **Back + Green** | **Confirm (`Cross` / `A`)** *(menus)* |
| **Back + Red** | **Cancel / Back (`Circle` / `B`)** *(menus)* |
| **Start** | **Options (Pause)** |
| **Back (alone)** | **Touchpad (Star Power / Select)** |
| **Whammy / Tilt** | **Right Stick X / Y** |

---

## Building from Source

Requires the actively maintained [`ps5-payload-dev/sdk`](https://github.com/ps5-payload-dev/sdk). Set `PS5_PAYLOAD_SDK` to your SDK path:

```bash
# Run host-native protocol & Guitar Hero mode unit tests
make test

# Build stripped release ELFs in build/
PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk make release

# Deploy directly to PS5 elfldr (port 9021)
make deploy PS5_HOST=<your-ps5-ip>
```

---

## License & Credits

Free to use, modify, and share under **GPL-3.0-or-later**. See [LICENSE](LICENSE) and [NOTICE.md](NOTICE.md).

- Game-side `libScePad` import bridge derived from **[PoorDS4](https://github.com/ItsBlurf/PoorDS4)** by ItsBlurf (GPL-3.0-or-later).
- Built with **[ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk)**.
