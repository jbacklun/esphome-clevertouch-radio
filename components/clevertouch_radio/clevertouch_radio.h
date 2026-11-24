#pragma once

#include <map>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/cc1101/cc1101.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text/text.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/time/real_time_clock.h"

#include "clevertouch_logic.h"

namespace esphome::clevertouch_radio {

class ClevertouchHub;
class ClevertouchRadiator;

// Heat with preset comfort / eco (reduced) / away (frost protection), off, auto (schedule).
enum RadMode : uint8_t { RAD_OFF = 0, RAD_COMFORT = 1, RAD_AUTO = 2, RAD_FROST = 3, RAD_REDUCED = 4 };

// Kept in flash per radiator. Temperatures in 0.1 F as on the radio.
struct RadiatorStore {
  uint32_t radio_id;  // paired from this ESP (only for radiators without radio_id in YAML)
  uint16_t comfort_set;
  uint16_t reduced_set;
  uint16_t frost_set;
  uint8_t mode;   // RadMode
  uint8_t limit;  // front panel limit byte 64/46/37, 0 = leave as the radiator has it
  uint8_t valid;
} __attribute__((packed));

// The week schedule in flash, as switch points (the day texts are built from them).
struct PointsStore {
  uint8_t n;
  SchedulePoint pts[MAX_POINTS];
};

// A temporary change in Auto, kept over a reboot; ends at the next switch point.
struct OverrideStore {
  uint8_t valid;
  uint8_t level;        // as SchedulePoint::level
  uint8_t byte;         // mode byte sent with it (auto comfort / auto reduced symbol)
  int16_t start;        // minute of the week it was made (-1: not known yet, no clock)
  int16_t until;        // minute of the week of the next switch point then
  uint32_t sched_hash;  // of the schedule it belongs to
} __attribute__((packed));

// A timer set on the radiator, kept over a reboot.
struct TimerStore {
  uint32_t end;   // UTC, s; 0 = none
  uint8_t level;  // as SchedulePoint::level
} __attribute__((packed));

// The schedule of one weekday (0 = Monday).
class ClevertouchScheduleText : public text::Text {
 public:
  void set_radiator(ClevertouchRadiator *r, int day) {
    this->radiator_ = r;
    this->day_ = day;
  }

 protected:
  void control(const std::string &value) override;
  ClevertouchRadiator *radiator_{nullptr};
  int day_{0};
};

class ClevertouchLimitSelect : public select::Select {
 public:
  void set_radiator(ClevertouchRadiator *r) { this->radiator_ = r; }

 protected:
  void control(size_t index) override;
  ClevertouchRadiator *radiator_{nullptr};
};

// Comfort / eco (reduced) / away (frost protection) temperature, settable without
// switching the radiator to that preset; and the room temperature offset.
enum TempKind : uint8_t { TEMP_COMFORT = 0, TEMP_REDUCED = 1, TEMP_FROST = 2, TEMP_OFFSET = 3 };

class ClevertouchTempNumber : public number::Number {
 public:
  void set_radiator(ClevertouchRadiator *r, TempKind kind) {
    this->radiator_ = r;
    this->kind_ = kind;
  }

 protected:
  void control(float value) override;
  ClevertouchRadiator *radiator_{nullptr};
  TempKind kind_{TEMP_COMFORT};
};

// On while the radiator's pairing window is open; turns itself off when the
// radiator joins, after 2 min, or when pairing can't start.
class ClevertouchPairSwitch : public switch_::Switch {
 public:
  void set_radiator(ClevertouchRadiator *r) { this->radiator_ = r; }

 protected:
  void write_state(bool state) override;
  ClevertouchRadiator *radiator_{nullptr};
};

// Double panel: front panel first, back panel only as a boost 1 C below the
// setpoint (the radiator's parameter "SrC"). Shows what the radiator reports.
class ClevertouchSeqSwitch : public switch_::Switch {
 public:
  void set_radiator(ClevertouchRadiator *r) { this->radiator_ = r; }

 protected:
  void write_state(bool state) override;
  ClevertouchRadiator *radiator_{nullptr};
};

class ClevertouchRecalButton : public button::Button {
 public:
  void set_hub(ClevertouchHub *h) { this->hub_ = h; }

