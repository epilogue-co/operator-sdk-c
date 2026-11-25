add_library(opsdk_fake_device STATIC fake_device.c)
target_link_libraries(opsdk_fake_device PUBLIC opsdk::opsdk)
target_compile_definitions(opsdk_fake_device PUBLIC ${OPSDK_PROGRAM_DEFINITIONS} _XOPEN_SOURCE=600)
set_target_properties(opsdk_fake_device PROPERTIES C_STANDARD 99 C_EXTENSIONS OFF)
target_compile_options(opsdk_fake_device PRIVATE ${OPSDK_WARNING_FLAGS})

function(opsdk_test name)
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} PRIVATE opsdk_fake_device)
  set_target_properties(${name} PROPERTIES C_STANDARD 99 C_EXTENSIONS OFF)
  target_compile_options(${name} PRIVATE ${OPSDK_WARNING_FLAGS})
  add_test(NAME ${name} COMMAND ${name} ${ARGN})
  set_tests_properties(${name} PROPERTIES TIMEOUT 60 LABELS unit)
  if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.22)
    set_tests_properties(${name} PROPERTIES ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:$<TARGET_FILE_DIR:opsdk>")
  endif()
endfunction()

opsdk_test(test_opsdk)
opsdk_test(test_conformance ${PROJECT_SOURCE_DIR}/conformance)

if(NOT WIN32)
  opsdk_test(test_serial_pty)

  add_executable(fake_pty fake_pty.c)
  target_link_libraries(fake_pty PRIVATE opsdk_fake_device)
  set_target_properties(fake_pty PROPERTIES C_STANDARD 99 C_EXTENSIONS OFF)
  target_compile_options(fake_pty PRIVATE ${OPSDK_WARNING_FLAGS})
  if(OPSDK_BUILD_EXAMPLES)
    add_test(NAME examples
      COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_examples.sh $<TARGET_FILE:fake_pty>
        ${PROJECT_BINARY_DIR}/examples ${CMAKE_CURRENT_BINARY_DIR}/examples-work)
    set_tests_properties(examples PROPERTIES TIMEOUT 60 LABELS unit)
  endif()
endif()
