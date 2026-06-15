
#include "ReportWriterNativeRemote.hpp"

asio::ip::tcp::endpoint parse_tcp_endpoint(const std::string& endpoint);

ReportWriterNativeRemote::ReportWriterNativeRemote(const ReportConfig& config): ReportWriterNative(config) {
	endpoint = parse_tcp_endpoint(config.report_folder);
}

asio::awaitable<void> ReportWriterNativeRemote::launch_begin(const std::vector<std::shared_ptr<IR::Test>>& tests,
	const std::vector<std::shared_ptr<IR::TestRun>>& tests_runs)
{
	co_await ReportWriterNative::launch_begin(tests, tests_runs);

	nlohmann::json tests_meta = nlohmann::json::array();
	for (auto& test: tests) {
		tests_meta.push_back(to_json(test));
	}

	co_await connect();
	nlohmann::json msg = {
		{"type", "launch_begin"},
		{"current_launch", current_launch_meta},
		{"tests", tests_meta},
	};
	co_await send(std::move(msg));
	co_await wait_for_confirmation();
}

asio::awaitable<void> ReportWriterNativeRemote::test_skip_begin(const std::shared_ptr<IR::TestRun>& test_run) {
	co_await ReportWriterNative::test_skip_begin(test_run);
	nlohmann::json msg = {
		{"type", "test_skip_begin"},
		{"current_test_run", to_json(test_run)},
	};
	co_await send(std::move(msg));
	co_await wait_for_confirmation();
}

asio::awaitable<void> ReportWriterNativeRemote::test_skip_end(const std::shared_ptr<IR::TestRun>& test_run) {
	co_await ReportWriterNative::test_skip_end(test_run);
	nlohmann::json msg = {
		{"type", "test_skip_end"},
		{"current_test_run", to_json(test_run)},
	};
	co_await send(std::move(msg));
	co_await wait_for_confirmation();
}

asio::awaitable<void> ReportWriterNativeRemote::test_begin(const std::shared_ptr<IR::TestRun>& test_run) {
	co_await ReportWriterNative::test_begin(test_run);
	nlohmann::json msg = {
		{"type", "test_begin"},
		{"current_test_run", to_json(test_run)},
	};
	co_await send(std::move(msg));
	co_await wait_for_confirmation();
}

asio::awaitable<void> ReportWriterNativeRemote::report(const std::shared_ptr<IR::TestRun>& test_run, const std::string& text) {
	std::vector<uint8_t> binary_text(text.begin(), text.end());
	nlohmann::json msg = {
		{"type", "report"},
		{"text", binary_text},
	};
	if (test_run) {
		msg["current_test_run"] = to_json(test_run);
	}
	co_await send(msg);
	co_await wait_for_confirmation();
}

asio::awaitable<void> ReportWriterNativeRemote::report_screenshot(const std::shared_ptr<IR::TestRun>& test_run, const stb::Image<stb::RGB>& screenshot, const std::string& tag) {
	nlohmann::json msg = {
		{"type", "report_screenshot"},
		{"screenshot", screenshot.write_png_mem()},
		{"tag", tag},
	};
	if (test_run) {
		msg["current_test_run"] = to_json(test_run);
	}
	co_await send(msg);
	co_await wait_for_confirmation();
}

asio::awaitable<void> ReportWriterNativeRemote::test_end(const std::shared_ptr<IR::TestRun>& test_run) {
	co_await ReportWriterNative::test_end(test_run);
	nlohmann::json msg = {
		{"type", "test_end"},
		{"current_test_run", to_json(test_run)},
	};
	co_await send(std::move(msg));
	co_await wait_for_confirmation();
}

asio::awaitable<void> ReportWriterNativeRemote::launch_end() {
	co_await ReportWriterNative::launch_end();
	nlohmann::json msg = {
		{"type", "launch_end"},
		{"current_launch", current_launch_meta},
	};
	co_await send(std::move(msg));
	co_await wait_for_confirmation();
}

asio::awaitable<void> ReportWriterNativeRemote::connect() {
	socket.emplace(co_await asio::this_coro::executor);
	co_await socket->async_connect(endpoint, asio::use_awaitable);
}

asio::awaitable<nlohmann::json> ReportWriterNativeRemote::recv() {
	uint32_t msg_size = 0;
	co_await asio::async_read(*socket, asio::buffer(&msg_size, 4), asio::use_awaitable);

	std::vector<uint8_t> json_data;
	json_data.resize(msg_size);
	co_await asio::async_read(*socket, asio::buffer(json_data), asio::use_awaitable);

	co_return nlohmann::json::from_cbor(json_data);
}

asio::awaitable<void> ReportWriterNativeRemote::send(nlohmann::json json) {
	std::vector<uint8_t> json_data = nlohmann::json::to_cbor(json);
	uint32_t json_size = (uint32_t)json_data.size();
	co_await asio::async_write(*socket, asio::buffer(&json_size, sizeof(json_size)), asio::use_awaitable);
	co_await asio::async_write(*socket, asio::buffer(json_data), asio::use_awaitable);
}

asio::awaitable<void> ReportWriterNativeRemote::wait_for_confirmation() {
	nlohmann::json response = co_await recv();
	if (response.at("type") != "confirmation") {
		throw std::runtime_error("Got unexpected response from the tcp report server");
	}
}
