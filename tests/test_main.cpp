#include "test_harness.hpp"

#include <exception>
#include <iostream>

int main() {
  int failures = 0;

  for (const auto& test : libatlas_test::registry()) {
    try {
      test.function();
      std::cout << "[PASS] " << test.name << "\n";
    } catch (const std::exception& exception) {
      ++failures;
      std::cerr << "[FAIL] " << test.name << ": " << exception.what() << "\n";
    }
  }

  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }

  std::cout << libatlas_test::registry().size() << " test(s) passed\n";
  return 0;
}
