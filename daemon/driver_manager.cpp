//
//  driver_manager.cpp
//
//  Copyright (c) 2019 2020 Andrea Bondavalli. All rights reserved.
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

#include <cstring>
#include <thread>

#include "log.hpp"
#include "driver_manager.hpp"

static const std::vector<std::string> alsa_msg_str = {"Start",
                                                      "Stop",
                                                      "Reset",
                                                      "StartIO",
                                                      "StopIO",
                                                      "SetSampleRate",
                                                      "GetSampleRate",
                                                      "GetAudioMode",
                                                      "SetDSDAudioMode",
                                                      "SetTICFrameSizeAt1FS",
                                                      "SetMaxTICFrameSize",
                                                      "SetNumberOfInputs",
                                                      "SetNumberOfOutputs",
                                                      "GetNumberOfInputs",
                                                      "GetNumberOfOutputs",
                                                      "SetInterfaceName",
                                                      "Add_RTPStream",
                                                      "Remove_RTPStream",
                                                      "Update_RTPStream_Name",
                                                      "GetPTPInfo",
                                                      "Hello",
                                                      "Bye",
                                                      "Ping",
                                                      "SetMasterOutputVolume",
                                                      "SetMasterOutputSwitch",
                                                      "GetMasterOutputVolume",
                                                      "GetMasterOutputSwitch",
                                                      "SetPlayoutDelay",
                                                      "SetCaptureDelay",
                                                      "GetRTPStreamStatus",
                                                      "SetPTPConfig",
                                                      "GetPTPConfig",
                                                      "GetPTPStatus",
                                                      "AddPCM",
                                                      "RemovePCM"};

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
  if (!DriverHandler::init(config)) {
    return false;
  }

  /* W7: get_current_sample_rate() reflects the manager-wide rate, which
   * the kernel keeps equal to chip 0 / group 0's rate. Per-group rates
   * for groups 1+ are resolved via Config::rate_for_group at each use
   * site (sources, AddPCM); this stays the group-0 / default rate. */
  sample_rate_ = config.rate_for_group(0);

  TPTPConfig ptp_config;
  ptp_config.ui8Domain = config.get_ptp_domain();
  ptp_config.ui8DSCP = config.get_ptp_dscp();

  if (hello())
    return false;

  bool res(false);
  if (config.get_driver_restart()) {
    res = start() || reset(0) ||
          set_interface_name(config.get_interface_name()) ||
          set_ptp_config(ptp_config) ||
          set_tic_frame_size_at_1fs(config.get_tic_frame_size_at_1fs()) ||
          set_max_tic_frame_size(config.get_max_tic_frame_size()) ||
          /* W7 (Decision 10): group 0's resolved rate is pushed via the
           * manager-wide SetSampleRate — chip 0 is probe-created and has
           * no AddPCM, so this is how its rate is set. Groups 1+ carry
           * their own rate in AddPCM below. (Since W5 the kernel ticks
           * off-rate chips correctly, so AddPCM no longer requires the
           * rate to equal m_SampleRate; this SetSampleRate-first step now
           * concerns only group 0.) */
          set_sample_rate(config.rate_for_group(0));
    /* multi-rate Stage 1: PCM 0 already exists (created at module probe).
     * Issue AddPCM for groups 1..N-1, each at its own resolved rate (W7).
     * playout_delay is currently stored manager-wide in the kernel, so we
     * only push it once (from group 0) to make the "shared delay" Stage 1
     * limitation visible — any per-group delay other than group 0's is
     * logged but not applied. */
    if (!res) {
      int32_t shared_playout_delay = 0;
      bool have_shared_delay = false;
      for (const auto& g : config.get_device_groups()) {
        if (g.id > 0) {
          if (auto ec = add_pcm(g.id, config.rate_for_group(g.id),
                                g.num_inputs, g.num_outputs, g.name)) {
            BOOST_LOG_TRIVIAL(fatal)
                << "driver_manager:: add_pcm id=" << (int)g.id
                << " failed: " << ec.message();
            res = true;
            break;
          }
        }
        if (g.id == 0) {
          shared_playout_delay = g.playout_delay;
          have_shared_delay = true;
        } else if (g.playout_delay != 0) {
          BOOST_LOG_TRIVIAL(warning)
              << "driver_manager:: per-group playout_delay not supported "
                 "in Stage 1; ignoring playout_delay=" << g.playout_delay
              << " on device_group id=" << (int)g.id
              << " (only id=0's playout_delay is applied)";
        }
      }
      if (!res && have_shared_delay) {
        if (auto ec = set_playout_delay(0, shared_playout_delay)) {
          BOOST_LOG_TRIVIAL(warning)
              << "driver_manager:: set_playout_delay failed: " << ec.message();
        }
      }
    }
  }

  return !res;
}

