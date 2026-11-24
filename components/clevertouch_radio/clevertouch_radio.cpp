#include "clevertouch_radio.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "esphome/core/alloc_helpers.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::clevertouch_radio {

static const char *const TAG = "clevertouch_radio";

// Another controller (the E3) must have been silent this long before this firmware transmits.
// (How long it only listens after boot is listen_after_boot, 5 min by default.)
static const uint32_t SILENCE_MS = 300000;
static const uint32_t CHANGE_TIMEOUT_MS = 90000;
static const uint32_t PAIRING_MS = 120000;
// The E3 accepts a join request ~530 ms after it; the radiator then acks and
// reports at once (docs/PROTOCOL.md, Pairing). Accepted after 120 ms, a radiator paired
// but stayed silent until its next regular report.
static const uint32_t ACCEPT_DELAY_MS = 530;
static const uint32_t NOTICE_MS = 60000;
static const float NOMINAL_HZ = 868.35e6f;
// Front panel temperature limit 90 / 75 / 60 C, sent as the power limit in e1.
static const uint8_t LIMITS[3] = {0x64, 0x46, 0x37};
static const int SET_OFF = 0x140;                 // 0 C, sent with "off"
static const uint32_t TIMEOUT_SLOT = 0x59a10000;  // + radiator: its answer
static const uint32_t POLL_SLOT = 100;            // + radiator: its poll
static const uint32_t ACCEPT_SLOT = 200;
// Radiators being changed at the same time are polled one after the other, a
// few per second, so their reports and the answers don't collide.
static const uint32_t POLL_SPACING_MS = 200;
static const uint32_t POLLS_A_TICK = 5;
// Reports come every 2.5-4 min.
static const uint32_t REPORTING_MS = 1200000;

// ---------------------------------------------------------------------------
// Helpers

static void append_crc(std::vector<uint8_t> &e) {
  uint16_t c = crc16(e, 0, e.size());
  e.push_back(c & 0xff);
  e.push_back(c >> 8);
}

static void put_u32(std::vector<uint8_t> &v, uint32_t x) {
  v.push_back(x >> 24);
  v.push_back(x >> 16);
  v.push_back(x >> 8);
  v.push_back(x);
}

static uint32_t get_u32(const std::vector<uint8_t> &p, size_t i) {
  return (uint32_t) p[i] << 24 | (uint32_t) p[i + 1] << 16 | (uint32_t) p[i + 2] << 8 | p[i + 3];
}

static const char *const DAY_NAMES[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};

static const char *mode_name(uint8_t b) {
  switch (b) {
    case 0x00:
      return "comfort";
    case 0x01:
      return "off";
    case 0x02:
      return "frost protection";
    case 0x03:
      return "reduced";
    case 0x04:
      return "timer";
    case 0x08:
      return "auto (comfort)";
    case 0x0b:
      return "auto (reduced)";
    default:
      return "unknown";
  }
}

static RadMode mode_of_byte(uint8_t b) {
  switch (b) {
    case 0x01:
      return RAD_OFF;
    case 0x02:
      return RAD_FROST;
    case 0x03:
      return RAD_REDUCED;
    case 0x08:
    case 0x0b:
      return RAD_AUTO;
    default:
      return RAD_COMFORT;
  }
}

static uint32_t points_hash(const std::vector<SchedulePoint> &pts) {
  std::string b;
  for (const auto &p : pts) {
    b += (char) (p.minute >> 8);
    b += (char) (p.minute & 0xff);
    b += (char) p.level;
  }
  return fnv1_hash(b);
}

// ---------------------------------------------------------------------------
// Entities

void ClevertouchScheduleText::control(const std::string &value) { this->radiator_->on_day_text(this->day_, value); }
void ClevertouchLimitSelect::control(size_t index) { this->radiator_->on_limit_index(index); }
void ClevertouchTempNumber::control(float value) { this->radiator_->on_temp_number(this->kind_, value); }
void ClevertouchPairSwitch::write_state(bool state) { this->radiator_->on_pair_switch(state); }
void ClevertouchSeqSwitch::write_state(bool state) { this->radiator_->on_seq_switch(state); }
void ClevertouchRecalButton::press_action() { this->hub_->recalibrate(); }

// ---------------------------------------------------------------------------
// Radiator

void ClevertouchRadiator::setup_radiator(uint32_t hash) {
  this->store_pref_ = global_preferences->make_preference<RadiatorStore>(hash ^ 0x5101, true);
  bool loaded = this->store_pref_.load(&this->store_);
  if (!loaded || this->store_.valid != 1) {
    uint32_t paired = loaded ? this->store_.radio_id : 0;
    this->store_ = RadiatorStore{};
    this->store_.radio_id = paired;
    this->store_.comfort_set = c_to_f10(this->default_comfort_);
    this->store_.reduced_set = c_to_f10(this->default_reduced_);
    this->store_.frost_set = c_to_f10(this->default_frost_);
    this->store_.mode = RAD_COMFORT;
    this->store_.valid = 0;  // take the radiator's own state from its first report
  }

  // Schedule: the stored points, else the YAML.
  this->points_pref_ = global_preferences->make_preference<PointsStore>(hash ^ 0x5105, true);
  PointsStore ps{};
  if (this->points_pref_.load(&ps) && ps.n <= MAX_POINTS) {
    this->schedule_.assign(ps.pts, ps.pts + ps.n);
  } else {
    std::string err;
    if (!parse_schedule(this->default_schedule_, this->schedule_, err)) {
      ESP_LOGW(TAG, "%s: schedule not valid (%s), cleared", this->label_, err.c_str());
      this->schedule_.clear();
    }
    this->save_schedule_();
  }
  this->sched_hash_ = points_hash(this->schedule_);
  this->publish_day_texts_();

  // A temporary change in Auto from before the reboot (dropped once its switch point has passed).
  this->override_pref_ = global_preferences->make_preference<OverrideStore>(hash ^ 0x5104, true);
  OverrideStore ov{};
  if (this->override_pref_.load(&ov) && ov.valid == 1 && ov.sched_hash == this->sched_hash_ &&
      (ov.byte == 0x08 || ov.byte == 0x0b)) {
    this->override_ = true;
    this->override_level_ = ov.level;
    this->override_byte_ = ov.byte;
    this->override_start_ = ov.start;
    this->override_until_ = ov.until;
  }

  // A timer set on the radiator (its end is checked in refresh_).
  this->timer_pref_ = global_preferences->make_preference<TimerStore>(hash ^ 0x5108, true);
  TimerStore ts{};
  if (this->timer_pref_.load(&ts)) {
    this->timer_end_ = ts.end;
    this->timer_level_ = ts.level;
  }

  // Energy as a double: a float stops adding small amounts after a few thousand kWh.
  this->energy_pref_ = global_preferences->make_preference<double>(hash ^ 0x5107, true);
  if (!this->energy_pref_.load(&this->kwh_) || !std::isfinite(this->kwh_))
    this->kwh_ = 0;
  this->kwh_saved_ = this->kwh_;
  if (this->energy_sensor_ != nullptr)
    this->energy_sensor_->publish_state(this->kwh_);

  // min/max temperature may have changed in the YAML since these were stored.
  this->store_.comfort_set = this->clamp_set_(this->store_.comfort_set);
  this->store_.reduced_set = this->clamp_set_(this->store_.reduced_set);
  this->store_.frost_set = this->clamp_set_(this->store_.frost_set);

  this->offset_pref_ = global_preferences->make_preference<int16_t>(hash ^ 0x5106, true);
  if (!this->offset_pref_.load(&this->offset_f10_))
    this->offset_f10_ = 0;
  if (this->offset_number_ != nullptr)
    this->offset_number_->publish_state(unit_fahrenheit() ? this->offset_f10_ / 10.0f
                                                          : roundf(this->offset_f10_ / 1.8f) / 10.0f);

  this->save_();  // publishes the temperature numbers (and stores the defaults on a first boot)

  if (this->limit_select_ != nullptr) {
    for (size_t i = 0; i < 3; i++)
      if (this->store_.limit == LIMITS[i])
        this->limit_select_->publish_state(i);
  }
  if (this->pair_switch_ != nullptr)
    this->pair_switch_->publish_state(false);
  this->publish_radio_id_();
  this->publish_climate_();
}

void ClevertouchRadiator::publish_radio_id_() {
  if (this->radio_id_sensor_ == nullptr)
    return;
  uint32_t id = this->radio_id();
  std::string s = id == 0 ? "not paired" : str_sprintf("%08X", (unsigned) id);
  if (id != 0 && this->config_radio_id_ == 0)
    s += " (paired here, not in the YAML)";
  this->radio_id_sensor_->publish_state(s);
}

int ClevertouchRadiator::clamp_set_(int set) const {
  return std::max(c_to_f10(this->min_c_), std::min(c_to_f10(this->max_c_), set));
}

void ClevertouchRadiator::set_offset_f10_(int16_t f10) {
  if (f10 == this->offset_f10_)
    return;
  this->offset_f10_ = f10;
  this->offset_pref_.save(&this->offset_f10_);
  if (this->offset_number_ != nullptr)
    this->offset_number_->publish_state(unit_fahrenheit() ? f10 / 10.0f : roundf(f10 / 1.8f) / 10.0f);
}

climate::ClimateTraits ClevertouchRadiator::traits() {
  auto traits = climate::ClimateTraits();
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE | climate::CLIMATE_SUPPORTS_ACTION);
  traits.set_supported_modes({climate::CLIMATE_MODE_OFF, climate::CLIMATE_MODE_HEAT, climate::CLIMATE_MODE_AUTO});
  traits.set_supported_presets(
      {climate::CLIMATE_PRESET_COMFORT, climate::CLIMATE_PRESET_ECO, climate::CLIMATE_PRESET_AWAY});
  traits.set_visual_min_temperature(this->min_c_);
  traits.set_visual_max_temperature(this->max_c_);
  traits.set_visual_target_temperature_step(0.5f);
  traits.set_visual_current_temperature_step(0.1f);
  return traits;
}

