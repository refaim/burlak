cmake_minimum_required(VERSION 3.28)

project(Burlak LANGUAGES CXX RC)

include(CTest)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(BURLAK_COVERAGE "Instrument Burlak and its tests for llvm-cov" OFF)

add_library(burlak_options INTERFACE)
target_compile_definitions(burlak_options INTERFACE UNICODE _UNICODE STRICT_TYPED_ITEMIDS)
target_compile_options(
  burlak_options
  INTERFACE
    /W4
    /WX
    /permissive-
    /EHsc
    /utf-8
    /GS
    $<$<CXX_COMPILER_ID:Clang>:/clang:-std=c++23>
    # CMake forwards MSVC-only driver arguments to clang-cl, which otherwise diagnoses them as unused.
    $<$<CXX_COMPILER_ID:Clang>:-Wno-unused-command-line-argument>
    $<$<BOOL:${BURLAK_COVERAGE}>:/Od>
    $<$<BOOL:${BURLAK_COVERAGE}>:/Zi>
    $<$<BOOL:${BURLAK_COVERAGE}>:-fprofile-instr-generate>
    $<$<BOOL:${BURLAK_COVERAGE}>:-fcoverage-mapping>
    $<$<BOOL:${BURLAK_COVERAGE}>:/clang:-fprofile-update=atomic>
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:MSVC>>:/O2>)
target_link_options(
  burlak_options
  INTERFACE
    /guard:cf
    $<$<BOOL:${BURLAK_COVERAGE}>:-fprofile-instr-generate>
    $<$<CONFIG:Release>:/DEBUG:NONE>
    $<$<CONFIG:Release>:/INCREMENTAL:NO>)
target_compile_options(burlak_options INTERFACE /guard:cf)

add_library(
  burlak_core STATIC
  src/core/Geometry.cpp
  src/core/Gesture.cpp
  src/core/DragPlan.cpp
  src/core/Session.cpp
  src/core/Policies.cpp)
target_include_directories(burlak_core PUBLIC src)
target_link_libraries(burlak_core PUBLIC burlak_options)

add_library(
  burlak_adapters STATIC
  src/adapters/far/FarApi.cpp
  src/adapters/far/PluginCall.cpp
  src/adapters/win/Screen.cpp
  src/adapters/win/Input.cpp
  src/adapters/shell/Shell.cpp)
target_include_directories(burlak_adapters PUBLIC src PRIVATE sdk)
target_link_libraries(burlak_adapters PUBLIC burlak_core burlak_options PRIVATE ole32 shell32 user32)

add_library(
  burlak_drag STATIC
  src/drag/DragSource.cpp
  src/drag/DropTarget.cpp
  src/drag/ToolWindow.cpp)
target_include_directories(burlak_drag PUBLIC src)
target_link_libraries(burlak_drag PUBLIC burlak_core burlak_options PRIVATE ole32 shell32 user32)

add_library(burlak_plugin_support STATIC src/plugin/Composition.cpp)
target_include_directories(burlak_plugin_support PUBLIC src PRIVATE sdk)
target_link_libraries(burlak_plugin_support PUBLIC burlak_drag burlak_adapters burlak_core burlak_options)

