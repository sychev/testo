# PoC: переход testo с библиотеки `coro` на нативные asio-корутины (C++20)

Этот каталог — **Этап 1** миграции `testo` на C++20 + asio 1.36.0, цель которой —
полностью убрать зависимость `testo` от библиотеки `3rd_party/coro` и писать
асинхронный код напрямую на asio (`asio::awaitable` / `co_await` / `co_spawn`).

> Касается только `testo`. `nn_server` и прочие бинарники не затрагиваются по
> смыслу (их код не переписывается), но bump тулчейна на C++20 и asio 1.36
> общий для всего проекта, т.к. asio в репозитории один.

## Что сделано на этом этапе

1. **Bump тулчейна** (`CMakeLists.txt`):
   - `CMAKE_CXX_STANDARD 20`;
   - asio 1.36.0 подтягивается через `FetchContent` и перекрывает вендоренный
     `3rd_party/asio` (через `include_directories(BEFORE ...)`).

2. **Правки совместимости `coro` под asio 1.36** (временно, пока `coro` ещё
   используется остальным кодом `testo`). asio 1.36 удалил ряд API:
   - `asio::io_service` → `asio::io_context` (`coro/IoService.h`);
   - `io_service::post/dispatch` (методы) → свободные `asio::post/asio::dispatch`
     (`coro/IoService.h`);
   - `asio::io_service::work` → `asio::executor_work_guard` (`coro/Work.h`);
   - `steady_timer::expires_from_now` → `expires_after` (`coro/Timer.h`,
     `coro/Timeout.h`);
   - `#include <exception>` в `coro/Coro.h` (под C++20 он больше не приходит
     транзитивно через asio).

3. **Временный мост** `src/testo/coro_asio_bridge.hpp` — `coro::await(awaitable)`:
   запускает `asio::awaitable<T>` на текущем io_context, усыпляет coro-фибер до
   завершения, пробрасывает исключения и **корректно отменяет** asio-операцию
   через `cancellation_signal`, если в фибер прилетает `CancelError` /
   `TimeoutError`. Это позволяет переводить модули на asio-корутины по одному,
   не трогая остальной (ещё coro-шный) код. Удаляется в конце миграции.

4. **Первый переведённый срез** — `src/testo/report/ReportWriterNativeRemote.*`:
   транспорт (`connect`/`send`/`recv`) переписан на `asio::awaitable` поверх
   `asio::ip::tcp::socket`; публичный интерфейс `ReportWriter` пока остаётся
   синхронным за счёт моста.

## Как это проверялось

`main.cpp` — round-trip: coro-фибер через мост вызывает awaitable-транспорт
(connect → send → recv) к эхо-серверу на отдельном потоке.

`main_cancel.cpp` — отмена: вечный `async_read` прерывается `coro::Timeout`,
asio-операция отменяется через мост (доказывает, что таймауты/Ctrl-C, на которых
держится testo, продолжают работать).

Сборка/запуск (нужен только компилятор с C++20 и доступ к asio 1.36):

```sh
./build.sh    # скачивает asio 1.36, собирает coro + оба теста, запускает их
```

Ожидаемый вывод:

```
[client] server replied: CONFIRMED:hello-from-coro-fiber
PoC OK: awaitable-транспорт отработал через coro-мост
PoC OK: read прерван по coro::Timeout, asio-операция отменена через мост
```

## Этап 2 (сделано)

Транспорт и простые таймеры переведены на `awaitable` через мост:

- **Транспорт guest additions**: `QemuGuestAdditions` (Unix-сокет) и
  `HyperVGuestAdditions` (vsock) — `connect`/`send_raw`/`recv_raw` теперь
  `asio::awaitable` поверх `asio::*::socket`, наружу синхронные через мост.
- **`ReportWriterNativeRemote`** — переведён ещё на этапе 1.
- **Таймеры `coro::Timer`** заменены на `coro::sleep_for` (хелпер добавлен в
  мост): `NNClient`, `VisitorInterpreterAction::visit_sleep`,
  `VisitorInterpreterActionMachine` (член `timer`, 10 мест), `QemuVM::resume`,
  `HyperVVM`.
- **Чистка**: убраны неиспользуемые/вестигиальные `coro`-инклюды в `Configs`,
  `Utils`, `QemuFlashDrive`.
- **Правка под asio 1.36**: `asio::ip::address::from_string` → `make_address`
  (`Utils.cpp`).

