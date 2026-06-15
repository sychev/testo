
#pragma once

#include "ReportWriter.hpp"

struct ReportWriterNative: ReportWriter {
	ReportWriterNative(const ReportConfig& config);

	virtual asio::awaitable<void> launch_begin(const std::vector<std::shared_ptr<IR::Test>>& tests,
		const std::vector<std::shared_ptr<IR::TestRun>>& tests_runs) override;

	virtual asio::awaitable<void> test_skip_begin(const std::shared_ptr<IR::TestRun>& test_run) override;
	virtual asio::awaitable<void> test_skip_end(const std::shared_ptr<IR::TestRun>& test_run) override;

	virtual asio::awaitable<void> test_begin(const std::shared_ptr<IR::TestRun>& test_run) override;
	virtual asio::awaitable<void> report_prefix(const std::shared_ptr<IR::TestRun>& test_run) override;
	virtual asio::awaitable<void> report_raw(const std::shared_ptr<IR::TestRun>& test_run, const std::string& text) override;
	virtual asio::awaitable<void> test_end(const std::shared_ptr<IR::TestRun>& test_run) override;

	virtual asio::awaitable<void> launch_end() override;

protected:
	static nlohmann::json to_json(const std::shared_ptr<IR::Test>& test);
	static nlohmann::json to_json(const std::shared_ptr<IR::TestRun>& test_run);

	nlohmann::json current_launch_meta;
};
