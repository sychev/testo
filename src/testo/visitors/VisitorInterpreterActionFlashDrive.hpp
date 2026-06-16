
#pragma once

#include "VisitorInterpreterAction.hpp"

struct VisitorInterpreterActionFlashDrive: public VisitorInterpreterAction {
	VisitorInterpreterActionFlashDrive(
		std::shared_ptr<IR::FlashDrive> fdc,
		std::shared_ptr<StackNode> stack,
		Reporter& reporter,
		std::shared_ptr<IR::Test> current_test,
		bool ignore_repl
	):
		VisitorInterpreterAction(fdc, stack, reporter, ignore_repl), fdc(fdc), current_test(current_test) {}

	~VisitorInterpreterActionFlashDrive() {}

	asio::awaitable<void> visit_action(std::shared_ptr<AST::Action> action) override;
	asio::awaitable<void> visit_copy(const IR::Copy& copy) override;
	asio::awaitable<bool> visit_check(const IR::Check& check) override;

	std::shared_ptr<IR::FlashDrive> fdc;
	std::shared_ptr<IR::Test> current_test;
};