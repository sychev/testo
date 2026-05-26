
#include "Stack.hpp"

std::string StackNode::find_param(const std::string& name) const {
	auto it = params.find(name);
	if (it != params.end()) {
		return it->second;
	}
	if (!parent) {
		throw std::runtime_error("param \"" + name + "\" is not defined");
	}
	return parent->find_param(name);
}

bool StackNode::is_defined(const std::string& var) const {
	auto it = params.find(var);
	if (it != params.end()) {
		return true;
	}
	if (!parent) {
		return false;
	}
	return parent->is_defined(var);
}

nlohmann::json stack_to_json(const std::shared_ptr<StackNode>& stack) {
	nlohmann::json frames = nlohmann::json::array();
	for (auto node = stack; node; node = node->parent) {
		frames.push_back(nlohmann::json{{"params", node->params}});
	}
	return frames;
}

std::shared_ptr<StackNode> stack_from_json(const nlohmann::json& frames) {
	if (!frames.is_array()) {
		throw std::runtime_error("stack_from_json: expected JSON array");
	}
	std::shared_ptr<StackNode> result;
	for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
		auto node = std::make_shared<StackNode>();
		node->parent = result;
		node->params = it->at("params").get<std::map<std::string, std::string>>();
		result = node;
	}
	return result;
}

bool stacks_equal(const std::shared_ptr<StackNode>& a, const std::shared_ptr<StackNode>& b) {
	auto pa = a;
	auto pb = b;
	while (pa && pb) {
		if (pa->params != pb->params) {
			return false;
		}
		pa = pa->parent;
		pb = pb->parent;
	}
	return !pa && !pb;
}
