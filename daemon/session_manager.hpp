//
//  session_manager.hpp
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

#ifndef _SESSION_MANAGER_HPP_
#define _SESSION_MANAGER_HPP_

#include <future>
#include <list>
#include <map>
#include <shared_mutex>
#include <thread>
#include <chrono>

#include "config.hpp"
#include "driver_interface.hpp"
#include "browser.hpp"
#include "igmp.hpp"
#include "sap.hpp"

constexpr static uint8_t media_max = 2;

struct StreamSource {
  uint8_t id{0};
  bool enabled{false};
  std::string name;
  std::string io;
  uint32_t max_samples_per_packet{0};
  std::string codec;
  std::string address;
  uint8_t ttl{0};
  uint8_t payload_type{0};
  uint8_t dscp{0};
  bool refclk_ptp_traceable{false};
  std::vector<uint8_t> map;
  /* multi-rate Stage 1: which PCM (hw:RAVENNA,pcm) this source draws from.
   * Defaults to 0 (the default PCM), so legacy single-PCM configs work
   * unchanged. JSON field "pcm" is optional and defaults to 0. */
  uint8_t pcm{0};
};

struct StreamSink {
  uint8_t id;
  std::string name;
  std::string io;
  bool use_sdp{false};
  std::string source;
  std::string sdp;
  uint32_t delay{0};
  bool ignore_refclk_gmid{false};
  std::vector<uint8_t> map;
  /* multi-rate Stage 1: which PCM (hw:RAVENNA,pcm) this sink writes into. */
  uint8_t pcm{0};
};

/* W10.2 runtime multi-card. A Card is one ALSA snd_card = one PTP clock domain
 * (a domain is NOT exclusive — several cards may share it). It holds N Pcms.
 * Card-level attributes ONLY: rate/channels/delays live on the Pcm (the kernel
 * sets rate per-chip via add_pcm_to_card). Runtime state owned by the
 * SessionManager, persisted to status.json (seeded from daemon.conf
 * device_groups on first boot). Addressed by `name` — the durable identity and
 * the hw:<name> ALSA id (handle is a recyclable slot). */
struct Card {
  uint8_t handle{0};   // kernel card slot [0, card_handle_max); recyclable
  std::string name;    // ALSA card id (hw:<name>); the durable identity
  uint8_t domain{0};   // PTP clock domain (W11; not unique — cards may share)

  friend bool operator==(const Card& a, const Card& b) {
    return a.handle == b.handle && a.name == b.name && a.domain == b.domain;
  }
};

/* A Pcm is one PCM device on a card. NAMED (unique within its card) — the
 * user-facing handle. `pcm_id` is the GLOBAL registry slot (the manager's
 * m_apALSAChip[] index, [0, pcm_id_max)) that streams reference via their `pcm`
 * FK; it is INTERNAL and is NOT the ALSA per-card device index (the `Dev` in
 * hw:<card>,<Dev>, which the daemon derives from add-order at serialization). */
struct Pcm {
  uint8_t pcm_id{0};        // global registry slot; the stream FK (internal)
  std::string card;         // FK: owning card's name
  std::string name;         // user-facing, unique within the card
  uint32_t sample_rate{0};  // 0 = inherit the daemon-wide default
  uint32_t num_inputs{0};
  uint32_t num_outputs{0};
  int32_t playout_delay{0};
  int32_t capture_delay{0};

  friend bool operator==(const Pcm& a, const Pcm& b) {
    return a.pcm_id == b.pcm_id && a.card == b.card && a.name == b.name &&
           a.sample_rate == b.sample_rate && a.num_inputs == b.num_inputs &&
           a.num_outputs == b.num_outputs &&
           a.playout_delay == b.playout_delay &&
           a.capture_delay == b.capture_delay;
  }
};

struct SinkStreamStatus {
  bool is_rtp_seq_id_error{false};
  bool is_rtp_ssrc_error{false};
  bool is_rtp_payload_type_error{false};
  bool is_rtp_sac_error{false};
  bool is_receiving_rtp_packet{false};
  bool is_muted{false};
  bool is_some_muted{false};
  bool is_all_muted{false};
  int min_time{0};
};

struct PTPConfig {
  uint8_t domain{0};
  uint8_t dscp{0};
};

