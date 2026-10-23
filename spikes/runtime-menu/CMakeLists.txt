cmake_minimum_required(VERSION 3.21)

# The product name lives here and nowhere else; C++ gets it as INKWELL_PRODUCT_NAME.
set(INKWELL_PRODUCT_NAME "Inkwell")

project(${INKWELL_PRODUCT_NAME} VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

find_package(nlohmann_json CONFIG REQUIRED)

# Generates SKSEPlugin_Version: Address Library + struct-independent, no hard-coded runtime.
add_commonlibsse_plugin(${INKWELL_PRODUCT_NAME}
  NAME ${INKWELL_PRODUCT_NAME}
  AUTHOR "Inkwell"
  VERSION ${PROJECT_VERSION}
  SOURCES src/main.cpp)

target_compile_definitions(${INKWELL_PRODUCT_NAME} PRIVATE INKWELL_PRODUCT_NAME="${INKWELL_PRODUCT_NAME}")
target_link_libraries(${INKWELL_PRODUCT_NAME} PRIVATE nlohmann_json::nlohmann_json)
target_precompile_headers(${INKWELL_PRODUCT_NAME} PRIVATE src/PCH.h)
