cmake_minimum_required(VERSION 3.28)
project(qoderz VERSION 0.1.2 LANGUAGES CXX)

include(GNUInstallDirs)
include(CTest)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
  message(FATAL_ERROR "qoderz requires GCC 14 or newer (found ${CMAKE_CXX_COMPILER_VERSION})")
endif()

# GCC's "-fmodules-ts" dependency scanner makes every compile uncacheable for
# ccache (it reports "Unsupported compiler option") and doubles the work per
# translation unit. QoderZ does not use C++20 modules, so the scanner only costs
# time here.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF CACHE BOOL "Skip C++ module dependency scanning")

find_program(QODERZ_CCACHE ccache)
if(QODERZ_CCACHE)
  set(CMAKE_CXX_COMPILER_LAUNCHER "${QODERZ_CCACHE}" CACHE STRING "Compiler launcher")
  message(STATUS "qoderz: compiling through ccache (${QODERZ_CCACHE})")
else()
  message(STATUS "qoderz: ccache not found; compiling directly")
endif()
find_package(Boost 1.83 REQUIRED CONFIG COMPONENTS url)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)
find_package(CLI11 2.3 REQUIRED)

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/common")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/common/version.hpp.in"
  "${CMAKE_BINARY_DIR}/generated/common/version.hpp"
  @ONLY
)

file(GLOB_RECURSE QODERZ_CORE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
list(REMOVE_ITEM QODERZ_CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

# GCC 14 pairs the `std::aligned_alloc` that Asio uses for a coroutine frame with
# the frame's `operator delete` and reports them as a mismatched new/delete pair.
# Both halves of that pair come from Asio and the pair is legal, so the warning is
# a false positive. QoderZ forbids raw `new`/`delete`, leaving it meaningless.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  add_compile_options(-Wno-mismatched-new-delete)
endif()

add_library(qoderz_core STATIC ${QODERZ_CORE_SOURCES})
add_library(qoderz::core ALIAS qoderz_core)

target_include_directories(qoderz_core
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_BINARY_DIR}/generated"
)

target_link_libraries(qoderz_core
  PUBLIC
    nlohmann_json::nlohmann_json
    Boost::headers
    CLI11::CLI11
  PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    Boost::url
)
target_compile_features(qoderz_core PUBLIC cxx_std_23)
target_compile_options(qoderz_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow)

add_executable(qoderz src/main.cpp)
target_link_libraries(qoderz
  PRIVATE
    qoderz_core
    CLI11::CLI11
    Boost::headers
    OpenSSL::SSL
    OpenSSL::Crypto
)
target_compile_options(qoderz PRIVATE -Wall -Wextra -Wpedantic -Wshadow)

install(TARGETS qoderz RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(BUILD_TESTING)
  find_package(Catch2 3 REQUIRED)
  add_subdirectory(tests)
endif()
