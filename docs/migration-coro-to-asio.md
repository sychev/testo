# Миграция с `3rd_party/coro` (stackful-файберы) на C++20-корутины asio

> Документ описывает переход проекта `testo` с самописной файберной библиотеки
> `3rd_party/coro` на C++20-сопрограммы (`co_await`) поверх **asio 1.36.0**, а также
> объясняет принцип работы нового подхода и учит «читать и проговаривать» новый код.

---

## 1. Краткое резюме

| | Было | Стало |
|---|---|---|
| Асинхронность | **stackful**-файберы (ucontext / Win32 Fibers), 4 МБ стека на корутину | **stackless** C++20-корутины (`asio::awaitable`) |
| Библиотека | `3rd_party/coro` (собственный движок поверх колбэков asio) | только **asio 1.36.0** + тонкий header-only shim `lib/coro` |
| Стандарт | C++17 | **C++20** |
| Вид кода | синхронно выглядящий код, который «магически» уступал управление | явные точки приостановки `co_await` |
| Отмена/таймаут | исключение, брошенное в произвольную глубину стека файбера | кооперативная отмена asio в точках `co_await` |

Ключевая мысль: **раньше приостановка была невидимой** (любой `socket.read()` мог уступить
управление, потому что под ним был отдельный стек файбера). **Теперь приостановка видима** — она
происходит только там, где написано `co_await`, и поэтому каждая функция, которая может
приостановиться, обязана вернуть `asio::awaitable<T>`.

---

## 2. Изменения инфраструктуры

1. **asio 1.14.0 → 1.36.0** — заменён вендоренный `3rd_party/asio` (+ `asio.hpp`).
2. **C++17 → C++20** — `set(CMAKE_CXX_STANDARD 20)` + `CMAKE_CXX_STANDARD_REQUIRED ON`.
3. **Удалена библиотека `3rd_party/coro`** целиком (файберы `ucontext`/Win32, `Coro`, `CoroPool`,
   `AsioTask`, `Mutex`, `Queue`, тесты).
4. **Новый header-only shim `lib/coro/`** — пространство имён и имена заголовков **сохранены**
   (`#include <coro/Timer.h>` и т.п.), поэтому строки `#include` менять не пришлось — менялась
   только реальная `co_await`-раскраска.
5. **`testo_nn_server` исключён из сборки** (по условию задачи).
6. Сопутствующие фиксы, всплывшие из-за более строгого C++20/asio 1.36:
   - `lib/posixapi/*` — добавлен `#include <cstdint>`;
   - `Utils.cpp` — `asio::ip::address::from_string()` → `asio::ip::make_address()`
     (старая функция удалена в новых asio).

---

## 3. Принцип работы нового подхода (подробно)

### 3.1. Stackful против stackless — в чём суть

**Старый `coro` (stackful).** Каждая корутина владела отдельным стеком (4 МБ). Когда код вызывал
`socket.read()`, под капотом запускалась асинхронная операция asio, а текущий стек целиком
«замораживался» переключением контекста (`swapcontext`). Снаружи это выглядело как обычный
синхронный вызов:

```cpp
// СТАРЫЙ стиль: выглядит синхронно, но втихую уступает поток на время чтения
size_t n = socket.read(buf, size);
```

Поскольку замораживался весь стек, **промежуточные функции не требовали никаких пометок** —
приостановка «пронизывала» их незаметно.

**Новый подход (stackless).** У C++20-корутины нет отдельного стека: компилятор превращает функцию
в конечный автомат, сохраняя её локальные переменные в куче (frame). Приостановиться можно **только
в точке `co_await`**, и **только если сама функция объявлена корутиной** (возвращает `awaitable`).
Поэтому:

```cpp
// НОВЫЙ стиль: приостановка видна явно
size_t n = co_await socket.read(buf, size);
```

### 3.2. «Раскраска функций» (function coloring)

Так как приостановиться может только корутина, **любая функция, которая (прямо или транзитивно)
делает `co_await`, обязана сама стать корутиной** — вернуть `asio::awaitable<T>` — а каждый её
вызов обязан быть `co_await`-нут. Это и есть «раскраска»: «асинхронность» вирусно поднимается вверх
по дереву вызовов от листьев (сетевой I/O, таймеры) до точки входа (`main`).

