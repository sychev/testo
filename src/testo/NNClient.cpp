
#include "NNClient.hpp"
#include "Logger.hpp"
#include "Exceptions.hpp"
#include "Utils.hpp"

#include <coro/Timer.h>
#include <iostream>

using namespace std::chrono_literals;

static const VersionNumber client_version(TESTO_VERSION);
static const VersionNumber minimal_server_version(3, 2, 0);

asio::ip::tcp::endpoint parse_tcp_endpoint(const std::string& endpoint);

NNClient::NNClient(const std::string& endpoint_):
	endpoint(parse_tcp_endpoint(endpoint_)),
	channel(new Channel(Socket()))
{
	TRACE();
}

NNClient::~NNClient() {
	TRACE();
}

bool is_connection_lost(const std::error_code& code) {
	int value = code.value();
	if (value == ECONNABORTED ||
		value == ECONNRESET ||
		value == ENETDOWN ||
		value == ENETRESET ||
		value == ENOENT ||
		value == EPIPE ||
		value == ENOTCONN)
	{
		return true;
	} else {
		return false;
	}
}

asio::awaitable<void> NNClient::establish_connection() {
	co_await establish_connection_wrapper([&]() -> asio::awaitable<void> {
		channel->socket = Socket();
		co_await channel->socket.connect(endpoint);
	});

	co_await channel->send(create_handshake_request(client_version));
	nlohmann::json response = co_await channel->recv();
	std::string type = response.at("type");
	if (type == ERROR_RESPONSE) {
		server_version = VersionNumber(3, 0, 0);
	} else if (type == HANDSHAKE_RESPONSE) {
		server_version = response.at("server_version").get<std::string>();
	} else {
		throw std::runtime_error(std::string("Unexpected message type: ") + type);
	}
	if (server_version < minimal_server_version) {
		throw std::runtime_error("Testo NN Server has an incompatible version. You should update it to the version " + minimal_server_version.to_string() + " or higher");
	}
}

asio::awaitable<nlohmann::json> NNClient::receive_response() {
	nlohmann::json response = co_await channel->recv();
	std::string type = response.at("type");
	if (type == ERROR_RESPONSE) {
		std::string message = response.at("data");
		std::string failure_category = response.at("failure_category");
		throw ExceptionWithCategory(message, failure_category);
	} else if (type == CONTINUE_ERROR_RESPONSE) {
		std::string message = response.at("data");
		throw ContinueError(message);
	}
	co_return response;
}

asio::awaitable<void> NNClient::establish_connection_wrapper(const std::function<asio::awaitable<void>()>& fn) {
	for (size_t i = 0; i < establish_connection_tries; ++i) {
		bool need_retry = false;
		try {
			co_await fn();
			co_return;
		} catch (const std::exception& error) {
			std::cerr << error.what() << std::endl;
			if (i < (establish_connection_tries - 1)) {
				std::cerr << "Failed to connect to the server, reconnecting ...\n";
				need_retry = true;
			}
		}
		if (need_retry) {
			co_await coro::Timer().waitFor(2s);
		}
	}

	throw std::runtime_error("Exceeding the number of attempts to connect to the server");
}

asio::awaitable<nlohmann::json> NNClient::rcp_wrapper(const std::function<asio::awaitable<nlohmann::json>()>& fn) {
	for (size_t i = 0; i < rpc_tries; ++i) {
		bool reconnect_needed = false;
		try {
			co_return co_await fn();
		} catch (const std::system_error& error) {
			if (is_connection_lost(error.code())) {
				std::cerr << error.what() << std::endl;
				if (i < (rpc_tries - 1)) {
					std::cerr << "Lost the connection to the server, reconnecting...\n";
					reconnect_needed = true;
				}
			} else {
				throw;
			}
		}
		if (reconnect_needed) {
			connected = false;
			co_await establish_connection();
			connected = true;
		}
	}
	throw std::runtime_error("Exceeding the number of attempts to execute RPC");
}

asio::awaitable<nlohmann::json> NNClient::eval_js(const stb::Image<stb::RGB>* image, const std::string& script) {
	if (!connected) {
		co_await establish_connection();
		connected = true;
	}

	co_return co_await rcp_wrapper([&]() -> asio::awaitable<nlohmann::json> {
		co_await channel->send(create_js_eval_request(*image, script));

		while (true) {
			nlohmann::json response = co_await receive_response();
			std::string type = response.at("type");
			if (type == REF_IMAGE_REQUEST) {
				std::string ref_file_path = response.at("data");

				stb::Image<stb::RGBA> ref_image;
				try {
					ref_image = stb::Image<stb::RGBA>(ref_file_path);
				} catch (const std::exception& error) {
					std::throw_with_nested(std::runtime_error("NN server requested image " + ref_file_path + " but we failed to open the file"));
				}

				co_await channel->send(create_ref_image_response(ref_image));
				continue;
			} else if (type == JS_EVAL_RESPONSE) {
				co_return response;
			} else {
				throw std::runtime_error(std::string("Unexpected message type: ") + type);
			}
		}
	});
}
