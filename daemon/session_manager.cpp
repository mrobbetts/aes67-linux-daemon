//
//  seesion_manager.cpp
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

#define CPPHTTPLIB_PAYLOAD_MAX_LENGTH 4096  // max for SDP file
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include <stdlib.h>
#include <httplib.h>

#include <boost/algorithm/string.hpp>
#include <boost/foreach.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>  /* D7: std::rename / std::remove for atomic status writes */
#include <experimental/map>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <vector>

#include "json.hpp"
#include "log.hpp"
#include "rtsp_client.hpp"
#include "utils.hpp"
#include "session_manager.hpp"
#include "interface.hpp"

static uint8_t get_codec_word_length(std::string_view codec) {
  if (codec == "L16") {
    return 2;
  }
  if (codec == "L24") {
    return 3;
  }
  if (codec == "L2432" || codec == "AM824") {
    return 4;
  }
  if (codec == "DSD64") {
    return 1;
  }
  if (codec == "DSD128") {
    return 2;
  }
  if (codec == "DSD64_32" || codec == "DSD128_32" || codec == "DSD256") {
    return 4;
  }
  return 0;
}

bool SessionManager::parse_sdp(const std::string& sdp, StreamInfo& info) const {
  /*
  v=0
  o=- 2831159553 317021570 IN IP4 192.168.1.17
  s=Daemon a8c01101 ALSA Source 0
  t=0 0
  a=group:DUP 1 2
  m=audio 5004 RTP/AVP 98
  c=IN IP4 239.1.0.1/15
  a=source-filter: incl IN IP4 239.1.0.1 192.168.1.17
  a=rtpmap:98 L24/48000/2
  a=sync-time:0
  a=framecount:48
  a=ptime:1
  a=mediaclk:direct=0
  a=clock-domain:PTPv2 0
  a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-50-36-33:0
  a=recvonly
  a=mid:1
  m=audio 5006 RTP/AVP 98
  c=IN IP4 239.1.0.1/15
  a=source-filter: incl IN IP4 239.1.0.1 192.168.1.18
  a=rtpmap:98 L24/48000/2
  a=sync-time:0
  a=framecount:48
  a=ptime:1
  a=mediaclk:direct=0
  a=clock-domain:PTPv2 0
  a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-50-36-33:0
  a=mid:2
  */

  int num = 0;
  try {
    enum class sdp_parser_status { init, time, media };
    sdp_parser_status status = sdp_parser_status::init;
    std::stringstream ssstrem(sdp);
    std::string line;
    int mid = -1;
    bool dup = false;
    while (getline(ssstrem, line, '\n') && mid < media_max) {
      boost::trim(line);
      ++num;
      if (line[1] != '=') {
        BOOST_LOG_TRIVIAL(error)
            << "session_manager:: invalid SDP file at line " << num;
        return false;
      }
      std::string val = line.substr(2);
      switch (line[0]) {
        case 'v':
          /* v=0 */
          if (val != "0") {
            BOOST_LOG_TRIVIAL(error)
                << "session_manager:: unsupported SDP version at line " << num;
            return false;
          }
          break;
        case 'o': {
          std::vector<std::string> fields;
          boost::split(fields, val, [line](char c) { return c == ' '; });
          if (fields.size() < 6) {
            BOOST_LOG_TRIVIAL(warning)
                << "session_manager:: invalid origin at line " << num;
          } else {
            info.origin.username = fields[0];
            info.origin.session_id = fields[1];
            info.origin.session_version = std::stoull(fields[2]);
            info.origin.network_type = fields[3];
            info.origin.address_type = fields[4];
            info.origin.unicast_address = fields[5];
          }
        } break;
        case 't':
          /* t=0 0 */
          status = sdp_parser_status::time;
          break;
        case 'a': {
          auto pos = val.find(':');
          if (pos == std::string::npos) {
            /* skip this attribute */
            break;
          }
          std::string name = val.substr(0, pos);
          std::string value = val.substr(pos + 1);
          switch (status) {
            case sdp_parser_status::init:
            case sdp_parser_status::time:
              /* session level attributes */
              if (name == "group") {
                /* a=group:DUP 1 2 */
                if (value.substr(0, 3) == "DUP") {
                  dup = true;
                }
              } else if (name == "clock-domain") {
                /* a=clock-domain:PTPv2 0 */
                if (value.substr(0, 5) != "PTPv2") {
                  BOOST_LOG_TRIVIAL(error)
                      << "session_manager:: unsupported PTP "
                         "clock version in SDP at line "
                      << num;
                  return false;
                }
              }
              break;
            case sdp_parser_status::media:
              /* audio media attributes */
              if (name == "rtpmap") {
                /* a=rtpmap:98 L16/44100/8 */
                std::vector<std::string> fields;
                boost::split(fields, value,
                             [line](char c) { return c == ' ' || c == '/'; });
                if (fields.size() < 4) {
                  BOOST_LOG_TRIVIAL(error) << "session_manager:: invalid audio "
                                              "rtpmap in SDP at line "
                                           << num;
                  return false;
                }
                // if matching payload
                if (info.stream[mid].m_byPayloadType == std::stoi(fields[0])) {
                  strncpy(info.stream[mid].m_cCodec, fields[1].c_str(),
                          sizeof(info.stream[mid].m_cCodec) - 1);
                  info.stream[mid].m_byWordLength =
                      get_codec_word_length(fields[1]);
                  info.stream[mid].m_ui32SamplingRate = std::stoul(fields[2]);
                  if (info.stream[mid].m_byNbOfChannels !=
                      std::stoi(fields[3])) {
                    BOOST_LOG_TRIVIAL(warning)
                        << "session_manager:: invalid audio channel "
                           "number in SDP at line "
                        << num << ", using "
                        << (int)info.stream[mid].m_byNbOfChannels;
                    /*return false; */
                  }
                }
              } else if (name == "sync-time") {
                /* a=sync-time:0 */
                info.stream[mid].m_ui32RTPTimestampOffset = std::stoul(value);
              } else if (name == "framecount") {
                /* a=framecount:64-192 */
              } else if (name == "ptime") {
                /* a=mediaclk:ptime=4.35374165 */
                info.stream[mid].m_ui32MaxSamplesPerPacket =
                    (static_cast<double>(info.stream[mid].m_ui32SamplingRate) *
                     std::stod(value)) /
                    1000;
              } else if (name == "mediaclk") {
                /* a=mediaclk:direct=0 */
                std::vector<std::string> fields;
                boost::split(fields, value,
                             [line](char c) { return c == '='; });
                if (fields.size() == 2 && fields[0] == "direct") {
                  info.stream[mid].m_ui32RTPTimestampOffset =
                      std::stoul(fields[1]);
                }
              } else if (name == "ts-refclk" && !info.ignore_refclk_gmid) {
                /* a=ts-refclk:ptp=IEEE1588-2008:00-0C-29-FF-FE-0E-90-C8:0
                 * Validate the stream's advertised GM against the GMID we are
                 * locked to IN THE DOMAIN THE SDP DECLARES (fields[2]) — not a
                 * single daemon-wide domain (W11: a sink may live in any domain).
                 * The per-domain GMID is mirrored from the kernel into
                 * ptp_status_by_domain_; mirror get_source_sdp_'s lookup. We only
                 * reject a PROVEN mismatch: if that domain isn't mirrored yet we
                 * can't validate, so we accept rather than false-reject (which is
                 * exactly what the old global-gmid check did to other domains). */
                std::vector<std::string> fields;
                boost::split(fields, value,
                             [line](char c) { return c == ':'; });
                if (fields.size() == 3) {
                  uint8_t sdp_domain = static_cast<uint8_t>(stoi(fields[2]));
                  std::shared_lock ptp_lock(ptp_mutex_);
                  auto it = ptp_status_by_domain_.find(sdp_domain);
                  if (it != ptp_status_by_domain_.end() &&
                      !it->second.gmid.empty() && fields[1] != it->second.gmid) {
                    BOOST_LOG_TRIVIAL(warning)
                        << "session_manager:: SDP grand master " << fields[1]
                        << " in domain " << (int)sdp_domain
                        << " doesn't match the locked GM " << it->second.gmid
                        << " at line " << num;
                    return false;
                  }
                }
              } else if (name == "mid") {
                /* a=mid:1*/
              }
              break;
          }
        } break;
        case 'm': {
          /* m=audio 5004 RTP/AVP 98 */
          std::vector<std::string> fields;
          boost::split(fields, val, [line](char c) { return c == ' '; });
          if (fields.size() < 4) {
            BOOST_LOG_TRIVIAL(error)
                << "session_manager:: invalid nedia in SDP at line " << num;
            return false;
          }
          if (fields[0] == "audio") {
            /* new audio media */
            if (++mid < media_max) {
              info.stream[mid].m_usDestPort = std::stoi(fields[1]);
              info.stream[mid].m_byPayloadType =
                  std::stoi(fields[3]); /* take first payload */
              status = sdp_parser_status::media;
            }
          }
          break;
        }
        case 'c':
          /* c=IN IP4 239.1.0.12/15 */
          /* c=IN IP4 10.0.0.1 */
          /* connection info of audio media */
          if (status == sdp_parser_status::media ||
              /* generic connection info */
              status == sdp_parser_status::init) {
            std::vector<std::string> fields;
            boost::split(fields, val,
                         [line](char c) { return c == ' ' || c == '/'; });
            if (fields.size() < 3) {
              BOOST_LOG_TRIVIAL(error)
                  << "session_manager:: invalid connection in SDP at line "
                  << num;
              return false;
            }
            if (fields[0] != "IN" || fields[1] != "IP4") {
              BOOST_LOG_TRIVIAL(error)
                  << "session_manager:: unsupported connection in SDP at line "
                  << num;
              return false;
            }

            uint32_t destIP =
#if BOOST_VERSION < 108700
                ip::address_v4::from_string(fields[2].c_str()).to_ulong();
#else

                ip::make_address(fields[2].c_str()).to_v4().to_uint();
#endif
            if (destIP == INADDR_NONE) {
              BOOST_LOG_TRIVIAL(error) << "session_manager:: invalid IPv4 "
                                          "connection address in SDP at line "
                                       << num;
              return false;
            }
            uint8_t byTTL = fields.size() > 3 ? std::stoi(fields[3]) : 64;

            if (status == sdp_parser_status::init) {
              /* generic connection info, copy to all media */
              for (int i = 0; i < media_max; ++i) {
                info.stream[i].m_ui32DestIP = destIP;
                info.stream[i].m_byTTL = byTTL;
              }
            } else {
              /* connection info of audio media, copy only to current media */
              info.stream[mid].m_ui32DestIP = destIP;
              info.stream[mid].m_byTTL = byTTL;
            }
          }
          break;
        default:
          if (line[0] < 'a' || line[0] > 'z') {
            BOOST_LOG_TRIVIAL(fatal)
                << "session_manager:: invalid SDP at line " << num;
            return false;
          }
          break;
      }
    }
    if (dup && mid > 0) {
      /* DUP attribute and two audio media found */
      info.st20227_enabled = true;
    }
  } catch (...) {
    BOOST_LOG_TRIVIAL(fatal) << "session_manager:: invalid SDP at line " << num
                             << ", cannot perform number conversion";
    return false;
  }

  return true;
}

std::shared_ptr<SessionManager> SessionManager::create(
    std::shared_ptr<DriverManager> driver,
    std::shared_ptr<Browser> browser,
    std::shared_ptr<Config> config) {
  // no need to be thread-safe here
  static std::weak_ptr<SessionManager> instance;
  if (auto ptr = instance.lock()) {
    return ptr;
  }
  auto ptr = std::shared_ptr<SessionManager>(
      new SessionManager(driver, browser, config));
  /* W15: route the kernel's PCMRateApplied K2U event into the heap-owned queue
   * (captured by shared_ptr), so the driver's event-thread handler never touches
   * the SessionManager and is safe even if it fires during/after teardown. The
   * worker waits on this queue's cv and re-attaches the sink immediately. */
  driver->set_pcm_rate_applied_handler(
      [q = ptr->rerate_events_](uint8_t pcm_id, uint32_t rate) {
        {
          std::lock_guard<std::mutex> lk(q->mtx);
          q->applied.emplace_back(pcm_id, rate);
        }
        q->cv.notify_one();
      });
  instance = ptr;
  return ptr;
}

std::error_code SessionManager::get_source(uint8_t id,
                                           StreamSource& source) const {
  std::shared_lock sources_lock(sources_mutex_);
  auto const it = sources_.find(id);
  if (it == sources_.end()) {
    BOOST_LOG_TRIVIAL(error)
        << "session_manager:: source " << std::to_string(id) << " not in use";
    return DaemonErrc::stream_id_not_in_use;
  }
  const auto& info = (*it).second;
  source = get_source_(id, info);
  return std::error_code{};
}

std::error_code SessionManager::get_sink(uint8_t id, StreamSink& sink) const {
  std::shared_lock sinks_lock(sinks_mutex_);
  auto const it = sinks_.find(id);
  if (it == sinks_.end()) {
    BOOST_LOG_TRIVIAL(error)
        << "session_manager:: sink " << std::to_string(id) << " not in use";
    return DaemonErrc::stream_id_not_in_use;
  }
  const auto& info = (*it).second;
  sink = get_sink_(id, info);
  return std::error_code{};
}

std::list<StreamSink> SessionManager::get_sinks() const {
  std::shared_lock sinks_lock(sinks_mutex_);
  std::list<StreamSink> sinks_list;
  for (auto const& [id, info] : sinks_) {
    sinks_list.emplace_back(get_sink_(id, info));
  }
  return sinks_list;
}

std::list<StreamSource> SessionManager::get_sources() const {
  std::shared_lock sources_lock(sources_mutex_);
  std::list<StreamSource> sources_list;
  for (auto const& [id, info] : sources_) {
    sources_list.emplace_back(get_source_(id, info));
  }
  return sources_list;
}

StreamSource SessionManager::get_source_(uint8_t id,
                                         const StreamInfo& info) const {
  return {id,
          info.enabled,
          info.stream[0].m_cName,
          info.io,
          info.stream[0].m_ui32MaxSamplesPerPacket,
          info.stream[0].m_cCodec,
          ip::address_v4(info.stream[0].m_ui32DestIP).to_string(),
          info.stream[0].m_byTTL,
          info.stream[0].m_byPayloadType,
          info.stream[0].m_ucDSCP,
          info.refclk_ptp_traceable,
          {info.stream[0].m_aui32Routing,
           info.stream[0].m_aui32Routing + info.stream[0].m_byNbOfChannels},
          /* 2026-06-09 review fix: pcm was missing from this positional
           * init, silently defaulting to 0 — making the PCM binding
           * write-only (status.json round-trips and REST GETs collapsed
           * every stream to pcm 0). */
          static_cast<uint8_t>(info.stream[0].m_uiPCMId)};
}

