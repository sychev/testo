
#pragma once

// The hand-rolled ucontext fiber engine (the old Coro/Fiber classes) has been
// removed. Coroutines are now provided by asio::spawn; the shared machinery
// lives in coro/detail/Engine.hpp. This header is kept only so that existing
// `#include "coro/Coro.h"` lines keep resolving.

#include "coro/detail/Engine.hpp"
