#ifndef UTILS_HPP_
#define UTILS_HPP_

#include <chrono>
#include <format>
#include <iostream>
#include <string>

#define TERM_GREY "\033[90m"
#define TERM_RED "\033[91m"
#define TERM_GREEN "\033[92m"
#define TERM_YELLOW "\033[93m"
#define TERM_NONE "\033[0m"

enum class LogLevel {
  Debug,
  Info,
  Warn,
  Error,
};

void log(const std::string& msg, LogLevel log_level) {
  std::string term_color;
  std::string prefix;

  auto time = std::chrono::system_clock::now();

  switch (log_level) {
    case LogLevel::Debug:
      term_color = TERM_GREY;
      prefix = "DEBUG";
      break;
    case LogLevel::Info:
      term_color = TERM_GREEN;
      prefix = "INFO";
      break;
    case LogLevel::Warn:
      term_color = TERM_YELLOW;
      prefix = "WARN";
      break;
    case LogLevel::Error:
      term_color = TERM_RED;
      prefix = "ERROR";
      break;
  }

  std::cout << std::format(
                 "[{} {} {:5} {}] {}", term_color, time, prefix, TERM_NONE, msg
               )
            << std::endl;
}

#endif