StreamSink SessionManager::get_sink_(uint8_t id, const StreamInfo& info) const {
  return {id,
          info.stream[0].m_cName,
          info.io,
          info.sink_use_sdp,
          info.sink_source,
          info.sink_sdp,
          info.stream[0].m_ui32PlayOutDelay,
          info.ignore_refclk_gmid,
          {info.stream[0].m_aui32Routing,
           info.stream[0].m_aui32Routing + info.stream[0].m_byNbOfChannels},
          /* 2026-06-09 review fix: see get_source_ — without this, SAP
           * auto-update (get_updated_sinks -> add_sink) live-migrated
           * sinks to pcm 0 on any upstream SDP version bump. */
          static_cast<uint8_t>(info.stream[0].m_uiPCMId)};
}

int SessionManager::alloc_card_handle_() const {
  /* caller holds cards_mutex_ */
  for (uint8_t h = 0; h < card_handle_max; ++h) {
    if (cards_.find(h) == cards_.end()) {
      return h;
    }
  }
  return -1;
}

int SessionManager::alloc_pcm_id_() const {
  /* caller holds cards_mutex_. pcm_ids are global across all cards. */
  for (uint8_t p = 0; p < pcm_id_max; ++p) {
    if (pcms_.find(p) == pcms_.end()) {
      return p;
    }
  }
  return -1;
}

std::list<Pcm> SessionManager::pcms_of_card_(const std::string& card_name) const {
  /* caller holds cards_mutex_. pcms_ is keyed by pcm_id, so iteration is in
   * pcm_id order -- which is the add-order / ALSA device-index order. */
  std::list<Pcm> list;
  for (const auto& [pcm_id, pcm] : pcms_) {
    (void)pcm_id;
    if (pcm.card == card_name) {
      list.push_back(pcm);
    }
  }
  return list;
}

std::error_code SessionManager::bring_up_card_(const Card& card,
                                               const std::list<Pcm>& pcms) {
  /* caller holds cards_mutex_ (unique). Brings the card up with its full pcm set
   * using the handle/pcm_ids AS GIVEN (no allocation) -- serves runtime add
   * (after it allocates) and load-time rehydration/seeding (persisted ids).
   * Deferred-register: add_card -> add_pcm_to_card x N -> register_card. PCMs are
   * added in pcm_id order so the kernel's per-card device index lines up with
   * device_index_of(). Inserts card + pcms into the maps on success; unwinds the
   * kernel card on failure. */
  if (auto ec = driver_->add_card(card.handle, card.name, card.domain)) {
    BOOST_LOG_TRIVIAL(error)
        << "session_manager:: bring_up_card add_card failed: " << ec.message();
    return ec;
  }
  std::vector<Pcm> ordered(pcms.begin(), pcms.end());
  std::sort(ordered.begin(), ordered.end(),
            [](const Pcm& a, const Pcm& b) { return a.pcm_id < b.pcm_id; });
  for (const auto& pcm : ordered) {
    /* every PCM carries its own validated rate — no daemon-wide fallback. */
    if (auto ec = driver_->add_pcm_to_card(card.handle, pcm.pcm_id,
                                           pcm.sample_rate,
                                           pcm.num_inputs, pcm.num_outputs,
                                           pcm.name)) {
      BOOST_LOG_TRIVIAL(error)
          << "session_manager:: bring_up_card add_pcm_to_card (card=\""
          << card.name << "\" pcm=\"" << pcm.name
          << "\") failed: " << ec.message();
      (void)driver_->remove_card(card.handle);
      return ec;
    }
  }
  if (auto ec = driver_->register_card(card.handle)) {
    BOOST_LOG_TRIVIAL(error)
        << "session_manager:: bring_up_card register_card failed: "
        << ec.message();
    (void)driver_->remove_card(card.handle);
    return ec;
  }
  /* W9 #14: advisory ALSA latency, per-PCM (no daemon-wide default — each PCM
   * carries its own playout/capture delay, 0 = none). Real per-chip properties
   * in the kernel, read at prepare() into runtime->delay. (The real RTP
   * buffering depth is the per-sink link offset, not these.) */
  for (const auto& pcm : ordered) {
    if (auto ec = driver_->set_playout_delay(pcm.pcm_id, pcm.playout_delay)) {
      BOOST_LOG_TRIVIAL(warning)
          << "session_manager:: set_playout_delay(pcm_id " << (int)pcm.pcm_id
          << ") failed: " << ec.message();
    }
    if (auto ec = driver_->set_capture_delay(pcm.pcm_id, pcm.capture_delay)) {
      BOOST_LOG_TRIVIAL(warning)
          << "session_manager:: set_capture_delay(pcm_id " << (int)pcm.pcm_id
          << ") failed: " << ec.message();
    }
  }
  cards_[card.handle] = card;
  for (const auto& pcm : ordered) {
    pcms_[pcm.pcm_id] = pcm;
  }
  mark_status_dirty();  /* D7: card/pcm topology committed */
  return std::error_code{};
}

std::error_code SessionManager::add_card(const Card& spec) {
  Card card = spec;
  std::unique_lock cards_lock(cards_mutex_);
  /* the name is the card's durable identity (REST addresses cards by name, and
   * it is the kernel's hw:<name> ALSA id) -- require it non-empty and unique. */
  if (card.name.empty()) {
    BOOST_LOG_TRIVIAL(error) << "session_manager:: add_card: empty card name";
    return DaemonErrc::invalid_card_name;
  }
  /* W11: domains index the per-(NIC,domain) servo array directly (slot == domain
   * number), bounded by the kernel's MAX_DOMAINS (== MR_ALSA_MAX_CARDS ==
   * card_handle_max). Reject out-of-range here instead of letting the kernel
   * clamp it silently. */
  if (card.domain >= card_handle_max) {
    BOOST_LOG_TRIVIAL(error)
        << "session_manager:: add_card: domain " << (int)card.domain
        << " out of range [0," << (int)card_handle_max << ")";
    return DaemonErrc::invalid_card_domain;
  }
  if (!is_valid_rate_change_mode(card.rate_change_mode)) {
    BOOST_LOG_TRIVIAL(error)
        << "session_manager:: add_card: invalid rate_change_mode \""
        << card.rate_change_mode << "\"";
    return DaemonErrc::invalid_rate_change_mode;
  }
  for (const auto& [h, c] : cards_) {
    (void)h;
    if (c.name == card.name) {
      BOOST_LOG_TRIVIAL(error) << "session_manager:: add_card: name \""
                               << card.name << "\" already in use";
      return DaemonErrc::card_name_in_use;
    }
  }
  int handle = alloc_card_handle_();
  if (handle < 0) {
    BOOST_LOG_TRIVIAL(error)
        << "session_manager:: add_card: no free card handle (max "
        << (int)card_handle_max << ")";
    return DaemonErrc::card_slots_exhausted;
  }
  card.handle = static_cast<uint8_t>(handle);
  /* a card is created EMPTY -- PCMs are added via add_pcm (which recreates it
   * with the PCM attached). The kernel registers a zero-PCM card fine. */
  if (auto ec = bring_up_card_(card, {})) {
    return ec;
  }
  /* persistence is shutdown-only (matches add_source/add_sink; see remove_card).
   */
  BOOST_LOG_TRIVIAL(info) << "session_manager:: added card handle="
                          << (int)card.handle << " name=\"" << card.name
                          << "\" domain=" << (int)card.domain;
  return std::error_code{};
}

std::error_code SessionManager::remove_card(uint8_t handle) {
  std::string card_name;
  std::set<uint8_t> pcm_ids;
  {
    std::shared_lock cards_lock(cards_mutex_);
    auto it = cards_.find(handle);
    if (it == cards_.end()) {
      return DaemonErrc::invalid_card_handle;
    }
    card_name = it->second.name;
    for (const auto& [pid, pcm] : pcms_) {
      if (pcm.card == card_name) {
        pcm_ids.insert(pid);
      }
    }
  }
  /* cascade: drop the streams bound to ANY of this card's pcms before tearing it
   * down. get_sources()/get_sinks() return copies, so mutating via remove_*
   * while we iterate is safe; remove_* take their own source/sink locks. */
  for (const auto& source : get_sources()) {
    if (pcm_ids.count(source.pcm)) {
      remove_source(source.id);
    }
  }
  for (const auto& sink : get_sinks()) {
    if (pcm_ids.count(sink.pcm)) {
      remove_sink(sink.id);
    }
  }
  {
    std::unique_lock cards_lock(cards_mutex_);
    auto it = cards_.find(handle);
    if (it == cards_.end()) {
      return DaemonErrc::invalid_card_handle;
    }
    if (auto ec = driver_->remove_card(handle)) {
      BOOST_LOG_TRIVIAL(warning)
          << "session_manager:: remove_card driver remove failed: "
          << ec.message();
    }
    cards_.erase(it);
    for (auto pit = pcms_.begin(); pit != pcms_.end();) {
      if (pit->second.card == card_name) {
        pit = pcms_.erase(pit);
      } else {
        ++pit;
      }
    }
    mark_status_dirty();  /* D7: card removed */
  }
  /* persistence is captured at shutdown / by the REST handler (see add_card). */
  BOOST_LOG_TRIVIAL(info) << "session_manager:: removed card handle="
                          << (int)handle << " (\"" << card_name << "\")";
  return std::error_code{};
}

std::error_code SessionManager::recreate_card_(const std::string& card_name,
                                               const std::list<Pcm>& new_pcms,
                                               const std::string& new_name,
                                               int new_domain,
                                               const std::string& new_mode) {
  /* the generic recreate engine behind add_pcm/remove_pcm/update_pcm AND the
   * card-level rename/re-domain (update_card): rebuild `card_name` to hold
   * exactly `new_pcms` (each carrying its pcm_id -- survivors keep theirs, so
   * stream FKs stay valid), re-establishing the streams that were bound.
   * new_name (if non-empty) renames the card; new_domain (if >= 0) re-domains
   * it; both default to keeping the existing values. Best-effort (remove-then-
   * add forced by name-uniqueness): a failed rebuild leaves the card removed. */
  uint8_t old_handle;
  uint8_t domain;
  std::string mode;
  std::set<uint8_t> old_pcm_ids;
  {
    std::shared_lock cards_lock(cards_mutex_);
    const Card* card = nullptr;
    for (const auto& [h, c] : cards_) {
      (void)h;
      if (c.name == card_name) {
        card = &c;
        break;
      }
    }
    if (!card) {
      return DaemonErrc::invalid_card_name;
    }
    old_handle = card->handle;
    domain = card->domain;
    mode = card->rate_change_mode;
    for (const auto& [pid, pcm] : pcms_) {
      if (pcm.card == card_name) {
        old_pcm_ids.insert(pid);
      }
    }
  }
  const std::string target_name = new_name.empty() ? card_name : new_name;
  const uint8_t target_domain =
      new_domain >= 0 ? static_cast<uint8_t>(new_domain) : domain;
  const std::string target_mode = new_mode.empty() ? mode : new_mode;

  /* capture the streams currently bound to any of this card's pcms. */
  std::list<StreamSource> bound_sources;
  for (const auto& s : get_sources()) {
    if (old_pcm_ids.count(s.pcm)) {
      bound_sources.push_back(s);
    }
  }
  std::list<StreamSink> bound_sinks;
  for (const auto& s : get_sinks()) {
    if (old_pcm_ids.count(s.pcm)) {
      bound_sinks.push_back(s);
    }
  }

  /* tear the card down (cascade-removes its pcms + bound streams). */
  if (auto ec = remove_card(old_handle)) {
    return ec;
  }

  /* rebuild it with the new pcm set (same name + domain; reuse a handle). */
  {
    std::unique_lock cards_lock(cards_mutex_);
    int handle = alloc_card_handle_();
    if (handle < 0) {
      BOOST_LOG_TRIVIAL(error)
          << "session_manager:: recreate_card \"" << card_name
          << "\": no free card handle -- card and its streams are now removed";
      return DaemonErrc::card_slots_exhausted;
    }
    Card card;
    card.handle = static_cast<uint8_t>(handle);
    card.name = target_name;
    card.domain = target_domain;
    card.rate_change_mode = target_mode;
    std::list<Pcm> pcms = new_pcms;
    for (auto& p : pcms) {
      p.card = target_name;
    }
    if (auto ec = bring_up_card_(card, pcms)) {
      BOOST_LOG_TRIVIAL(error)
          << "session_manager:: recreate_card \"" << card_name
          << "\": rebuild failed (" << ec.message()
          << ") -- card and its streams are now removed";
      return ec;
    }
  }

  /* re-establish the captured streams whose pcm survived (pcm_id still present);
   * streams on a removed pcm are dropped, and a stream that no longer fits
   * (e.g. a sink whose SDP rate != its pcm's new rate) is dropped by the normal
   * validated path. Surviving pcm_ids are preserved, so no re-pointing. */
  std::set<uint8_t> new_pcm_ids;
  for (const auto& p : new_pcms) {
    new_pcm_ids.insert(p.pcm_id);
  }
  int reestablished = 0, dropped = 0;
  for (const auto& src : bound_sources) {
    if (!new_pcm_ids.count(src.pcm)) {
      ++dropped;
      continue;
    }
    if (auto ec = add_source(src)) {
      BOOST_LOG_TRIVIAL(warning)
          << "session_manager:: recreate_card \"" << card_name << "\": source "
          << (int)src.id << " not re-established: " << ec.message();
      ++dropped;
    } else {
      ++reestablished;
    }
  }
  for (const auto& snk : bound_sinks) {
    if (!new_pcm_ids.count(snk.pcm)) {
      ++dropped;
      continue;
    }
    if (auto ec = add_sink(snk)) {
      BOOST_LOG_TRIVIAL(warning)
          << "session_manager:: recreate_card \"" << card_name << "\": sink "
          << (int)snk.id << " not re-established: " << ec.message();
      ++dropped;
    } else {
      ++reestablished;
    }
  }
  BOOST_LOG_TRIVIAL(info) << "session_manager:: recreate_card \"" << card_name
                          << "\": " << reestablished
                          << " stream(s) re-established, " << dropped
                          << " dropped";
  return std::error_code{};
}