 protected:
  void press_action() override;
  ClevertouchHub *hub_{nullptr};
};

class ClevertouchControlSwitch : public switch_::Switch {
 protected:
  void write_state(bool state) override { this->publish_state(state); }
};

class ClevertouchRadiator : public climate::Climate {
 public:
  void set_hub(ClevertouchHub *hub) { this->hub_ = hub; }
  void set_label(const char *label) { this->label_ = label; }
  void set_config_radio_id(uint32_t id) { this->config_radio_id_ = id; }
  void set_watts(float watts) { this->watts_ = watts; }
  // Double panel: the front panel's share of the power (the front panel limit
  // throttles only the front panel). 1 for a single panel.
  void set_front_share(float f) { this->front_share_ = f; }
  void set_default_schedule(const char *s) { this->default_schedule_ = s; }
  void set_default_temperatures(float comfort, float reduced, float frost) {
    this->default_comfort_ = comfort;
    this->default_reduced_ = reduced;
    this->default_frost_ = frost;
  }
  void set_room_sensor(sensor::Sensor *s) { this->room_sensor_ = s; }
  void set_power_sensor(sensor::Sensor *s) { this->power_sensor_ = s; }
  void set_energy_sensor(sensor::Sensor *s) { this->energy_sensor_ = s; }
  void set_signal_sensor(sensor::Sensor *s) { this->signal_sensor_ = s; }
  void set_mode_sensor(text_sensor::TextSensor *s) { this->mode_sensor_ = s; }
  void set_day_text(int day, ClevertouchScheduleText *t) { this->day_texts_[day] = t; }
  void set_limit_select(ClevertouchLimitSelect *s) { this->limit_select_ = s; }
  void set_temp_number(TempKind kind, ClevertouchTempNumber *n) { this->temp_numbers_[kind] = n; }
  void set_schedule_status_sensor(text_sensor::TextSensor *s) { this->schedule_status_ = s; }
  void set_radio_id_sensor(text_sensor::TextSensor *s) { this->radio_id_sensor_ = s; }
  void set_pair_switch(ClevertouchPairSwitch *s) { this->pair_switch_ = s; }
  void set_offset_number(ClevertouchTempNumber *n) { this->offset_number_ = n; }
  void set_seq_switch(ClevertouchSeqSwitch *s) { this->seq_switch_ = s; }
  void set_reporting_sensor(binary_sensor::BinarySensor *s) { this->reporting_sensor_ = s; }
  // Lowest / highest setpoint the radiator is ever sent (off excepted), in C.
  void set_temperature_limits(float lo, float hi) {
    this->min_c_ = lo;
    this->max_c_ = hi;
  }

  uint32_t radio_id() const { return this->config_radio_id_ != 0 ? this->config_radio_id_ : this->store_.radio_id; }
  const char *label() const { return this->label_; }

  void setup_radiator(uint32_t hash);
  void on_day_text(int day, const std::string &value);
  void on_limit_index(size_t index);
  void on_temp_number(TempKind kind, float value);
  void on_pair_switch(bool on);
  void on_seq_switch(bool on);

  climate::ClimateTraits traits() override;

 protected:
  friend class ClevertouchHub;
  struct Desired {
    int set;
    uint8_t byte;
  };

