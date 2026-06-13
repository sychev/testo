
#pragma once

#include <asio.hpp>
#include "coro/detail/Engine.hpp"

namespace coro {

/// Synchronous-looking stream IO over an asio handle (drop-in for old Stream).
/// Every operation suspends through detail::await(), so it honours the active
/// deadline and cancellation just like before.
template <typename Handle>
class Stream {
public:
	Stream(Handle handle): _handle(std::move(handle)) {}

	Stream(Stream&& other): _handle(std::move(other._handle)) {}
	Stream& operator=(Stream&& other) {
		_handle = std::move(other._handle);
		return *this;
	}

	template <typename ...T>
	size_t write(T&&... t) {
		auto buffer = asio::buffer(std::forward<T>(t)...);
		return detail::await([&](auto token) {
			return asio::async_write(_handle, buffer, token);
		});
	}

	template <typename ...T>
	size_t read(T&&... t) {
		auto buffer = asio::buffer(std::forward<T>(t)...);
		return detail::await([&](auto token) {
			return asio::async_read(_handle, buffer, token);
		});
	}

	template <typename ...T>
	size_t writeSome(T&&... t) {
		auto buffer = asio::buffer(std::forward<T>(t)...);
		return detail::await([&](auto token) {
			return _handle.async_write_some(buffer, token);
		});
	}

	template <typename ...T>
	size_t readSome(T&&... t) {
		auto buffer = asio::buffer(std::forward<T>(t)...);
		return detail::await([&](auto token) {
			return _handle.async_read_some(buffer, token);
		});
	}

	Handle& handle() { return _handle; }
	const Handle& handle() const { return _handle; }

protected:
	Handle _handle;
};

}
