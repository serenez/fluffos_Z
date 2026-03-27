#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "base/package_api.h"
#include "comm.h"
#include "interactive.h"
#include "mainlib.h"
#include "user.h"
#include "vm/internal/base/object.h"
#include "vm/internal/simulate.h"
#include "vm/vm.h"

namespace {

object_t *load_probe_object(bool clone = false) {
  object_t *ob = nullptr;
  current_object = master_ob;

  error_context_t econ{};
  save_context(&econ);
  try {
    ob = clone ? clone_object("/clone/mud_input_probe", 0) : find_object("/clone/mud_input_probe");
  } catch (...) {
    restore_context(&econ);
  }
  pop_context(&econ);
  return ob;
}

std::string build_mud_string_packet(std::string_view text) {
  svalue_t value{};
  value.type = T_STRING;
  value.subtype = STRING_SHARED;
  std::string owned_text(text);
  value.u.string = make_shared_string(owned_text.c_str());

  save_svalue_depth = 0;
  int const reserved_size = svalue_save_size(&value);
  std::string payload(static_cast<size_t>(reserved_size), '\0');
  char *payload_cursor = payload.data();
  save_svalue(&value, &payload_cursor);
  payload.resize(static_cast<size_t>(payload_cursor - payload.data()));

  uint32_t network_size = htonl(static_cast<uint32_t>(payload.size()));
  std::string packet(sizeof(network_size) + payload.size(), '\0');
  std::memcpy(packet.data(), &network_size, sizeof(network_size));
  std::memcpy(packet.data() + sizeof(network_size), payload.data(), payload.size());

  free_svalue(&value, "mud_input_bench_packet");
  return packet;
}

struct BenchInteractive {
  interactive_t *ip = nullptr;
  object_t *owner = nullptr;

  BenchInteractive() {
    owner = load_probe_object(true);
    if (owner == nullptr) {
      return;
    }
    ip = user_add();
    if (ip == nullptr || ip->text == nullptr) {
      if (ip != nullptr) {
        user_del(ip);
        interactive_free_text(ip);
        FREE(ip);
      }
      ip = nullptr;
      if (owner != nullptr && !(owner->flags & O_DESTRUCTED)) {
        destruct_object(owner);
      }
      if (owner != nullptr) {
        free_object(&owner, "BenchInteractive ctor");
      }
      return;
    }

    ip->ob = owner;
    ip->connection_type = PORT_TYPE_MUD;
    owner->interactive = ip;
  }

  ~BenchInteractive() {
    if (owner != nullptr && owner->interactive == ip) {
      owner->interactive = nullptr;
    }
    if (ip != nullptr) {
      user_del(ip);
      interactive_free_text(ip);
      FREE(ip);
    }
    if (owner != nullptr && !(owner->flags & O_DESTRUCTED)) {
      destruct_object(owner);
    }
    if (owner != nullptr) {
      free_object(&owner, "BenchInteractive dtor");
    }
  }