Мысленная модель:

> «Эта функция где-то внутри ждёт ввод-вывод → значит она *красная* (awaitable) → значит все, кто
> её зовёт, должны её `co_await`-ить → значит и они становятся *красными*».

### 3.3. Как это исполняется

- `asio::io_context` — цикл событий (event loop). Запускается `ctx.run()`.
- `asio::co_spawn(ctx, my_coro(), token)` — планирует корутину на исполнение в этом цикле.
- Внутри корутины `co_await some_async_op(use_awaitable)` запускает асинхронную операцию asio,
  **приостанавливает** корутину и возвращает управление в `io_context`. Когда операция завершится
  (придёт сетевое событие, сработает таймер), `io_context` **возобновит** корутину ровно с того
  места, где был `co_await`, подставив результат.
- Всё в одном потоке: пока одна корутина ждёт I/O, цикл исполняет другие готовые задачи.

### 3.4. Отмена и таймауты в новой модели

В старом `coro` таймаут был RAII-объектом, который бросал исключение **внутрь** работающего файбера,
разматывая его настоящий стек откуда угодно. В stackless так нельзя: отмена доставляется
**кооперативно и только в точках `co_await`**.

- asio распространяет *cancellation* по дереву `co_await`. Когда операцию отменяют, ближайший
  `co_await` завершается с `asio::error::operation_aborted` (т.е. бросает исключение в точке
  приостановки).
- «Накрыть таймаутом произвольную лексическую область» больше нельзя — защищаемый участок
  оформляется лямбдой-корутиной и передаётся в `coro::with_timeout(...)` (см. §4 и §7).
- Кооперативная отмена теста (Ctrl-C) реализована через гонку «работа против наблюдателя сигналов»
  с оператором `||` (см. §8.4).

---

## 4. Новый shim `lib/coro` — справочник

Все типы — тонкие awaitable-обёртки над asio. Никаких файберов; зависимость — только asio.

| Заголовок | Что даёт | Как использовать |
|---|---|---|
| `Runtime.h` | `coro::executor_type`, `coro::current_executor()`, `coro::CancelError` | базис, tls-исполнитель Application |
| `Application.h` | `coro::Application(fn)` где `fn` → `awaitable<void>` | точка входа: `coro::Application(main).run();` |
| `Timer.h` | `coro::Timer` | `co_await t.waitFor(dur);` / `waitUntil(tp)` |
| `Stream.h`, `StreamSocket.h` | `read/write/readSome/writeSome/connect` | `co_await s.read(p,n);` ; `s.handle()` — «сырой» asio-сокет для **синхронного** `connect` в конструкторе |
| `Acceptor.h` | `coro::Acceptor<Proto>` | `auto sock = co_await acc.accept();` |
| `SignalSet.h` | `coro::SignalSet` | `int sig = co_await set.wait();` |
| `Resolver.h` | `coro::Resolver<Proto>` | `co_await r.resolve(host, svc);` |
| `CheckPoint.h` | `coro::CheckPoint()` | `co_await coro::CheckPoint();` — точка кооперативной отмены |
| `Timeout.h` | `coro::with_timeout`, `coro::TimeoutError` | замена RAII `coro::Timeout` (см. ниже) |
| `Finally.h` | `coro::Finally` | scope-guard, как и раньше (синхронный) |

### 4.1. Самое важное: `with_timeout`

```cpp
// lib/coro/Timeout.h
template <typename F>
asio::awaitable<void> with_timeout(std::chrono::nanoseconds duration, F f) {
    using namespace asio::experimental::awaitable_operators;
    asio::steady_timer timer(co_await asio::this_coro::executor);
    timer.expires_after(duration);

    // Гонка: защищаемый блок f() ПРОТИВ таймера.
    auto which = co_await ( std::move(f)() || timer.async_wait(asio::use_awaitable) );

    if (which.index() == 1) {   // победил таймер
        throw TimeoutError();
    }
}
```

Как читать: «*запусти `f()` и таймер одновременно; верни управление, когда завершится первый;
если первым был таймер (`index() == 1`) — брось `TimeoutError`*». При победе таймера оператор `||`
**автоматически отменяет** `f()`, и все её вложенные `co_await` завершатся `operation_aborted`.

