#include <gtest/gtest.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <array>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "base/package_api.h"

#include "mainlib.h"

#include "base/internal/rc.h"
#include "base/internal/strutils.h"
#include "backend.h"
#include "comm.h"
#include "compiler/internal/compiler.h"
#include "compiler/internal/scratchpad.h"
#include "interactive.h"
#include "packages/core/add_action.h"
#include "packages/core/call_out.h"
#include "packages/core/ed.h"
#include "packages/core/dns.h"
#include "packages/core/heartbeat.h"
#include "packages/gateway/gateway.h"
#include "packages/mudlib_stats/mudlib_stats.h"
#include "packages/ops/parse.h"
#include "packages/parser/parser.h"
#include "packages/sockets/socket_efuns.h"
#include "user.h"
#include "vm/internal/base/mapping.h"
#include "vm/internal/base/array.h"
#include "vm/internal/base/object.h"
#include "vm/internal/base/program.h"
#include "vm/internal/eval_limit.h"
#include "vm/internal/simulate.h"

char *extract_first_command_for_test(interactive_t *ip);
bool schedule_user_logon_for_test(event_base *base, interactive_t *user);
void cleanup_pending_user_for_test(interactive_t *user, bool close_socket);
void reset_user_logon_callback_runs_for_test();
int user_logon_callback_runs_for_test();
void f_remove_action();
#ifdef F_COMPRESS_FILE
void f_compress_file();
#endif
#ifdef F_UNCOMPRESS_FILE
void f_uncompress_file();
#endif
void f_in_edit();

namespace {

std::filesystem::path g_test_binary_dir;

std::streamoff compile_log_size() {
  std::ifstream file("log/compile", std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    return 0;
  }
  return static_cast<std::streamoff>(file.tellg());
}

std::string compile_log_delta(std::streamoff start_offset) {
  std::ifstream file("log/compile", std::ios::binary);
  if (!file.is_open()) {
    return "";
  }

  file.seekg(start_offset);
  std::ostringstream content;
  content << file.rdbuf();
  return content.str();
}

svalue_t *call_lpc_method(object_t *ob, const char *method, int num_args = 0) {
  save_command_giver(ob);
  set_eval(max_eval_cost);
  auto *ret = safe_apply(method, ob, num_args, ORIGIN_DRIVER);
  restore_command_giver();
  return ret;
}

LPC_INT call_lpc_number_method(object_t *ob, const char *method, int num_args = 0) {
  auto *ret = call_lpc_method(ob, method, num_args);
  if (!ret || ret->type != T_NUMBER) {
    return 0;
  }
  return ret->u.number;
}

int heartbeat_gametick_span() {
  return time_to_next_gametick(
      std::chrono::milliseconds(CONFIG_INT(__RC_HEARTBEAT_INTERVAL_MSEC__)));
}

void advance_gameticks(int count) {
  for (int i = 0; i < count; i++) {
    run_gametick_events_for_test();
  }
}

void reset_gametick_queue_for_test() { clear_tick_events(); }

LPC_INT schedule_string_call_out_for_test(object_t *ob, const char *function_name,
                                          std::chrono::milliseconds delay) {
  svalue_t function{};
  function.type = T_STRING;
  function.subtype = STRING_SHARED;
  function.u.string = make_shared_string(function_name);
  auto handle = new_call_out(ob, &function, delay, 0, nullptr, false);
  free_svalue(&function, "schedule_string_call_out_for_test");
  return handle;
}

object_t *load_lpc_object_for_test(const char *path, bool clone = false) {
  object_t *ob = nullptr;

  current_object = master_ob;
  error_context_t econ{};
  save_context(&econ);
  try {
    ob = clone ? clone_object(path, 0) : find_object(path);
  } catch (...) {
    restore_context(&econ);
  }
  pop_context(&econ);
  return ob;
}

std::vector<std::string> lpc_array_to_strings(svalue_t *ret) {
  std::vector<std::string> values;
  if (!ret || ret->type != T_ARRAY || ret->u.arr == nullptr) {
    return values;
  }

  values.reserve(ret->u.arr->size);
  for (int i = 0; i < ret->u.arr->size; i++) {
    if (ret->u.arr->item[i].type == T_STRING && ret->u.arr->item[i].u.string != nullptr) {
      values.emplace_back(ret->u.arr->item[i].u.string);
    }
  }
  return values;
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

  free_svalue(&value, "build_mud_string_packet");
  return packet;
}

array_t *object_vector_to_lpc_array(const std::vector<object_t *> &objects) {
  auto *arr = allocate_empty_array(static_cast<int>(objects.size()));
  for (int i = 0; i < arr->size; i++) {
    svalue_t value{};
    value.type = T_OBJECT;
    value.u.ob = objects[i];
    assign_svalue_no_free(&arr->item[i], &value);
  }
  return arr;
}

void destroy_lpc_object_for_test(object_t **ob, const char *reason) {
  if (ob == nullptr || *ob == nullptr) {
    return;
  }
  if (!((*ob)->flags & O_DESTRUCTED)) {
    destruct_object(*ob);
  }
  free_object(ob, reason);
}

struct ManualCommandUser {
  object_t *user = nullptr;

  ManualCommandUser() {
    user = load_lpc_object_for_test("/clone/user_parser_probe", true);
    if (user != nullptr) {
      call_lpc_method(user, "reset_state");
      user->flags |= O_ENABLE_COMMANDS;
    }
  }

  ~ManualCommandUser() {
    if (user != nullptr && !(user->flags & O_DESTRUCTED)) {
      destruct_object(user);
    }
    if (user != nullptr) {
      free_object(&user, "ManualCommandUser");
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

  sentence_t *prepend_sentence(object_t *owner, const char *verb, const char *function, int flags) {
    auto *sentence = alloc_sentence();
    sentence->ob = owner;
    sentence->verb = make_shared_string(verb);
    sentence->function.s = make_shared_string(function);
    sentence->flags = flags;
    link_sentence(user, sentence);
    return sentence;
  }

  int run_command(const std::string &command) {
    std::vector<char> buffer(command.begin(), command.end());
    buffer.push_back('\0');
    return parse_command(buffer.data(), user);
  }
};

struct MoveChainBenchCase {
  const char *name;
  int room_size;
  int init_every;
  bool item_enabled;
  bool item_has_init;
  bool contents_enabled;
  int iterations;
};

void populate_move_chain_room(object_t *room, const MoveChainBenchCase &bench,
                              std::vector<object_t *> *owned) {
  for (int i = 0; i < bench.room_size; i++) {
    const bool has_init = bench.init_every != 0 && (i % bench.init_every) == 0;
    auto *ob = load_lpc_object_for_test(has_init ? "/clone/move_chain_init" : "/clone/move_chain_noinit", true);
    EXPECT_NE(ob, nullptr);
    if (ob == nullptr) {
      return;
    }
    call_lpc_method(ob, "reset_state");
    if (bench.contents_enabled) {
      ob->flags |= O_ENABLE_COMMANDS;
    }
    move_object(ob, room);
    owned->push_back(ob);
  }
}

int64_t measure_move_chain_case(const MoveChainBenchCase &bench) {
  std::vector<object_t *> owned;
  auto *room_a = load_lpc_object_for_test("/clone/move_chain_noinit", true);
  auto *room_b = load_lpc_object_for_test("/clone/move_chain_noinit", true);
  EXPECT_NE(room_a, nullptr);
  EXPECT_NE(room_b, nullptr);
  if (room_a == nullptr || room_b == nullptr) {
    return 0;
  }
  owned.push_back(room_a);
  owned.push_back(room_b);

  populate_move_chain_room(room_a, bench, &owned);
  populate_move_chain_room(room_b, bench, &owned);

  auto *item =
      load_lpc_object_for_test(bench.item_has_init ? "/clone/move_chain_init" : "/clone/move_chain_noinit", true);
  EXPECT_NE(item, nullptr);
  if (item == nullptr) {
    return 0;
  }
  call_lpc_method(item, "reset_state");
  if (bench.item_enabled) {
    item->flags |= O_ENABLE_COMMANDS;
  }
  owned.push_back(item);
  move_object(item, room_a);

  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < bench.iterations; i++) {
    move_object(item, (i + 1) & 1 ? room_b : room_a);
  }
  auto end = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / bench.iterations;
}

struct RemoveSentBenchCase {
  const char *name;
  int owner_count;
  int actions_per_owner;
  int target_owner_index;
  int iterations;
};

void remove_sent_linear_for_bench(object_t *owner, object_t *user) {
  if (!(user->flags & O_ENABLE_COMMANDS)) {
    return;
  }

  clear_parse_command_cache(user);
  user->sent_owners = nullptr;

  sentence_t *previous = nullptr;
  for (auto *sentence = user->sent; sentence != nullptr;) {
    auto *next = sentence->next;
    if (sentence->ob != owner) {
      previous = sentence;
      sentence = next;
      continue;
    }

    if (previous != nullptr) {
      previous->next = next;
    } else {
      user->sent = next;
    }
    if (next != nullptr) {
      next->prev = previous;
    }

    free_sentence(sentence);
    sentence = next;
  }
}

void push_test_string(const std::string &value);

LPC_INT call_remove_action_efun(object_t *owner, object_t *user, const std::string &function,
                                const std::string &verb) {
  auto *saved_current_object = current_object;
  push_test_string(function);
  push_test_string(verb);
  st_num_arg = 2;
  current_object = owner;
  save_command_giver(user);
  f_remove_action();
  restore_command_giver();
  current_object = saved_current_object;
  EXPECT_EQ(T_NUMBER, sp->type);
  auto result = sp->u.number;
  pop_stack();
  return result;
}

int64_t measure_remove_sent_case(const RemoveSentBenchCase &bench, bool legacy_linear = false) {
  if (bench.owner_count <= 0 || bench.actions_per_owner <= 0 || bench.iterations <= 0 ||
      bench.target_owner_index < 0 || bench.target_owner_index >= bench.owner_count) {
    return 0;
  }

  std::vector<object_t *> owners;
  owners.reserve(bench.owner_count);
  std::vector<std::string> verbs;
  verbs.reserve(bench.owner_count * bench.actions_per_owner);

  for (int owner_index = 0; owner_index < bench.owner_count; owner_index++) {
    auto *owner = load_lpc_object_for_test("/clone/user_parser_probe", true);
    EXPECT_NE(owner, nullptr);
    if (owner == nullptr) {
      break;
    }
    call_lpc_method(owner, "reset_state");
    owners.push_back(owner);

    for (int action_index = 0; action_index < bench.actions_per_owner; action_index++) {
      verbs.emplace_back("verb_" + std::to_string(owner_index) + "_" + std::to_string(action_index));
    }
  }

  if (owners.size() != static_cast<size_t>(bench.owner_count)) {
    for (auto *owner : owners) {
      destroy_lpc_object_for_test(&owner, "measure_remove_sent_case partial owner cleanup");
    }
    return 0;
  }

  int64_t total_elapsed_ns = 0;
  for (int iteration = 0; iteration < bench.iterations; iteration++) {
    ManualCommandUser user;
    EXPECT_NE(user.user, nullptr);
    if (user.user == nullptr) {
      break;
    }

    size_t verb_index = 0;
    for (int owner_index = 0; owner_index < bench.owner_count; owner_index++) {
      for (int action_index = 0; action_index < bench.actions_per_owner; action_index++) {
        user.prepend_sentence(owners[owner_index], verbs[verb_index++].c_str(), "record_exact_one", 0);
      }
    }

    auto start = std::chrono::steady_clock::now();
    if (legacy_linear) {
      remove_sent_linear_for_bench(owners[bench.target_owner_index], user.user);
    } else {
      remove_sent(owners[bench.target_owner_index], user.user);
    }
    auto end = std::chrono::steady_clock::now();
    total_elapsed_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  }

  for (auto *owner : owners) {
    destroy_lpc_object_for_test(&owner, "measure_remove_sent_case owner cleanup");
  }

  return total_elapsed_ns / bench.iterations;
}

void configure_parse_command_probe(object_t *ob, int variant, int id_count, int adjective_count,
                                   bool include_shared_noun = true,
                                   bool include_shared_adjectives = true) {
  push_number(variant);
  push_number(id_count);
  push_number(adjective_count);
  push_number(include_shared_noun ? 1 : 0);
  push_number(include_shared_adjectives ? 1 : 0);
  call_lpc_method(ob, "configure_lists", 5);
}

void clear_test_parse_command_users_state() { call_lpc_method(master_ob, "clear_test_parse_command_users_state"); }

void refresh_parser_users_cache() { call_lpc_method(master_ob, "refresh_parser_users_cache"); }

LPC_INT query_test_parse_command_users_calls() {
  return call_lpc_number_method(master_ob, "query_test_parse_command_users_calls");
}

void set_test_parse_command_users_override(const std::vector<object_t *> &objects) {
  auto *arr = object_vector_to_lpc_array(objects);
  push_refed_array(arr);
  call_lpc_method(master_ob, "set_test_parse_command_users_override", 1);
}

void configure_parser_perf_probe(object_t *ob, const std::string &noun, bool living = false,
                                 bool visible = true, bool accessible = true) {
  push_test_string(noun);
  push_number(living ? 1 : 0);
  push_number(visible ? 1 : 0);
  push_number(accessible ? 1 : 0);
  call_lpc_method(ob, "configure_target", 4);
}

void set_parser_handler_remote_livings(object_t *handler, bool enabled) {
  push_number(enabled ? 1 : 0);
  call_lpc_method(handler, "set_remote_livings_enabled", 1);
}

struct ParserPackageResult {
  int matched = 0;
  object_t *target = nullptr;
};

ParserPackageResult run_parser_package_for_test(object_t *handler, const std::string &input,
                                                const std::vector<object_t *> &env,
                                                bool legacy_mode = false) {
  ParserPackageResult result;
  auto *env_array = object_vector_to_lpc_array(env);

  push_test_string(input);
  push_refed_array(env_array);
  set_parser_legacy_master_user_loading_for_test(legacy_mode);
  auto *ret = call_lpc_method(handler, "run_parse_input", 2);
  set_parser_legacy_master_user_loading_for_test(false);

  if (ret && ret->type == T_NUMBER) {
    result.matched = ret->u.number;
  }

  auto *target = call_lpc_method(handler, "query_last_target");
  if (target && target->type == T_OBJECT && target->u.ob != nullptr) {
    result.target = target->u.ob;
  }

  return result;
}

struct ParserPackageBenchCase {
  const char *name;
  int local_count;
  int remote_count;
  bool remote_enabled;
  int iterations;
};

int64_t measure_parser_package_case(const ParserPackageBenchCase &bench, bool legacy_mode) {
  std::vector<object_t *> owned;
  std::vector<object_t *> pinned;
  std::vector<object_t *> env_objects;
  std::vector<object_t *> remote_objects;

  auto *handler = load_lpc_object_for_test("/clone/parser_perf_handler", true);
  EXPECT_NE(handler, nullptr);
  if (handler == nullptr) {
    return 0;
  }
  owned.push_back(handler);
  add_ref(handler, "measure_parser_package_case pinned handler");
  pinned.push_back(handler);
  call_lpc_method(handler, "reset_state");
  set_parser_handler_remote_livings(handler, bench.remote_enabled);

  for (int i = 0; i < bench.local_count; i++) {
    auto *probe = load_lpc_object_for_test("/clone/parser_perf_probe", true);
    EXPECT_NE(probe, nullptr);
    if (probe == nullptr) {
      break;
    }
    call_lpc_method(probe, "reset_state");
    configure_parser_perf_probe(probe, i == 0 ? "token" : "local_" + std::to_string(i));
    env_objects.push_back(probe);
    owned.push_back(probe);
    add_ref(probe, "measure_parser_package_case pinned env");
    pinned.push_back(probe);
  }

  for (int i = 0; i < bench.remote_count; i++) {
    auto *probe = load_lpc_object_for_test("/clone/parser_perf_probe", true);
    EXPECT_NE(probe, nullptr);
    if (probe == nullptr) {
      break;
    }
    call_lpc_method(probe, "reset_state");
    configure_parser_perf_probe(probe, "remote_" + std::to_string(i), true);
    remote_objects.push_back(probe);
    owned.push_back(probe);
    add_ref(probe, "measure_parser_package_case pinned remote");
    pinned.push_back(probe);
  }

  if (env_objects.size() != static_cast<size_t>(bench.local_count) ||
      remote_objects.size() != static_cast<size_t>(bench.remote_count)) {
    for (auto *ob : owned) {
      destroy_lpc_object_for_test(&ob, "measure_parser_package_case partial cleanup");
    }
    return 0;
  }

  clear_test_parse_command_users_state();
  set_test_parse_command_users_override(remote_objects);
  refresh_parser_users_cache();

  auto warmup = run_parser_package_for_test(handler, "inspect token", env_objects, legacy_mode);
  EXPECT_EQ(1, warmup.matched);
  EXPECT_EQ(env_objects.front(), warmup.target);

  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < bench.iterations; i++) {
    auto run = run_parser_package_for_test(handler, "inspect token", env_objects, legacy_mode);
    EXPECT_EQ(1, run.matched);
    EXPECT_EQ(env_objects.front(), run.target);
  }
  auto end = std::chrono::steady_clock::now();

