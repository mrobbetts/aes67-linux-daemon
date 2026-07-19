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
                                                      "RemoveCard",
                                                      "GetPCMStatus",
                                                      "SetPCMRate",
                                                      "PCMRateApplied",
                                                      "CancelPCMRate"};

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

  TPTPConfig ptp_config;
  ptp_config.ui8Domain = config.get_ptp_domain();
  ptp_config.ui8DSCP = config.get_ptp_dscp();

  if (hello())
    return false;

  bool res(false);
  if (config.get_driver_restart()) {
    /* W14: only the genuinely manager-wide setup (clock-domain config, TIC
     * frame sizing) goes through init now. There is no manager-wide sample
     * rate: every PCM self-rates via add_pcm_to_card (the daemon always sends
     * an explicit rate). */
    res = start() || reset(-1 /* all PCMs: clean slate */) ||
          set_interface_name(config.get_interface_name()) ||
          set_ptp_config(ptp_config) ||
          set_tic_frame_size_at_1fs(config.get_tic_frame_size_at_1fs()) ||
          set_max_tic_frame_size(config.get_max_tic_frame_size());
    /* W10.2: the SessionManager owns the runtime card set (rehydrated from
     * status.json); init() keeps only the manager-wide driver setup above. */
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
    /* W10.2: card teardown moved to SessionManager::terminate (it owns the
     * cards now and runs before this). The kernel's reset(-1) clean-slate at
     * next init stays the backstop for unclean exits (crash/SIGKILL). */
  }
  bye();
  return DriverHandler::terminate(config);
}

std::error_code DriverManager::hello() {
  return send_command(MT_ALSA_Msg_Hello, 0, nullptr);
}

std::error_code DriverManager::bye() {
  return send_command(MT_ALSA_Msg_Bye, 0, nullptr);
}

std::error_code DriverManager::start() {
  return send_command(MT_ALSA_Msg_Start, 0, nullptr);
}

std::error_code DriverManager::stop() {
  return send_command(MT_ALSA_Msg_Stop, 0, nullptr);
}

std::error_code DriverManager::reset(int32_t pcm_id) {
  /* Payload: int32_t pcm_id. W9 convention (see manager.c MT_ALSA_Msg_Reset):
   * pcm_id < 0 drains ALL streams (the init-time clean slate); pcm_id >= 0
   * drains only that PCM's streams, leaving the others running. */
  int32_t id = pcm_id;
  return send_command(MT_ALSA_Msg_Reset, sizeof(id),
                     reinterpret_cast<const uint8_t*>(&id));
}

std::error_code DriverManager::set_ptp_config(const TPTPConfig& config) {
  BOOST_LOG_TRIVIAL(info) << "driver_manager:: setting PTP Domain "
                          << (int)config.ui8Domain << " DSCP "
                          << (int)config.ui8DSCP;
  return send_command(MT_ALSA_Msg_SetPTPConfig, sizeof(TPTPConfig),
                     reinterpret_cast<const uint8_t*>(&config));
}

std::error_code DriverManager::get_ptp_config(TPTPConfig& config) {
  const auto ret =
      send_command(MT_ALSA_Msg_GetPTPConfig, 0, nullptr, sizeof(TPTPConfig),
                   reinterpret_cast<uint8_t*>(&config));
  if (!ret) {
    BOOST_LOG_TRIVIAL(debug)
        << "driver_manager:: PTP Domain " << (int)config.ui8Domain << " DSCP "
        << (int)config.ui8DSCP;
  }
  return ret;
}

std::error_code DriverManager::get_ptp_status(uint8_t domain, TPTPStatus& status) {
  const auto ret =
      send_command(MT_ALSA_Msg_GetPTPStatus, sizeof(domain),
                   reinterpret_cast<const uint8_t*>(&domain),
                   sizeof(TPTPStatus), reinterpret_cast<uint8_t*>(&status));
  if (!ret) {
    BOOST_LOG_TRIVIAL(debug)
        << "driver_manager:: PTP Status "
        << ptp_status_str[status.nPTPLockStatus] << " GMID "
        << status.ui64GMID[0] << " Jitter " << status.i32ClockJitter;
  }
  return ret;
}

