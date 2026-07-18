add_executable(contextual_runtime_format_test
  ContextualRuntimeFormatTest.cpp
  ${REPO_ROOT}/lib/Dictionary/ContextualRuntimeFormat.cpp
  ${REPO_ROOT}/lib/Dictionary/DictionaryPackage.cpp
  ${REPO_ROOT}/lib/Dictionary/Crc32.cpp
)

target_include_directories(contextual_runtime_format_test PRIVATE
  ${REPO_ROOT}/lib/Dictionary
)

target_link_libraries(contextual_runtime_format_test PRIVATE
  crosspoint_test_common
  gtest_main
)

gtest_discover_tests(contextual_runtime_format_test TEST_PREFIX "ContextualRuntimeFormat.")
