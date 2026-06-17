
#pragma once

#include <asio.hpp>
#include <interruption/Interruption.hpp>

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