  clear_test_parse_command_users_state();
  refresh_parser_users_cache();

  for (auto *ob : owned) {
    destroy_lpc_object_for_test(&ob, "measure_parser_package_case cleanup");
  }

  return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / bench.iterations;
}

struct ParseCommandResult {
  int matched = 0;
  LPC_INT numeral = -1;
  int object_matches = 0;
};

ParseCommandResult run_parse_command_efun_for_test(const std::string &command, object_t *source,
                                                   const char *pattern, bool legacy_mode = false) {
  ParseCommandResult result;
  svalue_t source_value{};
  svalue_t output{};

  source_value.type = T_OBJECT;
  source_value.u.ob = source;
  output.type = T_INVALID;

  set_parse_command_legacy_mode_for_test(legacy_mode);
  result.matched = parse(command.c_str(), &source_value, pattern, &output, 1);
  set_parse_command_legacy_mode_for_test(false);

  if (result.matched && output.type == T_ARRAY && output.u.arr != nullptr && output.u.arr->size > 0) {
    if (output.u.arr->item[0].type == T_NUMBER) {
      result.numeral = output.u.arr->item[0].u.number;
    }
    result.object_matches = output.u.arr->size - 1;
  }

  free_svalue(&output, "run_parse_command_efun_for_test");
  return result;
}

struct ParseCommandBenchCase {
  const char *name;
  int object_count;
  int id_count;
  int adjective_count;
  int iterations;
};

int64_t measure_parse_command_case(const ParseCommandBenchCase &bench, bool legacy_mode) {
  std::vector<object_t *> owned;
  auto *room = load_lpc_object_for_test("/clone/move_chain_noinit", true);
  EXPECT_NE(room, nullptr);
  if (room == nullptr) {
    return 0;
  }
  owned.push_back(room);

  for (int i = 0; i < bench.object_count; i++) {
    auto *probe = load_lpc_object_for_test("/clone/parse_command_probe", true);
    EXPECT_NE(probe, nullptr);
    if (probe == nullptr) {
      break;
    }
    call_lpc_method(probe, "reset_state");
    configure_parse_command_probe(probe, i, bench.id_count, bench.adjective_count, true, true);
    move_object(probe, room);
    owned.push_back(probe);
  }

  if (owned.size() != static_cast<size_t>(bench.object_count + 1)) {
    for (auto *ob : owned) {
      destroy_lpc_object_for_test(&ob, "measure_parse_command_case partial cleanup");
    }
    return 0;
  }

  constexpr auto kPattern = " 'get' %i ";
  const std::string command = "get ancient bronze tokens";
  auto warmup = run_parse_command_efun_for_test(command, room, kPattern, legacy_mode);
  EXPECT_EQ(1, warmup.matched);
  EXPECT_EQ(0, warmup.numeral);
  EXPECT_EQ(bench.object_count, warmup.object_matches);

  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < bench.iterations; i++) {
    auto run = run_parse_command_efun_for_test(command, room, kPattern, legacy_mode);
    EXPECT_EQ(1, run.matched);
    EXPECT_EQ(0, run.numeral);
    EXPECT_EQ(bench.object_count, run.object_matches);
  }
  auto end = std::chrono::steady_clock::now();

  for (auto *ob : owned) {
    destroy_lpc_object_for_test(&ob, "measure_parse_command_case cleanup");
  }

  return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / bench.iterations;
}

void push_test_string(const std::string &value) {
  push_malloced_string(string_copy(value.c_str(), "DriverTest::push_test_string"));
}

void clear_master_error_state() {
  ASSERT_NE(master_ob, nullptr);
  call_lpc_method(master_ob, "clear_last_error");
}

std::string query_master_error_string_field(const char *key) {
  push_constant_string(key);
  auto *ret = call_lpc_method(master_ob, "query_last_error_field", 1);
  if (!ret || ret->type != T_STRING || ret->u.string == nullptr) {
    return {};
  }
  return ret->u.string;
}

LPC_INT query_master_error_int_field(const char *key) {
  push_constant_string(key);
  auto *ret = call_lpc_method(master_ob, "query_last_error_field", 1);
  if (!ret || ret->type != T_NUMBER) {
    return 0;
  }
  return ret->u.number;
}

void set_test_login_object(const char *path) {
  push_constant_string(path);
  call_lpc_method(master_ob, "set_test_login_ob", 1);
}

void reset_test_login_object() { call_lpc_method(master_ob, "reset_test_login_ob"); }

void set_test_domain_override(const char *value) {
  push_constant_string(value);
  call_lpc_method(master_ob, "set_test_domain_override", 1);
}

void set_test_author_override(const char *value) {
  push_constant_string(value);
  call_lpc_method(master_ob, "set_test_author_override", 1);
}

void reset_test_stat_overrides() { call_lpc_method(master_ob, "reset_test_stat_overrides"); }

void clear_test_input_snapshot() { call_lpc_method(master_ob, "clear_test_input_snapshot"); }

std::string query_test_input_snapshot() {
  auto *ret = call_lpc_method(master_ob, "query_test_input_snapshot");
  if (!ret || ret->type != T_STRING || ret->u.string == nullptr) {
    return {};
  }
  return ret->u.string;
}

std::vector<std::string> query_test_input_history_snapshot() {
  std::vector<std::string> history;
  auto *ret = call_lpc_method(master_ob, "query_test_input_history_snapshot");
  if (!ret || ret->type != T_ARRAY || ret->u.arr == nullptr) {
    return history;
  }

  history.reserve(ret->u.arr->size);
  for (int i = 0; i < ret->u.arr->size; i++) {
    if (ret->u.arr->item[i].type == T_STRING && ret->u.arr->item[i].u.string != nullptr) {
      history.emplace_back(ret->u.arr->item[i].u.string);
    }
  }
  return history;
}

#ifdef F_COMPRESS_FILE
LPC_INT call_compress_file_efun(const std::string &input_file, const std::string *output_file = nullptr) {
  push_test_string(input_file);
  st_num_arg = 1;
  if (output_file != nullptr) {
    push_test_string(*output_file);
    st_num_arg = 2;
  }
  f_compress_file();
  EXPECT_EQ(T_NUMBER, sp->type);
  auto result = sp->u.number;
  pop_stack();
  return result;
}
#endif

#ifdef F_UNCOMPRESS_FILE
LPC_INT call_uncompress_file_efun(const std::string &input_file,
                                  const std::string *output_file = nullptr) {
  push_test_string(input_file);
  st_num_arg = 1;
  if (output_file != nullptr) {
    push_test_string(*output_file);
    st_num_arg = 2;
  }
  f_uncompress_file();
  EXPECT_EQ(T_NUMBER, sp->type);
  auto result = sp->u.number;
  pop_stack();
  return result;
}
#endif

void write_test_file(const std::string &path, const std::string &content) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(file.is_open());
  file << content;
  file.close();
}

struct CommandResult {
  int exit_code = -1;
  std::string output;
};

std::filesystem::path src_binary_path(const std::string &name) {
  return g_test_binary_dir.parent_path() / name;
}

std::filesystem::path tool_binary_path(const std::string &name) {
  return src_binary_path("tools") / name;
}

struct CurrentWorkingDirectoryGuard {
  explicit CurrentWorkingDirectoryGuard(const std::filesystem::path &next)
      : active(!next.empty()), previous(active ? std::filesystem::current_path() : std::filesystem::path()) {
    if (active) {
      std::filesystem::current_path(next);
    }
  }

  ~CurrentWorkingDirectoryGuard() {
    if (active) {
      std::filesystem::current_path(previous);
    }
  }

  bool active = false;
  std::filesystem::path previous;
};

struct ScopedTestDirectory {
  explicit ScopedTestDirectory(const std::string &prefix) {
    path = std::filesystem::current_path() /
           (prefix + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path);
  }

  ~ScopedTestDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }

  std::filesystem::path path;
};

CommandResult run_command_capture(const std::filesystem::path &executable,
                                 const std::vector<std::string> &args = {},
                                 const std::filesystem::path &working_directory = {}) {
  CommandResult result;
  std::string command = "\"" + executable.string() + "\"";
  for (const auto &arg : args) {
    command += " " + arg;
  }
  CurrentWorkingDirectoryGuard working_directory_guard(working_directory);
#ifdef _WIN32
  const auto capture_file = g_test_binary_dir / "tool_command_capture.txt";
  std::remove(capture_file.string().c_str());
  result.exit_code =
      std::system(("cmd /c \"" + command + " > \"" + capture_file.string() + "\" 2>&1\"").c_str());
  std::ifstream file(capture_file, std::ios::binary);
  if (file.is_open()) {
    std::ostringstream content;
    content << file.rdbuf();
    result.output = content.str();
  }
  std::remove(capture_file.string().c_str());
#else
  FILE *pipe = popen((command + " 2>&1").c_str(), "r");
  if (pipe == nullptr) {
    return result;
  }

  std::array<char, 256> buffer{};
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    result.output += buffer.data();
  }
  result.exit_code = pclose(pipe);
#endif
  return result;
}

std::string take_interactive_output(interactive_t *ip) {
  if (ip == nullptr || ip->ev_buffer == nullptr) {
    return {};
  }

  auto *output = bufferevent_get_output(ip->ev_buffer);
  auto length = evbuffer_get_length(output);
  std::string result(length, '\0');
  if (length > 0) {
    evbuffer_remove(output, result.data(), length);
  }
  return result;
}

void start_ed_session(object_t *ob, const char *filename) {
  save_command_giver(ob);
  set_eval(max_eval_cost);
  ed_start(filename, nullptr, nullptr, 0, nullptr, 20);
  restore_command_giver();
}

void run_ed_command(object_t *ob, std::string command) {
  save_command_giver(ob);
  set_eval(max_eval_cost);
  ed_cmd(command.data());
  restore_command_giver();
}

std::string query_in_edit_path(object_t *ob) {
  push_object(ob);
  f_in_edit();

  std::string result;
  if (sp->type == T_STRING && sp->u.string != nullptr) {
    result = sp->u.string;
  }
  pop_stack();
  return result;
}

LPC_INT call_gateway_config_number(const char *key) {
  push_constant_string(key);
  st_num_arg = 1;
  f_gateway_config();
  EXPECT_EQ(T_NUMBER, sp->type);
  auto result = sp->u.number;
  pop_stack();
  return result;
}

LPC_INT call_gateway_config_number(const char *key, LPC_INT value) {
  push_constant_string(key);
  push_number(value);
  st_num_arg = 2;
  f_gateway_config();
  EXPECT_EQ(T_NUMBER, sp->type);
  auto result = sp->u.number;
  pop_stack();
  return result;
}

void noop_timer_callback(evutil_socket_t, short, void *) {}

struct ConfigIntGuard {
  explicit ConfigIntGuard(int key, int value) : index(key - RC_BASE_CONFIG_INT), previous(config_int[index]) {
    config_int[index] = value;
  }

  ~ConfigIntGuard() { config_int[index] = previous; }

  int index;
  int previous;
};

}  // namespace

// Test fixture class
class DriverTest : public ::testing::Test {
 public:
  static void SetUpTestSuite() {
    g_test_binary_dir = std::filesystem::current_path();
    chdir(TESTSUITE_DIR);
    // Initialize libevent, This should be done before executing LPC.
    init_main("etc/config.test", false);
    vm_start();
  }

 protected:
  void SetUp() override {
    scratch_destroy();
    clear_state();
    clear_tick_events();
    clear_heartbeats();
  }

  void TearDown() override {
    scratch_destroy();
    clear_heartbeats();
    clear_tick_events();
    clear_state();
  }
};

TEST_F(DriverTest, TestCompileDumpProgWorks) {
  current_object = master_ob;
  const char* file = "single/master.c";
  struct object_t* obj = nullptr;

  error_context_t econ{};
  save_context(&econ);
  try {
    obj = find_object(file);
  } catch (...) {
    restore_context(&econ);
    FAIL();
  }
  pop_context(&econ);

  ASSERT_NE(obj, nullptr);
  ASSERT_NE(obj->prog, nullptr);

  dump_prog(obj->prog, stdout, 1 | 2);

  free_object(&obj, "DriverTest::TestCompileDumpProgWorks");
}

TEST_F(DriverTest, TestInMemoryCompileFile) {
  program_t* prog = nullptr;

  std::istringstream source("void test() {}");
  auto stream = std::make_unique<IStreamLexStream>(source);
  prog = compile_file(std::move(stream), "test");

  ASSERT_NE(prog, nullptr);
  deallocate_program(prog);
}

TEST_F(DriverTest, TestInMemoryCompileFileFail) {
  auto compile_log_offset = compile_log_size();
  program_t* prog = nullptr;
  std::istringstream source("aksdljfaljdfiasejfaeslfjsaef");
  auto stream = std::make_unique<IStreamLexStream>(source);
  prog = compile_file(std::move(stream), "test");

  ASSERT_EQ(prog, nullptr);
  auto compile_log = compile_log_delta(compile_log_offset);
  ASSERT_NE(compile_log.find("/test line 1, column 29: syntax error"), std::string::npos);
  ASSERT_NE(compile_log.find("aksdljfaljdfiasejfaeslfjsaef"), std::string::npos);
}

TEST_F(DriverTest, TestSyntaxErrorInIncludedHeaderReportsHeaderLine) {
  auto compile_log_offset = compile_log_size();
  struct object_t* obj = nullptr;

  current_object = master_ob;
  error_context_t econ{};
  save_context(&econ);
  try {
    obj = find_object("/single/tests/compiler/fail/line_error_include");
  } catch (...) {
    restore_context(&econ);
  }
  pop_context(&econ);

  ASSERT_EQ(obj, nullptr);
  auto compile_log = compile_log_delta(compile_log_offset);
  ASSERT_NE(compile_log.find("/include/line_error_bad_header.h line 2, column 18: syntax error"),
            std::string::npos);
  ASSERT_NE(compile_log.find("int header_broken"), std::string::npos);
  ASSERT_EQ(compile_log.find("inherit F_CLEAN_UP;"), std::string::npos);
}

TEST_F(DriverTest, TestSyntaxErrorAtEndOfFileReportsSourceLineContext) {
  auto compile_log_offset = compile_log_size();
  struct object_t* obj = nullptr;

  current_object = master_ob;
  error_context_t econ{};
  save_context(&econ);
  try {
    obj = find_object("/single/tests/compiler/fail/line_error_direct");
  } catch (...) {
    restore_context(&econ);
  }
  pop_context(&econ);

  ASSERT_EQ(obj, nullptr);
  auto compile_log = compile_log_delta(compile_log_offset);
  ASSERT_NE(compile_log.find("/single/tests/compiler/fail/line_error_direct.c line 1, column 18: syntax error"),
            std::string::npos);
  ASSERT_NE(compile_log.find("broken_identifier"), std::string::npos);
}

