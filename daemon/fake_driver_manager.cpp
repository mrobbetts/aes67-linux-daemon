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
  /* W14: no manager-wide sample rate; every PCM self-rates via add_pcm_to_card. */
  TPTPConfig ptp_config;
  ptp_config.ui8Domain = config.get_ptp_domain();
  ptp_config.ui8DSCP = config.get_ptp_dscp();

  if (hello())
    return false;

  bool res(false);
  if (config.get_driver_restart()) {
    /* W10.2: mirror the real DriverManager — card creation moved to the
     * SessionManager (it owns the runtime card set and brings cards up in
     * load_status). init() keeps only the manager-wide setup. */
    res = start() || reset(-1 /* all PCMs: clean slate */) ||
          set_interface_name(config.get_interface_name()) ||
          set_ptp_config(ptp_config) ||
          set_tic_frame_size_at_1fs(config.get_tic_frame_size_at_1fs()) ||
          set_max_tic_frame_size(config.get_max_tic_frame_size());
  }

  return !res;
}

bool DriverManager::terminate(const Config& config) {
  if (config.get_driver_restart()) {
    stop();
    /* W10.2: card teardown moved to SessionManager::terminate (mirror the real
     * DriverManager). reset(-1) at next init is the backstop for unclean exits. */
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

std::error_code DriverManager::get_ptp_status(uint8_t domain, TPTPStatus& status) {
  (void)domain;
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

std::error_code DriverManager::get_pcm_status(int32_t pcm_id,
                                              TPCMStatus& status) {
  (void)pcm_id;
  status.nTICLockStatus = PTPLS_UNLOCKED;
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

std::error_code DriverManager::set_capture_delay(uint8_t /*pcm_id*/,
                                                 int32_t /*delay*/) {
  return std::error_code{};
}

std::error_code DriverManager::set_pcm_rate(uint8_t /*pcm_id*/,
                                            uint32_t /*rate*/) {
  /* W15: the fake driver has no open clients, so a re-rate always "applies". */
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