  void reset() {
    if (owner != nullptr) {
      save_command_giver(owner);
      set_eval(max_eval_cost);
      safe_apply("reset_state", owner, 0, ORIGIN_DRIVER);
      restore_command_giver();
    }
    if (ip != nullptr) {
      interactive_reset_text(ip);
    }
  }
};

bool decode_packet_size(const char *buffer, int *packet_size) {
  uint32_t network_size = 0;
  std::memcpy(&network_size, buffer, sizeof(network_size));
  auto decoded_size = static_cast<int32_t>(ntohl(network_size));
  if (decoded_size <= 0 || decoded_size > MAX_TEXT - 5) {
    return false;
  }
  *packet_size = decoded_size;
  return true;
}

bool ensure_legacy_capacity(interactive_t *ip, int required_size) {
  if (required_size <= ip->text_capacity) {
    return true;
  }
  return interactive_ensure_text_capacity(ip, required_size);
}

int legacy_prepare_text_space(interactive_t *ip) {
  if (ip->text_end < static_cast<int>(sizeof(uint32_t))) {
    if (!ensure_legacy_capacity(ip, static_cast<int>(sizeof(uint32_t)) + 1)) {
      return -1;
    }
    return static_cast<int>(sizeof(uint32_t)) - ip->text_end;
  }

  int packet_size = 0;
  if (!decode_packet_size(ip->text, &packet_size)) {
    return -1;
  }
  if (!ensure_legacy_capacity(ip, packet_size + 5)) {
    return -1;
  }
  int const text_space = packet_size - ip->text_end + static_cast<int>(sizeof(uint32_t));
  return text_space > 0 ? text_space : -1;
}

int legacy_process_chunk(interactive_t *ip, const unsigned char *buf, int num_bytes) {
  if (!ip || !buf || num_bytes <= 0) {
    return 0;
  }
  if (!ensure_legacy_capacity(ip, ip->text_end + num_bytes + 1)) {
    return 0;
  }

  std::memcpy(ip->text + ip->text_end, buf, num_bytes);
  ip->text_end += num_bytes;
  if (ip->text_end < static_cast<int>(sizeof(uint32_t))) {
    return 0;
  }

  int packet_size = 0;
  if (!decode_packet_size(ip->text, &packet_size)) {
    return 0;
  }
  if (!mud_packet_is_complete(packet_size, ip->text_end)) {
    return 0;
  }

  svalue_t value;
  ip->text[ip->text_end] = '\0';
  if (restore_svalue(ip->text + static_cast<int>(sizeof(uint32_t)), &value) == 0) {
    STACK_INC;
    *sp = value;
  } else {
    push_undefined();
  }

  interactive_reset_text(ip);
  set_eval(max_eval_cost);
  safe_apply(APPLY_PROCESS_INPUT, ip->ob, 1, ORIGIN_DRIVER);
  return 1;
}

int run_current_stream(interactive_t *ip, const std::string &stream) {
  size_t cursor = 0;
  int callback_rounds = stream.empty() ? 0 : 1;
  while (cursor < stream.size()) {
    int const text_space = legacy_prepare_text_space(ip);
    if (text_space <= 0) {
      break;
    }
    int const chunk_size =
        std::min<int>(text_space, static_cast<int>(stream.size() - cursor));
    process_mud_chunk_for_test(ip, reinterpret_cast<const unsigned char *>(stream.data() + cursor),
                               chunk_size);
    cursor += chunk_size;
  }
  return callback_rounds;
}

int run_legacy_stream(interactive_t *ip, const std::string &stream) {
  size_t cursor = 0;
  int callback_rounds = 0;
  while (cursor < stream.size()) {
    int const text_space = legacy_prepare_text_space(ip);
    if (text_space <= 0) {
      break;
    }
    callback_rounds++;
    int const chunk_size =
        std::min<int>(text_space, static_cast<int>(stream.size() - cursor));
    legacy_process_chunk(ip, reinterpret_cast<const unsigned char *>(stream.data() + cursor),
                         chunk_size);
    cursor += chunk_size;
  }
  return callback_rounds;
}

std::string build_packet_stream(int packet_count, int payload_len) {
  std::string payload(static_cast<size_t>(payload_len), 'a');
  std::string stream;
  for (int i = 0; i < packet_count; i++) {
    payload[0] = static_cast<char>('a' + (i % 26));
    auto packet = build_mud_string_packet(payload);
    stream += packet;
  }
  return stream;
}

struct BenchCase {
  std::string_view name;
  int packet_count;
  int payload_len;
  int iterations;
};

void run_case(const BenchCase &bench) {
  BenchInteractive current;
  BenchInteractive legacy;
  if (current.ip == nullptr || legacy.ip == nullptr) {
    std::cerr << "failed to init bench case=" << bench.name << '\n';
    return;
  }

  auto stream = build_packet_stream(bench.packet_count, bench.payload_len);
  long long current_elapsed_ns = 0;
  long long legacy_elapsed_ns = 0;
  long long current_rounds = 0;
  long long legacy_rounds = 0;

  for (int i = 0; i < bench.iterations; i++) {
    current.reset();
    auto current_start = std::chrono::steady_clock::now();
    current_rounds += run_current_stream(current.ip, stream);
    auto current_end = std::chrono::steady_clock::now();
    current_elapsed_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(current_end - current_start).count();

    legacy.reset();
    auto legacy_start = std::chrono::steady_clock::now();
    legacy_rounds += run_legacy_stream(legacy.ip, stream);
    auto legacy_end = std::chrono::steady_clock::now();
    legacy_elapsed_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(legacy_end - legacy_start).count();
  }

  std::cout << "case=" << bench.name << " packets=" << bench.packet_count
            << " payload_len=" << bench.payload_len << " iterations=" << bench.iterations
            << " current_avg_ns=" << (current_elapsed_ns / bench.iterations)
            << " legacy_avg_ns=" << (legacy_elapsed_ns / bench.iterations)
            << " current_rounds_avg=" << (current_rounds / bench.iterations)
            << " legacy_rounds_avg=" << (legacy_rounds / bench.iterations) << '\n';
}

}  // namespace

int main() {
  std::filesystem::current_path(TESTSUITE_DIR);
  init_main("etc/config.test", false);
  vm_start();

  std::vector<BenchCase> cases = {
      {"mud_packets_32_payload_16", 32, 16, 5000},
      {"mud_packets_128_payload_16", 128, 16, 1500},
      {"mud_packets_128_payload_128", 128, 128, 800},
  };

  for (const auto &bench : cases) {
    run_case(bench);
  }

  clear_state();
  return 0;
}
