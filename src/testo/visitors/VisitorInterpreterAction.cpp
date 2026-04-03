
#include <coro/Timer.h>
#include "VisitorInterpreterAction.hpp"
#include "../Exceptions.hpp"
#include "../IR/Program.hpp"
#include <coro/Finally.h>
#include "../Logger.hpp"
#include <termios.h>
#include <unistd.h>

extern std::atomic<bool> REPL_mode_is_active;

// Read a line from stdin with history navigation (arrow up/down) support.
// Returns false on EOF/Ctrl-D or when REPL_mode_is_active becomes false (Ctrl-C).
static bool readline_with_history(std::string& result, std::vector<std::string>& history) {
	result.clear();

	if (!isatty(STDIN_FILENO)) {
		// Fallback for non-interactive input
		if (!std::getline(std::cin, result)) {
			return false;
		}
		return true;
	}

	struct termios orig_termios, raw_termios;
	tcgetattr(STDIN_FILENO, &orig_termios);
	raw_termios = orig_termios;
	raw_termios.c_lflag &= ~(ICANON | ECHO);
	raw_termios.c_cc[VMIN] = 1;
	raw_termios.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_termios);

	auto restore_term = [&orig_termios]() {
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
	};

	std::string line;
	// history_index == history.size() means "current new line"
	size_t history_index = history.size();
	std::string saved_line; // saves current input when browsing history
	size_t cursor_pos = 0;

	while (true) {
		if (!REPL_mode_is_active) {
			restore_term();
			return false;
		}

		char c;
		ssize_t n = read(STDIN_FILENO, &c, 1);
		if (n <= 0) {
			restore_term();
			return false;
		}

		if (c == '\n' || c == '\r') {
			write(STDOUT_FILENO, "\n", 1);
			result = line;
			restore_term();
			return true;
		} else if (c == 4) { // Ctrl-D
			restore_term();
			return false;
		} else if (c == 3) { // Ctrl-C
			restore_term();
			REPL_mode_is_active = false;
			return false;
		} else if (c == 127 || c == 8) { // Backspace
			if (cursor_pos > 0) {
				line.erase(cursor_pos - 1, 1);
				cursor_pos--;
				// Move cursor back, rewrite from cursor, clear tail
				write(STDOUT_FILENO, "\b", 1);
				const std::string tail = line.substr(cursor_pos) + " ";
				write(STDOUT_FILENO, tail.c_str(), tail.size());
				// Move cursor back to correct position
				std::string move_back(tail.size(), '\b');
				write(STDOUT_FILENO, move_back.c_str(), move_back.size());
			}
		} else if (c == '\x1b') { // Escape sequence
			char seq[2];
			if (read(STDIN_FILENO, &seq[0], 1) != 1) continue;
			if (read(STDIN_FILENO, &seq[1], 1) != 1) continue;

			if (seq[0] == '[') {
				auto replace_line = [&](const std::string& new_line) {
					// Move cursor to start of input
					if (cursor_pos > 0) {
						std::string move_left(cursor_pos, '\b');
						write(STDOUT_FILENO, move_left.c_str(), move_left.size());
					}
					// Clear old line
					std::string clear(line.size(), ' ');
					write(STDOUT_FILENO, clear.c_str(), clear.size());
					std::string move_back(line.size(), '\b');
					write(STDOUT_FILENO, move_back.c_str(), move_back.size());
					// Write new line
					line = new_line;
					cursor_pos = line.size();
					write(STDOUT_FILENO, line.c_str(), line.size());
				};

				if (seq[1] == 'A') { // Arrow Up
					if (history_index > 0) {
						if (history_index == history.size()) {
							saved_line = line;
						}
						history_index--;
						replace_line(history[history_index]);
					}
				} else if (seq[1] == 'B') { // Arrow Down
					if (history_index < history.size()) {
						history_index++;
						if (history_index == history.size()) {
							replace_line(saved_line);
						} else {
							replace_line(history[history_index]);
						}
					}
				} else if (seq[1] == 'C') { // Arrow Right
					if (cursor_pos < line.size()) {
						cursor_pos++;
						write(STDOUT_FILENO, "\x1b[C", 3);
					}
				} else if (seq[1] == 'D') { // Arrow Left
					if (cursor_pos > 0) {
						cursor_pos--;
						write(STDOUT_FILENO, "\x1b[D", 3);
					}
				}
			}
		} else if (c == 1) { // Ctrl-A: move to start
			if (cursor_pos > 0) {
				std::string move_left(cursor_pos, '\b');
				write(STDOUT_FILENO, move_left.c_str(), move_left.size());
				cursor_pos = 0;
			}
		} else if (c == 5) { // Ctrl-E: move to end
			if (cursor_pos < line.size()) {
				std::string tail = line.substr(cursor_pos);
				write(STDOUT_FILENO, tail.c_str(), tail.size());
				cursor_pos = line.size();
			}
		} else if (c >= 32) { // Printable characters
			line.insert(cursor_pos, 1, c);
			cursor_pos++;
			// Write from cursor position to end, then move back
			const std::string tail = line.substr(cursor_pos - 1);
			write(STDOUT_FILENO, tail.c_str(), tail.size());
			if (cursor_pos < line.size()) {
				size_t chars_after = line.size() - cursor_pos;
				std::string move_back(chars_after, '\b');
				write(STDOUT_FILENO, move_back.c_str(), move_back.size());
			}
		}
	}
}

