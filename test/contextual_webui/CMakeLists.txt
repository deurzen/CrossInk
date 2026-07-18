find_program(NODE_EXECUTABLE node REQUIRED)

add_test(
  NAME ContextualWebUi.PackageValidation
  COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_contextual_package.js
)