---

## 5. Правила миграции и ловушки C++20

1. **Раскраска.** Функция, которая `co_await`-ит → возвращает `asio::awaitable<T>`; каждый её вызов
   → `co_await`.
2. **`co_return` вместо `return`** внутри корутины (`co_return;` для `void`, `co_return x;` для
   значения). Любой `return` в корутине — ошибка компиляции.
3. **`co_await` ЗАПРЕЩЁН внутри `catch`.** Это ограничение языка. Решение: в `catch` выставить флаг
   / сохранить данные, а сам `co_await` сделать **после** блока `try/catch` (см. §7.2).
4. **Корутина обязана содержать хотя бы один `co_await`/`co_return`.** Если функция помечена
   `awaitable`, но только бросает исключение — добавляем «недостижимый» `co_return;`.
5. **Время жизни временных объектов.** Временный объект, владеющий awaited-операцией, живёт до конца
   полного выражения, а `co_await` — часть полного выражения, поэтому `co_await Temp().method()`
   безопасно.
6. **Конструкторы не могут `co_await`.** Если в конструкторе нужно подключиться к сокету — делаем
   **синхронный** блокирующий `connect` через `socket.handle().connect(ep)`, а собственно I/O —
   асинхронным.

---

## 6. Контракт: что асинхронно, а что осталось синхронным

Раскрашивалось **только то, что обязано кооперативно отменяться/таймаутиться**. Это сознательно
ограничивает «вирус» co_await.

**Асинхронно (`awaitable`):**
- `GuestAdditions::*` (протокол гостевых дополнений: copy/exec/mount/…);
- `NNClient::eval_js` и протокол `Channel` нейросетевого сервера;
- весь визиторный слой действий (`VisitorInterpreter*`), `IR::MacroCall::visit_interpreter`,
  `IR::Program::run`, режимы `run_mode`/`clean_mode`, `do_main`;
- `coro::CheckPoint`, `coro::with_timeout`, `coro::Timer`.

**Синхронно (осознанно НЕ раскрашено):**
- **`Reporter` и `ReportWriter`** — логирование/отчёт не нуждается в кооперативной отмене.
  `ReportWriterNativeRemote` переведён на **синхронные** asio-операции (`asio::read`/`asio::write`
  поверх `socket.handle()`).
- **Весь VM/бэкенд-слой** (`VM`, `QemuVM`, `HyperVVM`, libvirt) — это блокирующие вызовы libvirt и
  ограниченный локальный поллинг. Бывшие `coro::Timer/Timeout` там (например в `QemuVM::resume`)
  заменены на `std::chrono::steady_clock`-дедлайн + `std::this_thread::sleep_for`.
- **IR-слой**, парсер, лексер, семантический визитор, js — чистый CPU.
- Граница между синхронным VM и асинхронным гостевым I/O: `vmc->vm()->guest_additions()` — вызов
  **синхронный**, а методы у полученного объекта — **awaitable**.

---

## 7. Разбор кода: НЕВИЗИТОРНЫЙ слой

### 7.1. Протокол гостевых дополнений: `with_timeout` вместо RAII-таймаута

`src/testo_guest_additions_protocol/GuestAdditions.cpp`:

```cpp
asio::awaitable<bool> GuestAdditions::is_avaliable(std::chrono::milliseconds time_to_wait) {
    try {
        nlohmann::json request = {{"method", "check_avaliable"}};

        bool success = false;
        co_await coro::with_timeout(time_to_wait, [&]() -> asio::awaitable<void> {
            co_await send(std::move(request));
            auto response = co_await recv();
            success = response.at("success").get<bool>();
        });
        co_return success;
    } catch (const std::exception&) {
        co_return false;
    }
}
```

**Как проговаривать в голове:**
> «`is_avaliable` — *красная* функция, возвращает `awaitable<bool>`.
> Внутри: `co_await with_timeout(timeout, лямбда)` — *“выполни блок, но не дольше timeout”*.
> Блок — это лямбда-корутина: *“отправь запрос (`co_await send`), дождись ответа (`co_await recv`),
> запиши флаг в `success`”*.
> Если таймаут сработал раньше — `with_timeout` бросит `TimeoutError`, его поймает `catch`,
> и мы `co_return false`. Иначе `co_return success`.»

