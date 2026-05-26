
#pragma once

#include "../IR/Action.hpp"
#include "../IR/Expr.hpp"
#include "../IR/Controller.hpp"
#include "../IR/Macro.hpp"
#include "../IR/Expr.hpp"
#include "../IR/Resume.hpp"
#include "../IR/Test.hpp"
#include "../report/Reporter.hpp"

struct ActionException: ExceptionWithPos {
	ActionException(const std::shared_ptr<AST::Node>& node, const std::shared_ptr<IR::Controller>& controller):
		ExceptionWithPos(node->begin(), {})
	{
		msg = "Error while performing action " + node->to_string();
		if (controller) {
			msg += " on " + controller->type() + " " + controller->name();
		}
	}
};

struct AbortException: ExceptionWithPos {
	AbortException(const std::shared_ptr<AST::Abort>& node, const std::shared_ptr<IR::Controller>& controller, const std::string& message):
		ExceptionWithPos(node->begin(), {})
	{
		msg = "Caught abort action ";
		if (controller) {
			msg += "on " + controller->type() + " " +  controller->name();
		}
		msg += " with message: ";
		msg += message;
	}
};

struct CycleControlException: std::exception {
	CycleControlException(Token token_): token(std::move(token_))
	{
	}

	Token token;
};

// Shared between all action visitors created for a single test execution.
// When `active` is true, leaf actions are skipped until the visitor reaches
// the AST position recorded in `pos` with a matching stack chain; at that point
// `active` is cleared and normal execution resumes after the recorded action.
struct ResumeContext {
	IR::ResumePos pos;
	std::shared_ptr<StackNode> saved_stack;
	bool active = true;
};

struct VisitorInterpreterAction {
	VisitorInterpreterAction(
		std::shared_ptr<IR::Controller> controller,
		std::shared_ptr<StackNode> stack,
		Reporter& reporter,
		std::shared_ptr<IR::Test> current_test,
		bool ignore_repl
	):
		current_controller(controller), stack(stack), reporter(reporter),
		current_test(current_test), ignore_repl(ignore_repl) {}

	virtual ~VisitorInterpreterAction() {}

	virtual void visit_action(std::shared_ptr<AST::Action> action) = 0;
	virtual void visit_copy(const IR::Copy& copy) = 0;
	virtual bool visit_check(const IR::Check& check) = 0;

	void visit_action_block(std::shared_ptr<AST::Block<AST::Action>> action_block);
	void visit_print(const IR::Print& print);
	void visit_repl(const IR::REPL& repl);
	void visit_abort(const IR::Abort& abort);
	void visit_bug(const IR::Bug& abort);
	void visit_sleep(const IR::Sleep& sleep);
	void visit_macro_call(const IR::MacroCall& macro_call);
	void visit_macro_body(const std::shared_ptr<AST::Block<AST::Action>>& macro_body);
	void visit_if_clause(std::shared_ptr<AST::IfClause> if_clause);
	void visit_for_clause(std::shared_ptr<AST::ForClause> for_clause);
	void visit_snapshot_create(const IR::SnapshotCreate& snapshot_create);
	void visit_snapshot_revert(const IR::SnapshotRevert& snapshot_revert);

	bool visit_expr(std::shared_ptr<AST::Expr> expr);
	bool visit_binop(std::shared_ptr<AST::BinOp> binop);
	bool visit_string_expr(const IR::String& string_expr);
	bool visit_comparison(const IR::Comparison& comparison);
	bool visit_defined(const IR::Defined& defined);

	// Returns true if the given leaf action should be skipped because the
	// visitor is currently fast-forwarding to a recorded resume point. When
	// the action's position and current stack match the recorded point, the
	// resume context is cleared and `true` is still returned (the recorded
	// `snapshot create` is not re-executed).
	bool should_skip_leaf(const std::shared_ptr<AST::Action>& action);

	std::shared_ptr<IR::Controller> current_controller;
	std::shared_ptr<StackNode> stack;
	Reporter& reporter;
	std::shared_ptr<IR::Test> current_test;
	bool ignore_repl;
	std::shared_ptr<ResumeContext> resume_context;
};