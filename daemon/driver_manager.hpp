//
//  driver_manager.hpp
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

#ifndef _DRIVER_MANAGER_HPP_
#define _DRIVER_MANAGER_HPP_

#include <boost/asio.hpp>
#include <functional>
#include <mutex>

#include "RTP_stream_info.h"
#include "audio_streamer_clock_PTP_defs.h"
#include "driver_handler.hpp"

class DriverManager : public DriverHandler {
 public:
  static std::shared_ptr<DriverManager> create();

  /* Stop+join the event-receiver thread while this object is still fully
   * constructed. An init failure (the driver's, or a later component's) unwinds
   * and destroys this manager without calling terminate(); without this the
   * netlink clients are freed under the still-running thread (or it calls our
   * torn-down on_event) -> SEGV. See DriverHandler::stop_event_thread. */
  ~DriverManager() override;

  // driver interface
  bool init(const Config& config) override;
  bool terminate(const Config& config) override;

  std::error_code ping();  // unused, return error
  std::error_code set_ptp_config(const TPTPConfig& config);
  std::error_code get_ptp_config(TPTPConfig& config);
  std::error_code get_ptp_status(uint8_t domain, TPTPStatus& status);
  std::error_code get_pcm_status(int32_t pcm_id, TPCMStatus& status);
  std::error_code set_interface_name(const std::string& ifname);
  /* W10 multi-card: the daemon owns card bringup. A card is created
   * UNregistered (add_card), its PCM device(s) added (add_pcm_to_card),
   * then committed (register_card) so userspace sees the card with its full
   * PCM set at once. card_handle is a daemon-assigned index in [0, MAX_CARDS);
   * global_pcm_id is the kernel manager's m_apALSAChip[] slot (distinct from
   * the per-card ALSA device index, assigned by the kernel within the card). */
  std::error_code add_card(uint8_t card_handle,
                           const std::string& id,  // ALSA card id (hw:<id>)
                           uint8_t domain);
  std::error_code add_pcm_to_card(uint8_t card_handle,
                                  uint8_t global_pcm_id,
                                  uint32_t sample_rate,
                                  uint32_t num_inputs,
                                  uint32_t num_outputs,
                                  const std::string& name = "");  // ALSA dev name
  std::error_code register_card(uint8_t card_handle);
  std::error_code remove_card(uint8_t card_handle);
  std::error_code add_rtp_stream(uint8_t pcm_id,
                                 const TRTP_stream_info& stream_info,
                                 uint64_t& stream_handle);
  std::error_code get_rtp_stream_status(uint64_t stream_handle,
                                        TRTP_stream_status& stream_status);
  std::error_code remove_rtp_stream(uint64_t stream_handle);
  std::error_code set_tic_frame_size_at_1fs(uint64_t frame_size);
  std::error_code set_max_tic_frame_size(uint64_t frame_size);
  std::error_code set_playout_delay(uint8_t pcm_id, int32_t delay);
  std::error_code set_capture_delay(uint8_t pcm_id, int32_t delay);
  /* W15 in-place re-rate: re-key the PCM's (domain,rate) timer entry to `rate`
   * without recreating its card. Returns {} if applied immediately (chip idle),
   * DriverErrc::busy if the chip is held open (armed — the kernel applies it on
   * the client's last close; the caller should retry until it clears). */
  std::error_code set_pcm_rate(uint8_t pcm_id, uint32_t rate);
  /* W28: retract an armed in-place re-rate (disarm the kernel latch); idempotent. */
  std::error_code cancel_pcm_rate(uint8_t pcm_id);
  std::error_code get_number_of_inputs(uint8_t pcm_id, int32_t& inputs);
  std::error_code get_number_of_outputs(uint8_t pcm_id, int32_t& outputs);

  int32_t get_current_output_volume() const { return output_volume_; };
  int32_t get_current_output_switch() const { return output_switch_; };

  /* W15: register a handler for the kernel's PCMRateApplied K2U event (an armed
   * in-place re-rate that applied autonomously on the client's last close). The
   * handler runs on the event-receiver thread and MUST only signal — no
   * re-entrant driver/session work. */
  void set_pcm_rate_applied_handler(std::function<void(uint8_t, uint32_t)> h) {
    pcm_rate_applied_handler_ = std::move(h);
  }

 protected:
  // singleton, use create to build
  DriverManager() = default;

  // these are used in init/terminate
  std::error_code hello();
  std::error_code start();
  std::error_code stop();
  std::error_code reset(int32_t pcm_id);  // pcm_id < 0 = all PCMs (clean slate); >= 0 = that PCM only (W9)
  std::error_code bye();

  /* D2 (2026-06 audit): command results now come back BY VALUE from
   * send_command (reply payload copied into the caller's buffer under the
   * command mutex) — the shared retcode_/recv_data_ members and the
   * on_command_done/on_command_error callbacks are gone; concurrent driver
   * calls from REST threads and the worker can no longer swap each other's
   * replies. */
  void on_event(enum MT_ALSA_msg_id id,
                size_t& res_size,
                uint8_t* res,
                size_t req_size = 0,
                const uint8_t* req = nullptr) override;
  void on_event_error(enum MT_ALSA_msg_id id, std::error_code error) override;

 private:
  int32_t output_volume_{-20};
  int32_t output_switch_{0};
  std::function<void(uint8_t /*pcm_id*/, uint32_t /*rate*/)>
      pcm_rate_applied_handler_;  // W15
};

#endif
