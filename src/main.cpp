#include "endpoints.hpp"

int main(int argc, char* argv[]) {
  // --port, -p
  uint16_t port = 8080;
  // --role, -r
  Endpoint::Role role = Endpoint::Role::Sender;
  // --remote-ip, -i
  std::string remote_ip = "";
  // --remote-port, -o
  uint16_t remote_port = 0;
  // --timeout, -t
  std::chrono::milliseconds timeout = std::chrono::milliseconds(1000);

  Endpoint endpoint;

  try {
    // Parse command line arguments.
    for (int i = 1; i < argc; i++) {
      std::string arg = argv[i];
      if (arg == "--port" || arg == "-p") {
        port = std::stoi(argv[++i]);
      } else if (arg == "--role" || arg == "-r") {
        std::string role_str = argv[++i];
        if (role_str == "sender") {
          role = Endpoint::Role::Sender;
        } else if (role_str == "receiver") {
          role = Endpoint::Role::Receiver;
        } else {
          throw std::runtime_error("Invalid role: " + role_str);
        }
      } else if (arg == "--remote-ip" || arg == "-i") {
        remote_ip = argv[++i];
      } else if (arg == "--remote-port" || arg == "-o") {
        remote_port = std::stoi(argv[++i]);
      } else if (arg == "--timeout" || arg == "-t") {
        timeout = std::chrono::milliseconds(std::stoi(argv[++i]));
      } else {
        throw std::runtime_error("Invalid argument: " + arg);
      }
    }

    endpoint.set_remote(remote_ip, remote_port);
    endpoint.init(port, role);
    endpoint.show_info();
    endpoint.run();
    endpoint.cleanup();

    return 0;
  } catch (const std::exception& e) {
    endpoint.cleanup();
    std::cerr << e.what() << std::endl;
    return 1;
  }
}