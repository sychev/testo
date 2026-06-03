
#pragma once

#include <net/Cancel.hpp>
#include <net/Deadline.hpp>

namespace net {

/*!
	@brief Точка кооперативной отмены (аналог coro::CheckPoint)

	Вызывайте в длинных CPU-циклах. Бросает Interruption, если запрошена
	отмена (Ctrl-C), и TimeoutError, если истёк действующий Deadline.
*/
inline void check_point() {
	if (interrupt_requested()) {
		throw Interruption{};
	}
	if (Deadline::expired()) {
		throw TimeoutError{};
	}
}

}
