
#pragma once

#include <asio.hpp>
#include <queue>

namespace coro {

/// Wrapper вокруг asio::io_service
class IoService {
public:
	static IoService* current();

	void run();

	template <typename T>
	void post(T&& t) {
		asio::post(_impl, std::forward<T>(t));
	}

	template <typename T>
	void dispatch(T&& t) {
		asio::dispatch(_impl, std::forward<T>(t));
	}

	std::queue<std::function<void()>> checkpoints;

	asio::io_context _impl;
};

}