// Tests of the component's pure logic (clevertouch_logic.cpp), on a PC. How to
// build and run them: README.md, "Development".

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "clevertouch_radio/clevertouch_logic.h"

using namespace esphome::clevertouch_radio;

static int g_failed = 0, g_checks = 0;

#define CHECK(cond, ...) \
  do { \
    g_checks++; \
    if (!(cond)) { \
      g_failed++; \
      printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
      printf(__VA_ARGS__); \
      printf("\n"); \
    } \
  } while (0)

static std::vector<uint8_t> hex(const std::string &s) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < s.size(); i += 2)
    out.push_back((uint8_t) std::stoi(s.substr(i, 2), nullptr, 16));
  return out;
}

static std::vector<std::string> split_bar(const std::string &line) {
  std::vector<std::string> out;
  std::stringstream ss(line);
  std::string part;
  while (std::getline(ss, part, '|'))
    out.push_back(part);
  return out;
}

// The shared cases: same verdict (and point count) as the Python validator.
static void test_schedule_cases(const char *path) {
  std::ifstream f(path);
  CHECK(f.good(), "can't open %s", path);
  std::string line;
  int n = 0;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line[0] == '#')
      continue;
    auto c = split_bar(line);
    set_unit_fahrenheit(c[0] == "F");
    std::vector<SchedulePoint> pts;
    std::string err;
    bool ok = parse_schedule(c[1], pts, err);
    if (c[2] == "ok") {
      CHECK(ok, "'%s' (%s) rejected: %s", c[1].c_str(), c[0].c_str(), err.c_str());
      CHECK(!ok || (int) pts.size() == std::stoi(c[3]), "'%s': %d points, expected %s", c[1].c_str(), (int) pts.size(),
            c[3].c_str());
    } else {
      CHECK(!ok, "'%s' (%s) accepted, should be an error", c[1].c_str(), c[0].c_str());
    }
    n++;
  }
  CHECK(n > 30, "only %d cases read", n);
  set_unit_fahrenheit(false);
}

static void test_schedule_details() {
  std::vector<SchedulePoint> pts;
  std::string err;

  // Points sorted, levels and minutes right.
  CHECK(parse_schedule("tue 21:00 comfort; mon 07:00/17.5 08:00/off", pts, err), "%s", err.c_str());
  CHECK(pts.size() == 3, "%d", (int) pts.size());
  CHECK(pts[0].minute == 7 * 60 && pts[0].level == 35, "%u %u", pts[0].minute, pts[0].level);
  CHECK(pts[1].minute == 8 * 60 && pts[1].level == PT_OFF, "%u", pts[1].minute);
  CHECK(pts[2].minute == 1440 + 21 * 60 && pts[2].level == PT_COMFORT, "%u", pts[2].minute);

  // Ranges wrap round the week end.
  CHECK(parse_schedule("sat-mon 09:00 eco", pts, err), "%s", err.c_str());
  CHECK(pts.size() == 3 && pts[0].minute == 9 * 60 && pts[2].minute == 6 * 1440 + 9 * 60, "%d", (int) pts.size());

  // Day texts: format(parse(x)) == x for canonical texts, per day.
  const char *texts[] = {"07:00/eco 21:00/comfort", "06:30/21 08:00/20.5 15:00/eco 22:30/off", "00:00/5 23:59/30"};
  for (const char *t : texts) {
    for (int day = 0; day < 7; day++) {
      CHECK(parse_day(t, day, pts, err), "%s: %s", t, err.c_str());
      std::string back = format_day(pts, day);
      CHECK(back == t, "day %d: '%s' came back as '%s'", day, t, back.c_str());
      CHECK(format_day(pts, (day + 1) % 7).empty(), "other day not empty");
    }
  }
  // A space instead of '/', and an empty day.
  CHECK(parse_day("07:00 eco  21:00 comfort", 2, pts, err) && format_day(pts, 2) == "07:00/eco 21:00/comfort", "%s",
        format_day(pts, 2).c_str());
  CHECK(parse_day("   ", 3, pts, err) && pts.empty(), "empty day");
  // The longest day text fits in Home Assistant's 255 characters, in both time formats.
  std::string longest;
  for (size_t i = 0; i < MAX_DAY_POINTS; i++)
    longest += (i ? " 10:" : "10:") + std::to_string(i / 6) + std::to_string(i % 6) + "/comfort";
  CHECK(parse_day(longest, 6, pts, err) && pts.size() == MAX_DAY_POINTS, "%s", err.c_str());
  CHECK(format_day(pts, 6).size() <= 255, "%d", (int) format_day(pts, 6).size());
  set_time_12h(true);
  CHECK(format_day(pts, 6).size() <= 255, "%d", (int) format_day(pts, 6).size());
  set_time_12h(false);
  CHECK(!parse_day("07:00 eco; 08:00 comfort", 0, pts, err), "';' in a day text accepted");
  CHECK(!parse_day("07:00 eco\n08:00 comfort", 0, pts, err), "newline in a day text accepted");
}

