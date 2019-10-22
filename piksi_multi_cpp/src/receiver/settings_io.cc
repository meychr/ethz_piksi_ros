#include "piksi_multi_cpp/receiver/settings_io.h"

#include <libsettings/settings_util.h>
#include <ros/assert.h>
#include <fstream>
#include <regex>
#include "piksi_multi_cpp/sbp_callback_handler/sbp_lambda_callback_handler.h"

namespace piksi_multi_cpp {

SettingsIo::SettingsIo(const ros::NodeHandle& nh, const Device::Ptr& device)
    : Receiver(nh, device) {}

bool SettingsIo::readSetting(const std::string& section,
                             const std::string& name, const int timeout_ms) {
  value_.clear();

  // Parse request to format setting\0name\0
  size_t kLen = section.size() + name.size() + 2;
  char read_req[kLen] = {0};
  settings_format(section.c_str(), name.c_str(), nullptr, nullptr, read_req,
                  kLen);

  // Register setting listener.
  SBPLambdaCallbackHandler<msg_settings_read_resp_t> settings_listener(
      std::bind(&SettingsIo::receiveReadResponse, this, std::placeholders::_1,
                std::placeholders::_2),
      SBP_MSG_SETTINGS_READ_RESP, state_);

  // Send message.
  int req_success = sbp_send_message(
      state_.get(), SBP_MSG_SETTINGS_READ_REQ, SBP_SENDER_ID, kLen,
      (unsigned char*)(&read_req), &Device::write_redirect);
  if (req_success != SBP_OK) {
    ROS_ERROR("Cannot request setting %s.%s, %d", section.c_str(), name.c_str(),
              req_success);
    return false;
  }

  // Wait to receive setting.
  if (!settings_listener.waitForCallback(timeout_ms)) {
    ROS_ERROR("Did not receive setting %s.%s", section.c_str(), name.c_str());
    return false;
  }

  return true;
}

bool SettingsIo::writeSetting(const std::string& section,
                              const std::string& name, const std::string& value,
                              const int timeout_ms) {
  // Parse request to format setting\0name\0value\0
  size_t kLen = section.size() + name.size() + value.size() + 2;
  char write_req[kLen] = {0};
  settings_format(section.c_str(), name.c_str(), value.c_str(), nullptr,
                  write_req, kLen);

  // Register setting listener.
  SBPLambdaCallbackHandler<msg_settings_write_resp_t> write_resp_listener(
      std::bind(&SettingsIo::receiveWriteResponse, this, std::placeholders::_1,
                std::placeholders::_2),
      SBP_MSG_SETTINGS_WRITE_RESP, state_);

  // Send message.
  int req_success = sbp_send_message(
      state_.get(), SBP_MSG_SETTINGS_WRITE, SBP_SENDER_ID, kLen,
      (unsigned char*)(&write_req), &Device::write_redirect);
  if (req_success != SBP_OK) {
    ROS_ERROR("Cannot write setting %s.%s.%s, %d", section.c_str(),
              name.c_str(), value.c_str(), req_success);
    return false;
  }

  // Wait to receive setting.
  if (!write_resp_listener.waitForCallback(timeout_ms)) {
    ROS_ERROR("Did not receive write response %s.%s.%s", section.c_str(),
              name.c_str(), value.c_str());
    return false;
  }

  return true;
}

void SettingsIo::receiveReadResponse(const msg_settings_read_resp_t& msg,
                                     const uint8_t len) {
  // Parse settings.
  const char *section = nullptr, *name = nullptr, *value = nullptr,
             *type = nullptr;
  int num_tokens =
      settings_parse(&msg.setting[0], len, &section, &name, &value, &type);
  if (num_tokens < 0) {
    ROS_ERROR("Failed to parse settings %d", num_tokens);
    return;
  }
  // Store value.
  value_ = std::string(value);

  ROS_DEBUG("Read setting section: %s", section);
  ROS_DEBUG("Read setting name: %s", name);
  ROS_DEBUG("Read setting value: %s", value);
  ROS_DEBUG("Read setting type: %s", type);
}

void SettingsIo::receiveWriteResponse(const msg_settings_write_resp_t& msg,
                                      const uint8_t len) {
  write_success_ = msg.status == 0;

  // Parse settings.
  const char *section = nullptr, *name = nullptr, *value = nullptr,
             *type = nullptr;
  int num_tokens =
      settings_parse(&msg.setting[0], len - 1, &section, &name, &value, &type);
  if (num_tokens < 0) {
    ROS_ERROR("Failed to parse settings %d", num_tokens);
    return;
  }

  std::string setting;
  if (section) setting += std::string(section);
  if (name) setting += "." + std::string(name);
  if (value) setting += "." + std::string(value);
  if (type) setting += "." + std::string(type);

  switch (msg.status) {
    case 0:
      ROS_DEBUG("Accepted; value updated %s", setting.c_str());
      break;
    case 1:
      ROS_ERROR("Rejected; value unparsable or out-of-range %s",
                setting.c_str());
      break;
    case 2:
      ROS_ERROR("Rejected; requested setting does not exist %s",
                setting.c_str());
      break;
    case 3:
      ROS_ERROR("Rejected; setting name could not be parsed %s",
                setting.c_str());
      break;
    case 4:
      ROS_WARN("Rejected; setting is read only %s", setting.c_str());
      break;
    case 5:
      ROS_WARN("Rejected; modification is temporarily disabled %s",
               setting.c_str());
      break;
    case 6:
      ROS_ERROR("Rejected; unspecified error %s", setting.c_str());
      break;
    default:
      ROS_ERROR("Rejected; unknown error %d, %s", msg.status, setting.c_str());
  }
}

bool SettingsIo::compareValue(const std::string& value) const {
  return value_ == value;
}
bool SettingsIo::checkBoolTrue() const { return compareValue("True"); }
std::string SettingsIo::getValue() const { return value_; }

bool SettingsIo::openConfig(const std::string& file) {
  ROS_INFO("Opening config: %s", file.c_str());

  // Open.
  std::ifstream in_file;
  in_file.open(file);
  if (!in_file) {
    ROS_ERROR("Cannot open file: %s", file.c_str());
    return false;
  }

  // Compare firmware version.
  std::regex rgx_firmware("firmware_version\\ \\=\\ (.+)");
  std::smatch firmware_matches;
  std::string firmware_config;
  std::string line;
  while (std::getline(in_file, line)) {
    if (std::regex_search(line, firmware_matches, rgx_firmware)) {
      if (firmware_matches.size() > 1) firmware_config = firmware_matches[1];
      ROS_DEBUG("%s", firmware_config.c_str());
      break;
    }
  }
  in_file.seekg(0);  // Reset file pointer.
  if (!readSetting("system_info", "firmware_version")) {
    ROS_ERROR("Cannot read firmware version setting.");
    return false;
  }

  if (!compareValue(firmware_config)) {
    ROS_ERROR(
        "Firmware version in config file %s does not match firmware version on "
        "device %s. Please update this repo and use Swift console to update "
        "device firmware.",
        firmware_config.c_str(), value_.c_str());
    return false;
  }

  // Parse and write setting.
  std::string section;
  while (std::getline(in_file, line)) {
    // Find section.
    std::regex rgx_section("\\[(.*)\\]");  // Group between square brackets.
    std::smatch section_matches;
    if (std::regex_search(line, section_matches, rgx_section)) {
      if (section_matches.size() > 1) section = section_matches[1];
      ROS_INFO("%s", section.c_str());
      continue;  // New section.
    }
    if (section.empty()) continue;

    // Find name.
    std::regex rgx_name(".+?(?=\\ \\=)");  // Up to whitespace equal.
    std::smatch name_matches;
    std::string name;
    if (std::regex_search(line, name_matches, rgx_name)) {
      name = name_matches[0];
      ROS_INFO("%s", name.c_str());
    } else {
      continue;  // No name found.
    }

    // Find value.
    std::regex rgx_value(
        "\\ \\=\\ (.+)");  // After whitespace equal whitespace combination.
    std::smatch value_matches;
    std::string value;
    if (std::regex_search(line, value_matches, rgx_value)) {
      if (value_matches.size() > 1) value = value_matches[1];
      ROS_INFO("%s", value.c_str());
    }

    // Write setting.
    ROS_INFO("Writing setting %s.%s.%s", section.c_str(), name.c_str(),
             value.c_str());
    writeSetting(section, name, value);
  }
  return true;
}

}  // namespace piksi_multi_cpp
