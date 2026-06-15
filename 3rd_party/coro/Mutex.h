
#pragma once

#include "coro/Coro.h"
#include <mutex>
#include <list>

namespace coro {

/*!
	@brief Обеспечивает монопольный доступ корутины к ресурсу.

	Используйте вместе с std::lock_guard

	@warning НЕ потокобезопасен!!!
*/
class Mutex {
public:
	/*!
		@brief Захват мьютекса

		Если мьютекс уже захвачен, то происходит выход из корутины до тех пор пока мьютекс
		не освободится. Или до тех пор, пока корутина не будет отменена.
	*/
	void lock();
	/// Освобождение мьютекса
	void unlock();

private:
	Coro* _owner = nullptr;
	std::list<Coro*> _coros;
};

}
