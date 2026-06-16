
#include "Channel.hpp"
#include <coro/Timer.h>
#include <chrono>

asio::awaitable<nlohmann::json> Channel::receive() {
	uint32_t msg_size;
	while (true) {
		size_t bytes_read = co_await read((uint8_t*)&msg_size, 4);
		if (bytes_read == 0) {
			co_await coro::Timer().waitFor(std::chrono::milliseconds(100));
			continue;
		} else if (bytes_read != 4) {
			throw std::runtime_error("Can't read msg size");
		} else {
			break;
		}
	}

	std::string json_str;
	json_str.resize(msg_size);
	co_await receive_raw((uint8_t*)json_str.data(), json_str.size());

	nlohmann::json result = nlohmann::json::parse(json_str);
	co_return result;
}

asio::awaitable<void> Channel::send(nlohmann::json response) {
	response["version"] = TESTO_VERSION;
	auto response_str = response.dump();
	uint32_t response_size = (uint32_t)response_str.size();
	co_await send_raw((uint8_t*)&response_size, sizeof(response_size));
	co_await send_raw((uint8_t*)response_str.data(), response_size);
}

asio::awaitable<void> Channel::receive_raw(uint8_t* data, size_t size) {
	size_t already_read = 0;
	while (already_read < size) {
		size_t n = co_await read(&data[already_read], size - already_read);
		if (n == 0) {
			throw std::runtime_error("EOF while reading");
		}
		already_read += n;
	}
}

asio::awaitable<void> Channel::send_raw(uint8_t* data, size_t size) {
	size_t already_send = 0;
	while (already_send < size) {
		size_t n = co_await write(&data[already_send], size - already_send);
		if (n == 0) {
			throw std::runtime_error("EOF while writing");
		}
		already_send += n;
	}
}
