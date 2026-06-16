
#include <coro/Application.h>
#include <coro/StreamSocket.h>
#include <clipp.h>
#include <iostream>
#include <testo_guest_additions_protocol/GuestAdditions.hpp>

#ifdef __linux__
struct GA: CLIGuestAdditions {
	// The connect is synchronous (blocking) so that GA can be constructed
	// outside of a coroutine; the I/O afterwards is asynchronous.
	GA(): socket(asio::local::stream_protocol::endpoint("/var/run/testo-guest-additions.sock")) {}

private:
	asio::awaitable<void> send_raw(const uint8_t* data, size_t size) override {
		size_t n = co_await socket.write(data, size);
		if (n != size) {
			throw std::runtime_error(__PRETTY_FUNCTION__);
		}
	}
	asio::awaitable<void> recv_raw(uint8_t* data, size_t size) override {
		size_t n = co_await socket.read(data, size);
		if (n != size) {
			throw std::runtime_error(__PRETTY_FUNCTION__);
		}
	}

	coro::StreamSocket<asio::local::stream_protocol> socket;
};
#else
struct GA: CLIGuestAdditions {
	GA() {
		// IMPLEMENT ME!!!!
		throw std::runtime_error(__PRETTY_FUNCTION__);
	}

private:
	asio::awaitable<void> send_raw(const uint8_t* data, size_t size) override {
		// IMPLEMENT ME!!!!
		throw std::runtime_error(__PRETTY_FUNCTION__);
		co_return;
	}
	asio::awaitable<void> recv_raw(uint8_t* data, size_t size) override {
		// IMPLEMENT ME!!!!
		throw std::runtime_error(__PRETTY_FUNCTION__);
		co_return;
	}
};
#endif

struct MountArgs {
	std::string folder_name;
	std::string guest_path;
	bool permanent = false;
};

struct UmountArgs {
	std::string folder_name;
	bool permanent = false;
};

struct SetArgs {
	std::string var_name;
	std::string var_value;
	bool global = false;
};

struct GetArgs {
	std::string var_name;
};

asio::awaitable<void> mount_mode(const MountArgs& args) {
	bool was_indeed_mounted = co_await GA().mount(args.folder_name, fs::absolute(args.guest_path), args.permanent);
	if (!was_indeed_mounted) {
		std::cout << "The shared folder is already mounted" << std::endl;
	}
}

asio::awaitable<void> umount_mode(const UmountArgs& args) {
	bool was_indeed_umounted = co_await GA().umount(args.folder_name, args.permanent);
	if (!was_indeed_umounted) {
		std::cout << "The shared folder is already umounted" << std::endl;
	}
}

asio::awaitable<void> set_mode(const SetArgs& args) {
	co_await GA().set_var(args.var_name, args.var_value, args.global);
}

asio::awaitable<void> get_mode(const GetArgs& args) {
	std::string var_value = co_await GA().get_var(args.var_name);
	std::cout << var_value;
}

enum class mode {
	mount,
	umount,
	set,
	get
};

mode selected_mode;

asio::awaitable<int> do_main(int argc, char** argv) {

	using namespace clipp;

	mode selected_mode;

	MountArgs mount_args;
	auto mount_spec = "mount options:" % (
		command("mount").set(selected_mode, mode::mount),
		value("folder_name", mount_args.folder_name) % "Shared folder name",
		value("guest_path", mount_args.guest_path) % "Path on the guest where to mount",
		option("--permanent").set(mount_args.permanent) % "Mount this folder automatically after system reboot"
	);

	UmountArgs umount_args;
	auto umount_spec = "umount options:" % (
		command("umount").set(selected_mode, mode::umount),
		value("folder_name", umount_args.folder_name) % "Shared folder name",
		option("--permanent").set(umount_args.permanent) % "Stop mounting this folder automatically after system reboot"
	);

	SetArgs set_args;
	auto set_spec = "set options:" % (
		command("set").set(selected_mode, mode::set),
		value("var_name", set_args.var_name) % "Name of the variable to be set",
		value("var_value", set_args.var_value) % "Value of the variable",
		option("--global").set(set_args.global) % "If this variable should be set for all VMs of the current test"
	);

	GetArgs get_args;
	auto get_spec = "get options:" % (
		command("get").set(selected_mode, mode::get),
		value("var_name", set_args.var_name) % "Name of the variable to be get"
	);

	auto cli = (mount_spec | umount_spec | set_spec | get_spec);

	if (!parse(argc, argv, cli)) {
		std::cout << make_man_page(cli, argv[0]) << std::endl;
		co_return 1;
	}

	switch (selected_mode) {
		case mode::mount:
			co_await mount_mode(mount_args);
			break;
		case mode::umount:
			co_await umount_mode(umount_args);
			break;
		case mode::set:
			co_await set_mode(set_args);
			break;
		case mode::get:
			co_await get_mode(get_args);
			break;
		default:
			throw std::runtime_error("Invalid mode");
	}

	co_return 0;
}

int main(int argc, char** argv) {
	int result = 0;

	coro::Application([&]() -> asio::awaitable<void> {
		try {
			result = co_await do_main(argc, argv);
		} catch (const std::exception& error) {
			std::cerr << error.what() << std::endl;
			result = 1;
		}
	}).run();

	return result;
}
