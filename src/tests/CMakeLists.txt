# GTEST
find_package(GTest)

if(${GTEST_FOUND})
  include(GoogleTest)
  enable_testing()

  add_executable(lpc_tests test_lpc.cc)
  target_link_libraries(lpc_tests PRIVATE ${FLUFFOS_LINK} GTest::GTest  GTest::Main)
  target_compile_definitions(lpc_tests PRIVATE -DTESTSUITE_DIR="${CMAKE_SOURCE_DIR}/testsuite")

  add_executable(ofile_tests test_ofile.cc)
  target_link_libraries(ofile_tests PRIVATE ${FLUFFOS_LINK} GTest::GTest  GTest::Main)
  target_compile_definitions(ofile_tests PRIVATE -DTESTSUITE_DIR="${CMAKE_SOURCE_DIR}/testsuite")

  add_executable(heartbeat_bench heartbeat_bench.cc)
  target_link_libraries(heartbeat_bench PRIVATE ${FLUFFOS_LINK})
  target_compile_definitions(heartbeat_bench PRIVATE -DTESTSUITE_DIR="${CMAKE_SOURCE_DIR}/testsuite")

  add_executable(gametick_bench gametick_bench.cc)
  target_link_libraries(gametick_bench PRIVATE ${FLUFFOS_LINK})
  target_compile_definitions(gametick_bench PRIVATE -DTESTSUITE_DIR="${CMAKE_SOURCE_DIR}/testsuite")

  add_executable(call_out_bench call_out_bench.cc)
  target_link_libraries(call_out_bench PRIVATE ${FLUFFOS_LINK})
  target_compile_definitions(call_out_bench PRIVATE -DTESTSUITE_DIR="${CMAKE_SOURCE_DIR}/testsuite")

  add_executable(user_parser_bench user_parser_bench.cc)
  target_link_libraries(user_parser_bench PRIVATE ${FLUFFOS_LINK})
  target_compile_definitions(user_parser_bench PRIVATE -DTESTSUITE_DIR="${CMAKE_SOURCE_DIR}/testsuite")

  add_executable(input_buffer_bench input_buffer_bench.cc)
  target_link_libraries(input_buffer_bench PRIVATE ${FLUFFOS_LINK})
  target_compile_definitions(input_buffer_bench PRIVATE -DTESTSUITE_DIR="${CMAKE_SOURCE_DIR}/testsuite")

  add_executable(mud_input_bench mud_input_bench.cc)
  target_link_libraries(mud_input_bench PRIVATE ${FLUFFOS_LINK})
  target_compile_definitions(mud_input_bench PRIVATE -DTESTSUITE_DIR="${CMAKE_SOURCE_DIR}/testsuite")

  gtest_discover_tests(lpc_tests ofile_tests DISCOVERY_TIMEOUT 60)
endif()

