find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
  NAME ContextualDictionary.ZdlModelPin
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_zdl_model_pin.py
)

add_test(
  NAME ContextualDictionary.AnalysisPolicy
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_analysis_policy.py
)

add_test(
  NAME ContextualDictionary.Formats
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_contextual_formats.py
)