void ClevertouchRadiator::control(const climate::ClimateCall &call) {
  if (this->hub_->e3_active()) {
    ESP_LOGW(TAG, "%s: another controller (the E3) is in charge; change ignored", this->label_);
    this->publish_climate_();
    return;
  }
  uint16_t minute;
  ESPTime t;
  bool time_ok = this->hub_->now_(minute, t);
  auto mode = call.get_mode();
  auto preset = call.get_preset();
  auto temp = call.get_target_temperature();
  this->clear_timer_();
  if (mode.has_value()) {
    switch (*mode) {
      case climate::CLIMATE_MODE_OFF:
        this->store_.mode = RAD_OFF;
        break;
      case climate::CLIMATE_MODE_HEAT:
        // Heat keeps its preset; from off or auto it starts at comfort.
        if (this->store_.mode == RAD_OFF || this->store_.mode == RAD_AUTO)
          this->store_.mode = RAD_COMFORT;
        break;
      case climate::CLIMATE_MODE_AUTO:
        // Also ends a temporary change: back to the schedule.
        this->store_.mode = RAD_AUTO;
        this->clear_override_();
        break;
      default:
        break;
    }
  }
  if (preset.has_value()) {
    // In Auto, comfort / eco hold until the next switch point; away leaves Auto.
    bool in_auto = this->store_.mode == RAD_AUTO;
    switch (*preset) {
      case climate::CLIMATE_PRESET_COMFORT:
        if (in_auto)
          this->set_override_(PT_COMFORT, 0x08, time_ok, minute);
        else
          this->store_.mode = RAD_COMFORT;
        break;
      case climate::CLIMATE_PRESET_ECO:
        if (in_auto)
          this->set_override_(PT_REDUCED, 0x0b, time_ok, minute);
        else
          this->store_.mode = RAD_REDUCED;
        break;
      case climate::CLIMATE_PRESET_AWAY:
        this->store_.mode = RAD_FROST;
        this->clear_override_();
        break;
      default:
        break;
    }
  }
  if (temp.has_value()) {
    // Like the radiator's buttons: the temperature of the preset in force.
    int s = c_to_f10(std::max(this->min_c_, std::min(this->max_c_, round_half(*temp))));
    switch (this->store_.mode) {
      case RAD_AUTO:
        // Holds until the next switch point of the schedule.
        this->set_override_(level_of_set(s), this->auto_byte_(s), time_ok, minute);
        break;
      case RAD_REDUCED:
        this->store_.reduced_set = s;
        break;
      case RAD_FROST:
        this->store_.frost_set = s;
        break;
      default:  // comfort, and off (shows the comfort temperature)
        this->store_.comfort_set = s;
        break;
    }
  }
  this->store_.valid = 1;
  this->save_();
  this->hub_->refresh_(this, time_ok, minute);
  this->publish_climate_();
}

void ClevertouchRadiator::save_() {
  this->store_pref_.save(&this->store_);
  const uint16_t sets[3] = {this->store_.comfort_set, this->store_.reduced_set, this->store_.frost_set};
  for (int i = 0; i < 3; i++) {
    auto *n = this->temp_numbers_[i];
    float c = shown_temp(sets[i]);
    if (n != nullptr && (!n->has_state() || n->state != c))
      n->publish_state(c);
  }
}

void ClevertouchRadiator::publish_climate_() {
  RadMode m;
  int set;
  if (this->hub_->e3_active() && this->e3_valid_) {
    m = mode_of_byte(this->e3_byte_);
    set = this->e3_set_;
  } else {
    m = (RadMode) this->store_.mode;
    set = this->cur_valid_ ? this->cur_set_ : this->store_.comfort_set;
  }
  auto preset = climate::CLIMATE_PRESET_NONE;
  switch (m) {
    case RAD_OFF:
      this->mode = climate::CLIMATE_MODE_OFF;
      set = this->store_.comfort_set;
      break;
    case RAD_AUTO: {
      // The preset shows the level in force: the temporary change, else the schedule's.
      this->mode = climate::CLIMATE_MODE_AUTO;
      int level = -1;
      uint16_t minute;
      ESPTime t;
      if (this->hub_->e3_active() && this->e3_valid_)
        level = this->e3_byte_ == 0x08 ? PT_COMFORT : PT_REDUCED;
      else if (this->override_)
        level = this->override_level_;
      else if (!this->schedule_.empty() && this->hub_->now_(minute, t))
        level = this->schedule_[this->active_point_(minute)].level;
      if (level == PT_COMFORT)
        preset = climate::CLIMATE_PRESET_COMFORT;
      else if (level == PT_REDUCED)
        preset = climate::CLIMATE_PRESET_ECO;
      break;
    }
    case RAD_REDUCED:
      this->mode = climate::CLIMATE_MODE_HEAT;
      preset = climate::CLIMATE_PRESET_ECO;
      break;
    case RAD_FROST:
      this->mode = climate::CLIMATE_MODE_HEAT;
      preset = climate::CLIMATE_PRESET_AWAY;
      break;
    default:
      this->mode = climate::CLIMATE_MODE_HEAT;
      preset = climate::CLIMATE_PRESET_COMFORT;
      break;
  }
  if (set == SET_OFF)  // a schedule's "off"
    set = this->store_.comfort_set;
  // A timer, in any mode (also off): its temperature, no preset.
  bool timer = !(this->hub_->e3_active() && this->e3_valid_) && this->timer_end_ != 0;
  if (timer) {
    if (m == RAD_OFF)
      this->mode = climate::CLIMATE_MODE_HEAT;
    preset = climate::CLIMATE_PRESET_NONE;
    set = this->point_set_(this->timer_level_);
  }
  this->set_preset_(preset);
  this->target_temperature = round_half(f10_to_c(set));
  this->current_temperature = this->room_c_;
  if (m == RAD_OFF && !timer)
    this->action = climate::CLIMATE_ACTION_OFF;
  else
    this->action = this->power_ > 0 ? climate::CLIMATE_ACTION_HEATING : climate::CLIMATE_ACTION_IDLE;
  this->publish_state();
}

int ClevertouchRadiator::active_point_(uint16_t now_minute) const {
  int best = (int) this->schedule_.size() - 1;  // before the week's first point: the last one still holds
  for (size_t i = 0; i < this->schedule_.size(); i++) {
    if (this->schedule_[i].minute > now_minute)
      break;
    best = (int) i;
  }
  return best;
}

int ClevertouchRadiator::point_set_(uint8_t level) const {
  switch (level) {
    case PT_OFF:
      return SET_OFF;
    case PT_COMFORT:
      return this->store_.comfort_set;
    case PT_REDUCED:
      return this->store_.reduced_set;
    default:
      return this->clamp_set_(c_to_f10(level / 2.0f));
  }
}

// The radiator shows a comfort or a reduced symbol in Auto. For a number in
// the schedule (or a temperature set in Auto): comfort for the schedule's
// highest temperature (or warmer), reduced for everything lower.
uint8_t ClevertouchRadiator::auto_byte_(int set) const {
  int hi = INT_MIN;
  for (const auto &p : this->schedule_)
    if (p.level != PT_OFF)
      hi = std::max(hi, this->point_set_(p.level));
  if (hi == INT_MIN)
    return this->cur_byte_ == 0x0b ? 0x0b : 0x08;
  return set >= hi - 1 ? 0x08 : 0x0b;
}

ClevertouchRadiator::Desired ClevertouchRadiator::compute_desired_(bool time_ok, uint16_t now_minute) {
  if (this->timer_end_ != 0)  // in any mode
    return {this->point_set_(this->timer_level_), 0x04};
  switch (this->store_.mode) {
    case RAD_OFF:
      return {SET_OFF, 0x01};
    case RAD_COMFORT:
      return {this->store_.comfort_set, 0x00};
    case RAD_REDUCED:
      return {this->store_.reduced_set, 0x03};
    case RAD_FROST:
      return {this->store_.frost_set, 0x02};
    default:
      break;
  }
  if (this->override_)
    return {this->point_set_(this->override_level_), this->override_byte_};
  if (!this->schedule_.empty() && time_ok) {
    uint8_t level = this->schedule_[this->active_point_(now_minute)].level;
    switch (level) {
      case PT_OFF:
        return {SET_OFF, 0x01};
      case PT_COMFORT:
        return {this->store_.comfort_set, 0x08};
      case PT_REDUCED:
        return {this->store_.reduced_set, 0x0b};
      default: {
        int s = this->point_set_(level);
        return {s, this->auto_byte_(s)};
      }
    }
  }
  // No schedule (or no clock yet): hold what the radiator has (not a timer's temperature).
  if (this->cur_valid_ && this->cur_set_ != SET_OFF && this->cur_byte_ != 0x04)
    return {this->cur_set_, (uint8_t) ((this->cur_byte_ == 0x0b) ? 0x0b : 0x08)};
  return {this->store_.comfort_set, 0x08};
}