TEST_F(DriverTest, TestCallOtherTypeErrorHandlesLongFunctionName) {
  ConfigIntGuard type_check_guard(__RC_CALL_OTHER_TYPE_CHECK__, 1);
  ConfigIntGuard warn_guard(__RC_CALL_OTHER_WARN__, 0);
  const std::string object_path = "/clone/call_other_long_name_helper";

  object_t *obj = nullptr;
  current_object = master_ob;

  error_context_t econ{};
  save_context(&econ);
  try {
    obj = find_object(object_path.c_str());
  } catch (...) {
    restore_context(&econ);
  }
  pop_context(&econ);

  ASSERT_NE(obj, nullptr);

  auto *name_ret = call_lpc_method(obj, "query_long_function_name");
  ASSERT_NE(name_ret, nullptr);
  ASSERT_EQ(T_STRING, name_ret->type);
  auto long_function_name = std::string(name_ret->u.string);

  auto *ret = call_lpc_method(obj, "trigger");
  ASSERT_NE(ret, nullptr);
  ASSERT_EQ(T_STRING, ret->type);

  auto message = std::string(ret->u.string);
  ASSERT_NE(message.find("Bad argument 1 in call to"), std::string::npos);
  ASSERT_NE(message.find(long_function_name), std::string::npos);
  ASSERT_NE(message.find("Expected: string Got int."), std::string::npos);
}

TEST_F(DriverTest, TestGetLineNumberHandlesLongFilename) {
  auto long_file_name = std::string(320, 'l');
  std::istringstream source("void test() {}\n");
  auto stream = std::make_unique<IStreamLexStream>(source);
  auto *prog = compile_file(std::move(stream), long_file_name.c_str());

  ASSERT_NE(prog, nullptr);

  auto location = std::string(get_line_number(prog->program, prog));
  ASSERT_NE(location.find(long_file_name), std::string::npos);
  ASSERT_NE(location.find(':'), std::string::npos);

  deallocate_program(prog);
}

TEST_F(DriverTest, TestEdTracksLongFilenamesWithoutTruncation) {
  object_t *ob = nullptr;
  interactive_t *ip = nullptr;
  event_base *base = nullptr;

  current_object = master_ob;
  error_context_t econ{};
  save_context(&econ);
  try {
    ob = load_object("/clone/user", 1);
  } catch (...) {
    restore_context(&econ);
  }
  pop_context(&econ);

  ASSERT_NE(ob, nullptr);
  ob->flags |= O_IS_WIZARD;
  ip = user_add();
  ASSERT_NE(ip, nullptr);
  base = event_base_new();
  ASSERT_NE(base, nullptr);
  ip->ev_buffer = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
  ASSERT_NE(ip->ev_buffer, nullptr);
  ob->interactive = ip;
  ip->ob = ob;

  const std::string startup_path = "/" + std::string(320, 's');
  const std::string command_name(360, 'c');
  const std::string command_path = "/" + command_name;

  start_ed_session(ob, startup_path.c_str());
  EXPECT_EQ(startup_path, query_in_edit_path(ob));
  take_interactive_output(ip);

  run_ed_command(ob, "f " + command_path);
  take_interactive_output(ip);
  EXPECT_EQ(command_path, query_in_edit_path(ob));

  run_ed_command(ob, "Q");
  ob->interactive = nullptr;
  bufferevent_free(ip->ev_buffer);
  ip->ev_buffer = nullptr;
  user_del(ip);
  interactive_free_text(ip);
  FREE(ip);
  event_base_free(base);
  destruct_object(ob);
  free_object(&ob, "DriverTest::TestEdTracksLongFilenamesWithoutTruncation");
}

TEST_F(DriverTest, TestIncludeResolutionHandlesLongCurrentFilenameSafely) {
  auto compile_log_offset = compile_log_size();
  current_object = master_ob;

  const std::string long_file_name(4600, 'i');
  std::istringstream source("#include \"missing_header.h\"\n");
  auto stream = std::make_unique<IStreamLexStream>(source);
  auto *prog = compile_file(std::move(stream), long_file_name.c_str());

  ASSERT_EQ(prog, nullptr);
  auto compile_log = compile_log_delta(compile_log_offset);
  ASSERT_NE(compile_log.find("Cannot #include missing_header.h"), std::string::npos);
}

TEST_F(DriverTest, TestBuildAppliesReportsMissingInputFileGracefully) {
  auto result = run_command_capture(tool_binary_path("build_applies.exe"), {"missing_source_dir"});

  EXPECT_NE(0, result.exit_code);
  EXPECT_NE(result.output.find("failed to open"), std::string::npos);
  EXPECT_NE(result.output.find("vm/internal/applies"), std::string::npos);
}

TEST_F(DriverTest, TestMakeOptionsDefsHandlesLongMissingIncludeGracefully) {
  const std::string input_file = "preprocessor_long_include_test.h";
  const std::string include_name(1800, 'h');

  std::remove("options.autogen.h");
  std::remove(input_file.c_str());
  write_test_file(input_file, "#include \"" + include_name + "\"\n");

  auto result = run_command_capture(tool_binary_path("make_options_defs.exe"),
                                    {"-I.", "preprocessor_long_include_test.h"});

  EXPECT_NE(0, result.exit_code);
  EXPECT_NE(result.output.find("Cannot #include "), std::string::npos);

  std::remove("options.autogen.h");
  std::remove(input_file.c_str());
}

TEST_F(DriverTest, TestMakeOptionsDefsHandlesLongUnknownDirectiveGracefully) {
  const std::string input_file = "preprocessor_unknown_directive_test.h";
  const std::string directive = "#" + std::string(1800, 'x') + "\n";

  std::remove("options.autogen.h");
  std::remove(input_file.c_str());
  write_test_file(input_file, directive);

  auto result = run_command_capture(tool_binary_path("make_options_defs.exe"),
                                    {"-I.", "preprocessor_unknown_directive_test.h"});

  EXPECT_NE(0, result.exit_code);
  EXPECT_NE(result.output.find("Unrecognised # directive"), std::string::npos);

  std::remove("options.autogen.h");
  std::remove(input_file.c_str());
}

TEST_F(DriverTest, TestO2JsonReportsOutputOpenFailure) {
  ScopedTestDirectory temp_dir("o2json_output_failure");
  const auto input_file = (std::filesystem::current_path() / "test.o").generic_string();
  auto result = run_command_capture(src_binary_path("o2json.exe"),
                                    {input_file, "missing_dir/out.json"}, temp_dir.path);

  EXPECT_NE(0, result.exit_code);
  EXPECT_NE(result.output.find("cannot open output file"), std::string::npos);
}

TEST_F(DriverTest, TestJson2OReportsOutputOpenFailure) {
  ScopedTestDirectory temp_dir("json2o_output_failure");
  const auto input_file = temp_dir.path / "input.json";
  write_test_file(input_file.string(),
                  "{\n"
                  "  \"program_name\": \"/test\",\n"
                  "  \"variables\": []\n"
                  "}\n");
  auto result = run_command_capture(src_binary_path("json2o.exe"),
                                    {input_file.filename().generic_string(), "missing_dir/out.o"},
                                    temp_dir.path);

  EXPECT_NE(0, result.exit_code);
  EXPECT_NE(result.output.find("cannot open output file"), std::string::npos);
}

TEST_F(DriverTest, TestGenerateKeywordsReportsOutputOpenFailure) {
  ScopedTestDirectory temp_dir("generate_keywords_output_failure");
  ASSERT_TRUE(std::filesystem::create_directory(temp_dir.path / "keywords.json"));

  auto result = run_command_capture(src_binary_path("generate_keywords.exe"), {}, temp_dir.path);

  EXPECT_NE(0, result.exit_code);
  EXPECT_NE(result.output.find("cannot open output file keywords.json"), std::string::npos);
}

TEST_F(DriverTest, TestMakeOptionsDefsReportsOutputOpenFailure) {
  ScopedTestDirectory temp_dir("make_options_defs_output_failure");
  write_test_file((temp_dir.path / "options_input.h").string(), "#define LOCAL_OPTION 1\n");
  ASSERT_TRUE(std::filesystem::create_directory(temp_dir.path / "options.autogen.h"));

  auto result = run_command_capture(tool_binary_path("make_options_defs.exe"),
                                    {"-I.", "options_input.h"}, temp_dir.path);

  EXPECT_NE(0, result.exit_code);
  EXPECT_NE(result.output.find("cannot open output file options.autogen.h"), std::string::npos);
}

TEST_F(DriverTest, TestMakeFuncHandlesLongAliasWithoutOverflow) {
  ScopedTestDirectory temp_dir("make_func_long_alias");
  const std::string function_name(90, 'f');
  const std::string alias(90, 'a');
  const std::string long_arg_types = "int|string|object|mapping|mixed|float|function|buffer";
  const std::string spec = "int " + function_name + " " + alias + "(" + long_arg_types + "," +
                           long_arg_types + "," + long_arg_types + "," + long_arg_types + ");\n";
  write_test_file((temp_dir.path / "long_alias.spec").string(), spec);

  auto result = run_command_capture(tool_binary_path("make_func.exe"), {"long_alias.spec"}, temp_dir.path);

  EXPECT_EQ(0, result.exit_code);
  EXPECT_TRUE(std::filesystem::exists(temp_dir.path / "efuns.autogen.cc"));
  EXPECT_TRUE(std::filesystem::exists(temp_dir.path / "efuns.autogen.h"));
}

#ifdef F_COMPRESS_FILE
TEST_F(DriverTest, TestCompressFileRejectsShortInputNameSafely) {
  current_object = master_ob;

  EXPECT_EQ(0, call_compress_file_efun("a"));
  EXPECT_EQ(0, call_compress_file_efun("ab"));
}

#ifdef F_UNCOMPRESS_FILE
TEST_F(DriverTest, TestCompressAndUncompressHandleLongOutputPathsSafely) {
  current_object = master_ob;

  const std::string compress_source_disk = "compress_long_output_source.txt";
  const std::string compress_source_lpc = "/compress_long_output_source.txt";
  const std::string uncompress_source_disk = "uncompress_long_output_source.txt";
  const std::string uncompress_source_lpc = "/uncompress_long_output_source.txt";
  const std::string uncompress_source_gz_lpc = "/uncompress_long_output_source.txt.gz";
  const std::string long_compress_output = "/missing_dir/" + std::string(1100, 'c') + ".gz";
  const std::string long_uncompress_output = "/missing_dir/" + std::string(1100, 'u');

  std::remove(compress_source_disk.c_str());
  std::remove(uncompress_source_disk.c_str());
  std::remove("uncompress_long_output_source.txt.gz");

  write_test_file(compress_source_disk, "compress payload");
  write_test_file(uncompress_source_disk, "uncompress payload");

  EXPECT_EQ(0, call_compress_file_efun(compress_source_lpc, &long_compress_output));
  ASSERT_EQ(1, call_compress_file_efun(uncompress_source_lpc));
  EXPECT_EQ(0, call_uncompress_file_efun(uncompress_source_gz_lpc, &long_uncompress_output));

  std::remove(compress_source_disk.c_str());
  std::remove(uncompress_source_disk.c_str());
  std::remove("uncompress_long_output_source.txt.gz");
}
#endif
#endif

#ifdef F_UNCOMPRESS_FILE
TEST_F(DriverTest, TestUncompressFileRejectsShortInputNameSafely) {
  current_object = master_ob;

  EXPECT_EQ(0, call_uncompress_file_efun("a"));
  EXPECT_EQ(0, call_uncompress_file_efun("ab"));
}
#endif

TEST_F(DriverTest, TestLoadObjectRejectsOverlongInheritedFileSafely) {
  const std::string generated_source = "single/tests/compiler/fail/generated_inherit_too_long.c";
  const std::string object_path = "/single/tests/compiler/fail/generated_inherit_too_long";
  const std::string long_inherit_path = "/" + std::string(MAX_OBJECT_NAME_SIZE + 64, 'i');
  object_t *obj = nullptr;

  current_object = master_ob;
  std::remove(generated_source.c_str());
  write_test_file(generated_source, "inherit \"" + long_inherit_path + "\";\nvoid test() {}\n");

  error_context_t econ{};
  save_context(&econ);
  try {
    obj = find_object(object_path.c_str());
  } catch (...) {
    restore_context(&econ);
  }
  pop_context(&econ);

  EXPECT_EQ(nullptr, obj);

  std::remove(generated_source.c_str());
}

#ifdef PACKAGE_DWLIB
TEST_F(DriverTest, TestReplaceObjectsHandlesLongObjectDescriptions) {
  object_t *helper = nullptr;

  current_object = master_ob;
  error_context_t econ{};
  save_context(&econ);
  try {
    helper = find_object("/clone/dwlib_replace_helper");
  } catch (...) {
    restore_context(&econ);
  }
  pop_context(&econ);

  ASSERT_NE(helper, nullptr);

  auto *ret = call_lpc_method(helper, "run_replace");
  ASSERT_NE(ret, nullptr);
  ASSERT_EQ(T_STRING, ret->type);

  auto replaced = std::string(ret->u.string);
  ASSERT_NE(replaced.find("/clone/dwlib_replace_helper"), std::string::npos);
  ASSERT_NE(replaced.find("(\""), std::string::npos);
  ASSERT_GT(replaced.size(), 2200u);
}
#endif

TEST_F(DriverTest, TestMasterErrorHandlerReceivesCompileDiagnosticFieldsForDirectSyntaxError) {
  struct object_t *obj = nullptr;

  clear_master_error_state();
  current_object = master_ob;
  error_context_t econ{};
  save_context(&econ);
  try {
    obj = find_object("/single/tests/compiler/fail/line_error_direct");
  } catch (...) {
    restore_context(&econ);
  }
  pop_context(&econ);

  ASSERT_EQ(obj, nullptr);
  EXPECT_EQ("/single/tests/compiler/fail/line_error_direct",
            query_master_error_string_field("compile_error_object"));
  EXPECT_EQ("/single/tests/compiler/fail/line_error_direct.c",
            query_master_error_string_field("compile_error_file"));
  EXPECT_EQ(1, query_master_error_int_field("compile_error_line"));
  EXPECT_EQ(18, query_master_error_int_field("compile_error_column"));
  EXPECT_EQ("syntax error, unexpected end of file, expecting L_ASSIGN or ';' or '(' or ','",
            query_master_error_string_field("compile_error_message"));
  EXPECT_EQ("broken_identifier", query_master_error_string_field("compile_error_source"));

  auto caret = query_master_error_string_field("compile_error_caret");
  ASSERT_GT(caret.size(), static_cast<size_t>(17));
  EXPECT_EQ('^', caret[17]);
}

TEST_F(DriverTest, TestMasterErrorHandlerReceivesHeaderDiagnosticFieldsForIncludeFailure) {
  struct object_t *obj = nullptr;

  clear_master_error_state();
  current_object = master_ob;
  error_context_t econ{};
  save_context(&econ);
  try {
    obj = find_object("/single/tests/compiler/fail/line_error_include");
  } catch (...) {
    restore_context(&econ);
  }
  pop_context(&econ);

  ASSERT_EQ(obj, nullptr);
  EXPECT_EQ("/single/tests/compiler/fail/line_error_include",
            query_master_error_string_field("compile_error_object"));
  EXPECT_EQ("/include/line_error_bad_header.h",
            query_master_error_string_field("compile_error_file"));
  EXPECT_EQ(2, query_master_error_int_field("compile_error_line"));
  EXPECT_EQ(18, query_master_error_int_field("compile_error_column"));
  EXPECT_EQ("syntax error, unexpected L_BASIC_TYPE, expecting L_ASSIGN or ';' or '(' or ','",
            query_master_error_string_field("compile_error_message"));
  EXPECT_EQ("int header_broken", query_master_error_string_field("compile_error_source"));

  auto caret = query_master_error_string_field("compile_error_caret");
  ASSERT_GT(caret.size(), static_cast<size_t>(17));
  EXPECT_EQ('^', caret[17]);
}