Обрати внимание: результат блока (`bool`) «выносится наружу» через захваченную по ссылке переменную
`success`, потому что `with_timeout` сам возвращает `void`.

### 7.2. Ловушка «`co_await` в `catch`»: NNClient

`src/testo/NNClient.cpp` — повтор подключения с задержкой. Раньше `coro::Timer().waitFor(2s)` стоял
**внутри** `catch`. В C++20 так нельзя, поэтому ожидание вынесено за `try/catch` через флаг:

```cpp
asio::awaitable<void> NNClient::establish_connection_wrapper(
        const std::function<asio::awaitable<void>()>& fn) {
    for (size_t i = 0; i < establish_connection_tries; ++i) {
        bool need_retry = false;
        try {
            co_await fn();      // успешно подключились/отработали
            co_return;
        } catch (const std::exception& error) {
            std::cerr << error.what() << std::endl;
            if (i < (establish_connection_tries - 1)) {
                need_retry = true;   // co_await здесь делать НЕЛЬЗЯ — только ставим флаг
            }
        }
        if (need_retry) {
            co_await coro::Timer().waitFor(2s);   // ждём уже ПОСЛЕ try/catch
        }
    }
    throw std::runtime_error("Exceeding the number of attempts to connect to the server");
}
```

**Как проговаривать:**
> «Цикл попыток. *“Попробуй `co_await fn()`; получилось — `co_return`”*. Если бросило исключение —
> в `catch` только **запоминаем** “нужен повтор” (ждать в `catch` запрещено). После `try/catch`,
> *“если нужен повтор — `co_await` подожди 2 секунды”* и идём на следующую итерацию.»

Здесь же видно правило №1 для колбэков: `fn` теперь имеет тип
`std::function<asio::awaitable<void>()>` — «*функция, возвращающая awaitable*», и её результат
`co_await`-ится.

### 7.3. Daemon-канал: цикл дочитывания

`src/testo_guest_additions/src/Channel.cpp`:

```cpp
asio::awaitable<void> Channel::receive_raw(uint8_t* data, size_t size) {
    size_t already_read = 0;
    while (already_read < size) {
        size_t n = co_await read(&data[already_read], size - already_read);
        if (n == 0) {
            throw std::runtime_error("EOF while reading");
        }
        already_read += n;
    }
}
```

**Как проговаривать:**
> «Пока не дочитали `size` байт: *“`co_await read(...)` — приостановись, пока не придут данные”*,
> прибавь прочитанное. `read` — чисто виртуальный awaitable-метод; в QEMU это блокирующий fd
> (`co_return n;` без реальной приостановки), в HyperV/local — asio-сокет (реальный `co_await`).»

### 7.4. Почему отчёт остался синхронным

`src/testo/report/ReportWriterNativeRemote.cpp` — отчёт по TCP **не** на пути кооперативной отмены,
поэтому используем синхронные операции asio и НЕ заражаем весь `Reporter`:

```cpp
void ReportWriterNativeRemote::send(const nlohmann::json& json) {
    std::vector<uint8_t> json_data = nlohmann::json::to_cbor(json);
    uint32_t json_size = (uint32_t)json_data.size();
    // Синхронный I/O: отчёт не участвует в кооперативной отмене.
    asio::write(socket.handle(), asio::buffer((uint8_t*)&json_size, sizeof(json_size)));
    asio::write(socket.handle(), asio::buffer(json_data.data(), json_size));
}
```

**Как проговаривать:**
> «`send` — обычная *синхронная* функция (нет `co_await`, нет `awaitable`). `asio::write` блокирует
> поток до отправки. Так `Reporter` и все `reporter.xxx(...)` в визиторах остаются синхронными — это
> сильно сократило раскраску.»

---

## 8. Разбор кода: ВИЗИТОРНЫЙ слой

### 8.1. `visit_copy` — таймаут поверх вложенных `co_await`

`src/testo/visitors/VisitorInterpreterActionMachine.cpp`:

