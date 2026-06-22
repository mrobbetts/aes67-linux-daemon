//
//  error_code.hpp
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

#ifndef _ERROR_CODE_HPP_
#define _ERROR_CODE_HPP_

#include <system_error>

// Driver errors
enum class DriverErrc {
  unknown = 10,                      // unhandled code from driver
  invalid_data_size = 11,            // driver -315 invalid data size
  invalid_value = 12,                // driver -815 invalid value specified
  command_failed = 13,               // driver -401 command failed
  command_not_found = 14,            // driver -404 command not found
  unknown_command = 15,              // driver -314 unknown command
  invalid_daemon_response = 16,      // driver -303 invalid daemon response
  invalid_daemon_response_size = 17  // driver -302 invalid daemon response
};

namespace std {
template <>
struct is_error_code_enum<DriverErrc> : true_type {};
}  // namespace std

std::error_code make_error_code(DriverErrc);

std::error_code get_driver_error(int code);

// Daemon errors
enum class DaemonErrc {
  invalid_stream_id = 40,     // daemon invalid stream id
  stream_id_in_use = 41,      // daemon stream id is in use
  stream_id_not_in_use = 42,  // daemon stream not in use
  invalid_url = 43,           // daemon invalid URL
  cannot_retrieve_sdp = 44,   // daemon cannot retrieve SDP
  cannot_parse_sdp = 45,      // daemon cannot parse SDP
  stream_name_in_use = 46,    // daemon source or sink name in use
  cannot_retrieve_mac = 47,   // daemon cannot retrieve MAC for IP
  streamer_invalid_ch = 48,   // daemon streamer sink channel not captured
  streamer_retry_later = 49,  // daemon streamer not enough samples buffered
  streamer_not_running = 50,  // daemon streamer not running
  invalid_pcm = 51,           // stream references an undeclared PCM/device group
  invalid_channel_map = 52,   // channel map too large or has duplicate channels
  channel_map_overlap = 53,   // channel map collides with another sink on the same PCM
  invalid_sample_rate = 54,   // stream rate doesn't match its device group's rate (W7)
  card_slots_exhausted = 55,  // no free card handle / pcm id for a new card (W10.2)
  invalid_card_handle = 56,   // operation references an unknown card handle (W10.2)
  card_name_in_use = 57,      // a card with this name already exists (W10.2)
  invalid_card_name = 58,     // card name empty/invalid or card not found (W10.2)
  pcm_name_in_use = 59,       // a pcm with this name already exists on the card (W10.2)
  invalid_pcm_name = 60,      // pcm name empty/invalid or pcm not found (W10.2)
  invalid_card_domain = 61,   // card PTP domain out of range [0, MAX_DOMAINS) (W11)
  invalid_rate_change_mode = 62,  // card rate-change mode not "recreate"/"in-place" (W15)
  /* NB: get_http_error_status maps daemon codes < send_invalid_size to HTTP 400
   * (client errors) and the rest to 500 — keep request-validation codes below
   * send_invalid_size. */
  send_invalid_size = 70,     // daemon data size too big for buffer
  send_u2k_failed = 71,       // daemon failed to send command to driver
  send_k2u_failed = 72,       // daemon failed to send event response to driver
  receive_u2k_failed = 73,    // daemon failed to receive response from driver
  receive_k2u_failed = 74,    // daemon failed to receive event from driver
  invalid_driver_response = 75  // unexpected driver command response code
};

namespace std {
template <>
struct is_error_code_enum<DaemonErrc> : true_type {};
}  // namespace std

std::error_code make_error_code(DaemonErrc);

#endif
