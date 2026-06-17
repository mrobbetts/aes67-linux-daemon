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
                                                      "RemovePCM",
                                                      "AddCard",
                                                      "RegisterCard",
                                                      "RemoveCard"};

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
    /* W10 multi-card: the manager-wide setup (clock-domain config, TIC frame
     * sizing) still applies, but the per-PCM rate no longer goes through the
     * manager-wide SetSampleRate. That call existed only to rate the
     * probe-created chip 0; with no probe card, every PCM — group 0 included —
     * gets its rate via add_pcm_to_card below, and calling SetSampleRate here
     * (with no chips yet) would needlessly enter the kernel's PTP-relock wait.
     * m_SampleRate stays at its kernel default (Layer 3 retires it). */
    res = start() || reset(-1 /* all PCMs: clean slate */) ||
          set_interface_name(config.get_interface_name()) ||
          set_ptp_config(ptp_config) ||
          set_tic_frame_size_at_1fs(config.get_tic_frame_size_at_1fs()) ||
          set_max_tic_frame_size(config.get_max_tic_frame_size());
    /* W10 multi-card: each device_group becomes its own ALSA card holding a
     * single PCM (the flat-config mapping for W10.1b; the nested
     * cards:[{pcms:[…]}] form is W10.2). Bring each up as
     * add_card -> add_pcm_to_card -> register_card so the card appears to
     * userspace with its PCM already attached. card_handle is the group's
     * enumeration order in [0, MAX_CARDS); the global pcm_id stays g.id. The
     * old group-0-is-the-probe-card special case is gone — group 0 is created
     * here like any other group.
     *
     * playout_delay is still manager-wide in the kernel (per-PCM is W9
     * remainder), so we push only group 0's once and warn on the rest. */
    if (!res) {
      uint8_t card_handle = 0;
      int32_t shared_playout_delay = 0;
      bool have_shared_delay = false;
      for (const auto& g : config.get_device_groups()) {
        if (auto ec = add_card(card_handle, g.name, g.domain)) {
          BOOST_LOG_TRIVIAL(fatal)
              << "driver_manager:: add_card handle=" << (int)card_handle
              << " (device_group id=" << (int)g.id
              << ") failed: " << ec.message();
          res = true;
          break;
        }
        if (auto ec = add_pcm_to_card(card_handle, g.id,
                                      config.rate_for_group(g.id),
                                      g.num_inputs, g.num_outputs, g.name)) {
          BOOST_LOG_TRIVIAL(fatal)
              << "driver_manager:: add_pcm_to_card card=" << (int)card_handle
              << " pcm_id=" << (int)g.id << " failed: " << ec.message();
          res = true;
          break;
        }
        if (auto ec = register_card(card_handle)) {
          BOOST_LOG_TRIVIAL(fatal)
              << "driver_manager:: register_card handle=" << (int)card_handle
              << " failed: " << ec.message();
          res = true;
          break;
        }
        ++card_handle;
        if (g.id == 0) {
          shared_playout_delay = g.playout_delay;
          have_shared_delay = true;
        } else if (g.playout_delay != 0) {
          BOOST_LOG_TRIVIAL(warning)
              << "driver_manager:: per-group playout_delay not supported "
                 "yet; ignoring playout_delay=" << g.playout_delay
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

DriverManager::~DriverManager() {
  /* Last line of defence: ensure the event-receiver thread is gone before this
   * object's members/vtable are destroyed. No-op after a clean terminate(). */
  stop_event_thread();
}

bool DriverManager::terminate(const Config& config) {
  if (config.get_driver_restart()) {
    stop();
    /* W10: the daemon owns the cards it created -- delete them on clean
     * shutdown so a stopped daemon doesn't leave hw:* cards lingering in the
     * kernel. The kernel's reset(-1) clean-slate stays the backstop for unclean
     * exits (crash/SIGKILL). Same enumeration order init() used for handles. */
    for (size_t i = 0; i < config.get_device_groups().size(); ++i)
      (void)remove_card(static_cast<uint8_t>(i));
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

std::error_code DriverManager::reset(int32_t pcm_id) {
  /* Payload: int32_t pcm_id. W9 convention (see manager.c MT_ALSA_Msg_Reset):
   * pcm_id < 0 drains ALL streams (the init-time clean slate); pcm_id >= 0
   * drains only that PCM's streams, leaving the others running. */
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

/* W10 multi-card constants — kept in lockstep by hand with the kernel (no
 * shared header across the daemon/kernel split, same caveat as
 * is_valid_pcm_rate in config.hpp). */
static constexpr uint8_t kMaxCards = 4;   // MR_ALSA_MAX_CARDS (audio_driver.h)
static constexpr uint8_t kMaxPcmId = 15;  // MAX_PCMS=16 → global ids 0..15

std::error_code DriverManager::add_card(uint8_t card_handle,
                                        const std::string& id,
                                        uint8_t domain) {
  if (card_handle >= kMaxCards) {
    BOOST_LOG_TRIVIAL(fatal)
        << "driver_manager:: add_card: handle " << (int)card_handle
        << " out of range [0.." << (int)(kMaxCards - 1) << "]";
    return std::make_error_code(std::errc::invalid_argument);
  }
  struct MT_ALSA_AddCard_args args;
  memset(&args, 0, sizeof(args));
  args.card_handle = card_handle;
  args.domain = domain;
  /* ALSA card id (hw:<id>); empty ⇒ kernel default. Truncated to the wire
   * field, always NUL-terminated. */
  std::strncpy(args.id, id.c_str(), sizeof(args.id) - 1);
  BOOST_LOG_TRIVIAL(info) << "driver_manager:: add card handle="
                          << (int)card_handle << " id=\"" << args.id
                          << "\" domain=" << (int)domain;
  this->send_command(MT_ALSA_Msg_AddCard, sizeof(args),
                     reinterpret_cast<const uint8_t*>(&args));
  return retcode_;
}

std::error_code DriverManager::add_pcm_to_card(uint8_t card_handle,
                                               uint8_t global_pcm_id,
                                               uint32_t sample_rate,
                                               uint32_t num_inputs,
                                               uint32_t num_outputs,
                                               const std::string& name) {
  /* global_pcm_id 0 is now a normal PCM (its own card), no longer the
   * probe-created default — the whole [0..MAX_PCMS) range is valid. */
  if (global_pcm_id > kMaxPcmId) {
    BOOST_LOG_TRIVIAL(fatal)
        << "driver_manager:: add_pcm_to_card: pcm_id " << (int)global_pcm_id
        << " out of range [0.." << (int)kMaxPcmId << "]";
    return std::make_error_code(std::errc::invalid_argument);
  }
  if (card_handle >= kMaxCards) {
    BOOST_LOG_TRIVIAL(fatal)
        << "driver_manager:: add_pcm_to_card: handle " << (int)card_handle
        << " out of range [0.." << (int)(kMaxCards - 1) << "]";
    return std::make_error_code(std::errc::invalid_argument);
  }
  struct MT_ALSA_AddPCM_args args;
  memset(&args, 0, sizeof(args));
  args.card_handle = card_handle;
  args.pcm_id = global_pcm_id;
  args.sample_rate = sample_rate;
  args.num_inputs = num_inputs;
  args.num_outputs = num_outputs;
  /* ALSA device name (truncated to the wire field, always NUL-terminated). */
  std::strncpy(args.name, name.c_str(), sizeof(args.name) - 1);
  BOOST_LOG_TRIVIAL(info) << "driver_manager:: add PCM card=" << (int)card_handle
                          << " pcm_id=" << (int)global_pcm_id
                          << " rate=" << sample_rate << " in=" << num_inputs
                          << " out=" << num_outputs << " name=\"" << args.name
                          << "\"";
  this->send_command(MT_ALSA_Msg_AddPCM, sizeof(args),
                     reinterpret_cast<const uint8_t*>(&args));
  return retcode_;
}

std::error_code DriverManager::register_card(uint8_t card_handle) {
  int32_t handle = card_handle;
  BOOST_LOG_TRIVIAL(info) << "driver_manager:: register card handle="
                          << (int)card_handle;
  this->send_command(MT_ALSA_Msg_RegisterCard, sizeof(handle),
                     reinterpret_cast<const uint8_t*>(&handle));
  return retcode_;
}

std::error_code DriverManager::remove_card(uint8_t card_handle) {
  int32_t handle = card_handle;
  BOOST_LOG_TRIVIAL(info) << "driver_manager:: remove card handle="
                          << (int)card_handle;
  this->send_command(MT_ALSA_Msg_RemoveCard, sizeof(handle),
                     reinterpret_cast<const uint8_t*>(&handle));
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
