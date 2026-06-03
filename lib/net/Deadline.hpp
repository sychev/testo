
#pragma once

#include <chrono>
#include <optional>
#include <stdexcept>

namespace net {

using Clock = std::chrono::steady_clock;

/// Исключение, выбрасываемое при срабатывании дедлайна (аналог coro::TimeoutError)
class TimeoutError: public std::runtime_error {
public:
	TimeoutError(): std::runtime_error("Timeout was triggered") {}
};

/*!
	@brief Ограничение времени на текущую область видимости (аналог coro::Timeout)

	RAII-объект: пока он жив, в этом потоке действует дедлайн. И check_point(),
	и блокирующий ввод/вывод (см. IoPump) сверяются с ним и бросают TimeoutError,
	когда время истекло. Дедлайны можно вкладывать — действует самый ранний.

	@warning Как и у coro::Timeout, это объявление ПЕРЕМЕННОЙ, а не функции:
	@code
		net::Deadline deadline(std::chrono::seconds(10));
	@endcode
*/
class Deadline {
public:
	template <typename Duration>
	explicit Deadline(Duration duration) {
		push(Clock::now() + std::chrono::duration_cast<Clock::duration>(duration));
	}
	~Deadline();

	Deadline(const Deadline&) = delete;
	Deadline& operator=(const Deadline&) = delete;

	/// Текущий действующий дедлайн в этом потоке (если есть)
	static std::optional<Clock::time_point> current();
	/// Истёк ли текущий дедлайн
	static bool expired();

private:
	void push(Clock::time_point deadline);
};

}
