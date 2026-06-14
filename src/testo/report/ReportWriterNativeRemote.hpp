
#pragma once

#include <asio.hpp>
#include <optional>
#include "ReportWriterNative.hpp"

struct ReportWriterNativeRemote: ReportWriterNative {
	ReportWriterNativeRemote(const ReportConfig& config);

	virtual void launch_begin(const std::vector<std::shared_ptr<IR::Test>>& tests,
		const std::vector<std::shared_ptr<IR::TestRun>>& tests_runs) override;

	virtual void test_skip_begin(const std::shared_ptr<IR::TestRun>& test_run) override;
	virtual void test_skip_end(const std::shared_ptr<IR::TestRun>& test_run) override;

	virtual void test_begin(const std::shared_ptr<IR::TestRun>& test_run) override;
	virtual void report(const std::shared_ptr<IR::TestRun>& test_run, const std::string& text) override;
	virtual void report_screenshot(const std::shared_ptr<IR::TestRun>& test_run, const stb::Image<stb::RGB>& screenshot, const std::string& tag) override;
	virtual void test_end(const std::shared_ptr<IR::TestRun>& test_run) override;

	virtual void launch_end() override;

private:
	using Endpoint = asio::ip::tcp::endpoint;

	std::optional<asio::ip::tcp::socket> socket;
	Endpoint endpoint;

	// Транспорт на нативных asio-корутинах (C++20).
	asio::awaitable<void> async_connect();
	asio::awaitable<void> async_send(nlohmann::json message);
	asio::awaitable<nlohmann::json> async_recv();

	// Синхронно выглядящие обёртки для вызова из coro-кода интерпретатора.
	// Используют временный мост coro::await (см. coro_asio_bridge.hpp).
	void connect();
	nlohmann::json recv();
	void send(const nlohmann::json& message);
	void wait_for_confirmation();
};
