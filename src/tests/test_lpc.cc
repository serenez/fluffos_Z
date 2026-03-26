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
#include "packages/core/ed.h"
#include "packages/core/dns.h"
#include "packages/core/heartbeat.h"
#include "packages/gateway/gateway.h"
#include "packages/mudlib_stats/mudlib_stats.h"
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
