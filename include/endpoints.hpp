#ifndef ENDPOINTS_HPP_
#define ENDPOINTS_HPP_

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstring>
#include <format>
#include <fstream>
#include <optional>
#include <thread>

#include "abstract.hpp"
#include "packet.hpp"
#include "utils.hpp"

/// Endpoint instance of a connection, server of client.
struct Endpoint {
  enum class Role {
    /// Sender endpoint
    Sender = 1,
    /// Receiver endpoint
    Receiver = 2,
  };

  enum class State {
    /// Idle
    Idle = 1,
    /// Connecting
    Connecting = 2,
    /// Connected
    Connected = 3,
    /// Closing
    Closing = 4,
  };

  Endpoint() : active(false) {}

  /// Initialize the socket.
  void init(uint16_t port, Role role);

  void show_info();

  /// Connection process.
  ///
  /// Simple 4-way handshake:
  /// 1. Sender sends a connection request.
  /// 2. Receiver sends a connection response.
  /// Each side sends a connection request and receives a connection response.
  /// Just to make sure that the connection is established.
  void connect();

  void set_remote(const std::string& ip, uint16_t port);

  /// Handler to receive a packet.
  void recv_handler();

  /// Send a file.
  void send_file(const std::string& filename);

  /// Set the timeout.
  void set_timeout(std::chrono::milliseconds timeout) {
    this->timeout = timeout;
  }

  /// Run the endpoint with a repl.
  void run();

  /// Cleanup the resources.
  void cleanup();

 private:
  /// Address of this endpoint
  std::optional<struct sockaddr_in> addr;
  /// Remote address
  std::optional<struct sockaddr_in> remote_addr;
  /// If the endpoint is active
  std::atomic<bool> active;
  /// Last seq, because this is a stop-and-wait protocol
  std::atomic<uint32_t> last_seq;
  /// Waiting for ack
  std::atomic<bool> waiting_for_ack;
  /// If require a resend
  std::atomic<bool> require_resend;
  /// Require resend condition variable
  std::condition_variable require_resend_cv;
  /// The type of the endpoint
  Role role;
  /// The state of the endpoint
  State state;
  /// The file
  std::fstream file;
  /// The socket
  std::optional<socket_t> socket;
  /// Timeout
  std::chrono::milliseconds timeout;
  /// Sending tunnel synchronization
  std::mutex send_mutex;

  std::thread recv_thread;
};

void Endpoint::init(uint16_t port, Role role) {
#ifdef _WIN32
  WSADATA wsa_data;
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    log(
      std::format("failed to initialize Winsock: {}", WSAGetLastError()),
      LogLevel::Error
    );
    throw std::runtime_error("failed to initialize Winsock");
  }
#endif

  this->role = role;
  this->state = State::Idle;

  if (this->socket.has_value()) {
    log("socket already initialized", LogLevel::Error);
    throw std::runtime_error("socket already initialized");
  }

  this->socket = create_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (!this->socket.has_value()) {
    log("failed to create socket", LogLevel::Error);
    throw std::runtime_error("failed to create socket");
  }

  this->addr = std::make_optional<struct sockaddr_in>();

  this->addr.value().sin_family = AF_INET;
  this->addr.value().sin_port = htons(port);
  this->addr.value().sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(this->socket.value(), (struct sockaddr*)&this->addr.value(), sizeof(this->addr.value())) < 0) {
    log("failed to bind socket", LogLevel::Error);
    throw std::runtime_error("failed to bind socket");
  }

  this->active = true;

  this->last_seq = 0;
  this->waiting_for_ack = false;
  this->require_resend = false;

  this->timeout = std::chrono::milliseconds(1000);

  this->state = Endpoint::State::Idle;

  log("endpoint initialized", LogLevel::Info);
}

