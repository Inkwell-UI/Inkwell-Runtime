cmake_minimum_required(VERSION 3.21)

# The product name lives here and nowhere else; C++ gets it as INKWELL_PRODUCT_NAME.
set(INKWELL_PRODUCT_NAME "Inkwell")

project(${INKWELL_PRODUCT_NAME} VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(INKWELL_BUILD_PLUGIN "Build Inkwell.dll (needs CommonLibSSE-NG; the first build takes ~15 minutes)" ON)
option(INKWELL_BUILD_TESTS "Build and register the core unit tests" ON)
# Until 1.0 the log defaults to trace so every in-game test run tells as much as possible (S2 says "info" for the final default).
set(INKWELL_DEFAULT_LOG_LEVEL "trace" CACHE STRING "Log level used when Inkwell.ini does not set [Runtime] LogLevel")

find_package(nlohmann_json CONFIG REQUIRED)

# ---- core: pure C++ (menu JSON, INI, versions, tables). No engine headers, so the tests build in seconds. ----
add_library(InkwellCore STATIC src/core/MenuConfig.cpp src/core/Ini.cpp src/core/Saved.cpp
  src/core/Crypto.cpp src/core/WebSocket.cpp src/core/DevProtocol.cpp src/core/DevServer.cpp src/core/DirWatcher.cpp src/core/MenuScan.cpp)
target_include_directories(InkwellCore PUBLIC src)
target_compile_definitions(InkwellCore PUBLIC
  INKWELL_PRODUCT_NAME="${INKWELL_PRODUCT_NAME}"
  INKWELL_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  INKWELL_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  INKWELL_VERSION_PATCH=${PROJECT_VERSION_PATCH})
target_link_libraries(InkwellCore PUBLIC nlohmann_json::nlohmann_json ws2_32 bcrypt)
if(MSVC)
  target_compile_options(InkwellCore PUBLIC /utf-8 /W4)
endif()

if(INKWELL_BUILD_TESTS)
  enable_testing()
  add_executable(InkwellTests tests/core_tests.cpp)
  target_link_libraries(InkwellTests PRIVATE InkwellCore)
  add_test(NAME core COMMAND InkwellTests ${CMAKE_CURRENT_SOURCE_DIR}/test-vectors)
  # dev server (S6): unit tests plus a loopback end-to-end run, no game needed (--serve keeps a fake host up for scripts/dev-client.mjs)
  add_executable(InkwellDevTests tests/dev_tests.cpp)
  target_link_libraries(InkwellDevTests PRIVATE InkwellCore)
  add_test(NAME dev COMMAND InkwellDevTests ${CMAKE_CURRENT_SOURCE_DIR}/test-assets)
endif()

# ---- the SKSE plugin ----
if(INKWELL_BUILD_PLUGIN)
  # Pinned CommonLibSSE-NG release (alandtse fork). Supports 1.7.104; see README.
  include(FetchContent)
  FetchContent_Declare(
    CommonLibSSE
    GIT_REPOSITORY https://github.com/alandtse/CommonLibSSE-NG.git
    GIT_TAG v13.1.0
    GIT_SHALLOW TRUE
  )
  set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(CommonLibSSE)
  # The source-build path of CommonLib does not define the helper itself.
  if(NOT COMMAND add_commonlibsse_plugin)
    include(${commonlibsse_SOURCE_DIR}/cmake/CommonLibSSE.cmake)
  endif()

  # Generates SKSEPlugin_Version: Address Library, struct-independent, no hard-coded runtime (SE, AE and GOG).
  add_commonlibsse_plugin(${INKWELL_PRODUCT_NAME}
    NAME ${INKWELL_PRODUCT_NAME}
    AUTHOR "Inkwell"
    VERSION ${PROJECT_VERSION}
    SOURCES
      src/plugin/main.cpp
      src/plugin/Runtime.cpp
      src/plugin/Menu.cpp
      src/plugin/Bridge.cpp
      src/plugin/Input.cpp
      src/plugin/Papyrus.cpp
      src/plugin/Messaging.cpp
      src/plugin/Saved.cpp
      src/plugin/Dev.cpp)

  target_include_directories(${INKWELL_PRODUCT_NAME} PRIVATE src include)
  target_compile_definitions(${INKWELL_PRODUCT_NAME} PRIVATE
    INKWELL_PRODUCT_NAME="${INKWELL_PRODUCT_NAME}"
    INKWELL_DEFAULT_LOG_LEVEL="${INKWELL_DEFAULT_LOG_LEVEL}")
  target_link_libraries(${INKWELL_PRODUCT_NAME} PRIVATE InkwellCore)
  # a PDB next to the DLL (Release stays optimized): crash logs and debuggers can resolve the addresses
  if(MSVC)
    target_compile_options(${INKWELL_PRODUCT_NAME} PRIVATE /Zi)
    target_link_options(${INKWELL_PRODUCT_NAME} PRIVATE /DEBUG /OPT:REF /OPT:ICF)
  endif()
  target_precompile_headers(${INKWELL_PRODUCT_NAME} PRIVATE src/plugin/PCH.h)
endif()
