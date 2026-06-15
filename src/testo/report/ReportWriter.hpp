
#pragma once

#include <asio.hpp>
#include <nlohmann/json.hpp>
#include "../IR/Test.hpp"
#include "../Configs.hpp"
#include <fstream>

struct ReportWriter {
	ReportWriter(const ReportConfig& config) {}
	virtual ~ReportWriter() {}

	virtual asio::awaitable<void> launch_begin(const std::vector<std::shared_ptr<IR::Test>>& tests,
		const std::vector<std::shared_ptr<IR::TestRun>>& tests_runs) { co_return; }

	virtual asio::awaitable<void> test_skip_begin(const std::shared_ptr<IR::TestRun>& test_run) { co_return; }
	virtual asio::awaitable<void> test_skip_end(const std::shared_ptr<IR::TestRun>& test_run) { co_return; }

	virtual asio::awaitable<void> test_begin(const std::shared_ptr<IR::TestRun>& test_run) { co_return; }
	virtual asio::awaitable<void> report_prefix(const std::shared_ptr<IR::TestRun>& test_run) { co_return; }
	virtual asio::awaitable<void> report(const std::shared_ptr<IR::TestRun>& test_run, const std::string& text) { co_return; }
	virtual asio::awaitable<void> report_raw(const std::shared_ptr<IR::TestRun>& test_run, const std::string& text) { co_return; }
	virtual asio::awaitable<void> report_screenshot(const std::shared_ptr<IR::TestRun>& test_run, const stb::Image<stb::RGB>& screenshot, const std::string& tag) { co_return; }
	virtual asio::awaitable<void> test_end(const std::shared_ptr<IR::TestRun>& test_run) { co_return; }

	virtual asio::awaitable<void> launch_end() { co_return; }
};
