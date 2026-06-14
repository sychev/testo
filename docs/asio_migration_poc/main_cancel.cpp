// PoC #2: отмена asio-операции через coro::Timeout, проброшенная мостом.
#include <asio.hpp>
#include <coro/Application.h>
#include <coro/Timeout.h>
#include <testo_guest_additions_protocol/coro_asio_bridge.hpp>

#include <chrono>
#include <future>
#include <iostream>
#include <optional>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using asio::ip::tcp;

// Сервер принимает соединение и НИЧЕГО не отвечает -> клиентский read зависнет.
static void run_silent_server(unsigned short port, std::promise<void>& ready, std::promise<void>& stop) {
	asio::io_context ctx;
	tcp::acceptor acceptor(ctx, tcp::endpoint(tcp::v4(), port));
	ready.set_value();
	tcp::socket sock = acceptor.accept();
	stop.get_future().wait(); // держим соединение открытым
}

int main() {
	const unsigned short port = 34568;
	std::promise<void> ready, stop;
	auto ready_future = ready.get_future();
	std::thread server(run_silent_server, port, std::ref(ready), std::ref(stop));
	ready_future.wait();

	int result = 1;
	coro::Application([&] {
		std::optional<tcp::socket> sock;
		try {
			sock.emplace(coro::IoService::current()->_impl);
			coro::await(sock->async_connect(
				tcp::endpoint(asio::ip::make_address("127.0.0.1"), port),
				asio::use_awaitable));

			coro::Timeout timeout(200ms); // должен прервать вечный read
			uint32_t size = 0;
			coro::await(asio::async_read(*sock, asio::buffer(&size, 4), asio::use_awaitable));

			std::cerr << "ERROR: read неожиданно завершился" << std::endl;
		} catch (const coro::TimeoutError&) {
			std::cout << "PoC OK: read прерван по coro::Timeout, asio-операция отменена через мост" << std::endl;
			result = 0;
		} catch (const std::exception& e) {
			std::cerr << "OTHER EXCEPTION: " << e.what() << std::endl;
		}
	}).run();

	stop.set_value();
	server.join();
	return result;
}
