
#pragma once

#include "coro/IoService.h"
#include "coro/AsioTask.h"
#include <string>

namespace coro {

/*!
	@brief Wrapper вокруг asio::ip::basic_resolver

	В asio 1.36 устаревшие basic_resolver::query / basic_resolver::iterator удалены, поэтому
	используется современный async_resolve(protocol, host, service) с результатом results_type.
*/
template <typename InternetProtocol>
class Resolver {
public:
	typedef asio::ip::basic_resolver<InternetProtocol> Impl;
	typedef typename Impl::results_type Results;

	Resolver(): _handle(IoService::current()->_impl) {}

	Results resolve(const InternetProtocol& protocol, const std::string& host, const std::string& service) {
		return awaitValue<Results>([&](auto&& token) {
			return _handle.async_resolve(protocol, host, service, std::forward<decltype(token)>(token));
		});
	}

private:
	Impl _handle;
};

typedef Resolver<asio::ip::udp> UdpResolver;
typedef Resolver<asio::ip::tcp> TcpResolver;

}