std::error_code DriverManager::get_pcm_status(int32_t pcm_id,
                                              TPCMStatus& status) {
  return send_command(MT_ALSA_Msg_GetPCMStatus, sizeof(pcm_id),
                      reinterpret_cast<const uint8_t*>(&pcm_id),
                      sizeof(TPCMStatus), reinterpret_cast<uint8_t*>(&status));
}

std::error_code DriverManager::set_interface_name(const std::string& ifname) {
  BOOST_LOG_TRIVIAL(info) << "driver_manager:: setting interface " << ifname;
  return send_command(MT_ALSA_Msg_SetInterfaceName, ifname.length() + 1,
                     reinterpret_cast<const uint8_t*>(ifname.c_str()));
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
  return send_command(MT_ALSA_Msg_AddCard, sizeof(args),
                     reinterpret_cast<const uint8_t*>(&args));
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
  return send_command(MT_ALSA_Msg_AddPCM, sizeof(args),
                     reinterpret_cast<const uint8_t*>(&args));
}

std::error_code DriverManager::register_card(uint8_t card_handle) {
  int32_t handle = card_handle;
  BOOST_LOG_TRIVIAL(info) << "driver_manager:: register card handle="
                          << (int)card_handle;
  return send_command(MT_ALSA_Msg_RegisterCard, sizeof(handle),
                     reinterpret_cast<const uint8_t*>(&handle));
}

std::error_code DriverManager::remove_card(uint8_t card_handle) {
  int32_t handle = card_handle;
  BOOST_LOG_TRIVIAL(info) << "driver_manager:: remove card handle="
                          << (int)card_handle;
  return send_command(MT_ALSA_Msg_RemoveCard, sizeof(handle),
                     reinterpret_cast<const uint8_t*>(&handle));
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
  const auto ret =
      send_command(MT_ALSA_Msg_Add_RTPStream, sizeof(buf), buf,
                   sizeof(stream_handle),
                   reinterpret_cast<uint8_t*>(&stream_handle));
  if (!ret) {
    BOOST_LOG_TRIVIAL(info)
        << "driver_manager:: add RTP stream pcm_id=" << (int)pcm_id
        << " success handle " << stream_handle;
  }
  return ret;
}

std::error_code DriverManager::get_rtp_stream_status(
    uint64_t stream_handle,
    TRTP_stream_status& stream_status) {
  return send_command(MT_ALSA_Msg_GetRTPStreamStatus, sizeof(uint64_t),
                      reinterpret_cast<const uint8_t*>(&stream_handle),
                      sizeof(stream_status),
                      reinterpret_cast<uint8_t*>(&stream_status));
}

std::error_code DriverManager::remove_rtp_stream(uint64_t stream_handle) {
  return send_command(MT_ALSA_Msg_Remove_RTPStream, sizeof(uint64_t),
                     reinterpret_cast<const uint8_t*>(&stream_handle));
}

std::error_code DriverManager::ping() {
  return send_command(MT_ALSA_Msg_Ping);
}

std::error_code DriverManager::set_tic_frame_size_at_1fs(uint64_t frame_size) {
  return send_command(MT_ALSA_Msg_SetTICFrameSizeAt1FS, sizeof(uint64_t),
                     reinterpret_cast<const uint8_t*>(&frame_size));
}

std::error_code DriverManager::set_max_tic_frame_size(uint64_t frame_size) {
  return send_command(MT_ALSA_Msg_SetMaxTICFrameSize, sizeof(uint64_t),
                     reinterpret_cast<const uint8_t*>(&frame_size));
}

std::error_code DriverManager::set_playout_delay(uint8_t pcm_id, int32_t delay) {
  /* Payload: {int32_t pcm_id, int32_t delay_in_samples}. */
  int32_t buf[2] = { pcm_id, delay };
  return send_command(MT_ALSA_Msg_SetPlayoutDelay, sizeof(buf),
                     reinterpret_cast<const uint8_t*>(buf));
}

