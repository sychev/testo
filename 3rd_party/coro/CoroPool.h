
#pragma once

#include "coro/Coro.h"
#include <asio/experimental/channel.hpp>
#include <vector>
#include <exception>

namespace coro {

/*!
	@brief Иерархическое управление дочерними корутинами (структурная конкурентность)

	Дочерние корутины запускаются каждая на СВОЁМ strand — то есть выполняются параллельно по
	потокам общего io_context. При этом всё состояние самого пула (счётчик, накопленное
	исключение) изменяется только на strand родителя: exec/waitAll/cancelAll выполняются в
	родительской корутине, а уведомления о завершении детей постятся на strand родителя. Поэтому
	пул не требует блокировок и потокобезопасен по построению.
*/
class CoroPool {
public:
	CoroPool();
	~CoroPool();

	CoroPool(const CoroPool& other) = delete;
	CoroPool& operator=(const CoroPool& other) = delete;
	CoroPool(CoroPool&& other) = delete;
	CoroPool& operator=(CoroPool&& other) = delete;

	/// Запустить дочернюю корутину (на отдельном strand)
	Coro* exec(std::function<void()> routine);
	/// Дождаться завершения всех дочерних корутин
	void waitAll(bool noThrow = false);
	/// Отменить все дочерние корутины
	void cancelAll();

private:
	void onChildDone(std::shared_ptr<std::exception_ptr> exception);

	strand_t _strand;                                   // strand родительской корутины
	std::vector<std::shared_ptr<Coro>> _children;       // удерживают детей живыми
	int _running = 0;
	bool _waiting = false;
	std::exception_ptr _firstException;
	asio::experimental::channel<void(std::error_code)> _allDone;   // сигнал "все дети завершились"
};

}