TEST_F(DriverTest, TestDirectFunctionArgumentTypeErrorHighlightsBadArgument) {
  compile_diagnostic_snapshot_t snapshot{};
  std::istringstream source(
      "#pragma strict_types\n"
      "void notify_line(string prefix, int color, string msg) {}\n"
      "void test() {\n"
      "  notify_line(\"sys\", \"bad\", \"msg\");\n"
      "}\n");

  auto stream = std::make_unique<IStreamLexStream>(source);
  auto *prog = compile_file(std::move(stream), "test");

  ASSERT_EQ(prog, nullptr);
  ASSERT_TRUE(take_last_compile_diagnostic(&snapshot));
  EXPECT_EQ("/test", snapshot.file);
  EXPECT_EQ(4, snapshot.line);
  EXPECT_EQ(22, snapshot.column);
  EXPECT_NE(snapshot.message.find("Bad type for argument 2 of notify_line"), std::string::npos);
  EXPECT_EQ("  notify_line(\"sys\", \"bad\", \"msg\");", snapshot.source_line);
}

TEST_F(DriverTest, TestEfunArgumentTypeErrorHighlightsBadArgument) {
  compile_diagnostic_snapshot_t snapshot{};
  std::istringstream source(
      "#pragma strict_types\n"
      "void test() {\n"
      "  strlen(123);\n"
      "}\n");

  auto stream = std::make_unique<IStreamLexStream>(source);
  auto *prog = compile_file(std::move(stream), "test");

  ASSERT_EQ(prog, nullptr);
  ASSERT_TRUE(take_last_compile_diagnostic(&snapshot));
  EXPECT_EQ("/test", snapshot.file);
  EXPECT_EQ(3, snapshot.line);
  EXPECT_EQ(10, snapshot.column);
  EXPECT_NE(snapshot.message.find("Bad argument 1 to efun strlen()"), std::string::npos);
  EXPECT_EQ("  strlen(123);", snapshot.source_line);
}

TEST_F(DriverTest, TestFunctionalEfunArgumentTypeErrorHighlightsBadArgument) {
  compile_diagnostic_snapshot_t snapshot{};
  std::istringstream source(
      "#pragma strict_types\n"
      "void test() {\n"
      "  function fn = (: strlen, 123 :);\n"
      "}\n");

  auto stream = std::make_unique<IStreamLexStream>(source);
  auto *prog = compile_file(std::move(stream), "test");

  ASSERT_EQ(prog, nullptr);
  ASSERT_TRUE(take_last_compile_diagnostic(&snapshot));
  EXPECT_EQ("/test", snapshot.file);
  EXPECT_EQ(3, snapshot.line);
  EXPECT_EQ(28, snapshot.column);
  EXPECT_NE(snapshot.message.find("Bad argument 1 to efun strlen()"), std::string::npos);
  EXPECT_EQ("  function fn = (: strlen, 123 :);", snapshot.source_line);
}

TEST_F(DriverTest, TestValidLPC_FunctionDeafultArgument) {
  const char* source = R"(
// default case
void test1() {
}

// default case
void test2(int a, int b) {
  ASSERT_EQ(a, 1);
  ASSERT_EQ(b, 2);
}

// varargs
void test3(int a, int* b ...) {
  ASSERT_EQ(a, 1);
  ASSERT_EQ(b[0], 2);
  ASSERT_EQ(b[1], 3);
  ASSERT_EQ(b[2], 4);
  ASSERT_EQ(b[3], 5);
}

// can have multiple trailing arguments with a FP for calculating default value
void test4(int a, string b: (: "str" :), int c: (: 0 :)) {
  switch(a) {
    case 1: {
      ASSERT_EQ("str", b);
      ASSERT_EQ(0, c);
      break;
    }
    case 2: {
      ASSERT_EQ("aaa", b);
      ASSERT_EQ(0, c);
      break;
    }
    case 3: {
      ASSERT_EQ("bbb", b);
      ASSERT_EQ(3, c);
      break;
    }
  }
}

void do_tests() {
    test1();
    test2(1, 2);
    test3(1, 2, 3, 4, 5);
    // direct call
    test4(1);
    test4(2, "aaa");
    test4(3, "bbb", 3);
    // apply
    this_object()->test4(1);
    this_object()->test4(2, "aaa");
    this_object()->test4(3, "bbb", 3);
}
  )";
  std::istringstream iss(source);
  auto stream = std::make_unique<IStreamLexStream>(iss);
  auto *prog = compile_file(std::move(stream), "test");

  ASSERT_NE(prog, nullptr);
  dump_prog(prog, stdout, 1 | 2);
  deallocate_program(prog);
}


TEST_F(DriverTest, TestLPC_FunctionInherit) {
    // Load the inherited object first
    error_context_t econ{};
    save_context(&econ);
    try {
    auto obj = find_object("/single/tests/compiler/function");
    ASSERT_NE(obj , nullptr);

    auto obj2 = find_object("/single/tests/compiler/function_inherit");
    ASSERT_NE(obj2 , nullptr);

    auto obj3 = find_object("/single/tests/compiler/function_inherit_2");
    ASSERT_NE(obj3 , nullptr);

    dump_prog(obj3->prog, stdout, 1 | 2);
    } catch (...) {
        restore_context(&econ);
        FAIL();
    }
    pop_context(&econ);

}

TEST_F(DriverTest, TestHeartbeatIntervalLifecycle) {
  object_t ob{};

  EXPECT_EQ(1, set_heart_beat(&ob, 3));
  EXPECT_TRUE(ob.flags & O_HEART_BEAT);
  EXPECT_EQ(3, query_heart_beat(&ob));

  EXPECT_EQ(1, set_heart_beat(&ob, 5));
  EXPECT_EQ(5, query_heart_beat(&ob));

  EXPECT_EQ(1, set_heart_beat(&ob, 0));
  EXPECT_EQ(0, query_heart_beat(&ob));
  EXPECT_FALSE(ob.flags & O_HEART_BEAT);
}

TEST_F(DriverTest, TestHeartbeatNewEntryActivatesAfterPendingDrain) {
  current_object = master_ob;

  object_t *helper = nullptr;
  error_context_t econ{};
  save_context(&econ);
  try {
    helper = find_object("/clone/heartbeat_probe");
  } catch (...) {
    restore_context(&econ);
  }
  pop_context(&econ);

  ASSERT_NE(helper, nullptr);
  ASSERT_NE(helper->prog, nullptr);
  EXPECT_GT(helper->prog->heart_beat, 0);
  call_lpc_method(helper, "reset_state");

  EXPECT_EQ(1, set_heart_beat(helper, 1));
  EXPECT_EQ(1, query_heart_beat(helper));

  add_gametick_event(0, TickEvent::callback_type(call_heart_beat));

  advance_gameticks(1);
  EXPECT_EQ(0, call_lpc_number_method(helper, "query_hb_count"));
  EXPECT_EQ(1, query_heart_beat(helper));

  advance_gameticks(heartbeat_gametick_span());
  EXPECT_EQ(1, call_lpc_number_method(helper, "query_hb_count"));
  EXPECT_EQ(1, query_heart_beat(helper));

  destruct_object(helper);
  free_object(&helper, "DriverTest::TestHeartbeatNewEntryActivatesAfterPendingDrain");
}

TEST_F(DriverTest, TestHeartbeatSelfDisableEnableDoesNotDuplicateEntries) {
  current_object = master_ob;

  object_t *helper = nullptr;
  error_context_t econ{};
  save_context(&econ);
  try {
    helper = find_object("/clone/heartbeat_probe");
  } catch (...) {
    restore_context(&econ);
  }
  pop_context(&econ);

  ASSERT_NE(helper, nullptr);
  ASSERT_NE(helper->prog, nullptr);
  EXPECT_GT(helper->prog->heart_beat, 0);
  call_lpc_method(helper, "reset_state");

  push_number(1);
  push_number(0);
  push_number(1);
  call_lpc_method(helper, "configure_action", 3);

  EXPECT_EQ(1, set_heart_beat(helper, 1));
  add_gametick_event(0, TickEvent::callback_type(call_heart_beat));

  advance_gameticks(1);
  EXPECT_EQ(0, call_lpc_number_method(helper, "query_hb_count"));

  advance_gameticks(heartbeat_gametick_span());
  EXPECT_EQ(1, call_lpc_number_method(helper, "query_hb_count"));
  EXPECT_EQ(1, call_lpc_number_method(helper, "query_hb_interval"));

  auto *heartbeats_arr = get_heart_beats();
  ASSERT_NE(heartbeats_arr, nullptr);
  EXPECT_EQ(1, heartbeats_arr->size);
  ASSERT_EQ(T_OBJECT, heartbeats_arr->item[0].type);
  EXPECT_EQ(helper, heartbeats_arr->item[0].u.ob);
  free_array(heartbeats_arr);

  check_heartbeats();

  advance_gameticks(heartbeat_gametick_span());
  EXPECT_EQ(2, call_lpc_number_method(helper, "query_hb_count"));
  EXPECT_EQ(1, call_lpc_number_method(helper, "query_hb_interval"));

  destruct_object(helper);
  free_object(&helper, "DriverTest::TestHeartbeatSelfDisableEnableDoesNotDuplicateEntries");
}

TEST_F(DriverTest, TestHeartbeatCanEnableOtherObjectForSameRoundExecution) {
  current_object = master_ob;

  object_t *source = nullptr;
  object_t *target = nullptr;
  error_context_t econ{};
  save_context(&econ);
  try {
    source = find_object("/clone/heartbeat_probe");
    target = clone_object("/clone/heartbeat_probe", 0);
  } catch (...) {
    restore_context(&econ);
  }
  pop_context(&econ);

  ASSERT_NE(source, nullptr);
  ASSERT_NE(target, nullptr);
  call_lpc_method(source, "reset_state");
  call_lpc_method(target, "reset_state");

  push_object(target);
  push_number(1);
  push_number(1);
  call_lpc_method(source, "configure_target_action", 3);

  EXPECT_EQ(1, set_heart_beat(source, 1));
  EXPECT_EQ(1, set_heart_beat(target, 5));

  add_gametick_event(0, TickEvent::callback_type(call_heart_beat));
  advance_gameticks(1);

  advance_gameticks(heartbeat_gametick_span());
  EXPECT_EQ(1, call_lpc_number_method(source, "query_hb_count"));
  EXPECT_EQ(1, call_lpc_number_method(target, "query_hb_count"));
  EXPECT_EQ(1, query_heart_beat(target));

  destruct_object(target);
  free_object(&target, "DriverTest::TestHeartbeatCanEnableOtherObjectForSameRoundExecution target");
  destruct_object(source);
  free_object(&source, "DriverTest::TestHeartbeatCanEnableOtherObjectForSameRoundExecution source");
}

TEST_F(DriverTest, TestHeartbeatCanDelayOtherObjectCurrentRoundExecution) {
  current_object = master_ob;

  object_t *source = nullptr;
  object_t *target = nullptr;
  error_context_t econ{};
  save_context(&econ);
  try {
    source = find_object("/clone/heartbeat_probe");
    target = clone_object("/clone/heartbeat_probe", 0);
  } catch (...) {
    restore_context(&econ);
  }
  pop_context(&econ);

  ASSERT_NE(source, nullptr);
  ASSERT_NE(target, nullptr);
  call_lpc_method(source, "reset_state");
  call_lpc_method(target, "reset_state");

  push_object(target);
  push_number(1);
  push_number(2);
  call_lpc_method(source, "configure_target_action", 3);

  EXPECT_EQ(1, set_heart_beat(source, 1));
  EXPECT_EQ(1, set_heart_beat(target, 1));

  add_gametick_event(0, TickEvent::callback_type(call_heart_beat));
  advance_gameticks(1);

  advance_gameticks(heartbeat_gametick_span());
  EXPECT_EQ(1, call_lpc_number_method(source, "query_hb_count"));
  EXPECT_EQ(0, call_lpc_number_method(target, "query_hb_count"));
  EXPECT_EQ(2, query_heart_beat(target));

  advance_gameticks(heartbeat_gametick_span());
  EXPECT_EQ(1, call_lpc_number_method(target, "query_hb_count"));
  EXPECT_EQ(2, query_heart_beat(target));

  destruct_object(target);
  free_object(&target, "DriverTest::TestHeartbeatCanDelayOtherObjectCurrentRoundExecution target");
  destruct_object(source);
  free_object(&source, "DriverTest::TestHeartbeatCanDelayOtherObjectCurrentRoundExecution source");
}

TEST_F(DriverTest, TestHeartbeatDoesNotPullEarlierSparseObjectIntoCurrentRound) {
  current_object = master_ob;

  object_t *source = nullptr;
  object_t *target = nullptr;
  error_context_t econ{};
  save_context(&econ);
  try {
    source = find_object("/clone/heartbeat_probe");
    target = clone_object("/clone/heartbeat_probe", 0);
  } catch (...) {
    restore_context(&econ);
  }
  pop_context(&econ);

  ASSERT_NE(source, nullptr);
  ASSERT_NE(target, nullptr);
  call_lpc_method(source, "reset_state");
  call_lpc_method(target, "reset_state");

  push_object(target);
  push_number(1);
  push_number(1);
  call_lpc_method(source, "configure_target_action", 3);

  EXPECT_EQ(1, set_heart_beat(target, 5));
  EXPECT_EQ(1, set_heart_beat(source, 1));

  add_gametick_event(0, TickEvent::callback_type(call_heart_beat));
  advance_gameticks(1);

  advance_gameticks(heartbeat_gametick_span());
  EXPECT_EQ(1, call_lpc_number_method(source, "query_hb_count"));
  EXPECT_EQ(0, call_lpc_number_method(target, "query_hb_count"));
  EXPECT_EQ(1, query_heart_beat(target));

  advance_gameticks(heartbeat_gametick_span());
  EXPECT_EQ(1, call_lpc_number_method(target, "query_hb_count"));
  EXPECT_EQ(1, query_heart_beat(target));

  destruct_object(target);
  free_object(&target, "DriverTest::TestHeartbeatDoesNotPullEarlierSparseObjectIntoCurrentRound target");
  destruct_object(source);
  free_object(&source, "DriverTest::TestHeartbeatDoesNotPullEarlierSparseObjectIntoCurrentRound source");
}

TEST_F(DriverTest, TestGametickPreservesInsertionOrderWithinSameTick) {
  reset_gametick_queue_for_test();

  std::vector<int> order;
  add_gametick_event(0, TickEvent::callback_type([&order] { order.push_back(1); }));
  add_gametick_event(0, TickEvent::callback_type([&order] { order.push_back(2); }));
  add_gametick_event(0, TickEvent::callback_type([&order] { order.push_back(3); }));

  run_gametick_events_for_test();

  EXPECT_EQ((std::vector<int>{1, 2, 3}), order);
  reset_gametick_queue_for_test();
}

TEST_F(DriverTest, TestGametickSchedulesNewSameTickEventsAfterCurrentBatch) {
  reset_gametick_queue_for_test();

  std::vector<int> order;
  add_gametick_event(0, TickEvent::callback_type([&order] {
                       order.push_back(1);
                       add_gametick_event(0, TickEvent::callback_type([&order] { order.push_back(3); }));
                     }));
  add_gametick_event(0, TickEvent::callback_type([&order] { order.push_back(2); }));

  run_gametick_events_for_test();

  EXPECT_EQ((std::vector<int>{1, 2, 3}), order);
  reset_gametick_queue_for_test();
}

TEST_F(DriverTest, TestGametickSkipsInvalidatedPendingSameTickEvent) {
  reset_gametick_queue_for_test();

  std::vector<int> order;
  TickEvent *cancelled = nullptr;
  add_gametick_event(0, TickEvent::callback_type([&order, &cancelled] {
                       order.push_back(1);
                       ASSERT_NE(cancelled, nullptr);
                       cancelled->valid = false;
                     }));
  cancelled = add_gametick_event(0, TickEvent::callback_type([&order] { order.push_back(2); }));

  run_gametick_events_for_test();

  EXPECT_EQ((std::vector<int>{1}), order);
  reset_gametick_queue_for_test();
}

