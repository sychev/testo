
#pragma once

#include "coro/detail/Engine.hpp"

namespace coro {

/// Wrapper around asio::ip::basic_resolver (drop-in for old coro::Resolver).
template <typename InternetProtocol>
class Resolver {
public:
	typedef asio::ip::basic_resolver<InternetProtocol> Impl;
	typedef typename Impl::iterator Iterator;
	typedef typename Impl::query Query;

	Resolver(): _handle(detail::io()) {}

	Iterator resolve(const Query& query) {
		return detail::await([&](auto token) {
			return _handle.async_resolve(query, token);
		});
	}

private:
	Impl _handle;
};

typedef Resolver<asio::ip::udp> UdpResolver;
typedef Resolver<asio::ip::tcp> TcpResolver;

}
