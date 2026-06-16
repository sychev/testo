
#pragma once

#include <thread>
#include <chrono>
#include <memory>
#include <iostream>

#include "Messages.hpp"
#include <nlohmann/json.hpp>

#include <coro/StreamSocket.h>
#include <coro/Runtime.h>

using Socket = coro::StreamSocket<asio::ip::tcp>;
using Endpoint = asio::ip::tcp::endpoint;

struct Channel {
	Channel(Socket _socket): socket(std::move(_socket)) {}
	~Channel() = default;

	asio::awaitable<nlohmann::json> recv();
	asio::awaitable<void> send(const nlohmann::json& message);

	Socket socket;
};


inline asio::awaitable<nlohmann::json> Channel::recv() {
	uint32_t msg_size;

	co_await socket.read((uint8_t*)&msg_size, 4);

	std::vector<uint8_t> json_data;
	json_data.resize(msg_size);
	co_await socket.read((uint8_t*)json_data.data(), json_data.size());

	co_return nlohmann::json::from_cbor(json_data);
}

inline asio::awaitable<void> Channel::send(const nlohmann::json& json) {
	std::vector<uint8_t> json_data = nlohmann::json::to_cbor(json);
	uint32_t json_size = (uint32_t)json_data.size();
	co_await socket.write((uint8_t*)&json_size, sizeof(json_size));
	co_await socket.write((uint8_t*)json_data.data(), json_size);
}
