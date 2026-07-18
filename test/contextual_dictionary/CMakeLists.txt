find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
  NAME ContextualDictionary.ZdlModelPin
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_zdl_model_pin.py
)
