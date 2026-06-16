
#pragma once

#include <nlohmann/json.hpp>

#include <ghc/filesystem.hpp>
namespace fs = ghc::filesystem;

#include <version_number/VersionNumber.hpp>
#include <coro/Runtime.h>

#include <chrono>
#include <functional>
#include <map>
#include <string>

struct GuestAdditions {
	virtual ~GuestAdditions() = default;

	asio::awaitable<bool> is_avaliable(std::chrono::milliseconds timeout = std::chrono::seconds(3));
	asio::awaitable<void> copy_to_guest(const fs::path& src, const fs::path& dst);
	asio::awaitable<void> copy_from_guest(const fs::path& src, const fs::path& dst);
	asio::awaitable<void> remove_from_guest(const fs::path& path);
	asio::awaitable<nlohmann::json> execute(const std::string& command, const std::map<std::string, std::string>& vars,
		const std::function<void(const std::string&)>& callback);
	asio::awaitable<std::string> get_tmp_dir();
	asio::awaitable<bool> mount(const std::string& folder_name, const fs::path& guest_path, bool permanent);
	asio::awaitable<nlohmann::json> get_shared_folder_status(const std::string& folder_name);
	asio::awaitable<bool> umount(const std::string& folder_name, bool permanent);

protected:
	asio::awaitable<void> copy_file_to_guest(const fs::path& src, const fs::path& dst);
	asio::awaitable<void> copy_dir_to_guest(const fs::path& src, const fs::path& dst);

	asio::awaitable<void> send(nlohmann::json command);
	asio::awaitable<nlohmann::json> recv();

	VersionNumber ver;

	virtual asio::awaitable<void> send_raw(const uint8_t* data, size_t size) = 0;
	virtual asio::awaitable<void> recv_raw(uint8_t* data, size_t size) = 0;
};

struct CLIGuestAdditions: GuestAdditions
{
	asio::awaitable<void> set_var(const std::string& var_name, const std::string& var_value, bool global);
	asio::awaitable<std::string> get_var(const std::string& var_name);
};