```cpp
asio::awaitable<void> VisitorInterpreterActionMachine::visit_copy(const IR::Copy& copy) {
    TRACE();
    try {
        reporter.copy(current_controller, copy);          // СИНХРОННО (reporter)

        co_await coro::with_timeout(copy.timeout().value(), [&]() -> asio::awaitable<void> {
            if (vmc->vm()->state() != VmState::Running) {  // СИНХРОННО (libvirt)
                throw std::runtime_error("virtual machine is not running");
            }
            auto ga = vmc->vm()->guest_additions();        // СИНХРОННО (получили объект)

            if (!co_await ga->is_avaliable()) {            // АСИНХРОННО (гостевой I/O)
                throw std::runtime_error("guest additions are not installed");
            }
            if (copy.ast_node->is_to_guest()) {
                if (!fs::exists(copy.from())) {
                    throw std::runtime_error("Specified path doesn't exist: " + copy.from());
                }
                co_await ga->copy_to_guest(copy.from(), copy.to());   // АСИНХРОННО
            } else {
                co_await ga->copy_from_guest(copy.from(), copy.to()); // АСИНХРОННО
            }
        });
    } catch (const std::exception& error) {
        std::throw_with_nested(ActionException(copy.ast_node, current_controller));
    }
}
```

**Как проговаривать:**
> «`visit_copy` — *красная*. Сначала *синхронно* зарепортить начало копирования.
> Затем *“выполни блок копирования, но не дольше `copy.timeout()`”*.
> Внутри блока: проверки VM — *синхронные* (libvirt), `guest_additions()` — *синхронно* вернул
> объект, а вот `is_avaliable`/`copy_to_guest` — *“приостановись и дождись гостя”* (`co_await`).
> Если истёк таймаут — `with_timeout` отменит эти `co_await` и бросит `TimeoutError`; внешний
> `catch` обернёт всё в `ActionException`.»

Главное наблюдение: **синхронные (reporter/VM) и асинхронные (guest additions) вызовы спокойно
соседствуют** — `co_await` ставится ровно там, где идёт обращение к гостю.

### 8.2. `screenshot_loop` — таймер, чекпойнт и шаблонная корутина

```cpp
template <typename Func>
asio::awaitable<bool> VisitorInterpreterActionMachine::screenshot_loop(
        Func&& func, std::chrono::milliseconds timeout, std::chrono::milliseconds interval) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    uint64_t empty_screenshots_counter = 0;

    do {
        auto start = std::chrono::high_resolution_clock::now();
        auto& screenshot = vmc->make_new_screenshot();     // СИНХРОННО (libvirt)

        if (screenshot.data) {
            empty_screenshots_counter = 0;
            bool screenshot_found = co_await func(screenshot);  // АСИНХРОННО (нейросеть)
            if (screenshot_found) {
                co_return true;
            }
        } else {
            ++empty_screenshots_counter;
            if (empty_screenshots_counter > 10) {
                throw std::runtime_error("Can't get a screenshot ... it's turned off");
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        if (interval > end - start) {
            co_await timer.waitFor(interval - (end - start));   // пауза между кадрами
        } else {
            co_await coro::CheckPoint();   // не успели — хотя бы дать шанс отмене
        }
    } while (std::chrono::steady_clock::now() < deadline);

    co_return false;
}
```

**Как проговаривать:**
> «Крутимся до дедлайна. Каждую итерацию: *синхронно* снять скриншот; *“`co_await func(screenshot)`
> — спроси нейросеть, нашлось ли”*; нашлось — `co_return true`.
> В конце итерации: если есть запас по интервалу — *“`co_await` поспи остаток интервала”*, иначе —
> *“`co_await CheckPoint()` — точка, где можно кооперативно отмениться”*. Истёк дедлайн — `co_return
> false`.»

Зачем `CheckPoint()` в ветке «не успели»: даже без паузы нужно дать циклу событий шанс доставить
отмену (Ctrl-C), иначе плотный цикл никогда не уступит управление.

### 8.3. `visit_check` — `co_return co_await` и лямбда-awaitable

```cpp
asio::awaitable<bool> VisitorInterpreterActionMachine::visit_check(const IR::Check& check) {
    TRACE();
    try {
        reporter.check(vmc, check);   // синхронно

        co_return co_await screenshot_loop(
            [&](const stb::Image<stb::RGB>& screenshot) -> asio::awaitable<bool> {
                co_return co_await visit_detect_expr(check.ast_node->select_expr, screenshot);
            },
            check.timeout().value(), check.interval().value());
    } catch (const std::exception& error) {
        std::throw_with_nested(ActionException(check.ast_node, current_controller));
    }
}
```

