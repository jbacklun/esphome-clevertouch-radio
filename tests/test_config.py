"""Tests of the YAML validation in components/clevertouch_radio/__init__.py.

    python tests/test_config.py        (with ESPHome installed: pip install esphome)

Uses the same schedule cases as test_logic.cpp (tests/schedule_cases.txt), so
the compile-time check and the parser on the ESP agree.
"""

import copy
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "components"))

import esphome.config_validation as cv  # noqa: E402

import clevertouch_radio as cr  # noqa: E402

failed = 0
checks = 0


def check(cond, msg):
    global failed, checks
    checks += 1
    if not cond:
        failed += 1
        print("FAIL:", msg)


def raises(fn, *args):
    try:
        fn(*args)
    except cv.Invalid as e:
        return str(e)
    return None


# --- the shared schedule cases ------------------------------------------------
def schedule_ok(unit, text):
    """What the build does with a schedule: the grammar, then the unit's range."""
    try:
        cr.schedule(text)
    except cv.Invalid:
        return False
    lo, hi, _ = cr.UNITS[unit]
    return all(lo <= v <= hi for v in cr._schedule_numbers(text))


n = 0
with open(os.path.join(HERE, "schedule_cases.txt"), encoding="utf-8") as f:
    for line in f:
        line = line.rstrip("\r\n")
        if not line or line.startswith("#"):
            continue
        unit, text, verdict = line.split("|")[:3]
        ok = schedule_ok(unit, text)
        check(ok == (verdict == "ok"), f"'{text}' ({unit}): {'accepted' if ok else 'rejected'}, expected {verdict}")
        n += 1
check(n > 30, f"only {n} cases read")


# --- 12-hour times --------------------------------------------------------------
check(cr._minute_of_day("7:00am") == 420 and cr._minute_of_day("6:15pm") == 1095, "am/pm")
check(cr._minute_of_day("12:00am") == 0 and cr._minute_of_day("12:30pm") == 750, "12am / 12pm")
check(cr._minute_of_day("13:00pm") is None and cr._minute_of_day("0:30am") is None, "bad 12h hours accepted")
check(cr._minute_of_day("18:15") == 1095, "24h")


# --- temperature unit: defaults, ranges, conversion ---------------------------
ENTITY_KEYS = ("room_temperature", "comfort_temperature", "eco_temperature", "away_temperature", "temperature_offset")


def radiator(**kw):
    rc = {cr.CONF_NAME: "Bedroom", cr.CONF_SCHEDULE: ""}
    for key in ENTITY_KEYS:
        rc[key] = {"unit_of_measurement": "°C"}
    rc.update(kw)
    return rc


def config(unit, *radiators):
    return {cr.CONF_TEMPERATURE_UNIT: unit, cr.CONF_RADIATORS: [copy.deepcopy(r) for r in radiators]}


c = cr._apply_unit(config("C", radiator()))["radiators"][0]
check((c["comfort"], c["eco"], c["away"], c["min_temperature"], c["max_temperature"]) == (21, 17, 7, 5, 30),
      f"C defaults: {c}")
c = cr._apply_unit(config("C", radiator(comfort=19.3)))["radiators"][0]
check(c["comfort"] == 19.5, f"C rounded to 0.5: {c['comfort']}")

f = cr._apply_unit(config("F", radiator()))["radiators"][0]
check(f["comfort"] == 21 and f["eco"] == 17 and f["away"] == 7, f"F defaults 70/63/45 F: {f}")
check(f["min_temperature"] == 5 and f["max_temperature"] == 30, f"F limits 41/86 F = 5/30 C: {f}")
check(all(f[k]["unit_of_measurement"] == "°F" for k in ENTITY_KEYS), "F: entities shown in F")
f = cr._apply_unit(config("F", radiator(comfort=69)))["radiators"][0]
check(f["comfort"] == 20.5, f"69 F in 0.5 C steps: {f['comfort']}")

check(raises(cr._apply_unit, config("F", radiator(comfort=21))) is not None, "21 accepted as F")
check(raises(cr._apply_unit, config("C", radiator(comfort=70))) is not None, "70 accepted as C")
check(raises(cr._apply_unit, config("C", radiator(min_temperature=20, max_temperature=20))) is not None,
      "min = max accepted")
check(raises(cr._apply_unit, config("F", radiator(schedule="mon 07:00 20"))) is not None, "schedule 20 accepted as F")
check(raises(cr._apply_unit, config("F", radiator(schedule="mon 07:00 68"))) is None, "schedule 68 F rejected")


# --- unique names and radio IDs ------------------------------------------------
def unique(*radiators):
    return raises(cr._check_unique, {cr.CONF_RADIATORS: list(radiators)})


check(unique({"name": "A", "radio_id": 1}, {"name": "B", "radio_id": 2}) is None, "distinct rejected")
check(unique({"name": "A"}, {"name": "B"}) is None, "no radio_ids rejected")
check(unique({"name": "A"}, {"name": "A"}) is not None, "duplicate name accepted")
check(unique({"name": "A", "radio_id": 1}, {"name": "B", "radio_id": 1}) is not None, "duplicate radio_id accepted")


# --- controller entities, radio IDs ---------------------------------------------
v = cr._fill_hub_entities({"control": {"icon": "mdi:radiator"}, "status": {"internal": True}})
check(v["control"]["name"] == "Control radiators" and v["status"]["name"] == "Controller status",
      f"default names not filled: {v}")
v = cr._fill_hub_entities({"control": {"name": "Radiator control"}})
check(v["control"]["name"] == "Radiator control", "a given name was replaced")
check(raises(cr._fill_hub_entities, {"control": {"restore_mode": "ALWAYS_ON"}}) is not None,
      "restore_mode accepted instead of control_radiators")

check(cr.radio_id("F11A4E62") == 0xF11A4E62, "radio_id parse")
check(cr.radio_id("f11a4e62") == 0xF11A4E62, "radio_id lower case")
check(raises(cr.radio_id, "F11A4E6") is not None, "7 digits accepted")
check(raises(cr.radio_id, "G11A4E62") is not None, "non-hex accepted")

print(f"{checks} checks, {failed} failed")
sys.exit(1 if failed else 0)