void ClevertouchRadiator::adopt_(int set, uint8_t byte, bool local, bool time_ok, uint16_t now_minute,
                                 bool keep_presets, int timer_min) {
  // Kept within min/max; the radiator keeps what it reported (cur_set_ below),
  // so a value outside the limits is pushed back to the limit.
  int raw = set;
  set = this->clamp_set_(set);
  // 0 C with a mode other than off isn't a preset temperature.
  bool learn = !keep_presets && raw != SET_OFF;
  this->clear_timer_();
  if (byte != 0x04)
    this->clear_override_();  // also "auto" chosen again: back to the schedule
  switch (byte) {
    case 0x00:
      this->store_.mode = RAD_COMFORT;
      if (learn)
        this->store_.comfort_set = set;
      break;
    case 0x01:
      this->store_.mode = RAD_OFF;
      break;
    case 0x02:
      this->store_.mode = RAD_FROST;
      if (learn)
        this->store_.frost_set = set;
      break;
    case 0x03:
      this->store_.mode = RAD_REDUCED;
      if (learn)
        this->store_.reduced_set = set;
      break;
    case 0x08:
    case 0x0b:
      this->store_.mode = RAD_AUTO;
      // The E3's auto sends its comfort / reduced temperatures: learn them. (Not
      // after "auto" chosen on the radiator: it reports the last setpoint it took,
      // whatever that was for.)
      if (!local && learn) {
        if (byte == 0x08)
          this->store_.comfort_set = set;
        else
          this->store_.reduced_set = set;
      }
      break;
    case 0x04:
      // Timer: this temperature for that long (1 h to 44 days), then back to the
      // mode before. What a radiator makes of a temperature changed in Auto. "no"
      // there (0 min): no timer, the mode before stays.
      if (timer_min > 0)
        this->set_timer_(level_of_set(set), timer_min);
      break;
    default:
      ESP_LOGW(TAG, "%s: mode %02x (%s) is not supported yet; back to comfort", this->label_, byte, mode_name(byte));
      this->store_.mode = RAD_COMFORT;
      break;
  }
  this->store_.valid = 1;
  this->save_();
  this->cur_valid_ = true;
  this->cur_set_ = raw;
  this->cur_byte_ = byte;
  this->pend_ms_ = 0;
}

void ClevertouchRadiator::add_energy_(float seconds) {
  if (seconds <= 0)
    return;
  this->kwh_ += seconds * this->watts_ / 3.6e6;
  this->save_energy_(false);
  if (this->energy_sensor_ != nullptr)
    this->energy_sensor_->publish_state(this->kwh_);
}

// Flash: once 10 Wh have added up, else 10 min after the last write (checked
// from tick_ too, so nothing waits while the radiator is idle). At a planned
// restart (force) everything, so the total doesn't step back.
void ClevertouchRadiator::save_energy_(bool force) {
  if (this->kwh_ == this->kwh_saved_)
    return;
  uint32_t now = millis();
  if (!force && this->kwh_ - this->kwh_saved_ < 0.01 && now - this->kwh_save_ms_ < 600000)
    return;
  this->energy_pref_.save(&this->kwh_);
  this->kwh_saved_ = this->kwh_;
  this->kwh_save_ms_ = now;
}

void ClevertouchRadiator::on_day_text(int day, const std::string &value) {
  auto *txt = this->day_texts_[day];
  std::vector<SchedulePoint> pts;
  std::string err;
  if (!parse_day(value, day, pts, err)) {
    ESP_LOGW(TAG, "%s: %s schedule not accepted: %s", this->label_, DAYS[day], err.c_str());
    txt->publish_state(format_day(this->schedule_, day));
    return;
  }
  // The rest of the week stays; this day's points are replaced.
  std::vector<SchedulePoint> week;
  std::vector<SchedulePoint> old_day;
  for (const auto &p : this->schedule_)
    (p.minute / 1440 == day ? old_day : week).push_back(p);
  bool same = old_day.size() == pts.size();
  for (size_t i = 0; same && i < pts.size(); i++)
    same = old_day[i].minute == pts[i].minute && old_day[i].level == pts[i].level;
  if (same) {
    // Written again unchanged (e.g. a sync from HA): keep a temporary change.
    txt->publish_state(format_day(this->schedule_, day));
    return;
  }
  if (week.size() + pts.size() > MAX_POINTS) {
    ESP_LOGW(TAG, "%s: %s schedule not accepted: more than %u switch points a week", this->label_, DAYS[day],
             (unsigned) MAX_POINTS);
    txt->publish_state(format_day(this->schedule_, day));
    return;
  }
  week.insert(week.end(), pts.begin(), pts.end());
  std::sort(week.begin(), week.end(),
            [](const SchedulePoint &a, const SchedulePoint &b) { return a.minute < b.minute; });
  this->schedule_ = week;
  this->save_schedule_();
  this->sched_hash_ = points_hash(this->schedule_);
  txt->publish_state(format_day(this->schedule_, day));
  this->clear_override_();
  ESP_LOGI(TAG, "%s: %s schedule set (%u switch points a week)", this->label_, DAYS[day],
           (unsigned) this->schedule_.size());
  uint16_t minute;
  ESPTime t;
  bool time_ok = this->hub_->now_(minute, t);
  this->hub_->refresh_(this, time_ok, minute);
}

void ClevertouchRadiator::on_limit_index(size_t index) {
  if (index >= 3)
    return;
  if (this->hub_->e3_active()) {
    ESP_LOGW(TAG, "%s: another controller (the E3) is in charge; change ignored", this->label_);
    auto ai = this->limit_select_->active_index();
    if (ai.has_value())
      this->limit_select_->publish_state(*ai);
    return;
  }
  this->store_.limit = LIMITS[index];
  this->save_();
  this->limit_select_->publish_state(index);
  if (this->reported_limit_ != this->store_.limit)
    this->pend_ms_ = std::max<uint32_t>(1, millis());
}

void ClevertouchRadiator::on_temp_number(TempKind kind, float value) {
  if (this->hub_->e3_active()) {
    ESP_LOGW(TAG, "%s: another controller (the E3) is in charge; change ignored", this->label_);
    auto *n = kind == TEMP_OFFSET ? this->offset_number_ : this->temp_numbers_[kind];
    n->publish_state(n->state);
    return;
  }
  if (kind == TEMP_OFFSET) {
    // Sent in every answer; push it now (the radiator doesn't echo it).
    this->set_offset_f10_((int16_t) lroundf(value * (unit_fahrenheit() ? 10.0f : 18.0f)));
    ESP_LOGI(TAG, "%s: temperature offset %.1f %s", this->label_, value, unit_fahrenheit() ? "F" : "C");
    this->pend_ms_ = std::max<uint32_t>(1, millis());
    return;
  }
  // In 0.5 C steps, as the radiator keeps it: it would round anything else, and
  // its reports would never match what was sent.
  int s = c_to_f10(round_half(unit_fahrenheit() ? (value - 32.0f) * 5.0f / 9.0f : value));
  s = std::max(c_to_f10(this->min_c_), std::min(c_to_f10(this->max_c_), s));
  switch (kind) {
    case TEMP_COMFORT:
      this->store_.comfort_set = s;
      break;
    case TEMP_REDUCED:
      this->store_.reduced_set = s;
      break;
    default:
      this->store_.frost_set = s;
      break;
  }
  this->save_();
  // Pushed at once if the radiator is in that preset (or at that level of its schedule).
  uint16_t minute;
  ESPTime t;
  bool time_ok = this->hub_->now_(minute, t);
  this->hub_->refresh_(this, time_ok, minute);
  this->publish_climate_();
}

void ClevertouchRadiator::save_schedule_() {
  PointsStore ps{};
  ps.n = (uint8_t) std::min(this->schedule_.size(), MAX_POINTS);
  for (size_t i = 0; i < ps.n; i++)
    ps.pts[i] = this->schedule_[i];
  this->points_pref_.save(&ps);
}

void ClevertouchRadiator::publish_day_texts_() {
  for (int d = 0; d < 7; d++)
    if (this->day_texts_[d] != nullptr)
      this->day_texts_[d]->publish_state(format_day(this->schedule_, d));
}

void ClevertouchRadiator::set_override_(uint8_t level, uint8_t byte, bool time_ok, uint16_t now_minute) {
  this->override_ = true;
  this->override_level_ = level;
  this->override_byte_ = byte;
  this->override_start_ = -1;
  this->override_until_ = -1;
  if (time_ok && !this->schedule_.empty())
    this->anchor_override_(now_minute);
  this->save_override_();
}

void ClevertouchRadiator::anchor_override_(uint16_t now_minute) {
  this->override_start_ = (int16_t) now_minute;
  this->override_until_ = (int16_t) next_switch_minute(this->schedule_, now_minute);
}

bool ClevertouchRadiator::override_expired_(uint16_t now_minute) const {
  return override_expired(this->override_start_, this->override_until_, now_minute);
}

void ClevertouchRadiator::clear_override_() {
  if (!this->override_)
    return;
  this->override_ = false;
  this->save_override_();
}

void ClevertouchRadiator::save_override_() {
  OverrideStore ov{};
  ov.valid = this->override_ ? 1 : 0;
  ov.level = this->override_level_;
  ov.byte = this->override_byte_;
  ov.start = this->override_start_;
  ov.until = this->override_until_;
  ov.sched_hash = this->sched_hash_;
  this->override_pref_.save(&ov);
}

void ClevertouchRadiator::set_timer_(uint8_t level, int minutes) {
  uint32_t now = this->hub_->utc_();
  if (now == 0)
    return;  // no clock yet
  this->timer_end_ = now + minutes * 60;
  this->timer_level_ = level;
  this->save_timer_();
}

void ClevertouchRadiator::clear_timer_() {
  if (this->timer_end_ == 0)
    return;
  this->timer_end_ = 0;
  this->save_timer_();
}

void ClevertouchRadiator::save_timer_() {
  TimerStore ts{};
  ts.end = this->timer_end_;
  ts.level = this->timer_level_;
  this->timer_pref_.save(&ts);
}

// "timer 17.0 °C until 12:57", "... until Fri 12:57", "... until 2027-01-08 12:57".
std::string ClevertouchRadiator::timer_text_() const {
  ESPTime end = ESPTime::from_epoch_local(this->timer_end_);
  std::string when = format_time(end.hour * 60 + end.minute);
  uint32_t now = this->hub_->utc_();
  ESPTime today = ESPTime::from_epoch_local(now);
  if (now == 0 || this->timer_end_ > now + 6 * 86400)
    when = str_sprintf("%04d-%02d-%02d ", (int) end.year, (int) end.month, (int) end.day_of_month) + when;
  else if (end.day_of_week != today.day_of_week)
    when = std::string(DAY_NAMES[(end.day_of_week + 5) % 7]) + " " + when;
  return "timer " + this->level_text_(this->timer_level_) + " until " + when;
}