Проверено: `Configs.cpp`, `Utils.cpp`, `NNClient.cpp` и транспортный паттерн
(tcp/unix-сокет + `coro::sleep_for`) проходят компиляцию против asio 1.36
(`-fsyntax-only`). Файлы с зависимостью от libvirt/Windows
(`QemuVM`/`QemuGuestAdditions`/`HyperV*`/визиторы) переведены по проверенному
паттерну, но локально не собирались (нет libvirt-dev / Windows SDK).

`coro::Timeout` и `coro::CheckPoint` оставлены намеренно: они завязаны на
ещё-синхронный код визиторов и переводятся на `||`-комбинаторы /
`co_await asio::post` на следующих этапах, когда визиторы станут `awaitable`.

## Этап 3 (сделано) — протокол GuestAdditions на awaitable

Пройдена граница общего протокола (по согласованию — scope расширен на
`testo_guest_additions`, агент `testo-guest-additions` не затронут, т.к. не
использует клиентский класс `GuestAdditions`).

- **`GuestAdditions` (общий протокол)**: все методы (`is_avaliable`, `execute`,
  `copy_to_guest/from_guest`, `mount/umount`, `send/recv`, `send_raw/recv_raw`,
  `set_var/get_var` …) переведены на `asio::awaitable` + `co_await`.
- **`coro::Timeout` внутри протокола** (в `is_avaliable`/`get_tmp_dir`) заменён
  на `coro::with_timeout(op, d)` — чистый asio через `awaitable_operators` (`||`
  с таймером), добавлен в мост.
- **Реализации транспорта** `QemuGuestAdditions`/`HyperVGuestAdditions`:
  `send_raw/recv_raw` теперь прямые awaitable-оверрайды (без внутреннего моста).
- **Вызывающие** (`QemuVM` 5 мест, `VisitorInterpreterActionMachine` 8 мест,
  guest `CLI.cpp` 4 места) пока вызывают протокол через мост `coro::await(...)`
  — граница awaitable поднимется выше на следующих этапах.
- **Мост перенесён** в общую папку `src/testo_guest_additions_protocol/` (нужен
  обоим бинарникам), все включения обновлены.
- Починен латентный баг: `QemuFlashDrive.cpp` не включал `<cstdint>` (раньше
  приходил транзитивно).

Проверено: **все 51 .cpp `testo_core`** (libvirt/guestfs поставлены) + общий
`GuestAdditions.cpp` + guest `CLI.cpp` проходят `-fsyntax-only` под C++20/asio
1.36; PoC-harness (round-trip + отмена) собирается и проходит.

## Этап 4 (сделано) — слой отчётов `ReportWriter` на awaitable

Вся иерархия `ReportWriter` переведена на `asio::awaitable` + `co_await`:
- `ReportWriter` (база), `ReportWriterNative`, `ReportWriterNativeLocal`,
  `ReportWriterAllure`, `ReportWriterNativeRemote` — все виртуальные методы
  (`launch_begin`/`report`/`report_screenshot`/`test_*`/`launch_end` …) теперь
  `awaitable`; внутренние кросс-вызовы (`report_prefix`→`report`, подкласс→база)
  идут через `co_await`.
- `ReportWriterNativeRemote` больше не бриджует внутри — транспорт целиком на
  `co_await`.
- **`Reporter` стал границей моста**: его публичный интерфейс остался
  синхронным, а 13 вызовов `report_writer->…` обёрнуты в `coro::await(...)`.
  Благодаря этому 57 вызовов `reporter.*` в визиторах **не тронуты** (никакого
  временного churn).
- Обойдён ICE GCC 13 на `co_await send({brace-init json})` — JSON выносится в
  именованную переменную перед `co_await` (clang компилировал и так).

Проверено: **все 51 .cpp `testo_core`** проходят `-fsyntax-only` под GCC 13
(C++20/asio 1.36), 0 ошибок; слой отчётов компилируется и на clang 18.

## Следующие шаги (Этап 5+)

Поднять границу моста ещё выше: VM-методы, использующие GuestAdditions
(`QemuVM::make_snapshot/rollback` и т.п.) → `awaitable`; затем визиторы
(`VisitorInterpreter*`) — `co_await` для ga/reporter/VM, `CheckPoint` →
`co_await asio::post`, `coro::Timeout` → `coro::with_timeout`; затем `Reporter`
делается `awaitable` (57 вызовов переключаются с `coro::await` на `co_await`); и
наконец `main.cpp` (`Application`/`CoroPool`/`SignalSet` → `io_context` +
`co_spawn` + `cancellation_signal` + `asio::signal_set`), плюс `Channel.hpp`/
`NNClient`-сокет. После этого мост и `coro` из сборки удаляются.
