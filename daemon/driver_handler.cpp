//
//  driver_handler.cpp
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

#include <algorithm>
#include <iostream>
#include <thread>

#include "log.hpp"
#include "driver_handler.hpp"

/*
void dump(const void* mem, unsigned int n) {
  const char* p = reinterpret_cast<const char*>(mem);
  for (unsigned int i = 0; i < n; i++) {
    std::cout << std::hex << int(p[i]) << " ";
  }
  std::cout << std::endl;
}
*/

bool DriverHandler::init(const Config& /* config */) {
  if (running_) {
    return true;
  }
  try {
    client_u2k_.init(nl_endpoint<nl_protocol>(0), nl_protocol(NETLINK_U2K_ID));
    client_k2u_.init(nl_endpoint<nl_protocol>(0), nl_protocol(NETLINK_K2U_ID));
    running_ = true;
    res_ = std::async(std::launch::async, &DriverHandler::event_receiver, this);
    return true;
  } catch (const boost::system::system_error& se) {
    BOOST_LOG_TRIVIAL(fatal) << "driver_handler:: init " << se.what();
    BOOST_LOG_TRIVIAL(fatal) << "Kernel module not loaded ?";
    return false;
  }
}

void DriverHandler::send(enum MT_ALSA_msg_id id,
                         NetlinkClient& client,
                         uint8_t* buffer,
                         size_t data_size,
                         const uint8_t* data) const {
  struct MT_ALSA_msg alsa_msg;
  memset(&alsa_msg, 0, sizeof(alsa_msg));
  alsa_msg.id = id;
  alsa_msg.errCode = 0;
  alsa_msg.dataSize = data_size;

  struct nlmsghdr* nlh = (struct nlmsghdr*)buffer;
  nlh->nlmsg_len =
      sizeof(struct nlmsghdr) + sizeof(struct MT_ALSA_msg) + data_size;
  nlh->nlmsg_pid = getpid();
  nlh->nlmsg_flags = 0;
  nlh->nlmsg_type = NLMSG_DONE;
  memcpy(NLMSG_DATA(nlh), &alsa_msg, sizeof(struct MT_ALSA_msg));
  if (data != nullptr && data_size > 0) {
    memcpy(reinterpret_cast<uint8_t*>(NLMSG_DATA(nlh)) +
               sizeof(struct MT_ALSA_msg),
           data, data_size);
  }

  nl_endpoint<nl_protocol> kernel_endpoint(0, 0); /* For Linux Kernel */
  client.get_socket().send_to(boost::asio::buffer(nlh, nlh->nlmsg_len),
                              kernel_endpoint);
}

bool DriverHandler::event_receiver() {
  while (running_) {
    boost::system::error_code ec;
    auto bytes =
        client_k2u_.receive(boost::asio::buffer(event_buffer_, max_payload),
                            boost::posix_time::seconds(reply_timeout_secs), ec);
    if (ec) {
      if (ec != boost::asio::error::operation_aborted) {
        BOOST_LOG_TRIVIAL(fatal)
            << "driver_handler::k2u_receive " << ec.message();
        return false;
      }
    }

    for (struct nlmsghdr* nlh = (nlmsghdr*)event_buffer_;
         NLMSG_OK(nlh, (size_t)bytes); nlh = NLMSG_NEXT(nlh, bytes)) {
      if (nlh->nlmsg_type == NLMSG_DONE) {
        struct MT_ALSA_msg* palsa_msg =
            reinterpret_cast<struct MT_ALSA_msg*> NLMSG_DATA(nlh);

        BOOST_LOG_TRIVIAL(debug)
            << "driver_handler:: received event code " << palsa_msg->id
            << " error " << palsa_msg->errCode << " data len "
            << palsa_msg->dataSize;

        if (palsa_msg->errCode == 0) {
          size_t res_size = sizeof(int32_t);
          uint8_t res[sizeof(int32_t)];
          memset(res, 0, res_size);
          on_event(palsa_msg->id, res_size, res, palsa_msg->dataSize,
                   reinterpret_cast<const uint8_t*>(palsa_msg) + data_offset);

          BOOST_LOG_TRIVIAL(debug) << "driver_handler::sending event response "
                                   << palsa_msg->id << " data len " << res_size;
          memset(response_buffer_, 0, sizeof(response_buffer_));
          try {
            send(palsa_msg->id, client_k2u_, response_buffer_, res_size, res);
          } catch (boost::system::error_code& ec) {
            BOOST_LOG_TRIVIAL(error)
                << "driver_handler::k2u_send_to " << ec.message();
            on_event_error(palsa_msg->id, DaemonErrc::send_u2k_failed);
          }
        } else {
          on_event_error(palsa_msg->id, get_driver_error(palsa_msg->errCode));
        }
      }
    }
  }
  return true;
}

