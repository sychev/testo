
#pragma once

#include <chrono>

#include <nlohmann/json.hpp>

#include <ghc/filesystem.hpp>
namespace fs = ghc::filesystem;

#include <version_number/VersionNumber.hpp>

struct GuestAdditions {
	virtual ~GuestAdditions() = default;

	/*
		Абсолютный дедлайн всей текущей операции. Заменяет ambient-семантику
		coro::Timeout: один дедлайн накрывает всю многошаговую (чанковую)
		последовательность send_raw/recv_raw. max() == без таймаута.

		Листовые транспорты (send_raw/recv_raw в наследниках) взводят свой
		steady_timer на expires_at(deadline), поэтому общий дедлайн делится
		между всеми чанковыми вызовами.
	*/
	std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();

	/*
		RAII-замена coro::Timeout (1:1). Сохраняет прежний дедлайн, ставит
		min(прежний, now + d) и восстанавливает прежний в деструкторе. min
		воспроизводит вложенность coro::Timeout: внутренний (например, 3s в
		is_avaliable) не затирает внешний, срабатывает тот, что раньше.
	*/
	struct DeadlineGuard {
		GuestAdditions* ga;
		std::chrono::steady_clock::time_point prev;

		DeadlineGuard(GuestAdditions* ga_, std::chrono::steady_clock::time_point d): ga(ga_), prev(ga_->deadline) {
			if (d < ga->deadline) {
				ga->deadline = d;
			}
		}
		~DeadlineGuard() {
			ga->deadline = prev;
		}

		DeadlineGuard(const DeadlineGuard&) = delete;
		DeadlineGuard& operator=(const DeadlineGuard&) = delete;
	};

	DeadlineGuard with_deadline(std::chrono::milliseconds d) {
		return DeadlineGuard(this, std::chrono::steady_clock::now() + d);
	}

	bool is_avaliable(std::chrono::milliseconds timeout = std::chrono::seconds(3));
	void copy_to_guest(const fs::path& src, const fs::path& dst);
	void copy_from_guest(const fs::path& src, const fs::path& dst);
	void remove_from_guest(const fs::path& path);
	nlohmann::json execute(const std::string& command, const std::map<std::string, std::string>& vars,
		const std::function<void(const std::string&)>& callback);
	std::string get_tmp_dir();
	bool mount(const std::string& folder_name, const fs::path& guest_path, bool permanent);
	nlohmann::json get_shared_folder_status(const std::string& folder_name);
	bool umount(const std::string& folder_name, bool permanent);

protected:
	void copy_file_to_guest(const fs::path& src, const fs::path& dst);
	void copy_dir_to_guest(const fs::path& src, const fs::path& dst);

	void send(nlohmann::json command);
	nlohmann::json recv();

	VersionNumber ver;

	virtual void send_raw(const uint8_t* data, size_t size) = 0;
	virtual void recv_raw(uint8_t* data, size_t size) = 0;
};

struct CLIGuestAdditions: GuestAdditions
{
	void set_var(const std::string& var_name, const std::string& var_value, bool global);
	std::string get_var(const std::string& var_name);
};
