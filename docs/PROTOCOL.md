# CleverTouch (Touch E3) radiator radio protocol

What is known about the 868 MHz link between a Touch E3 controller and its
radiators. It comes from logic-analyser captures of the SPI bus between the
E3's microcontroller and its CC110L radio chip, from listening with a CC1101,
and from tests answering the radiators in place of the E3. Observed with Purmo
Yali Digital single-panel radiators. Watts Vision thermostats use the same kind
of frames (see [Watts Vision](#watts-vision)).

Byte values are hex. "E3" means the controller; this component plays the
same role.

## Radio settings

The E3 writes these to its CC110L once at boot (26 MHz crystal). A CC1101 has
the same register set.

| Setting | Registers | Value |
|---|---|---|
| Frequency | FREQ2/1/0 = 21 65 6a, then FREQ1/0 rewritten to 65 e9 | **868.350 MHz** |
| Modulation | MDMCFG2 = 13 | **GFSK**, no Manchester, 30/32 sync bits |
| Data rate | MDMCFG4/3 = ca 83 | **38.383 kBaud** |
| Deviation | DEVIATN = 34 | 19.04 kHz |
| RX bandwidth | MDMCFG4 = ca | 101.6 kHz |
| Preamble | MDMCFG1 = 22 | 4 bytes |
| Sync word | SYNC1/0 = d3 91 | **0xD391** (the chip default) |
| Packet | PKTCTRL0 = 05, PKTCTRL1 = 08, PKTLEN = ff | variable length, **hardware CRC on**, no whitening, no address filter |
| PA | FREND0 = 10, PATABLE not written | default PATABLE 0xC6 |
| Other | FSCTRL1=06 FSCTRL0=00 MCSM0=18 MCSM1=03 FOCCFG=16 BSCFG=6c AGCCTRL2/1/0=43 40 91 FSCAL3..0=e9 2a 00 1f FREND1=56 TEST2/1/0=81 35 09 | |

CC1101 modules' crystals are often tens of kHz off. With the E3's 102 kHz
filter an uncalibrated module may hear only some senders; the component starts
at 203 kHz, averages the frequency offset of received packets, stores the
correction, and then narrows to 102 kHz.

## Frames

    len | sender (4) | c6 | receiver (4) | command | attributes … | CRC16 (2, LE)

- **CRC:** CRC-16/MODBUS (poly 0x8005 reflected, init 0xFFFF) over everything
  after the length byte, stored low byte first. This is on top of the radio's
  own hardware CRC.
- `c6` is constant in every packet seen.
- **No encryption, no rolling code, no key.** The only thing that changes
  between otherwise identical controller packets is the clock.
- **IDs** are 4 bytes. Radiators seen start `f1`. The E3 shows each radiator's
  ID in its radiator settings ("Device ID") in exactly this form. The
  controller has its own ID, and a radiator only accepts the controller it was
  paired to; any device can send as that ID.

Example, a poll from controller `f14f8b6c` to radiator `f11a6571`
(the length byte `0d` is added by the radio driver):

    0d | f1 4f 8b 6c | c6 | f1 1a 65 71 | 19 00 | 88 dc

## Commands

| Cmd | Direction | Length byte | Meaning |
|---|---|---|---|
| `19` | controller → radiator | 0d | "report now": `19 00` |
| `df` | radiator → controller | 23 | full status report: by itself every ~2.5–4 min (each radiator has its own interval), or right after a `19` |
| `45` | controller → radiator | 38 | answer to every `df`: the desired state and the clock |
| `01` | radiator → controller | 0f | `01 00 0a 47`: acknowledges a `45` |
| `00` | radiator → controller | 0d | join request (pairing, below) |

**One report cycle** is four packets: radiator `df` → controller `45`
(the E3 answers after ~0.12 s) → radiator `01` (~0.48 s after its report) →
radiator `df` again (~0.58 s). The second `df` is not a copy: its heating
counters are already 0 and it shows the state just set, so it confirms.

**Timing.** The radiator works in 0.5 s slots after its report. An answer
within ~0.4 s gets the `01` at a fixed ~0.48 s and then the confirming `df`.
A later answer (tested up to 1 s) is still accepted, but the radiator has
already repeated its report at +0.5 s, and no confirming `df` follows. So the
window is generous; answering within ~0.3 s avoids the repeat.

## Attributes

From the command byte on, the payload is a chain of tag + value. The tag's
top 2 bits give the value length: 00 → 1, 01 → 2, 10 → 3, 11 → 4 bytes. A
`00` tag ends the chain. (The "command" byte is simply the first tag.)

Radiator `df`:

    df 20 30 00 40 | 4b 02 b0 | 8a 02 45 03 | e1 00 64 00 00 | da 00 00 00 00 | 0a 47

Controller `45`:

    45 00 00 | 4b 02 b0 | 3b 00 | 4c 00 00 | 8e 00 00 84 | cc 00 00 00 00 | 97 20 30 04 |
    94 02 46 03 | e1 00 64 00 00 | da 00 00 00 00 | 5b 00 78 | b2 0f 2d 13

| Tag | Value | Meaning |
|---|---|---|
| `4b` | u16 BE | **room temperature in 0.1 °F**: `02 b0` = 68.8 °F = 20.4 °C. Echoed by the controller |
| `8a` (radiator) / `94` (controller) | u16 BE + u8 | **setpoint in 0.1 °F + mode** (below). The controller sends the target in `94`; the radiator confirms in `8a` |
| `df` (radiator) / `97` (controller) | 4 / 3 bytes | 2nd byte bit 0: **changed on the radiator** (below). 3rd byte 00/01 (radiator) ↔ 04/05 (controller): heating off/on |
| `e1` | 4 bytes | `<flags> <limit %> <delivered %> <asked %>`. Flags bit 0 = **sequential control** of a double panel (front panel first, back panel only as a boost 1 °C below the setpoint; the radiator's parameter "SrC"): set by the controller, echoed by the radiator, also when set on its own buttons. **Front panel limit** set by the controller (`64` = 100 %, `46` = 70 %, `37` = 55 %), echoed by the radiator with its heating power. Under a limit, delivered < asked (e.g. `37 64` at full demand) |
| `da` | 4 bytes | `00 c1 00 c2` from the radiator, zeros from the controller: **seconds of heating since the last acknowledged report**, counter 1 at the delivered power, counter 2 at the asked power. Energy = c1 × the radiator's wattage (not sent over the radio) |
| `b2` | 3 bytes | **controller clock** hh mm ss |
| `5b` | u16 | from the controller: minutes left of a running timer, else `00 78` = 120. From the radiator only in the report of a timer set on it (below): minutes chosen |
| `8e` | 3 bytes | `00 00 84` to radiators. For Watts thermostats the first two bytes are the installer's setpoint range in whole °C and the low two bits of the third select the regulating sensor (0 room, 1 floor, 2 floor-limited, 3 air). To radiators, bit 6 of the third byte = adaptive start (ITCS) on (`c4`), bit 5 = pre-heating now (`e4`). Sending `84` is safe |
| `45` (first tag of a `45` frame) | u16 BE | **room temperature offset in 0.1 °F**, set per radiator in the E3: signed (two's complement): +0.5 °C sent as `45 00 09` (0.9 °F), −0.5 °C as `45 ff f7`. The radiator's reported `4b` doesn't change; it presumably shifts its regulation. The E3's min/max temperature settings are not sent at all: they only limit the E3's own menu |
| `0a` | u8 | `47` on all radiators seen; meaning unknown |
| `3b` `4c` `cc` | — | zeros in every radiator capture. For Watts thermostats: `4c` = diagnostic code and warning flags, `cc` = two floor-limit temperatures |

### Modes

The third byte of `94` / `8a`:

| Byte | Mode | Setpoint the E3 sends |
|---|---|---|
| 00 | comfort | the comfort temperature |
| 01 | off | `01 40` = 0.0 °C |
| 02 | frost protection | the frost temperature (e.g. 7.0 °C) |
| 03 | reduced | the reduced temperature |
| 04 | timer | the timer temperature |
| 08 | auto, schedule in a comfort period | the comfort temperature |
| 0b | auto, schedule in a reduced period | the reduced temperature |
| 10 | manual / temporary (seen with Watts thermostats) | |

Bit 3 = auto; the low bits show the level in force (00 comfort, 03 reduced).
The radiator always heats to the setpoint in `94`, whatever the mode: it does
not substitute its own comfort level. So the controller has to keep a
temperature per mode. The mode only selects the symbol on the radiator's
display.

**The week schedule runs in the controller.** At a switch point the E3 polls
the radiator and sends the new state; a radiator in auto without a controller
behaves like comfort.

**Setpoints** are 0.1 °F: 14.5 °C was sent as `02 46` (58.2 °F), and the
radiator reported `02 45` (58.1 °F, exactly 14.5 °C).

### Changes made on the radiator

When someone changes the temperature or mode with the radiator's buttons, the
radiator reports by itself with **bit 0 of the `df` tag's second byte set**
(`df 20 31 …`) and the new setpoint/mode in `8a`. The E3 adopts it and
acknowledges by echoing `97 20 30 …` with the bit cleared; the radiator's next
`df` is back to `20 30`. A controller has to do the same, or it overwrites
every change made on the buttons. The radiator remembers the levels it was
last sent: choosing comfort on its buttons picks the last comfort temperature
it received.

Stepping through the modes sends nothing until the last one is chosen. The
radiator doesn't always take the answer at once: in one test (answers after
~150 ms) it repeated the same report for 4-7 s, bit still set, and answered a
`19` with it too; in another (answers after ~230 ms) it took each one at once.
Such repeats are one change, not several. Choosing **auto** reports mode `0b`
with the last setpoint it took, whatever that was for: 16.0 °C after reduced,
but `8a 01 40 0b` (0.0 °C, with `df 20 31 02 40`) after off. That isn't a
temperature to adopt.

**Timer:** a temperature changed on the radiator in auto becomes a timer (mode
`04`). That report has one more tag, after `8a`, with the minutes chosen (39
bytes instead of 36), here 17.0 °C for 60 minutes:

    df 20 31 00 40 | 4b 02 93 | 8a 02 72 04 | 5b 00 3c | e1 00 64 00 00 | da 00 00 00 00 | 0a 47

The times offered are 1-23 hours and 1-44 days (`f7 80` = 63360 minutes);
"no" reports `5b 00 00`, and the radiator goes back to auto by itself 10-20 s
later. The E3 runs a timer by answering `94` with
mode `04` and the minutes left in `5b`; the radiator's reports then have mode
`04` and the usual 36 bytes. Left unanswered, the radiator ended a 1 h timer by
itself, back to auto.

## Changing a radiator

What the E3 does, and what works: send `19` about once a second until a `df`
comes; answer it with the new setpoint/mode in `94`; repeat the `45` about
once a second until the `01`; check that the next `df` shows the new state.
Typically confirmed in about a second.

## Pairing

1. **Controller beacon**, about once a second, alternating two forms, with a
   counter that goes up by one each time:

       0f <ctrl> c6 f1 10 00 <n> 00 00 03 04 <crc>                 (n odd)
       0f d0 <ctrl bytes 2-4> c6 d0 00 00 <n> 00 00 03 04 <crc>    (n even)

   The receiver is a broadcast (`f1 10 00 nn` / `d0 00 00 nn`). The `d0` form
   is probably for another device family.
2. **Join request** from the radiator (in pairing mode), to the controller:
   `0d <radiator> c6 <ctrl> 00 00 <crc>`
3. **Accept:** `0d <ctrl> c6 <radiator> 01 00 <crc>`. The E3 sends it about
   **530 ms** after the join request; the radiator then sends `01 00 0a 47`
   ~0.47 s later and its first `df` about a second after that. Accepted after
   only ~120 ms, the radiator still paired, but stayed silent until its next
   regular report.
4. The first reports have **mode 08** (new, no mode set) until the controller
   sends one.

No key or secret is exchanged. Wattage, panel type and name, which the E3
asks for during pairing, are not sent over the radio.

**Deleting a radiator in the E3 sends nothing.** The radiator keeps reporting
to the controller's ID; the E3 just stops answering it. To move a radiator to
another controller, pair it again.

An unpaired radiator was seen sending `0f <radiator> c6 00ffffff 01 00 0a 47`
(an acknowledgement-like frame to a broadcast address), probably on entering
pairing mode or on power-up. It sends no `df` reports before it is paired.

## Open-window detection

Tested on one radiator by opening a door: when the room cooled quickly it went
to frost protection by itself and reported

    df 2a 30 02 40 | 4b 02 6b | 8a 01 be 02 | e1 00 64 00 00 | da 00 15 00 14 | 0a 47

that is mode `02`, 7.0 °C, power 0. Against its normal `df 20 30 00 40`, bits 1
and 3 of the first byte and bit 1 of the third byte are set (the latter was also
seen when auto was chosen on the radiator, below); the "changed on the
radiator" bit (2nd byte, bit 0) is not. A `45` with the wanted mode
(comfort) was acknowledged with `01`, but the `df` after it and the next
regular report, four minutes later, still said mode `02`. Eight minutes after
the first one it reported comfort again (in two earlier cases after four). So
the controller doesn't need to do anything: it keeps answering with the wanted
state, and the radiator ends the detection itself. How the E3 answers these
reports hasn't been captured.

## Without a controller

- Radiators keep their last setpoint and mode and carry on reporting on their
  own schedule, each report sent twice about 0.5 s apart. No searching, no
  beacons, no channel change. A radiator left unanswered was still reporting
  after five days.
- The heating counters keep adding up until a report is answered, so nothing
  is lost while the controller is away, as long as it later answers.
- When the E3 powers on, it polls every radiator in turn, then goes back to
  answering their own reports.

## Several nodes

For houses where one radio doesn't reach every radiator. Designed, not built.

Answers are addressed to one radiator and carry only the controller ID, so any
node can transmit as the controller for any radiator, without re-pairing. A
`45` is built from the radiator's `df` (echoed fields) plus controller state
(setpoint/mode `94`, timer `5b`, clock `b2`), so a node that has the state can
answer on its own. To keep the network out of the ~0.12 s answer path:

- One master (also a full node, facing Home Assistant) holds the state and the
  schedules, and pushes setpoints and modes to every node. Not time-critical.
- Every node reports the signal it hears from each radiator; the master gives
  each radiator an order of nodes, best signal first (radio paths are
  reciprocal). Change the order only between report cycles, with hysteresis.
- On a `df`, rank 1 answers at once; rank 2 after ~150 ms if it has not heard a
  `45` to that radiator by then; rank 3 after ~300 ms, and so on. The radiator
  repeats an unanswered report anyway.
- The node that owns a radiator runs its change procedure; the others stay
  quiet for that radiator.
- Pairing through any node: in pairing mode all nodes beacon in turn
  (staggered slots); every node that hears a join request reports radiator ID
  and signal to the master, which tells the best node to send the accept.

## Watts Vision

Published thermostat frames of Watts' Vision wireless system (rtl_433, below)
have the same layout as the frames here: the `c6` byte, the CRC and the tags.
Their device IDs start with `d0`, which matches the E3's second pairing beacon
form, so the Touch E3 may well work with both families: `f1` radiators and `d0`
devices. Only those published thermostat frames were compared. Watts Vision
receivers and central units, and pairing, have not been looked at, and nothing
was tested on Watts Vision hardware.

rtl_433 has a receive-only decoder for Watts Vision thermostats
([watts_vision.c](https://github.com/merbanan/rtl_433/blob/master/src/devices/watts_vision.c),
from [issue #2885](https://github.com/merbanan/rtl_433/issues/2885)). What it
adds for other devices: a status tag `8d` from the device (in place of `df`)
with the same "changed on the device" bit, `5e` = floor / external
temperature, `03` = slot ID in the controller's commands, `0x084c` in a
temperature field = sensor unavailable, and the `8e` / `4c` / `cc` meanings
above. Example thermostat report, decoded with this document's tag rules:

    22 | d0374654 | c6 | d0904d89 | 8d 00 11 01 | 8a 02 84 03 | 4b 02 bf | cc 00 00 00 00 |
    8e 0a 24 93 | 4c 00 00 | 31 0d

setpoint 18.0 °C reduced, room 21.3 °C, setpoint range 10–36 °C, air sensor.

## Unknown

What the constant fields mean (`3b`, the `0a 47` at the end of reports, the
`c6` byte); how a double-panel radiator fills the two counters (the component
assumes counter 2 runs at full power for the back panel and splits the power
40 / 60 front / back, from the radiator manual's output figures); what the
radiator does with the adaptive-start bits in `8e`; which of the status bits
below means "open window".
