cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

# The Picture Gallery finds the OBS frontend at runtime (see src/frontend-shim.h)
# rather than linking it, so the plugin also loads where there is no frontend.
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_DL_LIBS})

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
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

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/playlist-common.c
    src/playlist-common.h
    src/frontend-shim.c
    src/frontend-shim.h
    src/ad-playlist-source.c
    src/ad-playlist-source.h
    src/picture-gallery-source.c
    src/picture-gallery-source.h
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# Runtime tests: real libobs, headless under Xvfb. Linux only; see
# tests/run-runtime-tests.sh.
option(ENABLE_RUNTIME_TESTS "Build the runtime tests (Linux)" OFF)

if(ENABLE_RUNTIME_TESTS)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The runtime tests run on Linux only")
  endif()

  find_package(X11 REQUIRED)
  find_package(Threads REQUIRED)

  add_executable(runtime-test tests/runtime-test.c)
  target_link_libraries(runtime-test PRIVATE OBS::libobs X11::X11 Threads::Threads)
  # the test stands in for the OBS frontend; the plugin finds it with dlsym
  set_target_properties(runtime-test PROPERTIES ENABLE_EXPORTS ON C_VISIBILITY_PRESET default)
endif()