std::error_code SessionManager::add_pcm(const std::string& card_name,
                                        const Pcm& spec) {
  Pcm pcm = spec;
  pcm.card = card_name;
  std::list<Pcm> new_pcms;
  {
    std::unique_lock cards_lock(cards_mutex_);
    bool card_exists = false;
    for (const auto& [h, c] : cards_) {
      (void)h;
      if (c.name == card_name) {
        card_exists = true;
        break;
      }
    }
    if (!card_exists) {
      return DaemonErrc::invalid_card_name;
    }
    if (pcm.name.empty()) {
      BOOST_LOG_TRIVIAL(error) << "session_manager:: add_pcm: empty pcm name";
      return DaemonErrc::invalid_pcm_name;
    }
    /* rate is required per-PCM — there is no daemon-wide default to inherit. */
    if (!is_valid_pcm_rate(pcm.sample_rate)) {
      BOOST_LOG_TRIVIAL(error)
          << "session_manager:: add_pcm: pcm \"" << pcm.name
          << "\" has missing/invalid sample_rate " << pcm.sample_rate;
      return DaemonErrc::invalid_sample_rate;
    }
    for (const auto& [pid, p] : pcms_) {
      (void)pid;
      if (p.card == card_name && p.name == pcm.name) {
        BOOST_LOG_TRIVIAL(error)
            << "session_manager:: add_pcm: pcm \"" << pcm.name
            << "\" already exists on card \"" << card_name << "\"";
        return DaemonErrc::pcm_name_in_use;
      }
    }
    int pid = alloc_pcm_id_();
    if (pid < 0) {
      BOOST_LOG_TRIVIAL(error) << "session_manager:: add_pcm: no free pcm_id (max "
                               << (int)pcm_id_max << ")";
      return DaemonErrc::card_slots_exhausted;
    }
    pcm.pcm_id = static_cast<uint8_t>(pid);
    new_pcms = pcms_of_card_(card_name);
    new_pcms.push_back(pcm);
  }
  BOOST_LOG_TRIVIAL(info) << "session_manager:: add_pcm \"" << pcm.name
                          << "\" (pcm_id " << (int)pcm.pcm_id << ") to card \""
                          << card_name << "\"";
  return recreate_card_(card_name, new_pcms);
}

std::error_code SessionManager::remove_pcm(const std::string& card_name,
                                           const std::string& pcm_name) {
  std::list<Pcm> new_pcms;
  bool found = false;
  {
    std::shared_lock cards_lock(cards_mutex_);
    for (const auto& [pid, p] : pcms_) {
      (void)pid;
      if (p.card != card_name) {
        continue;
      }
      if (p.name == pcm_name) {
        found = true;
      } else {
        new_pcms.push_back(p);
      }
    }
  }
  if (!found) {
    return DaemonErrc::invalid_pcm_name;
  }
  BOOST_LOG_TRIVIAL(info) << "session_manager:: remove_pcm \"" << pcm_name
                          << "\" from card \"" << card_name << "\"";
  return recreate_card_(card_name, new_pcms);
}

std::error_code SessionManager::update_pcm(const std::string& card_name,
                                           const std::string& pcm_name,
                                           const Pcm& new_params) {
  std::list<Pcm> new_pcms;
  bool found = false;
  /* new_params.name (if non-empty) renames the pcm; empty keeps it. The rename
   * is benign vs a card rename -- pcm_id (hence hw:<card>,<dev> and stream FKs)
   * is preserved; only the management label + the snd_pcm description change. */
  const std::string new_name =
      new_params.name.empty() ? pcm_name : new_params.name;
  {
    std::shared_lock cards_lock(cards_mutex_);
    if (new_name != pcm_name) {
      for (const auto& [pid, p] : pcms_) {
        (void)pid;
        if (p.card == card_name && p.name == new_name) {
          BOOST_LOG_TRIVIAL(error)
              << "session_manager:: update_pcm: pcm \"" << new_name
              << "\" already exists on card \"" << card_name << "\"";
          return DaemonErrc::pcm_name_in_use;
        }
      }
    }
    for (const auto& [pid, p] : pcms_) {
      (void)pid;
      if (p.card != card_name) {
        continue;
      }
      if (p.name == pcm_name) {
        found = true;
        Pcm updated = p;  // keep pcm_id, card
        updated.name = new_name;
        updated.sample_rate = new_params.sample_rate;
        updated.num_inputs = new_params.num_inputs;
        updated.num_outputs = new_params.num_outputs;
        updated.playout_delay = new_params.playout_delay;
        updated.capture_delay = new_params.capture_delay;
        updated.rate_follows_source = new_params.rate_follows_source;
        new_pcms.push_back(updated);
      } else {
        new_pcms.push_back(p);
      }
    }
  }
  if (!found) {
    return DaemonErrc::invalid_pcm_name;
  }
  BOOST_LOG_TRIVIAL(info) << "session_manager:: update_pcm \"" << pcm_name
                          << "\" on card \"" << card_name << "\" -> name=\""
                          << new_name << "\"";
  return recreate_card_(card_name, new_pcms);
}

std::error_code SessionManager::update_card(const std::string& name,
                                            const std::string& new_name,
                                            uint8_t new_domain,
                                            const std::string& new_mode) {
  /* card-level edit: rename, re-domain, and/or change the rate-change mode.
   * Recreates the card under the new identity, keeping its pcm set (pcm_ids
   * preserved) and re-establishing bound streams. NB: renaming changes the
   * hw:<name> ALSA id (clients referencing the old name must update),
   * re-domaining moves the card to another PTP domain (W11), and the rate-change
   * mode picks recreate vs in-place re-rate (W15) -- all surfaced in the WebUI. */
  if (new_domain >= card_handle_max) {  // card_handle_max == kernel MAX_DOMAINS
    BOOST_LOG_TRIVIAL(error)
        << "session_manager:: update_card: domain " << (int)new_domain
        << " out of range [0," << (int)card_handle_max << ")";
    return DaemonErrc::invalid_card_domain;
  }
  if (!is_valid_rate_change_mode(new_mode)) {
    BOOST_LOG_TRIVIAL(error)
        << "session_manager:: update_card: invalid rate_change_mode \""
        << new_mode << "\"";
    return DaemonErrc::invalid_rate_change_mode;
  }
  std::list<Pcm> pcms;
  {
    std::shared_lock cards_lock(cards_mutex_);
    bool exists = false;
    for (const auto& [h, c] : cards_) {
      (void)h;
      if (c.name == name) {
        exists = true;
        break;
      }
    }
    if (!exists) {
      return DaemonErrc::invalid_card_name;
    }
    if (new_name.empty()) {
      BOOST_LOG_TRIVIAL(error)
          << "session_manager:: update_card: empty new name";
      return DaemonErrc::invalid_card_name;
    }
    if (new_name != name) {
      for (const auto& [h, c] : cards_) {
        (void)h;
        if (c.name == new_name) {
          BOOST_LOG_TRIVIAL(error) << "session_manager:: update_card: name \""
                                   << new_name << "\" already in use";
          return DaemonErrc::card_name_in_use;
        }
      }
    }
    pcms = pcms_of_card_(name);
  }
  BOOST_LOG_TRIVIAL(info) << "session_manager:: update_card \"" << name
                          << "\" -> name=\"" << new_name << "\" domain="
                          << (int)new_domain << " mode=\"" << new_mode << "\"";
  return recreate_card_(name, pcms, new_name, static_cast<int>(new_domain),
                        new_mode);
}

std::list<Card> SessionManager::get_cards() const {
  std::shared_lock cards_lock(cards_mutex_);
  std::list<Card> list;
  for (const auto& [handle, card] : cards_) {
    (void)handle;
    list.push_back(card);
  }
  return list;
}

std::error_code SessionManager::get_card(uint8_t handle, Card& card) const {
  std::shared_lock cards_lock(cards_mutex_);
  auto it = cards_.find(handle);
  if (it == cards_.end()) {
    return DaemonErrc::invalid_card_handle;
  }
  card = it->second;
  return std::error_code{};
}

std::error_code SessionManager::get_card_by_name(const std::string& name,
                                                 Card& card) const {
  std::shared_lock cards_lock(cards_mutex_);
  for (const auto& [handle, c] : cards_) {
    (void)handle;
    if (c.name == name) {
      card = c;
      return std::error_code{};
    }
  }
  return DaemonErrc::invalid_card_name;
}

std::list<Pcm> SessionManager::get_pcms() const {
  std::shared_lock cards_lock(cards_mutex_);
  std::list<Pcm> list;
  for (const auto& [pid, pcm] : pcms_) {
    (void)pid;
    list.push_back(pcm);
  }
  return list;
}

std::error_code SessionManager::get_pcm_by_name(const std::string& card_name,
                                                const std::string& pcm_name,
                                                Pcm& pcm) const {
  std::shared_lock cards_lock(cards_mutex_);
  for (const auto& [pid, p] : pcms_) {
    (void)pid;
    if (p.card == card_name && p.name == pcm_name) {
      pcm = p;
      return std::error_code{};
    }
  }
  return DaemonErrc::invalid_pcm_name;
}

int SessionManager::device_index_of(uint8_t pcm_id) const {
  std::shared_lock cards_lock(cards_mutex_);
  auto it = pcms_.find(pcm_id);
  if (it == pcms_.end()) {
    return -1;
  }
  const std::string card_name = it->second.card;
  int idx = 0;
  /* pcms_ iterates in pcm_id order, matching bring_up_card_'s add order. */
  for (const auto& [pid, p] : pcms_) {
    if (p.card != card_name) {
      continue;
    }
    if (pid == pcm_id) {
      return idx;
    }
    ++idx;
  }
  return -1;
}

bool SessionManager::pcm_for_id_(uint8_t pcm_id, Pcm& out) const {
  std::shared_lock cards_lock(cards_mutex_);
  auto it = pcms_.find(pcm_id);
  if (it == pcms_.end()) {
    return false;
  }
  out = it->second;
  return true;
}

bool SessionManager::pcm_declared_(uint8_t pcm_id) const {
  Pcm pcm;
  return pcm_for_id_(pcm_id, pcm);
}

uint32_t SessionManager::rate_for_pcm_(uint8_t pcm_id) const {
  /* W28: the kernel is authoritative for the LIVE rate. Read the lock-free
   * mirror (GetPCMStatus poll + PCMRateApplied event) so SDP generation, the
   * sink-fit check and the rate-follow decision all see kernel truth rather
   * than a cached/commanded value that could have drifted. */
  if (pcm_id < pcm_id_max) {
    uint32_t live = pcm_live_rate_[pcm_id].load(std::memory_order_acquire);
    if (live)
      return live;
  }
  /* Not yet mirrored (startup window before the first poll, or the fake driver):
   * fall back to the configured/intent rate the PCM was created with. */
  Pcm pcm;
  if (pcm_for_id_(pcm_id, pcm)) {
    return pcm.sample_rate;
  }
  /* unknown pcm — validation rejects streams bound to undeclared PCMs, so this
   * is the can't-happen path; there is no daemon-wide default to return. */
  return 0;
}

uint8_t SessionManager::domain_for_pcm_(uint8_t pcm_id) const {
  Pcm pcm;
  Card card;
  if (pcm_for_id_(pcm_id, pcm) && !get_card_by_name(pcm.card, card)) {
    return card.domain;
  }
  /* unknown pcm/card (add-time validation rejects those) — fall back to the
   * daemon-wide configured PTP domain, mirroring rate_for_pcm_. */
  return config_->get_ptp_domain();
}

bool SessionManager::load_status() {
  std::list<Card> cards_list;
  std::list<Pcm> pcms_list;
  std::list<StreamSource> sources_list;
  std::list<StreamSink> sinks_list;

  /* Read persisted state if a status file is configured and present. A missing
   * file is normal on first boot (cards are seeded from config below).
   * D7 (2026-06 audit): a present-but-unparseable file used to be silently
   * ignored — the daemon ran with an empty topology and the next clean save
   * OVERWROTE the only copy, making the loss permanent. QUARANTINE the bad
   * file instead (rename to <path>.corrupt): the evidence survives for hand
   * recovery, the daemon comes up empty and says so loudly. (With the atomic
   * tmp+rename writer we should never produce such a file ourselves.) */
  if (!config_->get_status_file().empty()) {
    std::ifstream jsonstream(config_->get_status_file());
    if (jsonstream) {
      try {
        json_to_status(jsonstream, cards_list, pcms_list, sources_list,
                       sinks_list);
      } catch (const std::runtime_error& e) {
        const auto path = config_->get_status_file();
        const auto quarantine_path = path + ".corrupt";
        jsonstream.close();
        if (std::rename(path.c_str(), quarantine_path.c_str()) == 0) {
          BOOST_LOG_TRIVIAL(fatal)
              << "session_manager:: cannot parse status file (" << e.what()
              << "); quarantined it as " << quarantine_path
              << " and continuing with an EMPTY topology — recover by fixing "
                 "and restoring that file";
        } else {
          BOOST_LOG_TRIVIAL(fatal)
              << "session_manager:: cannot parse status file (" << e.what()
              << ") and could not quarantine it; continuing with an EMPTY "
                 "topology — the file will be OVERWRITTEN by the next save";
        }
        return false;
      }
    } else {
      BOOST_LOG_TRIVIAL(info)
          << "session_manager:: no status file yet, seeding topology from "
             "config";
    }
  }

  /* W10.2: the SessionManager owns the card+pcm topology, brought up from
   * status.json. Cards must come up BEFORE sources/sinks, which reference their
   * pcms by pcm_id. A card with no pcms is brought up empty. First boot (no
   * status.json) simply has no cards — create them at runtime via REST/WebUI;
   * there is no daemon.conf seeding anymore. */
  {
    std::unique_lock cards_lock(cards_mutex_);
    for (const auto& card : cards_list) {
      std::list<Pcm> card_pcms;
      for (const auto& pcm : pcms_list) {
        if (pcm.card == card.name) {
          card_pcms.push_back(pcm);
        }
      }
      if (auto ec = bring_up_card_(card, card_pcms)) {
        BOOST_LOG_TRIVIAL(error)
            << "session_manager:: load_status: card \"" << card.name
            << "\" bring-up failed: " << ec.message();
      }
    }
  }

  /* W10.2 defensive (load): clamp any persisted stream whose channel map no
   * longer fits its pcm (e.g. a 6-ch source auto-assigned to a 4-out pcm across
   * the model transition) so it is KEPT + fixed rather than rejected by
   * add_source/add_sink's range guard. The REST path stays strict (reject); this
   * is the gentle-on-stale-data load path. */
  auto clamp_map = [](std::vector<uint8_t>& map, uint32_t limit) {
    std::vector<uint8_t> kept;
    for (uint8_t ch : map) {
      if (ch < limit) {
        kept.push_back(ch);
      }
    }
    bool changed = kept.size() != map.size();
    map = std::move(kept);
    return changed;
  };
  for (auto& source : sources_list) {
    Pcm pcm;
    if (pcm_for_id_(source.pcm, pcm)) {
      size_t before = source.map.size();
      if (clamp_map(source.map, pcm.num_outputs)) {
        BOOST_LOG_TRIVIAL(warning)
            << "session_manager:: load: clamped source " << (int)source.id
            << " to pcm \"" << pcm.name << "\" outputs (" << before << " -> "
            << source.map.size() << " channels)";
      }
    }
  }
  for (auto& sink : sinks_list) {
    Pcm pcm;
    if (pcm_for_id_(sink.pcm, pcm)) {
      size_t before = sink.map.size();
      if (clamp_map(sink.map, pcm.num_inputs)) {
        BOOST_LOG_TRIVIAL(warning)
            << "session_manager:: load: clamped sink " << (int)sink.id
            << " to pcm \"" << pcm.name << "\" inputs (" << before << " -> "
            << sink.map.size() << " channels)";
      }
    }
  }

  for (auto const& source : sources_list) {
    add_source(source);
  }
  for (auto const& sink : sinks_list) {
    add_sink(sink);
  }

  return true;
}

