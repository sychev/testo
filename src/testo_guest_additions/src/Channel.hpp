
#pragma once

#include <nlohmann/json.hpp>
#include <coro/Runtime.h>
#include <cstdint>

struct Channel {
	virtual ~Channel() = default;

	asio::awaitable<nlohmann::json> receive();
	asio::awaitable<void> send(nlohmann::json response);

	asio::awaitable<void> receive_raw(uint8_t* data, size_t size);
	asio::awaitable<void> send_raw(uint8_t* data, size_t size);

	virtual asio::awaitable<size_t> read(uint8_t* data, size_t size) = 0;
	virtual asio::awaitable<size_t> write(uint8_t* data, size_t size) = 0;
};