TEST_F(DriverTest, TestCallOutByHandleRemovalDoesNotLeaveOwnerIndexGarbage) {
  clear_call_outs();
  auto base_entries = call_out_owner_index_entry_count_for_test();
  auto base_buckets = call_out_owner_index_bucket_count_for_test();

  auto *probe = load_lpc_object_for_test("/clone/call_out_probe", true);
  ASSERT_NE(probe, nullptr);

  auto handle = schedule_string_call_out_for_test(probe, "nop", std::chrono::seconds(60));
  EXPECT_EQ(base_entries + 1, call_out_owner_index_entry_count_for_test());
  EXPECT_EQ(base_buckets + 1, call_out_owner_index_bucket_count_for_test());

  EXPECT_NE(-1, remove_call_out_by_handle(probe, handle));
  EXPECT_EQ(base_entries, call_out_owner_index_entry_count_for_test());
  EXPECT_EQ(base_buckets, call_out_owner_index_bucket_count_for_test());

  destroy_lpc_object_for_test(&probe, "DriverTest::TestCallOutByHandleRemovalDoesNotLeaveOwnerIndexGarbage");
  clear_call_outs();
}

TEST_F(DriverTest, TestExecutedCallOutDoesNotLeaveOwnerIndexGarbage) {
  clear_call_outs();
  auto base_entries = call_out_owner_index_entry_count_for_test();
  auto base_buckets = call_out_owner_index_bucket_count_for_test();

  auto *probe = load_lpc_object_for_test("/clone/call_out_probe", true);
  ASSERT_NE(probe, nullptr);

  schedule_string_call_out_for_test(probe, "nop", std::chrono::milliseconds(0));
  EXPECT_EQ(base_entries + 1, call_out_owner_index_entry_count_for_test());

  run_gametick_events_for_test();

  EXPECT_EQ(1, call_lpc_number_method(probe, "query_fired"));
  EXPECT_EQ(base_entries, call_out_owner_index_entry_count_for_test());
  EXPECT_EQ(base_buckets, call_out_owner_index_bucket_count_for_test());

  destroy_lpc_object_for_test(&probe, "DriverTest::TestExecutedCallOutDoesNotLeaveOwnerIndexGarbage");
  clear_call_outs();
}

TEST_F(DriverTest, TestRemoveAllCallOutClearsOwnerIndexBucket) {
  clear_call_outs();
  auto base_entries = call_out_owner_index_entry_count_for_test();
  auto base_buckets = call_out_owner_index_bucket_count_for_test();

  auto *probe = load_lpc_object_for_test("/clone/call_out_probe", true);
  ASSERT_NE(probe, nullptr);

  schedule_string_call_out_for_test(probe, "nop", std::chrono::seconds(60));
  schedule_string_call_out_for_test(probe, "nop", std::chrono::seconds(60));
  EXPECT_EQ(base_entries + 2, call_out_owner_index_entry_count_for_test());
  EXPECT_EQ(base_buckets + 1, call_out_owner_index_bucket_count_for_test());

  remove_all_call_out(probe);

  EXPECT_EQ(base_entries, call_out_owner_index_entry_count_for_test());
  EXPECT_EQ(base_buckets, call_out_owner_index_bucket_count_for_test());

  destroy_lpc_object_for_test(&probe, "DriverTest::TestRemoveAllCallOutClearsOwnerIndexBucket");
  clear_call_outs();
}

TEST_F(DriverTest, TestCallOutPoolReusesFreedEntriesWhenEnabled) {
  clear_call_outs();
  set_call_out_pool_enabled_for_test(true);
  clear_call_out_pool_for_test();

  auto *probe = load_lpc_object_for_test("/clone/call_out_probe", true);
  ASSERT_NE(nullptr, probe);

  auto first_handle = schedule_string_call_out_for_test(probe, "nop", std::chrono::seconds(60));
  ASSERT_NE(0, first_handle);
  EXPECT_EQ(0u, call_out_pool_entry_count_for_test());

  EXPECT_NE(-1, remove_call_out_by_handle(probe, first_handle));
  EXPECT_EQ(1u, call_out_pool_entry_count_for_test());

  auto second_handle = schedule_string_call_out_for_test(probe, "nop", std::chrono::seconds(60));
  ASSERT_NE(0, second_handle);
  EXPECT_EQ(0u, call_out_pool_entry_count_for_test());
  EXPECT_NE(-1, find_call_out(probe, "nop"));

  EXPECT_NE(-1, remove_call_out_by_handle(probe, second_handle));
  EXPECT_EQ(1u, call_out_pool_entry_count_for_test());

  destroy_lpc_object_for_test(&probe, "DriverTest::TestCallOutPoolReusesFreedEntriesWhenEnabled");
  clear_call_outs();
}

TEST_F(DriverTest, TestCallOutPoolCanBeDisabledForLegacyBenchComparison) {
  clear_call_outs();
  set_call_out_pool_enabled_for_test(false);
  clear_call_out_pool_for_test();

  auto *probe = load_lpc_object_for_test("/clone/call_out_probe", true);
  ASSERT_NE(nullptr, probe);

  auto handle = schedule_string_call_out_for_test(probe, "nop", std::chrono::seconds(60));
  ASSERT_NE(0, handle);
  EXPECT_NE(-1, remove_call_out_by_handle(probe, handle));
  EXPECT_EQ(0u, call_out_pool_entry_count_for_test());

  set_call_out_pool_enabled_for_test(true);
  destroy_lpc_object_for_test(&probe,
                              "DriverTest::TestCallOutPoolCanBeDisabledForLegacyBenchComparison");
  clear_call_outs();
}

TEST_F(DriverTest, TestUserParserPreservesEmptyShortFallbackOrderAgainstExactVerb) {
  auto *probe = load_lpc_object_for_test("/clone/user_parser_probe");
  ASSERT_NE(probe, nullptr);
  call_lpc_method(probe, "reset_state");

  ManualCommandUser user;
  ASSERT_NE(user.user, nullptr);
  user.prepend_sentence(probe, "look", "record_exact_one", 0);
  user.prepend_sentence(probe, "", "record_short_zero", V_SHORT);

  EXPECT_EQ(1, user.run_command("look stone"));

  auto history = lpc_array_to_strings(call_lpc_method(probe, "query_history"));
  ASSERT_EQ(2u, history.size());
  EXPECT_EQ("short:stone", history[0]);
  EXPECT_EQ("exact:stone", history[1]);

  destruct_object(probe);
  free_object(&probe, "DriverTest::TestUserParserPreservesEmptyShortFallbackOrderAgainstExactVerb");
}

TEST_F(DriverTest, TestUserParserPreservesExactBeforePrefixWhenChainRequiresIt) {
  auto *exact_owner = load_lpc_object_for_test("/clone/user_parser_probe");
  auto *prefix_owner = load_lpc_object_for_test("/clone/user_parser_probe", true);
  ASSERT_NE(exact_owner, nullptr);
  ASSERT_NE(prefix_owner, nullptr);
  call_lpc_method(exact_owner, "reset_state");
  call_lpc_method(prefix_owner, "reset_state");

  ManualCommandUser user;
  ASSERT_NE(user.user, nullptr);
  user.prepend_sentence(prefix_owner, "l", "record_short_zero", V_SHORT);
  user.prepend_sentence(exact_owner, "look", "record_exact_one", 0);

  EXPECT_EQ(1, user.run_command("look target"));

  auto exact_history = lpc_array_to_strings(call_lpc_method(exact_owner, "query_history"));
  auto prefix_history = lpc_array_to_strings(call_lpc_method(prefix_owner, "query_history"));
  ASSERT_EQ(1u, exact_history.size());
  EXPECT_EQ("exact:target", exact_history[0]);
  EXPECT_TRUE(prefix_history.empty());

  destruct_object(prefix_owner);
  free_object(&prefix_owner, "DriverTest::TestUserParserPreservesExactBeforePrefixWhenChainRequiresIt prefix");
  destruct_object(exact_owner);
  free_object(&exact_owner, "DriverTest::TestUserParserPreservesExactBeforePrefixWhenChainRequiresIt exact");
}

TEST_F(DriverTest, TestUserParserPreservesExplicitDefaultVerbOrderAgainstExactVerb) {
  auto *probe = load_lpc_object_for_test("/clone/user_parser_probe");
  ASSERT_NE(probe, nullptr);
  call_lpc_method(probe, "reset_state");

  ManualCommandUser user;
  ASSERT_NE(user.user, nullptr);
  user.prepend_sentence(probe, "look", "record_exact_one", 0);
  user.prepend_sentence(probe, "", "record_default_zero", 0);

  EXPECT_EQ(1, user.run_command("look apple"));

  auto history = lpc_array_to_strings(call_lpc_method(probe, "query_history"));
  ASSERT_EQ(2u, history.size());
  EXPECT_EQ("default:apple", history[0]);
  EXPECT_EQ("exact:apple", history[1]);

  destruct_object(probe);
  free_object(&probe, "DriverTest::TestUserParserPreservesExplicitDefaultVerbOrderAgainstExactVerb");
}

TEST_F(DriverTest, TestUserParserHonorsRemoveSentAfterPreviousParse) {
  auto *target = load_lpc_object_for_test("/clone/user_parser_probe", true);
  ASSERT_NE(target, nullptr);
  call_lpc_method(target, "reset_state");

  ManualCommandUser user;
  ASSERT_NE(user.user, nullptr);
  user.prepend_sentence(target, "", "record_short_one", V_SHORT);

  EXPECT_EQ(1, user.run_command("look gem"));

  call_lpc_method(target, "reset_state");
  remove_sent(target, user.user);

  EXPECT_EQ(0, user.run_command("look gem"));

  auto target_history = lpc_array_to_strings(call_lpc_method(target, "query_history"));
  EXPECT_TRUE(target_history.empty());

  destroy_lpc_object_for_test(&target, "DriverTest::TestUserParserHonorsRemoveSentAfterPreviousParse target");
}

TEST_F(DriverTest, TestRemoveSentRemovesOnlyTargetOwnerCommands) {
  auto *target = load_lpc_object_for_test("/clone/user_parser_probe", true);
  auto *survivor = load_lpc_object_for_test("/clone/user_parser_probe", true);
  ASSERT_NE(target, nullptr);
  ASSERT_NE(survivor, nullptr);
  call_lpc_method(target, "reset_state");
  call_lpc_method(survivor, "reset_state");

  ManualCommandUser user;
  ASSERT_NE(user.user, nullptr);
  user.prepend_sentence(target, "look", "record_exact_one", 0);
  user.prepend_sentence(target, "get", "record_exact_one", 0);
  user.prepend_sentence(survivor, "take", "record_exact_one", 0);

  remove_sent(target, user.user);

  EXPECT_EQ(0, user.run_command("look gem"));
  EXPECT_EQ(0, user.run_command("get gem"));
  EXPECT_EQ(1, user.run_command("take gem"));

  auto removed_history = lpc_array_to_strings(call_lpc_method(target, "query_history"));
  auto survivor_history = lpc_array_to_strings(call_lpc_method(survivor, "query_history"));
  EXPECT_TRUE(removed_history.empty());
  ASSERT_EQ(1u, survivor_history.size());
  EXPECT_EQ("exact:gem", survivor_history[0]);

  destroy_lpc_object_for_test(&survivor, "DriverTest::TestRemoveSentRemovesOnlyTargetOwnerCommands survivor");
  destroy_lpc_object_for_test(&target, "DriverTest::TestRemoveSentRemovesOnlyTargetOwnerCommands target");
}

TEST_F(DriverTest, TestRemoveSentHandlesInterleavedOwnerCommands) {
  auto *target = load_lpc_object_for_test("/clone/user_parser_probe", true);
  auto *survivor = load_lpc_object_for_test("/clone/user_parser_probe", true);
  ASSERT_NE(target, nullptr);
  ASSERT_NE(survivor, nullptr);
  call_lpc_method(target, "reset_state");
  call_lpc_method(survivor, "reset_state");

  ManualCommandUser user;
  ASSERT_NE(user.user, nullptr);
  user.prepend_sentence(target, "look", "record_exact_one", 0);
  user.prepend_sentence(survivor, "take", "record_exact_one", 0);
  user.prepend_sentence(target, "get", "record_exact_one", 0);

  remove_sent(target, user.user);

  EXPECT_EQ(0, user.run_command("look gem"));
  EXPECT_EQ(0, user.run_command("get gem"));
  EXPECT_EQ(1, user.run_command("take gem"));

  auto removed_history = lpc_array_to_strings(call_lpc_method(target, "query_history"));
  auto survivor_history = lpc_array_to_strings(call_lpc_method(survivor, "query_history"));
  EXPECT_TRUE(removed_history.empty());
  ASSERT_EQ(1u, survivor_history.size());
  EXPECT_EQ("exact:gem", survivor_history[0]);

  destroy_lpc_object_for_test(&survivor, "DriverTest::TestRemoveSentHandlesInterleavedOwnerCommands survivor");
  destroy_lpc_object_for_test(&target, "DriverTest::TestRemoveSentHandlesInterleavedOwnerCommands target");
}

TEST_F(DriverTest, TestRemoveActionKeepsOwnerBucketsConsistentForLaterRemoveSent) {
  auto *target = load_lpc_object_for_test("/clone/user_parser_probe", true);
  auto *survivor = load_lpc_object_for_test("/clone/user_parser_probe", true);
  ASSERT_NE(target, nullptr);
  ASSERT_NE(survivor, nullptr);
  call_lpc_method(target, "reset_state");
  call_lpc_method(survivor, "reset_state");

  ManualCommandUser user;
  ASSERT_NE(user.user, nullptr);
  user.prepend_sentence(target, "look", "record_exact_one", 0);
  user.prepend_sentence(survivor, "take", "record_exact_one", 0);
  user.prepend_sentence(target, "get", "record_exact_one", 0);

  ASSERT_EQ(1, call_remove_action_efun(target, user.user, "record_exact_one", "look"));
  remove_sent(target, user.user);

  EXPECT_EQ(0, user.run_command("look gem"));
  EXPECT_EQ(0, user.run_command("get gem"));
  EXPECT_EQ(1, user.run_command("take gem"));

  auto removed_history = lpc_array_to_strings(call_lpc_method(target, "query_history"));
  auto survivor_history = lpc_array_to_strings(call_lpc_method(survivor, "query_history"));
  EXPECT_TRUE(removed_history.empty());
  ASSERT_EQ(1u, survivor_history.size());
  EXPECT_EQ("exact:gem", survivor_history[0]);

  destroy_lpc_object_for_test(&survivor,
                              "DriverTest::TestRemoveActionKeepsOwnerBucketsConsistentForLaterRemoveSent survivor");
  destroy_lpc_object_for_test(&target,
                              "DriverTest::TestRemoveActionKeepsOwnerBucketsConsistentForLaterRemoveSent target");
}

TEST_F(DriverTest, TestParseCommandMatchesLegacyPluralWithAdjectives) {
  auto *room = load_lpc_object_for_test("/clone/move_chain_noinit", true);
  ASSERT_NE(room, nullptr);

  std::vector<object_t *> owned;
  owned.push_back(room);
  for (int i = 0; i < 4; i++) {
    auto *probe = load_lpc_object_for_test("/clone/parse_command_probe", true);
    ASSERT_NE(probe, nullptr);
    call_lpc_method(probe, "reset_state");
    configure_parse_command_probe(probe, i, 4, 4, true, true);
    move_object(probe, room);
    owned.push_back(probe);
  }

  auto current = run_parse_command_efun_for_test("get ancient bronze tokens", room, " 'get' %i ");
  auto legacy = run_parse_command_efun_for_test("get ancient bronze tokens", room, " 'get' %i ", true);

  EXPECT_EQ(1, current.matched);
  EXPECT_EQ(legacy.matched, current.matched);
  EXPECT_EQ(legacy.numeral, current.numeral);
  EXPECT_EQ(legacy.object_matches, current.object_matches);
  EXPECT_EQ(0, current.numeral);
  EXPECT_EQ(4, current.object_matches);

  for (auto *ob : owned) {
    destroy_lpc_object_for_test(&ob, "DriverTest::TestParseCommandMatchesLegacyPluralWithAdjectives cleanup");
  }
}

