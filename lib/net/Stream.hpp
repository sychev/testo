
#pragma once

#include <asio.hpp>
#include <memory>
#include <system_error>

#include <net/IoPump.hpp>

namespace net {

/*!
	@brief Синхронный потоковый ввод/вывод поверх asio (аналог coro::Stream)

	Сохраняет интерфейс read/write/readSome/writeSome. Внутри использует
	блокирующую (но прерываемую и с поддержкой дедлайна) модель IoPump.

	Каждый Stream владеет собственным io_context (на куче, чтобы объект
	оставался перемещаемым: asio-хендлы хранят ссылку на контекст).
*/
template <typename Handle>
class Stream {
public:
	Stream()
		: _io(std::make_unique<asio::io_context>())
		, _handle(*_io)
	{}

	explicit Stream(std::unique_ptr<asio::io_context> io, Handle handle)
		: _io(std::move(io))
		, _handle(std::move(handle))
	{}

	Stream(Stream&&) = default;
	Stream& operator=(Stream&&) = default;

	Stream(const Stream&) = delete;
	Stream& operator=(const Stream&) = delete;

	template <typename ...T>
	size_t write(T&&... t) {
		auto buffer = asio::buffer(std::forward<T>(t)...);
		return transfer([&](auto callback) {
			asio::async_write(_handle, buffer, std::move(callback));
		});
	}

	template <typename ...T>
	size_t read(T&&... t) {
		auto buffer = asio::buffer(std::forward<T>(t)...);
		return transfer([&](auto callback) {
			asio::async_read(_handle, buffer, std::move(callback));
		});
	}

	template <typename ...T>
	size_t writeSome(T&&... t) {
		auto buffer = asio::buffer(std::forward<T>(t)...);
		return transfer([&](auto callback) {
			_handle.async_write_some(buffer, std::move(callback));
		});
	}

	template <typename ...T>
	size_t readSome(T&&... t) {
		auto buffer = asio::buffer(std::forward<T>(t)...);
		return transfer([&](auto callback) {
			_handle.async_read_some(buffer, std::move(callback));
		});
	}

	Handle& handle() { return _handle; }
	const Handle& handle() const { return _handle; }

	asio::io_context& io() { return *_io; }

protected:
	// Запускает операцию переноса данных и дожидается её завершения.
	template <typename Operation>
	size_t transfer(Operation operation) {
		std::error_code error_code;
		size_t transferred = 0;
		detail::pump(*_io, _handle, [&](auto on_done) {
			operation([&, on_done](const std::error_code& ec, size_t bytes) {
				error_code = ec;
				transferred = bytes;
				on_done();
			});
		});
		if (error_code) {
			throw std::system_error(error_code);
		}
		return transferred;
	}

	std::unique_ptr<asio::io_context> _io;
	Handle _handle;
};

}