std::error_code DriverManager::set_capture_delay(uint8_t pcm_id, int32_t delay) {
  /* Payload: {int32_t pcm_id, int32_t delay_in_samples}. Advisory ALSA capture
   * latency (snd_pcm_delay()); the real receive buffering is the sink link
   * offset. Was never sent before W9 #14 (capture delay was dead end-to-end). */
  int32_t buf[2] = { pcm_id, delay };
  return send_command(MT_ALSA_Msg_SetCaptureDelay, sizeof(buf),
                     reinterpret_cast<const uint8_t*>(buf));
}

std::error_code DriverManager::set_pcm_rate(uint8_t pcm_id, uint32_t rate) {
  /* W15: payload {int32_t pcm_id, uint32_t sample_rate}. A {} return means
   * applied (chip was idle); DriverErrc::busy means armed (chip held open, the
   * kernel applies on last close — the caller retries). */
  int32_t buf[2] = { pcm_id, static_cast<int32_t>(rate) };
  return send_command(MT_ALSA_Msg_SetPCMRate, sizeof(buf),
                     reinterpret_cast<const uint8_t*>(buf));
}

std::error_code DriverManager::cancel_pcm_rate(uint8_t pcm_id) {
  /* W28: retract an armed in-place re-rate (disarm the kernel latch). Payload
   * {int32_t pcm_id}. Idempotent — a no-op when the chip isn't armed. */
  int32_t buf = pcm_id;
  return send_command(MT_ALSA_Msg_CancelPCMRate, sizeof(buf),
                     reinterpret_cast<const uint8_t*>(&buf));
}

std::error_code DriverManager::get_number_of_inputs(uint8_t pcm_id,
                                                    int32_t& inputs) {
  /* Payload: int32_t pcm_id. Reply: uint32_t count. */
  int32_t id = pcm_id;
  const auto ret = send_command(MT_ALSA_Msg_GetNumberOfInputs, sizeof(id),
                                reinterpret_cast<const uint8_t*>(&id),
                                sizeof(uint32_t),
                                reinterpret_cast<uint8_t*>(&inputs));
  if (!ret) {
    BOOST_LOG_TRIVIAL(info) << "driver_manager:: number of inputs pcm_id="
                            << (int)pcm_id << " = " << inputs;
  }
  return ret;
}

std::error_code DriverManager::get_number_of_outputs(uint8_t pcm_id,
                                                     int32_t& outputs) {
  int32_t id = pcm_id;
  const auto ret = send_command(MT_ALSA_Msg_GetNumberOfOutputs, sizeof(id),
                                reinterpret_cast<const uint8_t*>(&id),
                                sizeof(uint32_t),
                                reinterpret_cast<uint8_t*>(&outputs));
  if (!ret) {
    BOOST_LOG_TRIVIAL(info) << "driver_manager:: number of outputs pcm_id="
                            << (int)pcm_id << " = " << outputs;
  }
  return ret;
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
    case MT_ALSA_Msg_PCMRateApplied:
      /* W15: the kernel applied an armed in-place re-rate autonomously (the
       * holding client closed the PCM). Payload {int32_t pcm_id, int32_t rate}.
       * Hand it to the registered handler (session_manager) — which only
       * signals its worker — then ack. */
      if (req_size == sizeof(int32_t) * 2 && pcm_rate_applied_handler_) {
        int32_t pcm_id = *reinterpret_cast<const int32_t*>(req);
        uint32_t rate = *reinterpret_cast<const uint32_t*>(req + sizeof(int32_t));
        BOOST_LOG_TRIVIAL(info) << "driver_manager:: event PCMRateApplied pcm_id="
                                << pcm_id << " rate=" << rate;
        pcm_rate_applied_handler_(static_cast<uint8_t>(pcm_id), rate);
      }
      resp_size = 0;
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