TEST_F(DriverTest, TestParseCommandMatchesLegacyPluralSelection) {
  auto *room = load_lpc_object_for_test("/clone/move_chain_noinit", true);
  ASSERT_NE(room, nullptr);

  std::vector<object_t *> owned;
  owned.push_back(room);
  for (int i = 0; i < 6; i++) {
    auto *probe = load_lpc_object_for_test("/clone/parse_command_probe", true);
    ASSERT_NE(probe, nullptr);
    call_lpc_method(probe, "reset_state");
    configure_parse_command_probe(probe, i, 2, 2, true, false);
    move_object(probe, room);
    owned.push_back(probe);
  }

  auto current = run_parse_command_efun_for_test("get tokens", room, " 'get' %i ");
  auto legacy = run_parse_command_efun_for_test("get tokens", room, " 'get' %i ", true);

  EXPECT_EQ(1, current.matched);
  EXPECT_EQ(legacy.matched, current.matched);
  EXPECT_EQ(legacy.numeral, current.numeral);
  EXPECT_EQ(legacy.object_matches, current.object_matches);
  EXPECT_EQ(0, current.numeral);
  EXPECT_EQ(6, current.object_matches);

  for (auto *ob : owned) {
    destroy_lpc_object_for_test(&ob, "DriverTest::TestParseCommandMatchesLegacyPluralSelection cleanup");
  }
}

TEST_F(DriverTest, TestParserAddRuleDoesNotFetchMasterUsers) {
  clear_test_parse_command_users_state();
  refresh_parser_users_cache();

  auto *handler = load_lpc_object_for_test("/clone/parser_perf_handler", true);
  ASSERT_NE(handler, nullptr);

  EXPECT_EQ(0, query_test_parse_command_users_calls());

  clear_test_parse_command_users_state();
  refresh_parser_users_cache();
  destroy_lpc_object_for_test(&handler, "DriverTest::TestParserAddRuleDoesNotFetchMasterUsers");
}

TEST_F(DriverTest, TestParserSkipsRemoteUsersWhenVerbDoesNotNeedThem) {
  auto *handler = load_lpc_object_for_test("/clone/parser_perf_handler", true);
  auto *local = load_lpc_object_for_test("/clone/parser_perf_probe", true);
  auto *remote = load_lpc_object_for_test("/clone/parser_perf_probe", true);
  ASSERT_NE(handler, nullptr);
  ASSERT_NE(local, nullptr);
  ASSERT_NE(remote, nullptr);

  call_lpc_method(handler, "reset_state");
  set_parser_handler_remote_livings(handler, false);
  call_lpc_method(local, "reset_state");
  call_lpc_method(remote, "reset_state");
  configure_parser_perf_probe(local, "token");
  configure_parser_perf_probe(remote, "wanderer", true);

  std::vector<object_t *> env_objects = {local};
  std::vector<object_t *> remote_objects = {remote};

  clear_test_parse_command_users_state();
  set_test_parse_command_users_override(remote_objects);
  refresh_parser_users_cache();

  auto current = run_parser_package_for_test(handler, "inspect token", env_objects);
  EXPECT_EQ(1, current.matched);
  EXPECT_EQ(local, current.target);
  EXPECT_EQ(0, query_test_parse_command_users_calls());

  clear_test_parse_command_users_state();
  set_test_parse_command_users_override(remote_objects);
  refresh_parser_users_cache();

  auto legacy = run_parser_package_for_test(handler, "inspect token", env_objects, true);
  EXPECT_EQ(1, legacy.matched);
  EXPECT_EQ(local, legacy.target);
  EXPECT_EQ(1, query_test_parse_command_users_calls());

  clear_test_parse_command_users_state();
  refresh_parser_users_cache();
  destroy_lpc_object_for_test(&remote, "DriverTest::TestParserSkipsRemoteUsersWhenVerbDoesNotNeedThem remote");
  destroy_lpc_object_for_test(&local, "DriverTest::TestParserSkipsRemoteUsersWhenVerbDoesNotNeedThem local");
  destroy_lpc_object_for_test(&handler, "DriverTest::TestParserSkipsRemoteUsersWhenVerbDoesNotNeedThem handler");
}

TEST_F(DriverTest, TestParserRemoteUsersRemainReachableWhenEnabled) {
  auto *handler = load_lpc_object_for_test("/clone/parser_perf_handler", true);
  auto *local = load_lpc_object_for_test("/clone/parser_perf_probe", true);
  auto *remote = load_lpc_object_for_test("/clone/parser_perf_probe", true);
  ASSERT_NE(handler, nullptr);
  ASSERT_NE(local, nullptr);
  ASSERT_NE(remote, nullptr);

  call_lpc_method(handler, "reset_state");
  set_parser_handler_remote_livings(handler, true);
  call_lpc_method(local, "reset_state");
  call_lpc_method(remote, "reset_state");
  configure_parser_perf_probe(local, "token");
  configure_parser_perf_probe(remote, "wanderer", true);

  std::vector<object_t *> env_objects = {local};
  std::vector<object_t *> remote_objects = {remote};

  clear_test_parse_command_users_state();
  set_test_parse_command_users_override(remote_objects);
  refresh_parser_users_cache();

  auto current = run_parser_package_for_test(handler, "inspect wanderer", env_objects);

  clear_test_parse_command_users_state();
  set_test_parse_command_users_override(remote_objects);
  refresh_parser_users_cache();

  auto legacy = run_parser_package_for_test(handler, "inspect wanderer", env_objects, true);
  EXPECT_EQ(1, current.matched);
  EXPECT_EQ(1, legacy.matched);
  EXPECT_EQ(remote, current.target);
  EXPECT_EQ(remote, legacy.target);

  clear_test_parse_command_users_state();
  refresh_parser_users_cache();
  destroy_lpc_object_for_test(&remote, "DriverTest::TestParserRemoteUsersRemainReachableWhenEnabled remote");
  destroy_lpc_object_for_test(&local, "DriverTest::TestParserRemoteUsersRemainReachableWhenEnabled local");
  destroy_lpc_object_for_test(&handler, "DriverTest::TestParserRemoteUsersRemainReachableWhenEnabled handler");
}

TEST_F(DriverTest, TestMoveObjectEnabledItemCallsInitObjectsInDestination) {
  auto *room = load_lpc_object_for_test("/clone/move_chain_noinit", true);
  auto *with_init = load_lpc_object_for_test("/clone/move_chain_init", true);
  auto *without_init = load_lpc_object_for_test("/clone/move_chain_noinit", true);
  auto *item = load_lpc_object_for_test("/clone/move_chain_noinit", true);
  ASSERT_NE(room, nullptr);
  ASSERT_NE(with_init, nullptr);
  ASSERT_NE(without_init, nullptr);
  ASSERT_NE(item, nullptr);

  call_lpc_method(with_init, "reset_state");
  call_lpc_method(without_init, "reset_state");
  move_object(with_init, room);
  move_object(without_init, room);

  item->flags |= O_ENABLE_COMMANDS;
  move_object(item, room);

  EXPECT_EQ(1, call_lpc_number_method(with_init, "query_init_calls"));
  EXPECT_EQ(0, call_lpc_number_method(without_init, "query_init_calls"));

  destroy_lpc_object_for_test(&item, "DriverTest::TestMoveObjectEnabledItemCallsInitObjectsInDestination item");
  destroy_lpc_object_for_test(&without_init,
                              "DriverTest::TestMoveObjectEnabledItemCallsInitObjectsInDestination without_init");
  destroy_lpc_object_for_test(&with_init, "DriverTest::TestMoveObjectEnabledItemCallsInitObjectsInDestination with_init");
  destroy_lpc_object_for_test(&room, "DriverTest::TestMoveObjectEnabledItemCallsInitObjectsInDestination room");
}

TEST_F(DriverTest, TestMoveObjectEnabledSiblingCallsMovedItemInit) {
  auto *room = load_lpc_object_for_test("/clone/move_chain_noinit", true);
  auto *enabled_sibling = load_lpc_object_for_test("/clone/move_chain_noinit", true);
  auto *item = load_lpc_object_for_test("/clone/move_chain_init", true);
  ASSERT_NE(room, nullptr);
  ASSERT_NE(enabled_sibling, nullptr);
  ASSERT_NE(item, nullptr);

  call_lpc_method(item, "reset_state");
  enabled_sibling->flags |= O_ENABLE_COMMANDS;
  move_object(enabled_sibling, room);
  move_object(item, room);

  EXPECT_EQ(1, call_lpc_number_method(item, "query_init_calls"));

  destroy_lpc_object_for_test(&item, "DriverTest::TestMoveObjectEnabledSiblingCallsMovedItemInit item");
  destroy_lpc_object_for_test(&enabled_sibling,
                              "DriverTest::TestMoveObjectEnabledSiblingCallsMovedItemInit sibling");
  destroy_lpc_object_for_test(&room, "DriverTest::TestMoveObjectEnabledSiblingCallsMovedItemInit room");
}

TEST_F(DriverTest, TestMoveObjectLargeNoInitRoomSingleMoveCompletes) {
  MoveChainBenchCase bench{"large_noinit_single_move", 64, 0, true, false, false, 8};
  EXPECT_GT(measure_move_chain_case(bench), 0);
}

TEST_F(DriverTest, DISABLED_PerfMoveObjectEnabledItemRoomNoInit256) {
  MoveChainBenchCase bench{"enabled_item_room_noinit_256", 256, 0, true, false, false, 1000};
  auto avg_ns = measure_move_chain_case(bench);
  std::cout << "case=" << bench.name << " avg_ns=" << avg_ns << std::endl;
}

TEST_F(DriverTest, DISABLED_PerfMoveObjectEnabledItemRoomMixed256InitEvery8) {
  MoveChainBenchCase bench{"enabled_item_room_mixed_256_init_every_8", 256, 8, true, false, false, 1000};
  auto avg_ns = measure_move_chain_case(bench);
  std::cout << "case=" << bench.name << " avg_ns=" << avg_ns << std::endl;
}

TEST_F(DriverTest, DISABLED_PerfMoveObjectDisabledNoInitItemRoomEnabled64) {
  MoveChainBenchCase bench{"disabled_noinit_item_room_enabled_64", 64, 0, false, false, true, 2000};
  auto avg_ns = measure_move_chain_case(bench);
  std::cout << "case=" << bench.name << " avg_ns=" << avg_ns << std::endl;
}

TEST_F(DriverTest, DISABLED_PerfRemoveSentOwners256Actions8) {
  RemoveSentBenchCase bench{"remove_sent_owners_256_actions_8", 256, 8, 127, 1000};
  auto avg_ns = measure_remove_sent_case(bench);
  auto legacy_avg_ns = measure_remove_sent_case(bench, true);
  std::cout << "case=" << bench.name << " avg_ns=" << avg_ns << " legacy_avg_ns=" << legacy_avg_ns
            << std::endl;
}

TEST_F(DriverTest, DISABLED_PerfRemoveSentOwners1024Actions1) {
  RemoveSentBenchCase bench{"remove_sent_owners_1024_actions_1", 1024, 1, 511, 1000};
  auto avg_ns = measure_remove_sent_case(bench);
  auto legacy_avg_ns = measure_remove_sent_case(bench, true);
  std::cout << "case=" << bench.name << " avg_ns=" << avg_ns << " legacy_avg_ns=" << legacy_avg_ns
            << std::endl;
}

TEST_F(DriverTest, DISABLED_PerfParseCommandRoomShared256Ids8Adjs4) {
  ParseCommandBenchCase bench{"parse_command_room_shared_256_ids_8_adjs_4", 256, 8, 4, 500};
  auto avg_ns = measure_parse_command_case(bench, false);
  auto legacy_avg_ns = measure_parse_command_case(bench, true);
  std::cout << "case=" << bench.name << " avg_ns=" << avg_ns << " legacy_avg_ns=" << legacy_avg_ns
            << std::endl;
}

TEST_F(DriverTest, DISABLED_PerfParseCommandRoomShared512Ids4Adjs8) {
  ParseCommandBenchCase bench{"parse_command_room_shared_512_ids_4_adjs_8", 512, 4, 8, 300};
  auto avg_ns = measure_parse_command_case(bench, false);
  auto legacy_avg_ns = measure_parse_command_case(bench, true);
  std::cout << "case=" << bench.name << " avg_ns=" << avg_ns << " legacy_avg_ns=" << legacy_avg_ns
            << std::endl;
}

TEST_F(DriverTest, DISABLED_PerfParserNoRemoteUsersOverride2048) {
  ParserPackageBenchCase bench{"parser_no_remote_users_override_2048", 1, 2048, false, 200};
  auto avg_ns = measure_parser_package_case(bench, false);
  auto legacy_avg_ns = measure_parser_package_case(bench, true);
  std::cout << "case=" << bench.name << " avg_ns=" << avg_ns << " legacy_avg_ns=" << legacy_avg_ns
            << std::endl;
}

TEST_F(DriverTest, DISABLED_PerfParserRemoteUsersOverride2048) {
  ParserPackageBenchCase bench{"parser_remote_users_override_2048", 64, 2048, true, 120};
  auto avg_ns = measure_parser_package_case(bench, false);
  auto legacy_avg_ns = measure_parser_package_case(bench, true);
  std::cout << "case=" << bench.name << " avg_ns=" << avg_ns << " legacy_avg_ns=" << legacy_avg_ns
            << std::endl;
}

TEST_F(DriverTest, TestGatewaySessionDestroyReleasesInteractiveReference) {
  svalue_t login_data{};
  login_data.type = T_NUMBER;
  login_data.u.number = 0;

  set_test_login_object("/clone/gateway_login_example");
  auto *ob =
      gateway_create_session_internal(1, "unit-test-session", &login_data, "127.0.0.1", 9527);
  reset_test_login_object();

  ASSERT_NE(ob, nullptr);
  ASSERT_NE(ob->interactive, nullptr);
  EXPECT_TRUE(ob->interactive->iflags & GATEWAY_SESSION);

  add_ref(ob, "DriverTest::TestGatewaySessionDestroyReleasesInteractiveReference");
  auto ref_before_destroy = ob->ref;

  ASSERT_EQ(1, gateway_destroy_session_internal("unit-test-session", "unit_test", "done"));
  ASSERT_EQ(nullptr, ob->interactive);
  EXPECT_EQ(ref_before_destroy - 1, ob->ref);

  destruct_object(ob);
  free_object(&ob, "DriverTest::TestGatewaySessionDestroyReleasesInteractiveReference");
}

TEST_F(DriverTest, TestInteractiveBufferRestoredToOneMiB) {
  EXPECT_EQ(1 * 1024 * 1024, MAX_TEXT);
}

TEST_F(DriverTest, TestInteractiveStateDoesNotEmbedFullMaxTextBuffer) {
  EXPECT_LT(sizeof(interactive_t), static_cast<size_t>(64 * 1024));
}

TEST_F(DriverTest, TestInteractiveBufferStartsSmallAndCanGrowOnDemand) {
  auto *ip = user_add();
  ASSERT_NE(ip, nullptr);
  ASSERT_NE(ip->text, nullptr);
  EXPECT_EQ(INTERACTIVE_TEXT_INITIAL_CAPACITY, ip->text_capacity);
  EXPECT_LT(ip->text_capacity, MAX_TEXT);

  EXPECT_TRUE(interactive_ensure_text_capacity(ip, 128 * 1024));
  EXPECT_GE(ip->text_capacity, 128 * 1024);
  EXPECT_LE(ip->text_capacity, MAX_TEXT);
  EXPECT_FALSE(interactive_ensure_text_capacity(ip, MAX_TEXT + 1));

  user_del(ip);
  interactive_free_text(ip);
  FREE(ip);
}

TEST_F(DriverTest, TestGatewaySessionStartsWithSmallSharedInputBuffer) {
  svalue_t login_data{};
  login_data.type = T_NUMBER;
  login_data.u.number = 0;

  set_test_login_object("/clone/gateway_login_example");
  auto *ob = gateway_create_session_internal(1, "unit-test-small-buffer", &login_data,
                                             "127.0.0.1", 9527);
  reset_test_login_object();

  ASSERT_NE(ob, nullptr);
  ASSERT_NE(ob->interactive, nullptr);
  EXPECT_EQ(INTERACTIVE_TEXT_INITIAL_CAPACITY, ob->interactive->text_capacity);
  EXPECT_LT(ob->interactive->text_capacity, MAX_TEXT);

  ASSERT_EQ(1, gateway_destroy_session_internal("unit-test-small-buffer", "unit_test", "done"));
  destruct_object(ob);
}

