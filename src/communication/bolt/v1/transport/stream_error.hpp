#pragma once

#include "utils/exceptions/stacktrace_exception.hpp"

namespace bolt {

class StreamError : StacktraceException {
 public:
  using StacktraceException::StacktraceException;
};
}
