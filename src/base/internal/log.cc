#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <algorithm>
#include <array>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

#include "rc.h"  // for CONFIG_*

#define E(x) \
  { #x, DBG_##x }

const debug_t levels[] = {E(call_out),      E(d_flag),     E(connections), E(mapping),  E(sockets),
                          E(comp_func_tab), E(LPC),        E(LPC_line),    E(event),    E(dns),
                          E(file),          E(add_action), E(telnet),      E(websocket),
                          E(external_start)};

const int sizeof_levels = (sizeof(levels) / sizeof(levels[0]));

namespace {
FILE *debug_message_fp = nullptr;
std::deque<std::string> pending_messages;
bool stdout_buffering_configured = false;
thread_local std::vector<char> format_buffer(2048);
thread_local std::wstring wide_stdout_buffer;

#ifdef _WIN32
bool write_utf8_to_stdout(const char *text) {
  auto handle = GetStdHandle(STD_OUTPUT_HANDLE);
  if (handle == INVALID_HANDLE_VALUE || handle == nullptr) {
    return false;
  }

  DWORD mode = 0;
  if (!GetConsoleMode(handle, &mode)) {
    return false;
  }

  auto const utf8_len = strlen(text);
  wide_stdout_buffer.resize(utf8_len + 1);
  int const converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1,
                                            wide_stdout_buffer.data(),
                                            static_cast<int>(wide_stdout_buffer.size()));
  if (converted <= 0) {
    return false;
  }

  DWORD written = 0;
  return WriteConsoleW(handle, wide_stdout_buffer.c_str(), static_cast<DWORD>(converted - 1),
                       &written,
                       nullptr) != 0;
}
#endif

void configure_stdout_buffering() {
  if (!stdout_buffering_configured) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    stdout_buffering_configured = true;
  }
}

std::string_view format_debug_message(const char *fmt, va_list args) {
  va_list args_copy;
  va_copy(args_copy, args);

  auto result = vsnprintf(format_buffer.data(), format_buffer.size(), fmt, args_copy);
  va_end(args_copy);

  if (result < 0) {
    static constexpr std::string_view kInvalidFormat = "Invalid debug message format.\n";
    return kInvalidFormat;
  }

  auto required = static_cast<size_t>(result);
  if (required >= format_buffer.size()) {
    format_buffer.resize(required + 1);
    va_copy(args_copy, args);
    vsnprintf(format_buffer.data(), format_buffer.size(), fmt, args_copy);
    va_end(args_copy);
  }

  return std::string_view(format_buffer.data(), required);
}

void write_debug_output(std::string_view message) {
  configure_stdout_buffering();
  bool const needs_flush = message.empty() || message.back() != '\n';

  bool wrote_to_console = false;
#ifdef _WIN32
  wrote_to_console = write_utf8_to_stdout(message.data());
#endif
  if (!wrote_to_console) {
    fwrite(message.data(), 1, message.size(), stdout);
    if (needs_flush) {
      fflush(stdout);
    }
  }

  if (debug_message_fp) {
    fwrite(message.data(), 1, message.size(), debug_message_fp);
    if (needs_flush) {
      fflush(debug_message_fp);
    }
  } else {
    pending_messages.emplace_back(message);
  }
}
}  // namespace

void reset_debug_message_fp() {
  static char deb_buf[1024];
  char *deb = deb_buf;

  if (CONFIG_STR(__LOG_DIR__) == nullptr) {
    return;
  }

  auto *dlf = CONFIG_STR(__DEBUG_LOG_FILE__);
  if (dlf && strlen(dlf)) {
    snprintf(deb, 1023, "%s/%s", CONFIG_STR(__LOG_DIR__), dlf);
  } else {
    snprintf(deb, 1023, "%s/debug.log", CONFIG_STR(__LOG_DIR__));
  }
  deb[1023] = 0;
  while (*deb == '/') {
    deb++;
  }
  auto *new_location = fopen(deb, "w");
  if (!new_location) {
    debug_message("Unable to open log file: \"%s\", error: \"%s\".\n", deb, strerror(errno));
  } else {
    debug_message("New Debug log location: \"%s\".\n", deb);

    setvbuf(new_location, nullptr, _IOLBF, 0);
    debug_message_fp = new_location;

    if (!pending_messages.empty()) {
      for (auto &msg : pending_messages) {
        fwrite(msg.data(), 1, msg.size(), debug_message_fp);
      }
      pending_messages.clear();
      pending_messages.shrink_to_fit();
    }
  }
}

void debug_message(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  auto message = format_debug_message(fmt, args);
  va_end(args);

  write_debug_output(message);
}

void debug_message_with_location(const char *file, int line, const char *fmt, ...) {
  std::array<char, 96> prefix{};
  time_t rawtime;
  time(&rawtime);
  struct tm res = {};
  char timestamp[64] = "";
#ifdef _WIN32
  if (localtime_s(&res, &rawtime) == 0) {
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &res);
  }
#else
  if (localtime_r(&rawtime, &res) != nullptr) {
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &res);
  }
#endif
  auto prefix_len = snprintf(prefix.data(), prefix.size(), "[%s] %s:%d ", timestamp, file, line);

  va_list args;
  va_start(args, fmt);
  auto message = format_debug_message(fmt, args);
  va_end(args);

  std::string output;
  output.reserve(static_cast<size_t>(std::max(prefix_len, 0)) + message.size());
  if (prefix_len > 0) {
    output.append(prefix.data(), static_cast<size_t>(prefix_len));
  }
  output.append(message.data(), message.size());
  write_debug_output(output);
}

unsigned int debug_level = 0;

#define NELEM(x) (sizeof(x) / sizeof((x)[0]))

void debug_level_set(const char *level) {
  unsigned int i;

  for (i = 0; i < NELEM(levels); i++) {
    if (strcmp(level, levels[i].name) == 0) {
      debug_level |= levels[i].bit;
      return;
    }
  }
}

void debug_level_clear(const char *level) {
  unsigned int i;

  for (i = 0; i < NELEM(levels); i++) {
    if (strcmp(level, levels[i].name) == 0) {
      debug_level &= ~levels[i].bit;
      return;
    }
  }
}