void Endpoint::show_info() {
  log("endpoint info:", LogLevel::Info);
  log(
    std::format(
      "  type: {}", this->role == Endpoint::Role::Sender ? "sender" : "receiver"
    ),
    LogLevel::Info
  );

  char hostname[256];
  gethostname(hostname, sizeof(hostname));

  struct addrinfo hints = {};
  struct addrinfo* addrs;

  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;

  getaddrinfo(hostname, NULL, &hints, &addrs);

  char ip[16];
  inet_ntop(
    AF_INET, &((struct sockaddr_in*)addrs->ai_addr)->sin_addr, ip, sizeof(ip)
  );

  log(
    std::format("  address: {}:{}", ip, ntohs(this->addr.value().sin_port)),
    LogLevel::Info
  );

  // show remote
  if (this->remote_addr.has_value()) {
    inet_ntop(AF_INET, &this->remote_addr.value().sin_addr, ip, sizeof(ip));
    log(
      std::format(
        "  remote: {}:{}", ip, ntohs(this->remote_addr.value().sin_port)
      ),
      LogLevel::Info
    );
  }

  freeaddrinfo(addrs);
}

void Endpoint::cleanup() {
  this->active = false;

  if (this->recv_thread.joinable()) {
    this->recv_thread.join();
  }

  if (this->socket.has_value()) {
#ifdef _WIN32
    closesocket(this->socket.value());
#else
    close(this->socket.value());
#endif
  }
#ifdef _WIN32
  WSACleanup();
#endif
}

void Endpoint::set_remote(const std::string& ip, uint16_t port) {
  this->remote_addr = std::make_optional<struct sockaddr_in>();

  this->remote_addr.value().sin_family = AF_INET;
  this->remote_addr.value().sin_port = htons(port);
  this->remote_addr.value().sin_addr.s_addr = inet_addr(ip.c_str());
}

void Endpoint::connect() {
  if (this->role == Endpoint::Role::Sender) {
    // Send a CONN_REQ, try to receive a CONN_ACK (resend if timeout, up to
    // RDT_MAX_RETRIEs). Use a thread to do the timing,

    if (this->state != Endpoint::State::Idle) {
      log("cannot propose a connect from non-idle state", LogLevel::Error);
      throw std::runtime_error("cannot propose a connect from non-idle state");
    }

    this->state = Endpoint::State::Connecting;

    // send connection request
    RdtHeader conn_req_header = {
      .version = RDT_VERSION,
      .reserved = 0,
      .number = 1,
      .checksum = 0,
    };

    RdtCommonFrameHeader conn_req_frame = {
      .type = RdtFrameType::CONN_REQ,
      .seq = 0,
    };

    uint8_t conn_req_buffer[1024];

    assert(
      sizeof(conn_req_header) + sizeof(conn_req_frame) <=
      sizeof(conn_req_buffer)
    );

    memcpy(conn_req_buffer, &conn_req_header, sizeof(conn_req_header));
    memcpy(
      conn_req_buffer + sizeof(conn_req_header), &conn_req_frame,
      sizeof(conn_req_frame)
    );

    // checksum
    ((RdtHeader*)conn_req_buffer)->checksum =
      rdt_packet_checksum(conn_req_buffer);

    // occupy the socket
    this->send_mutex.lock();

    if (sendto(this->socket.value(), (char*)conn_req_buffer, sizeof(conn_req_buffer), 0, (struct sockaddr*)&this->remote_addr.value(), sizeof(this->remote_addr.value())) < 0) {
      log("failed to send connection request", LogLevel::Error);
      throw std::runtime_error("failed to send connection request");
    }

    log("sent connection request", LogLevel::Info);

    // release the socket
    this->send_mutex.unlock();

    size_t retries = 0;
    while (retries < RDT_MAX_RETRIES) {
      // wait for ACK
      std::unique_lock<std::mutex> lock(this->send_mutex);
      this->waiting_for_ack = true;
      this->require_resend_cv.wait_for(lock, this->timeout, [this]() {
        return !this->waiting_for_ack;
      });

      if (this->require_resend) {
        // resend because of inconsistent seq
        if (sendto(this->socket.value(), (char*)conn_req_buffer, sizeof(conn_req_buffer), 0, (struct sockaddr*)&this->remote_addr.value(), sizeof(this->remote_addr.value())) < 0) {
          log("failed to resend connection request", LogLevel::Error);
          throw std::runtime_error("failed to resend connection request");
        }

        log("resend connection request", LogLevel::Info);

        retries++;
      } else if (this->waiting_for_ack) {
        // resend because of timeout
        if (sendto(this->socket.value(), (char*)conn_req_buffer, sizeof(conn_req_buffer), 0, (struct sockaddr*)&this->remote_addr.value(), sizeof(this->remote_addr.value())) < 0) {
          log("failed to resend connection request", LogLevel::Error);
          throw std::runtime_error("failed to resend connection request");
        }

        log("resend connection request", LogLevel::Info);

        retries++;
      } else {
        // received ACK
        break;
      }
    }

    if (this->state != Endpoint::State::Connected) {
      log("failed to connect", LogLevel::Error);
      throw std::runtime_error("failed to connect");
    } else {
      // show retries
      log(
        std::format("connection established after {} retries", retries),
        LogLevel::Info
      );
    }
  } else {
    log("cannot connect from receiver endpoint", LogLevel::Error);
    throw std::runtime_error("Cannot connect from receiver endpoint");
  }
}

