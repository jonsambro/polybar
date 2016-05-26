#include <thread>

#include "config.hpp"
#include "lemonbuddy.hpp"
#include "modules/battery.hpp"
#include "services/logger.hpp"
#include "utils/config.hpp"
#include "utils/io.hpp"
#include "utils/math.hpp"
#include "utils/string.hpp"

using namespace modules;

const int modules::BatteryModule::STATE_UNKNOWN;
const int modules::BatteryModule::STATE_CHARGING;
const int modules::BatteryModule::STATE_DISCHARGING;
const int modules::BatteryModule::STATE_FULL;

BatteryModule::BatteryModule(const std::string& name_) : InotifyModule(name_)
{
  this->battery = config::get<std::string>(name(), "battery", "BAT0");
  this->adapter = config::get<std::string>(name(), "adapter", "ADP1");
  this->full_at = config::get<int>(name(), "full_at", 100);

  this->state = STATE_UNKNOWN;
  this->percentage = 0;

  this->formatter->add(FORMAT_CHARGING, TAG_LABEL_CHARGING,
    { TAG_BAR_CAPACITY, TAG_RAMP_CAPACITY, TAG_ANIMATION_CHARGING, TAG_LABEL_CHARGING });

  this->formatter->add(FORMAT_DISCHARGING, TAG_LABEL_DISCHARGING,
    { TAG_BAR_CAPACITY, TAG_RAMP_CAPACITY, TAG_LABEL_DISCHARGING });

  this->formatter->add(FORMAT_FULL, TAG_LABEL_FULL,
    { TAG_BAR_CAPACITY, TAG_RAMP_CAPACITY, TAG_LABEL_FULL });

  if (this->formatter->has(TAG_ANIMATION_CHARGING, FORMAT_CHARGING))
    this->animation_charging = drawtypes::get_config_animation(
      name(), get_tag_name(TAG_ANIMATION_CHARGING));
  if (this->formatter->has(TAG_BAR_CAPACITY))
    this->bar_capacity = drawtypes::get_config_bar(
      name(), get_tag_name(TAG_BAR_CAPACITY));
  if (this->formatter->has(TAG_RAMP_CAPACITY))
    this->ramp_capacity = drawtypes::get_config_ramp(
      name(), get_tag_name(TAG_RAMP_CAPACITY));
  if (this->formatter->has(TAG_LABEL_CHARGING, FORMAT_CHARGING))
    this->label_charging = drawtypes::get_optional_config_label(
      name(), get_tag_name(TAG_LABEL_CHARGING), "%percentage%");
  if (this->formatter->has(TAG_LABEL_DISCHARGING, FORMAT_DISCHARGING))
    this->label_discharging = drawtypes::get_optional_config_label(
      name(), get_tag_name(TAG_LABEL_DISCHARGING), "%percentage%");
  if (this->formatter->has(TAG_LABEL_FULL, FORMAT_FULL))
    this->label_full = drawtypes::get_optional_config_label(
      name(), get_tag_name(TAG_LABEL_FULL), "%percentage%");

  this->watch(string::replace(PATH_BATTERY_CAPACITY, "%battery%", this->battery), InotifyEvent::ACCESSED);
  this->watch(string::replace(PATH_ADAPTER_STATUS, "%adapter%", this->adapter), InotifyEvent::ACCESSED);

  if (this->animation_charging)
    this->threads.emplace_back(std::thread(&BatteryModule::animation_thread_runner, this));
}

void BatteryModule::animation_thread_runner()
{
  std::this_thread::yield();

  const auto dur = std::chrono::duration<double>(
      float(this->animation_charging->get_framerate()) / 1000.0f);

  int retries = 5;

  while (retries-- > 0)
  {
    while (this->enabled()) {
      std::unique_lock<concurrency::SpinLock> lck(this->broadcast_lock);

      if (retries > 0)
        retries = 0;

      if (this->state == STATE_CHARGING) {
        lck.unlock();
        this->broadcast();
      } else {
        log_trace("state != charging");
      }

      std::this_thread::sleep_for(dur);
    }

    std::this_thread::sleep_for(500ms);
  }
}

bool BatteryModule::on_event(InotifyEvent *event)
{
  if (event != nullptr)
    log_trace(event->filename);

  int state = STATE_UNKNOWN;

  auto path_capacity  = string::replace(PATH_BATTERY_CAPACITY, "%battery%", this->battery);
  auto path_status = string::replace(PATH_ADAPTER_STATUS, "%adapter%", this->adapter);
  auto status = io::file::get_contents(path_status);

  if (status.empty()) {
    log_error("Failed to read "+ path_status);
    return false;
  }

  auto capacity = io::file::get_contents(path_capacity);

  if (capacity.empty()) {
    log_error("Failed to read "+ path_capacity);
    return false;
  }

  int percentage = (int) math::cap<float>(std::atof(capacity.c_str()), 0, 100) + 0.5;

  switch (status[0]) {
    case '0': state = STATE_DISCHARGING; break;
    case '1': state = STATE_CHARGING; break;
  }

  if ((state == STATE_CHARGING) && percentage >= this->full_at)
    percentage = 100;

  if (percentage == 100)
    state = STATE_FULL;

  if (!this->label_charging_tokenized)
    this->label_charging_tokenized = this->label_charging->clone();
  if (!this->label_discharging_tokenized)
    this->label_discharging_tokenized = this->label_discharging->clone();
  if (!this->label_full_tokenized)
    this->label_full_tokenized = this->label_full->clone();

  this->label_charging_tokenized->text = this->label_charging->text;
  this->label_charging_tokenized->replace_token("%percentage%", std::to_string(percentage) + "%");

  this->label_discharging_tokenized->text = this->label_discharging->text;
  this->label_discharging_tokenized->replace_token("%percentage%", std::to_string(percentage) + "%");

  this->label_full_tokenized->text = this->label_full->text;
  this->label_full_tokenized->replace_token("%percentage%", std::to_string(percentage) + "%");

  this->state = state;
  this->percentage = percentage;

  return true;
}

std::string BatteryModule::get_format()
{
  int state = this->state();

  if (state == STATE_FULL)
    return FORMAT_FULL;
  else if (state == STATE_CHARGING)
    return FORMAT_CHARGING;
  else
    return FORMAT_DISCHARGING;
}

bool BatteryModule::build(Builder *builder, const std::string& tag)
{
  if (tag == TAG_ANIMATION_CHARGING)
    builder->node(this->animation_charging);
  else if (tag == TAG_BAR_CAPACITY) {
    builder->node(this->bar_capacity, this->percentage());
  } else if (tag == TAG_RAMP_CAPACITY)
    builder->node(this->ramp_capacity, this->percentage());
  else if (tag == TAG_LABEL_CHARGING)
    builder->node(this->label_charging_tokenized);
  else if (tag == TAG_LABEL_DISCHARGING)
    builder->node(this->label_discharging_tokenized);
  else if (tag == TAG_LABEL_FULL)
    builder->node(this->label_full_tokenized);
  else
    return false;

  return true;
}