**Как проговаривать `co_return co_await X`:**
> «*“Дождись `X` (`co_await`) и сразу верни его результат (`co_return`)”*.»
> То есть `visit_check` запускает `screenshot_loop`, передавая ему лямбду-корутину
> (*“для данного скриншота `co_await visit_detect_expr` и верни найдено/нет”*), дожидается итога и
> возвращает его наружу.

### 8.4. `main.cpp` — структурная отмена вместо `CoroPool`

Раньше был `coro::CoroPool` + дочерняя корутина-наблюдатель сигналов, бросавшая `Interruption`.
Теперь — **гонка** двух корутин через `||`:

```cpp
using namespace asio::experimental::awaitable_operators;

auto watch_signals = [&]() -> asio::awaitable<void> {
    coro::SignalSet set({SIGINT, SIGTERM});
    while (true) {
        int signal = co_await set.wait();
        if ((signal == SIGINT) && REPL_mode_is_active) { REPL_mode_is_active = false; continue; }
        throw Interruption();
    }
};

auto do_work = [&]() -> asio::awaitable<int> {
    if (selected_mode == mode::clean) {
        co_return co_await clean_mode(clean_args);
    } else if (selected_mode == mode::run) {
        run_args.params_names.push_back("TESTO_HYPERVISOR");
        run_args.params_values.push_back(hypervisor);
        co_return co_await run_mode(run_args);
    } else {
        throw std::runtime_error("Unknown mode");
    }
};

int result = 0;
co_await ([&]() -> asio::awaitable<void> {
    auto r = co_await (do_work() || watch_signals());   // кто первый завершится
    result = std::get<0>(r);   // index 0 = do_work (watch_signals нормально не завершается)
}());
co_return result;
```

**Как проговаривать:**
> «Две корутины наперегонки: *основная работа* и *наблюдатель сигналов*.
> `co_await (do_work() || watch_signals())` — *“запусти обе, верни управление, когда завершится
> первая, вторую отмени”*.
> Нормально первой завершается `do_work` → берём `std::get<0>` (её `int`-результат); наблюдатель при
> этом отменяется (его `co_await set.wait()` тихо завершается `operation_aborted`).
> Если пришёл сигнал — `watch_signals` бросает `Interruption`, оно вылетает из `||`, отменяя
> `do_work`, и ловится в `main` → корректное завершение по Ctrl-C.»

Это и есть «переосмысление отмены»: вместо вброса исключения в чужой стек — **гонка + кооперативная
отмена проигравшей ветви** в её точках `co_await`.

### 8.5. Смешение sync/async в оркестровке теста

`src/testo/visitors/VisitorInterpreter.cpp` (подготовка контроллеров):

```cpp
if (controller->is_defined() &&            // sync
    controller->has_snapshot("_init") &&   // sync
    controller->check_metadata_version() &&
    controller->check_config_relevance())
{
    reporter.restore_snapshot(controller, "initial");  // sync (reporter)
    controller->restore_snapshot("_init");             // sync (libvirt)
    co_await coro::CheckPoint();                        // async: точка отмены
} else {
    reporter.create_controller(controller);            // sync
    controller->create();                              // sync (libvirt)
    reporter.take_snapshot(controller, "initial");     // sync
    controller->create_snapshot("_init", "", true);    // sync
    controller->current_state = "_init";
    co_await coro::CheckPoint();                        // async: точка отмены
}
```

**Как проговаривать:**
> «Почти всё здесь — *синхронные* вызовы libvirt и отчёта. Единственная «красная» точка —
> `co_await coro::CheckPoint()` в конце каждой ветви: *“я закончил тяжёлый синхронный шаг —
> дай циклу событий шанс доставить отмену, прежде чем я пойду дальше”*. Именно наличие этих
> `CheckPoint` делает функцию `awaitable` и заставляет вызывающих её `co_await`-ить.»

---

## 9. Методичка: как «читать и проговаривать» co_await-код