add_library(Burlak SHARED src/plugin/Exports.cpp src/plugin/Burlak.rc src/plugin/Burlak.def)
target_include_directories(Burlak PRIVATE src sdk)
target_link_libraries(Burlak PRIVATE burlak_plugin_support ole32 shell32 user32)
set_target_properties(
  Burlak
  PROPERTIES
    PREFIX ""
    OUTPUT_NAME "Burlak"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")

if(BUILD_TESTING)
  add_library(burlak_test_main OBJECT tests/TestMain.cpp)
  target_include_directories(burlak_test_main PUBLIC third_party)
  target_link_libraries(burlak_test_main PUBLIC burlak_options)

  add_executable(
    core_tests
    $<TARGET_OBJECTS:burlak_test_main>
    tests/core/GeometryTests.cpp
    tests/core/GestureTests.cpp
    tests/core/DragPlanTests.cpp
    tests/core/SessionTests.cpp
    tests/core/PoliciesTests.cpp)
  target_include_directories(core_tests PRIVATE third_party)
  target_link_libraries(core_tests PRIVATE burlak_core)
  add_test(NAME core COMMAND core_tests)

  add_library(getfiles_success SHARED tests/adapters/GetFilesSuccess.cpp)
  target_include_directories(getfiles_success PRIVATE sdk)
  target_link_libraries(getfiles_success PRIVATE burlak_options)
  set_target_properties(
    getfiles_success PROPERTIES PREFIX "" OUTPUT_NAME "GetFilesSuccess" WINDOWS_EXPORT_ALL_SYMBOLS ON)

  add_library(getfiles_crash SHARED tests/adapters/GetFilesCrash.cpp)
  target_include_directories(getfiles_crash PRIVATE sdk)
  target_link_libraries(getfiles_crash PRIVATE burlak_options)
  set_target_properties(
    getfiles_crash PROPERTIES PREFIX "" OUTPUT_NAME "GetFilesCrash" WINDOWS_EXPORT_ALL_SYMBOLS ON)

  add_executable(
    adapter_tests
    $<TARGET_OBJECTS:burlak_test_main>
    tests/adapters/FarTests.cpp
    tests/adapters/PluginCallTests.cpp
    tests/adapters/ScreenTests.cpp
    tests/adapters/InputTests.cpp
    tests/adapters/ShellTests.cpp)
  target_include_directories(adapter_tests PRIVATE third_party sdk)
  target_compile_definitions(
    adapter_tests
    PRIVATE
      GETFILES_SUCCESS_PATH=L"$<TARGET_FILE:getfiles_success>"
      GETFILES_CRASH_PATH=L"$<TARGET_FILE:getfiles_crash>")
  target_link_libraries(adapter_tests PRIVATE burlak_adapters ole32 shell32 user32)
  add_dependencies(adapter_tests getfiles_success getfiles_crash)
  add_test(NAME adapters COMMAND adapter_tests)

  add_executable(
    drag_tests
    $<TARGET_OBJECTS:burlak_test_main>
    tests/drag/DragSourceTests.cpp
    tests/drag/DropTargetTests.cpp
    tests/drag/ToolWindowTests.cpp)
  target_include_directories(drag_tests PRIVATE third_party)
  target_link_libraries(drag_tests PRIVATE burlak_drag burlak_adapters ole32 shell32 user32)
  add_test(NAME drag COMMAND drag_tests)

  add_executable(
    plugin_tests
    $<TARGET_OBJECTS:burlak_test_main>
    src/plugin/Exports.cpp
    tests/plugin/PluginTests.cpp)
  target_include_directories(plugin_tests PRIVATE third_party sdk src)
  target_link_libraries(plugin_tests PRIVATE burlak_plugin_support ole32 shell32 user32)
  add_test(NAME plugin COMMAND plugin_tests)

  add_executable(
    e2e_tests
    $<TARGET_OBJECTS:burlak_test_main>
    tests/e2e/E2eTests.cpp)
  target_include_directories(e2e_tests PRIVATE third_party sdk src)
  target_compile_definitions(e2e_tests PRIVATE BURLAK_DLL_PATH=L"$<TARGET_FILE:Burlak>")
  target_link_libraries(e2e_tests PRIVATE burlak_drag burlak_adapters ole32 shell32 user32)
  add_dependencies(e2e_tests Burlak)
  add_test(NAME e2e COMMAND e2e_tests)
  # Adapters and e2e move the cursor, while drag can raise its tool window topmost; they must not
  # compete for the shared desktop when CTest runs in parallel.
  set_tests_properties(adapters drag e2e PROPERTIES RESOURCE_LOCK desktop)
  if(BURLAK_COVERAGE)
    set_tests_properties(
      e2e PROPERTIES ENVIRONMENT "LLVM_PROFILE_FILE=${CMAKE_BINARY_DIR}/burlak-e2e-%p-%m.profraw")
  endif()

  add_executable(guard_tests $<TARGET_OBJECTS:burlak_test_main> tests/guard/GuardTests.cpp)
  target_include_directories(guard_tests PRIVATE third_party)
  target_compile_definitions(guard_tests PRIVATE BURLAK_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  target_link_libraries(guard_tests PRIVATE burlak_options)
  add_test(NAME guard COMMAND guard_tests)
endif()
