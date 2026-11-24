"""CleverTouch (Touch E3) radiators over 868 MHz with a CC1101, without the E3."""

import re

import esphome.codegen as cg
from esphome.components import (
    binary_sensor,
    button,
    cc1101,
    climate,
    number,
    select,
    sensor,
    switch,
    text,
    text_sensor,
    time,
)
import esphome.config_validation as cv
from esphome.const import (
    CONF_DEVICE_ID,
    CONF_ID,
    CONF_NAME,
    CONF_TIME_ID,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_SIGNAL_STRENGTH,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_TEMPERATURE_DELTA,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_CELSIUS,
    UNIT_DECIBEL_MILLIWATT,
    UNIT_KILOWATT_HOURS,
    UNIT_PERCENT,
)

CODEOWNERS = []
DEPENDENCIES = ["cc1101", "time"]
AUTO_LOAD = ["binary_sensor", "button", "climate", "number", "select", "sensor", "switch", "text", "text_sensor"]

CONF_CC1101_ID = "cc1101_id"
CONF_CONTROLLER_ID = "controller_id"
CONF_REPLY_DELAY = "reply_delay"
CONF_LISTEN_AFTER_BOOT = "listen_after_boot"
CONF_IGNORE = "ignore"
CONF_CONTROL_RADIATORS = "control_radiators"
CONF_RADIATORS = "radiators"
CONF_RADIO_ID = "radio_id"
CONF_WATTS = "watts"
CONF_PANELS = "panels"
CONF_SCHEDULE = "schedule"
CONF_COMFORT = "comfort"
CONF_ECO = "eco"
CONF_AWAY = "away"
CONF_MIN_TEMPERATURE = "min_temperature"
CONF_MAX_TEMPERATURE = "max_temperature"
CONF_TEMPERATURE_UNIT = "temperature_unit"
CONF_TIME_FORMAT = "time_format"
UNIT_FAHRENHEIT = "\u00b0F"

TEMPERATURE = cv.float_  # range and unit: _apply_unit()

# Per unit: (lowest, highest, defaults for comfort / eco / away / min / max).
UNITS = {
    "C": (5, 30, {CONF_COMFORT: 21, CONF_ECO: 17, CONF_AWAY: 7, CONF_MIN_TEMPERATURE: 5, CONF_MAX_TEMPERATURE: 30}),
    "F": (41, 86, {CONF_COMFORT: 70, CONF_ECO: 63, CONF_AWAY: 45, CONF_MIN_TEMPERATURE: 41, CONF_MAX_TEMPERATURE: 86}),
}
TEMP_KEYS = (CONF_COMFORT, CONF_ECO, CONF_AWAY, CONF_MIN_TEMPERATURE, CONF_MAX_TEMPERATURE)

ns = cg.esphome_ns.namespace("clevertouch_radio")
ClevertouchHub = ns.class_("ClevertouchHub", cg.Component)
ClevertouchRadiator = ns.class_("ClevertouchRadiator", climate.Climate)
ClevertouchScheduleText = ns.class_("ClevertouchScheduleText", text.Text)
ClevertouchLimitSelect = ns.class_("ClevertouchLimitSelect", select.Select)
ClevertouchTempNumber = ns.class_("ClevertouchTempNumber", number.Number)
TempKind = ns.enum("TempKind")
ClevertouchPairSwitch = ns.class_("ClevertouchPairSwitch", switch.Switch)
ClevertouchSeqSwitch = ns.class_("ClevertouchSeqSwitch", switch.Switch)
ClevertouchRecalButton = ns.class_("ClevertouchRecalButton", button.Button)
ClevertouchControlSwitch = ns.class_("ClevertouchControlSwitch", switch.Switch)

# Preset temperatures: radiator store field -> (entity key, icon).
TEMP_NUMBERS = {
    "TEMP_COMFORT": ("comfort_temperature", "mdi:sun-thermometer"),
    "TEMP_REDUCED": ("eco_temperature", "mdi:leaf"),
    "TEMP_FROST": ("away_temperature", "mdi:snowflake-thermometer"),
}