std::string ClevertouchRadiator::level_text_(uint8_t level) const {
  char buf[24];
  switch (level) {
    case PT_OFF:
      return "off";
    case PT_COMFORT:
      snprintf(buf, sizeof(buf), unit_fahrenheit() ? "comfort %.0f %s" : "comfort %.1f %s",
               shown_temp(this->store_.comfort_set), unit_text());
      return buf;
    case PT_REDUCED:
      snprintf(buf, sizeof(buf), unit_fahrenheit() ? "eco %.0f %s" : "eco %.1f %s",
               shown_temp(this->store_.reduced_set), unit_text());
      return buf;
    default:
      if (unit_fahrenheit())
        snprintf(buf, sizeof(buf), "%.0f %s", level / 2.0f * 9.0f / 5.0f + 32.0f, unit_text());
      else
        snprintf(buf, sizeof(buf), "%.1f %s", level / 2.0f, unit_text());
      return buf;
  }
}

void ClevertouchRadiator::update_schedule_status_(bool time_ok, uint16_t now_minute) {
  if (this->schedule_status_ == nullptr)
    return;
  std::string s;
  if (this->hub_->e3_active()) {
    s = "E3 in charge";
  } else if (this->timer_end_ != 0) {
    s = this->timer_text_();
  } else {
    switch (this->store_.mode) {
      case RAD_OFF:
        s = "off (manual)";
        break;
      case RAD_COMFORT:
        s = "comfort (manual)";
        break;
      case RAD_REDUCED:
        s = "eco (manual)";
        break;
      case RAD_FROST:
        s = "away (manual)";
        break;
      default:
        if (this->schedule_.empty()) {
          s = this->override_ ? "override " + this->level_text_(this->override_level_) + " (no schedule)"
                              : "auto without a schedule: holding";
        } else if (!time_ok) {
          s = "waiting for the clock";
        } else {
          int a = this->active_point_(now_minute);
          uint16_t next = this->schedule_[(a + 1) % this->schedule_.size()].minute;
          std::string when = format_time(next % 1440);
          if (!(next / 1440 == now_minute / 1440 && next > now_minute))
            when = std::string(DAY_NAMES[next / 1440]) + " " + when;
          s = this->override_ ? "override " + this->level_text_(this->override_level_)
                              : this->level_text_(this->schedule_[a].level);
          s += " until ";
          s += when;
        }
        break;
    }
  }
  if (!this->schedule_status_->has_state() || this->schedule_status_->state != s)
    this->schedule_status_->publish_state(s);
}

void ClevertouchRadiator::on_seq_switch(bool on) {
  if (this->hub_->e3_active()) {
    ESP_LOGW(TAG, "%s: another controller (the E3) is in charge; change ignored", this->label_);
    this->seq_switch_->publish_state(!on);
    return;
  }
  ESP_LOGI(TAG, "%s: sequential control %s", this->label_, on ? "on" : "off");
  this->seq_want_ = on ? 1 : 0;
  this->seq_switch_->publish_state(on);
  this->pend_ms_ = std::max<uint32_t>(1, millis());
}

void ClevertouchRadiator::on_pair_switch(bool on) {
  if (on)
    this->hub_->start_pairing(this);
  else
    this->hub_->cancel_pairing(this);
}

// ---------------------------------------------------------------------------
// Hub

void ClevertouchHub::setup() {
  this->boot_ms_ = millis();
  // "Control radiators" as its restore_mode says (by default: off after a first
  // flash, then the last state); a switch only restores when asked to.
  if (this->control_switch_ != nullptr)
    this->control_switch_->publish_state(this->control_switch_->get_initial_state_with_restore_mode().value_or(false));
  // The stored-state keys keep the component's first name ("yali_radio_..."):
  // changing them would make every existing install forget its schedules,
  // pairings, temperatures and frequency correction.
  this->cal_pref_ = global_preferences->make_preference<CalStore>(fnv1_hash("yali_radio_cal"), true);
  if (!this->cal_pref_.load(&this->cal_))
    this->cal_ = {0, false};
  this->scan_base_ = this->cal_.ready ? this->cal_.corr : 0;
  this->ctrl_pref_ = global_preferences->make_preference<uint32_t>(fnv1_hash("yali_radio_ctrl"), true);
  if (!this->ctrl_pref_.load(&this->learned_controller_id_))
    this->learned_controller_id_ = 0;
  this->publish_controller_id_();
  for (auto *r : this->radiators_)
    r->setup_radiator(fnv1_hash(std::string("yali_radio_") + r->label()));
  this->radio_->register_listener(this);
  if (this->shadow_ok_sensor_ != nullptr)
    this->shadow_ok_sensor_->publish_state(0);
  if (this->shadow_bad_sensor_ != nullptr)
    this->shadow_bad_sensor_->publish_state(0);
  if (this->missed_sensor_ != nullptr)
    this->missed_sensor_->publish_state(0);
  if (this->freq_sensor_ != nullptr)
    this->freq_sensor_->publish_state(this->cal_.corr / 1000);
  this->set_interval("tick", 1000, [this]() { this->tick_(); });
}

void ClevertouchHub::dump_config() {
  ESP_LOGCONFIG(TAG, "CleverTouch radio:");
  if (this->config_controller_id_ != 0) {
    ESP_LOGCONFIG(TAG, "  Controller ID: %08X", (unsigned) this->config_controller_id_);
  } else {
    ESP_LOGCONFIG(TAG, "  Controller ID: %08X (learned)", (unsigned) this->learned_controller_id_);
  }
  ESP_LOGCONFIG(TAG, "  Reply delay: %u ms", (unsigned) this->reply_delay_ms_);
  ESP_LOGCONFIG(TAG, "  Frequency correction: %.1f kHz (%s)", this->cal_.corr / 1000,
                this->cal_.ready ? "calibrated" : "not calibrated yet");
  for (auto *r : this->radiators_) {
    ESP_LOGCONFIG(TAG, "  Radiator %s: radio ID %08X, %.0f W, %s panel, %u schedule points", r->label(),
                  (unsigned) r->radio_id(), r->watts_, r->front_share_ < 1.0f ? "double" : "single",
                  (unsigned) r->schedule_.size());
  }
}

// e3_ever_ is cleared once the E3 has been silent for SILENCE_MS (tick_), so the
// unsigned difference can't wrap round to "recent" 49.7 days later.
bool ClevertouchHub::e3_active() const { return this->e3_ever_ && millis() - this->e3_last_ms_ < SILENCE_MS; }

bool ClevertouchHub::may_transmit(bool log_reason) {
  const char *why = nullptr;
  if (this->control_switch_ == nullptr || !this->control_switch_->state)
    why = "control is off";
  else if (this->e3_active())
    why = "another controller (the E3) was heard in the last 5 min";
  else if (this->listening_)
    why = "listening after boot";
  else if (!this->time_->now().is_valid())
    why = "no clock yet (the answers carry the time)";
  if (why != nullptr && log_reason)
    ESP_LOGW(TAG, "not transmitting: %s", why);
  return why == nullptr;
}

bool ClevertouchHub::now_(uint16_t &minute_of_week, ESPTime &t) {
  t = this->time_->now();
  if (!t.is_valid()) {
    minute_of_week = 0;
    return false;
  }
  int day = (t.day_of_week + 5) % 7;  // ESPTime: 1 = Sunday; here 0 = Monday
  minute_of_week = day * 1440 + t.hour * 60 + t.minute;
  return true;
}

uint32_t ClevertouchHub::utc_() const {
  ESPTime t = this->time_->now();
  return t.is_valid() ? (uint32_t) t.timestamp : 0;
}

uint32_t ClevertouchHub::own_id_() {
  if (this->config_controller_id_ != 0)
    return this->config_controller_id_;
  if (this->learned_controller_id_ != 0)
    return this->learned_controller_id_;
  uint8_t mac[6];
  get_mac_address_raw(mac);
  return 0xf1000000 | (uint32_t) mac[3] << 16 | (uint32_t) mac[4] << 8 | mac[5];
}

void ClevertouchHub::publish_controller_id_() {
  if (this->controller_id_sensor_ == nullptr)
    return;
  const char *src = this->config_controller_id_ != 0    ? "from the YAML"
                    : this->learned_controller_id_ != 0 ? "learned"
                                                        : "from the MAC, not used yet";
  this->controller_id_sensor_->publish_state(str_sprintf("%08X (%s)", (unsigned) this->own_id_(), src));
}

ClevertouchRadiator *ClevertouchHub::find_(uint32_t radio_id) {
  if (radio_id == 0)
    return nullptr;
  for (auto *r : this->radiators_)
    if (r->radio_id() == radio_id)
      return r;
  return nullptr;
}

void ClevertouchHub::refresh_(ClevertouchRadiator *r, bool time_ok, uint16_t minute) {
  if (this->e3_active() || !r->store_.valid)
    return;  // the E3 decides (mirrored), or the radiator's state isn't known yet
  if (r->timer_end_ != 0 && time_ok && this->utc_() >= r->timer_end_) {
    r->clear_timer_();
    ESP_LOGI(TAG, "%s: timer over", r->label());
  }
  if (r->override_ && time_ok && !r->schedule_.empty()) {
    if (r->override_start_ < 0) {
      // Made before the clock was set: it holds until the next switch point from now.
      r->anchor_override_(minute);
      r->save_override_();
    } else if (r->override_expired_(minute)) {
      r->clear_override_();
      ESP_LOGI(TAG, "%s: next switch point reached, back to the schedule", r->label());
    }
  }
  auto d = r->compute_desired_(time_ok, minute);
  if (r->cur_valid_ && d.set == r->cur_set_ && d.byte == r->cur_byte_)
    return;
  r->cur_valid_ = true;
  r->cur_set_ = d.set;
  r->cur_byte_ = d.byte;
  r->pend_ms_ = std::max<uint32_t>(1, millis());
  ESP_LOGI(TAG, "%s: now %.1f C, %s", r->label(), d.byte == 0x01 ? 0.0f : round_half(f10_to_c(d.set)),
           mode_name(d.byte));
  r->publish_climate_();
}

// Runs before ESPHome's own write of the preferences to flash (shutdown goes
// from the lowest setup priority up).
void ClevertouchHub::on_shutdown() {
  for (auto *r : this->radiators_)
    r->save_energy_(true);
}