TEST_F(DriverTest, TestGatewaySessionExecLogonKeepsSessionLookupWorking) {
  svalue_t login_data{};
  login_data.type = T_NUMBER;
  login_data.u.number = 0;

  set_test_login_object("/clone/gateway_login_exec_example");
  auto *ob = gateway_create_session_internal(1, "unit-test-session-exec", &login_data,
                                             "127.0.0.1", 9527);
  reset_test_login_object();

  ASSERT_NE(ob, nullptr);
  ASSERT_NE(ob->interactive, nullptr);
  EXPECT_TRUE(ob->interactive->iflags & GATEWAY_SESSION);
  ASSERT_NE(ob->obname, nullptr);
  EXPECT_NE(std::string::npos, std::string(ob->obname).find("clone/gateway_exec_user"));

  auto *session_info = call_lpc_method(ob, "query_gateway_session_snapshot");
  ASSERT_NE(session_info, nullptr);
  EXPECT_EQ(T_MAPPING, session_info->type);

  auto *real_ip = gateway_get_ip(ob);
  ASSERT_NE(real_ip, nullptr);
  EXPECT_STREQ("127.0.0.1", real_ip);

  ASSERT_EQ(1, gateway_destroy_session_internal("unit-test-session-exec", "unit_test", "done"));
  destruct_object(ob);
  free_object(&ob, "DriverTest::TestGatewaySessionExecLogonKeepsSessionLookupWorking");
}

TEST_F(DriverTest, TestMudPacketCompletionRequiresFullPayloadBytes) {
  constexpr int kPacketSize = 128 * 1024;

  EXPECT_FALSE(mud_packet_is_complete(kPacketSize, 4));
  EXPECT_FALSE(mud_packet_is_complete(kPacketSize, 64 * 1024 + 4));
  EXPECT_FALSE(mud_packet_is_complete(kPacketSize, kPacketSize + 3));
  EXPECT_TRUE(mud_packet_is_complete(kPacketSize, kPacketSize + 4));
}

TEST_F(DriverTest, TestGetUserDataMudProcessesMultiplePacketsFromSingleBufferedRead) {
  auto *ob = load_lpc_object_for_test("/clone/input_capture_user");
  ASSERT_NE(ob, nullptr);
  call_lpc_method(ob, "clear_inputs");

  auto *ip = user_add();
  ASSERT_NE(ip, nullptr);
  ASSERT_NE(ip->text, nullptr);

  ip->ob = ob;
  ip->connection_type = PORT_TYPE_MUD;
  ob->interactive = ip;

  auto packet1 = build_mud_string_packet("first");
  auto packet2 = build_mud_string_packet("second");
  auto packets = packet1 + packet2;

  EXPECT_EQ(2, process_mud_chunk_for_test(
                   ip, reinterpret_cast<const unsigned char *>(packets.data()),
                   static_cast<int>(packets.size())));

  auto history = lpc_array_to_strings(call_lpc_method(ob, "query_input_history"));
  ASSERT_EQ(2u, history.size());
  EXPECT_EQ("first", history[0]);
  EXPECT_EQ("second", history[1]);
  EXPECT_EQ(0, ip->text_end);

  ob->interactive = nullptr;
  user_del(ip);
  interactive_free_text(ip);
  FREE(ip);
  destroy_lpc_object_for_test(&ob, "DriverTest.TestGetUserDataMudProcessesMultiplePacketsFromSingleBufferedRead");
}

TEST_F(DriverTest, TestGetUserDataMudPreservesPartialSecondPacketAcrossReads) {
  auto *ob = load_lpc_object_for_test("/clone/input_capture_user");
  ASSERT_NE(ob, nullptr);
  call_lpc_method(ob, "clear_inputs");

  auto *ip = user_add();
  ASSERT_NE(ip, nullptr);
  ASSERT_NE(ip->text, nullptr);

  ip->ob = ob;
  ip->connection_type = PORT_TYPE_MUD;
  ob->interactive = ip;

  auto packet1 = build_mud_string_packet("first");
  auto packet2 = build_mud_string_packet("second");
  size_t const packet2_split = packet2.size() / 2;

  auto first_chunk = packet1 + std::string(packet2.data(), packet2_split);
  EXPECT_EQ(1, process_mud_chunk_for_test(
                   ip, reinterpret_cast<const unsigned char *>(first_chunk.data()),
                   static_cast<int>(first_chunk.size())));

  auto history = lpc_array_to_strings(call_lpc_method(ob, "query_input_history"));
  ASSERT_EQ(1u, history.size());
  EXPECT_EQ("first", history[0]);
  EXPECT_GT(ip->text_end, 0);

  auto second_chunk = std::string(packet2.data() + packet2_split, packet2.size() - packet2_split);
  EXPECT_EQ(1, process_mud_chunk_for_test(
                   ip, reinterpret_cast<const unsigned char *>(second_chunk.data()),
                   static_cast<int>(second_chunk.size())));

  history = lpc_array_to_strings(call_lpc_method(ob, "query_input_history"));
  ASSERT_EQ(2u, history.size());
  EXPECT_EQ("second", history[1]);
  EXPECT_EQ(0, ip->text_end);

  ob->interactive = nullptr;
  user_del(ip);
  interactive_free_text(ip);
  FREE(ip);
  destroy_lpc_object_for_test(&ob, "DriverTest.TestGetUserDataMudPreservesPartialSecondPacketAcrossReads");
}

TEST_F(DriverTest, TestGetUserDataMudContinuesAfterExecTransfersInteractive) {
  auto *ob = load_lpc_object_for_test("/clone/input_capture_user");
  ASSERT_NE(ob, nullptr);
  call_lpc_method(ob, "clear_inputs");
  call_lpc_method(ob, "enable_exec_on_input");

  auto *ip = user_add();
  ASSERT_NE(ip, nullptr);
  ASSERT_NE(ip->text, nullptr);

  ip->ob = ob;
  ip->connection_type = PORT_TYPE_MUD;
  ob->interactive = ip;

  auto packet1 = build_mud_string_packet("first");
  auto packet2 = build_mud_string_packet("second");
  auto packets = packet1 + packet2;

  EXPECT_EQ(2, process_mud_chunk_for_test(
                   ip, reinterpret_cast<const unsigned char *>(packets.data()),
                   static_cast<int>(packets.size())));

  auto *active_ob = ip->ob;
  ASSERT_NE(active_ob, nullptr);
  ASSERT_NE(active_ob, ob);
  add_ref(active_ob, "DriverTest.TestGetUserDataMudContinuesAfterExecTransfersInteractive");

  auto history = lpc_array_to_strings(call_lpc_method(active_ob, "query_input_history"));
  ASSERT_EQ(2u, history.size());
  EXPECT_EQ("first", history[0]);
  EXPECT_EQ("second", history[1]);

  active_ob->interactive = nullptr;
  user_del(ip);
  interactive_free_text(ip);
  FREE(ip);
  if (!(active_ob->flags & O_DESTRUCTED)) {
    destruct_object(active_ob);
  }
  free_object(&active_ob, "DriverTest.TestGetUserDataMudContinuesAfterExecTransfersInteractive");
  ob = nullptr;
}

TEST_F(DriverTest, TestGetUserDataMudStopsAfterProcessInputDestructsObject) {
  auto *ob = load_lpc_object_for_test("/clone/input_capture_user");
  ASSERT_NE(ob, nullptr);
  add_ref(ob, "DriverTest.TestGetUserDataMudStopsAfterProcessInputDestructsObject");
  call_lpc_method(ob, "clear_inputs");
  clear_test_input_snapshot();
  call_lpc_method(ob, "enable_destroy_on_input");

  auto *ip = user_add();
  ASSERT_NE(ip, nullptr);
  ASSERT_NE(ip->text, nullptr);

  ip->ob = ob;
  ip->connection_type = PORT_TYPE_MUD;
  ob->interactive = ip;

  auto packet1 = build_mud_string_packet("quit");
  auto packet2 = build_mud_string_packet("next");
  auto packets = packet1 + packet2;

  EXPECT_EQ(1, process_mud_chunk_for_test(
                   ip, reinterpret_cast<const unsigned char *>(packets.data()),
                   static_cast<int>(packets.size())));

  EXPECT_TRUE(ob->flags & O_DESTRUCTED);
  auto history = query_test_input_history_snapshot();
  ASSERT_EQ(1u, history.size());
  EXPECT_EQ("quit", history[0]);
  EXPECT_EQ("quit", query_test_input_snapshot());

  if (!(ob->flags & O_DESTRUCTED)) {
    if (ip != nullptr) {
      if (ob->interactive == ip) {
        ob->interactive = nullptr;
      }
      user_del(ip);
      interactive_free_text(ip);
      FREE(ip);
    }
    destruct_object(ob);
  }

  free_object(&ob, "DriverTest.TestGetUserDataMudStopsAfterProcessInputDestructsObject");
  clear_test_input_snapshot();
}

TEST_F(DriverTest, TestExtractFirstCommandPreservesLastBufferedLineCommand) {
  auto *ip = user_add();
  ASSERT_NE(ip, nullptr);
  ASSERT_NE(ip->text, nullptr);

  std::memcpy(ip->text, "look\n", 5);
  ip->text_end = 5;
  ip->text_start = 0;

  auto *command = extract_first_command_for_test(ip);
  ASSERT_NE(command, nullptr);
  EXPECT_STREQ("look", command);

  user_del(ip);
  interactive_free_text(ip);
  FREE(ip);
}

TEST_F(DriverTest, TestCmdInBufCachesBufferedCommandEndAcrossExtraction) {
  auto *ip = user_add();
  ASSERT_NE(ip, nullptr);
  ASSERT_NE(ip->text, nullptr);

  std::memcpy(ip->text, "look\nsay\n", 9);
  ip->text_end = 9;
  ip->text_start = 0;

  EXPECT_EQ(-1, cached_command_end_for_test(ip));
  EXPECT_TRUE(cmd_in_buf(ip));
  EXPECT_EQ(4, cached_command_end_for_test(ip));

  auto *first = extract_first_command_for_test(ip);
  ASSERT_NE(first, nullptr);
  EXPECT_STREQ("look", first);
  EXPECT_EQ(8, cached_command_end_for_test(ip));

  auto *second = extract_first_command_for_test(ip);
  ASSERT_NE(second, nullptr);
  EXPECT_STREQ("say", second);
  EXPECT_EQ(-1, cached_command_end_for_test(ip));

  user_del(ip);
  interactive_free_text(ip);
  FREE(ip);
}

TEST_F(DriverTest, TestOnUserInputBackspaceInvalidatesCachedCommandEnd) {
  auto *ip = user_add();
  ASSERT_NE(ip, nullptr);
  ASSERT_NE(ip->text, nullptr);

  on_user_input(ip, "look\n", 5);
  EXPECT_TRUE(cmd_in_buf(ip));
  EXPECT_EQ(4, cached_command_end_for_test(ip));

  on_user_input(ip, "\b", 1);
  EXPECT_EQ(-1, cached_command_end_for_test(ip));
  EXPECT_FALSE(cmd_in_buf(ip));
  EXPECT_EQ(-1, cached_command_end_for_test(ip));

  user_del(ip);
  interactive_free_text(ip);
  FREE(ip);
}

TEST_F(DriverTest, TestProcessUserCommandKeepsLastLineBufferedCommandIntact) {
  object_t *ob = nullptr;

  current_object = master_ob;
  error_context_t econ{};
  save_context(&econ);
  try {
    ob = find_object("/clone/input_capture_user");
  } catch (...) {
    restore_context(&econ);
  }
  pop_context(&econ);

  ASSERT_NE(ob, nullptr);

  auto *base = event_base_new();
  ASSERT_NE(base, nullptr);

  auto *ip = user_add();
  ASSERT_NE(ip, nullptr);
  ASSERT_NE(ip->text, nullptr);

  ip->ob = ob;
  ip->connection_type = PORT_TYPE_ASCII;
  ip->prompt = "> ";
  ip->iflags = CMD_IN_BUF | HAS_PROCESS_INPUT;
  ip->ev_buffer = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
  ASSERT_NE(ip->ev_buffer, nullptr);
  ob->interactive = ip;

  std::memcpy(ip->text, "look\n", 5);
  ip->text_end = 5;
  ip->text_start = 0;

  EXPECT_EQ(1, process_user_command(ip));

  auto *ret = call_lpc_method(ob, "query_last_input");
  ASSERT_NE(ret, nullptr);
  ASSERT_EQ(T_STRING, ret->type);
  ASSERT_NE(ret->u.string, nullptr);
  EXPECT_STREQ("look", ret->u.string);

  ob->interactive = nullptr;
  bufferevent_free(ip->ev_buffer);
  ip->ev_buffer = nullptr;
  user_del(ip);
  interactive_free_text(ip);
  FREE(ip);
  destruct_object(ob);
  free_object(&ob, "DriverTest::TestProcessUserCommandKeepsLastLineBufferedCommandIntact");
  event_base_free(base);
}

TEST_F(DriverTest, TestAsciiChunkProcessesEmptyLineSafely) {
  object_t *ob = nullptr;

  current_object = master_ob;
  error_context_t econ{};
  save_context(&econ);
  try {
    ob = find_object("/clone/input_capture_user");
  } catch (...) {
    restore_context(&econ);
  }
  pop_context(&econ);

  ASSERT_NE(ob, nullptr);
  call_lpc_method(ob, "clear_inputs");

  auto *ip = user_add();
  ASSERT_NE(ip, nullptr);
  ASSERT_NE(ip->text, nullptr);
  ip->ob = ob;
  ob->interactive = ip;

  const unsigned char chunk[] = {'\n'};
  EXPECT_EQ(1, process_ascii_chunk_for_test(ip, chunk, sizeof(chunk)));

  auto *ret = call_lpc_method(ob, "query_last_input");
  ASSERT_NE(ret, nullptr);
  ASSERT_EQ(T_STRING, ret->type);
  ASSERT_NE(ret->u.string, nullptr);
  EXPECT_STREQ("", ret->u.string);

  ob->interactive = nullptr;
  user_del(ip);
  interactive_free_text(ip);
  FREE(ip);
  destruct_object(ob);
  free_object(&ob, "DriverTest::TestAsciiChunkProcessesEmptyLineSafely");
}

TEST_F(DriverTest, TestAsciiChunkProcessesCommandAfterCrLfInSameRead) {
  object_t *ob = nullptr;

  current_object = master_ob;
  error_context_t econ{};
  save_context(&econ);
  try {
    ob = find_object("/clone/input_capture_user");
  } catch (...) {
    restore_context(&econ);
  }
  pop_context(&econ);

  ASSERT_NE(ob, nullptr);
  call_lpc_method(ob, "clear_inputs");

  auto *ip = user_add();
  ASSERT_NE(ip, nullptr);
  ASSERT_NE(ip->text, nullptr);
  ip->ob = ob;
  ob->interactive = ip;

  const unsigned char chunk[] = {'l', 'o', 'o', 'k', '\r', '\n', 's', 'a', 'y', '\n'};
  EXPECT_EQ(2, process_ascii_chunk_for_test(ip, chunk, sizeof(chunk)));

  auto *ret = call_lpc_method(ob, "query_input_history");
  ASSERT_NE(ret, nullptr);
  ASSERT_EQ(T_ARRAY, ret->type);
  ASSERT_NE(ret->u.arr, nullptr);
  ASSERT_EQ(2, ret->u.arr->size);
  ASSERT_EQ(T_STRING, ret->u.arr->item[0].type);
  ASSERT_EQ(T_STRING, ret->u.arr->item[1].type);
  EXPECT_STREQ("look", ret->u.arr->item[0].u.string);
  EXPECT_STREQ("say", ret->u.arr->item[1].u.string);

  ob->interactive = nullptr;
  user_del(ip);
  interactive_free_text(ip);
  FREE(ip);
  destruct_object(ob);
  free_object(&ob, "DriverTest::TestAsciiChunkProcessesCommandAfterCrLfInSameRead");
}