void VisitorInterpreterAction::visit_action_block(std::shared_ptr<AST::Block<AST::Action>> action_block) {
	for (auto action: action_block->items) {
		visit_action(action);
	}
}

void VisitorInterpreterAction::visit_print(const IR::Print& print) {
	TRACE();
	try {
		reporter.print(current_controller, print);
	} catch (const std::exception& error) {
		std::throw_with_nested(ActionException(print.ast_node, current_controller));
	}
}

// trim from start (in place)
static inline void ltrim(std::string &s) {
	s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
		return !std::isspace(ch);
	}));
}

// trim from end (in place)
static inline void rtrim(std::string &s) {
	s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
		return !std::isspace(ch);
	}).base(), s.end());
}

// trim from both ends (in place)
static inline void trim(std::string &s) {
	ltrim(s);
	rtrim(s);
}


void VisitorInterpreterAction::visit_repl(const IR::REPL& repl) {
	TRACE();
	if (ignore_repl) {
		return;
	}
	try {
		reporter.repl_begin(current_controller, repl);
		REPL_mode_is_active = true;
		std::cout << "Now you can type commands line-by-line. Use Ctrl-C to exit REPL mode." << std::endl;
		std::string all_lines;
		std::vector<std::string> history;
		while (true) {
			std::cout << "> " << std::flush;
			std::string line;
			if (!readline_with_history(line, history)) {
				break;
			}
			trim(line);
			if (!line.size()) {
				continue;
			}
			history.push_back(line);
			line += "\n";
			try {
				std::shared_ptr<AST::Action> ast_action = Parser(".", line, false).action();
				visit_action(ast_action);
				all_lines += line;
			}
			catch (const AbortException&) {
				throw;
			}
			catch (const std::exception& error) {
				std::stringstream ss;
				ss << error << std::endl;
				reporter.error(ss.str());
			}
		}
		if (all_lines.size()) {
			std::cout << "You have entered the following commands:" << std::endl;
			std::cout << all_lines;
		}
		reporter.repl_end(current_controller, repl);
	} catch (const std::exception& error) {
		std::throw_with_nested(ActionException(repl.ast_node, current_controller));
	}
}

void VisitorInterpreterAction::visit_abort(const IR::Abort& abort) {
	TRACE();
	reporter.abort(current_controller, abort);
	throw AbortException(abort.ast_node, current_controller, abort.message());
}

void VisitorInterpreterAction::visit_bug(const IR::Bug& bug) {
	TRACE();
	reporter.bug(current_controller, bug);
}

void VisitorInterpreterAction::visit_sleep(const IR::Sleep& sleep) {
	TRACE();
	reporter.sleep(current_controller, sleep);
	coro::Timer timer;
	timer.waitFor(sleep.timeout().value());
}

void VisitorInterpreterAction::visit_macro_call(const IR::MacroCall& macro_call) {
	TRACE();
	reporter.macro_action_call(current_controller, macro_call);
	macro_call.visit_interpreter<AST::Action>(this);
}

void VisitorInterpreterAction::visit_macro_body(const std::shared_ptr<AST::Block<AST::Action>>& macro_body) {
	TRACE();
	visit_action_block(macro_body);
}

