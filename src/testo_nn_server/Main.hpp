
#pragma once

#include <iostream>
#include <chrono>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>

#include <asio.hpp>

#include <nlohmann/json.hpp>
#include <ghc/filesystem.hpp>

#include "nn/OnnxRuntime.hpp"
#include "MessageHandler.hpp"

namespace fs = ghc::filesystem;

// Единственный io_context сервера (заменяет coro::Application/IoService).
// Глобальный, чтобы Windows-служба могла остановить его из ControlHandler.
inline asio::io_context g_nn_io;

void local_handler(const nlohmann::json& settings) {
	auto port = settings.value("port", 8156);

	asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), port);
	asio::ip::tcp::acceptor acceptor(g_nn_io);
	acceptor.open(endpoint.protocol());
	acceptor.set_option(asio::socket_base::reuse_address(true));
	acceptor.bind(endpoint);
	acceptor.listen();

	spdlog::info(fmt::format("Listening on port {}", port));

	// Последовательная обработка: одно соединение целиком, затем следующий accept.
	// Сохраняет однопоточность (onnx-инференс не вызывается из разных потоков).
	while (true) {
		asio::ip::tcp::socket socket(g_nn_io);
		std::error_code accept_ec;
		bool accepted = false;
		acceptor.async_accept(socket, [&](const std::error_code& ec) {
			accept_ec = ec;
			accepted = true;
		});
		while (!accepted) {
			if (g_nn_io.run_one() == 0) {
				break; // g_nn_io.stop() из ControlHandler (остановка службы)
			}
		}
		if (g_nn_io.stopped()) {
			return;
		}
		if (accept_ec) {
			spdlog::error(fmt::format("Accept failed: {}", accept_ec.message()));
			continue;
		}

		std::string new_connection;
		try {
			new_connection = socket.remote_endpoint().address().to_string() +
				":" + std::to_string(socket.remote_endpoint().port());
			spdlog::info(fmt::format("Accepted new connection: {}", new_connection));

			std::shared_ptr<Channel> channel(new Channel(std::move(socket)));

			MessageHandler message_handler(std::move(channel));
			message_handler.run();
		} catch (const std::system_error& error) {
			if (error.code().value() == 2) {
				spdlog::info(fmt::format("Connection broken: {}", new_connection));
			}
		} catch (const std::exception& error) {
			std::cout << "Error inside local acceptor loop: " << error.what() << std::endl;
		}
	}
}

void setup_logs(const nlohmann::json& settings) {
	std::string log_file_path = settings.at("log_file").get<std::string>();

	auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file_path);
	auto console_sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
	auto logger = std::make_shared<spdlog::logger>("basic_logger", spdlog::sinks_init_list{file_sink, console_sink});

	std::string log_level = settings.value("log_level", "info");
	if (log_level == "info") {
		logger->set_level(spdlog::level::info);
		logger->flush_on(spdlog::level::info);
	} else if (log_level == "trace") {
		logger->set_level(spdlog::level::trace);
		logger->flush_on(spdlog::level::trace);
	} else {
		throw std::runtime_error("Only \"info\" and \"trace\" log levels are supported");
	}
	spdlog::set_default_logger(logger);
}

nlohmann::json load_settings(const std::string& settings_path) {
	nlohmann::json settings;
	if (!fs::exists(settings_path)) {
		fs::create_directories(fs::path(settings_path).parent_path());
		std::ofstream os(settings_path);
		if (!os) {
			throw std::runtime_error(std::string("Can't open settings file: ") + settings_path);
		}
		os << R"(
{
	"port": 8156,
	"log_level": "info",
	"use_gpu": false,
	"gpu_id": 0
}
)";
	}
	std::ifstream is(settings_path);
	if (!is) {
		throw std::runtime_error(std::string("Can't open settings file: ") + settings_path);
	}
	is >> settings;
	return settings;
}

void app_main(const nlohmann::json& settings) {
	try {
		setup_logs(settings);

		bool use_gpu = settings.value("use_gpu", false);
		size_t gpu_id = settings.value("gpu_id", 0);

		if (!use_gpu && settings.count("gpu_id")) {
			spdlog::info("Ignoring 'gpu_id' setting because GPU mode is disabled...");
		}

		nn::onnx::Runtime onnx_runtime(!use_gpu, gpu_id);

		spdlog::info("Starting testo nn server");
		spdlog::info("Testo framework version: {}", TESTO_VERSION);
		spdlog::info("GPU mode enabled: {}", use_gpu);
		local_handler(settings);
	} catch (const std::exception& error) {
		spdlog::error(error.what());
	} catch (...) {
		spdlog::error("Unknown exception");
	}
}
