//
//  fake_driver_manager.cpp
//
//  Copyright (c) 2019 2022 Andrea Bondavalli. All rights reserved.
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "log.hpp"
#include "fake_driver_manager.hpp"

static const std::vector<std::string> ptp_status_str = {"unlocked", "locking",
                                                        "locked"};

std::shared_ptr<DriverManager> DriverManager::create() {
  // no need to be thread-safe here
  static std::weak_ptr<DriverManager> instance;
  if (auto ptr = instance.lock()) {
    return ptr;
  }
  auto ptr = std::shared_ptr<DriverManager>(new DriverManager());
  instance = ptr;
  return ptr;
}

bool DriverManager::init(const Config& config) {
  /* W7: group 0's rate is the manager-wide rate (Decision 10) — mirror the
   * real DriverManager, which uses rate_for_group(0), not the top-level
   * default (they differ only when group 0 sets its own sample_rate). */
  sample_rate_ = config.rate_for_group(0);

  TPTPConfig ptp_config;
  ptp_config.ui8Domain = config.get_ptp_domain();
  ptp_config.ui8DSCP = config.get_ptp_dscp();

  if (hello())
    return false;

  bool res(false);
  if (config.get_driver_restart()) {
    /* W10 multi-card: mirror the real DriverManager — drop the group-0
     * SetSampleRate and bring each device_group up as its own card. */
    res = start() || reset(-1 /* all PCMs: clean slate */) ||
          set_interface_name(config.get_interface_name()) ||
          set_ptp_config(ptp_config) ||
          set_tic_frame_size_at_1fs(config.get_tic_frame_size_at_1fs()) ||
          set_max_tic_frame_size(config.get_max_tic_frame_size());
    if (!res) {
      uint8_t card_handle = 0;
      int32_t shared_playout_delay = 0;
      bool have_shared_delay = false;
      for (const auto& g : config.get_device_groups()) {
        (void)add_card(card_handle, g.name, g.domain);
        (void)add_pcm_to_card(card_handle, g.id, config.rate_for_group(g.id),
                              g.num_inputs, g.num_outputs, g.name);
        (void)register_card(card_handle);
        ++card_handle;
        if (g.id == 0) {
          shared_playout_delay = g.playout_delay;
          have_shared_delay = true;
        }
      }
      if (have_shared_delay)
        (void)set_playout_delay(0, shared_playout_delay);
    }
  }

  return !res;
}

bool DriverManager::terminate(const Config& config) {
  if (config.get_driver_restart()) {
    stop();
    /* W10: mirror the real DriverManager -- remove the cards we created on
     * clean shutdown (reset(-1) is the backstop for unclean exits). */
    for (size_t i = 0; i < config.get_device_groups().size(); ++i)
      (void)remove_card(static_cast<uint8_t>(i));
  }
  bye();
  return true;
}

std::error_code DriverManager::hello() {
  return std::error_code{};
}

std::error_code DriverManager::bye() {
  return std::error_code{};
}

std::error_code DriverManager::start() {
  return std::error_code{};
}

std::error_code DriverManager::stop() {
  return std::error_code{};
}

std::error_code DriverManager::reset(int32_t /*pcm_id*/) {
  return std::error_code{};
}

std::error_code DriverManager::set_ptp_config(const TPTPConfig& config) {
  BOOST_LOG_TRIVIAL(info) << "fake_driver_manager:: setting PTP Domain "
                          << (int)config.ui8Domain << " DSCP "
                          << (int)config.ui8DSCP;
  ptp_config_ = config;
  return std::error_code{};
}

std::error_code DriverManager::get_ptp_config(TPTPConfig& config) {
  config = ptp_config_;
  BOOST_LOG_TRIVIAL(debug) << "fake_driver_manager:: PTP Domain "
                           << (int)config.ui8Domain << " DSCP "
                           << (int)config.ui8DSCP;
  return std::error_code{};
}

std::error_code DriverManager::get_ptp_status(TPTPStatus& status) {
  status.nPTPLockStatus = PTPLS_UNLOCKED;
  status.ui64GMID[0] = 0xABABABABABABABAB;
  status.ui64GMID[1] = 0x0;
  status.i32ClockJitter = 0;
  BOOST_LOG_TRIVIAL(debug) << "fake_driver_manager:: PTP Status "
                           << ptp_status_str[status.nPTPLockStatus] << " GMID "
                           << status.ui64GMID[0] << " Jitter "
                           << status.i32ClockJitter;
  return std::error_code{};
}

