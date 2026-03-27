#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

#include "backend.h"

namespace {

struct BenchCase {
  std::string_view name;
  int event_count;
  int distinct_due_ticks;
  int run_ticks;
};

void run_case(const BenchCase &bench) {
  clear_tick_events();

  volatile std::uint64_t executed = 0;

  auto schedule_start = std::chrono::steady_clock::now();
  for (int i = 0; i < bench.event_count; i++) {
    add_gametick_event(i % bench.distinct_due_ticks,
                       TickEvent::callback_type([&executed] { executed++; }));
  }
  auto schedule_end = std::chrono::steady_clock::now();

  auto dispatch_start = std::chrono::steady_clock::now();
  for (int i = 0; i < bench.run_ticks; i++) {
    run_gametick_events_for_test();
  }
  auto dispatch_end = std::chrono::steady_clock::now();

  auto schedule_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(schedule_end - schedule_start).count();
  auto dispatch_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(dispatch_end - dispatch_start).count();

  std::cout << "case=" << bench.name << " events=" << bench.event_count
            << " distinct_due_ticks=" << bench.distinct_due_ticks
            << " schedule_ns=" << schedule_ns << " dispatch_ns=" << dispatch_ns
            << " schedule_avg_ns=" << (schedule_ns / bench.event_count)
            << " dispatch_avg_ns=" << (dispatch_ns / bench.event_count)
            << " executed=" << executed << '\n';

  clear_tick_events();
}

}  // namespace

int main() {
  std::vector<BenchCase> cases = {
      {"same_tick_50000", 50000, 1, 1},
      {"shared_16_ticks_50000", 50000, 16, 16},
      {"shared_256_ticks_50000", 50000, 256, 256},
  };

  for (const auto &bench : cases) {
    run_case(bench);
  }

  return 0;
}