struct PTPStatus {
  std::string status;
  std::string gmid;
  int32_t jitter{0};
};

struct StreamInfo {
  TRTP_stream_info stream[media_max];
  uint64_t handle[media_max]{0};
  bool enabled{false};
  bool st20227_enabled{false};
  bool refclk_ptp_traceable{false};
  bool ignore_refclk_gmid{false};
  std::string io;
  bool sink_use_sdp{true};
  std::string sink_source;
  std::string sink_sdp;
  uint32_t session_id{0};
  uint32_t session_version{0};
  SDPOrigin origin;
};

class SessionManager {
 public:
  constexpr static uint8_t stream_id_max = 63;
  /* W10.2 runtime cards. Mirror the kernel's MR_ALSA_MAX_CARDS / MAX_PCMS
   * (hand-kept in lockstep, same caveat as the kMaxPcmId note in
   * driver_manager.cpp). */
  constexpr static uint8_t card_handle_max = 4;  // MR_ALSA_MAX_CARDS
  constexpr static uint8_t pcm_id_max = 16;      // MAX_PCMS

  static std::shared_ptr<SessionManager> create(
      std::shared_ptr<DriverManager> driver,
      std::shared_ptr<Browser> browser,
      std::shared_ptr<Config> config);
  SessionManager() = delete;
  SessionManager(const SessionManager&) = delete;
  SessionManager& operator=(const SessionManager&) = delete;
  virtual ~SessionManager() = default;

  // session manager interface
  bool init() {
    if (!running_) {
      running_ = true;
      g_session_version = std::chrono::system_clock::now().time_since_epoch() /
                          std::chrono::seconds(1);
      // to have an increasing session versions between restarts
      res_ = std::async(std::launch::async, &SessionManager::worker, this);
    }
    return true;
  }

  bool terminate() {
    if (running_) {
      running_ = false;
      auto ret = res_.get();
      for (const auto& source : get_sources()) {
        remove_source(source.id);
      }
      for (const auto& sink : get_sinks()) {
        remove_sink(sink.id);
      }
      /* W10.2: the SessionManager owns the cards now -- tear them down on clean
       * shutdown (the streams above are already gone, so each remove_card's
       * cascade is a no-op). Runs before driver->terminate()/bye(), so the
       * driver is still alive to service the removals. status.json was already
       * captured by main's save_status() before this. */
      for (const auto& card : get_cards()) {
        remove_card(card.handle);
      }
      return ret;
    }
    return true;
  }

  std::error_code add_source(const StreamSource& source);
  std::error_code get_source(uint8_t id, StreamSource& source) const;
  std::list<StreamSource> get_sources() const;
  std::error_code get_source_sdp(uint32_t id, std::string& sdp) const;
  std::error_code remove_source(uint32_t id);
  uint8_t get_source_id(const std::string& name) const;

  enum class SourceObserverType { add_source, remove_source, update_source };
  using SourceObserver = std::function<
      bool(uint8_t id, const std::string& name, const std::string& sdp)>;
  void add_source_observer(SourceObserverType type, const SourceObserver& cb);

  enum class SinkObserverType { add_sink, remove_sink };
  using SinkObserver = std::function<bool(uint8_t id, const std::string& name)>;
  void add_sink_observer(SinkObserverType type, const SinkObserver& cb);

  using PtpStatusObserver = std::function<bool(const std::string& status)>;
  void add_ptp_status_observer(const PtpStatusObserver& cb);

  std::error_code add_sink(const StreamSink& sink);
  std::error_code get_sink(uint8_t id, StreamSink& sink) const;
  std::list<StreamSink> get_sinks() const;
  std::error_code get_sink_status(uint32_t id, SinkStreamStatus& status) const;
  std::error_code remove_sink(uint32_t id);
  uint8_t get_sink_id(const std::string& name) const;

