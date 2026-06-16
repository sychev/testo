
#include "coro/Coro.h"

namespace coro {

// Текущая корутина на данном потоке. Валидна только между точками ожидания: при каждом
// возобновлении после await его восстанавливает await-хелпер (см. AsioTask.h), а в начале
// тела — обёртка go().
thread_local Coro* t_current = nullptr;

Coro* Coro::current() {
	if (!t_current) {
		throw std::logic_error("Coro::current() is nullptr");
	}
	return t_current;
}

Coro* Coro::currentOrNull() {
	return t_current;
}

void Coro::setCurrent(Coro* coro) {
	t_current = coro;
}

Coro::Coro(strand_t strand): _strand(std::move(strand)) {}

void Coro::cancel() {
	// Отмена может прийти с любого потока, поэтому выполняем emit на strand корутины —
	// так он сериализуется с самой корутиной и её операциями.
	auto self = shared_from_this();
	asio::post(_strand, [self] {
		// _done выставляется в теле корутины на этом же strand, поэтому проверка безопасна.
		// Отмена уже завершённой корутины — no-op: иначе emit обратился бы к cancellation-slot
		// последней (уже разрушенной) операции и упал бы.
		if (!self->_done) {
			self->_signal.emit(asio::cancellation_type::terminal);
		}
	});
}

void Coro::requestTimeout(Timeout* timeout) {
	// Вызывается из обработчика таймера, уже на strand корутины.
	_pendingTimeout = timeout;
	_signal.emit(asio::cancellation_type::terminal);
}

Timeout* Coro::takePendingTimeout() {
	auto timeout = _pendingTimeout;
	_pendingTimeout = nullptr;
	return timeout;
}

std::shared_ptr<Coro> go(asio::io_context& io,
                         std::function<void()> routine,
                         std::function<void()> onDone) {
	auto coro = std::make_shared<Coro>(asio::make_strand(io));

	// Стартуем корутину строго из контекста цикла событий, а не inline внутри вызывающей
	// корутины: asio::spawn со свежего strand имеет свойство запускать тело синхронно
	// (dispatch), и если такой "inline"-ребёнок тут же приостановится, это ломает учёт
	// fiber-стека asio (вызывающая корутина падает на следующем suspend). Отложенный post
	// гарантирует, что тело корутины начнётся уже на чистом контексте цикла.
	asio::post(coro->strand(),
		[coro, routine = std::move(routine), onDone = std::move(onDone)]() mutable {
	asio::spawn(coro->strand(),
		[coro, routine = std::move(routine), onDone = std::move(onDone)](asio::yield_context yield) {
			coro->bindYield(yield);
			Coro::setCurrent(coro.get());
			try {
				routine();
			}
			catch (const CancelError&) {
				// Отмена — штатное завершение, ничего не делаем.
			}
			catch (...) {
				// В MT-модели необработанные исключения корутины должен подхватывать тот,
				// кто её запустил (например, CoroPool). Низкоуровневый go() их не хранит:
				// CoroPool оборачивает routine так, чтобы перехватить исключение до onDone.
			}
			coro->markDone();
			Coro::setCurrent(nullptr);
			if (onDone) {
				onDone();
			}
		},
		// Привязываем slot отмены spawn'а к нашему cancellation_signal: emit на нём отменяет
		// текущую async-операцию корутины (её и переводим в CancelError/TimeoutError).
		asio::bind_cancellation_slot(coro->signal().slot(), asio::detached));
	});

	return coro;
}

}
