
#pragma once

#include <vector>
#include <string>
#include <asio/awaitable.hpp>
#include "../Configs.hpp"

struct RunModeArgs: ProgramConfig {
	void validate() const;
};

asio::awaitable<int> run_mode(const RunModeArgs& args);