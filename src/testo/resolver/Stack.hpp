
#pragma once

#include <memory>
#include <map>
#include <string>
#include <stdexcept>
#include <nlohmann/json.hpp>

struct StackNode {
	StackNode() = default;

	StackNode(const StackNode& other) = delete;
	StackNode& operator=(const StackNode& other) = delete;
	StackNode(StackNode&& other) = delete;
	StackNode& operator=(StackNode&& other) = delete;

	std::string find_param(const std::string& name) const;
	bool is_defined(const std::string& var) const;

	std::shared_ptr<StackNode> parent;
	std::map<std::string, std::string> params;
};

// Serializes a chain of StackNode frames as a JSON array, innermost frame first.
nlohmann::json stack_to_json(const std::shared_ptr<StackNode>& stack);

// Reconstructs a chain of StackNode frames from JSON produced by stack_to_json.
std::shared_ptr<StackNode> stack_from_json(const nlohmann::json& frames);

// Structural equality of two stack chains: same depth, same params at each level.
bool stacks_equal(const std::shared_ptr<StackNode>& a, const std::shared_ptr<StackNode>& b);

template <typename StackHolder>
struct StackPusher {
	StackPusher(StackHolder* stack_holder_, std::shared_ptr<StackNode> new_stack): stack_holder(stack_holder_) {
		backup = std::move(stack_holder->stack);
		stack_holder->stack = std::move(new_stack);
	}
	~StackPusher() {
		stack_holder->stack = std::move(backup);
	}

	StackPusher(const StackPusher& other) = delete;
	StackPusher& operator=(const StackPusher& other) = delete;
	StackPusher(StackPusher&& other) = delete;
	StackPusher& operator=(StackPusher&& other) = delete;

	StackHolder* stack_holder = nullptr;
	std::shared_ptr<StackNode> backup;
};