bool SessionManager::save_status() const {
  if (config_->get_status_file().empty()) {
    return true;
  }

  /* D7 (2026-06 audit): atomic write — the direct overwrite meant a crash
   * mid-save TRUNCATED status.json, and the next boot then ran (and later
   * persisted) an empty topology. Write the whole document to a sibling tmp
   * file and rename() it into place: readers and crashes see either the old
   * complete file or the new complete file, never a torn one. */
  const auto path = config_->get_status_file();
  const auto tmp_path = path + ".tmp";
  {
    std::ofstream jsonstream(tmp_path, std::ios::trunc);
    if (!jsonstream) {
      BOOST_LOG_TRIVIAL(fatal)
          << "session_manager:: cannot write status tmp file " << tmp_path;
      return false;
    }
    jsonstream << status_to_json(get_cards(), get_pcms(), get_sources(),
                                 get_sinks());
    jsonstream.close();
    if (!jsonstream) {
      BOOST_LOG_TRIVIAL(fatal)
          << "session_manager:: write to status tmp file failed " << tmp_path;
      (void)std::remove(tmp_path.c_str());
      return false;
    }
  }
  if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
    BOOST_LOG_TRIVIAL(fatal) << "session_manager:: cannot move status tmp file "
                             << tmp_path << " into place";
    (void)std::remove(tmp_path.c_str());
    return false;
  }
  BOOST_LOG_TRIVIAL(info) << "session_manager:: status file saved";

  return true;
}

uint8_t SessionManager::get_source_id(const std::string& name) const {
  /* D3 (2026-06 audit): called from the RTSP io thread on every DESCRIBE,
   * racing writers (on_add/remove_source under sources_mutex_ unique) — an
   * unsynchronized std::map read vs a rebalancing insert/erase is UB. */
  std::shared_lock sources_lock(sources_mutex_);
  const auto it = source_names_.find(name);
  return it != source_names_.end() ? it->second : (stream_id_max + 1);
}

void SessionManager::add_ptp_status_observer(const PtpStatusObserver& cb) {
  ptp_status_observers_.push_back(cb);
}

void SessionManager::add_source_observer(SourceObserverType type,
                                         const SourceObserver& cb) {
  switch (type) {
    case SourceObserverType::add_source:
      add_source_observers_.push_back(cb);
      break;
    case SourceObserverType::remove_source:
      remove_source_observers_.push_back(cb);
      break;
    case SourceObserverType::update_source:
      update_source_observers_.push_back(cb);
      break;
  }
}

void SessionManager::add_sink_observer(SinkObserverType type,
                                       const SinkObserver& cb) {
  switch (type) {
    case SinkObserverType::add_sink:
      add_sink_observers_.push_back(cb);
      break;
    case SinkObserverType::remove_sink:
      remove_sink_observers_.push_back(cb);
      break;
  }
}

void SessionManager::on_add_source(const StreamSource& source,
                                   const StreamInfo& info) {
  for (const auto& cb : add_source_observers_) {
    cb(source.id, source.name, get_source_sdp_(source.id, info));
  }
  if (IN_MULTICAST(info.stream[0].m_ui32DestIP)) {
    igmp_[0].join(config_->get_ip_addr_str(),
                  ip::address_v4(info.stream[0].m_ui32DestIP).to_string());
    if (info.st20227_enabled) {
      auto [ip_addr, ip_str] = get_interface_ip(config_->get_interface_name(1));
      igmp_[1].join(ip_str,
                    ip::address_v4(info.stream[1].m_ui32DestIP).to_string());
    }
  }
  source_names_[source.name] = source.id;
}

void SessionManager::on_remove_source(const StreamInfo& info) {
  for (const auto& cb : remove_source_observers_) {
    cb((uint8_t)info.stream[0].m_uiId, info.stream[0].m_cName, {});
  }
  if (IN_MULTICAST(info.stream[0].m_ui32DestIP)) {
    igmp_[0].leave(config_->get_ip_addr_str(),
                   ip::address_v4(info.stream[0].m_ui32DestIP).to_string());
    if (info.st20227_enabled) {
      auto [ip_addr, ip_str] = get_interface_ip(config_->get_interface_name(1));
      igmp_[1].leave(ip_str,
                     ip::address_v4(info.stream[1].m_ui32DestIP).to_string());
    }
  }
  source_names_.erase(info.stream[0].m_cName);
}

std::error_code SessionManager::add_source(const StreamSource& source) {
  if (source.id > stream_id_max) {
    BOOST_LOG_TRIVIAL(error) << "session_manager:: source id "
                             << std::to_string(source.id) << " is not valid";
    return DaemonErrc::invalid_stream_id;
  }

  /* 2026-06-09 review hardening: a source bound to an undeclared PCM binds
   * to an empty kernel chip slot — silently dead stream + leaked kernel
   * stream handler slot. Fail loud here instead. (No overlap check for
   * sources: multiple sources READING the same playback channel is
   * harmless, unlike sinks writing.) W10.2: validate against the runtime card
   * set (cards_), not daemon.conf device_groups — a REST-added card is a valid
   * target too, and a config-only group that has no live card is not. */
  if (!pcm_declared_(source.pcm)) {
    BOOST_LOG_TRIVIAL(error)
        << "session_manager:: source " << std::to_string(source.id)
        << " references PCM " << std::to_string(source.pcm)
        << " with no live card";
    return DaemonErrc::invalid_pcm;
  }
  if (source.map.size() > MAX_CHANNELS_BY_RTP_STREAM) {
    BOOST_LOG_TRIVIAL(error)
        << "session_manager:: source " << std::to_string(source.id)
        << " channel map has " << source.map.size() << " entries (max "
        << MAX_CHANNELS_BY_RTP_STREAM << ")";
    return DaemonErrc::invalid_channel_map;
  }
  /* W10.2 defensive: every map index must fit the bound pcm. A source reads the
   * card's ALSA OUTPUT channels, so each index must be < num_outputs. Catches a
   * stale bind (e.g. a 6-ch source auto-assigned to a 4-out pcm) -- rejected at
   * the REST boundary and dropped+logged on load. */
  {
    Pcm pcm;
    if (pcm_for_id_(source.pcm, pcm)) {
      for (uint8_t ch : source.map) {
        if (ch >= pcm.num_outputs) {
          BOOST_LOG_TRIVIAL(error)
              << "session_manager:: source " << std::to_string(source.id)
              << " channel " << (int)ch << " exceeds pcm \"" << pcm.name
              << "\" output count (" << pcm.num_outputs << ")";
          return DaemonErrc::invalid_channel_map;
        }
      }
    }
  }

  StreamInfo info;
  memset(&info.stream[0], 0, sizeof info.stream[0]);
  info.stream[0].m_bSource = 1;  // source
  info.stream[0].m_ui32CRTP_stream_info_sizeof = sizeof(info.stream[0]);
  strncpy(info.stream[0].m_cName, source.name.c_str(),
          sizeof(info.stream[0].m_cName) - 1);
  info.stream[0].m_ucDSCP = source.dscp;  // IPv4 DSCP
  info.stream[0].m_uiIfPortId = 0;
  info.stream[0].m_byPayloadType = source.payload_type;
  info.stream[0].m_byWordLength = get_codec_word_length(source.codec);
  info.stream[0].m_byNbOfChannels = source.map.size();
  strncpy(info.stream[0].m_cCodec, source.codec.c_str(),
          sizeof(info.stream[0].m_cCodec) - 1);
  info.stream[0].m_ui32MaxSamplesPerPacket = source.max_samples_per_packet;
  /* W7 (Decision 10): a source advertises ITS pcm's rate, not the manager-wide
   * rate — so a source on the 44.1k card emits L24/44100 regardless of what
   * other cards run. W10.2: resolved from the owning card (cards_), not
   * device_groups. */
  info.stream[0].m_ui32SamplingRate = rate_for_pcm_(source.pcm);
  info.stream[0].m_uiId = source.id;
  info.stream[0].m_uiPCMId = source.pcm;  // multi-rate Stage 1
  info.stream[0].m_ui32RTCPSrcIP = config_->get_ip_addr();
  info.stream[0].m_ui32SrcIP = config_->get_ip_addr();  // only for Source
  bool use_source_address = false;
  boost::system::error_code ec;
#if BOOST_VERSION < 108700
  ip::address_v4::from_string(source.address, ec);
#else
  ip::make_address(source.address, ec);
#endif
  if (!ec) {
    info.stream[0].m_ui32DestIP =
#if BOOST_VERSION < 108700
        ip::address_v4::from_string(source.address).to_ulong();
#else
        ip::make_address(source.address).to_v4().to_uint();
#endif
    use_source_address = true;
  } else {
    info.stream[0].m_ui32DestIP =
#if BOOST_VERSION < 108700
        ip::address_v4::from_string(config_->get_rtp_mcast_base().c_str())
            .to_ulong() +
#else
        ip::make_address(config_->get_rtp_mcast_base().c_str())
            .to_v4()
            .to_uint() +
#endif
        source.id;
  }
  info.stream[0].m_usSrcPort = config_->get_rtp_port();
  info.stream[0].m_usDestPort = config_->get_rtp_port();
  info.stream[0].m_ui32SSRC = rand() % 65536;  // use random number
  std::copy(source.map.begin(), source.map.end(),
            info.stream[0].m_aui32Routing);

  if (IN_MULTICAST(info.stream[0].m_ui32DestIP)) {
    auto mac_addr = get_mcast_mac_addr(info.stream[0].m_ui32DestIP);
    std::copy(std::begin(mac_addr), std::end(mac_addr),
              info.stream[0].m_ui8DestMAC);
    info.stream[0].m_byTTL = source.ttl;
  } else {
    auto mac_addr = get_mac_from_arp_cache(
        ip::address_v4(info.stream[0].m_ui32DestIP).to_string());
    int retry = 3;
    while (!mac_addr.second.length() && retry > 0) {
      // if not in cache already try to populate the MAC cache
      (void)echo_try_connect(
          ip::address_v4(info.stream[0].m_ui32DestIP).to_string());
      mac_addr = get_mac_from_arp_cache(
          ip::address_v4(info.stream[0].m_ui32DestIP).to_string());
      retry--;
    }
    if (!mac_addr.second.length()) {
      BOOST_LOG_TRIVIAL(error)
          << "session_manager:: cannot retrieve MAC address for IP "
          << ip::address_v4(info.stream[0].m_ui32DestIP).to_string()
          << " on interface " << config_->get_interface_name(0);
      return DaemonErrc::cannot_retrieve_mac;
    }
    std::copy(std::begin(mac_addr.first), std::end(mac_addr.first),
              info.stream[0].m_ui8DestMAC);
    info.stream[0].m_byTTL = 64;
  }

  info.refclk_ptp_traceable = source.refclk_ptp_traceable;
  info.enabled = source.enabled;
  info.io = source.io;

  auto ip_addr = htonl(config_->get_ip_addr());
  info.session_id = (ip_addr << 16) + (ip_addr >> 16) + source.id;
  info.session_version = info.session_id + g_session_version++;
  // info.m_ui32PlayOutDelay = 0; // only for Sink

  std::unique_lock sources_lock(sources_mutex_);
  auto const it = sources_.find(source.id);
  if (it != sources_.end()) {
    BOOST_LOG_TRIVIAL(info)
        << "session_manager:: source id " << std::to_string(source.id)
        << " is in use, updating";
    // remove previous stream if enabled
    if ((*it).second.enabled) {
      (void)driver_->remove_rtp_stream((*it).second.handle[0]);
      if ((*it).second.st20227_enabled) {
        (void)driver_->remove_rtp_stream((*it).second.handle[1]);
      }
      on_remove_source((*it).second);
    }
  } else if (source_names_.find(source.name) != source_names_.end()) {
    BOOST_LOG_TRIVIAL(error)
        << "session_manager:: source name " << source.name << " is in use";
    return DaemonErrc::stream_name_in_use;
  }

  std::error_code ret;
  if (info.enabled) {
    ret = driver_->add_rtp_stream(source.pcm, info.stream[0], info.handle[0]);
    if (ret) {
      if (it != sources_.end()) {
        /* update operation failed */
        sources_.erase(source.id);
      }
      return ret;
    }

    info.st20227_enabled = false;
    if (config_->get_interface_name(1).length() > 0) {
      auto [ip_addr, ip_str] = get_interface_ip(config_->get_interface_name(1));
      if (!ip_str.empty()) {
        memcpy(&info.stream[1], &info.stream[0], sizeof(info.stream[0]));
        if (!use_source_address) {
          info.stream[1].m_ui32DestIP =
  #if BOOST_VERSION < 108700
              ip::address_v4::from_string(
                  config_->get_rtp_mcast_base_sec().c_str())
                  .to_ulong() +
  #else
              ip::make_address(config_->get_rtp_mcast_base_sec().c_str())
                  .to_v4()
                  .to_uint() +
  #endif
              source.id;
        }
        info.stream[1].m_ui32RTCPSrcIP = ip_addr;
        info.stream[1].m_ui32SrcIP = ip_addr;  // only for Source
        info.stream[1].m_uiIfPortId = 1;
        info.stream[1].m_usSrcPort = config_->get_rtp_port_sec();
        info.stream[1].m_usDestPort = config_->get_rtp_port_sec();
        /* multi-rate Stage 1: ST2022-7 NIC redundancy is orthogonal to
         * PCM binding — both copies of the stream belong to the same PCM. */
        info.stream[1].m_uiPCMId = source.pcm;

        if (!IN_MULTICAST(info.stream[1].m_ui32DestIP)) {
          /* reuse the MAC address found on interface 0 */
          info.stream[1].m_byTTL = 64;
        }
        ret = driver_->add_rtp_stream(source.pcm, info.stream[1], info.handle[1]);
        if (ret) {
          /* D1 (2026-06 audit): roll back the stream just created for THIS
           * add — info.handle[0] — not (*it).second.handle[0], which is the
           * PREVIOUS source's already-removed handle (double-remove) and an
           * end() deref (UB) on a fresh add. */
          (void)driver_->remove_rtp_stream(info.handle[0]);
          if (it != sources_.end()) {
            /* update operation failed */
            sources_.erase(source.id);
          }
          return ret;
        }
        info.st20227_enabled = true;
      }
    }
    on_add_source(source, info);
  }

  // update source map
  sources_[source.id] = info;
  mark_status_dirty();  /* D7: source committed */
  BOOST_LOG_TRIVIAL(info) << "session_manager:: added source "
                          << std::to_string(source.id) << " " << info.handle[0]
                          << "," << info.handle[1];
  return ret;
}

