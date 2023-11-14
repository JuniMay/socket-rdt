#ifndef PROTOCOL_HPP_
#define PROTOCOL_HPP_

#include <cstdint>
#include <cstdlib>

#include "utils.hpp"

#define RDT_VERSION 1
#define RDT_MAX_PACKET_SIZE 1024
#define RDT_MAX_RETRIES 5

/// RDT Header.
///
/// Format overview:
/// 0               8               16             24              32
/// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
/// |            Version            |           Reserved            |
/// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
/// |            Number             |           Checksum            |
/// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
/// |                            Frames                             |
/// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
///
struct RdtHeader {
  /// Semantic versioning
  uint16_t version;
  /// Reserved field, set to 0
  uint16_t reserved;
  /// Frame number
  uint16_t number;
  /// Checksum of the packet
  uint16_t checksum;
};

enum class RdtFrameType : uint8_t {
  /// Data frame
  DATA = 1,
  /// Data acknowledgement frame
  DATA_ACK = 2,
  /// Connection request
  CONN_REQ = 3,
  /// Connection response
  CONN_ACK = 4,
  /// Connection close
  CONN_CLOSE = 5,
};

/// Common frame header for all frames.
struct RdtCommonFrameHeader {
  /// Frame type
  RdtFrameType type;
  /// Sequence number
  uint32_t seq;
};

/// The data frame header
///
/// In a stop-and-wait protocol, no fragmentation information is needed.
struct RdtDataFrameHeader {
  /// Common header
  RdtCommonFrameHeader common;
  /// Length of the data
  uint16_t length;
};

/// Calculate the size of a frame.
size_t rdt_frame_size(uint8_t* frame) {
  RdtFrameType type = static_cast<RdtFrameType>(*frame);

  switch (type) {
    case RdtFrameType::DATA: {
      RdtDataFrameHeader* header = reinterpret_cast<RdtDataFrameHeader*>(frame);
      return sizeof(RdtDataFrameHeader) + header->length;
    }
    case RdtFrameType::CONN_REQ: {
      return sizeof(RdtCommonFrameHeader);
    }
    case RdtFrameType::CONN_ACK: {
      return sizeof(RdtCommonFrameHeader);
    }
    case RdtFrameType::CONN_CLOSE: {
      return sizeof(RdtCommonFrameHeader);
    }
  }

  return 0;
}

/// Calculate the size of a packet.
size_t rdt_packet_size(uint8_t* packet) {
  uint16_t number = reinterpret_cast<RdtHeader*>(packet)->number;
  size_t size = 0;
  for (int i = 0; i < number; i++) {
    uint8_t* frame = packet + sizeof(RdtHeader) + size;
    size += rdt_frame_size(frame);
  }
  return sizeof(RdtHeader) + size;
}

/// Calculate the checksum of a packet.
uint16_t rdt_packet_checksum(uint8_t* packet) {
  size_t size = rdt_packet_size(packet);

  log(std::format("rdt_packet_checksum: size = {}", size), LogLevel::Debug);

  uint16_t* data = reinterpret_cast<uint16_t*>(packet);
  uint32_t sum = 0;

  while (size > 1) {
    sum += *data++;
    size -= sizeof(uint16_t);
  }

  if (size) {
    sum += (*(uint8_t*)data) << 8;
  }

  while (sum >> 16) {
    sum = (sum & 0xffff) + (sum >> 16);
  }

  return (~sum) & 0xffff;
}

/// Check if the packet is valid.
bool rdt_packet_valid(uint8_t* packet) {
  RdtHeader* header = reinterpret_cast<RdtHeader*>(packet);
  uint16_t checksum = header->checksum;
  header->checksum = 0;

  uint16_t valid_checksum = rdt_packet_checksum(packet);
  bool valid = checksum == valid_checksum;

  log(
    std::format(
      "rdt_packet_valid: checksum = {}, valid_checksum = {}, valid = {}", checksum, valid_checksum,
      valid
    ),
    LogLevel::Debug
  );

  header->checksum = checksum;
  return valid;
}

#endif