  void control(const climate::ClimateCall &call) override;
  void save_();  // also publishes the three temperature numbers
  void save_schedule_();
  void publish_day_texts_();
  void publish_climate_();
  // "F1234567", "F1234567 (paired here, not in the YAML)" or "not paired".
  void publish_radio_id_();
  // Desired state (setpoint 0.1 F + mode byte) from mode, schedule and override.
  Desired compute_desired_(bool time_ok, uint16_t now_minute);
  // Index of the schedule point in force (the last one at or before now, wrapping round the week).
  int active_point_(uint16_t now_minute) const;
  uint8_t auto_byte_(int set) const;
  // Setpoint of a schedule point (PT_OFF -> 0 C).
  int point_set_(uint8_t level) const;
  // Temporary change in Auto, until the schedule's next switch point.
  void set_override_(uint8_t level, uint8_t byte, bool time_ok, uint16_t now_minute);
  // Ties an override to "now" and the next switch point after it.
  void anchor_override_(uint16_t now_minute);
  // True once the override's switch point has been reached.
  bool override_expired_(uint16_t now_minute) const;
  void clear_override_();
  void save_override_();
  // A timer set on the radiator: a temperature for some time, in any mode.
  void set_timer_(uint8_t level, int minutes);
  void clear_timer_();
  void save_timer_();
  std::string timer_text_() const;
  // "eco 16.0 C until 18:15", "override 16.5 C until 18:15", "comfort (manual)", ...
  void update_schedule_status_(bool time_ok, uint16_t now_minute);
  std::string level_text_(uint8_t level) const;
  // Take over a state seen on the radio (the radiator's own buttons, its first report, or the E3).
  // keep_presets: its mode, but not its temperature as the preset's.
  // timer_min: with byte 04, the time chosen.
  void adopt_(int set, uint8_t byte, bool local, bool time_ok, uint16_t now_minute, bool keep_presets = false,
              int timer_min = 0);
  // A setpoint (0.1 F) limited to min/max temperature.
  int clamp_set_(int set) const;
  // Room temperature offset: kept in 0.1 F as on the radio (+0.5 C = 9).
  void set_offset_f10_(int16_t f10);
  // Heating seconds at full rated power.
  void add_energy_(float seconds);
  void save_energy_(bool force);

  ClevertouchHub *hub_{nullptr};
  const char *label_{""};
  uint32_t config_radio_id_{0};
  float watts_{0};
  float front_share_{1.0f};
  const char *default_schedule_{""};
  float default_comfort_{21.0f};
  float default_reduced_{17.0f};
  float default_frost_{7.0f};
  float min_c_{5.0f};
  float max_c_{30.0f};

  sensor::Sensor *room_sensor_{nullptr};
  sensor::Sensor *power_sensor_{nullptr};
  sensor::Sensor *energy_sensor_{nullptr};
  sensor::Sensor *signal_sensor_{nullptr};
  text_sensor::TextSensor *mode_sensor_{nullptr};
  ClevertouchScheduleText *day_texts_[7]{};
  ClevertouchLimitSelect *limit_select_{nullptr};
  ClevertouchTempNumber *temp_numbers_[3]{};
  text_sensor::TextSensor *schedule_status_{nullptr};
  text_sensor::TextSensor *radio_id_sensor_{nullptr};
  ClevertouchPairSwitch *pair_switch_{nullptr};
  ClevertouchTempNumber *offset_number_{nullptr};
  ClevertouchSeqSwitch *seq_switch_{nullptr};
  // Sequential control to push (bit 0 of e1's first byte): -1 = none, just
  // echo what the radiator reports (also a setting made on the radiator).
  int8_t seq_want_{-1};
  binary_sensor::BinarySensor *reporting_sensor_{nullptr};

  RadiatorStore store_{};
  ESPPreferenceObject store_pref_;
  ESPPreferenceObject points_pref_;
  ESPPreferenceObject energy_pref_;
  ESPPreferenceObject override_pref_;
  ESPPreferenceObject timer_pref_;
  ESPPreferenceObject offset_pref_;
  int16_t offset_f10_{0};
  uint32_t sched_hash_{0};
  double kwh_{0};
  double kwh_saved_{0};  // last value written to flash
  uint32_t kwh_save_ms_{0};
  std::vector<SchedulePoint> schedule_;

  uint32_t timer_end_{0};  // UTC, s; 0 = no timer
  uint8_t timer_level_{0};

  // AUTO: a temperature set from HA holds until the next switch point.
  bool override_{false};
  uint8_t override_level_{PT_COMFORT};
  uint8_t override_byte_{0x08};
  int16_t override_start_{-1};  // minute of the week it was made; -1 = no clock yet
  int16_t override_until_{-1};  // minute of the week of the next switch point then

  // The state this firmware is enforcing (sent in every answer).
  bool cur_valid_{false};
  int cur_set_{0};
  uint8_t cur_byte_{0};
  uint32_t pend_ms_{0};  // a change is being pushed (0 = none)

