
#pragma once

// The old AsioTask1 / AsioTask2 helpers bridged a raw asio callback to a fiber
// suspend/resume. With asio::spawn that bridge is no longer needed: the
// yield_context IS the completion token, and detail::await() is the single
// suspension choke point. New code should call detail::await() directly.
//
// This header is kept so that stray `#include <coro/AsioTask.h>` lines keep
// resolving; it just pulls in the engine.

#include "coro/detail/Engine.hpp"
