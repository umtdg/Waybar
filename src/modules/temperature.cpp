#include "modules/temperature.hpp"

#include <filesystem>
#include <string>

#if defined(__FreeBSD__)
#include <sys/sysctl.h>
#endif

waybar::modules::Temperature::Temperature(const std::string& id, const Json::Value& config)
    : ALabel(config, "temperature", id, "{temperatureC}°C", 10) {
#if defined(__FreeBSD__)
// FreeBSD uses sysctlbyname instead of read from a file
#else
  if (config_["type"].isString()) {
    if (config_["type"].asString() == "temperature") sensor_type_ = SensorType::TEMPERATURE;
    if (config_["type"].asString() == "fan") sensor_type_ = SensorType::FAN;
    if (config_["type"].asString() == "power") sensor_type_ = SensorType::POWER;
  }

  auto traverseAsArray = [](const Json::Value& value, auto&& check_set_path) {
    if (value.isString())
      check_set_path(value.asString());
    else if (value.isArray())
      for (const auto& item : value)
        if (check_set_path(item.asString())) break;
  };

  // if hwmon_path is an array, loop to find first valid item
  traverseAsArray(config_["hwmon-path"], [this](const std::string& path) {
    if (!std::filesystem::exists(path)) return false;
    file_path_ = path;
    return true;
  });

  if (file_path_.empty() && config_["input-filename"].isString()) {
    // fallback to hwmon_paths-abs
    traverseAsArray(config_["hwmon-path-abs"], [this](const std::string& path) {
      if (!std::filesystem::is_directory(path)) return false;
      return std::ranges::any_of(
          std::filesystem::directory_iterator(path), [this](const auto& hwmon) {
            if (!hwmon.path().filename().string().starts_with("hwmon")) return false;
            file_path_ = hwmon.path().string() + "/" + config_["input-filename"].asString();
            return true;
          });
    });
  }

  if (file_path_.empty() && sensor_type_ == SensorType::TEMPERATURE) {
    auto zone = config_["thermal-zone"].isInt() ? config_["thermal-zone"].asInt() : 0;
    file_path_ = fmt::format("/sys/class/thermal/thermal_zone{}/temp", zone);
  }

  // check if file_path_ can be used to retrieve the temperature
  std::ifstream temp(file_path_);
  if (!temp.is_open()) {
    throw std::runtime_error("Can't open " + file_path_);
  }
  if (!temp.good()) {
    temp.close();
    throw std::runtime_error("Can't read from " + file_path_);
  }
  temp.close();
#endif

  thread_ = [this] {
    dp.emit();
    thread_.sleep_for(interval_);
  };
}

auto waybar::modules::Temperature::update() -> void {
  uint16_t readings = std::round(getReadings());
  auto critical = isCritical(readings);
  auto warning = isWarning(readings);
  auto format = format_;
  if (critical) {
    format = config_["format-critical"].isString() ? config_["format-critical"].asString() : format;
    label_.get_style_context()->add_class("critical");
  } else {
    label_.get_style_context()->remove_class("critical");
    if (warning) {
      format = config_["format-warning"].isString() ? config_["format-warning"].asString() : format;
      label_.get_style_context()->add_class("warning");
    } else {
      label_.get_style_context()->remove_class("warning");
    }
  }

  if (format.empty()) {
    event_box_.hide();
    return;
  }

  event_box_.show();

  auto max_reading =
      config_["critical-threshold"].isInt() ? config_["critical-threshold"].asInt() : 0;

  uint16_t power = 0;
  uint16_t fan_speed = 0;
  uint16_t temperature_c = 0;
  uint16_t temperature_f = 0;
  uint16_t temperature_k = 0;
  switch (sensor_type_) {
    case TEMPERATURE:
      temperature_c = readings;
      temperature_f = std::round(readings * 1.8 + 32);
      temperature_k = std::round(readings + 273.15);
      break;
    case FAN:
      fan_speed = readings;
      break;
    case POWER:
      power = readings;
      break;
  }

  label_.set_markup(fmt::format(fmt::runtime(format), fmt::arg("temperatureC", temperature_c),
                                fmt::arg("temperatureF", temperature_f),
                                fmt::arg("temperatureK", temperature_k),
                                fmt::arg("icon", getIcon(readings, "", max_reading)),
                                fmt::arg("fan", fan_speed), fmt::arg("power", power)));
  if (tooltipEnabled()) {
    std::string tooltip_format = "";
    switch (sensor_type_) {
      case TEMPERATURE:
        tooltip_format = "{temperatureC}°C";
        break;
      case FAN:
        tooltip_format = "{fan} RPM";
        break;
      case POWER:
        tooltip_format = "{power}W";
        break;
    }
    if (config_["tooltip-format"].isString()) {
      tooltip_format = config_["tooltip-format"].asString();
    }
    label_.set_tooltip_text(fmt::format(
        fmt::runtime(tooltip_format), fmt::arg("temperatureC", temperature_c),
        fmt::arg("temperatureF", temperature_f), fmt::arg("temperatureK", temperature_k),
        fmt::arg("power", power), fmt::arg("fan", fan_speed)));
  }
  // Call parent update
  ALabel::update();
}

float waybar::modules::Temperature::getReadings() {
#if defined(__FreeBSD__)
  if (sensor_type_ != SensorType::TEMPERATURE)
    throw std::runtime_error("Only temperature sensor reading is supported in FreeBSD");

  int temp;
  size_t size = sizeof temp;

  auto zone = config_["thermal-zone"].isInt() ? config_["thermal-zone"].asInt() : 0;

  // First, try with dev.cpu
  if ((sysctlbyname(fmt::format("dev.cpu.{}.temperature", zone).c_str(), &temp, &size, NULL, 0) ==
       0) ||
      (sysctlbyname(fmt::format("hw.acpi.thermal.tz{}.temperature", zone).c_str(), &temp, &size,
                    NULL, 0) == 0)) {
    auto temperature_c = ((float)temp - 2732) / 10;
    return temperature_c;
  }

  throw std::runtime_error(fmt::format(
      "sysctl hw.acpi.thermal.tz{}.temperature and dev.cpu.{}.temperature failed", zone, zone));
#else
  std::ifstream file(file_path_);
  if (!file.is_open()) {
    throw std::runtime_error("Can't open " + file_path_);
  }

  std::string line;
  if (file.good()) {
    getline(file, line);
  } else {
    file.close();
    throw std::runtime_error("Can't read from " + file_path_);
  }
  file.close();

  auto reading = std::strtol(line.c_str(), nullptr, 10);
  switch (sensor_type_) {
    case TEMPERATURE:
      return reading / 1000.0;
    case FAN:
      break;
    case POWER:
      return reading / 1000000.0;
  }

  return static_cast<float>(reading);
#endif
}

bool waybar::modules::Temperature::isWarning(uint16_t temperature_c) {
  return config_["warning-threshold"].isInt() &&
         temperature_c >= config_["warning-threshold"].asInt();
}

bool waybar::modules::Temperature::isCritical(uint16_t temperature_c) {
  return config_["critical-threshold"].isInt() &&
         temperature_c >= config_["critical-threshold"].asInt();
}
