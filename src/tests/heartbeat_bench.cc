#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

#include "base/package_api.h"
#include "packages/core/heartbeat.h"
#include "vm/internal/base/object.h"
#include "vm/internal/base/program.h"

namespace {

struct DummyHeartbeatObject {
  object_t ob{};
  program_t prog{};

  DummyHeartbeatObject() {
    ob.prog = &prog;
    prog.heart_beat = 0;
  }
};

struct BenchCase {
  std::string_view name;
  int object_count;
  int cycle_count;
  int first_interval;
  int second_interval;
  int second_interval_start;
};

void assign_intervals(const BenchCase &bench, std::vector<DummyHeartbeatObject> &objects) {
  for (int i = 0; i < bench.object_count; i++) {
    int interval = (i >= bench.second_interval_start) ? bench.second_interval : bench.first_interval;
    set_heart_beat(&objects[i].ob, interval);
  }
}

void run_case(const BenchCase &bench) {
  clear_heartbeats();

  std::vector<DummyHeartbeatObject> objects(bench.object_count);
  assign_intervals(bench, objects);

  // First cycle only activates pending entries; start timing after that.
  run_heartbeat_cycle_for_test();

  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < bench.cycle_count; i++) {
    run_heartbeat_cycle_for_test();
  }
  auto end = std::chrono::steady_clock::now();

  auto elapsed_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

  std::cout << "case=" << bench.name << " objects=" << bench.object_count
            << " cycles=" << bench.cycle_count << " elapsed_ns=" << elapsed_ns
            << " avg_cycle_ns=" << (elapsed_ns / bench.cycle_count) << '\n';

  clear_heartbeats();
}

}  // namespace

int main() {
  std::vector<BenchCase> cases = {
      {"uniform_interval_1", 50000, 200, 1, 1, 50000},
      {"uniform_interval_10", 50000, 200, 10, 10, 50000},
      {"uniform_interval_100", 50000, 200, 100, 100, 50000},
      {"mixed_interval_1_10", 50000, 200, 1, 10, 25000},
      {"mixed_interval_1_100", 50000, 200, 1, 100, 25000},
  };

  for (const auto &bench : cases) {
    run_case(bench);
  }

  return 0;
}
