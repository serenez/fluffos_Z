#include "base/std.h"

#include "eval_limit.h"
#include "posix_timers.h"

#include <chrono>

volatile int outoftime = 0;
uint64_t max_eval_cost;

#ifndef __linux__
namespace {
using EvalClock = std::chrono::steady_clock;

thread_local EvalClock::time_point eval_deadline = EvalClock::time_point::max();
thread_local bool eval_deadline_active = false;

EvalClock::time_point make_eval_deadline(uint64_t micros) {
  auto const max_delta =
      std::chrono::duration_cast<std::chrono::microseconds>(EvalClock::duration::max()).count();
  if (micros >= static_cast<uint64_t>(max_delta)) {
    return EvalClock::time_point::max();
  }
  return EvalClock::now() + std::chrono::microseconds(micros);
}
}  // namespace
#endif

void init_eval() {
#ifdef __linux__
  init_posix_timers();
#elif defined(_WIN32)
  // Windows does not provide the POSIX virtual timer used on Linux. Fall back
  // to a monotonic deadline that is checked from the interpreter loop.
#else
  debug_message("WARNING: Platform doesn't support eval limit!\n");
#endif
}

void set_eval(uint64_t etime) {
#ifdef __linux__
  posix_eval_timer_set(etime);
#else
  eval_deadline = make_eval_deadline(etime);
  eval_deadline_active = true;
#endif
  outoftime = 0;
}

void check_eval() {
#ifndef __linux__
  if (!outoftime && eval_deadline_active && EvalClock::now() >= eval_deadline) {
    outoftime = 1;
  }
#endif
}

int64_t get_eval() {
#ifdef __linux__
  return posix_eval_timer_get();
#else
  check_eval();
  if (!eval_deadline_active) {
    return max_eval_cost;
  }
  auto const remaining =
      std::chrono::duration_cast<std::chrono::microseconds>(eval_deadline - EvalClock::now())
          .count();
  return remaining > 0 ? remaining : 0;
#endif
}
