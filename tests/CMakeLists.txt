add_executable(libatlas_tests
  test_core.cpp
  test_main.cpp
)

target_compile_features(libatlas_tests PRIVATE cxx_std_17)
target_include_directories(libatlas_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(libatlas_tests PRIVATE libatlas)

add_test(NAME libatlas_tests COMMAND libatlas_tests)
