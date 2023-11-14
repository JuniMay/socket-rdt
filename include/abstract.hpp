#ifndef ABSTRACT_HPP_
#define ABSTRACT_HPP_

#ifdef _WIN32
#include "WS2tcpip.h"
#include "WinSock2.h"
#pragma comment(lib, "ws2_32.lib")
// ignore deprecated warnings
#pragma warning(disable : 4996)
typedef SOCKET socket_t;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int socket_t;
#endif

#include <optional>

#include "utils.hpp"

std::optional<socket_t> create_socket(int domain, int type, int protocol);

std::optional<socket_t> create_socket(int domain, int type, int protocol) {
  socket_t sock = socket(domain, type, protocol);
#ifdef _WIN32
  if (sock == INVALID_SOCKET) {
    // error code
    log(std::format("Failed to create socket: {}", WSAGetLastError()), LogLevel::Error);
    return std::nullopt;
  }
#else
  if (sock < 0) {
    return std::nullopt;
  }
#endif
  return sock;
}

#endif