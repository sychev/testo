
#pragma once

#include <coro/IoService.h>

namespace coro {

/// Удерживает io_context запущенным, пока есть незавершённая работа (бывший asio::io_service::work)
class Work {
public:

private:
	asio::executor_work_guard<asio::io_context::executor_type> _impl =
		asio::make_work_guard(IoService::current()->_impl);
};

}
