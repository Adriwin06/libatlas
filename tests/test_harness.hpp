#pragma once

#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace libatlas_test {

using TestFunction = void (*)();

struct TestCase {
  const char* name;
  TestFunction function;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

struct Registrar {
  Registrar(const char* name, TestFunction function) { registry().push_back(TestCase{name, function}); }
};

[[noreturn]] inline void fail(const char* file, int line, const std::string& message) {
  std::ostringstream stream;
  stream << file << ":" << line << ": " << message;
  throw std::runtime_error(stream.str());
}

template <typename T, typename U>
inline void expect_equal(const T& lhs,
                         const U& rhs,
                         const char* lhs_text,
                         const char* rhs_text,
                         const char* file,
                         int line) {
  if (!(lhs == rhs)) {
    std::ostringstream stream;
    stream << "expected " << lhs_text << " == " << rhs_text << " but got different values";
    fail(file, line, stream.str());
  }
}

}  // namespace libatlas_test

#define LIBATLAS_TEST(Name)                                                       \
  static void Name();                                                             \
  static ::libatlas_test::Registrar Name##_registrar(#Name, &Name);              \
  static void Name()

#define EXPECT_TRUE(Expression)                                                   \
  do {                                                                            \
    if (!(Expression)) {                                                          \
      ::libatlas_test::fail(__FILE__, __LINE__, "expected true: " #Expression);  \
    }                                                                             \
  } while (false)

#define EXPECT_FALSE(Expression)                                                   \
  do {                                                                             \
    if ((Expression)) {                                                            \
      ::libatlas_test::fail(__FILE__, __LINE__, "expected false: " #Expression);  \
    }                                                                              \
  } while (false)

#define EXPECT_EQ(Lhs, Rhs)                                                             \
  do {                                                                                  \
    ::libatlas_test::expect_equal((Lhs), (Rhs), #Lhs, #Rhs, __FILE__, __LINE__);       \
  } while (false)

#define REQUIRE_OK(ResultExpression)                                                      \
  do {                                                                                    \
    auto _libatlas_result = (ResultExpression);                                           \
    if (!_libatlas_result) {                                                              \
      ::libatlas_test::fail(__FILE__, __LINE__, _libatlas_result.error().message);       \
    }                                                                                     \
  } while (false)
