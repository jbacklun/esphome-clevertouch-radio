# Changelog

## 1.0.0 (2026-11-25)

First public release. Controls CleverTouch (Touch E3) radiators without the E3:

- Climate entity per radiator (off / heat with comfort, eco and away presets /
  auto with a week schedule), room temperature, heating power, energy, radio
  signal, front panel limit, preset temperatures as numbers.
- Week schedule kept and run on the ESP, also without Home Assistant or
  Wi-Fi; editable as day texts or with a Home Assistant Schedule helper
  (blueprint). Temporary changes in auto last until the next switch point; a
  timer set on the radiator for its time.
- Works with radiators paired to an E3 (no re-pairing), and pairs new ones.
- Listens before it transmits: never while an E3 is active (heard itself, or
  noticed by a radiator acknowledging an answer the ESP didn't send); takes
  over the E3's state when it goes away.
- Several ESPs can each control their own set of radiators: another
  controller only counts when it talks to one of this ESP's radiators.
- Frequency calibration for CC1101 modules' crystal offsets, from the
  radiators' packets only.
- `listen_after_boot` (default 5 min; `0s` without an E3) and `ignore:` (device
  IDs not to list as unconfigured).
- `control_radiators: DEFAULT_ON / DEFAULT_OFF`: the "Control radiators" switch
  right after the first flash; afterwards it remembers its last state.
- Temperature offset per radiator (the E3's "offset"), taken over from an E3.
- Optional min/max temperature per radiator.
- `panels: 2` for double-panel radiators: energy under a front panel limit
  counted as 40 % front panel (limited) + 60 % back panel (an estimate from
  the radiator manual, not verified), and a "Sequential control" switch (front
  panel first, back panel as a boost).
- Pairing switch for every radiator (pairing again), which refuses a radiator
  configured with another ID or under another name.
- "Reporting" sensor per radiator: off after 20 minutes without a report.
- `temperature_unit: F`: the YAML temperatures, schedule texts, "Schedule
  status", temperature numbers (in whole °F) and room sensor in Fahrenheit.
- 12-hour times: `7:00AM` / `6:15PM` always accepted in schedules;
  `time_format: 12h` shows them that way in the texts and "Schedule status"
  (the blueprint has a matching "Time format" input).
- "Controller ID" sensor: the ID used for pairing and where it came from
  (YAML, learned from the radiators, or made from the MAC).
- "Unconfigured devices heard" lists unknown radiators and other devices, and
  frames from unknown devices are logged in readable form, for adding support.
- Energy survives restarts: written to flash after 10 Wh or 10 minutes and
  before a planned restart (a power cut can lose up to that much).
- Radiator reports in an unknown layout are logged and never answered.
- Tests of the schedule parser, units and radio frame helpers (C++) and of the
  YAML validation (Python), run by GitHub Actions together with a formatting
  check and builds of the configs.
