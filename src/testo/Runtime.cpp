
#include <testo_runtime/Runtime.hpp>

/*
	Определения процесс-глобальных переменных хостового ядра testo.

	Лежат здесь (в testo_core), а НЕ в main.cpp, чтобы их видели все хостовые
	бинари: и сам testo, и testo_unit_tests. main.cpp в testo_core не входит
	(там точка входа), поэтому глобалы, определённые только в нём, не доставались
	бы тестовому бинарю — отсюда были undefined reference при линковке.

	Объявления — в <testo_runtime/Runtime.hpp> (g_io, REPL_mode_is_active) и в
	<interruption/Interruption.hpp> (g_interrupted, g_cancel_current).
*/

asio::io_context g_io;

std::atomic<bool> g_interrupted(false);
std::function<void()> g_cancel_current;

std::atomic<bool> REPL_mode_is_active(false);
