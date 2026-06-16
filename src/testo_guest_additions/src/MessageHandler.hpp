
#pragma once

#include <mutex>
#include <map>
#include <set>

#include "Channel.hpp"
#include "SharedFolder.hpp"
#include <version_number/VersionNumber.hpp>
#include <coro/Runtime.h>

struct ExecuteContext {
	ExecuteContext();
	~ExecuteContext();

	void set_var(const std::string& var_name, const std::string& var_value, bool global);
	std::string get_var(const std::string& var_name);

	void unlock(const nlohmann::json& j);
	nlohmann::json lock();

private:
	std::mutex mutex;
	std::map<std::string, std::string> vars;
	std::set<std::string> global_vars;
};

struct MessageHandler {
	MessageHandler() = default;
	virtual ~MessageHandler() = default;

	MessageHandler(const MessageHandler&) = delete;
	MessageHandler& operator=(const MessageHandler&) = delete;

	asio::awaitable<void> run(std::shared_ptr<Channel> channel_);

	ExecuteContext exec_ctx;

protected:
	asio::awaitable<void> handle_message();
	virtual asio::awaitable<void> do_handle_message(const std::string& method_name);
	asio::awaitable<void> handle_check_avaliable();
	asio::awaitable<void> handle_get_tmp_dir();
	asio::awaitable<void> handle_copy_file();

	nlohmann::json copy_directory_out(const fs::path& dir, const fs::path& dst);
	nlohmann::json copy_single_file_out(const fs::path& dir, const fs::path& dst);

	asio::awaitable<void> handle_copy_files_out();
	asio::awaitable<void> handle_execute();
	asio::awaitable<int> do_handle_execute(std::string cmd);

	asio::awaitable<void> handle_mount();
	asio::awaitable<void> handle_get_shared_folder_status();
	asio::awaitable<void> handle_umount();

	asio::awaitable<void> send_error(const std::string& error);

	std::shared_ptr<Channel> channel;
	nlohmann::json command;

	VersionNumber ver;
};

struct HostMessageHandler: MessageHandler {

};

struct CLIMessageHandler: MessageHandler {
	CLIMessageHandler(HostMessageHandler* host_handler_): host_handler(host_handler_) {}

protected:
	virtual asio::awaitable<void> do_handle_message(const std::string& method_name) override;
	asio::awaitable<void> handle_set_var();
	asio::awaitable<void> handle_get_var();

	HostMessageHandler* host_handler = nullptr;
};
