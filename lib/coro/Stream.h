
#pragma once

#include "coro/Runtime.h"
#include <asio.hpp>
#include <cstddef>
#include <utility>

namespace coro {

/// Асинхронный поток поверх произвольного asio stream-объекта.
template <typename Handle>
class Stream {
public:
	Stream(Handle handle): _handle(std::move(handle)) {}

	Stream(Stream&& other) = default;
	Stream& operator=(Stream&& other) = default;

	template <typename ...T>
	asio::awaitable<size_t> write(T&&... t) {
		co_return co_await asio::async_write(_handle, asio::buffer(std::forward<T>(t)...), asio::use_awaitable);
	}

	template <typename ...T>
	asio::awaitable<size_t> read(T&&... t) {
		co_return co_await asio::async_read(_handle, asio::buffer(std::forward<T>(t)...), asio::use_awaitable);
	}

	template <typename ...T>
	asio::awaitable<size_t> writeSome(T&&... t) {
		co_return co_await _handle.async_write_some(asio::buffer(std::forward<T>(t)...), asio::use_awaitable);
	}

	template <typename ...T>
	asio::awaitable<size_t> readSome(T&&... t) {
		co_return co_await _handle.async_read_some(asio::buffer(std::forward<T>(t)...), asio::use_awaitable);
	}

	Handle& handle() {
		return _handle;
	}

	const Handle& handle() const {
		return _handle;
	}

protected:
	Handle _handle;
};

}
