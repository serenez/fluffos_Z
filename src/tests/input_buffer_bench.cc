#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "base/std.h"
#include "interactive.h"
#include "mainlib.h"
#include "user.h"
#include "vm/vm.h"

int cmd_in_buf(interactive_t *ip);
char *extract_first_command_for_test(interactive_t *ip);

namespace {

struct LegacyInteractiveBuffer {
  std::vector<char> text;
  int text_start = 0;
  int text_end = 0;
  unsigned int iflags = 0;
};

bool legacy_clean_buf(LegacyInteractiveBuffer &ip) {
  while (ip.text_start < ip.text_end && !ip.text[ip.text_start]) {
    ip.text_start++;
  }

  if (ip.text_start >= ip.text_end) {
    ip.text_start = 0;
    ip.text_end = 0;
  }

  if (ip.iflags & SKIP_COMMAND) {
    for (int i = ip.text_start; i < ip.text_end; i++) {
      if (ip.text[i] == '\r' || ip.text[i] == '\n') {
        ip.text_start += i - ip.text_start + 1;
        ip.iflags &= ~SKIP_COMMAND;
        return legacy_clean_buf(ip);
      }
    }
  }

  return ip.text_end > ip.text_start;
}

int legacy_cmd_in_buf(LegacyInteractiveBuffer &ip) {
  if (!legacy_clean_buf(ip)) {
    return 0;
  }

  if (ip.iflags & SINGLE_CHAR) {
    return 1;
  }

  for (int i = ip.text_start; i < ip.text_end; i++) {
    if (ip.text[i] == '\r' || ip.text[i] == '\n') {
      return 1;
    }
  }

  return 0;
}

char *legacy_first_cmd_in_buf(LegacyInteractiveBuffer &ip) {
  static char tmp[2];

  if (!legacy_clean_buf(ip)) {
    return nullptr;
  }

  char *command = ip.text.data() + ip.text_start;
  if (ip.iflags & SINGLE_CHAR) {
    if (*command == 8 || *command == 127) {
      *command = 0;
    }
    tmp[0] = *command;
    ip.text[ip.text_start++] = 0;
    if (!legacy_clean_buf(ip)) {
      ip.iflags &= ~CMD_IN_BUF;
    }
    return tmp;
  }

  while (ip.text_start < ip.text_end && ip.text[ip.text_start] != '\n' &&
         ip.text[ip.text_start] != '\r') {
    ip.text_start++;
  }

  if (ip.text_start + 1 < ip.text_end &&
      ((ip.text[ip.text_start] == '\r' && ip.text[ip.text_start + 1] == '\n') ||
       (ip.text[ip.text_start] == '\n' && ip.text[ip.text_start + 1] == '\r'))) {
    ip.text[ip.text_start++] = 0;
  }

  ip.text[ip.text_start++] = 0;
  if (!legacy_cmd_in_buf(ip)) {
    ip.iflags &= ~CMD_IN_BUF;
  }
  return command;
}

void prepare_current_buffer(interactive_t *ip, std::string_view payload) {
  interactive_reset_text(ip);
  interactive_ensure_text_capacity(ip, static_cast<int>(payload.size()) + 1);
  std::memcpy(ip->text, payload.data(), payload.size());
  ip->text[payload.size()] = '\0';
  ip->text_start = 0;
  ip->text_end = static_cast<int>(payload.size());
  ip->iflags = CMD_IN_BUF;
}

void prepare_legacy_buffer(LegacyInteractiveBuffer &ip, std::string_view payload) {
  ip.text.assign(payload.begin(), payload.end());
  ip.text.push_back('\0');
  ip.text_start = 0;
  ip.text_end = static_cast<int>(payload.size());
  ip.iflags = CMD_IN_BUF;
}

struct BenchCase {
  std::string_view name;
  std::string payload;
  int iterations;
};

std::string build_line(size_t length, char fill) {
  return std::string(length, fill) + "\n";
}

std::string build_two_lines(size_t first_length, size_t second_length) {
  return std::string(first_length, 'a') + "\n" + std::string(second_length, 'b') + "\n";
}

void run_case(const BenchCase &bench) {
  auto *current = user_add();
  if (current == nullptr || current->text == nullptr) {
    std::cerr << "failed to allocate interactive for case=" << bench.name << '\n';
    if (current != nullptr) {
      user_del(current);
      interactive_free_text(current);
      FREE(current);
    }
    return;
  }

  interactive_ensure_text_capacity(current, static_cast<int>(bench.payload.size()) + 1);
  LegacyInteractiveBuffer legacy;

  long long current_elapsed_ns = 0;
  long long legacy_elapsed_ns = 0;
  volatile unsigned long long guard = 0;

  for (int i = 0; i < bench.iterations; i++) {
    prepare_current_buffer(current, bench.payload);
    auto current_start = std::chrono::steady_clock::now();
    auto current_ready = cmd_in_buf(current);
    auto *current_command = current_ready ? extract_first_command_for_test(current) : nullptr;
    auto current_end = std::chrono::steady_clock::now();
    current_elapsed_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(current_end - current_start).count();
    if (current_command != nullptr) {
      guard += static_cast<unsigned long long>(static_cast<unsigned char>(current_command[0]));
    }

    prepare_legacy_buffer(legacy, bench.payload);
    auto legacy_start = std::chrono::steady_clock::now();
    auto legacy_ready = legacy_cmd_in_buf(legacy);
    auto *legacy_command = legacy_ready ? legacy_first_cmd_in_buf(legacy) : nullptr;
    auto legacy_end = std::chrono::steady_clock::now();
    legacy_elapsed_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(legacy_end - legacy_start).count();
    if (legacy_command != nullptr) {
      guard += static_cast<unsigned long long>(static_cast<unsigned char>(legacy_command[0]));
    }
  }

  std::cout << "case=" << bench.name << " iterations=" << bench.iterations
            << " payload_bytes=" << bench.payload.size()
            << " current_avg_ns=" << (current_elapsed_ns / bench.iterations)
            << " legacy_avg_ns=" << (legacy_elapsed_ns / bench.iterations)
            << " guard=" << guard << '\n';

  user_del(current);
  interactive_free_text(current);
  FREE(current);
}

}  // namespace

int main() {
  std::filesystem::current_path(TESTSUITE_DIR);
  init_main("etc/config.test", false);
  vm_start();

  std::vector<BenchCase> cases = {
      {"single_line_256", build_line(256, 'a'), 200000},
      {"single_line_4096", build_line(4096, 'a'), 50000},
      {"two_lines_4096_4096", build_two_lines(4096, 4096), 30000},
  };

  for (const auto &bench : cases) {
    run_case(bench);
  }

  clear_state();
  return 0;
}