bool DriverManager::terminate(const Config& config) {
  if (config.get_driver_restart()) {
    stop();
  }
  bye();
  return DriverHandler::terminate(config);
}

std::error_code DriverManager::hello() {
  this->send_command(MT_ALSA_Msg_Hello, 0, nullptr);
  return retcode_;
}

std::error_code DriverManager::bye() {
  this->send_command(MT_ALSA_Msg_Bye, 0, nullptr);
  return retcode_;
}

std::error_code DriverManager::start() {
  this->send_command(MT_ALSA_Msg_Start, 0, nullptr);
  return retcode_;
}

std::error_code DriverManager::stop() {
  this->send_command(MT_ALSA_Msg_Stop, 0, nullptr);
  return retcode_;
}

std::error_code DriverManager::reset(uint8_t pcm_id) {
  /* Payload (multi-rate Stage 1+): int32_t pcm_id. Streams ARE tagged
   * with m_uiPCMId, but the kernel-side reset handler currently still
   * removes all streams regardless of pcm_id — per-pcm_id reset is a
   * Stage 2/3 follow-up. See manager.c MT_ALSA_Msg_Reset handler. */
  int32_t id = pcm_id;
  this->send_command(MT_ALSA_Msg_Reset, sizeof(id),
                     reinterpret_cast<const uint8_t*>(&id));
  return retcode_;
}

std::error_code DriverManager::set_ptp_config(const TPTPConfig& config) {
  BOOST_LOG_TRIVIAL(info) << "driver_manager:: setting PTP Domain "
                          << (int)config.ui8Domain << " DSCP "
                          << (int)config.ui8DSCP;
  this->send_command(MT_ALSA_Msg_SetPTPConfig, sizeof(TPTPConfig),
                     reinterpret_cast<const uint8_t*>(&config));
  return retcode_;
}

std::error_code DriverManager::get_ptp_config(TPTPConfig& config) {
  this->send_command(MT_ALSA_Msg_GetPTPConfig);
  if (!retcode_) {
    memcpy(&config, recv_data_, sizeof(TPTPConfig));
    BOOST_LOG_TRIVIAL(debug)
        << "driver_manager:: PTP Domain " << (int)config.ui8Domain << " DSCP "
        << (int)config.ui8DSCP;
  }
  return retcode_;
}

std::error_code DriverManager::get_ptp_status(TPTPStatus& status) {
  this->send_command(MT_ALSA_Msg_GetPTPStatus);
  if (!retcode_) {
    memcpy(&status, recv_data_, sizeof(TPTPStatus));
    BOOST_LOG_TRIVIAL(debug)
        << "driver_manager:: PTP Status "
        << ptp_status_str[status.nPTPLockStatus] << " GMID "
        << status.ui64GMID[0] << " Jitter " << status.i32ClockJitter;
  }
  return retcode_;
}

std::error_code DriverManager::set_interface_name(const std::string& ifname) {
  BOOST_LOG_TRIVIAL(info) << "driver_manager:: setting interface " << ifname;
  this->send_command(MT_ALSA_Msg_SetInterfaceName, ifname.length() + 1,
                     reinterpret_cast<const uint8_t*>(ifname.c_str()));
  return retcode_;
}

std::error_code DriverManager::add_pcm(uint8_t pcm_id,
                                       uint32_t sample_rate,
                                       uint32_t num_inputs,
                                       uint32_t num_outputs,
                                       const std::string& name) {
  /* Multi-rate: bounds-check pcm_id against the kernel's MAX_PCMS before
   * issuing netlink, so user-visible errors say "id N out of range [1..15]"
   * rather than the generic errno the kernel returns. Keep this in sync
   * with MAX_PCMS in 3rdparty/ravenna-alsa-lkm/driver/manager.h (bumped to
   * 16 in Stage 2 for the target deployment with HT + multi-rate music). */
  static constexpr uint8_t kMaxPcmId = 15;  // MAX_PCMS=16 → ids 0..15
  if (pcm_id == 0 || pcm_id > kMaxPcmId) {
    BOOST_LOG_TRIVIAL(fatal)
        << "driver_manager:: add_pcm: pcm_id " << (int)pcm_id
        << " out of range [1.." << (int)kMaxPcmId
        << "] (pcm_id 0 is the default PCM, created at module probe)";
    return std::make_error_code(std::errc::invalid_argument);
  }
  /* Stage 1: sample_rate must equal the manager-wide rate. We send it
   * anyway for forward-compat with Stage 2; the kernel validates. */
  struct MT_ALSA_AddPCM_args args;
  memset(&args, 0, sizeof(args));
  args.pcm_id = pcm_id;
  args.sample_rate = sample_rate;
  args.num_inputs = num_inputs;
  args.num_outputs = num_outputs;
  /* W7: ALSA device name (truncated to the wire field, always NUL-terminated). */
  std::strncpy(args.name, name.c_str(), sizeof(args.name) - 1);
  BOOST_LOG_TRIVIAL(info) << "driver_manager:: add PCM id=" << (int)pcm_id
                          << " rate=" << sample_rate << " in=" << num_inputs
                          << " out=" << num_outputs << " name=\"" << args.name
                          << "\"";
  this->send_command(MT_ALSA_Msg_AddPCM, sizeof(args),
                     reinterpret_cast<const uint8_t*>(&args));
  return retcode_;
}

