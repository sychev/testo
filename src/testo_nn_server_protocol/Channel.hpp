
#pragma once

#include <thread>
#include <chrono>
#include <memory>
#include <iostream>

#include "Messages.hpp"
#include <interruption/Interruption.hpp>
#include <nlohmann/json.hpp>

#include <asio.hpp>

using Socket = asio::ip::tcp::socket;
using Endpoint = asio::ip::tcp::endpoint;

struct Channel {
	Channel(Socket _socket): socket(std::move(_socket)) {}
	~Channel() = default;

	nlohmann::json recv();
	void send(const nlohmann::json& message);

	Socket socket;

private:
	void read_all(uint8_t* data, size_t size);
	void write_all(const uint8_t* data, size_t size);
};


inline void Channel::read_all(uint8_t* data, size_t size) {
	asio::io_context& io = static_cast<asio::io_context&>(socket.get_executor().context());
	std::error_code op_ec;
	bool done = false;
	auto prev_cancel = g_cancel_current;
	g_cancel_current = [this]{ socket.cancel(); };
	asio::async_read(socket, asio::buffer(data, size),
		[&](const std::error_code& ec, size_t) { op_ec = ec; done = true; });
	while (!done) {
		io.run_one();
	}
	g_cancel_current = prev_cancel;
	if (op_ec == asio::error::operation_aborted && g_interrupted) {
		throw Interruption();
	}
	if (op_ec) {
		throw std::system_error(op_ec);
	}
}

inline void Channel::write_all(const uint8_t* data, size_t size) {
	asio::io_context& io = static_cast<asio::io_context&>(socket.get_executor().context());
	std::error_code op_ec;
	bool done = false;
	auto prev_cancel = g_cancel_current;
	g_cancel_current = [this]{ socket.cancel(); };
	asio::async_write(socket, asio::buffer(data, size),
		[&](const std::error_code& ec, size_t) { op_ec = ec; done = true; });
	while (!done) {
		io.run_one();
	}
	g_cancel_current = prev_cancel;
	if (op_ec == asio::error::operation_aborted && g_interrupted) {
		throw Interruption();
	}
	if (op_ec) {
		throw std::system_error(op_ec);
	}
}

inline nlohmann::json Channel::recv() {
	uint32_t msg_size;

	read_all((uint8_t*)&msg_size, 4);

	std::vector<uint8_t> json_data;
	json_data.resize(msg_size);
	read_all((uint8_t*)json_data.data(), json_data.size());

	return nlohmann::json::from_cbor(json_data);
}

inline void Channel::send(const nlohmann::json& json) {
	std::vector<uint8_t> json_data = nlohmann::json::to_cbor(json);
	uint32_t json_size = (uint32_t)json_data.size();
	write_all((uint8_t*)&json_size, sizeof(json_size));
	write_all((uint8_t*)json_data.data(), json_size);
}
