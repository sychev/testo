
#pragma once

#include <asio.hpp>
#include <system_error>
#include <chrono>
#include <interruption/Interruption.hpp>

/*
	Блокирующий фасад над ОДНОЙ async-операцией asio.

	Запускает операцию через initiate(handler), крутит io_context до её
	завершения и возвращает итоговый error_code. Универсальная часть —
	кооперативное прерывание (Ctrl-C) и опциональный абсолютный дедлайн —
	спрятана здесь.

	- cancel_target: asio-объект (socket/timer), чей .cancel() обрывает операцию;
	                 его же executor даёт io_context, который мы крутим.
	- initiate(h):   запускает ровно одну async-операцию, передав ей h;
	                 h вызывается как h(ec) или h(ec, size) — лишнее игнорируется.
	- deadline:      абсолютный дедлайн; при наступлении cancel_target отменяется.
	                 max() == без дедлайна (тогда это обычный простой фасад,
	                 outstanding остаётся 1 и таймер не взводится).

	Бросает Interruption при Ctrl-C. Прочие коды возвращает вызывающему — он сам
	решает: Timeout / system_error / игнорировать.
*/
template <class Cancellable, class Initiate>
std::error_code await_io(
	Cancellable& cancel_target,
	Initiate&& initiate,
	std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max())
{
	auto& io = static_cast<asio::io_context&>(cancel_target.get_executor().context());

	std::error_code op_ec;
	int outstanding = 1;

	asio::steady_timer timer(io);
	const bool timed = deadline != std::chrono::steady_clock::time_point::max();
	if (timed) {
		++outstanding;
		timer.expires_at(deadline);
		timer.async_wait([&](const std::error_code& ec) {
			--outstanding;
			if (!ec) {
				cancel_target.cancel();   // дедлайн вышел -> обрываем операцию
			}
		});
	}

	auto prev_cancel = g_cancel_current;
	g_cancel_current = [&]{ cancel_target.cancel(); };

	initiate([&](const std::error_code& ec, auto&&...) {
		op_ec = ec;
		--outstanding;
		if (timed) {
			timer.cancel();
		}
	});

	while (outstanding) {
		io.run_one();
	}

	g_cancel_current = prev_cancel;

	if (op_ec == asio::error::operation_aborted && g_interrupted) {
		throw Interruption();
	}
	return op_ec;
}