  /* W10.2 runtime cards + pcms. A Card is a named container (allocate a free
   * handle, bring it up empty); Pcms are added to it. PCM add/remove/re-rate all
   * go through recreate-card (the kernel requires PCMs added before
   * register_card): the owning card is rebuilt with the new PCM set and its
   * bound streams re-established (surviving PCMs keep their pcm_id, so stream FKs
   * stay valid; a stream that no longer fits — e.g. a sink whose SDP rate != its
   * pcm's new rate — is dropped + logged). All best-effort: a failed rebuild
   * leaves the card removed. Persistence is shutdown-only (matches streams). */
  std::error_code add_card(const Card& card);
  std::error_code remove_card(uint8_t handle);
  /* card-level edit: rename and/or re-domain a card (recreates it, keeping its
   * pcms + re-establishing bound streams). new_name must be non-empty + unique. */
  std::error_code update_card(const std::string& name,
                              const std::string& new_name, uint8_t new_domain);
  std::list<Card> get_cards() const;
  std::error_code get_card(uint8_t handle, Card& card) const;
  std::error_code get_card_by_name(const std::string& name, Card& card) const;

  std::error_code add_pcm(const std::string& card_name, const Pcm& pcm);
  std::error_code remove_pcm(const std::string& card_name,
                             const std::string& pcm_name);
  std::error_code update_pcm(const std::string& card_name,
                             const std::string& pcm_name,
                             const Pcm& new_params);
  std::list<Pcm> get_pcms() const;
  std::error_code get_pcm_by_name(const std::string& card_name,
                                  const std::string& pcm_name, Pcm& pcm) const;
  /* the ALSA per-card device index (the Dev in hw:<card>,<Dev>) for a pcm_id =
   * its rank (by pcm_id) among its card's pcms. -1 if the pcm_id is unknown. */
  int device_index_of(uint8_t pcm_id) const;

  std::error_code set_ptp_config(const PTPConfig& config);
  void get_ptp_config(PTPConfig& config) const;
  void get_ptp_status(PTPStatus& status) const;
  /* W11: per-domain status for the active domains (the Clocks view). */
  void get_ptp_status_by_domain(std::map<uint8_t, PTPStatus>& status) const;
  /* #22: per-PCM TIC-engine lock (keyed by pcm_id) for the Cards per-PCM dots. */
  void get_pcm_clocks(std::map<uint8_t, std::string>& status) const;

  bool load_status();
  bool save_status() const;

  size_t process_sap();

 protected:
  constexpr static const char ptp_primary_mcast_addr[] = "224.0.1.129";
  constexpr static const char ptp_pdelay_mcast_addr[] = "224.0.1.107";

  std::list<StreamSink> get_updated_sinks(
      const std::list<RemoteSource>& sources_list);
  void update_sinks();

  void on_add_source(const StreamSource& source, const StreamInfo& info);
  void on_remove_source(const StreamInfo& info);

  void on_add_sink(const StreamSink& sink, const StreamInfo& info);
  void on_remove_sink(const StreamInfo& info);

  void on_ptp_status_changed(const std::string& status) const;

  void on_update_sources();

  std::string get_removed_source_sdp_(uint32_t id,
                                      uint32_t src_addr,
                                      uint32_t session_id,
                                      uint32_t session_version) const;
  std::string get_source_sdp_(uint32_t id, const StreamInfo& info) const;
  StreamSource get_source_(uint8_t id, const StreamInfo& info) const;
  StreamSink get_sink_(uint8_t id, const StreamInfo& info) const;

  /* W10.2 card/pcm helpers. The alloc_ helpers scan for the lowest free slot
   * (caller holds cards_mutex_); -1 when full. bring_up_card_ brings a card up
   * with its full pcm set (add_card -> add_pcm_to_card x N -> register_card),
   * using the handles/pcm_ids as given (caller holds cards_mutex_; inserts into
   * cards_/pcms_ on success). recreate_card_ rebuilds a card to match a new pcm
   * set and re-establishes its bound streams (the generic engine behind
   * add_pcm/remove_pcm/update_pcm). seed_topology_from_config_ builds the
   * first-boot card+pcm set. pcms_of_card_ lists a card's pcms (by pcm_id). */
  int alloc_card_handle_() const;
  int alloc_pcm_id_() const;
  std::error_code bring_up_card_(const Card& card, const std::list<Pcm>& pcms);
  std::error_code recreate_card_(const std::string& card_name,
                                 const std::list<Pcm>& new_pcms,
                                 const std::string& new_name = "",
                                 int new_domain = -1);
  void seed_topology_from_config_(std::list<Card>& cards,
                                  std::list<Pcm>& pcms) const;
  std::list<Pcm> pcms_of_card_(const std::string& card_name) const;
  /* pcm resolution against the runtime pcm set (pcms_ is authoritative now —
   * NOT daemon.conf device_groups, which is only the first-boot seed). All take
   * cards_mutex_ shared; callers must NOT already hold it. */
  bool pcm_for_id_(uint8_t pcm_id, Pcm& out) const;
  bool pcm_declared_(uint8_t pcm_id) const;
  uint32_t rate_for_pcm_(uint8_t pcm_id) const;
  /* W11: the PTP clock domain a pcm's stream rides — its owning card's domain.
   * Falls back to the daemon-wide configured domain for an unresolvable pcm
   * (validation rejects those), mirroring rate_for_pcm_. */
  uint8_t domain_for_pcm_(uint8_t pcm_id) const;

