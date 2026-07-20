//  rtsp_server.cpp
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

#include "utils.hpp"
#include "rtsp_server.hpp"

using boost::asio::ip::tcp;

bool RtspServer::update_source(uint8_t id,
                               const std::string& name,
                               const std::string& sdp) {
  BOOST_LOG_TRIVIAL(debug) << "rtsp_server:: added source " << name;
  std::lock_guard<std::mutex> lock{mutex_};
  for (unsigned int i = 0; i < sessions_.size(); i++) {
    auto session = sessions_[i].lock();
    if (session != nullptr) {
      /* D9: posts onto the io thread; whether this session described the
       * source is decided THERE (io-thread-only state). */
      session->announce(id, name, sdp, config_->get_ip_addr_str(),
                        config_->get_rtsp_port());
    }
  }
  return true;
}

void RtspServer::accept() {
  acceptor_.async_accept(socket_, [this](boost::system::error_code ec) {
    if (!ec) {
      /* D9: keepalive so the OS eventually errors reads on half-open peers
       * (a power-cycled device that never sent FIN/RST) — the error completes
       * the read chain and releases the session. */
      boost::system::error_code opt_ec;
      socket_.set_option(boost::asio::socket_base::keep_alive(true), opt_ec);

      std::lock_guard<std::mutex> lock{mutex_};
      /* check for free sessions */
      unsigned int i = 0;
      for (; i < sessions_.size(); i++) {
        if (sessions_[i].use_count() == 0) {
          break;
        }
      }

      /* D9: table full — evict the LEAST-RECENTLY-ACTIVE session instead of
       * refusing the newcomer. Slots used to leak one per dead/idle
       * connection forever (nothing ever reaped), so a 24/7 fabric with
       * reconnecting devices eventually exhausted all slots and new
       * DESCRIBEs were closed at accept: a slow, silent outage of the very
       * channel rate-follow rides. A live peer has recent reads/announces
       * and is never the eviction victim in practice. */
      if (i == sessions_.size()) {
        unsigned int oldest = 0;
        auto oldest_seen = steady_clock::time_point::max();
        for (unsigned int j = 0; j < sessions_.size(); j++) {
          auto session = sessions_[j].lock();
          if (session && session->last_activity() < oldest_seen) {
            oldest_seen = session->last_activity();
            oldest = j;
          }
        }
        if (auto session = sessions_[oldest].lock()) {
          BOOST_LOG_TRIVIAL(warning)
              << "rtsp_server:: session table full — evicting the least "
                 "recently active session (slot "
              << oldest << ") for a new connection";
          session->stop();
        }
        i = oldest;
      }

      auto session = std::make_shared<RtspSession>(config_, session_manager_,
                                                   std::move(socket_));
      sessions_[i] = session;
      session->start();
    }
    accept();
  });
}

/* D9: any-thread entry point — hop onto the io executor; ALL session state
 * (source_ids_, cseq_, the socket, the write queue) is io-thread-only. The
 * old direct call raced process_request from the observer thread and was
 * silently DROPPED whenever a request happened to be mid-parse. */
void RtspSession::announce(uint8_t id,
                           const std::string& name,
                           const std::string& sdp,
                           const std::string& address,
                           uint16_t port) {
  auto self(shared_from_this());
  boost::asio::post(socket_.get_executor(),
                    [self, id, name, sdp, address, port]() {
                      self->announce_io(id, name, sdp, address, port);
                    });
}

void RtspSession::announce_io(uint8_t id,
                              const std::string& name,
                              const std::string& sdp,
                              const std::string& address,
                              uint16_t port) {
  /* send only where this source was described on this session */
  if (source_ids_.find(id) == source_ids_.end()) {
    return;
  }
  std::string path(std::string("/by-name/") + config_->get_node_id() + " " +
                   name);
  std::stringstream ss;
  ss << "ANNOUNCE rtsp://" << address << ":" << std::to_string(port)
     << httplib::detail::encode_url(path) << " RTSP/1.0\r\n"
     << "User-Agent: aes67-daemon\r\n"
     << "connection: Keep-Alive" << "\r\n"
     << "CSeq: " << announce_cseq_++ << "\r\n"
     << "Content-Length: " << sdp.length() << "\r\n"
     << "Content-Type: application/sdp\r\n"
     << "\r\n"
     << sdp;

  BOOST_LOG_TRIVIAL(info) << "rtsp_server:: " << "ANNOUNCE for source " << name
                          << " queued to " << peer_str();

  send_response(ss.str());
}

bool RtspSession::process_request() {
  /*
     DESCRIBE rtsp://127.0.0.1:8080/by-name/test RTSP/1.0
     CSeq: 312
     User-Agent: pippo
     Accept: application/sdp

  */
  data_[length_] = 0;
  std::stringstream sstream(data_);
  /* read the request */
  if (!getline(sstream, request_, '\n')) {
    return false;
  }
  consumed_ = request_.length() + 1;
  boost::trim(request_);
  std::vector<std::string> fields;
  split(fields, request_, boost::is_any_of(" "));
  if (fields.size() < 3) {
    return false;
  }
  /* read the header */
  bool is_end{false};
  std::string header;
  while (getline(sstream, header, '\n')) {
    consumed_ += header.length() + 1;
    if (header == "" || header == "\r") {
      is_end = true;
      break;
    }
    boost::to_lower(header);
    boost::trim(header);
    if (header.rfind("cseq:", 0) != std::string::npos) {
      try {
        cseq_ = stoi(header.substr(5));
      } catch (...) {
        break;
      }
    }
  }

  if (!is_end) {
    return false;
  }

  if (fields[0].substr(0, 5) == "RTSP/") {
    /* we received a response, step to next request*/
    return true;
  } else if (cseq_ < 0) {
    BOOST_LOG_TRIVIAL(error) << "rtsp_server:: CSeq not specified from "
                             << peer_str();
    send_error(400, "Bad Request");
  } else if (fields[2].substr(0, 5) != "RTSP/") {
    BOOST_LOG_TRIVIAL(error)
        << "rtsp_server:: no RTSP specified from " << peer_str();
    send_error(400, "Bad Request");
  } else if (fields[0] != "DESCRIBE") {
    send_error(405, "Method Not Allowed");
  } else {
    boost::trim(fields[1]);
    build_response(fields[1]);
  }
  return true;
}

