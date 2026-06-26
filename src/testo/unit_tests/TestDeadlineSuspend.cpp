
#include <catch.hpp>

#include <asio.hpp>
#include <interruption/AsyncOp.hpp>
#include <testo_guest_additions_protocol/GuestAdditions.hpp>

#include <chrono>
#include <thread>

/*
	Регрессия на DeadlineGuard::suspend()/resume().

	Проверяем главное свойство: дедлайн в await_io взводится пер-операционно (на
	абсолютный this->deadline), а suspend/resume сдвигают это значение между
	операциями — поэтому следующий recv_raw честно получает продлённый таймаут.
	Если бы suspend/resume не работали (или сдвигали "не те" дедлайны), пауза
	списалась бы в счёт таймаута и тест бы это поймал.

	Тест самодостаточен: связанная пара TCP-сокетов на loopback, server-конец
	молчит, поэтому recv_raw на client всегда упирается в дедлайн. Никакой
	гипервизор/гость не нужен.
*/

using namespace std::chrono;

namespace {

struct TestGA: GuestAdditions {
	asio::ip::tcp::socket& sock;
	explicit TestGA(asio::ip::tcp::socket& s): sock(s) {}

	void send_raw(const uint8_t*, size_t) override {}
	void recv_raw(uint8_t* data, size_t size) override {
		auto ec = await_io(sock, [&](auto h){ asio::async_read(sock, asio::buffer(data, size), h); }, deadline);
		if (ec == asio::error::operation_aborted) {
			throw std::runtime_error("Timeout was triggered");
		}
		if (ec) {
			throw std::system_error(ec);
		}
	}
};

// Связанная пара сокетов на loopback. server держится открытым, но ничего не
// пишет, поэтому чтение на client всегда доходит до дедлайна (а не до EOF).
// work_guard не даёт пустому io_context перейти в stopped между операциями —
// в реальном хосте ту же роль играет вечный signals.async_wait на g_io.
struct SocketPair {
	asio::io_context io;
	asio::executor_work_guard<asio::io_context::executor_type> work;
	asio::ip::tcp::acceptor acceptor;
	asio::ip::tcp::socket client;
	asio::ip::tcp::socket server;

	SocketPair()
		: work(asio::make_work_guard(io)),
		  acceptor(io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)),
		  client(io),
		  server(io)
	{
		auto port = acceptor.local_endpoint().port();
		bool connected = false, accepted = false;
		client.async_connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), port),
			[&](const std::error_code&){ connected = true; });
		acceptor.async_accept(server, [&](const std::error_code&){ accepted = true; });
		while (!connected || !accepted) {
			io.run_one();
		}
	}
};

long elapsed_ms(steady_clock::time_point t0) {
	return duration_cast<milliseconds>(steady_clock::now() - t0).count();
}

} // namespace

TEST_CASE("await_io: deadline fires roughly at the deadline") {
	SocketPair p;
	TestGA ga(p.client);
	uint8_t buf[1];

	auto guard = ga.with_deadline(milliseconds(200));
	auto t0 = steady_clock::now();
	REQUIRE_THROWS_AS(ga.recv_raw(buf, sizeof(buf)), std::runtime_error);
	long el = elapsed_ms(t0);
	INFO("timed out after " << el << " ms");
	REQUIRE(el >= 150);          // не раньше дедлайна
	REQUIRE(el <= 2000);         // щедрый потолок для нагруженного CI
}

TEST_CASE("DeadlineGuard::suspend/resume extends the next operation's deadline") {
	SocketPair p;
	TestGA ga(p.client);
	uint8_t buf[1];

	// Контроль: дедлайн 200ms без паузы.
	long control = 0;
	{
		auto guard = ga.with_deadline(milliseconds(200));
		auto t0 = steady_clock::now();
		REQUIRE_THROWS_AS(ga.recv_raw(buf, sizeof(buf)), std::runtime_error);
		control = elapsed_ms(t0);
	}

	// Тот же дедлайн, но МЕЖДУ операциями — пауза 150ms (как callback в execute()).
	long shifted = 0;
	{
		auto guard = ga.with_deadline(milliseconds(200));
		auto t0 = steady_clock::now();
		guard.suspend();
		std::this_thread::sleep_for(milliseconds(150));
		guard.resume();
		REQUIRE_THROWS_AS(ga.recv_raw(buf, sizeof(buf)), std::runtime_error);
		shifted = elapsed_ms(t0);
	}

	INFO("control=" << control << "ms shifted=" << shifted << "ms");
	// Пауза должна добавиться к дедлайну: shifted ~ control + 150ms. Если
	// suspend/resume не работают, было бы shifted ~ control. Требуем хотя бы
	// +100ms запаса (sleep гарантирует >=150ms, остальное — на шум измерений).
	REQUIRE(shifted >= control + 100);
}

TEST_CASE("DeadlineGuard::suspend/resume credits the pause to outer (nested) deadlines too") {
	// Чисто на арифметику дедлайнов, без сокета: сдвиг должен дойти и до
	// внешнего гарда, иначе после раскрутки внутреннего внешний "съест" паузу.
	struct BareGA: GuestAdditions {
		void send_raw(const uint8_t*, size_t) override {}
		void recv_raw(uint8_t*, size_t) override {}
	} ga;

	constexpr auto never = steady_clock::time_point::max();
	REQUIRE(ga.deadline == never);

	auto outer = ga.with_deadline(milliseconds(10000));
	auto outer_deadline = ga.deadline;
	{
		auto inner = ga.with_deadline(milliseconds(3000));
		REQUIRE(ga.deadline < outer_deadline);          // min победил

		inner.suspend();
		std::this_thread::sleep_for(milliseconds(60));
		inner.resume();
	} // ~inner: ga.deadline := (сдвинутый) prev внешнего гарда

	long outer_shift = duration_cast<milliseconds>(ga.deadline - outer_deadline).count();
	INFO("outer deadline shifted by " << outer_shift << " ms");
	REQUIRE(outer_shift >= 50);     // пауза ~60ms докинулась и во внешний дедлайн
	REQUIRE(outer_shift <= 2000);

	// resume без suspend — no-op.
	auto before = ga.deadline;
	outer.resume();
	REQUIRE(ga.deadline == before);
}
