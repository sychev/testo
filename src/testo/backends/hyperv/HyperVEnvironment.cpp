
#include "HyperVEnvironment.hpp"
#include <winapi/RegKey.hpp>

fs::path HyperVEnvironment::testo_dir() const {
	winapi::RegKey regkey(HKEY_LOCAL_MACHINE, "SOFTWARE\\Testo Lang\\Testo", KEY_QUERY_VALUE);
	return fs::path(regkey.get_str("InstallDir")) / "metadata";
}

void HyperVEnvironment::setup(const EnvironmentConfig& config) {
	Environment::setup(config);
}

void HyperVEnvironment::validate_vm_config(const nlohmann::json& config) {
	if (config.count("nic")) {
		auto nics = config.at("nic");

		for (auto& nic: nics) {
			if (nic.count("adapter_type")) {
				std::string driver = nic.at("adapter_type").get<std::string>();
				throw std::runtime_error("NIC \"" +
					nic.at("name").get<std::string>() + "\" has unsupported adapter type: \"" + driver + "\"");
			}
		}
	}

	if (config.count("video")) {
		auto videos = config.at("video");

		for (auto& video: videos) {
			if (video.count("adapter_type")) {
				std::string driver = video.at("adapter_type").get<std::string>();
				throw std::runtime_error("Video \"" +
					video.at("name").get<std::string>() + "\" has unsupported adapter type: \"" + driver + "\"");
			}
		}
	}

	if (config.count("shared_folder")) {
		throw std::runtime_error("Shared folders are not supported for Hyper-V yet");
	}

	if (config.count("lid")) {
		auto lid = config.at("lid").get<std::string>();
		if (lid != "open" && lid != "close") {
			throw std::runtime_error("Unsupported value for \"lid\" attribute: \"" + lid + "\", expected \"open\" or \"close\"");
		}
	}

	if (config.count("battery")) {
		auto charge = config.at("battery").get<int32_t>();
		if (charge < 0 || charge > 100) {
			throw std::runtime_error("Unsupported value for \"battery\" attribute: " + std::to_string(charge) + ", expected a value in range 0..100");
		}
	}

	if (config.count("charging")) {
		auto charging = config.at("charging").get<std::string>();
		if (charging != "on" && charging != "off") {
			throw std::runtime_error("Unsupported value for \"charging\" attribute: \"" + charging + "\", expected \"on\" or \"off\"");
		}
	}
}

void HyperVEnvironment::validate_flash_drive_config(const nlohmann::json& config) {
	throw std::runtime_error("Flash drives are not supported for Hyper-V yet");
}

void HyperVEnvironment::validate_network_config(const nlohmann::json& config) {

}