static void test_12h() {
  std::vector<SchedulePoint> pts;
  std::string err;
  CHECK(format_time(0) == "00:00" && format_time(18 * 60 + 15) == "18:15", "%s", format_time(0).c_str());
  set_time_12h(true);
  CHECK(format_time(0) == "12:00AM", "%s", format_time(0).c_str());
  CHECK(format_time(7 * 60) == "7:00AM", "%s", format_time(7 * 60).c_str());
  CHECK(format_time(12 * 60 + 30) == "12:30PM", "%s", format_time(12 * 60 + 30).c_str());
  CHECK(format_time(18 * 60 + 15) == "6:15PM", "%s", format_time(18 * 60 + 15).c_str());
  CHECK(format_time(23 * 60 + 59) == "11:59PM", "%s", format_time(23 * 60 + 59).c_str());
  // Every minute of the day: shown in 12h, read back to the same minute.
  for (int m = 0; m < 1440; m++) {
    std::string t = format_time(m) + "/eco";
    CHECK(parse_day(t, 4, pts, err) && pts.size() == 1 && pts[0].minute == 4 * 1440 + m, "%s: %s", t.c_str(),
          err.c_str());
    CHECK(format_day(pts, 4) == t, "%s came back as %s", t.c_str(), format_day(pts, 4).c_str());
  }
  // 24-hour input still works, and comes out in 12h.
  CHECK(parse_day("07:00/eco 18:15/comfort", 0, pts, err) && format_day(pts, 0) == "7:00AM/eco 6:15PM/comfort", "%s",
        format_day(pts, 0).c_str());
  set_time_12h(false);
  CHECK(parse_day("7:00am/eco 6:15pm/comfort", 0, pts, err) && format_day(pts, 0) == "07:00/eco 18:15/comfort", "%s",
        format_day(pts, 0).c_str());
}

static void test_fahrenheit() {
  std::vector<SchedulePoint> pts;
  std::string err;
  set_unit_fahrenheit(true);
  // Every whole degree F comes back exactly.
  for (int f = 41; f <= 86; f++) {
    std::string t = "07:00/" + std::to_string(f);
    CHECK(parse_day(t, 0, pts, err), "%s: %s", t.c_str(), err.c_str());
    CHECK(format_day(pts, 0) == t, "%s came back as %s", t.c_str(), format_day(pts, 0).c_str());
  }
  CHECK(std::string(unit_text()) == "°F", "unit text");
  CHECK(shown_temp(700) == 70.0f, "%f", shown_temp(700));
  // Whole F typed, kept in 0.5 C steps, shown as typed.
  for (int f = 41; f <= 86; f++) {
    int s = c_to_f10(round_half((f - 32) * 5.0f / 9.0f));
    CHECK(shown_temp(s) == (float) f, "%d F comes back as %f", f, shown_temp(s));
  }
  set_unit_fahrenheit(false);
  CHECK(shown_temp(c_to_f10(19.0f)) == 19.0f, "%f", shown_temp(c_to_f10(19.0f)));
}

static void test_units() {
  // As the E3 sends them (captured): half degrees come out one higher.
  CHECK(c_to_f10(14.5f) == 582, "%d", c_to_f10(14.5f));
  CHECK(c_to_f10(18.5f) == 654, "%d", c_to_f10(18.5f));
  CHECK(c_to_f10(16.0f) == 608, "%d", c_to_f10(16.0f));
  CHECK(c_to_f10(19.0f) == 662, "%d", c_to_f10(19.0f));
  CHECK(c_to_f10(0.0f) == 320, "%d", c_to_f10(0.0f));
  CHECK(round_half(f10_to_c(581)) == 14.5f, "%f", round_half(f10_to_c(581)));
  CHECK(level_of_set(c_to_f10(17.5f)) == 35, "%d", level_of_set(c_to_f10(17.5f)));
  CHECK(level_of_set(320) == 10, "clamped to 5 C: %d", level_of_set(320));
}

