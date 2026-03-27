#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "base/package_api.h"
#include "mainlib.h"
#include "packages/core/add_action.h"
#include "vm/internal/simulate.h"
#include "vm/vm.h"

namespace {

object_t *load_probe_object(bool clone);

struct BenchSentenceUser {
  object_t *user = nullptr;

  BenchSentenceUser() {
    user = load_probe_object(true);
    if (user != nullptr) {
      user->flags |= O_ENABLE_COMMANDS;
    }
  }

  ~BenchSentenceUser() {
    if (user != nullptr && !(user->flags & O_DESTRUCTED)) {
      destruct_object(user);
    }
    if (user != nullptr) {
      free_object(&user, "BenchSentenceUser");
    }
  }

  static void link_sentence(object_t *user, sentence_t *sentence) {
    sentence_t **bucket = &user->sent_owners;
    while (*bucket != nullptr && (*bucket)->ob != sentence->ob) {
      bucket = &(*bucket)->owner_head_next;
    }
    if (*bucket != nullptr) {
      sentence->owner_next = *bucket;
      sentence->owner_head_next = (*bucket)->owner_head_next;
      (*bucket)->owner_head_next = nullptr;
      *bucket = sentence;
    } else {
      sentence->owner_next = nullptr;
      sentence->owner_head_next = user->sent_owners;
      user->sent_owners = sentence;
    }
    sentence->prev = nullptr;
    sentence->next = user->sent;
    if (sentence->next != nullptr) {
      sentence->next->prev = sentence;
    }
    user->sent = sentence;
  }

  void prepend_sentence(object_t *owner, const char *verb, const char *function, int flags) {
    auto *sentence = alloc_sentence();
    sentence->ob = owner;
    sentence->verb = make_shared_string(verb);
    sentence->function.s = make_shared_string(function);
    sentence->flags = flags;
    link_sentence(user, sentence);
  }

  int run_command(const std::string &command) {
    std::vector<char> buffer(command.begin(), command.end());
    buffer.push_back('\0');
    return parse_command(buffer.data(), user);
  }
};

object_t *load_probe_object(bool clone) {
  object_t *ob = nullptr;
  current_object = master_ob;

  error_context_t econ{};
  save_context(&econ);
  try {
    ob = clone ? clone_object("/clone/user_parser_probe", 0) : find_object("/clone/user_parser_probe");
  } catch (...) {
    restore_context(&econ);
  }
  pop_context(&econ);
  return ob;
}

struct BenchCase {
  std::string_view name;
  int exact_count;
  bool include_empty_short_fallback;
  std::string command;
  int iterations;
};

void build_exact_tail_hit_case(BenchSentenceUser &user, object_t *probe, int exact_count) {
  for (int i = 0; i < exact_count; i++) {
    auto verb = "verb_" + std::to_string(i);
    user.prepend_sentence(probe, verb.c_str(), "accept", 0);
  }
}

void run_case(const BenchCase &bench, object_t *probe) {
  BenchSentenceUser user;
  if (user.user == nullptr) {
    std::cerr << "failed to clone bench user\n";
    return;
  }
  build_exact_tail_hit_case(user, probe, bench.exact_count);
  if (bench.include_empty_short_fallback) {
    user.prepend_sentence(probe, "", "reject", V_SHORT);
  }

  auto start = std::chrono::steady_clock::now();
  int rc_sum = 0;
  for (int i = 0; i < bench.iterations; i++) {
    rc_sum += user.run_command(bench.command);
  }
  auto end = std::chrono::steady_clock::now();
  auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

  std::cout << "case=" << bench.name << " exact_count=" << bench.exact_count
            << " fallback=" << (bench.include_empty_short_fallback ? 1 : 0)
            << " iterations=" << bench.iterations << " elapsed_ns=" << elapsed_ns
            << " avg_ns=" << (elapsed_ns / bench.iterations) << " rc_sum=" << rc_sum << '\n';
}

}  // namespace

int main() {
  std::filesystem::current_path(TESTSUITE_DIR);
  init_main("etc/config.test", false);
  vm_start();

  auto *probe = load_probe_object(false);
  if (probe == nullptr) {
    std::cerr << "failed to load /clone/user_parser_probe\n";
    return 1;
  }

  std::vector<BenchCase> cases = {
      {"exact_miss_unique_4096", 4096, false, "missing target", 2000},
      {"exact_tail_hit_unique_4096", 4096, false, "verb_0 target", 2000},
      {"short_fallback_plus_exact_tail_hit_4096", 4096, true, "verb_0 target", 2000},
  };

  for (const auto &bench : cases) {
    run_case(bench, probe);
  }

  destruct_object(probe);
  free_object(&probe, "user_parser_bench");
  clear_state();
  return 0;
}