LIMIT_OPTIONS = ["90 °C (100 %)", "75 °C (70 %)", "60 °C (55 %)"]


def radio_id(value):
    """8 hex digits, as the E3 shows them in its radiator settings (Device ID), e.g. F1234567."""
    if isinstance(value, int):
        v = value
    else:
        s = re.sub(r"[\s:.\-]", "", str(value)).lower()
        if s.startswith("0x"):
            s = s[2:]
        if not re.fullmatch(r"[0-9a-f]{8}", s):
            raise cv.Invalid(f"radio ID must be 8 hex digits, like F1234567 (got {value!r})")
        v = int(s, 16)
    if not 0 < v <= 0xFFFFFFFF:
        raise cv.Invalid("radio ID out of range")
    return v


DAYS = ["mon", "tue", "wed", "thu", "fri", "sat", "sun"]
DAY_NAMES = ["Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"]
MAX_POINTS = 84  # as in clevertouch_logic.h
MAX_DAY_POINTS = 16


def _days(spec):
    out = set()
    for part in spec.split(","):
        if part in ("daily", "all"):
            out.update(range(7))
        elif "-" in part:
            a, _, b = part.partition("-")
            if a not in DAYS or b not in DAYS:
                raise cv.Invalid(f"unknown day range '{part}'")
            d = DAYS.index(a)
            while True:
                out.add(d)
                if d == DAYS.index(b):
                    break
                d = (d + 1) % 7
        elif part in DAYS:
            out.add(DAYS.index(part))
        else:
            raise cv.Invalid(f"unknown day '{part}'")
    return out


def _minute_of_day(t):
    """'7:00' / '18:15', or '7:00am' / '6:15pm' (hour 1-12; 12am = midnight,
    12pm = noon), as parse_time() in clevertouch_logic.cpp. None if not a time."""
    m = re.fullmatch(r"([0-9]{1,2}):([0-9]{2})(am|pm)?", t)
    if not m or int(m[2]) > 59:
        return None
    hh = int(m[1])
    if m[3] is None:
        return hh * 60 + int(m[2]) if hh <= 23 else None
    if not 1 <= hh <= 12:
        return None
    return ((hh % 12) + (12 if m[3] == "pm" else 0)) * 60 + int(m[2])


def _schedule_groups(text):
    """The groups of a schedule text, as lists of tokens. Separators as in
    parse_schedule(): no Unicode spaces (nor digits, below), which the ESP doesn't know."""
    for group in re.split(r"[;\n]", text.lower()):
        yield [t for t in re.split(r"[ \t\r/]+", group) if t]


def schedule(value):
    """Same grammar as parse_schedule() in clevertouch_logic.cpp, checked at compile time."""
    value = cv.string(value).strip()
    seen = set()
    for tok in _schedule_groups(value):
        if not tok:
            continue
        days = _days(tok[0])
        if len(tok) < 3 or len(tok) % 2 == 0:
            raise cv.Invalid(f"'{tok[0]}' needs \"HH:MM temperature\" pairs")
        for t, temp in zip(tok[1::2], tok[2::2]):
            minute = _minute_of_day(t)
            if minute is None:
                raise cv.Invalid(f"bad time '{t}'")
            # A plain decimal number ("21", "17.5"), as in parse_schedule(); the
            # range depends on temperature_unit and is checked in _apply_unit().
            if temp not in ("off", "comfort", "reduced", "eco") and not re.fullmatch(r"[0-9]+(\.[0-9]+)?", temp):
                raise cv.Invalid(f"bad temperature '{temp}' (a number, comfort, eco or off)")
            for d in days:
                key = (d, minute)
                if key in seen:
                    raise cv.Invalid(f"two temperatures at {DAYS[d]} {t}")
                seen.add(key)
    if len(seen) > MAX_POINTS:
        raise cv.Invalid(f"more than {MAX_POINTS} switch points a week")
    if any(sum(d == day for d, _ in seen) > MAX_DAY_POINTS for day in range(7)):
        raise cv.Invalid(f"more than {MAX_DAY_POINTS} switch points a day")
    return value


