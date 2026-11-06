cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version} LANGUAGES C CXX)

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

# libebur128 for LUFS measurement (static linking)
include(FetchContent)
FetchContent_Declare(libebur128 GIT_REPOSITORY https://github.com/jiixyj/libebur128.git GIT_TAG v1.2.6 EXCLUDE_FROM_ALL)
# Force static library build only
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_INTERNAL_QUEUE_H ON CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
FetchContent_MakeAvailable(libebur128)
unset(CMAKE_POLICY_VERSION_MINIMUM CACHE)
# Configure libebur128 target
if(TARGET ebur128)
  # Disable warning-as-error (has #warning on non-SSE2 platforms)
  set_target_properties(ebur128 PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
  target_compile_options(ebur128 PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang,GNU>:-Wno-error>)
  # Enable PIC for static library linked into shared library (required on Linux)
  set_target_properties(ebur128 PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
# Restore default for other targets
set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)

add_library(${CMAKE_PROJECT_NAME} MODULE)

# C++17 を要求する
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_17)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs ebur128)
if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ole32)
elseif(APPLE)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreFoundation")
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE PkgConfig::PULSE)
endif()


# Add libebur128 include directory
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${libebur128_SOURCE_DIR}/ebur128)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 REQUIRED COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

# 既存の target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.c) を置き換える

# src/ と data/ を自動で拾う (追加のたびに CMakeLists.txt を編集しないため)
file(
  GLOB PLUGIN_SOURCES
  CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

file(
  GLOB_RECURSE PLUGIN_RESOURCES
  CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/data/*.effect"
  "${CMAKE_CURRENT_SOURCE_DIR}/data/locale/*.ini"
)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${PLUGIN_SOURCES} ${PLUGIN_RESOURCES})

# Solution Explorer のツリー表示を実ディレクトリに揃える
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${PLUGIN_SOURCES} ${PLUGIN_RESOURCES})

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# ビルド直後に cmake --install を実行する (Windows 開発用)
option(ENABLE_POST_BUILD_INSTALL "Run cmake --install after build" ON)

if(WIN32 AND ENABLE_POST_BUILD_INSTALL)
  add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}" --config "$<CONFIG>"
    COMMENT "Installing OBS plugin (cmake --install)"
    VERBATIM
  )
endif()

option(ENABLE_BASELINE_PROBE "Build the isolated baseline verification module" OFF)
if(ENABLE_BASELINE_PROBE)
  add_library(lbm-baseline-probe MODULE .github/baseline/probe.cpp)
  if(APPLE)
    set_target_properties(lbm-baseline-probe PROPERTIES BUNDLE TRUE BUNDLE_EXTENSION plugin)
    add_custom_command(
      TARGET lbm-baseline-probe
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_DIR:lbm-baseline-probe>/Contents/Resources"
    )
  endif()
  target_compile_features(lbm-baseline-probe PRIVATE cxx_std_17)
  target_link_libraries(lbm-baseline-probe PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Widgets)
  set_target_properties(
    lbm-baseline-probe
    PROPERTIES
      PREFIX ""
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/baseline-probe"
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/baseline-probe"
  )
endif()

option(ENABLE_PREVIEW_TESTS "Build finite recording verification" OFF)
if(ENABLE_PREVIEW_TESTS)
  enable_testing()
  add_executable(lbm-preview-core .github/tests/preview-core.cpp)
  target_compile_features(lbm-preview-core PRIVATE cxx_std_17)
  add_test(NAME preview-recording-core COMMAND lbm-preview-core)
endif()