static void test_frames() {
  // PROTOCOL.md's test vector: a poll from f14f8b6c to f11a6571.
  auto poll = hex("F14F8B6CC6F11A65711900");
  CHECK(crc16(poll, 0, poll.size()) == 0xDC88, "%04x", crc16(poll, 0, poll.size()));

  // A radiator report (length byte first, CRC last), decoded completely.
  auto df = hex("23F11A6571C6F14F8B6CDF203000404B02C08A026003E10064000"
                "0DA000000000A475837");
  std::string t = decode_tags(df);
  CHECK(t == "df=20300040 4b=02c0 8a=026003 e1=00640000 da=00000000 0a=47", "%s", t.c_str());
  CHECK(crc16(df, 1, df.size() - 2) == (df[df.size() - 2] | df[df.size() - 1] << 8), "report CRC");

  // A Watts Vision thermostat report (rtl_433 issue #2885), same tag rules.
  auto watts = hex("22d0374654c6d0904d898d0011018a0284034b02bfcc000000008e0a24934c0000310d98a4");
  t = decode_tags(watts);
  CHECK(t == "8d=001101 8a=028403 4b=02bf cc=00000000 8e=0a2493 4c=0000 31=0d", "%s", t.c_str());

  // Undecodable rest after '|'; too short: nothing.
  // A timer set on a radiator (17.0 C for 60 min), captured: one tag more.
  auto timer = hex("26F11A4E62C6F14F8B6CDF203100404B02938A0272045B003CE100640000DA000000000A479285");
  CHECK(timer.size() == 39, "%d", (int) timer.size());
  CHECK(take_timer_tag(timer) == 60, "minutes");
  CHECK(timer.size() == 36 && timer[21] == 0x04 && timer[22] == 0xe1 && timer[27] == 0xda, "usual layout");
  CHECK(take_timer_tag(timer) == 0 && timer.size() == 36, "nothing to take out");

  auto odd = hex("0FF1000000C6F14F8B6C"
                 "8a0284"
                 "0000");
  CHECK(decode_tags(odd).find("|") != std::string::npos, "%s", decode_tags(odd).c_str());
  CHECK(decode_tags(hex("0D0102")).empty(), "short frame");
}

static void test_overrides() {
  const int D = 1440;
  std::vector<SchedulePoint> s = {{7 * 60, PT_REDUCED}, {21 * 60, PT_COMFORT}, {4 * D + 7 * 60, PT_REDUCED}};
  // Next switch point: later today, tomorrow-ish, and round the week end.
  CHECK(next_switch_minute(s, 6 * 60) == 7 * 60, "as expected");
  CHECK(next_switch_minute(s, 7 * 60) == 21 * 60, "a point at now isn't the next one");
  CHECK(next_switch_minute(s, 22 * 60) == 4 * D + 7 * 60, "as expected");
  CHECK(next_switch_minute(s, 6 * D) == 7 * 60, "wraps to next week");

  // Made Monday 12:00, until 21:00.
  CHECK(!override_expired(12 * 60, 21 * 60, 20 * 60 + 59), "as expected");
  CHECK(override_expired(12 * 60, 21 * 60, 21 * 60), "as expected");
  CHECK(override_expired(12 * 60, 21 * 60, 2 * D), "much later");
  // Made Sunday 22:00, until Monday 07:00: across the week end.
  int sun22 = 6 * D + 22 * 60;
  CHECK(!override_expired(sun22, 7 * 60, 23 * 60 + 6 * D), "as expected");
  CHECK(!override_expired(sun22, 7 * 60, 6 * 60), "as expected");
  CHECK(override_expired(sun22, 7 * 60, 7 * 60), "as expected");
  // One switch point a week: until == start's next == a week later.
  std::vector<SchedulePoint> one = {{2 * D + 8 * 60, PT_COMFORT}};
  int start = 3 * D;
  int until = next_switch_minute(one, start);
  CHECK(until == 2 * D + 8 * 60, "%d", until);
  CHECK(!override_expired(start, until, 4 * D), "as expected");
  CHECK(override_expired(start, until, 2 * D + 8 * 60), "a week later");
  // Made exactly at the only point: lasts the whole week.
  CHECK(!override_expired(one[0].minute, next_switch_minute(one, one[0].minute), one[0].minute + 1), "as expected");
  CHECK(override_expired(one[0].minute, next_switch_minute(one, one[0].minute), one[0].minute - 1), "not forever");
  // Daylight saving time ends (Sunday 04:00 -> 03:00): made at 03:40, until 07:00.
  int sun = 6 * D;
  CHECK(!override_expired(sun + 3 * 60 + 40, sun + 7 * 60, sun + 3 * 60 + 59), "as expected");
  CHECK(!override_expired(sun + 3 * 60 + 40, sun + 7 * 60, sun + 3 * 60), "the clock went back, not a week on");
  CHECK(!override_expired(sun + 3 * 60 + 40, sun + 7 * 60, sun + 6 * 60 + 59), "as expected");
  CHECK(override_expired(sun + 3 * 60 + 40, sun + 7 * 60, sun + 7 * 60), "as expected");
  // It starts (03:00 -> 04:00): until 03:30, which is skipped.
  CHECK(!override_expired(sun + 2 * 60, sun + 3 * 60 + 30, sun + 2 * 60 + 59), "as expected");
  CHECK(override_expired(sun + 2 * 60, sun + 3 * 60 + 30, sun + 4 * 60), "at the first minute after the gap");
  // Not anchored (no clock when made): never expires by itself.
  CHECK(!override_expired(-1, -1, 5000), "as expected");
}

int main(int argc, char **argv) {
  test_schedule_cases(argc > 1 ? argv[1] : "tests/schedule_cases.txt");
  test_schedule_details();
  test_12h();
  test_fahrenheit();
  test_units();
  test_frames();
  test_overrides();
  printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