void Endpoint::send_file(const std::string& filename) {
  if (this->role == Endpoint::Role::Receiver) {
    log("cannot send file from receiver endpoint", LogLevel::Error);
    throw std::runtime_error("Cannot send file from receiver endpoint");
  }

  if (!this->socket.has_value()) {
    log("socket not initialized", LogLevel::Error);
    throw std::runtime_error("socket not initialized");
  }
}

void Endpoint::recv_handler() {
  struct timeval recv_timeout;
  recv_timeout.tv_sec = 1;
  recv_timeout.tv_usec = 0;

  if (setsockopt(this->socket.value(), SOL_SOCKET, SO_RCVTIMEO, (char*)&recv_timeout, sizeof(recv_timeout)) < 0) {
    log("failed to set socket timeout", LogLevel::Error);
    throw std::runtime_error("failed to set socket timeout");
  }

  uint8_t buffer[1024];

  int addr_len = sizeof(struct sockaddr);

  while (this->active) {
    int recv_size = recvfrom(
      this->socket.value(), (char*)buffer, sizeof(buffer), 0,
      (struct sockaddr*)&this->remote_addr.value(),
      (socklen_t*)&addr_len
    );

    if (recv_size < 0) {
#ifdef _WIN32
      if (WSAGetLastError() == WSAETIMEDOUT) {
        continue;
      }
#else
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
#endif

#ifdef _WIN32
      log(
        std::format("failed to receive packet: {}", WSAGetLastError()),
        LogLevel::Error
      );
#else
      log(
        std::format("failed to receive packet: {}", strerror(errno)),
        LogLevel::Error
      );
#endif
      throw std::runtime_error("failed to receive packet");
    }

    if (recv_size < sizeof(RdtHeader)) {
      log("received packet too small", LogLevel::Warn);
      continue;
    }

    RdtHeader* header = (RdtHeader*)buffer;

    if (header->version != RDT_VERSION) {
      log("received packet with inconsistent version", LogLevel::Warn);
    }

    if (header->reserved != 0) {
      log("received packet with non-zero reserved field", LogLevel::Warn);
    }

    if (header->number == 0) {
      log("received packet with zero frame number", LogLevel::Warn);
    }

    // check validity
    if (!rdt_packet_valid(buffer)) {
      log("received packet with invalid checksum", LogLevel::Warn);
      continue;
    }

    size_t offset = sizeof(RdtHeader);

    for (size_t i = 0; i < header->number; i++) {
      RdtCommonFrameHeader* frame_header =
        (RdtCommonFrameHeader*)(buffer + offset);

      if (frame_header->type == RdtFrameType::DATA) {
        RdtDataFrameHeader* frame_header =
          (RdtDataFrameHeader*)(buffer + offset);

        // Only when a **Receiver** receives a data and the state is
        // **Connected** or **Closing**, it should send an ACK.
        // Also note that the file stream should be opened before receiving
        // data.
        if (this->role != Endpoint::Role::Receiver) {
          log("received data frame from sender endpoint", LogLevel::Warn);
        }

        if (this->state != Endpoint::State::Connected && this->state != Endpoint::State::Closing) {
          log("received data frame in invalid state", LogLevel::Warn);
        }

        if (!this->file.is_open()) {
          log("received data frame without file stream", LogLevel::Warn);
        }

        if (this->waiting_for_ack) {
          log("received data frame while waiting for ACK", LogLevel::Warn);
          offset += sizeof(RdtDataFrameHeader) + frame_header->length;
          continue;
        }

        log(
          std::format(
            "received data frame with seq {}", frame_header->common.seq
          ),
          LogLevel::Info
        );

        // write data to file (in binary)
        this->file.write(
          (char*)buffer + offset + sizeof(RdtDataFrameHeader),
          frame_header->length
        );

        // send ACK
        RdtHeader ack_header = {
          .version = RDT_VERSION,
          .reserved = 0,
          .number = 1,
          .checksum = 0,
        };

        RdtCommonFrameHeader ack_frame = {
          .type = RdtFrameType::DATA_ACK,
          .seq = frame_header->common.seq,
        };

        uint8_t ack_buffer[1024];

        assert(sizeof(ack_header) + sizeof(ack_frame) <= sizeof(ack_buffer));

        memcpy(ack_buffer, &ack_header, sizeof(ack_header));
        memcpy(ack_buffer + sizeof(ack_header), &ack_frame, sizeof(ack_frame));

        // checksum
        ((RdtHeader*)ack_buffer)->checksum = rdt_packet_checksum(ack_buffer);

        // occupy the socket
        std::lock_guard<std::mutex> lock(this->send_mutex);

        if (sendto(this->socket.value(), (char*)ack_buffer, sizeof(ack_buffer), 0, (struct sockaddr*)&this->remote_addr.value(), sizeof(this->remote_addr.value())) < 0) {
          log("failed to send data ACK", LogLevel::Error);
          throw std::runtime_error("failed to send data ACK");
        }

        log(
          std::format("sent data ACK with seq {}", ack_frame.seq),
          LogLevel::Info
        );

        offset += sizeof(RdtDataFrameHeader) + frame_header->length;

      } else if (frame_header->type == RdtFrameType::CONN_REQ) {
        if (this->role != Endpoint::Role::Receiver) {
          log(
            "received connection request from sender endpoint", LogLevel::Warn
          );

          offset += sizeof(RdtCommonFrameHeader);
          continue;
        }

        if (this->waiting_for_ack) {
          log(
            "received connection request while waiting for ACK", LogLevel::Warn
          );
          offset += sizeof(RdtCommonFrameHeader);
          continue;
        }

        uint32_t seq = frame_header->seq;

        if (this->state == Endpoint::State::Idle || this->state == Endpoint::State::Connected) {
          log(
            std::format("received connection request with seq {}", seq),
            LogLevel::Info
          );

          // send connection response
          RdtHeader conn_ack_header = {
            .version = RDT_VERSION,
            .reserved = 0,
            .number = 1,
            .checksum = 0,
          };

          RdtCommonFrameHeader conn_ack_frame = {
            .type = RdtFrameType::CONN_ACK,
            .seq = seq,
          };

          uint8_t conn_ack_buffer[1024];

          assert(
            sizeof(conn_ack_header) + sizeof(conn_ack_frame) <=
            sizeof(conn_ack_buffer)
          );

          memcpy(conn_ack_buffer, &conn_ack_header, sizeof(conn_ack_header));
          memcpy(
            conn_ack_buffer + sizeof(conn_ack_header), &conn_ack_frame,
            sizeof(conn_ack_frame)
          );

          // checksum
          ((RdtHeader*)conn_ack_buffer)->checksum =
            rdt_packet_checksum(conn_ack_buffer);

          // occupy the socket
          std::lock_guard<std::mutex> lock(this->send_mutex);

          if (sendto(this->socket.value(), (char*)conn_ack_buffer, sizeof(conn_ack_buffer), 0, (struct sockaddr*)&this->remote_addr.value(), sizeof(this->remote_addr.value())) < 0) {
            log("failed to send connection response", LogLevel::Error);
            throw std::runtime_error("failed to send connection response");
          }

          log(
            std::format("sent connection response with seq {}", seq),
            LogLevel::Info
          );

        } else {
          log("received connection request in invalid state", LogLevel::Warn);
        }

        offset += sizeof(RdtCommonFrameHeader);

      } else if (frame_header->type == RdtFrameType::CONN_ACK) {
        if (this->role != Endpoint::Role::Sender) {
          log(
            "received connection response from receiver endpoint",
            LogLevel::Warn
          );
          offset += sizeof(RdtCommonFrameHeader);
          continue;
        }

        if (!this->waiting_for_ack) {
          log(
            "received connection response while not waiting for ACK",
            LogLevel::Warn
          );
          offset += sizeof(RdtCommonFrameHeader);
          continue;
        }

        if (this->state != Endpoint::State::Connecting && this->state != Endpoint::State::Closing) {
          log("received connection response in invalid state", LogLevel::Warn);
          offset += sizeof(RdtCommonFrameHeader);
          continue;
        }

        uint32_t seq = frame_header->seq;

        log(
          std::format("received connection response with seq {}", seq),
          LogLevel::Info
        );

        // compare seq and notify the sending thread
        this->require_resend = seq != this->last_seq;
        this->waiting_for_ack = false;
        this->require_resend_cv.notify_one();

        if (this->state == Endpoint::State::Connecting) {
          this->state = Endpoint::State::Connected;
          log("connection established", LogLevel::Info);
        }

        if (this->state == Endpoint::State::Closing) {
          this->state = Endpoint::State::Idle;
          log("connection closed", LogLevel::Info);
        }

        offset += sizeof(RdtCommonFrameHeader);

      } else if (frame_header->type == RdtFrameType::CONN_CLOSE) {
        if (this->waiting_for_ack) {
          log(
            "received connection close while waiting for ACK", LogLevel::Warn
          );
          // just close
        }

        this->active = false;

        log(
          std::format(
            "received connection close with seq {}", frame_header->seq
          ),
          LogLevel::Info
        );

        RdtHeader conn_close_header = {
          .version = RDT_VERSION,
          .reserved = 0,
          .number = 1,
          .checksum = 0,
        };

        RdtCommonFrameHeader conn_close_frame = {
          .type = RdtFrameType::CONN_CLOSE,
          .seq = frame_header->seq,
        };

        uint8_t conn_close_buffer[1024];

        assert(
          sizeof(conn_close_header) + sizeof(conn_close_frame) <=
          sizeof(conn_close_buffer)
        );

        memcpy(
          conn_close_buffer, &conn_close_header, sizeof(conn_close_header)
        );

        memcpy(
          conn_close_buffer + sizeof(conn_close_header), &conn_close_frame,
          sizeof(conn_close_frame)
        );

        // checksum
        ((RdtHeader*)conn_close_buffer)->checksum =
          rdt_packet_checksum(conn_close_buffer);

        std::lock_guard<std::mutex> lock(this->send_mutex);

        if (sendto(this->socket.value(), (char*)conn_close_buffer, sizeof(conn_close_buffer), 0, (struct sockaddr*)&this->remote_addr.value(), sizeof(this->remote_addr.value())) < 0) {
          log("failed to send connection close", LogLevel::Error);
          throw std::runtime_error("failed to send connection close");
        }

        log(
          std::format("sent connection close with seq {}", frame_header->seq),
          LogLevel::Info
        );

        offset += sizeof(RdtCommonFrameHeader);

        this->state = Endpoint::State::Idle;

      } else {
        log("received packet with unknown frame type", LogLevel::Warn);
      }
    }
  }
}

void Endpoint::run() {
  while (true) {
    std::string prompt;
    std::cout << "> ";
    std::getline(std::cin, prompt);

    std::vector<std::string> tokens;

    // tokenize
    size_t pos = 0;
    while ((pos = prompt.find(' ')) != std::string::npos) {
      tokens.push_back(prompt.substr(0, pos));
      prompt.erase(0, pos + 1);
    }

    tokens.push_back(prompt);

    if (tokens[0] == "connect") {
      if (this->role == Endpoint::Role::Sender) {
        this->connect();
      } else {
        log("cannot connect from receiver endpoint", LogLevel::Error);
      }
    } else if (tokens[0] == "recv") {
      this->recv_thread = std::thread([this]() {
        try {
          this->recv_handler();
        } catch (std::exception& e) {
          log(std::format("recv thread error: {}", e.what()), LogLevel::Error);
        }
      });
    }
  }
}

#endif