void VisitorInterpreterAction::visit_if_clause(std::shared_ptr<AST::IfClause> if_clause) {
	TRACE();
	bool expr_result;
	try {
		expr_result = visit_expr(if_clause->expr);
	} catch (const std::exception& error) {
		std::throw_with_nested(ActionException(if_clause, current_controller));
	}
	//everything else should be caught at test level
	if (expr_result) {
		return visit_action(if_clause->if_action);
	} else if (if_clause->has_else()) {
		return visit_action(if_clause->else_action);
	}
}

void VisitorInterpreterAction::visit_for_clause(std::shared_ptr<AST::ForClause> for_clause) {
	TRACE();

	uint32_t i = 0;

	std::vector<std::string> values;

	if (auto p = std::dynamic_pointer_cast<AST::Range>(for_clause->counter_list)) {
		values = IR::Range({p, stack}).values();
	} else {
		throw std::runtime_error("Unknown counter list type");
	}

	std::map<std::string, std::string> params;
	for (i = 0; i < values.size(); ++i) {
		params[for_clause->counter.value()] = values[i];

		try {
			auto new_stack = std::make_shared<StackNode>();
			new_stack->parent = stack;
			new_stack->params = params;
			StackPusher<VisitorInterpreterAction> new_ctx(this, new_stack);
				visit_action(for_clause->cycle_body);

		} catch (const CycleControlException& cycle_control) {
			if (cycle_control.token.type() == Token::category::break_) {
				break;
			} else if (cycle_control.token.type() == Token::category::continue_) {
				continue;
			} else {
				throw std::runtime_error("Unknown cycle control command: " + cycle_control.token.value());
			}
		}
	}

	if ((i == values.size()) && for_clause->else_token) {
		visit_action(for_clause->else_action);
	}
}

bool VisitorInterpreterAction::visit_expr(std::shared_ptr<AST::Expr> expr) {
	if (auto p = std::dynamic_pointer_cast<AST::BinOp>(expr)) {
		return visit_binop(p);
	} else if (auto p = std::dynamic_pointer_cast<AST::StringExpr>(expr)) {
		std::shared_ptr<IR::Machine> vmc = std::dynamic_pointer_cast<IR::Machine>(current_controller);
		return visit_string_expr({ p->str, stack, vmc ? vmc->get_vars() : nullptr });
	} else if (auto p = std::dynamic_pointer_cast<AST::Negation>(expr)) {
		return !visit_expr(p->expr);
	} else if (auto p = std::dynamic_pointer_cast<AST::Comparison>(expr)) {
		std::shared_ptr<IR::Machine> vmc = std::dynamic_pointer_cast<IR::Machine>(current_controller);
		return visit_comparison({ p, stack, vmc ? vmc->get_vars() : nullptr });
	} else if (auto p = std::dynamic_pointer_cast<AST::Defined>(expr)) {
		return visit_defined({ p, stack });
	} else if (auto p = std::dynamic_pointer_cast<AST::Check>(expr)) {
		std::shared_ptr<IR::Machine> vmc = std::dynamic_pointer_cast<IR::Machine>(current_controller);
		if (!vmc) {
			throw std::runtime_error("\"check\" expression is only available for VMs");
		}
		return visit_check({ p, stack, vmc->get_vars() });
	} else if (auto p = std::dynamic_pointer_cast<AST::ParentedExpr>(expr)) {
		return visit_expr(p->expr);
	} else {
		throw std::runtime_error("Unknown expr type");
	}
}

bool VisitorInterpreterAction::visit_binop(std::shared_ptr<AST::BinOp> binop) {
	auto left = visit_expr(binop->left);

	if (binop->op.value() == "AND") {
		if (!left) {
			return left;
		} else {
			return visit_expr(binop->right);
		}
	} else if (binop->op.value() == "OR") {
		if (left) {
			return left;
		} else {
			return visit_expr(binop->right);
		}
	} else {
		throw std::runtime_error("Unknown binop operation");
	}
}

bool VisitorInterpreterAction::visit_string_expr(const IR::String& string_expr) {
	return string_expr.text().length();
}

bool VisitorInterpreterAction::visit_comparison(const IR::Comparison& comparison) {
	return comparison.calculate();
}

bool VisitorInterpreterAction::visit_defined(const IR::Defined& defined) {
	return defined.is_defined();
}