| Конструкция | Как проговорить про себя |
|---|---|
| `asio::awaitable<T> f(...)` | «*f — красная (приостанавливаемая) функция, отдаёт T*» |
| `co_await g();` | «*приостановись здесь, пока g не завершится; продолжи после* » |
| `T x = co_await g();` | «*дождись g, положи результат в x*» |
| `co_return x;` | «*заверши корутину, отдай x* (внутри корутины `return` запрещён)» |
| `co_return co_await g();` | «*дождись g и сразу верни её результат*» |
| `co_await with_timeout(d, [&]()->awaitable<void>{...});` | «*выполни блок, но не дольше d; иначе TimeoutError*» |
| `co_await coro::CheckPoint();` | «*точка, где меня можно кооперативно отменить*» |
| `co_await (a() \|\| b());` | «*гонка: жди первую, вторую отмени; результат — variant по индексу*» |
| `s.handle().connect(ep)` | «*синхронный connect (в конструкторе co_await нельзя)*» |
| вызов без `co_await` (reporter/VM/IR) | «*синхронный, на месте, не уступает поток*» |

Базовая эвристика при чтении функции:
1. Видишь `asio::awaitable<...>` в сигнатуре → функция приостанавливаема, её зовут через `co_await`.
2. Внутри ищи `co_await` — это **единственные** места, где функция может уступить поток
   (и где может прилететь отмена/таймаут как исключение).
3. Всё остальное (reporter, VM/libvirt, IR, парсер) — обычный синхронный код, исполняется на месте.

---

## 10. Верификация и ограничения

**Собрано, слинковано и запущено в этом окружении:**
- header-only shim `lib/coro` (отдельный smoke-тест: Timer, CheckPoint, with_timeout — оба исхода,
  Acceptor+Socket round-trip, операторы `||`/`&&`);
- весь стек `testo_guest_additions` — цели `testo_guest_additions_protocol`,
  `testo-guest-additions-cli`, `testo-guest-additions`.

**Проверено компилятором** (`g++ -std=c++20 -fsyntax-only`, без предупреждений о «discarded
awaitable»): все 4 визиторных трансляционных единицы, `NNClient`, `IR::Program`, `ModeRun`,
`ModeClean`, `Utils`, `ReportWriterNativeRemote` — т.е. **вся раскраска ядра**.

**НЕ проверено сборкой здесь:** бэкенды (`QemuVM`/`HyperVVM`/`Qemu*GuestAdditions`) и `main.cpp` —
они тянут `libvirt`/`libguestfs`, которых нет в окружении (сетевая политика блокирует apt-зеркала).
Изменения в них минимальны и механически повторяют **уже верифицированный** паттерн гостевых
дополнений (awaitable `send_raw/recv_raw` + синхронный `connect`; `sleep_for` вместо таймера).
Рекомендуется финальная сборка `testo` при наличии libvirt/guestfs.

---

## 11. Затронутые файлы (по слоям)

**Инфраструктура:** `CMakeLists.txt`, `3rd_party/CMakeLists.txt`, `src/CMakeLists.txt`,
`lib/CMakeLists.txt`, `3rd_party/asio*` (1.36.0), удалён `3rd_party/coro/`, добавлен `lib/coro/`,
`lib/posixapi/{File,Process}.hpp`.

**Невизиторный слой:** `src/testo_guest_additions_protocol/GuestAdditions.{hpp,cpp}`,
`src/testo_guest_additions/src/*` (daemon: Channel/MessageHandler/SharedFolder/MainLinux/CLI/каналы),
`src/testo/NNClient.{hpp,cpp}`, `src/testo_nn_server_protocol/Channel.hpp`,
`src/testo/report/ReportWriterNativeRemote.{hpp,cpp}`, `src/testo/IR/Program.{hpp,cpp}`,
`src/testo/IR/Macro.hpp`, `src/testo/Utils.cpp`, `src/testo/main/{main,ModeRun,ModeClean}.*`,
`src/testo/backends/qemu/*`, `src/testo/backends/hyperv/*`.

**Визиторный слой:** `src/testo/visitors/VisitorInterpreter.{hpp,cpp}`,
`VisitorInterpreterAction.{hpp,cpp}`, `VisitorInterpreterActionMachine.{hpp,cpp}`,
`VisitorInterpreterActionFlashDrive.{hpp,cpp}` (`VisitorSemantic.*` остался синхронным).