  // Last seen on the radio.
  uint32_t ctrl_id_{0};      // the controller this radiator reports to
  uint32_t join_ms_{0};      // just paired, no report yet (polled until it reports)
  bool new_pairing_{false};  // paired since boot, no report since
  bool local_seen_{false};   // a change made at its buttons, reported but not yet acknowledged
  int local_set_{0};
  uint8_t local_byte_{0};
  int taken_set_[5]{};  // per RadMode: the setpoint it last took for it (0 = not seen since boot)
  std::vector<uint8_t> last_df_;
  uint32_t last_df_ms_{0};
  float pending_s_{0};   // full-power heating seconds in the last report
  float answered_s_{0};  // ... of the report last answered: counted once the radiator confirms
  bool acked_{true};     // the last answer was acknowledged (01, or a report with reset counters)
  bool bad_layout_logged_{false};
  float room_c_{NAN};
  int power_{-1};
  uint8_t reported_limit_{0};
  // The E3's last answer (while it is in charge the climate entity mirrors it).
  bool e3_valid_{false};
  int e3_set_{0};
  uint8_t e3_byte_{0};
  uint32_t last_ans_ms_{0};  // last E3 answer
  // Own answers.
  uint32_t last_tx_ms_{0};
  bool answer_queued_{false};  // waiting for its reply delay
  std::vector<uint8_t> last_45_;
};

class ClevertouchHub : public Component, public cc1101::CC1101Listener {
 public:
  void set_radio(cc1101::CC1101Component *radio) { this->radio_ = radio; }
  void set_time(time::RealTimeClock *t) { this->time_ = t; }
  void set_controller_id(uint32_t id) { this->config_controller_id_ = id; }
  void set_reply_delay(uint32_t ms) { this->reply_delay_ms_ = ms; }
  // How long to only listen after boot (for an E3 to show itself). 0 without an E3.
  void set_listen_after_boot(uint32_t ms) { this->listen_after_boot_ms_ = ms; }
  void set_fahrenheit(bool f) { set_unit_fahrenheit(f); }
  void set_12h(bool on) { set_time_12h(on); }
  // Devices not to list or log as unconfigured (e.g. another ESP's radiators).
  void add_ignored(uint32_t id) { this->ignored_.push_back(id); }
  void add_radiator(ClevertouchRadiator *r) { this->radiators_.push_back(r); }
  void set_control_switch(ClevertouchControlSwitch *s) { this->control_switch_ = s; }
  void set_status_sensor(text_sensor::TextSensor *s) { this->status_sensor_ = s; }
  void set_unassigned_sensor(text_sensor::TextSensor *s) { this->unassigned_sensor_ = s; }
  void set_controller_id_sensor(text_sensor::TextSensor *s) { this->controller_id_sensor_ = s; }
  void set_freq_sensor(sensor::Sensor *s) { this->freq_sensor_ = s; }
  void set_shadow_ok_sensor(sensor::Sensor *s) { this->shadow_ok_sensor_ = s; }
  void set_shadow_bad_sensor(sensor::Sensor *s) { this->shadow_bad_sensor_ = s; }
  void set_missed_sensor(sensor::Sensor *s) { this->missed_sensor_ = s; }

  void setup() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void on_packet(const std::vector<uint8_t> &packet, float freq_offset, float rssi, uint8_t lqi) override;

  void recalibrate();
  void start_pairing(ClevertouchRadiator *r);
  void cancel_pairing(ClevertouchRadiator *r);
  // Another controller (the E3) has been heard in the last 5 min.
  bool e3_active() const;
  // True when this firmware may transmit: control on, no other controller for 5 min, clock valid.
  bool may_transmit(bool log_reason = false);

 protected:
  friend class ClevertouchRadiator;

