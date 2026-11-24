#pragma once

// The component's pure logic: schedules, units, frame helpers. No ESPHome
// dependencies, so tests/ can compile and test it on a PC.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace esphome::clevertouch_radio {

// One switch point of a week schedule.
struct SchedulePoint {
  uint16_t minute;  // minute of the week, Monday 00:00 = 0
  uint8_t level;    // temperature in 0.5 C steps, or PT_OFF / PT_COMFORT / PT_REDUCED
};
static const uint8_t PT_OFF = 0;
static const uint8_t PT_COMFORT = 254;  // the radiator's comfort temperature
static const uint8_t PT_REDUCED = 255;  // the radiator's reduced temperature

static const size_t MAX_POINTS = 84;      // switch points a week per radiator (12 a day on average)
static const size_t MAX_DAY_POINTS = 16;  // a day: its text must fit in Home Assistant's 255 characters
static const int MINUTES_A_WEEK = 7 * 1440;

extern const char *const DAYS[7];  // "mon" .. "sun"

// "mon-fri 06:30 comfort 08:00 eco; sat,sun 08:00/21 23:00/17.5" -> sorted points.
bool parse_schedule(const std::string &text, std::vector<SchedulePoint> &out, std::string &error);
// One day's text ("07:00/eco 21:00/comfort", empty = no switch that day) -> that day's points.
bool parse_day(const std::string &text, int day, std::vector<SchedulePoint> &out, std::string &error);
// That day's points of a schedule as text, "07:00/eco 21:00/comfort".
std::string format_day(const std::vector<SchedulePoint> &points, int day);

// Unit of the temperatures people read and type: schedule texts, "Schedule
// status", the temperature numbers and the room sensor. Internally always
// 0.1 F (setpoints, as on the radio) and 0.5 C steps (schedule points).
void set_unit_fahrenheit(bool f);
bool unit_fahrenheit();
// A setpoint (0.1 F) as shown in the configured unit: C to 0.5 degrees, F to whole ones.
float shown_temp(int f10);
const char *unit_text();  // "°C" / "°F"

// Times in the schedule texts and "Schedule status": 24-hour ("18:15", the
// default) or 12-hour ("6:15PM"). Both are always accepted as input.
void set_time_12h(bool on);
bool time_12h();
// A minute of the day as shown: "07:00" / "18:15", or "7:00AM" / "6:15PM".
std::string format_time(int minute_of_day);

// C -> 0.1 F the way the E3 does it: half degrees come out one higher
// (14.5 -> 582, 18.5 -> 654; whole degrees exact: 16 -> 608).
int c_to_f10(float c);
float f10_to_c(int f);
float round_half(float c);
// Setpoint (0.1 F) -> schedule level (0.5 C steps, 5-30 C).
uint8_t level_of_set(int set);

// CRC-16/MODBUS over d[from, to).
uint16_t crc16(const std::vector<uint8_t> &d, size_t from, size_t to);
// A radiator's report of a timer set on it has one more tag, 5b = minutes: taken
// out (the report then has the usual layout) and returned; 0 without it.
int take_timer_tag(std::vector<uint8_t> &report);
// A frame's payload as tags, "45=0009 4b=02c0 ...", for logging unknown devices.
std::string decode_tags(const std::vector<uint8_t> &p);

// Minute of the week of the first switch point strictly after now_minute
// (with one point a week: that point, a week later). The schedule must not be empty.
uint16_t next_switch_minute(const std::vector<SchedulePoint> &schedule, uint16_t now_minute);
// A temporary change made at minute start, lasting until minute until (both of
// the week; until == start means a whole week): has it ended at now_minute?
bool override_expired(int start, int until, uint16_t now_minute);

}  // namespace esphome::clevertouch_radio
