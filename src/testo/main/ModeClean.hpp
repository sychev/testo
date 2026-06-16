
#pragma once

#include <string>
#include <asio/awaitable.hpp>

struct CleanModeArgs {
	std::string prefix;
	bool assume_yes = false;
};

asio::awaitable<int> clean_mode(const CleanModeArgs& args);
