#include <gtest/gtest.h>
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
#include "compiler/internal/compiler.h"
#include "compiler/internal/scratchpad.h"
#include "packages/core/heartbeat.h"
#include "vm/internal/base/array.h"
#include "vm/internal/base/object.h"
#include "vm/internal/base/program.h"
#include "vm/internal/eval_limit.h"

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