# Entities per radiator: key -> name (on a sub-device the radiator name is the device name).
RADIATOR_ENTITIES = {
    "climate": None,
    "room_temperature": "Room temperature",
    "heating_power": "Heating power",
    "energy": "Energy",
    "signal": "Radio signal",
    "reporting": "Reporting",
    "radio_id_sensor": "Radio ID",
    "radiator_mode": "Radiator mode",
    "schedule_status": "Schedule status",
    "schedule_monday": "Schedule Monday",
    "schedule_tuesday": "Schedule Tuesday",
    "schedule_wednesday": "Schedule Wednesday",
    "schedule_thursday": "Schedule Thursday",
    "schedule_friday": "Schedule Friday",
    "schedule_saturday": "Schedule Saturday",
    "schedule_sunday": "Schedule Sunday",
    "front_panel_limit": "Front panel limit",
    "comfort_temperature": "Comfort temperature",
    "eco_temperature": "Eco temperature",
    "away_temperature": "Away temperature",
    "temperature_offset": "Temperature offset",
    "pairing": "Pairing",
    "sequential_control": "Sequential control",
}


def _fill_entities(value):
    """Give every per-radiator entity a name (and the radiator's device_id) unless set."""
    if not isinstance(value, dict):
        raise cv.Invalid("expected a radiator (name, radio_id, watts, ...)")
    value = dict(value)
    name = str(value.get(CONF_NAME, ""))
    dev = value.get(CONF_DEVICE_ID)
    for key, label in RADIATOR_ENTITIES.items():
        sub = dict(value.get(key) or {})
        if CONF_NAME not in sub:
            if dev is not None:
                sub[CONF_NAME] = label or ""  # the climate: empty, takes the device name
            else:
                sub[CONF_NAME] = name if label is None else f"{name} {label[0].lower()}{label[1:]}"
        if dev is not None and CONF_DEVICE_ID not in sub:
            sub[CONF_DEVICE_ID] = dev
        value[key] = sub
    return value


def _schedule_numbers(text):
    """The numeric temperatures in a schedule text."""
    out = []
    for tok in _schedule_groups(text):
        for temp in tok[2::2]:
            if temp not in ("off", "comfort", "reduced", "eco"):
                out.append(float(temp))
    return out


def _apply_unit(config):
    """Temperatures in the YAML are in temperature_unit: check them, fill the
    defaults, and convert to C in 0.5 steps, as the radiator keeps them."""
    unit = config[CONF_TEMPERATURE_UNIT]
    lo, hi, defaults = UNITS[unit]
    for rc in config[CONF_RADIATORS]:
        for key in TEMP_KEYS:
            v = rc.get(key, defaults[key])
            if not lo <= v <= hi:
                raise cv.Invalid(f"{rc[CONF_NAME]}: {key} {v} is outside {lo}-{hi} \u00b0{unit}")
            rc[key] = round(((v - 32) * 5 / 9 if unit == "F" else v) * 2) / 2
        if rc[CONF_MIN_TEMPERATURE] >= rc[CONF_MAX_TEMPERATURE]:
            raise cv.Invalid(f"{rc[CONF_NAME]}: min_temperature must be below max_temperature")
        for v in _schedule_numbers(rc[CONF_SCHEDULE]):
            if not lo <= v <= hi:
                raise cv.Invalid(f"{rc[CONF_NAME]}: schedule temperature {v:g} is outside {lo}-{hi} \u00b0{unit}")
        if unit == "F":
            # Shown in F too (the climate entity stays C: ESPHome climate is always C,
            # Home Assistant converts it).
            rc["room_temperature"]["unit_of_measurement"] = UNIT_FAHRENHEIT
            for key in ("comfort_temperature", "eco_temperature", "away_temperature", "temperature_offset"):
                rc[key]["unit_of_measurement"] = UNIT_FAHRENHEIT
    return config



