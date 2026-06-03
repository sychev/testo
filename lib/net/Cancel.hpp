
#pragma once

#include <stop_token>

namespace net {

/*!
	@brief Исключение кооперативной отмены (Ctrl-C, SIGTERM)

	Специально НЕ наследуется от std::exception, чтобы его не "съедали"
	блоки catch (const std::exception&) внутри интерпретатора и чтобы
	стек гарантированно полностью раскручивался до самого верха.
	Это прямой аналог старого coro::CancelError.
*/
struct Interruption {};

/// Процесс-глобальный источник отмены (C++20). Обычно его дёргает обработчик сигналов.
std::stop_source& interrupt_source();

/// stop_token, по которому можно подписаться на отмену (для std::jthread и т.п.)
inline std::stop_token interrupt_token() {
	return interrupt_source().get_token();
}

inline bool interrupt_requested() {
	return interrupt_source().stop_requested();
}

inline void request_interrupt() {
	interrupt_source().request_stop();
}

/// Сбрасывает запрос отмены, заменяя источник новым (инвалидирует прежние токены)
void reset_interrupt();

}