void DriverHandler::stop_event_thread() {
  /* Idempotent: the atomic exchange ensures only the first caller tears down,
   * so terminate() and a subclass destructor can both call it safely. */
  if (running_.exchange(false)) {
    client_u2k_.terminate();
    client_k2u_.terminate();
    if (res_.valid())
      res_.get();  // join the event_receiver thread
  }
}

bool DriverHandler::terminate(const Config& /* config */) {
  stop_event_thread();
  return true;
}

std::error_code DriverHandler::send_command(enum MT_ALSA_msg_id id,
                                            size_t data_size,
                                            const uint8_t* data,
                                            size_t reply_size,
                                            uint8_t* reply_data) {
  std::lock_guard<std::mutex> lock{mutex_};
  /* D2: deterministic caller buffer — a short reply or an error path leaves
   * zeros, never stale bytes from a previous transaction. */
  if (reply_size && reply_data)
    memset(reply_data, 0, reply_size);
  if (data_size > max_payload) {
    BOOST_LOG_TRIVIAL(error) << "driver_handler:: cmd " << id
                             << " invalid data size " << data_size;
    return DaemonErrc::send_invalid_size;
  }

  BOOST_LOG_TRIVIAL(debug) << "driver_handler:: sending command code " << id
                           << " data len " << data_size;
  memset(command_buffer_, 0, sizeof(command_buffer_));
  try {
    send(id, client_u2k_, command_buffer_, data_size, data);
  } catch (boost::system::error_code& ec) {
    BOOST_LOG_TRIVIAL(error) << "driver_handler:: u2k_send_to " << ec.message();
    return DaemonErrc::send_u2k_failed;
  }

  BOOST_LOG_TRIVIAL(debug) << "driver_handler:: command code " << id
                           << " data len " << data_size << " sent";
  boost::system::error_code ec;
  auto bytes =
      client_u2k_.receive(boost::asio::buffer(command_buffer_, max_payload),
                          boost::posix_time::seconds(reply_timeout_secs), ec);
  if (ec) {
    BOOST_LOG_TRIVIAL(error) << "driver_handler:: u2k_receive " << ec.message();
    return DaemonErrc::receive_u2k_failed;
  }

  for (struct nlmsghdr* nlh = (nlmsghdr*)command_buffer_;
       NLMSG_OK(nlh, (size_t)bytes); nlh = NLMSG_NEXT(nlh, bytes)) {
    if (nlh->nlmsg_type == NLMSG_DONE) {
      struct MT_ALSA_msg* palsa_msg =
          reinterpret_cast<struct MT_ALSA_msg*> NLMSG_DATA(nlh);

      BOOST_LOG_TRIVIAL(debug)
          << "driver_handler:: received cmd code " << palsa_msg->id << " error "
          << palsa_msg->errCode << " data len " << palsa_msg->dataSize;

      if (id != palsa_msg->id) {
        BOOST_LOG_TRIVIAL(warning)
            << "driver_handler:: unexpected cmd response:" << "sent " << id
            << " received " << palsa_msg->id;
        return DaemonErrc::invalid_driver_response;
      }
      if (palsa_msg->errCode != 0) {
        BOOST_LOG_TRIVIAL(error) << "driver_handler:: cmd " << id
                                 << " failed with driver error "
                                 << palsa_msg->errCode;
        return get_driver_error(palsa_msg->errCode);
      }
      /* Success: hand the payload to the caller — still under the lock, so a
       * concurrent transaction can never swap it out from under us. */
      if (reply_size && reply_data) {
        size_t n = std::min(reply_size, (size_t)palsa_msg->dataSize);
        if (n)
          memcpy(reply_data,
                 reinterpret_cast<const uint8_t*>(palsa_msg) + data_offset, n);
        if ((size_t)palsa_msg->dataSize < reply_size)
          BOOST_LOG_TRIVIAL(debug)
              << "driver_handler:: cmd " << id << " short reply ("
              << palsa_msg->dataSize << " < " << reply_size
              << " bytes) — tail zeroed";
      }
      return std::error_code{};
    }
  }
  /* Datagram carried no NLMSG_DONE reply at all. The legacy path fell through
   * SILENTLY here, leaving the previous transaction's result in place — a
   * latent stale-status bug on top of the race. */
  BOOST_LOG_TRIVIAL(error) << "driver_handler:: cmd " << id
                           << " reply datagram contained no response";
  return DaemonErrc::invalid_driver_response;
}
