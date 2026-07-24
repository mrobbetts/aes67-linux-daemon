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

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <list>
#include <map>
#include <shared_mutex>
#include <thread>

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
  /* lean streamer: opt-in per sink. Only sinks with stream==true are AAC-encoded
   * and served over HTTP. The streamer captures the lowest-pcm streamed sink's
   * PCM (one capture context at a time). Optional in JSON, defaults to false. */
  bool stream{false};
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
  /* W15: how a re-rate of one of this card's PCMs reaches ALSA clients —
   * "recreate" (default: rebuild the snd_card, today's behaviour) or "in-place"
   * (re-rate the live PCM, needs a rate-following client). See
   * is_valid_rate_change_mode in config.hpp. */
  std::string rate_change_mode{"recreate"};

  friend bool operator==(const Card& a, const Card& b) {
    return a.handle == b.handle && a.name == b.name && a.domain == b.domain &&
           a.rate_change_mode == b.rate_change_mode;
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
  bool rate_follows_source{false};  // auto re-rate to match bound sink's source
  /* W15: when following, require the source to bump its SDP o= session-version
   * to count as a rate change (strict RFC 4566). Default false (lenient): follow
   * a rate change even from a source that never increments its version (e.g. the
   * macOS VAD). Only meaningful when rate_follows_source is set. */
  bool rate_follow_strict_version{false};

  friend bool operator==(const Pcm& a, const Pcm& b) {
    return a.pcm_id == b.pcm_id && a.card == b.card && a.name == b.name &&
           a.sample_rate == b.sample_rate && a.num_inputs == b.num_inputs &&
           a.num_outputs == b.num_outputs &&
           a.playout_delay == b.playout_delay &&
           a.capture_delay == b.capture_delay &&
           a.rate_follows_source == b.rate_follows_source &&
           a.rate_follow_strict_version == b.rate_follow_strict_version;
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
  /* #32: the sink's receive offset in samples (received timestamp vs local
   * playout SAC — the buffering margin; a steady drift = the sender's clock
   * is not locked to ours) plus the pcm's live rate so consumers can convert
   * to time and compute drift in ppm without a join. */
  int32_t receive_offset{0};
  uint32_t sample_rate{0};
};

struct PTPConfig {
  uint8_t domain{0};
  uint8_t dscp{0};
};

struct PTPStatus {
  std::string status;
  std::string gmid;
  int32_t jitter{0};
  /* W16 slice 3b: the DOMAIN-level clock state — clock-source health is a
   * domain fact (the GM + servo), one truth for every PCM on it: "no-signal" |
   * "acquiring" | "locked" | "saturated". This is what UIs colour by; the
   * legacy composite `status` stays for compat/observers. */
  std::string clock_state;
  /* W16 slice 3: the elected GM's Announce properties (verbatim from the wire)
   * plus the kernel servo's estimate of the GM's rate offset vs the local
   * reference (ppb, negative = GM slow; meaningful once PTP-locked). Surfaced
   * for display and judgment — never gated on. */
  int64_t gm_rate_ppb{0};
  uint8_t gm_priority1{0};
  uint8_t gm_clock_class{0};
  uint8_t gm_clock_accuracy{0};
  uint16_t gm_offset_scaled_log_variance{0};
  uint8_t gm_priority2{0};
  uint16_t gm_steps_removed{0};
  uint8_t gm_time_source{0};
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

/* W28: per-PCM runtime truth for the Cards view — the TIC-engine lock plus the
 * kernel-authoritative live + armed rate. live_rate is what the chip is keyed to
 * right now; pending_rate is an armed in-place re-rate target (0 = not armed). */
struct PcmRuntime {
  std::string tic_status;
  uint32_t live_rate{0};
  uint32_t pending_rate{0};
  /* W16 slice 3: the engine's media-clock state WITH the reason for
   * unlockedness — "stopped" | "no-signal" | "acquiring" | "locked" |
   * "saturated". Slice 3b: detail only (tooltips) — the DOMAIN composite in
   * PTPStatus is the clock-source truth UIs colour by. */
  std::string clock_state;
  /* W16 slice 3b: engine-local EXECUTION health — is this PCM's tick engine
   * meeting its own schedule? Meaningful regardless of clock state (a
   * free-wheeling engine still ticks and services clients, W17). ticking ⇔
   * us_since_last_tick ≲ a few tick_period_us. Both 0 when stopped. */
  uint32_t tick_period_us{0};
  uint32_t us_since_last_tick{0};
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
  /* teardown safety: the worker thread (res_) loops on running_; if an
   * exception unwinds main before terminate() runs, the implicit dtor would
   * join a still-looping worker -> hang -> watchdog. Stop it first, then join
   * (while running_/res_ are still alive). Idempotent with terminate(). */
  virtual ~SessionManager() {
    running_ = false;
    wake_worker_();  // W15: unblock the worker's re-rate-event wait at once
    if (res_.valid())
      res_.wait();
  }

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
      wake_worker_();  // W15: unblock the worker's re-rate-event wait at once
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
  /* card-level edit: rename, re-domain, and/or change the rate-change mode
   * (recreates it, keeping its pcms + re-establishing bound streams). new_name
   * must be non-empty + unique; new_mode must be a valid rate-change mode. */
  std::error_code update_card(const std::string& name,
                              const std::string& new_name, uint8_t new_domain,
                              const std::string& new_mode);
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
  /* #22 + W28: per-PCM runtime (keyed by pcm_id) for the Cards view — TIC lock +
   * kernel-truth live/armed rate. */
  void get_pcm_clocks(std::map<uint8_t, PcmRuntime>& status) const;

  bool load_status();
  bool save_status() const;
  /* D7 (2026-06 audit): persistence used to be shutdown-only — a crash lost
   * every topology change since boot. Mutators mark the status dirty; the
   * worker saves (atomically) within ~1 s. Cheap, single-writer, crash-window
   * shrinks from "since boot" to "since the last mutation, ≤1 s". */
  void mark_status_dirty() { status_dirty_.store(true, std::memory_order_release); }

  size_t process_sap();

 protected:
  constexpr static const char ptp_primary_mcast_addr[] = "224.0.1.129";
  constexpr static const char ptp_pdelay_mcast_addr[] = "224.0.1.107";

  std::list<StreamSink> get_updated_sinks(
      const std::list<RemoteSource>& sources_list);
  /* W15 rate-follow: sinks whose rate_follows_source pcm must re-rate because a
   * discovered same-origin source now advertises a different sample rate —
   * independent of the SDP o= session-version. Catches sources (e.g. the VAD)
   * that change rate without bumping the version, which get_updated_sinks
   * correctly ignores. Flap-free: converges once pcm rate == source rate. */
  std::list<StreamSink> get_rate_follow_sinks(
      const std::list<RemoteSource>& sources_list);
  /* D10 (audit M7): update_sinks is a sequence of named phases — cancel
   * reverted re-rates, collect the changed sinks, apply each — with the
   * per-sink POLICY extracted into a pure decision function (see
   * decide_rerate in the cpp) and the effects in apply_sink_update_. All
   * worker-thread only, like the monolith they replace. */
  void update_sinks();
  void cancel_reverted_rerates_(const std::list<RemoteSource>& remote_sources);
  std::list<StreamSink> collect_sink_updates_(
      const std::list<RemoteSource>& remote_sources);
  void apply_sink_update_(StreamSink& sink);

  /* D4: the on_* hooks do the MODEL work (name maps, IGMP) under the caller's
   * lock and RETURN the observer notification as a closure over value-captured
   * payloads. Callers collect these in a DeferredNotifications guard declared
   * before their unique_lock, so notifications always fire after the lock is
   * released — structurally, on every return path, instead of by the "observers
   * must only signal" convention that produced the EDEADLK crash-loop and let
   * a blocking observer (avahi) stall every reader of the maps. */
  std::function<void()> on_add_source(const StreamSource& source,
                                      const StreamInfo& info);
  std::function<void()> on_remove_source(const StreamInfo& info);

  std::function<void()> on_add_sink(const StreamSink& sink,
                                    const StreamInfo& info);
  std::function<void()> on_remove_sink(const StreamInfo& info);

  void on_ptp_status_changed(const std::string& status) const;

  void on_update_sources();
  /* W15: after pcm_id re-rates in place, push a fresh ANNOUNCE (bumped o=
   * version, SDP regenerated at the new rate) for any source on that pcm — so
   * RTSP/SAP receivers (e.g. a Hapi) re-pull and auto-follow the new rate
   * instead of dropping until manually re-selected. Targeted variant of
   * on_update_sources (which re-announces every source on a GMID change). */
  void reannounce_pcm_sources_(uint8_t pcm_id);

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
   * add_pcm/remove_pcm/update_pcm). pcms_of_card_ lists a card's pcms (by
   * pcm_id). */
  int alloc_card_handle_() const;
  int alloc_pcm_id_() const;
  std::error_code bring_up_card_(const Card& card, const std::list<Pcm>& pcms);
  std::error_code recreate_card_(const std::string& card_name,
                                 const std::list<Pcm>& new_pcms,
                                 const std::string& new_name = "",
                                 int new_domain = -1,
                                 const std::string& new_mode = "");
  std::list<Pcm> pcms_of_card_(const std::string& card_name) const;
  /* pcm resolution against the runtime pcm set (pcms_ is authoritative now —
   * NOT daemon.conf device_groups, which is only the first-boot seed). All take
   * cards_mutex_ shared; callers must NOT already hold it. */
  bool pcm_for_id_(uint8_t pcm_id, Pcm& out) const;
  bool pcm_declared_(uint8_t pcm_id) const;
  uint32_t rate_for_pcm_(uint8_t pcm_id) const;
  /* W15 in-place re-rate (rate_change_mode == "in-place"): update only the
   * daemon's model rate for a PCM (the kernel already re-keyed via SetPCMRate);
   * no card recreate. */
  void set_pcm_rate_model_(uint8_t pcm_id, uint32_t new_rate);
  /* W15: on the worker, wait (briefly) for the kernel's PCMRateApplied K2U events
   * (an ARMED re-rate that applied autonomously on the client's last close), then
   * re-attach the deferred sink for each, and GC a pending whose event never
   * arrives. The events are delivered into rerate_events_ by the driver's
   * event-thread handler (signal-and-defer; it never touches `this`). */
  void process_rerate_events_();
  /* D5 (2026-06 audit): ONE convergence routine for "the kernel's live rate
   * changed": sync the model, re-announce (o= bump) so downstream re-pulls,
   * and re-attach a parked sink if one is pending. Called from the apply-event
   * path AND the status-poll backstop — a lost event can no longer leave SAP
   * announcing a new rate under a stale version. Worker thread only. */
  void reconcile_rate_applied_(uint8_t pcm_id, uint32_t rate);
  // W15: wake the worker if it's parked in process_rerate_events_'s cv wait.
  void wake_worker_() {
    {
      std::lock_guard<std::mutex> lk(rerate_events_->mtx);
      rerate_events_->stopping = true;
    }
    rerate_events_->cv.notify_all();
  }
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

  /* W15 in-place re-rate: when SetPCMRate is ARMED (the kernel couldn't apply it
   * because a client still holds the PCM open), the sink re-add is parked here
   * until the kernel applies the re-rate on the client's last close and fires a
   * PCMRateApplied event; the worker then re-attaches this sink (see
   * process_rerate_events_). A pending is valid for as long as the client holds
   * the device — there is NO timeout: abandoning it would drop the sink re-attach
   * AND make us discard the kernel's authoritative rate when the event arrives
   * (that was the daemon<->kernel desync bug). Bounded to one entry per pcm by
   * the dedup at arm time. Touched only by the worker thread (no lock). */
  struct PendingRerate {
    uint8_t pcm_id;
    uint32_t new_rate;
    StreamSink sink;  // re-added at the new rate once the re-rate applies
  };
  std::list<PendingRerate> pending_rerates_;   // worker-thread only

  /* D7: set by any topology mutator (REST threads or worker), consumed by the
   * worker's save pass. */
  mutable std::atomic<bool> status_dirty_{false};
  /* PCMRateApplied events. Lives in its OWN heap object so the driver's
   * event-thread handler (which captures a shared_ptr copy) can safely outlive
   * this SessionManager — it only ever touches this queue, never `this`. The
   * worker waits on the cv, so the sink re-attach is immediate (no polling). */
  struct RerateEvents {
    std::mutex mtx;
    std::condition_variable cv;
    std::list<std::pair<uint8_t /*pcm_id*/, uint32_t /*rate*/>> applied;
    bool stopping{false};
  };
  std::shared_ptr<RerateEvents> rerate_events_{std::make_shared<RerateEvents>()};

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
   * the worker for the Cards per-PCM dots. W16 slice 3: the mirror carries both
   * the legacy tic_status and the canonical clock_state (the reason-bearing
   * enum, stringified). Guarded by ptp_mutex_. */
  struct PcmClockMirror {
    std::string tic_status;
    std::string clock_state;
    uint32_t tick_period_us{0};       /* W16 3b: engine execution health */
    uint32_t us_since_last_tick{0};
  };
  std::map<uint8_t, PcmClockMirror> ptp_pcm_status_;
  mutable std::shared_mutex ptp_mutex_;
  /* W28 (intent-in/truth-out): the kernel is the source of truth for the LIVE
   * chip rate and the ARMED re-rate target. Mirrored lock-free from
   * GetPCMStatus (worker status poll) + PCMRateApplied (immediate) so
   * rate_for_pcm_ reads kernel truth rather than a speculative cache — this is
   * what makes the daemon<->kernel rate desync structurally impossible. 0 = not
   * yet mirrored (rate_for_pcm_ falls back to the configured/intent rate, e.g.
   * the startup window before the first poll, or the fake driver). Indexed by
   * pcm_id; reads are load-acquire, writes store-release (no lock, so safe to
   * read from rate_for_pcm_ which runs under ptp_mutex_/cards_mutex_). */
  std::array<std::atomic<uint32_t>, pcm_id_max> pcm_live_rate_{};
  std::array<std::atomic<uint32_t>, pcm_id_max> pcm_pending_rate_{};

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