  bool sink_is_still_valid(const std::string sdp,
                           const std::list<RemoteSource> sources_list) const;

  bool parse_sdp(const std::string& sdp, StreamInfo& info) const;
  bool worker();
  // singleton, use create() to build
  explicit SessionManager(std::shared_ptr<DriverManager> driver,
                          std::shared_ptr<Browser> browser,
                          std::shared_ptr<Config> config)
      : browser_(browser), driver_(driver), config_(config) {
    ptp_config_.domain = config->get_ptp_domain();
    ptp_config_.dscp = config->get_ptp_dscp();
  };

 private:
  std::shared_ptr<Browser> browser_;
  std::shared_ptr<DriverManager> driver_;
  std::shared_ptr<Config> config_;
  std::future<bool> res_;
  std::atomic_bool running_{false};

  /* current sources */
  std::map<uint8_t /* id */, StreamInfo> sources_;
  std::map<std::string, uint8_t /* id */> source_names_;
  mutable std::shared_mutex sources_mutex_;

  /* current sinks */
  std::map<uint8_t /* id */, StreamInfo> sinks_;
  std::map<std::string, uint8_t /* id */> sink_names_;
  mutable std::shared_mutex sinks_mutex_;

  /* W10.2 runtime card+pcm topology. cards_ keyed by kernel card handle; pcms_
   * keyed by global pcm_id (Pcm.card FKs its card by name). Owned here, persisted
   * to status.json. cards_mutex_ guards BOTH maps (one lock for the whole
   * topology); never held across a save_status(). */
  std::map<uint8_t /* handle */, Card> cards_;
  std::map<uint8_t /* pcm_id */, Pcm> pcms_;
  mutable std::shared_mutex cards_mutex_;

  /* current announced sources */
  std::map<uint32_t /* msg_id_hash */,
           std::tuple<uint32_t /* src_addr */,
                      uint32_t /* session_id */,
                      uint32_t /* session_version */> >
      announced_sources_;

  /* number of deletions sent for a  a deleted source */
  std::unordered_map<uint32_t /* msg_id_hash */, int /* count */>
      deleted_sources_count_;

  PTPConfig ptp_config_;
  PTPStatus ptp_status_;
  /* W11: per-domain PTP status (keyed by domain number) for the active domains
   * (distinct Card.domains), mirrored from the kernel by the worker. Feeds the
   * SDP refclk (gmid) and GET /api/ptp/domains (the Clocks view). ptp_status_
   * stays the domain-0 status for /api/ptp/status + observers. Guarded by
   * ptp_mutex_. */
  std::map<uint8_t, PTPStatus> ptp_status_by_domain_;
  /* #22: per-PCM TIC-engine lock (keyed by pcm_id), mirrored from the kernel by
   * the worker for the Cards per-PCM dots. Guarded by ptp_mutex_. */
  std::map<uint8_t, std::string> ptp_pcm_status_;
  mutable std::shared_mutex ptp_mutex_;

  std::list<SourceObserver> add_source_observers_;
  std::list<SourceObserver> remove_source_observers_;
  std::list<SourceObserver> update_source_observers_;
  std::list<PtpStatusObserver> ptp_status_observers_;
  std::list<SinkObserver> add_sink_observers_;
  std::list<SinkObserver> remove_sink_observers_;
  std::list<SinkObserver> update_sink_observers_;

  SAP sap_{config_->get_sap_mcast_addr()};
  IGMP igmp_[2];
  uint32_t last_sink_update_{0};

  /* used to handle session versioning */
  inline static std::atomic<uint32_t> g_session_version{0};
};

#endif
