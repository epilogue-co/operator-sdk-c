cmake_minimum_required(VERSION 3.16)

project(opsdk
  VERSION 0.3.0
  DESCRIPTION "Epilogue Operator SDK: C library"
  HOMEPAGE_URL "https://github.com/epilogue-co/operator-sdk-c"
  LANGUAGES C)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(OPSDK_TOP_LEVEL ON)
else()
  set(OPSDK_TOP_LEVEL OFF)
endif()

option(OPSDK_BUILD_TESTS "Build the unit tests" ${OPSDK_TOP_LEVEL})
option(OPSDK_BUILD_TOOLS "Build the opsdk command-line tool" ${OPSDK_TOP_LEVEL})
option(OPSDK_BUILD_EXAMPLES "Build the examples" ${OPSDK_TOP_LEVEL})
option(OPSDK_INSTALL "Generate install rules" ${OPSDK_TOP_LEVEL})
option(OPSDK_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

include(GNUInstallDirs)

if(WIN32)
  set(OPSDK_PLATFORM_SOURCE src/opsdk_serial_win32.c)
else()
  set(OPSDK_PLATFORM_SOURCE src/opsdk_serial_posix.c)
endif()

add_library(opsdk
  src/opsdk.c
  ${OPSDK_PLATFORM_SOURCE})
add_library(opsdk::opsdk ALIAS opsdk)

target_include_directories(opsdk
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src)
target_compile_features(opsdk PUBLIC c_std_99)
target_compile_definitions(opsdk PRIVATE _DEFAULT_SOURCE _DARWIN_C_SOURCE)
if(WIN32)
  target_compile_definitions(opsdk PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
  target_link_libraries(opsdk PRIVATE setupapi advapi32)
endif()
get_target_property(OPSDK_TYPE opsdk TYPE)
if(OPSDK_TYPE STREQUAL "SHARED_LIBRARY")
  target_compile_definitions(opsdk PRIVATE OPSDK_BUILDING_SHARED INTERFACE OPSDK_SHARED)
endif()
set_target_properties(opsdk PROPERTIES
  C_STANDARD 99
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
  C_VISIBILITY_PRESET hidden
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
  PUBLIC_HEADER include/opsdk.h)

include(CheckLibraryExists)
check_library_exists(m pow "" OPSDK_HAVE_LIBM)
if(OPSDK_HAVE_LIBM)
  target_link_libraries(opsdk PRIVATE m)
endif()

if(MSVC)
  set(OPSDK_WARNING_FLAGS /W4 /wd4204 /wd4221)
  if(OPSDK_WARNINGS_AS_ERRORS)
    list(APPEND OPSDK_WARNING_FLAGS /WX)
  endif()
else()
  set(OPSDK_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes)
  if(OPSDK_WARNINGS_AS_ERRORS)
    list(APPEND OPSDK_WARNING_FLAGS -Werror)
  endif()
endif()
set(OPSDK_PROGRAM_DEFINITIONS _DEFAULT_SOURCE _DARWIN_C_SOURCE _CRT_SECURE_NO_WARNINGS)
target_compile_options(opsdk PRIVATE ${OPSDK_WARNING_FLAGS})

if(OPSDK_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(OPSDK_BUILD_TOOLS)
  add_executable(opsdk_tool tools/opsdk.c)
  set_target_properties(opsdk_tool PROPERTIES OUTPUT_NAME opsdk C_STANDARD 99 C_EXTENSIONS OFF)
  target_compile_definitions(opsdk_tool PRIVATE ${OPSDK_PROGRAM_DEFINITIONS})
  target_include_directories(opsdk_tool PRIVATE ${PROJECT_SOURCE_DIR}/tools)
  target_compile_options(opsdk_tool PRIVATE ${OPSDK_WARNING_FLAGS})
  target_link_libraries(opsdk_tool PRIVATE opsdk::opsdk)
endif()

if(OPSDK_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(OPSDK_INSTALL)
  include(CMakePackageConfigHelpers)

  install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/opsdk)
  install(TARGETS opsdk
    EXPORT opsdkTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  set(OPSDK_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/opsdk)
  install(EXPORT opsdkTargets
    NAMESPACE opsdk::
    DESTINATION ${OPSDK_CMAKE_DIR})

  configure_package_config_file(cmake/opsdkConfig.cmake.in
    ${PROJECT_BINARY_DIR}/opsdkConfig.cmake
    INSTALL_DESTINATION ${OPSDK_CMAKE_DIR})
  write_basic_package_version_file(${PROJECT_BINARY_DIR}/opsdkConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion)
  install(FILES
      ${PROJECT_BINARY_DIR}/opsdkConfig.cmake
      ${PROJECT_BINARY_DIR}/opsdkConfigVersion.cmake
    DESTINATION ${OPSDK_CMAKE_DIR})

  if(OPSDK_HAVE_LIBM)
    set(OPSDK_PC_LIBS_PRIVATE "-lm")
  endif()
  file(RELATIVE_PATH OPSDK_PC_TO_PREFIX "/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "/")
  string(REGEX REPLACE "/$" "" OPSDK_PC_TO_PREFIX "${OPSDK_PC_TO_PREFIX}")
  configure_file(cmake/opsdk.pc.in ${PROJECT_BINARY_DIR}/opsdk.pc @ONLY)
  install(FILES ${PROJECT_BINARY_DIR}/opsdk.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

  if(OPSDK_BUILD_TOOLS)
    install(TARGETS opsdk_tool RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
endif()
