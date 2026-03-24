#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <deque>
#include <memory>
#include <string>
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
std::deque<std::unique_ptr<std::vector<char>>> pending_messages;

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

  int const wide_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
  if (wide_len <= 0) {
    return false;
  }

  std::wstring wide_text(static_cast<size_t>(wide_len), L'\0');
  auto converted =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide_text.data(), wide_len);
  if (converted <= 0) {
    return false;
  }

  DWORD written = 0;
  return WriteConsoleW(handle, wide_text.c_str(), static_cast<DWORD>(wide_text.size() - 1),
                       &written,
                       nullptr) != 0;
}
#endif
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

    debug_message_fp = new_location;

    if (!pending_messages.empty()) {
      for (auto &msg : pending_messages) {
        fputs(msg->data(), debug_message_fp);
      }
      pending_messages.clear();
      pending_messages.shrink_to_fit();
    }
  }
}

void debug_message(const char *fmt, ...) {
  va_list args1, args2;
  va_start(args1, fmt);
  va_copy(args2, args1);

  auto length = vsnprintf(nullptr, 0, fmt, args1);
  va_end(args1);

  std::unique_ptr<std::vector<char>> result = std::make_unique<std::vector<char>>(length + 1);

  vsnprintf(result->data(), result->size(), fmt, args2);
  va_end(args2);

  // Always output to stdout first
  bool wrote_to_console = false;
#ifdef _WIN32
  wrote_to_console = write_utf8_to_stdout(result->data());
#endif
  if (!wrote_to_console) {
    fputs(result->data(), stdout);
  }
  fflush(stdout);

  // try to put into log directly, if available
  if (debug_message_fp) {
    fputs(result->data(), debug_message_fp);
    fflush(debug_message_fp);
  } else {
    // Buffer messages until log is available
    pending_messages.emplace_back(result.release());
    // result will be freed when flushed.
  }
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