void ClevertouchHub::tick_() {
  uint32_t now = millis();
  uint16_t minute;
  ESPTime t;
  bool time_ok = this->now_(minute, t);
  // Latched timers (millis() wraps every 49.7 days).
  if (this->listening_ && now - this->boot_ms_ >= this->listen_after_boot_ms_)
    this->listening_ = false;
  if (this->e3_ever_ && now - this->e3_last_ms_ >= SILENCE_MS) {
    this->e3_ever_ = false;
    for (auto *r : this->radiators_)
      r->e3_valid_ = false;
  }
  if (!this->notice_.empty() && (int32_t) (now - this->notice_until_) >= 0)
    this->notice_.clear();
  for (auto *r : this->radiators_)
    r->save_energy_(false);
  bool tx = this->may_transmit();

  // Status, and the climate entities switch between mirroring the E3 and own state.
  std::string st;
  if (this->pairing_ != nullptr)
    st = std::string("pairing ") + this->pairing_->label() + ": put the radiator in pairing mode";
  else if (!this->notice_.empty())
    st = this->notice_;
  else if (this->e3_active())
    st = "E3 active (monitoring)";
  else if (this->control_switch_ == nullptr || !this->control_switch_->state)
    st = "monitoring (control off)";
  else if (this->listening_)
    st = "listening after boot";
  else if (!time_ok)
    st = "waiting for the clock";
  else
    st = "controlling";
  if (st != this->last_status_) {
    bool mirror_changed = (this->last_status_ == "E3 active (monitoring)") != (st == "E3 active (monitoring)");
    this->last_status_ = st;
    ESP_LOGI(TAG, "status: %s", st.c_str());
    if (this->status_sensor_ != nullptr)
      this->status_sensor_->publish_state(st);
    if (mirror_changed)
      for (auto *r : this->radiators_)
        r->publish_climate_();
  }

  // Frequency: the stored correction 30 s after boot; if nothing valid is heard
  // for 10 min (radiators report every 2.5-4 min), scan +-150 kHz, 5 min each.
  if (!this->cal_applied_) {
    if (now - this->boot_ms_ > 30000) {
      this->cal_applied_ = true;
      this->last_rx_ms_ = now;
      this->scan_ms_ = now;
      ESP_LOGI(TAG, "frequency correction %.1f kHz (%s)", this->cal_.corr / 1000,
               this->cal_.ready ? "stored" : "not calibrated yet");
      this->retune_(NOMINAL_HZ + this->cal_.corr);
    }
  } else if (now - this->last_rx_ms_ > 600000 && now - this->scan_ms_ > 300000) {
    static const float SCAN[5] = {0, -75e3f, 75e3f, -150e3f, 150e3f};
    if (this->cal_.ready)
      this->scan_base_ = this->cal_.corr;  // the stored correction stays in flash until a new one is found
    this->scan_i_ = (this->scan_i_ + 1) % 5;
    this->cal_ = {std::max(-200e3f, std::min(200e3f, this->scan_base_ + SCAN[this->scan_i_])), false};
    this->scan_ms_ = now;
    ESP_LOGW(TAG, "nothing received for %u s, scanning: correction %.0f kHz",
             (unsigned) ((now - this->last_rx_ms_) / 1000), this->cal_.corr / 1000);
    this->retune_(NOMINAL_HZ + this->cal_.corr);
  }

  // After boot, once the frequency correction is in: poll each radiator not
  // heard yet, one every 2 s, 3 rounds at most, so its values show up in
  // seconds instead of with its next report (2.5-4 min). A poll only makes the
  // radiator report (an E3 would answer it as usual; tested). Not while an E3
  // is heard, and this firmware still doesn't answer while listening after boot.
  size_t boot_polls = 3 * this->radiators_.size();
  if (this->cal_applied_ && this->boot_poll_n_ < boot_polls && now - this->boot_poll_ms_ >= 2000 &&
      this->control_switch_ != nullptr && this->control_switch_->state && !this->e3_active()) {
    while (this->boot_poll_n_ < boot_polls) {
      auto *r = this->radiators_[this->boot_poll_n_++ % this->radiators_.size()];
      if (r->radio_id() != 0 && r->last_df_ms_ == 0) {
        ESP_LOGD(TAG, "%s: polling after boot", r->label());
        this->send_poll_(r, 0);
        this->boot_poll_ms_ = now;
        break;
      }
    }
  }

  // Desired state per radiator, and the change procedure (as the E3 does it):
  // poll (19) about once a second until the radiator reports (the report is
  // answered with the new state), repeat that answer until the 01 ack, stop
  // when a report shows the new state. Gives up after 90 s; the next regular
  // report is answered with the new state anyway.
  // Starts with another radiator each second, so one that doesn't answer can't
  // hold up the others.
  uint32_t polls = 0;
  this->tick_n_++;
  for (size_t k = 0; k < this->radiators_.size(); k++) {
    auto *r = this->radiators_[(this->tick_n_ + k) % this->radiators_.size()];
    this->refresh_(r, time_ok, minute);
    r->update_schedule_status_(time_ok, minute);
    // Reporting: heard in the last 20 min. Unknown until a report comes or 20
    // min have passed since boot. Forgotten then, so that it can't look recent
    // again when millis() wraps.
    if (r->last_df_ms_ != 0 && now - r->last_df_ms_ >= REPORTING_MS)
      r->last_df_ms_ = 0;
    if (now - this->boot_ms_ > REPORTING_MS)
      this->reporting_known_ = true;
    if (r->reporting_sensor_ != nullptr && r->radio_id() != 0) {
      bool heard = r->last_df_ms_ != 0;
      if ((heard || this->reporting_known_) &&
          (!r->reporting_sensor_->has_state() || r->reporting_sensor_->state != heard)) {
        if (!heard)
          ESP_LOGW(TAG, "%s: no report for 20 min (out of range, switched off, or no longer paired?)", r->label());
        r->reporting_sensor_->publish_state(heard);
      }
    }
    if (r->join_ms_ != 0) {
      // Just paired: it should report within ~2 s by itself; if not, poll it
      // (like a change, also in the first 5 min after boot).
      if (r->last_df_ms_ != 0 && (int32_t) (r->last_df_ms_ - r->join_ms_) > 0) {
        r->join_ms_ = 0;
      } else if (now - r->join_ms_ > 30000) {
        ESP_LOGW(TAG, "%s: no report since pairing; it shows up with its next regular report (2.5-4 min)", r->label());
        r->join_ms_ = 0;
      } else if (now - r->join_ms_ > 2500 && this->control_switch_ != nullptr && this->control_switch_->state &&
                 !this->e3_active()) {
        this->send_poll_(r, 0);
      }
    }
    if (r->pend_ms_ == 0 || r->radio_id() == 0)
      continue;
    if (!tx) {
      // Listening after boot, control off or an E3 active: the 90 s start when
      // this may transmit.
      r->pend_ms_ = std::max<uint32_t>(1, now);
      continue;
    }
    // Signed: a change from the schedule is stamped in refresh_() above, after "now".
    if ((int32_t) (now - r->pend_ms_) > (int32_t) CHANGE_TIMEOUT_MS) {
      ESP_LOGW(TAG, "%s: change not confirmed in 90 s; it goes with the next report", r->label());
      r->pend_ms_ = 0;
      continue;
    }
    if (now - r->last_tx_ms_ < 1000 || now - r->last_df_ms_ < 1000)
      continue;  // a cycle is running
    if (polls >= POLLS_A_TICK)
      continue;  // the others in the next ticks
    uint32_t delay = POLL_SPACING_MS * polls++;
    bool answered = r->last_tx_ms_ != 0 && (int32_t) (r->last_tx_ms_ - r->pend_ms_) >= 0;
    if (answered && !r->acked_ && !r->last_45_.empty()) {
      ESP_LOGD(TAG, "%s: no ack yet, repeating the answer", r->label());
      r->last_tx_ms_ = now;
      this->send_after_(r, r->last_45_, delay);
    } else {
      this->send_poll_(r, delay);
    }
  }

  // Pairing beacon, once a second, alternating the two forms the E3 sends.
  if (this->pairing_ != nullptr) {
    if ((int32_t) (now - this->pairing_until_) > 0) {
      this->stop_pairing_("no radiator joined in 2 min");
    } else if (this->control_switch_ == nullptr || !this->control_switch_->state) {
      this->stop_pairing_("\"Control radiators\" was turned off");
    } else if (this->e3_active()) {
      this->stop_pairing_("another controller (the E3) was heard");
    } else {
      uint32_t id = this->own_id_();
      std::vector<uint8_t> e;
      if (this->beacon_n_ & 1) {
        put_u32(e, id);
        e.insert(e.end(), {0xc6, 0xf1, 0x10, 0x00, this->beacon_n_});
      } else {
        put_u32(e, (id & 0x00ffffff) | 0xd0000000);
        e.insert(e.end(), {0xc6, 0xd0, 0x00, 0x00, this->beacon_n_});
      }
      e.insert(e.end(), {0x00, 0x00, 0x03, 0x04});
      append_crc(e);
      this->radio_->transmit_packet(e);
      this->beacon_n_ = this->beacon_n_ == 255 ? 1 : this->beacon_n_ + 1;
    }
  }

  // Devices heard that aren't configured: radiators with their setpoint (for
  // finding radio IDs), anything else with its commands (for adding support).
  if (this->unassigned_sensor_ != nullptr && now - this->last_list_ms_ > 60000) {
    this->last_list_ms_ = now;
    std::string s;
    for (auto it = this->unknown_.begin(); it != this->unknown_.end();) {
      const Heard &h = it->second;
      if (now - h.ms > 1800000) {
        it = this->unknown_.erase(it);
        continue;
      }
      std::string item = str_sprintf("%s%08X ", s.empty() ? "" : ", ", (unsigned) it->first);
      if (h.has_df) {
        item += str_sprintf("%.1f%s %s", shown_temp(h.set), unit_fahrenheit() ? "F" : "C", mode_name(h.byte));
      } else {
        item += "cmd";
        for (uint8_t i = 0; i < h.n_cmds; i++)
          item += str_sprintf("%s%02x", i ? "," : " ", h.cmds[i]);
        if ((h.to & 0x00ffffff) == 0x00ffffff)
          item += " broadcast";
      }
      if (s.size() + item.size() <= 255)
        s += item;
      ++it;
    }
    if (s.empty())
      s = "none";
    if (this->unassigned_sensor_->state != s)
      this->unassigned_sensor_->publish_state(s);
  }
}

