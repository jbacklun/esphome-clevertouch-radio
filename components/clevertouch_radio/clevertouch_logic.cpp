#include "clevertouch_logic.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace esphome::clevertouch_radio {

const char *const DAYS[7] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};

// ---------------------------------------------------------------------------
// Units

static bool g_fahrenheit = false;
void set_unit_fahrenheit(bool f) { g_fahrenheit = f; }
bool unit_fahrenheit() { return g_fahrenheit; }

int c_to_f10(float c) {
  int s = (int) lroundf(c * 18.0f) + 320;
  if (fabsf(c - floorf(c) - 0.5f) < 0.01f)
    s += 1;
  return s;
}

float f10_to_c(int f) { return (f / 10.0f - 32.0f) * 5.0f / 9.0f; }
float round_half(float c) { return roundf(c * 2) / 2; }

uint8_t level_of_set(int set) { return (uint8_t) std::max(10, std::min(60, (int) lroundf(f10_to_c(set) * 2))); }

float shown_temp(int f10) { return g_fahrenheit ? roundf(f10 / 10.0f) : round_half(f10_to_c(f10)); }

static bool g_12h = false;
void set_time_12h(bool on) { g_12h = on; }
bool time_12h() { return g_12h; }

std::string format_time(int minute_of_day) {
  char buf[24];
  unsigned md = (unsigned) (((minute_of_day % 1440) + 1440) % 1440);
  unsigned h = md / 60, m = md % 60;
  if (g_12h)
    snprintf(buf, sizeof(buf), "%u:%02u%s", h % 12 == 0 ? 12u : h % 12, m, h < 12 ? "AM" : "PM");
  else
    snprintf(buf, sizeof(buf), "%02u:%02u", h, m);
  return buf;
}
const char *unit_text() { return g_fahrenheit ? "°F" : "°C"; }

// ---------------------------------------------------------------------------
// Frames

uint16_t crc16(const std::vector<uint8_t> &d, size_t from, size_t to) {
  uint16_t c = 0xFFFF;
  for (size_t i = from; i < to; i++) {
    c ^= d[i];
    for (int b = 0; b < 8; b++)
      c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1;
  }
  return c;
}

int take_timer_tag(std::vector<uint8_t> &report) {
  if (report.size() != 39 || report[10] != 0xdf || report[22] != 0x5b)
    return 0;
  int minutes = report[23] << 8 | report[24];
  report.erase(report.begin() + 22, report.begin() + 25);
  return minutes;
}

