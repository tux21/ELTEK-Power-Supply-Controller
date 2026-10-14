# Flatpack2 CAN protocol (as used by this controller)

This is a summary of the community reverse-engineered protocol, limited to
the messages this controller uses. The sources are listed at the end.

## Bus

- 125 kbit/s, **29-bit extended IDs**.
- The bus is referenced to the Flatpack's **DC minus**. The controller's GND
  must be connected to it.
- All multi-byte values are **little-endian** (low byte first).
- Units: current in **dA** (0.1 A), DC voltage in **cV** (0.01 V), AC voltage
  in V, temperatures in °C.

## Messages from the Flatpack

### Hello: `0x0500xxxx`
Sent about every 2 s while the unit is **not** logged in. `xxxx` is usually
the last 4 digits of the serial number.

| Byte | 0 | 1–6 | 7 |
|---|---|---|---|
| | `0x1B` | serial number (BCD, 6 bytes) | varies |

The serial number `060371019754` is sent as `06 03 71 01 97 54`.

### Login request: `0x05ii4400`
Sent about every 10–15 s by a logged-out unit, where `ii` is its preset ID.

| Byte | 0–5 | 6–7 |
|---|---|---|
| | serial number | `00 00` |

### Status: `0x05ii40ss`
Sent every 200 ms while logged in. `ii` is the assigned ID.

| `ss` | Meaning |
|---|---|
| `0x04` | normal, constant voltage |
| `0x08` | current limiting (CC) / warning |
| `0x0C` | alarm |
| `0x10` | walk-in (soft start) |

| Byte | 0 | 1–2 | 3–4 | 5–6 | 7 |
|---|---|---|---|---|---|
| | intake temp °C | output current dA | output voltage cV | AC input V | exhaust temp °C |

### Alarm reply: `0x05iiBFFC`

| Byte | 0 | 1 | 2 | 3 | 4 | 5–6 |
|---|---|---|---|---|---|---|
| | `0x0E` | `0x04` warnings / `0x08` alarms | `00` | flags 1 | flags 2 | `00 00` |

| Bit | Flags 1 | Flags 2 |
|---|---|---|
| 0 | OVS lock-out | Internal voltage |
| 1 | Module fail primary | Module fail |
| 2 | Module fail secondary | Module fail secondary |
| 3 | High mains | Fan 1 speed low |
| 4 | Low mains | Fan 2 speed low |
| 5 | High temp | Sub module 1 fail |
| 6 | Low temp | Fan 3 speed low |
| 7 | Current limit | Inner voltage |

## Messages to the Flatpack

### Log in: `0x050048xx`
`xx` = ID × 4. For example, `0x05004804` gives the unit ID 1, and
`0x05004808` gives it ID 2. IDs 1–63 are allowed.

| Byte | 0–5 | 6–7 |
|---|---|---|
| | serial number | `00 00` |

The unit logs out after about **13 s** (64 × 0.2 s) without a login and goes
back to its default voltage with no current limit. This controller logs in
every **2 s**.

### Set voltage and current: `0x05FF4004` (5 s walk-in) or `0x05FF4005` (60 s walk-in)
`FF` = broadcast to all logged-in units. The settings are lost when the unit
logs out.

| Byte | 0–1 | 2–3 | 4–5 | 6–7 |
|---|---|---|---|---|
| | current limit dA | "measured" voltage cV (= target) | target voltage cV | OVP voltage cV |

Example: 20.0 A, 55.20 V, OVP 57.50 V is `C8 00 90 15 90 15 76 16`.

The current limit is per unit. The voltage range of the 48 V models is
43.5–57.6 V. The unit does not regulate current below about 47–48 V output.

### Alarm request: `0x05iiBFFC`

| Byte | 0 | 1 | 2 |
|---|---|---|---|
| | `0x08` | `0x04` warnings / `0x08` alarms | `00` |

### Set default voltage: `0x05ii9C00` (optional)

| Byte | 0 | 1 | 2 | 3–4 |
|---|---|---|---|---|
| | `0x29` | `0x15` | `0x00` | voltage cV |

The voltage is stored permanently and is used whenever no controller is
logged in. This only works on revision 3.0 and newer units. Some projects
send it as `0x05009C00`, so the controller sends both forms.

## Sequence used by this controller

1. Wait for a **hello** or **login request**, and read the serial number.
2. Give the unit the next free ID (1–4) and send **log in** plus **set
   voltage and current** at once.
3. Every 2 s, send **log in** for each known unit, then one broadcast **set
   voltage and current**.
4. Decode the **status** messages. If one shows an alarm (`0x0C`), send an
   **alarm request** (at most every 5 s) and print the reply.
5. If a known unit sends **hello** again, it has logged out (for example
   after a grid outage), so log it in immediately.

## Sources

- [the6p4c/Flatpack2: Protocol.md](https://github.com/the6p4c/Flatpack2/blob/master/Protocol.md)
- [neggles/flatpack2s2: Protocol.md](https://github.com/neggles/flatpack2s2/blob/main/docs/Protocol.md)
- [taHC81/Eltek-Flatpack2-ESPhome](https://github.com/taHC81/Eltek-Flatpack2-ESPhome)
- [Endless-sphere: Eltek programming thread](https://endless-sphere.com/sphere/threads/ultra-compact-1800w-charger-eltek-programming.71139/)