void ClevertouchHub::on_packet(const std::vector<uint8_t> &packet, float freq_offset, float rssi, uint8_t lqi) {
  // [len, sender(4), c6, receiver(4), cmd, ..., crc lo, crc hi]
  std::vector<uint8_t> p;
  p.reserve(packet.size() + 1);
  p.push_back((uint8_t) packet.size());
  p.insert(p.end(), packet.begin(), packet.end());
  if (p.size() < 13) {
    ESP_LOGD(TAG, "short %s", format_hex_pretty(p).c_str());
    return;
  }
  uint16_t c = crc16(p, 1, p.size() - 2);
  bool ok = c == (p[p.size() - 2] | p[p.size() - 1] << 8);
  ESP_LOGD(TAG, "%s %s  rssi %.0f lqi %u  foff %.1f kHz", ok ? "rx " : "BAD", format_hex_pretty(p).c_str(), rssi, lqi,
           freq_offset / 1000.0f);
  if (!ok)
    return;
  if (p[5] == 0xc6 && this->cal_applied_)
    this->last_rx_ms_ = millis();

  uint32_t from = get_u32(p, 1), to = get_u32(p, 6);
  uint8_t cmd = p[10];
  if (this->pairing_ != nullptr && p.size() == 14 && cmd == 0x00 && to == this->own_id_()) {
    this->handle_join_(from);
    return;
  }
  ClevertouchRadiator *r = this->find_(from);
  if (r != nullptr) {
    // Frequency calibration from our radiators only: they are the reference
    // (hearing them, and being heard by them); an E3 or another ESP sits at its
    // own crystal's offset.
    if (p[5] == 0xc6)
      this->calibrate_(freq_offset);
    this->handle_radiator_(r, to, p, rssi);
    return;
  }
  ClevertouchRadiator *dst = this->find_(to);
  if (dst != nullptr) {
    // Another controller talking to one of our radiators (we never hear our own
    // transmissions): an E3, or another ESP configured with the same radiator.
    // It is in charge. Traffic to radiators we don't have is ignored, so
    // several ESPs can each control their own set of radiators.
    this->controller_heard_(from, dst, "heard talking to");
    if (cmd == 0x45 && p.size() == 57 && p[34] == 0x94 && p[38] == 0xe1 && p[51] == 0xb2)
      this->handle_e3_answer_(dst, p);
    return;
  }
  // Not devices: our controller ID (another ESP answering its own radiators,
  // or an E3 talking to radiators we don't have) and its "d0" pairing beacon.
  if (from == this->own_id_() || from == ((this->own_id_() & 0x00ffffff) | 0xd0000000))
    return;
  if (std::find(this->ignored_.begin(), this->ignored_.end(), from) != this->ignored_.end())
    return;
  this->note_unknown_(from, to, p);
}

void ClevertouchHub::controller_heard_(uint32_t id, ClevertouchRadiator *r, const char *how) {
  if (!this->e3_active())
    ESP_LOGI(TAG, "controller %08X %s %s: monitoring only while it is active", (unsigned) id, how, r->label());
  this->e3_last_ms_ = millis();
  this->e3_ever_ = true;
  // It is in charge: drop answers that are still waiting for their reply delay.
  for (size_t i = 0; i < this->radiators_.size(); i++) {
    auto *a = this->radiators_[i];
    if (!a->answer_queued_)
      continue;
    this->cancel_timeout(TIMEOUT_SLOT + i);
    a->answer_queued_ = false;
    a->pending_s_ = a->answered_s_;  // not sent: the other controller's answer counts it
    a->answered_s_ = 0;
    a->acked_ = true;
    a->last_tx_ms_ = 0;
  }
}

void ClevertouchHub::handle_radiator_(ClevertouchRadiator *r, uint32_t to, const std::vector<uint8_t> &frame,
                                      float rssi) {
  uint32_t now = millis();
  std::vector<uint8_t> p = frame;
  int timer_min = take_timer_tag(p);
  if (r->signal_sensor_ != nullptr)
    r->signal_sensor_->publish_state(rssi);
  uint8_t cmd = p[10];
  // 01 = the radiator acknowledges an answer. Ours if we answered just now:
  // its heating seconds count now (it has reset its counters).
  if (cmd == 0x01) {
    if (r->last_tx_ms_ != 0 && now - r->last_tx_ms_ < 3000) {
      r->local_seen_ = false;
      if (!r->acked_) {
        r->acked_ = true;
        r->add_energy_(r->answered_s_);
        r->answered_s_ = 0;
        ESP_LOGD(TAG, "%s: answer acknowledged (%u ms after the report)", r->label(),
                 (unsigned) (now - r->last_tx_ms_));
      }
    } else if ((to & 0x00ffffff) != 0x00ffffff && r->join_ms_ == 0) {
      // Not ours: another controller answered, also if that wasn't heard here.
      // (An unpaired radiator broadcasts an 01, and one follows pairing.)
      this->controller_heard_(to, r, "answered");
      bool reported = r->last_df_ms_ != 0 && now - r->last_df_ms_ < 2000;
      if (reported && (r->last_ans_ms_ == 0 || now - r->last_ans_ms_ > 3000)) {
        r->add_energy_(r->pending_s_);
        r->pending_s_ = 0;
        r->last_ans_ms_ = now;
      }
    }
    return;
  }
  if (cmd != 0xdf)
    return;
  // Read by fixed offsets: check the tag layout first (another radiator model
  // could send its tags in another order); never answer what isn't understood.
  if (p.size() != 36 || p[15] != 0x4b || p[18] != 0x8a || p[22] != 0xe1 || p[27] != 0xda) {
    if (!r->bad_layout_logged_) {
      r->bad_layout_logged_ = true;
      ESP_LOGW(TAG, "%s: report not in the known layout, not answered: %s", r->label(), decode_tags(p).c_str());
    }
    return;
  }
  // The controller this radiator reports to: only from its reports, never from
  // a broadcast (an unpaired radiator announces itself to xxFFFFFF).
  if ((to & 0x00ffffff) != 0x00ffffff) {
    r->ctrl_id_ = to;
    if (this->config_controller_id_ == 0 && this->learned_controller_id_ == 0) {
      this->learned_controller_id_ = to;
      this->ctrl_pref_.save(&this->learned_controller_id_);
      ESP_LOGI(TAG, "controller ID learned from %s: %08X", r->label(), (unsigned) to);
      this->publish_controller_id_();
    }
  }

  // Status report: df 20 3x 0y 40 | 4b room | 8a set mode | e1 00 limit delivered% asked% | da c1 c2 | 0a 47
  r->last_df_ms_ = now;
  r->last_df_ = p;
  float room = f10_to_c(p[16] << 8 | p[17]);
  int set = p[19] << 8 | p[20];
  uint8_t byte = p[21];
  // Heating seconds since the last answered report: counter 1 at the delivered
  // (limited) power, counter 2 at the asked power. A single panel is all front
  // panel; on a double panel the limit throttles only the front panel, so the
  // back panel counts at the asked power. They count once the radiator confirms
  // our answer to this report (it resets them when it gets the answer).
  int c1 = p[28] << 8 | p[29];
  int c2 = p[30] << 8 | p[31];
  if (!r->acked_) {
    if (c1 == 0 && c2 == 0 && r->last_tx_ms_ != 0 && now - r->last_tx_ms_ < 3000) {
      // The confirmation after our answer (counters reset): it arrived, though
      // its 01 wasn't heard here.
      r->acked_ = true;
      r->local_seen_ = false;
      r->add_energy_(r->answered_s_);
    }
    // Otherwise the answer didn't arrive: these counters still include those seconds.
    r->answered_s_ = 0;
  }
  r->pending_s_ = r->front_share_ * c1 + (1.0f - r->front_share_) * c2;
  r->room_c_ = roundf(room * 10) / 10;
  r->power_ = p[25];
  r->reported_limit_ = p[24];
  bool seq = p[23] & 0x01;
  if (r->seq_want_ >= 0 && seq == (r->seq_want_ == 1))
    r->seq_want_ = -1;
  if (r->seq_switch_ != nullptr && r->seq_want_ < 0 && (!r->seq_switch_->has_state() || r->seq_switch_->state != seq))
    r->seq_switch_->publish_state(seq);
  if (r->room_sensor_ != nullptr)
    r->room_sensor_->publish_state(unit_fahrenheit() ? (p[16] << 8 | p[17]) / 10.0f : r->room_c_);
  if (r->power_sensor_ != nullptr)
    r->power_sensor_->publish_state(p[25]);
  if (r->mode_sensor_ != nullptr && r->mode_sensor_->state != mode_name(byte))
    r->mode_sensor_->publish_state(mode_name(byte));
  if (r->limit_select_ != nullptr && r->store_.limit == 0) {
    auto ai = r->limit_select_->active_index();
    for (size_t i = 0; i < 3; i++)
      if (p[24] == LIMITS[i] && (!ai.has_value() || *ai != i))
        r->limit_select_->publish_state(i);
  }
  ESP_LOGI(TAG, "%s: room %.1f C, set %.1f C, %s, power %d %%, heated %d s", r->label(), room, f10_to_c(set),
           mode_name(byte), p[25], c1);

  uint16_t minute;
  ESPTime t;
  bool time_ok = this->now_(minute, t);
  // Bit 0 of df[12]: setpoint/mode changed on the radiator's own buttons. The
  // radiator wins (the E3 does the same).
  bool local = p[12] & 0x01;
  // It repeats that report until it takes an answer (which took some seconds in
  // a test): still the same change, adopted once.
  bool again = local && r->local_seen_ && set == r->local_set_ && byte == r->local_byte_;
  r->local_seen_ = local;
  r->local_set_ = set;
  r->local_byte_ = byte;
  if (local && byte == 0x04 && !time_ok)
    r->local_seen_ = false;  // a timer can't start yet: with its next report
  if (!r->store_.valid || (local && !again)) {
    if (local)
      ESP_LOGI(TAG, "%s: changed on the radiator itself", r->label());
    // A mode chosen at its buttons comes with the temperature it was last sent
    // for that mode (e.g. a number of the schedule): the preset's own is used.
    // Another one was set there along with the mode.
    int taken = r->taken_set_[mode_of_byte(byte)];
    bool chosen =
        local && r->store_.valid && mode_of_byte(byte) != r->store_.mode && (taken == 0 || abs(set - taken) <= 1);
    // A radiator just paired brings its own default temperatures: the YAML's stay.
    r->adopt_(set, byte, local, time_ok, minute, chosen || (r->new_pairing_ && !local), timer_min);
    // The answer below carries what follows from it (e.g. the schedule's
    // temperature after "auto"), not the radiator's own 0 C.
    if (local)
      this->refresh_(r, time_ok, minute);
  }
  r->new_pairing_ = false;
  // Auto's two levels are comfort's and reduced's on the radiator.
  if (!local && set != SET_OFF && byte != 0x04)
    r->taken_set_[byte == 0x08 ? RAD_COMFORT : byte == 0x0b ? RAD_REDUCED : mode_of_byte(byte)] = set;
  r->publish_climate_();

  // The radiator rounds the setpoint by 1 (582 -> 581), so allow +-1.
  bool match = abs(set - r->cur_set_) <= 1 && byte == r->cur_byte_ &&
               (r->store_.limit == 0 || p[24] == r->store_.limit) && r->seq_want_ < 0;
  if (!this->may_transmit()) {
    // E.g. listening after boot: the radiator already has the desired state.
    if (match)
      r->pend_ms_ = 0;
    return;
  }
  if (!r->cur_valid_)
    return;  // nothing to send yet (the next tick sets it)
  bool recent = r->last_tx_ms_ != 0 && now - r->last_tx_ms_ < 2000;
  if (match && r->pend_ms_ != 0) {
    ESP_LOGI(TAG, "%s: change confirmed (%.1f s)", r->label(), std::max(0, (int) (now - r->pend_ms_)) / 1000.0f);
    r->pend_ms_ = 0;
  }
  // Like the E3: answer every report, except the confirmation that follows our
  // own answer within ~0.6 s and already shows the desired state.
  if (recent && match && !local && r->acked_)
    return;
  this->answer_(r, p, t);
}