std::string SessionManager::get_removed_source_sdp_(
    uint32_t id,
    uint32_t src_addr,
    uint32_t session_id,
    uint32_t session_version) const {
  std::string sdp("o=- " + std::to_string(session_id) + " " +
                  std::to_string(session_version) + " IN IP4 " +
                  ip::address_v4(src_addr).to_string() + "\n");
  return sdp;
}

std::string SessionManager::get_source_sdp_(uint32_t id,
                                            const StreamInfo& info) const {
  std::shared_lock ptp_lock(ptp_mutex_);
  /* W7 (Decision 10): the SDP rtpmap must advertise this source's pcm rate
   * (matches m_ui32SamplingRate stamped in add_source), not the manager-wide
   * rate. ptime below derives from it. W10.2: resolved from the owning card. */
  uint32_t sample_rate = rate_for_pcm_(info.stream[0].m_uiPCMId);
  /* W11 slice 1: advertise THIS source's PTP domain (its owning card's), not the
   * single daemon-wide one — so a card in domain N announces clock-domain N.
   * Domain-0 cards are byte-identical to before. The GMID below is still the
   * single global ptp_status_.gmid; per-domain GMID lands in slice 2 once the
   * kernel runs a servo per domain. (ST-2022-7 stream[1] shares this pcm.) */
  uint8_t domain = domain_for_pcm_(info.stream[0].m_uiPCMId);
  /* W11: the GMID of THIS source's domain (mirrored per-domain by the worker);
   * falls back to the global/domain-0 status if that domain isn't mirrored yet.
   * ptp_mutex_ is held shared above. */
  std::string refclk_gmid = ptp_status_.gmid;
  {
    auto it = ptp_status_by_domain_.find(domain);
    if (it != ptp_status_by_domain_.end() && !it->second.gmid.empty()) {
      refclk_gmid = it->second.gmid;
    }
  }
  auto [ip_addr, sec_ip_str] = get_interface_ip(config_->get_interface_name(1));
  bool dup_entry =
      info.st20227_enabled && !sec_ip_str.empty()/* &&
      (info.stream[0].m_ui32DestIP != info.stream[1].m_ui32DestIP ||
       info.stream[0].m_usDestPort != info.stream[1].m_usDestPort)*/;

  // need a 12 digit precision for ptime
  std::ostringstream ss_ptime;
  ss_ptime.precision(12);
  ss_ptime << std::fixed
           << static_cast<double>(info.stream[0].m_ui32MaxSamplesPerPacket) *
                  1000 / static_cast<double>(sample_rate);
  std::string ptime = ss_ptime.str();
  // remove trailing zeros or dot
  ptime.erase(ptime.find_last_not_of("0.") + 1, std::string::npos);

  // build SDP
  std::stringstream ss;
  ss << "v=0\n"
     << "o=- " << info.session_id << " " << info.session_version << " IN IP4 "
     << ip::address_v4(info.stream[0].m_ui32SrcIP).to_string() << "\n"
     << "s=" << config_->get_node_id() << " " << info.stream[0].m_cName << "\n";
    ss << "t=0 0\n";
  if (dup_entry) {
    ss << "a=group:DUP 1 2\n";
  }
  ss << "m=audio " << info.stream[0].m_usDestPort << " RTP/AVP "
     << static_cast<unsigned>(info.stream[0].m_byPayloadType) << "\n"
     << "c=IN IP4 " << ip::address_v4(info.stream[0].m_ui32DestIP).to_string();
  if (IN_MULTICAST(info.stream[0].m_ui32DestIP)) {
    ss << "/" << static_cast<unsigned>(info.stream[0].m_byTTL);
  }
  ss << "\na=source-filter: incl IN IP4 "
     << ip::address_v4(info.stream[0].m_ui32DestIP).to_string() << " "
     << config_->get_ip_addr_str();
  ss << "\na=rtpmap:" << static_cast<unsigned>(info.stream[0].m_byPayloadType)
     << " " << info.stream[0].m_cCodec << "/" << sample_rate << "/"
     << static_cast<unsigned>(info.stream[0].m_byNbOfChannels) << "\n"
     << "a=sync-time:0\n"
     << "a=framecount:" << info.stream[0].m_ui32MaxSamplesPerPacket << "\n"
     << "a=ptime:" << ptime << "\n"
     << "a=mediaclk:direct=0\n";
  ss << "a=clock-domain:PTPv2 " << static_cast<unsigned>(domain)
     << "\na=ts-refclk:ptp=IEEE1588-2008:";
  if (info.refclk_ptp_traceable) {
    ss << "traceable\n";
  } else {
    ss << refclk_gmid << ":" << static_cast<unsigned>(domain)
       << "\n";
  }
  ss << "a=recvonly\n";
  if (dup_entry) {
    ss << "a=mid:1\n"
       << "m=audio " << info.stream[1].m_usDestPort << " RTP/AVP "
       << static_cast<unsigned>(info.stream[1].m_byPayloadType) << "\n"
       << "c=IN IP4 "
       << ip::address_v4(info.stream[1].m_ui32DestIP).to_string();
    if (IN_MULTICAST(info.stream[1].m_ui32DestIP)) {
      ss << "/" << static_cast<unsigned>(info.stream[1].m_byTTL);
    }
    ss << "\na=source-filter: incl IN IP4 "
       << ip::address_v4(info.stream[1].m_ui32DestIP).to_string() << " "
       << sec_ip_str;
    ss << "\na=rtpmap:" << static_cast<unsigned>(info.stream[1].m_byPayloadType)
       << " " << info.stream[1].m_cCodec << "/" << sample_rate << "/"
       << static_cast<unsigned>(info.stream[1].m_byNbOfChannels) << "\n"
       << "a=sync-time:0\n"
       << "a=framecount:" << info.stream[1].m_ui32MaxSamplesPerPacket << "\n"
       << "a=ptime:" << ptime << "\n"
       << "a=mediaclk:direct=0\n";
    ss << "a=clock-domain:PTPv2 " << static_cast<unsigned>(domain)
       << "\na=ts-refclk:ptp=IEEE1588-2008:";
    if (info.refclk_ptp_traceable) {
      ss << "traceable\n";
    } else {
      ss << refclk_gmid << ":" << static_cast<unsigned>(domain)
         << "\n";
    }
    ss << "a=mid:2\n";
  }
  return ss.str();
}

std::error_code SessionManager::get_source_sdp(uint32_t id,
                                               std::string& sdp) const {
  std::shared_lock sources_lock(sources_mutex_);
  auto const it = sources_.find(id);
  if (it == sources_.end()) {
    BOOST_LOG_TRIVIAL(error)
        << "session_manager:: source " << std::to_string(id) << " not in use";
    return DaemonErrc::stream_id_not_in_use;
  }
  const auto& info = (*it).second;
  sdp = get_source_sdp_(id, info);
  return std::error_code{};
}

std::error_code SessionManager::remove_source(uint32_t id) {
  if (id > stream_id_max) {
    BOOST_LOG_TRIVIAL(error) << "session_manager:: source id "
                             << std::to_string(id) << " is not valid";
    return DaemonErrc::invalid_stream_id;
  }

  std::unique_lock sources_lock(sources_mutex_);
  auto const it = sources_.find(id);
  if (it == sources_.end()) {
    BOOST_LOG_TRIVIAL(error)
        << "session_manager:: source " << std::to_string(id) << " not in use";
    return DaemonErrc::stream_id_not_in_use;
  }

  std::error_code ret;
  if (const auto& info = (*it).second; info.enabled) {
    ret = driver_->remove_rtp_stream(info.handle[0]);
    if (!ret && info.st20227_enabled) {
      ret = driver_->remove_rtp_stream(info.handle[1]);
    }
    if (!ret) {
      on_remove_source(info);
    }
  }
  if (!ret) {
    sources_.erase(id);
    mark_status_dirty();  /* D7: source removed */
  }

  return ret;
}

uint8_t SessionManager::get_sink_id(const std::string& name) const {
  /* D3: see get_source_id — reads must hold the mutex the writers hold. */
  std::shared_lock sinks_lock(sinks_mutex_);
  const auto it = sink_names_.find(name);
  return it != sink_names_.end() ? it->second : (stream_id_max + 1);
}

void SessionManager::on_add_sink(const StreamSink& sink,
                                 const StreamInfo& info) {
  for (const auto& cb : add_sink_observers_) {
    cb(sink.id, sink.name);
  }
  if (IN_MULTICAST(info.stream[0].m_ui32DestIP)) {
    igmp_[0].join(config_->get_ip_addr_str(),
                  ip::address_v4(info.stream[0].m_ui32DestIP).to_string());
    if (info.st20227_enabled) {
      auto [ip_addr, ip_str] = get_interface_ip(config_->get_interface_name(1));
      igmp_[1].join(ip_str,
                    ip::address_v4(info.stream[1].m_ui32DestIP).to_string());
    }
  }
  sink_names_[sink.name] = sink.id;
}

void SessionManager::on_remove_sink(const StreamInfo& info) {
  for (const auto& cb : remove_sink_observers_) {
    cb((uint8_t)info.stream[0].m_uiId, info.stream[0].m_cName);
  }
  if (IN_MULTICAST(info.stream[0].m_ui32DestIP)) {
    igmp_[0].leave(config_->get_ip_addr_str(),
                   ip::address_v4(info.stream[0].m_ui32DestIP).to_string());
    if (info.st20227_enabled) {
      auto [ip_addr, ip_str] = get_interface_ip(config_->get_interface_name(1));
      igmp_[1].leave(ip_str,
                     ip::address_v4(info.stream[1].m_ui32DestIP).to_string());
    }
  }
  sink_names_.erase(info.stream[0].m_cName);
}

