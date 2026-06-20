//  streamer.hpp
//
//  Copyright (c) 2019 2024 Andrea Bondavalli. All rights reserved.
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

#ifndef _STREAMER_HPP_
#define _STREAMER_HPP_

#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>
#include <alsa/asoundlib.h>
#include <faac.h>

#include "session_manager.hpp"

struct StreamerInfo {
  uint8_t status;
  uint16_t file_duration{0};
  uint8_t files_num{0};
  uint8_t player_buffer_files_num{0};
  uint8_t channels{0};
  uint8_t start_file_id{0};
  uint8_t current_file_id{0};
  uint32_t rate{0};
  std::string format;
};

struct StreamerLiveInfo {
  uint8_t sink_id{0};
  uint8_t file_id{0};
  uint8_t unwriteble{0};
};

class Streamer {
 public:
  static std::shared_ptr<Streamer> create(
      std::shared_ptr<SessionManager> session_manager,
      std::shared_ptr<Config> config);
  Streamer() = delete;
  Streamer(const Browser&) = delete;
  Streamer& operator=(const Browser&) = delete;

  bool init();
  bool terminate();

  std::error_code get_info(const StreamSink& sink, StreamerInfo& info);
  std::error_code get_stream(const StreamSink& sink,
                             uint8_t file_id,
                             uint8_t& current_file_id,
                             uint8_t& start_file_id,
                             uint32_t& file_count,
                             std::string& out);

  std::error_code live_stream_init(const StreamSink& sink,
                                   const std::string& ip,
                                   int port);
  bool live_stream_wait(httplib::DataSink& httpSink,
                        const std::string& ip,
                        int port);

 protected:
  explicit Streamer(std::shared_ptr<SessionManager> session_manager,
                    std::shared_ptr<Config> config)
      : session_manager_(session_manager), config_(config){};

 private:
  constexpr static snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE;

  bool pcm_xrun();
  bool pcm_suspend();
  ssize_t pcm_read(uint8_t* data, size_t rcount);

  bool on_ptp_status_change(const std::string& status);
  bool on_sink_add(uint8_t id);
  bool on_sink_remove(uint8_t id);
  /* lean streamer: pick the capture target (lowest pcm among streamed sinks)
   * and (re)start/stop the single capture context to follow it. */
  void reconcile_capture();
  bool start_capture(uint8_t pcm_id);
  bool stop_capture();
  /* a sink is served only while it is flagged for streaming, bound to the PCM
   * the single capture context currently follows, and that device is not held by
   * another consumer. */
  bool sink_eligible(const StreamSink& sink) const {
    return sink.stream && sink.pcm == active_capture_pcm_ && !capture_busy_;
  }
  bool setup_codec(const StreamSink& sink);
  void open_files(uint8_t files_id);
  void close_files(uint8_t files_id);
  void save_files(uint8_t files_id);

  std::shared_ptr<SessionManager> session_manager_;
  std::shared_ptr<Config> config_;
  snd_pcm_uframes_t chunk_samples_{0};
  size_t bytes_per_frame_{0};
  uint16_t file_duration_{1};
  uint8_t files_num_{8};
  uint8_t player_buffer_files_num_{1};
  size_t buffer_samples_{0};
  std::unordered_map<uint8_t, size_t> total_sink_samples_;
  uint32_t buffer_offset_{0};
  std::unordered_map<uint8_t, std::shared_mutex> streams_mutex_;
  std::unordered_map<uint8_t, std::stringstream> tmp_streams_;
  std::map<std::pair<uint8_t, uint8_t>, std::stringstream> output_streams_;
  std::unordered_map<uint8_t, uint32_t> output_ids_;
  uint32_t file_counter_{0};
  std::atomic<uint8_t> file_id_{0};
  std::unique_ptr<uint8_t[]> buffer_;
  std::unordered_map<uint8_t, std::unique_ptr<uint8_t[]> > out_buffer_;
  std::unordered_map<uint8_t, uint32_t> out_buffer_size_{0};
  uint8_t channels_{8};
  uint32_t rate_{0};
  /* lean streamer: the pcm_id of the PCM the single capture context currently
   * follows, and whether the last open failed because the device is held by
   * another consumer (e.g. CamillaDSP). Retried on the next reconcile. */
  uint8_t active_capture_pcm_{0};
  bool capture_busy_{false};
  std::future<bool> res_;
  snd_pcm_t* capture_handle_;
  std::atomic_bool running_{false};
#if defined(FAAC_VERSION_MAJOR)
  std::unordered_map<uint8_t, faac_encoder*> faac_;
#else
  std::unordered_map<uint8_t, faacEncHandle> faac_;
#endif
  std::unordered_map<uint8_t, uint32_t> codec_in_samples_;
  std::unordered_map<uint8_t, uint32_t> codec_out_buffer_size_;
  std::unordered_map<uint8_t, std::mutex> faac_mutex_;
  std::map<std::pair<std::string, int>, StreamerLiveInfo> liveInfos_;
};

#endif