void ClevertouchHub::answer_(ClevertouchRadiator *r, const std::vector<uint8_t> &df, const ESPTime &t) {
  uint32_t ctrl = r->ctrl_id_ != 0 ? r->ctrl_id_ : this->own_id_();
  uint8_t limit = r->store_.limit != 0 ? r->store_.limit : df[24];
  uint8_t e1b = df[23];
  if (r->seq_want_ >= 0)
    e1b = (uint8_t) ((e1b & 0xfe) | r->seq_want_);
  int ds = r->cur_set_;
  std::vector<uint8_t> e;
  e.reserve(56);
  put_u32(e, ctrl);
  e.push_back(0xc6);
  put_u32(e, r->radio_id());
  uint16_t off = (uint16_t) r->offset_f10_;
  uint32_t timer = 120;  // minutes: what the E3 sends without a timer, else what is left of it
  uint32_t utc = this->utc_();
  if (r->cur_byte_ == 0x04 && r->timer_end_ > utc)
    timer = std::min<uint32_t>(0xffff, (r->timer_end_ - utc + 59) / 60);
  // One tag per line.
  // clang-format off
  e.insert(e.end(), {0x45, (uint8_t) (off >> 8), (uint8_t) off,                           // temperature offset
                     0x4b, df[16], df[17],                                                // room temperature echo
                     0x3b, 0x00,
                     0x4c, 0x00, 0x00,
                     0x8e, 0x00, 0x00, 0x84,
                     0xcc, 0x00, 0x00, 0x00, 0x00,
                     0x97, df[11], (uint8_t) (df[12] & 0xfe), (uint8_t) (df[13] | 0x04),  // local-change bit cleared
                     0x94, (uint8_t) (ds >> 8), (uint8_t) ds, r->cur_byte_,               // setpoint + mode
                     0xe1, e1b, limit, df[25], df[26],                                    // sequential, limit
                     0xda, 0x00, 0x00, 0x00, 0x00,
                     0x5b, (uint8_t) (timer >> 8), (uint8_t) timer,
                     0xb2, t.hour, t.minute, t.second});                                  // clock
  // clang-format on
  append_crc(e);
  // The radiator resets its counters when it gets this answer; they count
  // once it confirms (01, or a report with reset counters).
  r->answered_s_ = r->pending_s_;
  r->pending_s_ = 0;
  r->acked_ = false;
  r->last_45_ = e;
  r->last_tx_ms_ = millis();
  if (this->reply_delay_ms_ == 0) {
    this->send_(e);
    return;
  }
  // Dropped if another controller answers first (controller_heard_).
  r->answer_queued_ = true;
  this->set_timeout(TIMEOUT_SLOT + this->index_(r), this->reply_delay_ms_, [this, r]() {
    r->answer_queued_ = false;
    this->send_(r->last_45_);
  });
}

void ClevertouchHub::send_poll_(ClevertouchRadiator *r, uint32_t delay_ms) {
  std::vector<uint8_t> e;
  put_u32(e, r->ctrl_id_ != 0 ? r->ctrl_id_ : this->own_id_());
  e.push_back(0xc6);
  put_u32(e, r->radio_id());
  e.insert(e.end(), {0x19, 0x00});
  append_crc(e);
  this->send_after_(r, e, delay_ms);
}

// Within the tick; not any more if another controller or a report of this
// radiator came first.
void ClevertouchHub::send_after_(ClevertouchRadiator *r, const std::vector<uint8_t> &frame, uint32_t delay_ms) {
  if (delay_ms == 0) {
    this->send_(frame);
    return;
  }
  this->set_timeout(TIMEOUT_SLOT + POLL_SLOT + this->index_(r), delay_ms, [this, r, frame]() {
    if (!this->e3_active() && (r->last_df_ms_ == 0 || millis() - r->last_df_ms_ >= 1000))
      this->send_(frame);
  });
}

void ClevertouchHub::send_(const std::vector<uint8_t> &frame) {
  if (this->control_switch_ == nullptr || !this->control_switch_->state)
    return;
  auto err = this->radio_->transmit_packet(frame);
  if (err != cc1101::CC1101Error::NONE) {
    ESP_LOGW(TAG, "transmit failed (error %d)", (int) err);
  } else {
    ESP_LOGD(TAG, "sent %s", format_hex_pretty(frame).c_str());
  }
}

uint32_t ClevertouchHub::index_(ClevertouchRadiator *r) const {
  return std::find(this->radiators_.begin(), this->radiators_.end(), r) - this->radiators_.begin();
}

void ClevertouchHub::handle_e3_answer_(ClevertouchRadiator *r, const std::vector<uint8_t> &p) {
  uint32_t now = millis();
  // While the E3 is in charge its setpoint/mode (94) is the state; carried
  // over so control continues from it once the E3 is gone.
  int eset = p[35] << 8 | p[36];
  uint8_t eb = p[37];
  if (!r->e3_valid_ || r->e3_set_ != eset || r->e3_byte_ != eb) {
    r->e3_valid_ = true;
    r->e3_set_ = eset;
    r->e3_byte_ = eb;
    uint16_t minute;
    ESPTime t;
    bool time_ok = this->now_(minute, t);
    r->adopt_(eset, eb, false, time_ok, minute, false, p[49] << 8 | p[50]);
    r->publish_climate_();
  }
  int16_t eoff = (int16_t) (p[11] << 8 | p[12]);
  if (eoff != r->offset_f10_) {
    ESP_LOGI(TAG, "%s: the E3's temperature offset %.1f C taken over", r->label(), eoff / 18.0f);
    r->set_offset_f10_(eoff);
  }
  if (r->limit_select_ != nullptr && r->store_.limit != p[40]) {
    for (size_t i = 0; i < 3; i++) {
      if (p[40] == LIMITS[i]) {
        r->store_.limit = p[40];
        r->save_();
        r->limit_select_->publish_state(i);
      }
    }
  }

  uint32_t prev = r->last_ans_ms_;
  uint32_t dt = now - prev;
  r->last_ans_ms_ = now;
  bool heard =
      r->last_df_ms_ != 0 && now - r->last_df_ms_ < 2000 && (prev == 0 || (int32_t) (r->last_df_ms_ - prev) > 0);
  if (!heard && prev != 0 && dt < 10000)
    return;  // a repeat, or a confirmation report not heard: nothing lost
  if (!heard) {
    // A cycle's first report, missed here. The radiator resets its counters on
    // this answer, so estimate the heating seconds lost: delivered power %
    // (echoed in this answer's e1) x time since the last answer. Over a night
    // this came within 2-6 % of the counters.
    this->missed_++;
    if (this->missed_sensor_ != nullptr)
      this->missed_sensor_->publish_state(this->missed_);
    if (prev != 0 && dt < 600000 && p[41] > 0) {
      // Delivered % for the front panel, asked % for a double panel's back panel.
      float pct = r->front_share_ * p[41] + (1.0f - r->front_share_) * p[42];
      float est = pct / 100.0f * dt / 1000.0f;
      r->add_energy_(est);
      ESP_LOGI(TAG, "%s: E3 answered a report missed here; estimated %.0f s heating", r->label(), est);
    }
    r->pending_s_ = 0;
    return;
  }
  // The E3 has answered: the report's heating seconds count.
  r->add_energy_(r->pending_s_);
  r->pending_s_ = 0;

  // Shadow check: the answer this firmware would send, against the E3's.
  // Setpoint/mode (94), timer (5b) and clock (b2) are the E3's own decisions.
  const auto &df = r->last_df_;
  if (df.size() != 36)
    return;
  std::vector<uint8_t> e = {0x38};
  put_u32(e, get_u32(p, 1));
  e.push_back(0xc6);
  put_u32(e, r->radio_id());
  uint16_t off = (uint16_t) r->offset_f10_;
  // clang-format off
  e.insert(e.end(), {0x45, (uint8_t) (off >> 8), (uint8_t) off,
                     0x4b, df[16], df[17],
                     0x3b, 0x00,
                     0x4c, 0x00, 0x00,
                     0x8e, 0x00, 0x00, 0x84,
                     0xcc, 0x00, 0x00, 0x00, 0x00,
                     0x97, df[11], (uint8_t) (df[12] & 0xfe), (uint8_t) (df[13] | 0x04),
                     0x94, p[35], p[36], p[37],
                     0xe1, df[23], df[24], df[25], df[26],
                     0xda, 0x00, 0x00, 0x00, 0x00,
                     0x5b, p[49], p[50],
                     0xb2, p[52], p[53], p[54]});
  // clang-format on
  uint16_t ec = crc16(e, 1, e.size());
  e.push_back(ec & 0xff);
  e.push_back(ec >> 8);
  if (e == p) {
    this->shadow_ok_++;
  } else {
    this->shadow_bad_++;
    size_t i = 0;
    while (i < e.size() && e[i] == p[i])
      i++;
    ESP_LOGW(TAG, "shadow mismatch %s at byte %u\n  E3   %s\n  mine %s", r->label(), (unsigned) i,
             format_hex_pretty(p).c_str(), format_hex_pretty(e).c_str());
  }
  if (this->shadow_ok_sensor_ != nullptr)
    this->shadow_ok_sensor_->publish_state(this->shadow_ok_);
  if (this->shadow_bad_sensor_ != nullptr)
    this->shadow_bad_sensor_->publish_state(this->shadow_bad_);
}

