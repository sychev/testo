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

## Следующие шаги (Этап 3+)

Интерфейс `ReportWriter`/`Reporter` → `awaitable`, затем визиторы
(`VisitorInterpreter*`, `CheckPoint` → `co_await asio::post`, `Timeout` → таймер
в `||` с операцией), и наконец `main.cpp` (`Application`/`CoroPool`/`SignalSet`
→ `io_context` + `co_spawn` + `cancellation_signal` + `asio::signal_set`),
а также `Channel.hpp`/`NNClient`-сокет (общий с `nn_server` — согласовать
отдельно). После этого мост и `coro` из сборки `testo` удаляются.