// From the command byte on: tag + value, the tag's top 2 bits giving the value
// length (1-4 bytes); a 00 tag ends the list. Anything left over is shown as
// hex after '|'.
std::string decode_tags(const std::vector<uint8_t> &p) {
  std::string out;
  char buf[8];
  if (p.size() < 13)
    return out;
  size_t i = 10, end = p.size() - 2;
  while (i < end && p[i] != 0x00) {
    int n = (p[i] >> 6) + 1;
    if (i + 1 + n > end)
      break;
    snprintf(buf, sizeof(buf), "%s%02x=", out.empty() ? "" : " ", p[i]);
    out += buf;
    for (int k = 1; k <= n; k++) {
      snprintf(buf, sizeof(buf), "%02x", p[i + k]);
      out += buf;
    }
    i += 1 + n;
  }
  if (i < end) {
    out += " |";
    for (; i < end; i++) {
      snprintf(buf, sizeof(buf), " %02x", p[i]);
      out += buf;
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Schedule: "mon-fri 06:30 comfort 08:00 eco; sat,sun 08:00/21 23:00/17.5"
// Groups separated by ';' or newlines. Each group: days (mon..sun, ranges with
// '-', lists with ',', or "daily"), then "HH:MM temperature" pairs (space or
// '/' between). A temperature is a number, "comfort" / "eco" (or "reduced":
// the preset temperatures) or "off"; it holds until the next switch point,
// across midnight. In HA each weekday has its own text, without the days.
// The same grammar is checked at compile time in __init__.py (schedule()).

static int day_index(const std::string &s) {
  for (int i = 0; i < 7; i++)
    if (s == DAYS[i])
      return i;
  return -1;
}

static std::vector<std::string> split(const std::string &s, const char *seps) {
  std::vector<std::string> out;
  std::string cur;
  for (char ch : s) {
    if (strchr(seps, ch) != nullptr) {
      if (!cur.empty())
        out.push_back(cur);
      cur.clear();
    } else {
      cur += ch;
    }
  }
  if (!cur.empty())
    out.push_back(cur);
  return out;
}

// Like split(), but keeps empty parts ("mon,,tue" -> "mon", "", "tue").
static std::vector<std::string> split_keep(const std::string &s, char sep) {
  std::vector<std::string> out(1);
  for (char ch : s) {
    if (ch == sep)
      out.emplace_back();
    else
      out.back() += ch;
  }
  return out;
}

static bool parse_days(const std::string &spec, bool days[7], std::string &error) {
  for (int i = 0; i < 7; i++)
    days[i] = false;
  for (const auto &part : split_keep(spec, ',')) {
    if (part == "daily" || part == "all") {
      for (int i = 0; i < 7; i++)
        days[i] = true;
      continue;
    }
    size_t dash = part.find('-');
    if (dash == std::string::npos) {
      int d = day_index(part);
      if (d < 0) {
        error = "unknown day '" + part + "'";
        return false;
      }
      days[d] = true;
      continue;
    }
    int a = day_index(part.substr(0, dash)), b = day_index(part.substr(dash + 1));
    if (a < 0 || b < 0) {
      error = "unknown day range '" + part + "'";
      return false;
    }
    for (int d = a;; d = (d + 1) % 7) {
      days[d] = true;
      if (d == b)
        break;
    }
  }
  return true;
}

// "H:MM" or "HH:MM" (00:00-23:59), or with "am" / "pm" attached ("7:00am",
// "06:15pm"; hour 1-12, 12am = midnight, 12pm = noon). Lower case: the
// schedule text is lowered first.
static bool parse_time(std::string s, int &hh, int &mm) {
  int ampm = 0;  // 0 none, 1 am, 2 pm
  if (s.size() > 2 && (s.compare(s.size() - 2, 2, "am") == 0 || s.compare(s.size() - 2, 2, "pm") == 0)) {
    ampm = s[s.size() - 2] == 'a' ? 1 : 2;
    s.resize(s.size() - 2);
  }
  size_t colon = s.find(':');
  if (colon == std::string::npos || colon < 1 || colon > 2 || s.size() != colon + 3)
    return false;
  for (size_t i = 0; i < s.size(); i++)
    if (i != colon && !isdigit((unsigned char) s[i]))
      return false;
  hh = atoi(s.substr(0, colon).c_str());
  mm = atoi(s.substr(colon + 1).c_str());
  if (mm > 59)
    return false;
  if (ampm == 0)
    return hh <= 23;
  if (hh < 1 || hh > 12)
    return false;
  hh = (hh % 12) + (ampm == 2 ? 12 : 0);
  return true;
}

// A plain decimal number: "21", "17.5" (no sign, exponent or hex).
static bool parse_number(const std::string &s, float &v) {
  size_t dot = s.find('.');
  if (s.empty() || s.find('.', dot == std::string::npos ? 0 : dot + 1) != std::string::npos)
    return false;
  for (size_t i = 0; i < s.size(); i++)
    if (i != dot && !isdigit((unsigned char) s[i]))
      return false;
  if (dot == 0 || dot == s.size() - 1)
    return false;
  v = strtof(s.c_str(), nullptr);
  return true;
}

bool parse_schedule(const std::string &text, std::vector<SchedulePoint> &out, std::string &error) {
  out.clear();
  std::string t = text;
  std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return (char) std::tolower(c); });
  for (const auto &group : split(t, ";\n")) {
    auto tok = split(group, " \t\r/");
    if (tok.empty())
      continue;
    bool days[7];
    if (!parse_days(tok[0], days, error))
      return false;
    if (tok.size() < 3 || tok.size() % 2 == 0) {
      error = "'" + tok[0] + "' needs \"HH:MM temperature\" pairs";
      return false;
    }
    for (size_t i = 1; i + 1 < tok.size(); i += 2) {
      int hh, mm;
      if (!parse_time(tok[i], hh, mm)) {
        error = "bad time '" + tok[i] + "'";
        return false;
      }
      const std::string &v = tok[i + 1];
      uint8_t level;
      if (v == "off") {
        level = PT_OFF;
      } else if (v == "comfort") {
        level = PT_COMFORT;
      } else if (v == "reduced" || v == "eco") {
        level = PT_REDUCED;
      } else {
        float c;
        bool ok = parse_number(v, c);
        if (ok && g_fahrenheit)
          c = (c - 32.0f) * 5.0f / 9.0f;  // F typed; kept in 0.5 C steps (0.9 F, so whole F come back exactly)
        if (!ok || c < 4.99f || c > 30.01f) {
          error = "bad temperature '" + v +
                  (g_fahrenheit ? "' (41-86, comfort, eco or off)" : "' (5-30, comfort, eco or off)");
          return false;
        }
        level = (uint8_t) std::max(10L, std::min(60L, lroundf(c * 2)));
      }
      for (int d = 0; d < 7; d++) {
        if (!days[d])
          continue;
        uint16_t minute = d * 1440 + hh * 60 + mm;
        for (const auto &p : out) {
          if (p.minute == minute) {
            error = std::string("two temperatures at ") + DAYS[d] + " " + tok[i];
            return false;
          }
        }
        out.push_back({minute, level});
      }
    }
  }
  if (out.size() > MAX_POINTS) {
    error = "more than " + std::to_string(MAX_POINTS) + " switch points a week";
    return false;
  }
  size_t per_day[7] = {};
  for (const auto &p : out) {
    if (++per_day[p.minute / 1440] > MAX_DAY_POINTS) {
      error = "more than " + std::to_string(MAX_DAY_POINTS) + " switch points a day";
      return false;
    }
  }
  std::sort(out.begin(), out.end(), [](const SchedulePoint &a, const SchedulePoint &b) { return a.minute < b.minute; });
  return true;
}

bool parse_day(const std::string &text, int day, std::vector<SchedulePoint> &out, std::string &error) {
  out.clear();
  if (split(text, " \t\r\n/;").empty())
    return true;  // no switch point this day
  if (text.find(';') != std::string::npos || text.find('\n') != std::string::npos) {
    error = "one day only: \"HH:MM/temperature ...\"";
    return false;
  }
  return parse_schedule(std::string(DAYS[day]) + " " + text, out, error);
}

std::string format_day(const std::vector<SchedulePoint> &points, int day) {
  std::string out;
  char buf[16];
  for (const auto &p : points) {
    if (p.minute / 1440 != day)
      continue;
    if (!out.empty())
      out += ' ';
    out += format_time(p.minute % 1440);
    out += '/';
    switch (p.level) {
      case PT_OFF:
        out += "off";
        break;
      case PT_COMFORT:
        out += "comfort";
        break;
      case PT_REDUCED:
        out += "eco";
        break;
      default:
        if (g_fahrenheit)
          snprintf(buf, sizeof(buf), "%ld", lroundf(p.level / 2.0f * 9.0f / 5.0f + 32.0f));
        else
          snprintf(buf, sizeof(buf), (p.level % 2) ? "%d.5" : "%d", p.level / 2);
        out += buf;
        break;
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Temporary changes in Auto

uint16_t next_switch_minute(const std::vector<SchedulePoint> &schedule, uint16_t now_minute) {
  for (const auto &p : schedule)
    if (p.minute > now_minute)
      return p.minute;
  return schedule.front().minute;  // wraps round to next week
}

bool override_expired(int start, int until, uint16_t now_minute) {
  if (start < 0)
    return false;  // not anchored yet (no clock when it was made)
  int span = (until - start + MINUTES_A_WEEK) % MINUTES_A_WEEK;
  if (span == 0)
    span = MINUTES_A_WEEK - 1;  // made at the week's only switch point (a full week would never be reached)
  int elapsed = ((int) now_minute - start + MINUTES_A_WEEK) % MINUTES_A_WEEK;
  // Up to 2 h before the start: the clock was set back (end of daylight saving
  // time), not almost a week gone by.
  static const int CLOCK_BACK = 120;
  if (elapsed >= MINUTES_A_WEEK - CLOCK_BACK && span < MINUTES_A_WEEK - CLOCK_BACK)
    return false;
  return elapsed >= span;
}

}  // namespace esphome::clevertouch_radio