std::error_code DriverManager::add_rtp_stream(
    uint8_t pcm_id,
    const TRTP_stream_info& stream_info,
    uint64_t& stream_handle) {
  /* Payload (multi-rate Stage 1+): {int32_t pcm_id, TRTP_stream_info}.
   * pcm_id duplicates stream_info.m_uiPCMId at the wire level so the
   * kernel can route before parsing the stream struct; callers should
   * keep the two in sync (session_manager does this in add_source/sink). */
  uint8_t buf[sizeof(int32_t) + sizeof(TRTP_stream_info)];
  int32_t id = pcm_id;
  memcpy(buf, &id, sizeof(id));
  memcpy(buf + sizeof(id), &stream_info, sizeof(stream_info));
  this->send_command(MT_ALSA_Msg_Add_RTPStream, sizeof(buf), buf);
  if (!retcode_) {
    memcpy(&stream_handle, recv_data_, sizeof(stream_handle));
    BOOST_LOG_TRIVIAL(info)
        << "driver_manager:: add RTP stream pcm_id=" << (int)pcm_id
        << " success handle " << stream_handle;
  }
  return retcode_;
}

std::error_code DriverManager::get_rtp_stream_status(
    uint64_t stream_handle,
    TRTP_stream_status& stream_status) {
  this->send_command(MT_ALSA_Msg_GetRTPStreamStatus, sizeof(uint64_t),
                     reinterpret_cast<const uint8_t*>(&stream_handle));
  if (!retcode_) {
    memcpy(&stream_status, recv_data_, sizeof(stream_status));
  }
  return retcode_;
}

std::error_code DriverManager::remove_rtp_stream(uint64_t stream_handle) {
  this->send_command(MT_ALSA_Msg_Remove_RTPStream, sizeof(uint64_t),
                     reinterpret_cast<const uint8_t*>(&stream_handle));
  return retcode_;
}

std::error_code DriverManager::ping() {
  this->send_command(MT_ALSA_Msg_Ping);
  return retcode_;
}

std::error_code DriverManager::set_sample_rate(uint32_t sample_rate) {
  this->send_command(MT_ALSA_Msg_SetSampleRate, sizeof(uint32_t),
                     reinterpret_cast<const uint8_t*>(&sample_rate));
  return retcode_;
}

std::error_code DriverManager::set_tic_frame_size_at_1fs(uint64_t frame_size) {
  this->send_command(MT_ALSA_Msg_SetTICFrameSizeAt1FS, sizeof(uint64_t),
                     reinterpret_cast<const uint8_t*>(&frame_size));
  return retcode_;
}

std::error_code DriverManager::set_max_tic_frame_size(uint64_t frame_size) {
  this->send_command(MT_ALSA_Msg_SetMaxTICFrameSize, sizeof(uint64_t),
                     reinterpret_cast<const uint8_t*>(&frame_size));
  return retcode_;
}

std::error_code DriverManager::set_playout_delay(uint8_t pcm_id, int32_t delay) {
  /* Payload: {int32_t pcm_id, int32_t delay_in_samples}. */
  int32_t buf[2] = { pcm_id, delay };
  this->send_command(MT_ALSA_Msg_SetPlayoutDelay, sizeof(buf),
                     reinterpret_cast<const uint8_t*>(buf));
  return retcode_;
}

std::error_code DriverManager::get_sample_rate(uint32_t& sample_rate) {
  this->send_command(MT_ALSA_Msg_GetSampleRate);
  if (!retcode_) {
    memcpy(&sample_rate, recv_data_, sizeof(uint32_t));
    BOOST_LOG_TRIVIAL(info) << "driver_manager:: sample rate " << sample_rate;
  }
  return retcode_;
}

