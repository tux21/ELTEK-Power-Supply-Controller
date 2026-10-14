# ELTEK Power Supply Controller

An Arduino controller that turns an **Eltek Flatpack2 48 V** rectifier into a
**CC/CV charger for a 16S LiFePO4 battery**.

Plug the Flatpack into the grid and the Arduino finds it on the CAN bus.
It sets 55.2 V with a current limit, and the Flatpack charges the battery
with constant current, then constant voltage. When the battery is full, the
controller drops it to a float voltage.

Out of the box a Flatpack2 gives 53.5 V. That is too low to fully charge a
16S LiFePO4 pack, and the output voltage can only be changed over CAN.

> **Status:** v1.0.0. It compiles for ATmega168 and ATmega328P and passes
> the PC simulation tests in [`tests/`](tests/). Do the
> [first-run check](#first-run-without-the-battery) before connecting a battery.

---

## Features

- **Finds Flatpacks by itself.** It reads each unit's serial number from the
  bus, so there is nothing to type in. Up to 4 units can share one bus.
- **CC/CV charging** at 55.2 V (3.45 V/cell) with an adjustable current
  limit. The limit is the total for the battery and is split between the
  running Flatpacks.
- **Float** at 53.6 V (3.35 V/cell) once the current has stayed below 2 A
  for 10 minutes. A new charge starts if the battery takes more than 5 A for
  a minute.
- **12-hour safety timer.** If the battery never gets "full" (for example
  because of a constant load), the controller switches to float and stays
  there until the Flatpack restarts.
- **Over-voltage limit** of 57.5 V sent to the Flatpack.
- **Alarm decoding.** Flatpack alarms such as high mains or fan failure are
  printed by name.
- Status on the **Serial Monitor**, plus an optional **status LED**.
- Fits an **ATmega168** (11 KB flash, 406 B RAM).

## Hardware

| Part | Notes |
|---|---|
| Arduino Nano or Pro Mini, **5 V / 16 MHz** | ATmega328P or ATmega168 |
| MCP2515 CAN module | The common blue board with MCP2515 + TJA1050. A transceiver on its own (TJA1050, SN65HVD230) does **not** work, because the ATmega has no CAN controller. |
| 5 V supply | See [Powering the Arduino](#powering-the-arduino) |
| LED + 1 kΩ resistor | Optional |

### Wiring

| MCP2515 module | Arduino |
|---|---|
| VCC | 5V |
| GND | GND |
| CS | D10 |
| SO | D12 |
| SI | D11 |
| SCK | D13 |
| INT | not used |

| Connection | To |
|---|---|
| MCP2515 **CAN-H / CAN-L** | Flatpack **CAN-H / CAN-L** (if nothing is received, swap them; that is harmless) |
| Arduino **GND** | Flatpack **DC minus** |
| Optional LED | D4 → LED → 1 kΩ → GND |

> ⚠️ **The Arduino GND must be connected to the Flatpack DC minus.** The
> Flatpack's CAN bus is referenced to its negative output rail, and without
> this wire the transceiver gets damaged.

- Leave the **J1 jumper** (120 Ω termination) on the MCP2515 module fitted.
- Check the crystal on the MCP2515 module. **8.000** is the default. For
  **16.000**, change `MCP_8MHZ` to `MCP_16MHZ` in the settings.

### Powering the Arduino

You have two options:

1. **A 5 V USB phone charger on the same mains socket as the Flatpack**
   (recommended). The Arduino then only runs when the grid is on, and it
   never drains the battery.
2. **A DC-DC buck from the Flatpack output.** It must be rated for
   **at least 80 V input**, for example an XL7015 module (5–80 V). LM2596
   boards are only rated for 40 V and will fail. A non-isolated buck ties the
   grounds together by itself.
   With the battery connected, the output terminals always carry battery
   voltage, so the Arduino stays on and draws about 5–10 mA from the pack.

The Arduino does not have to start before the Flatpack. A Flatpack that is
not logged in announces itself every 2 seconds, and the controller logs in
on the next announcement.

## Installing (Arduino IDE)

1. Install the [Arduino IDE](https://www.arduino.cc/en/software).
2. Open **Tools → Manage Libraries**, search for **`mcp_can`** and install
   the one **by coryjfowler**. Other CAN libraries with similar names will
   not compile with this sketch.
3. Open `Flatpack2_Charger/Flatpack2_Charger.ino`.
4. Select your board under **Tools → Board → Arduino AVR Boards**:

   | Board | Board menu | Processor |
   |---|---|---|
   | Nano with ATmega328P | Arduino Nano | ATmega328P, or **ATmega328P (Old Bootloader)** for most clones |
   | Nano with ATmega168 | Arduino Nano | ATmega168 |
   | Pro Mini 328P | Arduino Pro or Pro Mini | ATmega328P (5V, 16 MHz) |
   | Pro Mini 168 | Arduino Pro or Pro Mini | ATmega168 (5V, 16 MHz) |

   A Pro Mini needs a USB-serial adapter to upload.
5. Click **Upload**.
6. Open **Tools → Serial Monitor** and set it to **115200 baud**.

## First run, without the battery

1. Connect the Arduino and the CAN wires, but **leave the battery disconnected**.
2. Plug the Flatpack into the grid.
3. The Serial Monitor should show `>> Found Flatpack, serial ...` with the
   number from the Flatpack's label, then a status line every 2 seconds.
4. Measure the Flatpack output with a multimeter. It should read **55.2 V**.
5. Only then connect the battery.

If it says `Nothing received on CAN yet`, check, in this order: the grid
power, CAN-H/CAN-L (try swapping them), the GND to DC minus wire, and the
crystal setting.

### Example output

```
=== Flatpack2 LiFePO4 charger v1.0.0 ===
Charge 55.20 V, max 20.0 A, float 53.60 V, OVP 57.50 V
CAN ready (125 kbit/s). Waiting for Flatpack...
>> Found Flatpack, serial 060371019754 -> FP1
== Flatpack running - CHARGING started
CHARGING set 55.20 V / 20.0 A | FP1: 50.00 V 20.0 A CC AC 230 V 25/38 C
CHARGING set 55.20 V / 20.0 A  (tail 6/10 min) | FP1: 55.20 V 1.5 A CV AC 230 V 25/38 C
== Battery FULL - switching to float
FLOAT    set 53.60 V / 20.0 A | FP1: 53.58 V 0.3 A CV AC 230 V 25/38 C
```

Each status line shows the stage, the target voltage and current, and for
each Flatpack its output voltage and current, its mode (`CC`, `CV`,
`walk-in` or `ALARM`), the AC input voltage, and its intake and exhaust
temperatures.

## Settings

All settings are at the top of `Flatpack2_Charger.ino`. Only change the numbers.

| Setting | Default | Meaning |
|---|---|---|
| `CHARGE_VOLTAGE` | 55.20 V | CC/CV charge voltage (3.45 V/cell) |
| `MAX_CURRENT` | 20.0 A | **Total** charge current, split between all running Flatpacks |
| `ENABLE_FLOAT` | true | `false` = stay at the charge voltage when full |
| `FLOAT_VOLTAGE` | 53.60 V | Float voltage (3.35 V/cell) |
| `TAIL_CURRENT` | 2.0 A | The battery counts as full when the current stays below this... |
| `TAIL_MINUTES` | 10 | ...for this many minutes at the charge voltage |
| `REBULK_CURRENT` | 5.0 A | In float, more than this for 1 minute starts a new charge |
| `MAX_CHARGE_HOURS` | 12 | Safety timer (0 = off) |
| `OVP_VOLTAGE` | 57.50 V | The Flatpack shuts itself down above this |
| `LONG_WALK_IN` | false | Soft start: `false` = 5 s, `true` = 60 s |
| `WRITE_DEFAULT_VOLTAGE` | false | Permanently store `DEFAULT_VOLTAGE` in the Flatpack. This only works on revision 3.0 and newer units, and leaves them with no current limit when no controller is connected. |
| `CAN_CRYSTAL` | `MCP_8MHZ` | Crystal on the MCP2515 board |
| `LED_PIN` | 4 | Status LED pin (not 13, which is used by SPI) |

The sketch refuses to compile if a voltage is outside the Flatpack's
43.5–57.6 V range, or if the OVP is less than 1 V above the charge voltage.

## How it works

| Stage | Voltage | Ends when | LED |
|---|---|---|---|
| **WAITING** | none | a Flatpack reports status | short blip every 2 s |
| **CHARGING** | `CHARGE_VOLTAGE` | the current stays below `TAIL_CURRENT` for `TAIL_MINUTES`, or `MAX_CHARGE_HOURS` passes | blinks at 1 Hz |
| **FULL / FLOAT** | `FLOAT_VOLTAGE` | the battery takes more than `REBULK_CURRENT` for 1 min (not after a timeout) | solid |

A Flatpack alarm makes the LED flicker fast, and the alarm names are
printed on the Serial Monitor.

Every 2 seconds the controller logs in to each Flatpack and sends the
setpoint. If no Flatpack reports for 30 seconds (grid off), it goes back to
WAITING, and the next start is a new charge.

The CAN messages are described in [`docs/PROTOCOL.md`](docs/PROTOCOL.md).

## Limitations and safety

- **If the Arduino stops or is disconnected**, the Flatpack returns to its
  own default after about 15 s: 53.5 V with **no current limit** (up to
  33.6 A on the 1800 W model, 41.7 A on the 2000 W HE). The same happens for
  the first few seconds after the Flatpack is plugged in, before the
  controller logs in. 53.5 V is safe for LiFePO4, but the pack and BMS must
  handle that current.
- **The current limit only works above about 47–48 V output.** A pack below
  about 3.0 V/cell can get the Flatpack's full current until it comes up.
- **This is not a BMS.** Cell-level protection must come from a BMS such as
  a JK BMS.
- The per-unit current is capped at **33.6 A** (the 1800 W model), which is
  safe for all units.
- The Flatpack CAN protocol is reverse-engineered by the community, not
  published by Eltek.

## Compatibility

- **Flatpack2 48V 1800 W** (part 241115.001, version 1.4, non-HE): the
  hardware this was written for. The same CAN commands are reported to work
  on 1800 W non-HE units.
- **Flatpack2 HE 48/2000, 48/3000**: the units the protocol was worked out on.
- **Flatpack S**: reported command-compatible.

## Tests

[`tests/`](tests/) runs the real sketch on a PC against a simulated
Flatpack2. A fake CAN driver and clock let it check the exact bytes sent and
every charge stage, including a 12-hour safety-timer run.

```sh
cd tests
./run_tests.sh        # summary
./run_tests.sh -v     # also show the sketch's serial output
```

This needs `g++` (Linux, macOS, or WSL on Windows). It currently gives
**39 passed, 0 failed**.

## Credits

The protocol information comes from:

- [the6p4c/Flatpack2](https://github.com/the6p4c/Flatpack2): protocol notes and the first Arduino sketch
- [neggles/flatpack2s2](https://github.com/neggles/flatpack2s2/blob/main/docs/Protocol.md): extended protocol notes and alarm bits
- [taHC81/Eltek-Flatpack2-ESPhome](https://github.com/taHC81/Eltek-Flatpack2-ESPhome): automatic serial number detection
- [tomvanklinken/Flatpack2](https://github.com/tomvanklinken/Flatpack2): multi-unit login
- [Endless-sphere: Eltek programming thread](https://endless-sphere.com/sphere/threads/ultra-compact-1800w-charger-eltek-programming.71139/)
- [coryjfowler/MCP_CAN_lib](https://github.com/coryjfowler/MCP_CAN_lib): the CAN library
