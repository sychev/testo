
#pragma once

#include <asio.hpp>
#include "coro/detail/Engine.hpp"

namespace coro {

/// Keeps the event loop alive while in scope (drop-in for old coro::Work).
class Work {
private:
	asio::executor_work_guard<asio::any_io_executor> _impl = asio::make_work_guard(detail::executor());
};

}
