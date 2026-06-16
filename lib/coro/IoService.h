
#pragma once

#include "coro/Runtime.h"
#include <asio.hpp>
#include <optional>
#include <stdexcept>
#include <utility>

namespace coro {

/*!
	@brief Совместимая обёртка над работающим io_context.

	Оставлена для исходников, которые обращались к IoService::current()->_impl
	(например, для создания нативных asio-объектов). Новый код предпочитает
	coro::current_executor() и co_await asio::this_coro::executor.
*/
struct IoService {
	asio::io_context& _impl;

	static IoService* current() {
		auto* ctx = detail::tls_io_context();
		if (!ctx) {
			throw std::runtime_error("coro::IoService::current(): no Application is running on this thread");
		}
		static thread_local std::optional<IoService> inst;
		inst.emplace(IoService{*ctx});
		return &*inst;
	}

	template <typename F>
	void post(F&& f) {
		asio::post(_impl, std::forward<F>(f));
	}

	template <typename F>
	void dispatch(F&& f) {
		asio::dispatch(_impl, std::forward<F>(f));
	}
};

}
