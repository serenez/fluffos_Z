#include <chrono>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <vector>

#include "base/package_api.h"
#include "mainlib.h"
#include "packages/core/call_out.h"
#include "vm/vm.h"

namespace {

object_t *load_call_out_probe() {
  object_t *ob = nullptr;
  current_object = master_ob;

  error_context_t econ{};
  save_context(&econ);
  try {
    ob = clone_object("/clone/call_out_probe", 0);
  } catch (...) {
    restore_context(&econ);
  }
  pop_context(&econ);
  return ob;
}

LPC_INT schedule_string_call_out(object_t *ob, const char *function_name,
                                 std::chrono::milliseconds delay) {
  svalue_t function{};
  function.type = T_STRING;
  function.subtype = STRING_SHARED;
  function.u.string = make_shared_string(function_name);

  auto handle = new_call_out(ob, &function, delay, 0, nullptr, false);
  free_svalue(&function, "call_out_bench");
  return handle;
}

struct BenchCase {
  std::string_view name;
  int stale_count;
  int iterations;
};

struct AllocationBenchCase {
  std::string_view name;
  int callout_count;
  int iterations;
};

struct AllocationBenchResult {
  int64_t avg_ns;
  size_t pooled_entries;
};

void run_find_case(const BenchCase &bench) {
  object_t *probe = load_call_out_probe();
  if (probe == nullptr) {
    std::cerr << "failed to clone /clone/call_out_probe\n";
    return;
  }

  std::vector<LPC_INT> handles;
  handles.reserve(bench.stale_count);
  clear_call_outs();
  auto base_entries = call_out_owner_index_entry_count_for_test();
  auto base_buckets = call_out_owner_index_bucket_count_for_test();

  auto start = std::chrono::steady_clock::now();
  for (int iter = 0; iter < bench.iterations; iter++) {
    handles.clear();
    clear_call_outs();
    for (int i = 0; i < bench.stale_count; i++) {
      handles.push_back(schedule_string_call_out(probe, "nop", std::chrono::seconds(60)));
    }
    for (auto handle : handles) {
      remove_call_out_by_handle(probe, handle);
    }
    schedule_string_call_out(probe, "nop", std::chrono::seconds(60));
    (void)find_call_out(probe, "nop");
  }
  auto end = std::chrono::steady_clock::now();

  auto elapsed_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

  std::cout << "case=" << bench.name << " stale_count=" << bench.stale_count
            << " iterations=" << bench.iterations << " elapsed_ns=" << elapsed_ns
            << " avg_ns=" << (elapsed_ns / bench.iterations)
            << " owner_index_entries_delta="
            << (call_out_owner_index_entry_count_for_test() - base_entries)
            << " owner_index_buckets_delta="
            << (call_out_owner_index_bucket_count_for_test() - base_buckets) << '\n';

  clear_call_outs();
  destruct_object(probe);
  free_object(&probe, "call_out_bench");
}

void schedule_remove_cycle(object_t *probe, int callout_count, std::vector<LPC_INT> *handles) {
  handles->clear();
  for (int i = 0; i < callout_count; i++) {
    handles->push_back(schedule_string_call_out(probe, "nop", std::chrono::seconds(60)));
  }
  for (auto handle : *handles) {
    remove_call_out_by_handle(probe, handle);
  }
}

AllocationBenchResult measure_allocation_case(object_t *probe, const AllocationBenchCase &bench,
                                              bool pooling_enabled) {
  set_call_out_pool_enabled_for_test(pooling_enabled);
  clear_call_outs();
  clear_call_out_pool_for_test();

  std::vector<LPC_INT> handles;
  handles.reserve(bench.callout_count);

  // 先预热一轮，让分配器和池子都进入稳定态。
  schedule_remove_cycle(probe, bench.callout_count, &handles);

  auto start = std::chrono::steady_clock::now();
  for (int iter = 0; iter < bench.iterations; iter++) {
    schedule_remove_cycle(probe, bench.callout_count, &handles);
  }
  auto end = std::chrono::steady_clock::now();

  auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  AllocationBenchResult result{
      elapsed_ns / bench.iterations,
      call_out_pool_entry_count_for_test(),
  };

  clear_call_outs();
  return result;
}

void run_allocation_case(const AllocationBenchCase &bench) {
  object_t *probe = load_call_out_probe();
  if (probe == nullptr) {
    std::cerr << "failed to clone /clone/call_out_probe\n";
    return;
  }

  auto current = measure_allocation_case(probe, bench, true);
  auto legacy = measure_allocation_case(probe, bench, false);
  set_call_out_pool_enabled_for_test(true);

  std::cout << "case=" << bench.name << " callout_count=" << bench.callout_count
            << " iterations=" << bench.iterations << " current_avg_ns=" << current.avg_ns
            << " legacy_avg_ns=" << legacy.avg_ns
            << " pooled_entries_after_warmup=" << current.pooled_entries << '\n';

  destruct_object(probe);
  free_object(&probe, "call_out_bench");
}

}  // namespace

int main() {
  std::filesystem::current_path(TESTSUITE_DIR);
  init_main("etc/config.test", false);
  vm_start();

  std::vector<BenchCase> cases = {
      {"find_after_handle_removed_same_object_10000", 10000, 40},
      {"find_after_handle_removed_same_object_50000", 50000, 10},
  };
  std::vector<AllocationBenchCase> allocation_cases = {
      {"schedule_remove_same_object_10000", 10000, 40},
      {"schedule_remove_same_object_50000", 50000, 10},
  };

  for (const auto &bench : cases) {
    run_find_case(bench);
  }
  for (const auto &bench : allocation_cases) {
    run_allocation_case(bench);
  }

  clear_state();
  return 0;
}