RADIATOR_SCHEMA = cv.All(
    _fill_entities,
    cv.Schema(
        {
            cv.Required(CONF_NAME): cv.string_strict,
            cv.Optional(CONF_RADIO_ID): radio_id,
            cv.Required(CONF_WATTS): cv.All(cv.positive_float, cv.Range(max=5000)),
            # Double panel: for the energy figure under a front panel limit only
            # (control is the same). Front panel taken as 40 % of the power, from
            # the radiator manual's output figures; not verified on a radiator.
            cv.Optional(CONF_PANELS, default=1): cv.int_range(min=1, max=2),
            cv.Optional(CONF_SCHEDULE, default=""): schedule,
            # Starting temperatures; replaced by what the radiator / E3 send.
            # Defaults per unit, filled in by _apply_unit().
            cv.Optional(CONF_COMFORT): TEMPERATURE,
            cv.Optional(CONF_ECO): TEMPERATURE,
            cv.Optional(CONF_AWAY): TEMPERATURE,  # frost protection
            # The radiator is never sent a setpoint outside these (off excepted);
            # a change on its buttons beyond them is pushed back to the limit.
            cv.Optional(CONF_MIN_TEMPERATURE): TEMPERATURE,
            cv.Optional(CONF_MAX_TEMPERATURE): TEMPERATURE,
            cv.Optional(CONF_DEVICE_ID): cv.sub_device_id,
            cv.Required("climate"): climate.climate_schema(ClevertouchRadiator),
            cv.Required("room_temperature"): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Required("heating_power"): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT,
                accuracy_decimals=0,
                icon="mdi:radiator",
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Required("energy"): sensor.sensor_schema(
                unit_of_measurement=UNIT_KILOWATT_HOURS,
                accuracy_decimals=3,
                device_class=DEVICE_CLASS_ENERGY,
                state_class=STATE_CLASS_TOTAL_INCREASING,
            ),
            cv.Required("signal"): sensor.sensor_schema(
                unit_of_measurement=UNIT_DECIBEL_MILLIWATT,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_SIGNAL_STRENGTH,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Required("radio_id_sensor"): text_sensor.text_sensor_schema(
                icon="mdi:identifier", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
            ),
            cv.Required("reporting"): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_CONNECTIVITY,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Required("temperature_offset"): number.number_schema(
                ClevertouchTempNumber,
                icon="mdi:thermometer-plus",
                entity_category=ENTITY_CATEGORY_CONFIG,
                # A difference: Home Assistant mustn't convert 0 C to 32 F.
                device_class=DEVICE_CLASS_TEMPERATURE_DELTA,
                unit_of_measurement=UNIT_CELSIUS,
            ),
            cv.Required("radiator_mode"): text_sensor.text_sensor_schema(
                icon="mdi:radiator", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
            ),
            cv.Required("schedule_status"): text_sensor.text_sensor_schema(
                icon="mdi:calendar-clock"
            ),
            **{
                cv.Required(f"schedule_{d.lower()}"): text.text_schema(
                    ClevertouchScheduleText,
                    mode="TEXT",
                    icon="mdi:calendar-clock",
                    entity_category=ENTITY_CATEGORY_CONFIG,
                )
                for d in DAY_NAMES
            },
            cv.Required("front_panel_limit"): select.select_schema(
                ClevertouchLimitSelect,
                icon="mdi:thermometer-high",
                entity_category=ENTITY_CATEGORY_CONFIG,
            ),
            **{
                cv.Required(key): number.number_schema(
                    ClevertouchTempNumber,
                    icon=icon,
                    entity_category=ENTITY_CATEGORY_CONFIG,
                    device_class=DEVICE_CLASS_TEMPERATURE,
                    unit_of_measurement=UNIT_CELSIUS,
                )
                for key, icon in TEMP_NUMBERS.values()
            },
            # Double panels only (created with panels: 2).
            cv.Optional("sequential_control"): switch.switch_schema(
                ClevertouchSeqSwitch,
                icon="mdi:radiator",
                entity_category=ENTITY_CATEGORY_CONFIG,
                default_restore_mode="DISABLED",
            ),
            cv.Required("pairing"): switch.switch_schema(
                ClevertouchPairSwitch,
                icon="mdi:link-variant",
                entity_category=ENTITY_CATEGORY_CONFIG,
                default_restore_mode="ALWAYS_OFF",
            ),
        }
    ),
)


def _check_unique(config):
    """A radiator's name keys its stored state, and its radio_id its radio traffic:
    both must be unique."""
    names, ids = {}, {}
    for rc in config[CONF_RADIATORS]:
        name = rc[CONF_NAME]
        if name in names:
            raise cv.Invalid(f"two radiators are named '{name}'; the name keys the stored state, so it must be unique")
        names[name] = True
        rid = rc.get(CONF_RADIO_ID)
        if rid is not None:
            if rid in ids:
                raise cv.Invalid(f"radio_id {rid:08X} is used by both '{ids[rid]}' and '{name}'")
            ids[rid] = name
    return config


def _hub_entity(name):
    return {CONF_NAME: name}


# Controller entities: key -> default name, also when the key is given
# without a name (e.g. "control: {restore_mode: ALWAYS_ON}").
HUB_ENTITIES = {
    "control": "Control radiators",
    "status": "Controller status",
    "controller_id_sensor": "Controller ID",
    "unassigned": "Unconfigured devices heard",
    "frequency_correction": "Frequency correction",
    "recalibrate": "Recalibrate frequency",
    "shadow_matches": "E3 shadow matches",
    "shadow_mismatches": "E3 shadow mismatches",
    "missed_reports": "Missed radiator reports",
}


def _fill_hub_entities(value):
    if not isinstance(value, dict):
        return value
    value = dict(value)
    control = value.get("control")
    if isinstance(control, dict) and "restore_mode" in control:
        raise cv.Invalid(
            "set the start-up state with 'control_radiators: DEFAULT_ON' (or DEFAULT_OFF) "
            "under clevertouch_radio:, instead of restore_mode under control:"
        )
    for key, name in HUB_ENTITIES.items():
        if key in value and isinstance(value[key], dict) and CONF_NAME not in value[key]:
            value[key] = {**value[key], CONF_NAME: name}
    return value


CONFIG_SCHEMA = cv.All(cv.only_on_esp32, _fill_hub_entities, cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ClevertouchHub),
        cv.GenerateID(CONF_CC1101_ID): cv.use_id(cc1101.CC1101Component),
        cv.GenerateID(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
        cv.Optional(CONF_CONTROLLER_ID): radio_id,
        cv.Optional(CONF_REPLY_DELAY, default="200ms"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(max=cv.TimePeriod(milliseconds=400)),
        ),
        # Only listen this long after boot, so an E3 that is still running is
        # heard before this transmits. 0s for a house without an E3.
        cv.Optional(CONF_LISTEN_AFTER_BOOT, default="5min"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(max=cv.TimePeriod(minutes=30)),
        ),
        # Devices not to list as unconfigured or log (another ESP's radiators).
        cv.Optional(CONF_IGNORE, default=[]): cv.ensure_list(radio_id),
        # Unit of the temperatures in the YAML, the schedule texts, "Schedule
        # status", the temperature numbers and the room sensor.
        cv.Optional(CONF_TEMPERATURE_UNIT, default="C"): cv.one_of("C", "F", upper=True),
        # Times in the schedule texts and "Schedule status": 24h ("18:15") or 12h
        # ("6:15PM"). Both are always accepted as input.
        cv.Optional(CONF_TIME_FORMAT, default="24h"): cv.one_of("24h", "12h", lower=True),
        # "Control radiators" right after the very first flash; afterwards the
        # switch keeps whatever it was last set to, over reboots and updates.
        cv.Optional(CONF_CONTROL_RADIATORS, default="DEFAULT_OFF"): cv.one_of(
            "DEFAULT_ON", "DEFAULT_OFF", upper=True
        ),
        cv.Required(CONF_RADIATORS): cv.All(
            cv.ensure_list(RADIATOR_SCHEMA), cv.Length(min=1, max=32)
        ),
        cv.Optional(
            "control", default=_hub_entity("Control radiators")
        ): switch.switch_schema(
            ClevertouchControlSwitch,
            icon="mdi:radio-tower",
            default_restore_mode="RESTORE_DEFAULT_OFF",
        ),
        cv.Optional(
            "status", default=_hub_entity("Controller status")
        ): text_sensor.text_sensor_schema(icon="mdi:radiator"),
        cv.Optional(
            "controller_id_sensor", default=_hub_entity("Controller ID")
        ): text_sensor.text_sensor_schema(
            icon="mdi:identifier", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
        cv.Optional(
            "unassigned", default=_hub_entity("Unconfigured devices heard")
        ): text_sensor.text_sensor_schema(
            icon="mdi:radiator-disabled", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
        cv.Optional(
            "frequency_correction", default=_hub_entity("Frequency correction")
        ): sensor.sensor_schema(
            unit_of_measurement="kHz",
            accuracy_decimals=1,
            icon="mdi:sine-wave",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(
            "recalibrate", default=_hub_entity("Recalibrate frequency")
        ): button.button_schema(
            ClevertouchRecalButton, icon="mdi:tune", entity_category=ENTITY_CATEGORY_CONFIG
        ),
        cv.Optional(
            "shadow_matches", default=_hub_entity("E3 shadow matches")
        ): sensor.sensor_schema(
            accuracy_decimals=0, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
        cv.Optional(
            "shadow_mismatches", default=_hub_entity("E3 shadow mismatches")
        ): sensor.sensor_schema(
            accuracy_decimals=0, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
        cv.Optional(
            "missed_reports", default=_hub_entity("Missed radiator reports")
        ): sensor.sensor_schema(
            accuracy_decimals=0, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
    }
).extend(cv.COMPONENT_SCHEMA), _check_unique, _apply_unit)


async def to_code(config):
    hub = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(hub, config)
    cg.add(hub.set_radio(await cg.get_variable(config[CONF_CC1101_ID])))
    cg.add(hub.set_time(await cg.get_variable(config[CONF_TIME_ID])))
    if CONF_CONTROLLER_ID in config:
        cg.add(hub.set_controller_id(config[CONF_CONTROLLER_ID]))
    cg.add(hub.set_reply_delay(config[CONF_REPLY_DELAY].total_milliseconds))
    cg.add(hub.set_listen_after_boot(config[CONF_LISTEN_AFTER_BOOT].total_milliseconds))
    fahrenheit = config[CONF_TEMPERATURE_UNIT] == "F"
    if fahrenheit:
        cg.add(hub.set_fahrenheit(True))
    if config[CONF_TIME_FORMAT] == "12h":
        cg.add(hub.set_12h(True))
    for dev in config[CONF_IGNORE]:
        cg.add(hub.add_ignored(dev))

    sw = await switch.new_switch(config["control"])
    mode = "ON" if config[CONF_CONTROL_RADIATORS] == "DEFAULT_ON" else "OFF"
    cg.add(sw.set_restore_mode(cg.RawExpression(f"esphome::switch_::SWITCH_RESTORE_DEFAULT_{mode}")))
    cg.add(hub.set_control_switch(sw))
    cg.add(hub.set_status_sensor(await text_sensor.new_text_sensor(config["status"])))
    cg.add(hub.set_unassigned_sensor(await text_sensor.new_text_sensor(config["unassigned"])))
    cg.add(
        hub.set_controller_id_sensor(
            await text_sensor.new_text_sensor(config["controller_id_sensor"])
        )
    )
    cg.add(hub.set_freq_sensor(await sensor.new_sensor(config["frequency_correction"])))
    cg.add(hub.set_shadow_ok_sensor(await sensor.new_sensor(config["shadow_matches"])))
    cg.add(hub.set_shadow_bad_sensor(await sensor.new_sensor(config["shadow_mismatches"])))
    cg.add(hub.set_missed_sensor(await sensor.new_sensor(config["missed_reports"])))
    recal = await button.new_button(config["recalibrate"])
    cg.add(recal.set_hub(hub))

    for rc in config[CONF_RADIATORS]:
        rad = await climate.new_climate(rc["climate"])
        cg.add(rad.set_hub(hub))
        cg.add(rad.set_label(rc[CONF_NAME]))
        if CONF_RADIO_ID in rc:
            cg.add(rad.set_config_radio_id(rc[CONF_RADIO_ID]))
        cg.add(rad.set_watts(rc[CONF_WATTS]))
        if rc[CONF_PANELS] == 2:
            cg.add(rad.set_front_share(0.4))
            seq = await switch.new_switch(rc["sequential_control"])
            cg.add(seq.set_radiator(rad))
            cg.add(rad.set_seq_switch(seq))
        cg.add(rad.set_default_schedule(rc[CONF_SCHEDULE]))
        cg.add(
            rad.set_default_temperatures(rc[CONF_COMFORT], rc[CONF_ECO], rc[CONF_AWAY])
        )
        lo, hi = rc[CONF_MIN_TEMPERATURE], rc[CONF_MAX_TEMPERATURE]
        cg.add(rad.set_temperature_limits(lo, hi))
        cg.add(
            rad.set_reporting_sensor(
                await binary_sensor.new_binary_sensor(rc["reporting"])
            )
        )
        cg.add(rad.set_room_sensor(await sensor.new_sensor(rc["room_temperature"])))
        cg.add(rad.set_power_sensor(await sensor.new_sensor(rc["heating_power"])))
        cg.add(rad.set_energy_sensor(await sensor.new_sensor(rc["energy"])))
        cg.add(rad.set_signal_sensor(await sensor.new_sensor(rc["signal"])))
        cg.add(rad.set_mode_sensor(await text_sensor.new_text_sensor(rc["radiator_mode"])))
        cg.add(
            rad.set_radio_id_sensor(
                await text_sensor.new_text_sensor(rc["radio_id_sensor"])
            )
        )
        cg.add(
            rad.set_schedule_status_sensor(
                await text_sensor.new_text_sensor(rc["schedule_status"])
            )
        )
        for day, d in enumerate(DAY_NAMES):
            sched = await text.new_text(rc[f"schedule_{d.lower()}"], max_length=255)
            cg.add(sched.set_radiator(rad, day))
            cg.add(rad.set_day_text(day, sched))
        lim = await select.new_select(rc["front_panel_limit"], options=LIMIT_OPTIONS)
        cg.add(lim.set_radiator(rad))
        cg.add(rad.set_limit_select(lim))
        for kind, (key, _) in TEMP_NUMBERS.items():
            if fahrenheit:  # whole degrees F
                num = await number.new_number(
                    rc[key], min_value=round(lo * 9 / 5 + 32), max_value=round(hi * 9 / 5 + 32), step=1
                )
            else:
                num = await number.new_number(rc[key], min_value=lo, max_value=hi, step=0.5)
            cg.add(num.set_radiator(rad, getattr(TempKind, kind)))
            cg.add(rad.set_temp_number(getattr(TempKind, kind), num))
        # Room temperature offset, sent to the radiator (0.1 F on the radio).
        off = await number.new_number(
            rc["temperature_offset"],
            min_value=-9 if fahrenheit else -5,
            max_value=9 if fahrenheit else 5,
            step=0.1,
        )
        cg.add(off.set_radiator(rad, TempKind.TEMP_OFFSET))
        cg.add(rad.set_offset_number(off))
        pair = await switch.new_switch(rc["pairing"])
        cg.add(pair.set_radiator(rad))
        cg.add(rad.set_pair_switch(pair))
        cg.add(hub.add_radiator(rad))
