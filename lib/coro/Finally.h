
#pragma once

#include <functional>
#include <list>

namespace coro {

/*!
	@brief Выполняет действия при выходе из области видимости.

	Чистая утилита (scope guard), не зависит от способа реализации корутин.
*/
class Finally {
public:
	Finally() {};
	Finally(std::function<void()> fn)
	{
		_fnList.push_back(fn);
	}
	~Finally()
	{
		try
		{
			while (!_fnList.empty())
			{
				_fnList.back()();
				_fnList.pop_back();
			}
		}
		catch (...) {}
	}

	Finally(const Finally& other) = delete;
	Finally& operator=(const Finally& other) = delete;

	Finally(Finally&& other): _fnList(std::move(other._fnList)) {}
	Finally& operator=(Finally&& other) {
		_fnList = std::move(other._fnList);
		return *this;
	}

	void operator<<(std::function<void(void)> fn)
	{
		_fnList.push_back(std::move(fn));
	}

	void discard()
	{
		_fnList.clear();
	}

private:
	std::list<std::function<void(void)>> _fnList;
};

}