TEST_F(DriverTest, TestAsciiChunkProcessesBareCarriageReturn) {
  object_t *ob = nullptr;

  current_object = master_ob;
  error_context_t econ{};
  save_context(&econ);
  try {
    ob = find_object("/clone/input_capture_user");
  } catch (...) {
    restore_context(&econ);
  }
  pop_context(&econ);

  ASSERT_NE(ob, nullptr);
  call_lpc_method(ob, "clear_inputs");

  auto *ip = user_add();
  ASSERT_NE(ip, nullptr);
  ASSERT_NE(ip->text, nullptr);
  ip->ob = ob;
  ob->interactive = ip;

  const unsigned char chunk[] = {'l', 'o', 'o', 'k', '\r'};
  EXPECT_EQ(1, process_ascii_chunk_for_test(ip, chunk, sizeof(chunk)));

  auto *ret = call_lpc_method(ob, "query_last_input");
  ASSERT_NE(ret, nullptr);
  ASSERT_EQ(T_STRING, ret->type);
  ASSERT_NE(ret->u.string, nullptr);
  EXPECT_STREQ("look", ret->u.string);

  ob->interactive = nullptr;
  user_del(ip);
  interactive_free_text(ip);
  FREE(ip);
  destruct_object(ob);
  free_object(&ob, "DriverTest::TestAsciiChunkProcessesBareCarriageReturn");
}

TEST_F(DriverTest, TestAsciiChunkContinuesAfterExecTransfersInteractive) {
  object_t *ob = nullptr;

  current_object = master_ob;
  error_context_t econ{};
  save_context(&econ);
  try {
    ob = find_object("/clone/input_capture_user");
  } catch (...) {
    restore_context(&econ);
  }
  pop_context(&econ);

  ASSERT_NE(ob, nullptr);
  call_lpc_method(ob, "clear_inputs");
  call_lpc_method(ob, "enable_exec_on_input");

  auto *ip = user_add();
  ASSERT_NE(ip, nullptr);
  ASSERT_NE(ip->text, nullptr);
  ip->ob = ob;
  ob->interactive = ip;

  const unsigned char chunk[] = {'f', 'i', 'r', 's', 't', '\n', 's', 'e', 'c', 'o', 'n', 'd', '\n'};
  EXPECT_EQ(2, process_ascii_chunk_for_test(ip, chunk, sizeof(chunk)));

  auto *active_ob = ip->ob;
  ASSERT_NE(active_ob, nullptr);
  ASSERT_NE(active_ob, ob);
  add_ref(active_ob, "DriverTest::TestAsciiChunkContinuesAfterExecTransfersInteractive");

  auto *ret = call_lpc_method(active_ob, "query_input_history");
  ASSERT_NE(ret, nullptr);
  ASSERT_EQ(T_ARRAY, ret->type);
  ASSERT_NE(ret->u.arr, nullptr);
  ASSERT_EQ(2, ret->u.arr->size);
  EXPECT_STREQ("first", ret->u.arr->item[0].u.string);
  EXPECT_STREQ("second", ret->u.arr->item[1].u.string);

  active_ob->interactive = nullptr;
  user_del(ip);
  interactive_free_text(ip);
  FREE(ip);
  ip = nullptr;
  if (!(active_ob->flags & O_DESTRUCTED)) {
    destruct_object(active_ob);
  }
  free_object(&active_ob, "DriverTest::TestAsciiChunkContinuesAfterExecTransfersInteractive");
  ob = nullptr;
}

TEST_F(DriverTest, TestAsciiChunkStopsAfterProcessInputDestructsObject) {
  object_t *ob = nullptr;

  current_object = master_ob;
  error_context_t econ{};
  save_context(&econ);
  try {
    ob = find_object("/clone/input_capture_user");
  } catch (...) {
    restore_context(&econ);
  }
  pop_context(&econ);

  ASSERT_NE(ob, nullptr);
  add_ref(ob, "DriverTest::TestAsciiChunkStopsAfterProcessInputDestructsObject");
  call_lpc_method(ob, "clear_inputs");
  clear_test_input_snapshot();
  call_lpc_method(ob, "enable_destroy_on_input");

  auto *ip = user_add();
  ASSERT_NE(ip, nullptr);
  ASSERT_NE(ip->text, nullptr);
  ip->ob = ob;
  ob->interactive = ip;

  const unsigned char chunk[] = {'q', 'u', 'i', 't', '\n', 'n', 'e', 'x', 't', '\n'};
  EXPECT_EQ(1, process_ascii_chunk_for_test(ip, chunk, sizeof(chunk)));
  EXPECT_TRUE(ob->flags & O_DESTRUCTED);

  auto history = query_test_input_history_snapshot();
  ASSERT_EQ(1u, history.size());
  EXPECT_EQ("quit", history[0]);
  EXPECT_EQ("quit", query_test_input_snapshot());

  if (!(ob->flags & O_DESTRUCTED)) {
    if (ip != nullptr) {
      if (ob->interactive == ip) {
        ob->interactive = nullptr;
      }
      user_del(ip);
      interactive_free_text(ip);
      FREE(ip);
      ip = nullptr;
    }
    destruct_object(ob);
  }
  free_object(&ob, "DriverTest::TestAsciiChunkStopsAfterProcessInputDestructsObject");
  clear_test_input_snapshot();
}

TEST_F(DriverTest, TestProcessUserCommandSchedulesNextCommandAfterExec) {
  object_t *ob = nullptr;

  current_object = master_ob;
  error_context_t econ{};
  save_context(&econ);
  try {
    ob = find_object("/clone/input_capture_user");
  } catch (...) {
    restore_context(&econ);
  }
  pop_context(&econ);

  ASSERT_NE(ob, nullptr);
  call_lpc_method(ob, "clear_inputs");
  call_lpc_method(ob, "enable_exec_on_input");

  auto *base = event_base_new();
  ASSERT_NE(base, nullptr);

  auto *ip = user_add();
  ASSERT_NE(ip, nullptr);
  ASSERT_NE(ip->text, nullptr);

  ip->ob = ob;
  ip->connection_type = PORT_TYPE_ASCII;
  ip->prompt = "> ";
  ip->iflags = CMD_IN_BUF | HAS_PROCESS_INPUT;
  ip->ev_buffer = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
  ip->ev_command = evtimer_new(base, noop_timer_callback, nullptr);
  ASSERT_NE(ip->ev_buffer, nullptr);
  ASSERT_NE(ip->ev_command, nullptr);
  ob->interactive = ip;

  std::memcpy(ip->text, "first\nsecond\n", 13);
  ip->text_end = 13;
  ip->text_start = 0;

  EXPECT_EQ(1, process_user_command(ip));
  EXPECT_NE(ip->ob, ob);
  EXPECT_TRUE(event_pending(ip->ev_command, EV_TIMEOUT, nullptr));

  auto *active_ob = ip->ob;
  ASSERT_NE(active_ob, nullptr);
  add_ref(active_ob, "DriverTest::TestProcessUserCommandSchedulesNextCommandAfterExec");

  active_ob->interactive = nullptr;
  bufferevent_free(ip->ev_buffer);
  ip->ev_buffer = nullptr;
  event_free(ip->ev_command);
  ip->ev_command = nullptr;
  user_del(ip);
  interactive_free_text(ip);
  FREE(ip);
  // exec() 会重排对象生命周期；这里避免在测试清理阶段再次手动销毁迁移过的对象。
  active_ob = nullptr;
  ob = nullptr;
  event_base_free(base);
}

TEST_F(DriverTest, TestScheduledUserLogonCanBeCancelledBeforeDispatch) {
  auto *base = event_base_new();
  ASSERT_NE(base, nullptr);

  auto *ip = user_add();
  ASSERT_NE(ip, nullptr);
  ASSERT_NE(ip->text, nullptr);
  ip->ob = master_ob;
  ip->fd = -1;
  ip->local_port = 9527;

  reset_user_logon_callback_runs_for_test();
  ASSERT_TRUE(schedule_user_logon_for_test(base, ip));
  ASSERT_NE(ip->ev_logon, nullptr);

  cleanup_pending_user_for_test(ip, false);
  event_base_loop(base, EVLOOP_NONBLOCK);
  EXPECT_EQ(0, user_logon_callback_runs_for_test());

  event_base_free(base);
}

TEST_F(DriverTest, TestGatewayMaxPacketSizeRemainsIndependentFromMaxText) {
  auto original = g_gateway_max_packet_size;
  constexpr LPC_INT kRequestedPacketSize = 2 * 1024 * 1024;

  EXPECT_EQ(kRequestedPacketSize, call_gateway_config_number("max_packet_size", kRequestedPacketSize));
  EXPECT_EQ(static_cast<size_t>(kRequestedPacketSize), g_gateway_max_packet_size);
  EXPECT_GT(g_gateway_max_packet_size, static_cast<size_t>(MAX_TEXT));
  EXPECT_EQ(kRequestedPacketSize, call_gateway_config_number("max_packet_size"));

  g_gateway_max_packet_size = original;
}

TEST_F(DriverTest, TestSocketConnectRejectsOverlongNumericHost) {
  current_object = master_ob;

  auto fd = socket_create(STREAM, nullptr, nullptr);
  ASSERT_GE(fd, 0);

  std::string long_host(4096, '1');
  auto addr = long_host + " 80";

  EXPECT_EQ(EEBADADDR, socket_connect(fd, addr.c_str(), nullptr, nullptr));
  EXPECT_EQ(EESUCCESS, socket_close(fd, 0));
}

TEST_F(DriverTest, TestResolveSchedulesFailureCallbackWhenDnsBaseUnavailable) {
  struct DnsBaseRestorer {
    ~DnsBaseRestorer() { init_dns_event_base(g_event_base); }
  } restore_dns_base;

  object_t *helper = nullptr;

  current_object = master_ob;
  error_context_t econ{};
  save_context(&econ);
  try {
    helper = find_object("/clone/dns_callback_helper");
  } catch (...) {
    restore_context(&econ);
  }
  pop_context(&econ);

  ASSERT_NE(helper, nullptr);
  call_lpc_method(helper, "clear_result");

  current_object = helper;
  init_dns_event_base(nullptr);

  svalue_t callback{};
  callback.type = T_STRING;
  callback.subtype = STRING_SHARED;
  callback.u.string = make_shared_string("test_resolve_callback");

  auto key = query_addr_by_name("example.invalid", &callback);
  free_svalue(&callback, "DriverTest::TestResolveSchedulesFailureCallbackWhenDnsBaseUnavailable");

  run_gametick_events_for_test();

  auto *ret = call_lpc_method(helper, "query_result");
  ASSERT_NE(ret, nullptr);
  ASSERT_EQ(T_MAPPING, ret->type);
  ASSERT_NE(ret->u.map, nullptr);

  auto *key_sv = find_string_in_mapping(ret->u.map, "key");
  auto *name_sv = find_string_in_mapping(ret->u.map, "name");
  auto *addr_sv = find_string_in_mapping(ret->u.map, "addr");
  ASSERT_NE(key_sv, nullptr);
  ASSERT_NE(name_sv, nullptr);
  ASSERT_NE(addr_sv, nullptr);
  EXPECT_EQ(T_NUMBER, key_sv->type);
  EXPECT_EQ(key, key_sv->u.number);
  EXPECT_EQ(T_NUMBER, name_sv->type);
  EXPECT_EQ(T_UNDEFINED, name_sv->subtype);
  EXPECT_EQ(T_NUMBER, addr_sv->type);
  EXPECT_EQ(T_UNDEFINED, addr_sv->subtype);

  destruct_object(helper);
  free_object(&helper, "DriverTest::TestResolveSchedulesFailureCallbackWhenDnsBaseUnavailable");
}

TEST_F(DriverTest, TestMudlibStatsAcceptLongAuthorAndDomainNames) {
  std::string long_author(512, 'a');
  std::string long_domain(1024, 'd');
  long_author += "_author";
  long_domain += "_domain";

  reset_test_stat_overrides();
  set_test_author_override(long_author.c_str());
  set_test_domain_override(long_domain.c_str());

  auto *author_entry = set_master_author(long_author.c_str());
  auto *domain_entry = set_backbone_domain(long_domain.c_str());
  ASSERT_NE(author_entry, nullptr);
  ASSERT_NE(domain_entry, nullptr);

  auto author_errors = author_entry->errors;
  auto domain_errors = domain_entry->errors;

  add_errors_for_file("/single/void", 1);

  EXPECT_EQ(author_errors + 1, author_entry->errors);
  EXPECT_EQ(domain_errors + 1, domain_entry->errors);

  reset_test_stat_overrides();
}

TEST_F(DriverTest, TestMudlibStatsRestoreAcceptsLongNamesFromFile) {
  std::string long_author(768, 'a');
  std::string long_domain(1024, 'd');
  long_author += "_restore_author";
  long_domain += "_restore_domain";

  reset_test_stat_overrides();
  set_test_author_override(long_author.c_str());
  set_test_domain_override(long_domain.c_str());

  ASSERT_NE(set_master_author(long_author.c_str()), nullptr);
  ASSERT_NE(set_backbone_domain(long_domain.c_str()), nullptr);

  save_stat_files();
  restore_stat_files();

  auto *author_stats = get_author_stats(long_author.c_str());
  auto *domain_stats = get_domain_stats(long_domain.c_str());
  ASSERT_NE(author_stats, nullptr);
  ASSERT_NE(domain_stats, nullptr);
  free_mapping(author_stats);
  free_mapping(domain_stats);

  reset_test_stat_overrides();
}

TEST_F(DriverTest, TestConfigKeepsConfiguredMaximumLocalVariables) {
  EXPECT_EQ(30, CONFIG_INT(__MAX_LOCAL_VARIABLES__));
}

TEST_F(DriverTest, TestScratchpadLargeJoinRoundTrip) {
  std::string left(300, 'a');
  std::string right(220, 'b');

  char *lhs = scratch_copy(left.c_str());
  char *rhs = scratch_copy(right.c_str());
  char *joined = scratch_join(lhs, rhs);

  ASSERT_NE(joined, nullptr);
  EXPECT_EQ(left + right, joined);
}

TEST_F(DriverTest, TestReallocateLocalsGrowsBothLocalStorageArrays) {
  init_locals();

  auto initial_locals_size = locals_size;
  auto initial_type_size = type_of_locals_size;

  locals_ptr = locals + 7;
  type_of_locals_ptr = type_of_locals + 7;

  reallocate_locals();

  EXPECT_GT(locals_size, initial_locals_size);
  EXPECT_GT(type_of_locals_size, initial_type_size);
  EXPECT_EQ(locals_size, type_of_locals_size);
  EXPECT_EQ(locals_ptr, locals + 7);
  EXPECT_EQ(type_of_locals_ptr, type_of_locals + 7);
}

TEST_F(DriverTest, TestInheritListsHandleMoreThan256Entries) {
  constexpr int kInheritedPrograms = 300;

  object_t owner{};
  program_t root{};
  std::vector<program_t> inherited(kInheritedPrograms);
  std::vector<inherit_t> inherit_links(kInheritedPrograms);

  root.filename = "/root";
  root.num_inherited = kInheritedPrograms;
  root.inherit = inherit_links.data();
  owner.prog = &root;

  for (int i = 0; i < kInheritedPrograms; i++) {
    inherited[i].filename = "/child";
    inherit_links[i].prog = &inherited[i];
  }

  auto *direct = inherit_list(&owner);
  auto *deep = deep_inherit_list(&owner);

  ASSERT_NE(direct, nullptr);
  ASSERT_NE(deep, nullptr);
  EXPECT_EQ(kInheritedPrograms, direct->size);
  EXPECT_EQ(kInheritedPrograms, deep->size);

  free_array(direct);
  free_array(deep);
}

TEST_F(DriverTest, TestUtf8TruncateDropsIncompleteSuffix) {
  const uint8_t data[] = {'A', 0xe4, 0xb8};
  EXPECT_EQ(1u, u8_truncate(data, sizeof(data)));
}

TEST_F(DriverTest, TestEvalLimitDeadlineExpires) {
  outoftime = 0;
  set_eval(5'000);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  check_eval();
  EXPECT_EQ(1, outoftime);

  set_eval(max_eval_cost);
  EXPECT_EQ(0, outoftime);
}
