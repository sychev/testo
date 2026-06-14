// PoC: asio C++20 awaitable transport, вызванный из легаси coro-кода через мост.
#include <asio.hpp>
#include <coro/Application.h>
#include <testo_guest_additions_protocol/coro_asio_bridge.hpp>

#include <cstdint>
#include <future>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using asio::ip::tcp;

// --- Сервер на отдельном потоке: читает [u32 len][payload], отвечает [u32 len]["CONFIRMED:"+payload]
static void run_server(unsigned short port, std::promise<void>& ready) {
	asio::io_context ctx;
	tcp::acceptor acceptor(ctx, tcp::endpoint(tcp::v4(), port));
	ready.set_value();
	tcp::socket sock = acceptor.accept();

	uint32_t size = 0;
	asio::read(sock, asio::buffer(&size, 4));
	std::vector<uint8_t> data(size);
	asio::read(sock, asio::buffer(data));

	std::string reply = "CONFIRMED:" + std::string(data.begin(), data.end());
	uint32_t rsize = (uint32_t)reply.size();
	asio::write(sock, asio::buffer(&rsize, 4));
	asio::write(sock, asio::buffer(reply));
}

// --- Транспорт в целевом стиле: операции — asio::awaitable, наружу торчит
//     синхронный интерфейс (как у ReportWriterNativeRemote), реализованный через мост.
struct RemoteClient {
	tcp::endpoint endpoint;
	std::optional<tcp::socket> sock;

	asio::awaitable<void> async_connect() {
		sock.emplace(co_await asio::this_coro::executor);
		co_await sock->async_connect(endpoint, asio::use_awaitable);
	}

	asio::awaitable<void> async_send(std::vector<uint8_t> data) {
		uint32_t size = (uint32_t)data.size();
		co_await asio::async_write(*sock, asio::buffer(&size, 4), asio::use_awaitable);
		co_await asio::async_write(*sock, asio::buffer(data), asio::use_awaitable);
	}

	asio::awaitable<std::vector<uint8_t>> async_recv() {
		uint32_t size = 0;
		co_await asio::async_read(*sock, asio::buffer(&size, 4), asio::use_awaitable);
		std::vector<uint8_t> data(size);
		co_await asio::async_read(*sock, asio::buffer(data), asio::use_awaitable);
		co_return data;
	}

	// Синхронно выглядящий API для легаси coro-вызовов:
	void connect() { coro::await(async_connect()); }
	void send(std::vector<uint8_t> d) { coro::await(async_send(std::move(d))); }
	std::vector<uint8_t> recv() { return coro::await(async_recv()); }
};

int main() {
	const unsigned short port = 34567;
	std::promise<void> ready;
	auto ready_future = ready.get_future();
	std::thread server(run_server, port, std::ref(ready));
	ready_future.wait();

	int result = 0;
	coro::Application([&] {
		try {
			RemoteClient client;
			client.endpoint = tcp::endpoint(asio::ip::make_address("127.0.0.1"), port);

			client.connect();
			std::string msg = "hello-from-coro-fiber";
			client.send(std::vector<uint8_t>(msg.begin(), msg.end()));
			std::vector<uint8_t> resp = client.recv();

			std::string text(resp.begin(), resp.end());
			std::cout << "[client] server replied: " << text << std::endl;
			if (text != "CONFIRMED:hello-from-coro-fiber") {
				std::cerr << "UNEXPECTED REPLY" << std::endl;
				result = 1;
			} else {
				std::cout << "PoC OK: awaitable-транспорт отработал через coro-мост" << std::endl;
			}
		} catch (const std::exception& e) {
			std::cerr << "ERROR: " << e.what() << std::endl;
			result = 2;
		}
	}).run();

	server.join();
	return result;
}
