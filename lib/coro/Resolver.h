
#pragma once

#include "coro/Runtime.h"
#include <asio.hpp>

namespace coro {

/// Обёртка вокруг asio::ip::basic_resolver.
template <typename InternetProtocol>
class Resolver {
public:
	using Impl = asio::ip::basic_resolver<InternetProtocol>;
	using results_type = typename Impl::results_type;

	Resolver(): _handle(current_executor()) {}

	asio::awaitable<results_type> resolve(const std::string& host, const std::string& service) {
		co_return co_await _handle.async_resolve(host, service, asio::use_awaitable);
	}

private:
	Impl _handle;
};

using UdpResolver = Resolver<asio::ip::udp>;
using TcpResolver = Resolver<asio::ip::tcp>;

}
