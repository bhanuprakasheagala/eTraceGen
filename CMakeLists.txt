cmake_minimum_required(VERSION 3.20)
project(eTraceGen LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "eTraceGen is Linux-only. Build on a Linux host.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(etracegen
  src/main.cpp
  src/decoder/decoder.cpp
  src/model/normalizer.cpp
  src/registry/entity_registry.cpp
  src/util/proc_util.cpp
  src/policy/policy.cpp
  src/sinks/json_sink.cpp
  src/metrics/metrics.cpp
  src/collector/collector_libbpf.cpp
)

target_include_directories(etracegen PRIVATE include src)
target_compile_options(etracegen PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test_policy_process
  tests/test_policy_process.cpp
  src/policy/policy.cpp
)
target_include_directories(test_policy_process PRIVATE include src)
target_compile_options(test_policy_process PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test_model_registry
  tests/test_model_registry.cpp
  src/model/normalizer.cpp
  src/registry/entity_registry.cpp
  src/util/proc_util.cpp
)
target_include_directories(test_model_registry PRIVATE include src)
target_compile_options(test_model_registry PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test_json_sink
  tests/test_json_sink.cpp
  src/sinks/json_sink.cpp
)
target_include_directories(test_json_sink PRIVATE include src)
target_compile_options(test_json_sink PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_test(NAME policy_process COMMAND test_policy_process)
add_test(NAME model_registry COMMAND test_model_registry)
add_test(NAME json_sink COMMAND test_json_sink)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBBPF REQUIRED libbpf)

message(STATUS "Building Linux libbpf collector backend")
target_include_directories(etracegen PRIVATE ${LIBBPF_INCLUDE_DIRS})
target_link_libraries(etracegen PRIVATE ${LIBBPF_LIBRARIES})
target_compile_definitions(etracegen PRIVATE
  EVENT_LOGGER_HAVE_LIBBPF=1
  EVENT_LOGGER_DEFAULT_BPF_OBJECT="${CMAKE_SOURCE_DIR}/bpf/event_logger.bpf.o"
)

# Linux-only helper target for reproducible BPF object generation.
add_custom_target(bpf_object
  COMMAND "${CMAKE_SOURCE_DIR}/scripts/linux.sh" bpf
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Build eTraceGen BPF object (bpf/event_logger.bpf.o)"
)
