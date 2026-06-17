
#pragma once

#include <atomic>
#include <functional>

/*
	Минимальный разделяемый контракт прерывания, заменяющий вброс исключения,
	который раньше делал coro (CancelError/Interruption через файбер).

	- g_interrupted     взводится обработчиком сигнала (только в хосте testo);
	                    в серверных бинарях остаётся false.
	- g_cancel_current  блокирующий фасад выставляет его на время своей операции
	                    как "отменить мой активный asio-объект" (socket.cancel()
	                    или timer.cancel()), чтобы обработчик сигнала мог оборвать
	                    висящую операцию. Снимается по выходу из фасада.

	Каждый бинарь определяет эти переменные у себя один раз.
*/
struct Interruption {};

extern std::atomic<bool> g_interrupted;
extern std::function<void()> g_cancel_current;
