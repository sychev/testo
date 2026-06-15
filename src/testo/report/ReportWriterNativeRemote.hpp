
#pragma once

#include <asio.hpp>
#include <optional>
#include "ReportWriterNative.hpp"

struct ReportWriterNativeRemote: ReportWriterNative {
	ReportWriterNativeRemote(const ReportConfig& config);

	virtual asio::awaitable<void> launch_begin(const std::vector<std::shared_ptr<IR::Test>>& tests,
		const std::vector<std::shared_ptr<IR::TestRun>>& tests_runs) override;

	virtual asio::awaitable<void> test_skip_begin(const std::shared_ptr<IR::TestRun>& test_run) override;
	virtual asio::awaitable<void> test_skip_end(const std::shared_ptr<IR::TestRun>& test_run) override;

	virtual asio::awaitable<void> test_begin(const std::shared_ptr<IR::TestRun>& test_run) override;
	virtual asio::awaitable<void> report(const std::shared_ptr<IR::TestRun>& test_run, const std::string& text) override;
	virtual asio::awaitable<void> report_screenshot(const std::shared_ptr<IR::TestRun>& test_run, const stb::Image<stb::RGB>& screenshot, const std::string& tag) override;
	virtual asio::awaitable<void> test_end(const std::shared_ptr<IR::TestRun>& test_run) override;

	virtual asio::awaitable<void> launch_end() override;

private:
	using Endpoint = asio::ip::tcp::endpoint;

	std::optional<asio::ip::tcp::socket> socket;
	Endpoint endpoint;

	// Транспорт на нативных asio-корутинах (C++20).
	asio::awaitable<void> connect();
	asio::awaitable<void> send(nlohmann::json message);
	asio::awaitable<nlohmann::json> recv();
	asio::awaitable<void> wait_for_confirmation();
};
