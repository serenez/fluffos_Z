#include <gtest/gtest.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "base/package_api.h"

#include "mainlib.h"

#include "base/internal/rc.h"
#include "base/internal/strutils.h"
#include "comm.h"
#include "compiler/internal/compiler.h"
#include "compiler/internal/scratchpad.h"
#include "interactive.h"
#include "packages/core/heartbeat.h"
#include "packages/gateway/gateway.h"
#include "user.h"
#include "vm/internal/base/array.h"
#include "vm/internal/base/object.h"
#include "vm/internal/base/program.h"
#include "vm/internal/eval_limit.h"
#include "vm/internal/simulate.h"

char *extract_first_command_for_test(interactive_t *ip);

namespace {

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

}  // namespace

// Test fixture class
class DriverTest : public ::testing::Test {
 public:
  static void SetUpTestSuite() {
    chdir(TESTSUITE_DIR);
    // Initialize libevent, This should be done before executing LPC.
    init_main("etc/config.test", false);
    vm_start();
  }

 protected:
  void SetUp() override {
    scratch_destroy();
    clear_state();
    clear_heartbeats();
  }

  void TearDown() override {
    scratch_destroy();
    clear_heartbeats();
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

TEST_F(DriverTest, TestMudPacketCompletionRequiresFullPayloadBytes) {
  constexpr int kPacketSize = 128 * 1024;

  EXPECT_FALSE(mud_packet_is_complete(kPacketSize, 4));
  EXPECT_FALSE(mud_packet_is_complete(kPacketSize, 64 * 1024 + 4));
  EXPECT_FALSE(mud_packet_is_complete(kPacketSize, kPacketSize + 3));
  EXPECT_TRUE(mud_packet_is_complete(kPacketSize, kPacketSize + 4));
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

TEST_F(DriverTest, TestGatewayMaxPacketSizeRemainsIndependentFromMaxText) {
  auto original = g_gateway_max_packet_size;
  constexpr LPC_INT kRequestedPacketSize = 2 * 1024 * 1024;

  EXPECT_EQ(kRequestedPacketSize, call_gateway_config_number("max_packet_size", kRequestedPacketSize));
  EXPECT_EQ(static_cast<size_t>(kRequestedPacketSize), g_gateway_max_packet_size);
  EXPECT_GT(g_gateway_max_packet_size, static_cast<size_t>(MAX_TEXT));
  EXPECT_EQ(kRequestedPacketSize, call_gateway_config_number("max_packet_size"));

  g_gateway_max_packet_size = original;
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
