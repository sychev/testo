
#pragma once

#include <asio.hpp>
#include <chrono>
#include <interruption/Interruption.hpp>
#include <interruption/AsyncOp.hpp>

/*
	Единственный io_context хостового процесса testo.

	Раньше его роль играл coro::Application/IoService::current(). Теперь это
	обычный asio::io_context, который крутится внутри блокирующих фасадов
	(их циклы run_one) и в check-точках прерывания (poll). Определяется один
	раз в main.cpp.

	Общие протокольные библиотеки (Channel, GuestAdditions) на g_io НЕ
	завязаны: они достают io_context прямо из своего сокета. g_io нужен только
	хостовым местам, которые сами создают сокеты/таймеры.
*/
extern asio::io_context g_io;

/*
	Точка проверки прерывания. Прокачивает g_io (не засыпая), чтобы успел
	выполниться обработчик сигнала, и бросает Interruption, если был Ctrl-C.
	Ставится в чисто вычислительных местах, где нет сетевых ожиданий.
*/
inline void check_interruption() {
	g_io.poll();
	if (g_interrupted) {
		throw Interruption();
	}
}

/*
	Прерываемый аналог sleep: ждёт заданную длительность на g_io. Возвращается
	по истечении времени; бросает Interruption при Ctrl-C.
*/
inline void interruptible_sleep_for(std::chrono::steady_clock::duration duration) {
	asio::steady_timer timer(g_io);
	timer.expires_after(duration);
	await_io(timer, [&](auto h){ timer.async_wait(h); });
}
