
#pragma once

#include "../Utils.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <string>
#include <set>
#include <vector>

struct FlashDrive {
	FlashDrive() = delete;
	FlashDrive(const nlohmann::json& config_);
	virtual ~FlashDrive() = default;

	// Дедлайн операции (заменяет ambient coro::Timeout вокруг upload/download).
	// Бэкенд пробрасывает его в guestfs::Guestfs. max() == без таймаута.
	std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();

	struct DeadlineGuard {
		FlashDrive* fd;
		std::chrono::steady_clock::time_point prev;
		DeadlineGuard(FlashDrive* fd_, std::chrono::steady_clock::time_point d): fd(fd_), prev(fd_->deadline) {
			if (d < fd->deadline) {
				fd->deadline = d;
			}
		}
		~DeadlineGuard() {
			fd->deadline = prev;
		}
		DeadlineGuard(const DeadlineGuard&) = delete;
		DeadlineGuard& operator=(const DeadlineGuard&) = delete;
	};

	DeadlineGuard with_deadline(std::chrono::milliseconds d) {
		return DeadlineGuard(this, std::chrono::steady_clock::now() + d);
	}

	virtual bool is_defined() = 0;
	virtual void create() = 0;
	virtual void undefine() = 0;
	virtual bool has_snapshot(const std::string& snapshot) = 0;
	virtual void make_snapshot(const std::string& snapshot) = 0;
	virtual void delete_snapshot(const std::string& snapshot) = 0;
	virtual void rollback(const std::string& snapshot) = 0;
	virtual fs::path img_path() const = 0;
	virtual void upload(const fs::path& from, const fs::path& to) = 0;
	virtual void download(const fs::path& from, const fs::path& to) = 0;

	std::string id() const;
	std::string name() const;
	std::string prefix() const;

protected:
	nlohmann::json config;
};