void RtspSession::build_response(const std::string& url) {
  auto const res = parse_url(url);
  if (!std::get<0>(res)) {
    BOOST_LOG_TRIVIAL(error) << "rtsp_server:: cannot parse URL " << url
                             << " from " << peer_str();
    send_error(400, "Bad Request");
    return;
  }
  const auto& path = std::get<4>(res);
  auto base_path = std::string("/by-name/") + config_->get_node_id() + " ";
  uint8_t id = SessionManager::stream_id_max + 1;
  if (path.rfind(base_path) != std::string::npos) {
    /* extract the source name from path and retrive the id */
    id = session_manager_->get_source_id(path.substr(base_path.length()));
  } else if (path.rfind("/by-id/") != std::string::npos) {
    try {
      id = (uint8_t)stoi(path.substr(7));
    } catch (...) {
      id = SessionManager::stream_id_max + 1;
      ;
    }
  }
  if (id != (SessionManager::stream_id_max + 1)) {
    std::string sdp;
    if (!session_manager_->get_source_sdp(id, sdp)) {
      std::stringstream ss;
      ss << "RTSP/1.0 200 OK\r\n"
         << "CSeq: " << cseq_ << "\r\n"
         << "Content-Length: " << sdp.length() << "\r\n"
         << "Content-Type: application/sdp\r\n"
         << "\r\n"
         << sdp;
      BOOST_LOG_TRIVIAL(info)
          << "rtsp_server:: " << request_ << " response 200 to "
          << peer_str();
      send_response(ss.str());
      source_ids_.insert(id);
      return;
    }
  }
  send_error(404, "Not found");
}

void RtspSession::read_request() {
  auto self(shared_from_this());
  if (length_ == max_length) {
    /* request cannot be consumed and we exceeded max length */
    stop();
  } else {
    socket_.async_read_some(
        boost::asio::buffer(data_ + length_, max_length - length_),
        [this, self](boost::system::error_code ec, std::size_t length) {
          if (!ec) {
            BOOST_LOG_TRIVIAL(debug) << "rtsp_server:: received " << length
                                     << " from " << peer_str();
            touch();  /* D9: liveness for the eviction policy */
            length_ += length;
            while (length_ && process_request()) {
              /* step to the next request */
              std::memmove(data_, data_ + consumed_, length_ - consumed_);
              length_ -= consumed_;
              cseq_ = -1;
            }
            /* read more data */
            read_request();
          }
        });
  }
}

void RtspSession::send_error(int status_code, const std::string& description) {
  BOOST_LOG_TRIVIAL(error) << "rtsp_server:: " << request_ << " response "
                           << status_code << " to "
                           << peer_str();
  std::stringstream ss;
  ss << "RTSP/1.0 " << status_code << " " << description << "\r\n";
  if (cseq_ >= 0) {
    ss << "CSeq: " << cseq_ << "\r\n";
  }
  ss << "\r\n";
  send_response(ss.str());
}

/* D9 (io-thread only): per-session write queue. Exactly one async_write is
 * ever in flight per socket — two concurrent composed writes interleave their
 * partial writes and corrupt the byte stream (a DESCRIBE response racing an
 * ANNOUNCE, or two rapid announces from a re-rate reannounce sweep). */
void RtspSession::send_response(std::string response) {
  write_queue_.push_back(std::move(response));
  if (write_queue_.size() == 1) {
    write_next();
  }
}

void RtspSession::write_next() {
  auto self(shared_from_this());
  boost::asio::async_write(
      socket_, boost::asio::buffer(write_queue_.front()),
      [this, self](boost::system::error_code ec, std::size_t /*length*/) {
        if (ec) {
          /* D9: a failed write = a dead peer — tear the session down so its
           * slot frees, instead of leaving a zombie holding it. */
          BOOST_LOG_TRIVIAL(debug)
              << "rtsp_server:: write failed (" << ec.message()
              << "), closing session";
          stop();
          return;
        }
        touch();
        write_queue_.pop_front();
        if (!write_queue_.empty()) {
          write_next();
        }
      });
}

std::string RtspSession::peer_str() {
  boost::system::error_code ec;
  auto ep = socket_.remote_endpoint(ec);
  if (ec) {
    return "<disconnected>";
  }
  std::stringstream ss;
  ss << ep;
  return ss.str();
}

void RtspSession::start() {
  BOOST_LOG_TRIVIAL(debug) << "rtsp_server:: starting session with "
                           << peer_str();
  read_request();
}

void RtspSession::stop() {
  BOOST_LOG_TRIVIAL(debug) << "rtsp_server:: stopping session with "
                           << peer_str();
  boost::system::error_code ec;
  socket_.close(ec);
}