std::error_code DriverManager::set_interface_name(const std::string& ifname) {
  return std::error_code{};
}

std::error_code DriverManager::add_card(uint8_t card_handle,
                                        const std::string& id,
                                        uint8_t domain) {
  BOOST_LOG_TRIVIAL(info) << "fake_driver_manager:: add card handle="
                          << (int)card_handle << " id=\"" << id
                          << "\" domain=" << (int)domain;
  return std::error_code{};
}

std::error_code DriverManager::add_pcm_to_card(uint8_t card_handle,
                                               uint8_t global_pcm_id,
                                               uint32_t sample_rate,
                                               uint32_t /*num_inputs*/,
                                               uint32_t /*num_outputs*/,
                                               const std::string& name) {
  BOOST_LOG_TRIVIAL(info) << "fake_driver_manager:: add PCM card="
                          << (int)card_handle << " pcm_id="
                          << (int)global_pcm_id << " rate=" << sample_rate
                          << " name=\"" << name << "\"";
  return std::error_code{};
}

std::error_code DriverManager::register_card(uint8_t card_handle) {
  BOOST_LOG_TRIVIAL(info) << "fake_driver_manager:: register card handle="
                          << (int)card_handle;
  return std::error_code{};
}

std::error_code DriverManager::remove_card(uint8_t card_handle) {
  BOOST_LOG_TRIVIAL(info) << "fake_driver_manager:: remove card handle="
                          << (int)card_handle;
  return std::error_code{};
}

std::error_code DriverManager::add_rtp_stream(
    uint8_t pcm_id,
    const TRTP_stream_info& stream_info,
    uint64_t& stream_handle) {
  stream_handle = ++g_handle;
  handles_.insert(stream_handle);
  BOOST_LOG_TRIVIAL(info)
      << "fake_driver_manager:: add RTP stream pcm_id=" << (int)pcm_id
      << " success handle " << stream_handle;
  return std::error_code{};
}

std::error_code DriverManager::get_rtp_stream_status(
    uint64_t stream_handle,
    TRTP_stream_status& stream_status) {
  stream_status.u.flags = 0x0;
  stream_status.sink_min_time = 0;
  if (handles_.find(stream_handle) == handles_.end()) {
    return DriverErrc::invalid_value;
  }
  return std::error_code{};
}

std::error_code DriverManager::remove_rtp_stream(uint64_t stream_handle) {
  if (handles_.find(stream_handle) == handles_.end()) {
    return DriverErrc::invalid_value;
  }
  handles_.erase(stream_handle);
  return std::error_code{};
}

std::error_code DriverManager::ping() {
  return std::error_code{};
}

std::error_code DriverManager::set_sample_rate(uint32_t sample_rate) {
  sample_rate_ = sample_rate;
  /* W7: log it so group 0's rate is visible in FAKE_DRIVER test output
   * (the real driver's SetSampleRate is visible kernel-side). */
  BOOST_LOG_TRIVIAL(info) << "fake_driver_manager:: set sample rate "
                          << sample_rate;
  return std::error_code{};
}

std::error_code DriverManager::set_tic_frame_size_at_1fs(uint64_t frame_size) {
  frame_size_ = frame_size;
  return std::error_code{};
}

std::error_code DriverManager::set_max_tic_frame_size(uint64_t frame_size) {
  max_frame_size_ = frame_size;
  return std::error_code{};
}

std::error_code DriverManager::set_playout_delay(uint8_t /*pcm_id*/,
                                                 int32_t delay) {
  delay_ = delay;
  return std::error_code{};
}

std::error_code DriverManager::get_sample_rate(uint32_t& sample_rate) {
  sample_rate = sample_rate_;
  BOOST_LOG_TRIVIAL(info) << "fake_driver_manager:: sample rate "
                          << sample_rate;
  return std::error_code{};
}

std::error_code DriverManager::get_number_of_inputs(uint8_t /*pcm_id*/,
                                                    int32_t& inputs) {
  inputs = 0;
  return std::error_code{};
}

std::error_code DriverManager::get_number_of_outputs(uint8_t /*pcm_id*/,
                                                     int32_t& outputs) {
  outputs = 0;
  return std::error_code{};
}