std::error_code SessionManager::add_sink(const StreamSink& sink) {
  if (sink.id > stream_id_max) {
    BOOST_LOG_TRIVIAL(error) << "session_manager:: sink id "
                             << std::to_string(sink.id) << " is not valid";
    return DaemonErrc::invalid_stream_id;
  }

  /* 2026-06-09 review hardening: validate the PCM binding and the channel
   * map BEFORE anything reaches the kernel — it trusts both blindly (a bad
   * pcm binds to an empty chip slot and leaks a kernel stream handler; an
   * oversized map overruns m_aui32Routing[]). W10.2: validate against the
   * runtime card set (cards_), not daemon.conf device_groups. */
  if (!pcm_declared_(sink.pcm)) {
    BOOST_LOG_TRIVIAL(error)
        << "session_manager:: sink " << std::to_string(sink.id)
        << " references PCM " << std::to_string(sink.pcm)
        << " with no live card";
    return DaemonErrc::invalid_pcm;
  }
  {
    bool channel_used[256] = {false};
    if (sink.map.size() > MAX_CHANNELS_BY_RTP_STREAM) {
      BOOST_LOG_TRIVIAL(error)
          << "session_manager:: sink " << std::to_string(sink.id)
          << " channel map has " << sink.map.size() << " entries (max "
          << MAX_CHANNELS_BY_RTP_STREAM << ")";
      return DaemonErrc::invalid_channel_map;
    }
    /* W10.2 defensive: every map index must fit the bound pcm. A sink writes the
     * card's ALSA INPUT channels, so each index must be < num_inputs. */
    {
      Pcm bound;
      if (pcm_for_id_(sink.pcm, bound)) {
        for (uint8_t ch : sink.map) {
          if (ch >= bound.num_inputs) {
            BOOST_LOG_TRIVIAL(error)
                << "session_manager:: sink " << std::to_string(sink.id)
                << " channel " << (int)ch << " exceeds pcm \"" << bound.name
                << "\" input count (" << bound.num_inputs << ")";
            return DaemonErrc::invalid_channel_map;
          }
        }
      }
    }
    for (auto ch : sink.map) {
      if (channel_used[ch]) {
        BOOST_LOG_TRIVIAL(error)
            << "session_manager:: sink " << std::to_string(sink.id)
            << " maps physical channel " << std::to_string(ch) << " twice";
        return DaemonErrc::invalid_channel_map;
      }
      channel_used[ch] = true;
    }
    /* Reject overlap with any OTHER sink on the same PCM: two sinks
     * deinterleaving into the same physical channel is uncoordinated
     * last-writer-wins in the kernel ring — the 2026-06-04 "constant
     * crackle" (or silent channels when one sender is idle). */
    std::shared_lock sinks_lock(sinks_mutex_);
    for (const auto& [id, other] : sinks_) {
      if (id == sink.id)  // a PUT replaces this sink; don't self-collide
        continue;
      if (other.stream[0].m_uiPCMId != sink.pcm)
        continue;
      for (uint8_t ch = 0; ch < other.stream[0].m_byNbOfChannels; ++ch) {
        auto phys = other.stream[0].m_aui32Routing[ch];
        if (phys < 256 && channel_used[phys]) {
          BOOST_LOG_TRIVIAL(error)
              << "session_manager:: sink " << std::to_string(sink.id)
              << " physical channel " << phys << " on PCM "
              << std::to_string(sink.pcm) << " is already mapped by sink "
              << std::to_string(id);
          return DaemonErrc::channel_map_overlap;
        }
      }
    }
  }

  StreamInfo info;
  memset(&info.stream[0], 0, sizeof info.stream[0]);
  info.st20227_enabled = false;
  info.stream[0].m_bSource = 0;  // sink
  info.stream[0].m_ui32CRTP_stream_info_sizeof = sizeof(info.stream[0]);
  strncpy(info.stream[0].m_cName, sink.name.c_str(),
          sizeof(info.stream[0].m_cName) - 1);
  info.stream[0].m_uiId = sink.id;
  info.stream[1].m_uiIfPortId = 0;
  info.stream[0].m_uiPCMId = sink.pcm;  // multi-rate Stage 1
  info.stream[0].m_byNbOfChannels = sink.map.size();
  std::copy(sink.map.begin(), sink.map.end(), info.stream[0].m_aui32Routing);
  info.stream[0].m_ui32PlayOutDelay = sink.delay;
  info.stream[0].m_ui32RTCPSrcIP = config_->get_ip_addr();
  memcpy(&info.stream[1], &info.stream[0], sizeof(info.stream[0]));
  info.ignore_refclk_gmid = sink.ignore_refclk_gmid;
  info.io = sink.io;

  if (!sink.use_sdp) {
    auto const [ok, protocol, host, port, path] = parse_url(sink.source);
    if (!ok) {
      BOOST_LOG_TRIVIAL(error)
          << "session_manager:: cannot parse URL " << sink.source;
      return DaemonErrc::invalid_url;
    }

    std::string sdp;
    if (boost::iequals(protocol, "http")) {
      httplib::Client cli(host.c_str(),
                          !atoi(port.c_str()) ? 80 : atoi(port.c_str()));
      cli.set_connection_timeout(10);
      cli.set_read_timeout(10);
      cli.set_write_timeout(10);
      auto res = cli.Get(path.c_str());
      if (!res) {
        BOOST_LOG_TRIVIAL(error)
            << "session_manager:: annot retrieve SDP from URL " << sink.source;
        return DaemonErrc::cannot_retrieve_sdp;
      }
      if (res->status != 200) {
        BOOST_LOG_TRIVIAL(error)
            << "session_manager:: cannot retrieve SDP from URL " << sink.source
            << " server reply " << res->status;
        return DaemonErrc::cannot_retrieve_sdp;
      }
      sdp = std::move(res->body);
    } else if (boost::iequals(protocol, "rtsp")) {
      auto res = RtspClient::describe(path, host, port);
      if (!res.first) {
        BOOST_LOG_TRIVIAL(error)
            << "session_manager:: cannot retrieve SDP from URL " << sink.source;
        return DaemonErrc::cannot_retrieve_sdp;
      }
      sdp = std::move(res.second.sdp);
    } else {
      BOOST_LOG_TRIVIAL(error)
          << "session_manager:: unsupported protocol in URL " << sink.source;
      return DaemonErrc::invalid_url;
    }

    BOOST_LOG_TRIVIAL(info)
        << "session_manager:: SDP from URL " << sink.source << " :\n"
        << sdp;

    if (!parse_sdp(sdp, info)) {
      return DaemonErrc::cannot_parse_sdp;
    }

    info.sink_sdp = std::move(sdp);
  } else {
    BOOST_LOG_TRIVIAL(info) << "session_manager:: using SDP " << std::endl
                            << sink.sdp;
    if (!parse_sdp(sink.sdp, info)) {
      return DaemonErrc::cannot_parse_sdp;
    }

    info.sink_sdp = sink.sdp;
  }
  info.sink_source = sink.source;
  info.sink_use_sdp = true;  // save back and use with SDP file

  /* W7 (Decision 10): fail-loud if the source's SDP rate doesn't match
   * the rate of the card this sink is bound to — the daemon-side mirror
   * of the W6 kernel guard, caught at the REST boundary with a clear
   * message instead of surfacing as a generic kernel -EINVAL. W10.2: the
   * target rate comes from the owning card (cards_), not device_groups. */
  {
    uint32_t card_rate = rate_for_pcm_(sink.pcm);
    if (info.stream[0].m_ui32SamplingRate != card_rate) {
      BOOST_LOG_TRIVIAL(error)
          << "session_manager:: sink " << sink.id << " SDP rate "
          << info.stream[0].m_ui32SamplingRate << " does not match card on pcm "
          << std::to_string(sink.pcm) << " rate " << card_rate;
      return DaemonErrc::invalid_sample_rate;
    }
  }

  int media_num = info.st20227_enabled ? media_max : 1;
  for (int mid = 0; mid < media_num; mid++) {
    info.stream[mid].m_ui32FrameSize = info.stream[0].m_ui32MaxSamplesPerPacket;
    if (!info.stream[mid].m_ui32FrameSize) {
      // if not from SDP use config
      info.stream[mid].m_ui32FrameSize = config_->get_max_tic_frame_size();
    }

    BOOST_LOG_TRIVIAL(info)
        << "session_manager:: media " << mid << " sink addr "
        << ip::address_v4(info.stream[mid].m_ui32DestIP).to_string() << ":"
        << info.stream[mid].m_usDestPort;
    BOOST_LOG_TRIVIAL(info)
        << "session_manager:: media " << mid << " sink frame size "
        << info.stream[mid].m_ui32FrameSize;
    BOOST_LOG_TRIVIAL(info)
        << "session_manager:: media " << mid << " playout delay "
        << info.stream[mid].m_ui32PlayOutDelay;

    if (IN_MULTICAST(info.stream[mid].m_ui32DestIP)) {
      auto mcast_mac_addr = get_mcast_mac_addr(info.stream[0].m_ui32DestIP);
      std::copy(std::begin(mcast_mac_addr), std::end(mcast_mac_addr),
                info.stream[mid].m_ui8DestMAC);
    } else {
      auto mac_addr = config_->get_mac_addr();
      std::copy(std::begin(mac_addr), std::end(mac_addr),
                info.stream[mid].m_ui8DestMAC);
    }
  }

  std::unique_lock sinks_lock(sinks_mutex_);
  auto const it = sinks_.find(sink.id);
  if (it != sinks_.end()) {
    BOOST_LOG_TRIVIAL(info)
        << "session_manager:: sink id " << std::to_string(sink.id)
        << " is in use, updating";
    // remove previous stream
    (void)driver_->remove_rtp_stream((*it).second.handle[0]);
    if ((*it).second.st20227_enabled) {
      (void)driver_->remove_rtp_stream((*it).second.handle[1]);
    }
    on_remove_sink((*it).second);
  } else if (sink_names_.find(sink.name) != sink_names_.end()) {
    BOOST_LOG_TRIVIAL(error)
        << "session_manager:: sink name " << sink.name << " is in use";
    return DaemonErrc::stream_name_in_use;
  }

  auto ret = driver_->add_rtp_stream(sink.pcm, info.stream[0], info.handle[0]);
  if (ret) {
    if (it != sinks_.end()) {
      /* update operation failed */
      sinks_.erase(sink.id);
    }
    return ret;
  }

  if (config_->get_interface_name(1).length() > 0) {
    auto [ip_addr, ip_str] = get_interface_ip(config_->get_interface_name(1));
    if (!ip_str.empty()) {
      if (!info.st20227_enabled) {
        /* if no DUP in SDP, duplicate information of primary audio media */
        memcpy(&info.stream[1], &info.stream[0], sizeof(info.stream[0]));
      }

      info.stream[1].m_ui32RTCPSrcIP = ip_addr;
      info.stream[1].m_uiIfPortId = 1;
      /* multi-rate Stage 1: redundant copy stays on the same PCM. */
      info.stream[1].m_uiPCMId = sink.pcm;

      if (!IN_MULTICAST(info.stream[1].m_ui32DestIP)) {
        auto [mac_addr, mac_str] =
            get_interface_mac(config_->get_interface_name(1));
        if (!mac_str.empty()) {
          std::copy(std::begin(mac_addr), std::end(mac_addr),
                    info.stream[1].m_ui8DestMAC);
        }
      }
      ret = driver_->add_rtp_stream(sink.pcm, info.stream[1], info.handle[1]);
      if (ret) {
        /* D1 (2026-06 audit): roll back the stream just created for THIS add —
         * info.handle[0] — not (*it).second.handle[0], which is the PREVIOUS
         * sink's already-removed handle (double-remove) and an end() deref
         * (UB) on a fresh add. */
        (void)driver_->remove_rtp_stream(info.handle[0]);
        if (it != sinks_.end()) {
          /* update operation failed */
          sinks_.erase(sink.id);
        }
        return ret;
      }
      info.st20227_enabled = true;
    }
  } else {
    info.st20227_enabled = false;
  }
  on_add_sink(sink, info);

  // update sinks map
  sinks_[sink.id] = info;
  mark_status_dirty();  /* D7: sink committed */
  BOOST_LOG_TRIVIAL(info) << "session_manager:: added sink "
                          << std::to_string(sink.id) << " " << info.handle[0]
                          << "," << info.handle[1];
  return ret;
}

std::error_code SessionManager::remove_sink(uint32_t id) {
  if (id > stream_id_max) {
    BOOST_LOG_TRIVIAL(error) << "session_manager:: sink id "
                             << std::to_string(id) << " is not valid";
    return DaemonErrc::stream_id_in_use;
  }

  std::unique_lock sinks_lock(sinks_mutex_);
  auto const it = sinks_.find(id);
  if (it == sinks_.end()) {
    BOOST_LOG_TRIVIAL(error)
        << "session_manager:: sink " << std::to_string(id) << " not in use";
    return DaemonErrc::stream_id_not_in_use;
  }

  const auto& info = (*it).second;
  auto ret = driver_->remove_rtp_stream(info.handle[0]);
  if (!ret && info.st20227_enabled) {
    ret = driver_->remove_rtp_stream(info.handle[1]);
  }
  if (!ret) {
    on_remove_sink(info);
    sinks_.erase(id);
    mark_status_dirty();  /* D7: sink removed */
  }

  return ret;
}

std::error_code SessionManager::get_sink_status(
    uint32_t id,
    SinkStreamStatus& sink_status) const {
  if (id > stream_id_max) {
    BOOST_LOG_TRIVIAL(error) << "session_manager:: sink id "
                             << std::to_string(id) << " is not valid";
    return DaemonErrc::invalid_stream_id;
  }

  std::shared_lock sinks_lock(sinks_mutex_);
  auto const it = sinks_.find(id);
  if (it == sinks_.end()) {
    BOOST_LOG_TRIVIAL(error)
        << "session_manager:: sink " << std::to_string(id) << " not in use";
    return DaemonErrc::stream_id_not_in_use;
  }

  TRTP_stream_status status;
  const auto& info = (*it).second;
  auto ret = driver_->get_rtp_stream_status(info.handle[0], status);
  if (!ret) {
    sink_status.is_rtp_seq_id_error = status.u.flags & 0x01;
    sink_status.is_rtp_ssrc_error = status.u.flags & 0x02;
    sink_status.is_rtp_payload_type_error = status.u.flags & 0x04;
    sink_status.is_rtp_sac_error = status.u.flags & 0x08;
    sink_status.is_receiving_rtp_packet = status.u.flags & 0x10;
    sink_status.is_muted = status.u.flags & 0x20;
    sink_status.is_some_muted = status.u.flags & 0x40;
    sink_status.is_all_muted = status.u.flags & 0x80;
    sink_status.min_time = status.sink_min_time;
    /* #32: receive margin + the pcm's live rate (kernel truth) for time/ppm
     * conversion by consumers. */
    sink_status.receive_offset = status.sink_receive_offset;
    sink_status.sample_rate =
        rate_for_pcm_(static_cast<uint8_t>(info.stream[0].m_uiPCMId));
  }

  return ret;
}

std::error_code SessionManager::set_ptp_config(const PTPConfig& config) {
  TPTPConfig ptp_config;
  ptp_config.ui8Domain = config.domain;
  ptp_config.ui8DSCP = config.dscp;
  auto ret = driver_->set_ptp_config(ptp_config);
  if (!ret) {
    std::unique_lock ptp_lock(ptp_mutex_);
    ptp_config_ = config;
  }
  return ret;
}

void SessionManager::get_ptp_config(PTPConfig& config) const {
  std::shared_lock ptp_lock(ptp_mutex_);
  config = ptp_config_;
}

void SessionManager::get_ptp_status(PTPStatus& status) const {
  std::shared_lock ptp_lock(ptp_mutex_);
  status = ptp_status_;
}

void SessionManager::get_ptp_status_by_domain(
    std::map<uint8_t, PTPStatus>& status) const {
  std::shared_lock ptp_lock(ptp_mutex_);
  status = ptp_status_by_domain_;
}

void SessionManager::get_pcm_clocks(
    std::map<uint8_t, PcmRuntime>& status) const {
  std::shared_lock ptp_lock(ptp_mutex_);
  status.clear();
  for (const auto& [pcm_id, mirror] : ptp_pcm_status_) {
    PcmRuntime r;
    r.tic_status = mirror.tic_status;
    r.clock_state = mirror.clock_state;  // W16 slice 3: the reason-bearing state
    r.tick_period_us = mirror.tick_period_us;        // 3b: execution health
    r.us_since_last_tick = mirror.us_since_last_tick;
    if (pcm_id < pcm_id_max) {  // W28: kernel-truth live + armed rate (lock-free)
      r.live_rate = pcm_live_rate_[pcm_id].load(std::memory_order_acquire);
      r.pending_rate = pcm_pending_rate_[pcm_id].load(std::memory_order_acquire);
    }
    status.emplace(pcm_id, std::move(r));
  }
}

size_t SessionManager::process_sap() {
  size_t sdp_len_sum = 0;
  // set to contain sources currently announced
  std::set<uint32_t> active_sources;

  // announce all active sources
  std::shared_lock sources_lock(sources_mutex_);
  for (auto const& [id, info] : sources_) {
    if (info.enabled) {
      // retrieve current active source SDP
      auto sdp = get_source_sdp_(id, info);
      // compute source 16bit crc
      uint16_t msg_crc =
          crc16(reinterpret_cast<const uint8_t*>(sdp.c_str()), sdp.length());
      // compute source hash
      uint32_t msg_id_hash = (static_cast<uint32_t>(id) << 16) + msg_crc;
      // add/update this source in the announced sources
      announced_sources_[msg_id_hash] = {info.stream[0].m_ui32RTCPSrcIP,
                                         info.session_id, info.session_version};
      // add this source to the currently active sources
      active_sources.insert(msg_id_hash);
      // remove this source from deleted sources (if present)
      deleted_sources_count_.erase(msg_id_hash);
      // send announcement for this source
      sap_.announcement(msg_crc, info.stream[0].m_ui32RTCPSrcIP, sdp);
      // update amount of byte sent
      sdp_len_sum += sdp.length();
    }
  }

  // check for sources that are no longer announced and send deletion/s
  for (auto const& [msg_id_hash, info] : announced_sources_) {
    auto src_addr = std::get<0>(info);
    auto session_id = std::get<1>(info);
    auto session_version = std::get<2>(info);
    // check if this source is no longer announced
    if (active_sources.find(msg_id_hash) == active_sources.end()) {
      // retrieve deleted source SDP
      std::string sdp = get_removed_source_sdp_(msg_id_hash >> 16, src_addr,
                                                session_id, session_version);
      // send deletion for this source
      sap_.deletion(static_cast<uint16_t>(msg_id_hash), src_addr, sdp);
      // update amount of byte sent
      sdp_len_sum += sdp.length();
      // increase count
      deleted_sources_count_[msg_id_hash]++;
    }
  }

  // remove all deleted sources announced SAP::max_deletions times
  std::experimental::erase_if(announced_sources_, [this](auto source) {
    const auto& msg_id_hash = source.first;

    if (this->deleted_sources_count_[msg_id_hash] >= SAP::max_deletions) {
      // remove from deleted sources
      this->deleted_sources_count_.erase(msg_id_hash);
      // remove from announced sources
      return true;
    }
    return false;
  });

  return sdp_len_sum;
}

