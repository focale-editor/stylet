# The Flutter tooling requires that developers have a version of Visual Studio
# installed that includes CMake 3.14 or later. You should not increase this
# version, as doing so will cause the plugin to fail to compile for some
# customers of the plugin.
cmake_minimum_required(VERSION 3.14)

# Project-level configuration.
set(PROJECT_NAME "stylet")
project(${PROJECT_NAME} LANGUAGES CXX)

# Explicitly opt in to modern CMake behaviors to avoid warnings with recent
# versions of CMake.
cmake_policy(VERSION 3.14...3.25)

# This value is used when generating builds using this plugin, so it must
# not be changed
set(PLUGIN_NAME "stylet_plugin")

# PointerPredictor currently depends on an experimental Windows App SDK API.
# Keep it opt-in so normal Windows consumers retain the Flutter CMake baseline.
option(STYLET_ENABLE_EXPERIMENTAL_WINDOWS_PREDICTION
  "Enable experimental Windows App SDK stylus prediction"
  OFF)

set(stylet_bundled_libraries "")

# Any new source files that you add to the plugin should be added here.
list(APPEND PLUGIN_SOURCES
  "stylet_plugin.cpp"
  "stylet_plugin.h"
  "stylet_wintab.cpp"
  "stylet_wintab.h"
  "stylet_windows_prediction.cpp"
  "stylet_windows_prediction.h"
)

# Define the plugin library target. Its name must not be changed (see comment
# on PLUGIN_NAME above).
add_library(${PLUGIN_NAME} SHARED
  "include/stylet/stylet_plugin_c_api.h"
  "stylet_plugin_c_api.cpp"
  ${PLUGIN_SOURCES}
)

# Apply a standard set of build settings that are configured in the
# application-level CMakeLists.txt. This can be removed for plugins that want
# full control over build settings.
apply_standard_settings(${PLUGIN_NAME})

# Symbols are hidden by default to reduce the chance of accidental conflicts
# between plugins. This should not be removed; any symbols that should be
# exported should be explicitly exported with the FLUTTER_PLUGIN_EXPORT macro.
set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)

# Source include directories and library dependencies. Add any plugin-specific
# dependencies here.
target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(${PLUGIN_NAME} PRIVATE flutter flutter_wrapper_plugin)

if(STYLET_ENABLE_EXPERIMENTAL_WINDOWS_PREDICTION)
  if(CMAKE_VERSION VERSION_LESS 3.31)
    message(FATAL_ERROR
      "Experimental Stylet Windows prediction requires CMake 3.31 or newer.")
  endif()

  include(FetchContent)
  FetchContent_Declare(
    NuGetCMakePackage
    GIT_REPOSITORY https://github.com/mschofie/NuGetCMakePackage.git
    GIT_TAG 5f547903030535424f913d1e8153fcd5f85997fa
  )
  FetchContent_MakeAvailable(NuGetCMakePackage)
  add_nuget_packages(
    PACKAGES
      Microsoft.Windows.CppWinRT 2.0.250303.1
      Microsoft.WindowsAppSDK.Foundation 2.3.7-experimental
      Microsoft.WindowsAppSDK.Runtime 2.3.2-experimentalA
    CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/nuget.config"
    LOCK_FILE "${CMAKE_CURRENT_SOURCE_DIR}/stylet-packages.lock.json"
    FRAMEWORK native
  )
  find_package(Microsoft.WindowsAppSDK.Runtime CONFIG REQUIRED)
  find_package(Microsoft.WindowsAppSDK.Foundation CONFIG REQUIRED)

  target_compile_features(${PLUGIN_NAME} PRIVATE cxx_std_20)
  target_compile_definitions(${PLUGIN_NAME} PRIVATE
    STYLET_ENABLE_EXPERIMENTAL_WINDOWS_PREDICTION=1)
  target_link_libraries(${PLUGIN_NAME} PRIVATE
    Microsoft.WindowsAppSDK.Foundation
    Microsoft.WindowsAppSDK.Runtime
    Microsoft.WindowsAppSDK.Foundation._BootstrapBinary)

  get_target_property(stylet_bootstrap_dll
    Microsoft.WindowsAppSDK.Foundation._BootstrapBinary
    IMPORTED_LOCATION)
  list(APPEND stylet_bundled_libraries "${stylet_bootstrap_dll}")
endif()

# List of absolute paths to libraries that should be bundled with the plugin.
# This list could contain prebuilt libraries, or libraries created by an
# external build triggered from this build file.
set(stylet_bundled_libraries ${stylet_bundled_libraries} PARENT_SCOPE)

# === Tests ===
# These unit tests can be run from a terminal after building the example, or
# from Visual Studio after opening the generated solution file.

# Only enable test builds when building the example (which sets this variable)
# so that plugin clients aren't building the tests.
if (${include_${PROJECT_NAME}_tests})
set(TEST_RUNNER "${PROJECT_NAME}_test")
enable_testing()

# Add the Google Test dependency.
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
# Disable install commands for gtest so it doesn't end up in the bundle.
set(INSTALL_GTEST OFF CACHE BOOL "Disable installation of googletest" FORCE)
FetchContent_MakeAvailable(googletest)

# The plugin's C API is not very useful for unit testing, so build the sources
# directly into the test binary rather than using the DLL.
add_executable(${TEST_RUNNER}
  test/stylet_plugin_test.cpp
  ${PLUGIN_SOURCES}
)
apply_standard_settings(${TEST_RUNNER})
target_include_directories(${TEST_RUNNER} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(${TEST_RUNNER} PRIVATE flutter_wrapper_plugin)
target_link_libraries(${TEST_RUNNER} PRIVATE gtest_main gmock)
if(STYLET_ENABLE_EXPERIMENTAL_WINDOWS_PREDICTION)
  target_compile_features(${TEST_RUNNER} PRIVATE cxx_std_20)
  target_compile_definitions(${TEST_RUNNER} PRIVATE
    STYLET_ENABLE_EXPERIMENTAL_WINDOWS_PREDICTION=1)
  target_link_libraries(${TEST_RUNNER} PRIVATE
    Microsoft.WindowsAppSDK.Foundation
    Microsoft.WindowsAppSDK.Runtime
    Microsoft.WindowsAppSDK.Foundation._BootstrapBinary)
endif()
# flutter_wrapper_plugin has link dependencies on the Flutter DLL.
add_custom_command(TARGET ${TEST_RUNNER} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
  "${FLUTTER_LIBRARY}" $<TARGET_FILE_DIR:${TEST_RUNNER}>
)
if(STYLET_ENABLE_EXPERIMENTAL_WINDOWS_PREDICTION)
  add_custom_command(TARGET ${TEST_RUNNER} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${stylet_bootstrap_dll}" $<TARGET_FILE_DIR:${TEST_RUNNER}>
  )
endif()

# Enable automatic test discovery.
include(GoogleTest)
gtest_discover_tests(${TEST_RUNNER})
endif()
