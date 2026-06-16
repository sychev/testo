
#pragma once

#include <asio.hpp>
#include <asio/spawn.hpp>
#include <exception>
#include <functional>
#include <memory>

namespace coro {

using executor_t = asio::io_context::executor_type;
using strand_t   = asio::strand<executor_t>;

/*!
	@brief Исключение для отмены корутин

	Специально не наследуется от std::exception, чтобы гарантированно полностью раскрутить
	стек корутины. Помни об этом, когда будешь писать catch (...)
*/
struct CancelError {};

class Timeout;

/*!
	@brief Дескриптор одной корутины (spawned-функции asio)

	В MT-модели каждая корутина запускается через asio::spawn на собственном strand. Это
	гарантирует, что корутина и все её возобновления сериализованы (она никогда не выполняется
	на двух потоках одновременно), при этом разные корутины параллелятся по потокам общего
	io_context.

	Coro хранит:
	  - strand, на котором живёт корутина;
	  - указатель на её yield_context (валиден, пока тело корутины выполняется);
	  - cancellation_signal, через который снаружи запрашивается отмена;
	  - "ожидающий" Timeout, превращающий ближайшую отмену в TimeoutError.
*/
class Coro: public std::enable_shared_from_this<Coro> {
public:
	/// Текущая корутина на этом потоке (валидна между точками ожидания)
	static Coro* current();
	static Coro* currentOrNull();
	/// Установить текущую корутину (служебное; вызывается обёрткой запуска и await-хелпером)
	static void setCurrent(Coro* coro);

	explicit Coro(strand_t strand);

	strand_t& strand() { return _strand; }
	asio::yield_context& yield() { return *_yield; }

	/// Запросить отмену корутины (безопасно с любого потока). Доставляется как CancelError.
	void cancel();

	/// Завершилась ли корутина
	bool done() const { return _done; }

	// --- служебное, используется обёрткой запуска и Timeout ---
	void bindYield(asio::yield_context& y) { _yield = &y; }
	asio::cancellation_signal& signal() { return _signal; }
	void markDone() { _done = true; }
	/// Пометить, что ближайшая отмена этой корутины должна стать TimeoutError (вызывать на strand)
	void requestTimeout(Timeout* timeout);
	Timeout* takePendingTimeout();

private:
	strand_t _strand;
	asio::yield_context* _yield = nullptr;
	asio::cancellation_signal _signal;
	Timeout* _pendingTimeout = nullptr;
	bool _done = false;
};

/*!
	@brief Запустить корутину на собственном strand общего io_context

	@param io      общий io_context
	@param routine тело корутины
	@param onDone  колбэк по завершении (вызывается на strand корутины)
	@return        разделяемый дескриптор корутины (живёт минимум до завершения тела)
*/
std::shared_ptr<Coro> go(asio::io_context& io,
                         std::function<void()> routine,
                         std::function<void()> onDone = {});

}
