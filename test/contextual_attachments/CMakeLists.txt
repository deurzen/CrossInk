add_executable(contextual_attachments_test
  ContextualAttachmentsTest.cpp
  ${REPO_ROOT}/lib/Dictionary/ContextualAttachments.cpp
  ${REPO_ROOT}/lib/Dictionary/ContextualSourceCatalog.cpp
  ${REPO_ROOT}/lib/Dictionary/Crc32.cpp
)

target_include_directories(contextual_attachments_test PRIVATE
  ${REPO_ROOT}/lib/Dictionary
)

target_link_libraries(contextual_attachments_test PRIVATE
  crosspoint_test_common
  gtest_main
)

gtest_discover_tests(contextual_attachments_test TEST_PREFIX "ContextualAttachments.")
