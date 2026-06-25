
#include <asio.hpp>
#include <clipp.h>
#include <iostream>
#include <testo_guest_additions_protocol/GuestAdditions.hpp>

#ifdef __linux__
struct GA: CLIGuestAdditions {
	GA() {
		asio::local::stream_protocol::endpoint endpoint("/var/run/testo-guest-additions.sock");
		std::error_code op_ec;
		bool done = false;
		socket.async_connect(endpoint, [&](const std::error_code& ec) {
			op_ec = ec;
			done = true;
		});
		while (!done) {
			io.run_one();
		}
		if (op_ec) {
			throw std::system_error(op_ec);
		}
	}

private:
	void send_raw(const uint8_t* data, size_t size) override {
		std::error_code op_ec;
		bool done = false;
		asio::async_write(socket, asio::buffer(data, size), [&](const std::error_code& ec, size_t) {
			op_ec = ec;
			done = true;
		});
		while (!done) {
			io.run_one();
		}
		if (op_ec) {
			throw std::system_error(op_ec);
		}
	}
	void recv_raw(uint8_t* data, size_t size) override {
		std::error_code op_ec;
		bool done = false;
		asio::async_read(socket, asio::buffer(data, size), [&](const std::error_code& ec, size_t) {
			op_ec = ec;
			done = true;
		});
		while (!done) {
			io.run_one();
		}
		if (op_ec) {
			throw std::system_error(op_ec);
		}
	}

	asio::io_context io;
	// Якорь работы: иначе io_context после connect «осушается» и переходит в
	// stopped, и следующий run_one() (в send_raw/recv_raw) виснет на busy-loop.
	asio::executor_work_guard<asio::io_context::executor_type> io_work{asio::make_work_guard(io)};
	asio::local::stream_protocol::socket socket{io};
};
#else
struct GA: CLIGuestAdditions {
	GA() {
		// IMPLEMENT ME!!!!
		throw std::runtime_error(__PRETTY_FUNCTION__);
	}

private:
	void send_raw(const uint8_t* data, size_t size) override {
		// IMPLEMENT ME!!!!
		throw std::runtime_error(__PRETTY_FUNCTION__);
	}
	void recv_raw(uint8_t* data, size_t size) override {
		// IMPLEMENT ME!!!!
		throw std::runtime_error(__PRETTY_FUNCTION__);
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

void mount_mode(const MountArgs& args) {
	bool was_indeed_mounted = GA().mount(args.folder_name, fs::absolute(args.guest_path), args.permanent);
	if (!was_indeed_mounted) {
		std::cout << "The shared folder is already mounted" << std::endl;
	}
}

void umount_mode(const UmountArgs& args) {
	bool was_indeed_umounted = GA().umount(args.folder_name, args.permanent);
	if (!was_indeed_umounted) {
		std::cout << "The shared folder is already umounted" << std::endl;
	}
}

void set_mode(const SetArgs& args) {
	GA().set_var(args.var_name, args.var_value, args.global);
}

void get_mode(const GetArgs& args) {
	std::string var_value = GA().get_var(args.var_name);
	std::cout << var_value;
}

enum class mode {
	mount,
	umount,
	set,
	get
};

mode selected_mode;

int do_main(int argc, char** argv) {

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
		return 1;
	}

	switch (selected_mode) {
		case mode::mount:
			mount_mode(mount_args);
			break;
		case mode::umount:
			umount_mode(umount_args);
			break;
		case mode::set:
			set_mode(set_args);
			break;
		case mode::get:
			get_mode(get_args);
			break;
		default:
			throw std::runtime_error("Invalid mode");
	}

	return 0;
}

int main(int argc, char** argv) {
	int result = 0;

	try {
		result = do_main(argc, argv);
	} catch (const std::exception& error) {
		std::cerr << error.what() << std::endl;
		result = 1;
	}

	return result;
}