std::error_code DriverManager::get_number_of_inputs(uint8_t pcm_id,
                                                    int32_t& inputs) {
  /* Payload: int32_t pcm_id. Reply: uint32_t count. */
  int32_t id = pcm_id;
  this->send_command(MT_ALSA_Msg_GetNumberOfInputs, sizeof(id),
                     reinterpret_cast<const uint8_t*>(&id));
  if (!retcode_) {
    memcpy(&inputs, recv_data_, sizeof(uint32_t));
    BOOST_LOG_TRIVIAL(info) << "driver_manager:: number of inputs pcm_id="
                            << (int)pcm_id << " = " << inputs;
  }
  return retcode_;
}

std::error_code DriverManager::get_number_of_outputs(uint8_t pcm_id,
                                                     int32_t& outputs) {
  int32_t id = pcm_id;
  this->send_command(MT_ALSA_Msg_GetNumberOfOutputs, sizeof(id),
                     reinterpret_cast<const uint8_t*>(&id));
  if (!retcode_) {
    memcpy(&outputs, recv_data_, sizeof(uint32_t));
    BOOST_LOG_TRIVIAL(info) << "driver_manager:: number of outputs pcm_id="
                            << (int)pcm_id << " = " << outputs;
  }
  return retcode_;
}

void DriverManager::on_command_done(enum MT_ALSA_msg_id id,
                                    size_t size,
                                    const uint8_t* data) {
  BOOST_LOG_TRIVIAL(debug) << "driver_manager:: cmd " << alsa_msg_str[id]
                           << " done data len " << size;
  memcpy(recv_data_, data, size);
  retcode_ = std::error_code{};
}

void DriverManager::on_command_error(enum MT_ALSA_msg_id id,
                                     std::error_code error) {
  BOOST_LOG_TRIVIAL(error) << "driver_manager:: cmd " << alsa_msg_str[id]
                           << " failed with error " << error.message();
  retcode_ = error;
}

void DriverManager::on_event(enum MT_ALSA_msg_id id,
                             size_t& resp_size,
                             uint8_t* resp,
                             size_t req_size,
                             const uint8_t* req) {
  BOOST_LOG_TRIVIAL(debug) << "driver_manager:: event " << alsa_msg_str[id]
                           << " data len " << req_size;
  switch (id) {
    case MT_ALSA_Msg_Hello:
      resp_size = 0;
      break;
    case MT_ALSA_Msg_Bye:
      resp_size = 0;
      break;
    case MT_ALSA_Msg_SetMasterOutputVolume:
      if (req_size == sizeof(int32_t)) {
        memcpy(&output_volume_, req, req_size);
        BOOST_LOG_TRIVIAL(info)
            << "driver_manager:: event SetMasterOutputVolume "
            << output_volume_;
      }
      resp_size = 0;
      break;
    case MT_ALSA_Msg_SetMasterOutputSwitch:
      if (req_size == sizeof(int32_t)) {
        memcpy(&output_switch_, req, req_size);
        BOOST_LOG_TRIVIAL(info)
            << "driver_manager:: event SetMasterOutputSwitch "
            << output_switch_;
      }
      resp_size = 0;
      break;
    case MT_ALSA_Msg_SetSampleRate:
      if (req_size == sizeof(uint32_t)) {
        memcpy(&sample_rate_, req, req_size);
        BOOST_LOG_TRIVIAL(info)
            << "driver_manager:: event SetSampleRate " << sample_rate_;
      }
      resp_size = 0;
      break;
    case MT_ALSA_Msg_GetMasterOutputVolume:
      resp_size = sizeof(int32_t);
      memcpy(resp, &output_volume_, resp_size);
      BOOST_LOG_TRIVIAL(info)
          << "driver_manager:: event GetMasterOutputVolume " << output_volume_;
      break;
    case MT_ALSA_Msg_GetMasterOutputSwitch:
      resp_size = sizeof(int32_t);
      memcpy(resp, &output_switch_, resp_size);
      BOOST_LOG_TRIVIAL(info)
          << "driver_manager:: event GetMasterOutputSwitch " << output_switch_;
      break;
    default:
      BOOST_LOG_TRIVIAL(error) << "driver_manager:: unknown event "
                               << alsa_msg_str[id] << " data len " << req_size;
      break;
  }
}

void DriverManager::on_event_error(enum MT_ALSA_msg_id id,
                                   std::error_code error) {
  BOOST_LOG_TRIVIAL(error) << "driver_manager:: event " << alsa_msg_str[id]
                           << " error " << error;
}
