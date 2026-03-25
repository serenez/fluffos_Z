#include "base/std.h"

#include <cstdlib>
#include <cstdio>
#include <event2/event.h>
#include <iostream>
#include <unistd.h>

#include "mainlib.h"

#include "thirdparty/scope_guard/scope_guard.hpp"
#include "compiler/internal/disassembler.h"
#include "base/internal/rc.h"
#include "base/internal/tracing.h"
#include "vm/vm.h"

int main(int argc, char** argv) {
  std::string trace_log;
  std::string config_file;
  std::string lpc_file;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--tracing") == 0) {
      if (i + 1 >= argc) {
        std::cerr << "--tracing require an argument" << std::endl;
        return 1;
      }
      trace_log = argv[i + 1];
      i++;
      continue;
    }

    if (argv[i][0] == '-') {
      std::cerr << "Usage: lpcc [--tracing trace.json] config_file lpc_file" << std::endl;
      return 1;
    }

    if (config_file.empty()) {
      config_file = argv[i];
    } else if (lpc_file.empty()) {
      lpc_file = argv[i];
    } else {
      std::cerr << "Usage: lpcc [--tracing trace.json] config_file lpc_file" << std::endl;
      return 1;
    }
  }

  if (!trace_log.empty()) {
    Tracer::start(trace_log.c_str());
    Tracer::setThreadName("lpcc main");
  }
  DEFER { Tracer::collect(); };

  ScopedTracer const trace(__PRETTY_FUNCTION__);

  if (config_file.empty() || lpc_file.empty()) {
    std::cerr << "Usage: lpcc [--tracing trace.json] config_file lpc_file" << std::endl;
    return 1;
  }

  Tracer::begin("init_main", EventCategory::DEFAULT);

  // Initialize libevent, This should be done before executing LPC.
  init_main(config_file, false);

  Tracer::end("init_main", EventCategory::DEFAULT);

  // Start running.
  {
    ScopedTracer const tracer("vm_start");

    vm_start();
  }

  current_object = master_ob;
  const char* file = lpc_file.c_str();
  struct object_t* obj = nullptr;

  {
    ScopedTracer const tracer("find_object");

    error_context_t econ{};
    save_context(&econ);
    try {
      obj = find_object(file);
    } catch (...) {
      restore_context(&econ);
    }
    pop_context(&econ);
  }

  if (obj == nullptr || obj->prog == nullptr) {
    fprintf(stderr, "Fail to load object %s. \n", file);
    return 1;
  }

  {
    ScopedTracer const tracer("dump_prog");

    dump_prog(obj->prog, stdout, 1 | 2);
  }

  clear_state();

  return 0;
}