  void tick_();
  bool now_(uint16_t &minute_of_week, ESPTime &t);
  uint32_t utc_() const;  // s; 0 without a clock
  void refresh_(ClevertouchRadiator *r, bool time_ok, uint16_t minute);
  void handle_radiator_(ClevertouchRadiator *r, uint32_t to, const std::vector<uint8_t> &frame, float rssi);
  void handle_e3_answer_(ClevertouchRadiator *r, const std::vector<uint8_t> &p);
  void handle_join_(uint32_t radio_id);
  void stop_pairing_(const char *why);
  // A message shown in the controller status for a minute (pairing results).
  void set_notice_(const std::string &s);
  // Any device heard that isn't configured (radiators to find their IDs, and
  // other device types for adding support).
  void note_unknown_(uint32_t from, uint32_t to, const std::vector<uint8_t> &p);
  // Another controller (an E3, or another ESP with the same radiator) is active.
  void controller_heard_(uint32_t id, ClevertouchRadiator *r, const char *how);
  void answer_(ClevertouchRadiator *r, const std::vector<uint8_t> &df, const ESPTime &t);
  void send_poll_(ClevertouchRadiator *r, uint32_t delay_ms);
  void send_after_(ClevertouchRadiator *r, const std::vector<uint8_t> &frame, uint32_t delay_ms);
  void send_(const std::vector<uint8_t> &frame);
  uint32_t index_(ClevertouchRadiator *r) const;
  void retune_(float hz);
  void calibrate_(float freq_offset);
  uint32_t own_id_();
  // "F1ABCDEF (learned)", "... (from the YAML)", "... (from the MAC, not used yet)".
  void publish_controller_id_();
  ClevertouchRadiator *find_(uint32_t radio_id);

  cc1101::CC1101Component *radio_{nullptr};
  time::RealTimeClock *time_{nullptr};
  std::vector<ClevertouchRadiator *> radiators_;
  ClevertouchControlSwitch *control_switch_{nullptr};
  text_sensor::TextSensor *status_sensor_{nullptr};
  text_sensor::TextSensor *unassigned_sensor_{nullptr};
  text_sensor::TextSensor *controller_id_sensor_{nullptr};
  sensor::Sensor *freq_sensor_{nullptr};
  sensor::Sensor *shadow_ok_sensor_{nullptr};
  sensor::Sensor *shadow_bad_sensor_{nullptr};
  sensor::Sensor *missed_sensor_{nullptr};

  uint32_t config_controller_id_{0};
  uint32_t learned_controller_id_{0};
  ESPPreferenceObject ctrl_pref_;
  uint32_t reply_delay_ms_{200};
  uint32_t listen_after_boot_ms_{300000};
  std::vector<uint32_t> ignored_;
  uint32_t e3_last_ms_{0};
  bool e3_ever_{false};
  int shadow_ok_{0};
  int shadow_bad_{0};
  int missed_{0};

  // Frequency calibration (the CC1101 modules' crystals differ by tens of kHz).
  struct CalStore {
    float corr;
    bool ready;
  };
  ESPPreferenceObject cal_pref_;
  CalStore cal_{0, false};
  bool cal_applied_{false};
  float cal_sum_{0};
  int cal_n_{0};
  uint32_t last_rx_ms_{0};
  uint32_t scan_ms_{0};
  int scan_i_{0};
  float scan_base_{0};  // the last good correction; the scan steps around it
  uint32_t boot_ms_{0};
  bool listening_{true};         // in the listen-only time after boot (latched: millis() wraps)
  bool reporting_known_{false};  // 20 min since boot have passed (latched)
  uint32_t tick_n_{0};
  uint32_t last_list_ms_{0};
  // Polling the radiators once after boot.
  size_t boot_poll_n_{0};
  uint32_t boot_poll_ms_{0};

  // Pairing.
  ClevertouchRadiator *pairing_{nullptr};
  uint32_t pairing_until_{0};
  uint8_t beacon_n_{1};

  struct Heard {
    uint32_t to;
    uint8_t cmds[4];  // the first four different commands heard from it
    uint8_t n_cmds;
    uint8_t len;  // length byte of the last frame
    int set;      // last radiator report (df) only: setpoint and mode
    uint8_t byte;
    bool has_df;
    uint32_t count;
    uint32_t ms;
  };
  std::map<uint32_t, Heard> unknown_;
  std::string last_status_;
  std::string notice_;
  uint32_t notice_until_{0};
};

}  // namespace esphome::clevertouch_radio
