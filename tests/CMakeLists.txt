add_executable(unit_tests
  smoke_test.cpp
  test_unique_fd.cpp
  test_result.cpp
  test_types.cpp
  test_line_buffer.cpp
  test_log_source.cpp
  test_template_parser.cpp
  test_memory_store.cpp
  test_memory_journal.cpp
  test_incident_queue.cpp
  test_vectorizer.cpp
  test_cosine.cpp
  test_policy.cpp
  test_agent.cpp
  test_executor.cpp
  test_sandbox.cpp
  test_verifier.cpp
  test_cooldown.cpp
  test_skills.cpp
  test_config.cpp
  test_event_sink.cpp
  # one file per module, added as it is written:
)

if(AGENTICPP_ENABLE_LLM)
  target_sources(unit_tests PRIVATE test_deepseek.cpp)
endif()

add_executable(sandbox_probe sandbox_probe.cpp)
if(AGENTICPP_SANITIZE)
  target_compile_options(sandbox_probe PRIVATE -fno-sanitize=address,undefined)
  target_link_options(sandbox_probe PRIVATE -fno-sanitize=address,undefined)
endif()
if(AGENTICPP_TSAN)
  target_compile_options(sandbox_probe PRIVATE -fno-sanitize=thread)
  target_link_options(sandbox_probe PRIVATE -fno-sanitize=thread)
endif()

target_compile_definitions(unit_tests PRIVATE
  AGENTICPP_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  AGENTICPP_SANDBOX_PROBE="${CMAKE_BINARY_DIR}/bin/sandbox_probe")

target_link_libraries(unit_tests PRIVATE agenticpp_core)
target_link_libraries(unit_tests PRIVATE Catch2::Catch2WithMain)

# Catch2 registers each TEST_CASE as a separate ctest entry, so a failure
# names the behaviour that broke.
include(Catch)
catch_discover_tests(unit_tests)
