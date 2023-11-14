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

  /// Send a constructed packet.
  bool send_packet(uint8_t* buffer, int size);

  /// Set the timeout.
  void set_timeout(std::chrono::milliseconds timeout) { this->timeout = timeout; }

  /// Run the endpoint with a repl.
  void run();

  /// Close the connection.
  void close();

  /// Cleanup the resources.
  void cleanup();

  /// Update and return the last seq.
  uint32_t update_last_seq() {
    log(
      std::format("updating last seq from {} to {}", this->last_seq.load(), this->last_seq ^ 1),
      LogLevel::Debug
    );
    this->last_seq ^= 1;
    return this->last_seq;
  }

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
  /// Receive thread
  std::thread recv_thread;
  /// Receive count
  std::atomic<size_t> recv_count;
};

void Endpoint::init(uint16_t port, Role role) {
#ifdef _WIN32
  WSADATA wsa_data;
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    log(std::format("failed to initialize Winsock: {}", WSAGetLastError()), LogLevel::Error);
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

  this->recv_thread = std::thread([this]() {
    try {
      this->recv_handler();
    } catch (std::exception& e) {
      log(std::format("recv thread error: {}", e.what()), LogLevel::Error);
    }
  });

  log("endpoint initialized", LogLevel::Info);
}

void Endpoint::show_info() {
  log("endpoint info:", LogLevel::Info);
  log(
    std::format("  type: {}", this->role == Endpoint::Role::Sender ? "sender" : "receiver"),
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
  inet_ntop(AF_INET, &((struct sockaddr_in*)addrs->ai_addr)->sin_addr, ip, sizeof(ip));

  log(std::format("  address: {}:{}", ip, ntohs(this->addr.value().sin_port)), LogLevel::Info);

  // show remote
  if (this->remote_addr.has_value()) {
    inet_ntop(AF_INET, &this->remote_addr.value().sin_addr, ip, sizeof(ip));
    log(
      std::format("  remote: {}:{}", ip, ntohs(this->remote_addr.value().sin_port)), LogLevel::Info
    );
  }

  freeaddrinfo(addrs);
}

void Endpoint::cleanup() {
  this->active = false;

  if (this->recv_thread.joinable()) {
    this->recv_thread.join();
  }

  if (this->file.is_open()) {
    this->file.close();
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

bool Endpoint::send_packet(uint8_t* packet, int size) {
  if (!this->socket.has_value()) {
    log("socket not initialized", LogLevel::Error);
    throw std::runtime_error("socket not initialized");
  }

  size_t retries = 0;

  while (retries < RDT_MAX_RETRIES) {
    std::unique_lock<std::mutex> lock(this->send_mutex);

    if (sendto(this->socket.value(), (char*)packet, size, 0, (struct sockaddr*)&this->remote_addr.value(), sizeof(this->remote_addr.value())) < 0) {
      log("failed to send packet", LogLevel::Error);
      throw std::runtime_error("failed to send packet");
    }

    // sent packet
    log(std::format("sent packet with size {}", size), LogLevel::Info);

    // wait for ACK
    this->waiting_for_ack = true;
    this->require_resend_cv.wait_for(lock, this->timeout, [this]() {
      return !this->waiting_for_ack;
    });

    if (this->require_resend) {
      // resend because of inconsistent seq
      log(std::format("resend because of inconsistent seq, retries = {}", retries), LogLevel::Info);
      retries++;
      continue;
    } else if (this->waiting_for_ack) {
      // resend because of timeout
      log(std::format("resend because of timeout, retries = {}", retries), LogLevel::Info);
      retries++;
      continue;
    } else {
      // received ACK
      log(std::format("received ACK, retries = {}", retries), LogLevel::Info);
      break;
    }
  }

  return retries < RDT_MAX_RETRIES;
}

void Endpoint::close() {
  RdtHeader conn_close_header = {
    .version = RDT_VERSION,
    .reserved = 0,
    .number = 1,
    .checksum = 0,
  };

  RdtCommonFrameHeader conn_close_frame = {
    .type = RdtFrameType::CONN_CLOSE,
    .seq = this->last_seq,
  };

  uint8_t conn_close_buffer[sizeof(conn_close_header) + sizeof(conn_close_frame)];

  assert(sizeof(conn_close_header) + sizeof(conn_close_frame) <= sizeof(conn_close_buffer));

  memcpy(conn_close_buffer, &conn_close_header, sizeof(conn_close_header));

  memcpy(
    conn_close_buffer + sizeof(conn_close_header), &conn_close_frame, sizeof(conn_close_frame)
  );

  // checksum
  ((RdtHeader*)conn_close_buffer)->checksum = rdt_packet_checksum(conn_close_buffer);

  // occupy the socket
  std::lock_guard<std::mutex> lock(this->send_mutex);
  this->state = Endpoint::State::Closing;

  bool success =
    this->send_packet(conn_close_buffer, sizeof(conn_close_header) + sizeof(conn_close_frame));

  if (!success) {
    log("failed to close connection", LogLevel::Warn);
  }

  this->state = Endpoint::State::Idle;
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

    assert(sizeof(conn_req_header) + sizeof(conn_req_frame) <= sizeof(conn_req_buffer));

    memcpy(conn_req_buffer, &conn_req_header, sizeof(conn_req_header));
    memcpy(conn_req_buffer + sizeof(conn_req_header), &conn_req_frame, sizeof(conn_req_frame));

    // checksum
    ((RdtHeader*)conn_req_buffer)->checksum = rdt_packet_checksum(conn_req_buffer);

    log("sending connection request", LogLevel::Info);

    bool success = this->send_packet(conn_req_buffer, sizeof(conn_req_buffer));

    if (this->state != Endpoint::State::Connected) {
      log("failed to connect", LogLevel::Error);
      throw std::runtime_error("failed to connect");
    }

    this->update_last_seq();
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

  if (this->state != Endpoint::State::Connected) {
    log("cannot send file from non-connected state", LogLevel::Error);
    throw std::runtime_error("cannot send file from non-connected state");
  }

  if (this->file.is_open()) {
    log("closing previous file", LogLevel::Warn);
    this->file.close();
  }

  this->file.open(filename, std::ios::in | std::ios::binary);

  if (!this->file.is_open()) {
    log("failed to open file", LogLevel::Error);
    throw std::runtime_error("failed to open file");
  }

  uint8_t packet_buffer[RDT_MAX_PACKET_SIZE];

  const size_t max_fragment_size =
    RDT_MAX_PACKET_SIZE - sizeof(RdtHeader) - sizeof(RdtDataFrameHeader);

  uint8_t buffer[max_fragment_size];

  size_t fragment_count = 0;

  std::chrono::time_point<std::chrono::system_clock> start_time = std::chrono::system_clock::now();

  this->file.seekg(0, std::ios::end);
  size_t total_data_size = this->file.tellg();
  this->file.seekg(0, std::ios::beg);

  size_t remaining_data_size = total_data_size;

  size_t total_packet_size = 0;

  while (remaining_data_size > 0) {
    log(std::format("sending file fragment {}", fragment_count++), LogLevel::Debug);

    // read file
    if (remaining_data_size < max_fragment_size) {
      this->file.read((char*)buffer, remaining_data_size);
      remaining_data_size = 0;
    } else {
      this->file.read((char*)buffer, max_fragment_size);
      remaining_data_size -= max_fragment_size;
    }

    if (this->file.eof()) {
      log("reached end of file", LogLevel::Debug);
      break;
    }

    if (this->file.fail()) {
      log("failed to read file", LogLevel::Error);
      throw std::runtime_error("failed to read file");
    }

    // send data
    RdtHeader data_header = {
      .version = RDT_VERSION,
      .reserved = 0,
      .number = 1,
      .checksum = 0,
    };

    RdtDataFrameHeader data_frame = {
      .common =
        {
          .type = RdtFrameType::DATA,
          .seq = this->last_seq,
        },
      .length = (uint16_t)this->file.gcount(),
    };

    log(std::format("size of data header: {}", sizeof(data_header)), LogLevel::Debug);
    log(std::format("size of data frame: {}", sizeof(data_frame)), LogLevel::Debug);
    log(std::format("size of data: {}", data_frame.length), LogLevel::Debug);

    assert(sizeof(data_header) + sizeof(data_frame) + data_frame.length <= sizeof(packet_buffer));

    memcpy(packet_buffer, &data_header, sizeof(data_header));
    memcpy(packet_buffer + sizeof(data_header), &data_frame, sizeof(data_frame));
    memcpy(packet_buffer + sizeof(data_header) + sizeof(data_frame), buffer, data_frame.length);

    // checksum
    ((RdtHeader*)packet_buffer)->checksum = rdt_packet_checksum(packet_buffer);

    log(std::format("sending data frame with seq {}", data_frame.common.seq), LogLevel::Info);

    bool success = this->send_packet(
      packet_buffer, sizeof(data_header) + sizeof(data_frame) + data_frame.length
    );

    total_packet_size += sizeof(data_header) + sizeof(data_frame) + data_frame.length;

    if (!success) {
      log("failed to send packet", LogLevel::Error);
      throw std::runtime_error("failed to send packet");
    }

    this->update_last_seq();
  }

  std::chrono::time_point<std::chrono::system_clock> end_time = std::chrono::system_clock::now();

  std::chrono::duration<double> elapsed_seconds = end_time - start_time;

  // analysis
  log(std::format("total data size: {}", total_data_size), LogLevel::Info);
  log(std::format("total packet size: {}", total_packet_size), LogLevel::Info);
  log(std::format("elapsed time: {}s", elapsed_seconds.count()), LogLevel::Info);
  log(std::format("throughput: {}B/s", total_data_size / elapsed_seconds.count()), LogLevel::Info);
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
      (struct sockaddr*)&this->remote_addr.value(), (socklen_t*)&addr_len
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
      log(std::format("failed to receive packet: {}", WSAGetLastError()), LogLevel::Error);
#else
      log(std::format("failed to receive packet: {}", strerror(errno)), LogLevel::Error);
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

    bool valid = rdt_packet_valid(buffer);

    // check validity
    if (!valid) {
      log("received packet with invalid checksum", LogLevel::Warn);
    }

    size_t offset = sizeof(RdtHeader);

    for (size_t i = 0; i < header->number; i++) {
      RdtCommonFrameHeader* frame_header = (RdtCommonFrameHeader*)(buffer + offset);

      if (frame_header->type == RdtFrameType::DATA) {
        RdtDataFrameHeader* frame_header = (RdtDataFrameHeader*)(buffer + offset);

        // Only when a **Receiver** receives a data and the state is
        // **Connected** or **Closing**, should it send an ACK.
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

        log(
          std::format(
            "received data frame with seq {}, length: {}", frame_header->common.seq,
            frame_header->length
          ),
          LogLevel::Info
        );

        if (valid && frame_header->common.seq == this->last_seq) {
          log("received duplicate data frame", LogLevel::Warn);
        }

        // write data to file (in binary)
        if (valid && frame_header->common.seq != this->last_seq) {
          this->file.write(
            (char*)buffer + offset + sizeof(RdtDataFrameHeader), frame_header->length
          );
          this->file.flush();
          this->last_seq = frame_header->common.seq;

          this->recv_count++;
        }

        // send ACK
        RdtHeader ack_header = {
          .version = RDT_VERSION,
          .reserved = 0,
          .number = 1,
          .checksum = 0,
        };

        RdtCommonFrameHeader ack_frame = {
          .type = RdtFrameType::DATA_ACK,
          .seq = valid ? frame_header->common.seq : frame_header->common.seq ^ 1,
        };

        uint8_t ack_buffer[sizeof(ack_header) + sizeof(ack_frame)];

        assert(sizeof(ack_header) + sizeof(ack_frame) <= sizeof(ack_buffer));

        memcpy(ack_buffer, &ack_header, sizeof(ack_header));
        memcpy(ack_buffer + sizeof(ack_header), &ack_frame, sizeof(ack_frame));

        // checksum
        ((RdtHeader*)ack_buffer)->checksum = rdt_packet_checksum(ack_buffer);

        // occupy the socket
        std::lock_guard<std::mutex> lock(this->send_mutex);

        if (sendto(this->socket.value(), (char*)ack_buffer, 
          sizeof(ack_header) + sizeof(ack_frame)
        , 0, (struct sockaddr*)&this->remote_addr.value(), sizeof(this->remote_addr.value())) < 0) {
          log("failed to send data ACK", LogLevel::Error);
          throw std::runtime_error("failed to send data ACK");
        }

        log(std::format("sent data ACK with seq {}", ack_frame.seq), LogLevel::Info);

        offset += sizeof(RdtDataFrameHeader) + frame_header->length;

      } else if (frame_header->type == RdtFrameType::CONN_REQ) {
        if (this->role != Endpoint::Role::Receiver) {
          log("received connection request from sender endpoint", LogLevel::Warn);

          offset += sizeof(RdtCommonFrameHeader);
          continue;
        }

        if (this->waiting_for_ack) {
          log("received connection request while waiting for ACK", LogLevel::Warn);
          offset += sizeof(RdtCommonFrameHeader);
          continue;
        }

        uint32_t seq = frame_header->seq;

        if (this->state == Endpoint::State::Idle || this->state == Endpoint::State::Connected) {
          log(std::format("received connection request with seq {}", seq), LogLevel::Info);

          // send connection response
          RdtHeader conn_ack_header = {
            .version = RDT_VERSION,
            .reserved = 0,
            .number = 1,
            .checksum = 0,
          };

          RdtCommonFrameHeader conn_ack_frame = {
            .type = RdtFrameType::CONN_ACK,
            .seq = valid ? seq : seq ^ 1,
          };

          uint8_t conn_ack_buffer[sizeof(conn_ack_header) + sizeof(conn_ack_frame)];

          assert(sizeof(conn_ack_header) + sizeof(conn_ack_frame) <= sizeof(conn_ack_buffer));

          memcpy(conn_ack_buffer, &conn_ack_header, sizeof(conn_ack_header));
          memcpy(
            conn_ack_buffer + sizeof(conn_ack_header), &conn_ack_frame, sizeof(conn_ack_frame)
          );

          // checksum
          ((RdtHeader*)conn_ack_buffer)->checksum = rdt_packet_checksum(conn_ack_buffer);

          // occupy the socket
          std::lock_guard<std::mutex> lock(this->send_mutex);

          if (sendto(this->socket.value(), (char*)conn_ack_buffer, 
            sizeof(conn_ack_header) + sizeof(conn_ack_frame)
          , 0, (struct sockaddr*)&this->remote_addr.value(), sizeof(this->remote_addr.value())) < 0) {
            log("failed to send connection response", LogLevel::Error);
            throw std::runtime_error("failed to send connection response");
          }

          log(std::format("sent connection response with seq {}", seq), LogLevel::Info);

          log("connection established", LogLevel::Info);

          // set the seq of the last packet
          this->last_seq = seq;
          this->state = Endpoint::State::Connected;
        } else {
          log("received connection request in invalid state", LogLevel::Warn);
        }

        offset += sizeof(RdtCommonFrameHeader);

      } else if (frame_header->type == RdtFrameType::CONN_ACK) {
        if (this->role != Endpoint::Role::Sender) {
          log("received connection response from receiver endpoint", LogLevel::Warn);
          offset += sizeof(RdtCommonFrameHeader);
          continue;
        }

        if (!this->waiting_for_ack) {
          log("received connection response while not waiting for ACK", LogLevel::Warn);
          offset += sizeof(RdtCommonFrameHeader);
          continue;
        }

        if (this->state != Endpoint::State::Connecting && this->state != Endpoint::State::Closing) {
          log("received connection response in invalid state", LogLevel::Warn);
          offset += sizeof(RdtCommonFrameHeader);
          continue;
        }

        uint32_t seq = frame_header->seq;

        log(std::format("received connection response with seq {}", seq), LogLevel::Info);

        // compare seq and notify the sending thread
        this->require_resend = seq != this->last_seq || !valid;
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
      } else if (frame_header->type == RdtFrameType::DATA_ACK) {
        if (this->role != Endpoint::Role::Sender) {
          log("received data ACK from receiver endpoint", LogLevel::Warn);
          offset += sizeof(RdtCommonFrameHeader);
          continue;
        }

        if (!this->waiting_for_ack) {
          log("received data ACK while not waiting for ACK", LogLevel::Warn);
          offset += sizeof(RdtCommonFrameHeader);
          continue;
        }

        if (this->state != Endpoint::State::Connected && this->state != Endpoint::State::Closing) {
          log("received data ACK in invalid state", LogLevel::Warn);
          offset += sizeof(RdtCommonFrameHeader);
          continue;
        }

        uint32_t seq = frame_header->seq;

        log(std::format("received data ACK with seq {}", seq), LogLevel::Info);

        // compare seq and notify the sending thread
        this->require_resend = seq != this->last_seq || !valid;
        this->waiting_for_ack = false;
        this->require_resend_cv.notify_one();

        offset += sizeof(RdtCommonFrameHeader);
      } else if (frame_header->type == RdtFrameType::CONN_CLOSE) {
        if (this->waiting_for_ack) {
          log("received connection close while waiting for ACK", LogLevel::Warn);
          // just close
        }

        this->active = false;

        log(
          std::format("received connection close with seq {}", frame_header->seq), LogLevel::Info
        );

        RdtHeader conn_close_header = {
          .version = RDT_VERSION,
          .reserved = 0,
          .number = 1,
          .checksum = 0,
        };

        RdtCommonFrameHeader conn_close_frame = {
          .type = RdtFrameType::CONN_CLOSE,
          .seq = valid ? frame_header->seq : frame_header->seq ^ 1,
        };

        uint8_t conn_close_buffer[sizeof(conn_close_header) + sizeof(conn_close_frame)];

        assert(sizeof(conn_close_header) + sizeof(conn_close_frame) <= sizeof(conn_close_buffer));

        memcpy(conn_close_buffer, &conn_close_header, sizeof(conn_close_header));

        memcpy(
          conn_close_buffer + sizeof(conn_close_header), &conn_close_frame, sizeof(conn_close_frame)
        );

        // checksum
        ((RdtHeader*)conn_close_buffer)->checksum = rdt_packet_checksum(conn_close_buffer);

        std::lock_guard<std::mutex> lock(this->send_mutex);

        if (sendto(this->socket.value(), (char*)conn_close_buffer, 
          sizeof(conn_close_header) + sizeof(conn_close_frame)
        , 0, (struct sockaddr*)&this->remote_addr.value(), sizeof(this->remote_addr.value())) < 0) {
          log("failed to send connection close", LogLevel::Error);
          throw std::runtime_error("failed to send connection close");
        }

        log(std::format("sent connection close with seq {}", frame_header->seq), LogLevel::Info);

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
    } else if (tokens[0] == "send") {
      // given a file name, send the file
      if (tokens.size() < 2) {
        log("send requires a file name", LogLevel::Error);
        continue;
      }

      if (this->role == Endpoint::Role::Receiver) {
        log("cannot send file from receiver endpoint", LogLevel::Error);
        continue;
      }

      this->send_file(tokens[1]);
    } else if (tokens[0] == "close") {
      log("closing connection", LogLevel::Info);
      this->close();
      break;
    } else if (tokens[0] == "file") {
      log(std::format("received {} packets", this->recv_count.load()), LogLevel::Debug);

      this->recv_count = 0;

      if (this->file.is_open()) {
        log("closing file stream", LogLevel::Info);
        this->file.close();
      }

      // set the received file name
      if (tokens.size() == 2) {
        if (this->role == Endpoint::Role::Sender) {
          log("cannot set file name from sender endpoint", LogLevel::Warn);
          continue;
        }
        this->file.open(tokens[1], std::ios::out | std::ios::binary);
      }
    }
  }
}

#endif