/* Pull the media sample rate from the first audio rtpmap of an SDP
 * (a=rtpmap:<pt> <codec>/<rate>[/<ch>]); 0 if absent/unparseable. Rate-follow
 * detection needs only the rate, so unlike parse_sdp this neither validates the
 * PTP refclk/gmid (which legitimately differs per domain) nor warns per tick. */
static uint32_t sdp_media_rate(const std::string& sdp) {
  std::stringstream ss(sdp);
  std::string line;
  while (std::getline(ss, line, '\n')) {
    boost::trim(line);
    if (line.rfind("a=rtpmap:", 0) != 0)
      continue;
    auto first = line.find('/');
    if (first == std::string::npos)
      continue;
    auto second = line.find('/', first + 1);
    auto len = (second == std::string::npos ? line.size() : second) - first - 1;
    try {
      return static_cast<uint32_t>(std::stoul(line.substr(first + 1, len)));
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

std::list<StreamSink> SessionManager::get_updated_sinks(
    const std::list<RemoteSource>& sources_list) {
  std::list<StreamSink> sinks_list;
  std::shared_lock sinks_lock(sinks_mutex_);
  for (auto const& [id, info] : sinks_) {
    uint64_t newVersion{0};
    StreamSink sink{get_sink_(id, info)};
    for (auto& source : sources_list) {
      // if no remote source origin specified, skip
      if (source.origin.session_id == "")
        continue;

      // search for the largest corresponding remote source version
      if (sinks_[sink.id].origin == source.origin && sink.sdp != source.sdp &&
          sinks_[sink.id].origin.session_version <
              source.origin.session_version &&
          newVersion < source.origin.session_version) {
        newVersion = source.origin.session_version;
        sink.sdp = source.sdp;
      }
    }

    if (newVersion) {
      BOOST_LOG_TRIVIAL(info)
          << "session_manager:: sink " << std::to_string(sink.id)
          << " SDP change detected version " << newVersion << " updating";
      sinks_list.emplace_back(std::move(sink));
    }
  }
  return sinks_list;
}

std::list<StreamSink> SessionManager::get_rate_follow_sinks(
    const std::list<RemoteSource>& sources_list) {
  /* A source that re-rates without bumping its SDP o= version (e.g. the VAD)
   * slips past get_updated_sinks' RFC-4566 monotonicity guard. For a
   * rate_follows_source sink the version is irrelevant — the only question is
   * whether the discovered source's rate differs from the pcm's. Two phases so
   * the sinks_ -> cards_ lock order is never inverted: collect same-origin
   * content-changed candidates under sinks_mutex_, then resolve the pcm rate
   * (cards_mutex_) outside it. */
  struct Candidate {
    StreamSink sink;
    uint32_t src_rate;
  };
  std::list<Candidate> candidates;
  {
    std::shared_lock sinks_lock(sinks_mutex_);
    for (auto const& [id, info] : sinks_) {
      StreamSink sink{get_sink_(id, info)};
      for (auto const& source : sources_list) {
        if (source.origin.session_id.empty() ||
            !(sinks_[sink.id].origin == source.origin) ||
            sink.sdp == source.sdp)
          continue;
        uint32_t src_rate = sdp_media_rate(source.sdp);
        if (src_rate == 0)
          continue;
        sink.sdp = source.sdp;  // adopt the new SDP (mDNS/SAP carry the same body)
        candidates.push_back({std::move(sink), src_rate});
        break;
      }
    }
  }
  std::list<StreamSink> sinks_list;
  for (auto& c : candidates) {
    Pcm pcm;
    // Skip pcms that opted into strict RFC-4566 versioning — those follow only
    // via get_updated_sinks' version-bump gate, never this version-blind path.
    if (pcm_for_id_(c.sink.pcm, pcm) && pcm.rate_follows_source &&
        !pcm.rate_follow_strict_version && c.src_rate != pcm.sample_rate) {
      BOOST_LOG_TRIVIAL(info)
          << "session_manager:: sink " << std::to_string(c.sink.id)
          << " rate-follow: source advertises " << c.src_rate << " Hz, pcm \""
          << pcm.name << "\" at " << pcm.sample_rate
          << " Hz (no o= version bump) — following";
      sinks_list.emplace_back(std::move(c.sink));
    }
  }
  return sinks_list;
}

void SessionManager::update_sinks() {
  if (config_->get_auto_sinks_update()) {
    uint32_t last_update = browser_->get_last_update_ts();
    // check remote sources only if an update arrived
    if (last_update && last_sink_update_ != last_update) {
      BOOST_LOG_TRIVIAL(debug) << "Updating sinks ...";
      std::list<RemoteSource> remote_sources = browser_->get_remote_sources();
      /* W28: retract a stale in-place re-rate. If a pcm we armed is now already
       * at the rate its follow-source advertises (the source flapped back to the
       * live rate before the client released the device), cancel the latch so the
       * stale target can't fire on the next close. Match the pending's source by
       * SDP origin (version-blind) so a reverted source — same origin, old rate —
       * is recognised; compare against the kernel-truth live rate. */
      for (auto it = pending_rerates_.begin(); it != pending_rerates_.end();) {
        SDPOrigin o = sdp_get_origin(it->sink.sdp);
        uint32_t desired = 0;
        std::string cur_sdp;  // the source's CURRENT SDP (reverted rate; maybe new SSRC)
        for (auto const& src : remote_sources)
          if (src.origin == o) {
            desired = sdp_media_rate(src.sdp);
            cur_sdp = src.sdp;
            break;
          }
        uint32_t live = rate_for_pcm_(it->pcm_id);
        if (desired != 0 && desired == live && it->new_rate != live) {
          driver_->cancel_pcm_rate(it->pcm_id);
          if (it->pcm_id < pcm_id_max)
            pcm_pending_rate_[it->pcm_id].store(0, std::memory_order_release);
          /* The source restarted while we were armed (44100->48000->44100 is two
           * stream restarts) so its SSRC has likely rolled, even though its rate is
           * back to the live rate. The still-subscribed sink is locked to the old
           * SSRC — re-add it with the current SDP to re-acquire the new stream. */
          StreamSink sink = it->sink;
          sink.sdp = cur_sdp;
          add_sink(sink);
          BOOST_LOG_TRIVIAL(info)
              << "session_manager:: in-place re-rate of pcm_id "
              << (int)it->pcm_id << " cancelled — follow-source reverted to the "
              << "live rate (" << live << " Hz); sink " << (int)sink.id
              << " re-attached to re-acquire the restarted stream";
          it = pending_rerates_.erase(it);
        } else {
          ++it;
        }
      }
      auto sinks_list = get_updated_sinks(remote_sources);
      // W15: also follow sources that re-rated WITHOUT bumping their SDP o=
      // version (get_updated_sinks ignores those by RFC-4566 monotonicity).
      // Union by sink id — a version-bumped rate change already appears above.
      {
        std::set<uint8_t> seen;
        for (auto const& s : sinks_list)
          seen.insert(s.id);
        for (auto& s : get_rate_follow_sinks(remote_sources))
          if (seen.insert(s.id).second)
            sinks_list.emplace_back(std::move(s));
      }
      for (auto& sink : sinks_list) {
        // If the sink's pcm follows its source rate, re-rate the pcm to the new
        // source's rate BEFORE re-adding the sink, so the SDP-rate vs pcm-rate
        // check in add_sink passes instead of rejecting. Per the owning card's
        // rate_change_mode: "recreate" rebuilds the card (recreate_card_),
        // "in-place" re-keys the live chip (SetPCMRate) — and if the kernel arms
        // that (client still holds the device), the sink re-add is deferred until
        // the kernel's PCMRateApplied event (process_rerate_events_ on the worker).
        bool deferred = false;
        Pcm pcm;
        if (pcm_for_id_(sink.pcm, pcm) && pcm.rate_follows_source) {
          // Rate-follow needs only the source rate. Read it directly rather than
          // via full parse_sdp, which validates the PTP refclk/gmid and so rejects
          // a legitimate cross-domain source (and would log a warning every browse
          // tick); the real gmid policy is still enforced later in add_sink.
          uint32_t new_rate = sdp_media_rate(sink.sdp);
          if (new_rate != 0 && new_rate != pcm.sample_rate) {
            Card card;
            bool in_place = !get_card_by_name(pcm.card, card) &&
                            card.rate_change_mode == "in-place";
            // If the kernel already holds this exact re-rate latched (the client
            // still hasn't released the device), just wait for the apply event —
            // don't re-arm or re-log every browse tick (that was a storm + spam).
            auto pend = std::find_if(
                pending_rerates_.begin(), pending_rerates_.end(),
                [&](const PendingRerate& p) { return p.pcm_id == pcm.pcm_id; });
            bool already_armed = in_place && pend != pending_rerates_.end() &&
                                 pend->new_rate == new_rate;
            /* D6 (2026-06 audit): the daemon-side pending is a SHADOW of the
             * kernel latch — validate it against kernel truth (the W28 mirror).
             * If the kernel no longer holds this latch (module reloaded / card
             * recreated while armed), the pending is poison: it would defer the
             * sink forever waiting for an apply event that can never come.
             * Drop it and fall through to re-arm. */
            if (already_armed && pcm.pcm_id < pcm_id_max &&
                pcm_pending_rate_[pcm.pcm_id].load(std::memory_order_acquire) !=
                    new_rate) {
              BOOST_LOG_TRIVIAL(warning)
                  << "session_manager:: pending re-rate of pcm \"" << pcm.name
                  << "\" -> " << new_rate
                  << " has no kernel latch behind it (module reload or card "
                     "recreate while armed?); dropping and re-arming";
              pending_rerates_.erase(pend);
              pend = pending_rerates_.end();
              already_armed = false;
            }
            if (already_armed) {
              deferred = true;
            } else {
              BOOST_LOG_TRIVIAL(info)
                  << "session_manager:: auto-follow: re-rating pcm \"" << pcm.name
                  << "\" (card \"" << pcm.card << "\", "
                  << (in_place ? "in-place" : "recreate") << ") " << pcm.sample_rate
                  << " -> " << new_rate << " to match source of sink "
                  << std::to_string(sink.id);
              if (in_place) {
                if (pend != pending_rerates_.end())
                  pending_rerates_.erase(pend);  // re-targeting: drop the stale latch
                // Record the pending BEFORE arming, so the kernel's apply event
                // (fired the instant the client closes) can never beat the record.
                pending_rerates_.push_back({pcm.pcm_id, new_rate, sink});
                auto ec = driver_->set_pcm_rate(pcm.pcm_id, new_rate);
                if (!ec) {
                  // applied at once (chip idle): no close-apply event will come,
                  // so do the bookkeeping now; add_sink runs below.
                  set_pcm_rate_model_(pcm.pcm_id, new_rate);
                  reannounce_pcm_sources_(pcm.pcm_id);  // downstream auto-follow
                  pending_rerates_.pop_back();
                } else if (ec == DriverErrc::busy) {
                  deferred = true;  // armed; the apply event will re-attach the sink
                  if (pcm.pcm_id < pcm_id_max)
                    pcm_pending_rate_[pcm.pcm_id].store(
                        new_rate, std::memory_order_release);  // reflect for the UI
                  BOOST_LOG_TRIVIAL(info)
                      << "session_manager:: in-place re-rate of pcm \"" << pcm.name
                      << "\" armed (client holds the device); will re-attach on the "
                         "kernel's apply event";
                } else {
                  pending_rerates_.pop_back();
                  BOOST_LOG_TRIVIAL(warning)
                      << "session_manager:: in-place re-rate of pcm \"" << pcm.name
                      << "\" failed: " << ec.message();
                }
              } else {
                Pcm np = pcm;  // preserves card/num_inputs/.../rate_follows_source
                np.sample_rate = new_rate;
                np.name = "";  // update_pcm: empty name => keep current name
                if (auto ec = update_pcm(pcm.card, pcm.name, np)) {
                  BOOST_LOG_TRIVIAL(warning)
                      << "session_manager:: auto-follow re-rate of pcm \"" << pcm.name
                      << "\" failed: " << ec.message();
                }
              }
            }
          }
        }
        // Re-add sink with new SDP (same id => an update), unless an armed
        // in-place re-rate deferred it.
        if (!deferred)
          add_sink(sink);
      }
      last_sink_update_ = last_update;
    }
  }
}

void SessionManager::set_pcm_rate_model_(uint8_t pcm_id, uint32_t new_rate) {
  /* The kernel already re-keyed the chip (SetPCMRate applied); reflect the new
   * rate in the daemon model so the rate guards + status agree. Persistence is
   * shutdown-only (like every other topology mutation). */
  /* W28: kernel truth — update the live-rate mirror immediately (the event is
   * the prompt; the poll is the backstop). Lock-free, so do it outside the
   * cards lock. The configured/intent rate below converges to it: an applied
   * re-rate is one the daemon commanded/followed, so intent == truth here. */
  if (pcm_id < pcm_id_max) {
    pcm_live_rate_[pcm_id].store(new_rate, std::memory_order_release);
    pcm_pending_rate_[pcm_id].store(0, std::memory_order_release);  // latch cleared
  }
  std::unique_lock cards_lock(cards_mutex_);
  auto it = pcms_.find(pcm_id);
  if (it != pcms_.end()) {
    it->second.sample_rate = new_rate;
    mark_status_dirty();  /* D7: pcm rate truth changed */
    BOOST_LOG_TRIVIAL(info)
        << "session_manager:: in-place re-rate applied: pcm \"" << it->second.name
        << "\" (pcm_id " << (int)pcm_id << ") -> " << new_rate << " Hz";
  }
}

void SessionManager::process_rerate_events_() {
  /* Wait (up to 1 s, or until a PCMRateApplied event / shutdown wakes us) for the
   * kernel to report armed in-place re-rates that applied on the client's last
   * close, then sync the model and re-attach each deferred sink. The 1 s timeout
   * just lets the worker's other periodic work run. */
  std::list<std::pair<uint8_t, uint32_t>> applied;
  {
    std::unique_lock<std::mutex> lk(rerate_events_->mtx);
    rerate_events_->cv.wait_for(lk, std::chrono::seconds(1), [this] {
      return !rerate_events_->applied.empty() || rerate_events_->stopping;
    });
    applied.swap(rerate_events_->applied);
  }
  /* D6 (2026-06 audit): drop pendings whose pcm no longer exists (card/pcm
   * removed while armed) — they can never complete and would permanently
   * short-circuit future follows for that pcm_id via already_armed. */
  pending_rerates_.remove_if([this](const PendingRerate& p) {
    Pcm tmp;
    bool gone = !pcm_for_id_(p.pcm_id, tmp);
    if (gone)
      BOOST_LOG_TRIVIAL(warning)
          << "session_manager:: dropping pending re-rate for vanished pcm_id "
          << (int)p.pcm_id << " (target " << p.new_rate << " Hz)";
    return gone;
  });

  for (auto& [pcm_id, rate] : applied)
    reconcile_rate_applied_(pcm_id, rate);
}

void SessionManager::reconcile_rate_applied_(uint8_t pcm_id, uint32_t rate) {
  /* The kernel is authoritative for the live chip rate: sync the model and
   * re-announce on EVERY apply, whether or not we still hold a pending.
   * Discarding an event with no matching pending is exactly what desynced the
   * daemon from the kernel (a long-held client outlives any record we keep). */
  set_pcm_rate_model_(pcm_id, rate);
  reannounce_pcm_sources_(pcm_id);  // let downstream receivers auto-follow
  /* re-attach the deferred sink iff we recorded one for this pcm */
  auto it = std::find_if(
      pending_rerates_.begin(), pending_rerates_.end(),
      [pcm_id](const PendingRerate& p) { return p.pcm_id == pcm_id; });
  if (it != pending_rerates_.end()) {
    add_sink(it->sink);
    BOOST_LOG_TRIVIAL(info)
        << "session_manager:: in-place re-rate of pcm_id " << (int)pcm_id
        << " applied; sink " << (int)it->sink.id
        << " re-added at " << rate << " Hz";
    pending_rerates_.erase(it);
  } else {
    BOOST_LOG_TRIVIAL(info)
        << "session_manager:: in-place re-rate of pcm_id " << (int)pcm_id
        << " applied; model synced to " << rate << " Hz";
  }
}

void SessionManager::on_update_sources() {
  // trigger sources SDP file update
  sources_mutex_.lock();
  for (auto& [id, info] : sources_) {
    for (const auto& cb : update_source_observers_) {
      info.session_version++;
      cb(id, info.stream[0].m_cName, get_source_sdp_(id, info));
    }
  }
  sources_mutex_.unlock();
  g_session_version++;
}

void SessionManager::reannounce_pcm_sources_(uint8_t pcm_id) {
  /* Re-announce only the source(s) on the re-rated pcm. The periodic SAP loop
   * already carries the new rate, but with an UNCHANGED o= version — so a
   * conformant receiver ignores it (RFC 4566 monotonicity). Bumping the version
   * here is what makes the receiver treat it as a new SDP and re-pull. Mirrors
   * on_update_sources' mechanism (regenerate SDP + notify the RTSP/SAP/file
   * observers), scoped to the one pcm. Worker context, not holding cards_mutex_. */
  std::unique_lock sources_lock(sources_mutex_);
  for (auto& [id, info] : sources_) {
    if (static_cast<uint8_t>(info.stream[0].m_uiPCMId) != pcm_id)
      continue;
    info.session_version++;
    for (const auto& cb : update_source_observers_)
      cb(id, info.stream[0].m_cName, get_source_sdp_(id, info));
  }
}

void SessionManager::on_ptp_status_changed(const std::string& status) const {
  /* W14: no manager-wide rate to (re)set on lock -- each PCM self-rates via
   * AddPCM; the kernel has no chip-0 reference rate anymore. */
  for (const auto& cb : ptp_status_observers_) {
    (void)cb(status);
  }

  static std::string g_ptp_status;

  if (g_ptp_status != status && !config_->get_ptp_status_script().empty()) {
    pid_t pid = fork();
    if (pid == 0) {
      /* child */
      int fdlimit = (int)sysconf(_SC_OPEN_MAX);
      /* close all partent's fds */
      for (int i = STDERR_FILENO + 1; i < fdlimit; i++)
        close(i);

      char* argv_list[] = {
          const_cast<char*>(config_->get_ptp_status_script().c_str()),
          const_cast<char*>(status.c_str()), nullptr};

      execv(config_->get_ptp_status_script().c_str(), argv_list);
      exit(0);
    }
    g_ptp_status = status;
  }
}

using namespace std::chrono;
using second_t = duration<double, std::ratio<1> >;

namespace {
// Pure: the kernel's PTP status -> the daemon's PTPStatus view.
const char* ptp_lock_status_str(int s) {
  switch (s) {
    case PTPLS_LOCKING: return "locking";
    case PTPLS_LOCKED:  return "locked";
    default:            return "unlocked";
  }
}

/* W16 slice 3 — pure: the kernel's canonical EClockState -> its wire string.
 * The one vocabulary the API and UI speak; values carried verbatim. */
const char* clock_state_str(int s) {
  switch (s) {
    case CLK_NO_SIGNAL: return "no-signal";
    case CLK_ACQUIRING: return "acquiring";
    case CLK_LOCKED:    return "locked";
    case CLK_SATURATED: return "saturated";
    case CLK_STOPPED:
    default:            return "stopped";
  }
}

PTPStatus to_ptp_status(const TPTPStatus& ds) {
  char id[24];
  const uint8_t* g = reinterpret_cast<const uint8_t*>(&ds.ui64GMID);
  snprintf(id, sizeof(id), "%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X", g[0],
           g[1], g[2], g[3], g[4], g[5], g[6], g[7]);
  return PTPStatus{ptp_lock_status_str(ds.nPTPLockStatus),
                   id,
                   ds.i32ClockJitter,
                   /* W16 slice 3b: the domain-level clock state. */
                   clock_state_str(ds.clock_state),
                   /* W16 slice 3: GM properties + rate estimate, verbatim. */
                   ds.i64GMRateOffsetPPB,
                   ds.ui8GMPriority1,
                   ds.ui8GMClockClass,
                   ds.ui8GMClockAccuracy,
                   ds.ui16GMOffsetScaledLogVariance,
                   ds.ui8GMPriority2,
                   ds.ui16GMStepsRemoved,
                   ds.ui8GMTimeSource};
}
}  // namespace

bool SessionManager::worker() {
  TPTPConfig ptp_config;
  auto sap_timepoint = steady_clock::now();
  auto ptp_timepoint = steady_clock::now();
  int sap_interval = 1;
  int ptp_interval = 0;

  sap_.set_multicast_interface(config_->get_ip_addr_str());

  // join PTP multicast addresses
  igmp_[0].join(config_->get_ip_addr_str(), ptp_primary_mcast_addr);
  if (config_->get_interface_name(1).length() > 0) {
    auto [ip_addr, ip_str] = get_interface_ip(config_->get_interface_name(1));
    igmp_[1].join(ip_str, ptp_primary_mcast_addr);
  }

  while (running_) {
    // check if it's time to update the PTP status
    if ((duration_cast<second_t>(steady_clock::now() - ptp_timepoint).count()) >
        ptp_interval) {
      ptp_timepoint = steady_clock::now();
      /* Poll every relevant PTP domain ONCE into an immutable map, then derive
       * all views from it (no re-reads, no per-domain special case): poll the
       * active card domains plus the daemon default (so the legacy single
       * status always has a value). */
      if (driver_->get_ptp_config(ptp_config)) {
        BOOST_LOG_TRIVIAL(error)
            << "session_manager:: failed to retrieve PTP clock info";
      } else {
        const uint8_t default_domain = config_->get_ptp_domain();
        std::set<uint8_t> card_domains;
        for (const auto& card : get_cards()) {
          card_domains.insert(card.domain);
        }
        std::set<uint8_t> poll_domains = card_domains;
        poll_domains.insert(default_domain);

        std::map<uint8_t, PTPStatus> statuses;
        for (uint8_t d : poll_domains) {
          TPTPStatus ds;
          if (!driver_->get_ptp_status(d, ds)) {
            statuses[d] = to_ptp_status(ds);
          }
        }

        /* The Clocks mirror is the active (card) domains' subset; the legacy
         * single status is the default domain's entry. */
        std::map<uint8_t, PTPStatus> by_domain;
        for (uint8_t d : card_domains) {
          auto it = statuses.find(d);
          if (it != statuses.end()) {
            by_domain[d] = it->second;
          }
        }
        auto gmids_of = [](const std::map<uint8_t, PTPStatus>& m) {
          std::map<uint8_t, std::string> g;
          for (const auto& [d, s] : m) {
            g[d] = s.gmid;
          }
          return g;
        };

        std::string status_changed_to;
        bool gmid_changed;
        {
          std::unique_lock<std::shared_mutex> lk(ptp_mutex_);
          /* the SDP refclk carries the gmid, not the lock state, so re-advertise
           * only when some active domain's grandmaster changed. */
          gmid_changed = gmids_of(by_domain) != gmids_of(ptp_status_by_domain_);
          ptp_status_by_domain_ = std::move(by_domain);
          auto pit = statuses.find(default_domain);
          if (pit != statuses.end()) {
            if (ptp_status_.status != pit->second.status) {
              status_changed_to = pit->second.status;
            }
            ptp_status_ = pit->second;
          }
        }

        if (!status_changed_to.empty()) {
          BOOST_LOG_TRIVIAL(info)
              << "session_manager:: new PTP clock status " << status_changed_to;
          on_ptp_status_changed(status_changed_to);
        }
        if (gmid_changed) {
          // a grandmaster id changed: re-advertise sources (the SDP refclk
          // carries the gmid). W14: there is no manager-wide sample rate to
          // track or re-apply here -- each PCM self-rates.
          on_update_sources();
        }

        /* #22: mirror each PCM's TIC-engine lock (keyed by pcm_id) for the Cards
         * per-PCM dots — poll into an immutable map, then assign once. W16
         * slice 3: also the canonical clock_state (the reason-bearing enum). */
        std::map<uint8_t, PcmClockMirror> pcm_status;
        for (const auto& pcm : get_pcms()) {
          TPCMStatus ps;
          if (!driver_->get_pcm_status(pcm.pcm_id, ps)) {
            pcm_status[pcm.pcm_id] = {ptp_lock_status_str(ps.nTICLockStatus),
                                      clock_state_str(ps.clock_state),
                                      ps.tick_period_us,
                                      ps.us_since_last_tick};
            /* W28: refresh the kernel-truth live + armed rate mirror. This is the
             * reconcile backstop — even if a PCMRateApplied event is dropped, the
             * mirror re-syncs to kernel truth on the next poll. */
            if (pcm.pcm_id < pcm_id_max) {
              pcm_live_rate_[pcm.pcm_id].store(ps.live_rate,
                                               std::memory_order_release);
              pcm_pending_rate_[pcm.pcm_id].store(ps.pending_rate,
                                                  std::memory_order_release);
              /* D5 (2026-06 audit): the mirror alone was HALF a backstop — a
               * lost apply event left the model/announce stale: SAP carried the
               * new rate under an UNCHANGED o= version (conformant receivers
               * ignore it) and intent != truth persisted. Detect the silent
               * divergence (live differs from the model, and no armed re-rate
               * in flight to explain it) and run the SAME convergence as the
               * event path. Worker thread — same context as the event path. */
              if (ps.live_rate != 0 && ps.pending_rate == 0 &&
                  ps.live_rate != pcm.sample_rate) {
                BOOST_LOG_TRIVIAL(warning)
                    << "session_manager:: poll backstop: pcm_id "
                    << (int)pcm.pcm_id << " live rate " << ps.live_rate
                    << " != model " << pcm.sample_rate
                    << " with no apply event seen — reconciling";
                reconcile_rate_applied_(pcm.pcm_id, ps.live_rate);
              }
            }
          }
        }
        {
          std::unique_lock<std::shared_mutex> lk(ptp_mutex_);
          ptp_pcm_status_ = std::move(pcm_status);
        }

        /* #32 / audit D8: consume the kernel's per-sink status — nothing did
         * before, so a sender restarting with a new SSRC at the same rate left
         * its sink MUTED FOREVER on rtp_ssrc_error (the VAD does exactly this).
         * If the sink is receiving packets but rejecting them on SSRC, re-add
         * it (same id => update, which re-acquires the SSRC). The 10 s poll
         * cadence is the retry backoff; each pass logs what it did. */
        for (const auto& sink : get_sinks()) {
          SinkStreamStatus st;
          if (get_sink_status(sink.id, st))
            continue;
          if (st.is_rtp_ssrc_error && st.is_receiving_rtp_packet) {
            BOOST_LOG_TRIVIAL(warning)
                << "session_manager:: sink " << (int)sink.id
                << " is receiving but rejecting on SSRC (sender restarted?) — "
                   "re-adding to re-acquire";
            if (auto ec = add_sink(sink)) {
              BOOST_LOG_TRIVIAL(error)
                  << "session_manager:: sink " << (int)sink.id
                  << " SSRC re-acquire re-add failed: " << ec.message();
            }
          }
        }
      }
      ptp_interval = 10;
    }

    // check if it's time to send sap announcements
    if ((duration_cast<second_t>(steady_clock::now() - sap_timepoint).count()) >
        sap_interval) {
      sap_timepoint = steady_clock::now();

      auto sdp_len_sum = process_sap();

      if (config_->get_sap_interval()) {
        // if announcement interval specified in configuration
        sap_interval = config_->get_sap_interval();
      } else {
        // compute next announcement interval
        sap_interval = std::max(static_cast<size_t>(SAP::min_interval),
                                sdp_len_sum * 8 / SAP::bandwidth_limit);
        sap_interval +=
            (std::rand() % (sap_interval * 2 / 3)) - (sap_interval / 3);
      }

      BOOST_LOG_TRIVIAL(info) << "session_manager:: next SAP announcements in "
                              << sap_interval << " secs";
    }

    update_sinks();
    // W15: wait ~1 s OR until the kernel reports an armed re-rate applied (the
    // event wakes us immediately); re-attaches deferred sinks. Replaces the old
    // fixed 1 s sleep, so the loop is event-paced for re-rates.
    process_rerate_events_();

    /* D7 (2026-06 audit): persist topology changes as they happen — the old
     * shutdown-only save lost everything since boot on a crash. Mutators set
     * the dirty flag; this pass (~1 s cadence via the rerate cv-wait) writes
     * the file atomically. On a failed save, stay dirty and retry next pass. */
    if (status_dirty_.exchange(false, std::memory_order_acq_rel)) {
      if (!save_status()) {
        status_dirty_.store(true, std::memory_order_release);
      }
    }
  }

  // at end, send deletion for all announced sources
  for (auto const& [msg_id_hash, info] : announced_sources_) {
    auto src_addr = std::get<0>(info);
    auto session_id = std::get<1>(info);
    auto session_version = std::get<2>(info);
    // retrieve deleted source SDP
    std::string sdp = get_removed_source_sdp_(msg_id_hash >> 16, src_addr,
                                              session_id, session_version);
    // send deletion for this source
    sap_.deletion(static_cast<uint16_t>(msg_id_hash), src_addr, sdp);
  }

  // leave PTP multicast addresses
  igmp_[0].leave(config_->get_ip_addr_str(), ptp_primary_mcast_addr);
  if (config_->get_interface_name(1).length() > 0) {
    auto [ip_addr, ip_str] = get_interface_ip(config_->get_interface_name(1));
    igmp_[1].leave(ip_str, ptp_primary_mcast_addr);
  }

  return true;
}
