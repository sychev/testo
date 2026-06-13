
#include "coro/CheckPoint.h"
#include "coro/detail/Engine.hpp"

namespace coro {

void CheckPoint() {
	detail::await([](auto token) {
		return asio::post(detail::executor(), token);
	});
}

}