void ClevertouchHub::note_unknown_(uint32_t from, uint32_t to, const std::vector<uint8_t> &p) {
  uint8_t cmd = p[10];
  auto it = this->unknown_.find(from);
  bool is_new = it == this->unknown_.end();
  if (is_new && this->unknown_.size() >= 32) {
    // Full: forget the one heard longest ago.
    auto oldest = this->unknown_.begin();
    for (auto i = this->unknown_.begin(); i != this->unknown_.end(); ++i)
      if (millis() - i->second.ms > millis() - oldest->second.ms)
        oldest = i;
    this->unknown_.erase(oldest);
  }
  Heard &h = this->unknown_[from];
  if (is_new)
    h = Heard{};
  h.to = to;
  h.len = p[0];
  h.count++;
  h.ms = millis();
  bool seen = false;
  for (uint8_t i = 0; i < h.n_cmds; i++)
    seen |= h.cmds[i] == cmd;
  if (!seen && h.n_cmds < 4)
    h.cmds[h.n_cmds++] = cmd;
  bool radiator_report = cmd == 0xdf && (p.size() == 36 || p.size() == 39);
  if (radiator_report) {
    h.has_df = true;
    h.set = p[19] << 8 | p[20];
    h.byte = p[21];
  }
  if (is_new)
    ESP_LOGI(TAG, "%s %08X (to %08X) is not configured", radiator_report ? "radiator" : "device", (unsigned) from,
             (unsigned) to);
  // For adding support for other devices: the frame in readable form.
  if (!radiator_report) {
    ESP_LOGD(TAG, "unknown device %08X -> %08X, len %02x: %s", (unsigned) from, (unsigned) to, p[0],
             decode_tags(p).c_str());
  }
}

void ClevertouchHub::start_pairing(ClevertouchRadiator *r) {
  const char *refused = nullptr;
  if (this->control_switch_ == nullptr || !this->control_switch_->state)
    refused = "turn \"Control radiators\" on first";
  else if (this->e3_active())
    refused = "another controller (the E3) is active; unplug it and wait 5 min";
  if (refused != nullptr) {
    ESP_LOGW(TAG, "pairing %s: %s", r->label(), refused);
    this->set_notice_(std::string("pairing not started: ") + refused);
    if (r->pair_switch_ != nullptr)
      r->pair_switch_->publish_state(false);
    return;
  }
  if (this->pairing_ != nullptr && this->pairing_ != r)
    this->stop_pairing_((std::string("pairing ") + r->label() + " started").c_str());
  if (this->config_controller_id_ == 0 && this->learned_controller_id_ == 0) {
    this->learned_controller_id_ = this->own_id_();
    this->ctrl_pref_.save(&this->learned_controller_id_);
    this->publish_controller_id_();
  }
  this->pairing_ = r;
  this->pairing_until_ = millis() + PAIRING_MS;
  this->beacon_n_ = 1;
  if (r->pair_switch_ != nullptr)
    r->pair_switch_->publish_state(true);
  ESP_LOGI(TAG, "pairing %s as controller %08X for 2 min: put the radiator in pairing mode now", r->label(),
           (unsigned) this->own_id_());
}

void ClevertouchHub::cancel_pairing(ClevertouchRadiator *r) {
  if (this->pairing_ == r)
    this->stop_pairing_("switched off");
  else if (r->pair_switch_ != nullptr)
    r->pair_switch_->publish_state(false);
}

void ClevertouchHub::stop_pairing_(const char *why) {
  ClevertouchRadiator *r = this->pairing_;
  if (r == nullptr)
    return;
  this->pairing_ = nullptr;
  ESP_LOGW(TAG, "pairing %s stopped: %s", r->label(), why);
  this->set_notice_(std::string("pairing ") + r->label() + " stopped: " + why);
  if (r->pair_switch_ != nullptr)
    r->pair_switch_->publish_state(false);
}

void ClevertouchHub::set_notice_(const std::string &s) {
  this->notice_ = s;
  this->notice_until_ = millis() + NOTICE_MS;
}

void ClevertouchHub::handle_join_(uint32_t radio_id) {
  ClevertouchRadiator *r = this->pairing_;
  ClevertouchRadiator *other = this->find_(radio_id);
  if (other != nullptr && other != r) {
    this->stop_pairing_(str_sprintf("radiator %08X joined, but it is configured as %s; not accepted",
                                    (unsigned) radio_id, other->label())
                            .c_str());
    return;
  }
  if (r->config_radio_id_ != 0 && r->config_radio_id_ != radio_id) {
    this->stop_pairing_(str_sprintf("radiator %08X joined, but %s has radio_id %08X in the YAML; not accepted",
                                    (unsigned) radio_id, r->label(), (unsigned) r->config_radio_id_)
                            .c_str());
    return;
  }
  this->pairing_ = nullptr;
  uint32_t ctrl = this->own_id_();
  std::vector<uint8_t> e;
  put_u32(e, ctrl);
  e.push_back(0xc6);
  put_u32(e, radio_id);
  e.insert(e.end(), {0x01, 0x00});
  append_crc(e);
  this->set_timeout(TIMEOUT_SLOT + ACCEPT_SLOT, ACCEPT_DELAY_MS, [this, e]() { this->send_(e); });
  r->store_.radio_id = radio_id;
  r->ctrl_id_ = ctrl;
  r->new_pairing_ = true;
  r->join_ms_ = std::max<uint32_t>(1, millis());
  r->save_();
  if (r->pair_switch_ != nullptr)
    r->pair_switch_->publish_state(false);
  r->publish_radio_id_();
  this->set_notice_(str_sprintf("paired %s: radio ID %08X", r->label(), (unsigned) radio_id));
  ESP_LOGI(TAG, "paired %08X as %s (add radio_id: %08X to the YAML to keep it on a reflash)", (unsigned) radio_id,
           r->label(), (unsigned) radio_id);
}

void ClevertouchHub::calibrate_(float freq_offset) {
  // From our radiators' packets: after 8 packets the first time, then every 32
  // packets if the average is more than 3 kHz off. offset = signal - tuned.
  if (!this->cal_applied_)
    return;
  this->cal_sum_ += freq_offset;
  this->cal_n_++;
  if (this->cal_n_ < (this->cal_.ready ? 32 : 8))
    return;
  float avg = this->cal_sum_ / this->cal_n_;
  this->cal_sum_ = 0;
  this->cal_n_ = 0;
  if (this->cal_.ready && fabsf(avg) <= 3000)
    return;
  this->cal_.corr = std::max(-200e3f, std::min(200e3f, this->cal_.corr + avg));
  this->cal_.ready = true;
  this->cal_pref_.save(&this->cal_);
  ESP_LOGI(TAG, "frequency correction now %.1f kHz (average offset %.1f kHz)", this->cal_.corr / 1000, avg / 1000);
  this->retune_(NOMINAL_HZ + this->cal_.corr);
}

void ClevertouchHub::recalibrate() {
  this->cal_ = {0, false};
  this->cal_pref_.save(&this->cal_);
  this->scan_base_ = 0;
  this->scan_i_ = 0;
  this->scan_ms_ = millis();
  ESP_LOGI(TAG, "frequency calibration restarted");
  this->retune_(NOMINAL_HZ);
}

void ClevertouchHub::retune_(float hz) {
  // Once calibrated, the E3's 102 kHz filter (residual offsets are under
  // 10 kHz); before that 203 kHz, which tolerates about +-50 kHz.
  this->radio_->set_idle();
  this->radio_->set_filter_bandwidth(this->cal_.ready ? 102e3f : 203e3f);
  this->radio_->set_frequency(hz);
  this->radio_->begin_rx();
  this->cal_sum_ = 0;
  this->cal_n_ = 0;
  if (this->freq_sensor_ != nullptr)
    this->freq_sensor_->publish_state(this->cal_.corr / 1000);
}

}  // namespace esphome::clevertouch_radio
