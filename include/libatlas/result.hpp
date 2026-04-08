#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace libatlas {

enum class ErrorCode {
  None = 0,
  InvalidArgument,
  UnsupportedFormat,
  BufferSizeMismatch,
  IoError,
  DecodeError,
  EncodeError,
  ParseError,
  PackingFailed
};

struct Error {
  ErrorCode code = ErrorCode::None;
  std::string message;

  explicit operator bool() const noexcept { return code != ErrorCode::None; }
};

template <typename T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}
  Result(Error error) : error_(std::move(error)) {}

  static Result<T> success(T value) { return Result<T>(std::move(value)); }

  static Result<T> failure(ErrorCode code, std::string message) {
    return Result<T>(Error{code, std::move(message)});
  }

  bool ok() const noexcept { return value_.has_value(); }
  explicit operator bool() const noexcept { return ok(); }

  T& value() & {
    if (!value_) {
      throw std::logic_error("attempted to access a failed Result value");
    }
    return *value_;
  }

  const T& value() const& {
    if (!value_) {
      throw std::logic_error("attempted to access a failed Result value");
    }
    return *value_;
  }

  T&& value() && {
    if (!value_) {
      throw std::logic_error("attempted to access a failed Result value");
    }
    return std::move(*value_);
  }

  const Error& error() const noexcept { return error_; }

 private:
  std::optional<T> value_;
  Error error_;
};

template <>
class Result<void> {
 public:
  Result() : ok_(true) {}
  Result(Error error) : ok_(false), error_(std::move(error)) {}

  static Result<void> success() { return Result<void>(); }

  static Result<void> failure(ErrorCode code, std::string message) {
    return Result<void>(Error{code, std::move(message)});
  }

  bool ok() const noexcept { return ok_; }
  explicit operator bool() const noexcept { return ok(); }

  const Error& error() const noexcept { return error_; }

 private:
  bool ok_ = false;
  Error error_;
};

}  // namespace libatlas
