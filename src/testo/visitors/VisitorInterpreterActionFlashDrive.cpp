
#include <coro/CheckPoint.h>
#include <coro/Timeout.h>
#include "VisitorInterpreterActionFlashDrive.hpp"
#include "../Exceptions.hpp"
#include "../Logger.hpp"
#include <fmt/format.h>

asio::awaitable<void> VisitorInterpreterActionFlashDrive::visit_action(std::shared_ptr<AST::Action> action) {
	if (auto p = std::dynamic_pointer_cast<AST::Abort>(action)) {
		visit_abort({p, stack});
	} else if (auto p = std::dynamic_pointer_cast<AST::Bug>(action)) {
		visit_bug({p, stack});
	} else if (auto p = std::dynamic_pointer_cast<AST::Print>(action)) {
		visit_print({p, stack});
	} else if (auto p = std::dynamic_pointer_cast<AST::REPL>(action)) {
		co_await visit_repl({p, stack});
	} else if (auto p = std::dynamic_pointer_cast<AST::Sleep>(action)) {
		co_await visit_sleep({p, stack});
	} else if (auto p = std::dynamic_pointer_cast<AST::Copy>(action)) {
		co_await visit_copy({p, stack});
	} else if (auto p = std::dynamic_pointer_cast<AST::Block<AST::Action>>(action)) {
		co_await visit_action_block(p);
	} else if (auto p = std::dynamic_pointer_cast<AST::ActionWithDelim>(action)) {
		co_await visit_action(p->action);
	} else if (auto p = std::dynamic_pointer_cast<AST::Empty>(action)) {
		;
	} else if (auto p = std::dynamic_pointer_cast<AST::MacroCall<AST::Action>>(action)) {
		co_await visit_macro_call({p, stack});
	} else if (auto p = std::dynamic_pointer_cast<AST::IfClause>(action)) {
		co_await visit_if_clause(p);
	} else if (auto p = std::dynamic_pointer_cast<AST::ForClause>(action)) {
		co_await visit_for_clause(p);
	} else if (auto p = std::dynamic_pointer_cast<AST::CycleControl>(action)) {
		throw CycleControlException(p->token);
	}  else {
		throw std::runtime_error("Should never happen");
	}

	co_await coro::CheckPoint();
}

asio::awaitable<void> VisitorInterpreterActionFlashDrive::visit_copy(const IR::Copy& copy) {
	TRACE();
	try {
		reporter.copy(current_controller, copy);

		co_await coro::with_timeout(copy.timeout().value(), [&]() -> asio::awaitable<void> {
			for (auto vmc: current_test->get_all_machines()) {
				if (vmc->vm()->is_flash_plugged(fdc->fd())) {
					throw std::runtime_error(fmt::format("Flash drive {} is already plugged into vm {}. You should unplug it first", fdc->name(), vmc->name()));
				}
			}

			//TODO: timeouts
			if(copy.ast_node->is_to_guest()) {
				//Additional check since now we can't be sure the "from" actually exists
				if (!fs::exists(copy.from())) {
					throw std::runtime_error("Specified path doesn't exist: " + copy.from());
				}
				fdc->fd()->upload(copy.from(), copy.to());
			} else {
				fdc->fd()->download(copy.from(), copy.to());
			}
			co_return;
		});

	} catch (const std::exception& error) {
		std::throw_with_nested(ActionException(copy.ast_node, current_controller));
	}
}

asio::awaitable<bool> VisitorInterpreterActionFlashDrive::visit_check(const IR::Check& check) {
	throw std::runtime_error("Shouldn't get here");
	co_return false;
}
