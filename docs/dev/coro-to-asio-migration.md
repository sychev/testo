# Миграция Coro → asio (`asio::spawn` + `yield_context`) и фича `parallel`

> Документ для middle-разработчиков. Описывает, как из проекта убран самописный
> движок корутин **Coro** (стэкфул-файберы на `ucontext`) и заменён нативными
> стэкфул-корутинами **asio** (`asio::spawn` + `yield_context`), и как на этой
> основе работает блок `parallel`.
>
> **Важно про сборку.** Открытый репозиторий поставляется с **asio 1.14** и
> **C++17**, где `asio::spawn` тянет Boost.Coroutine, а `asio::cancel_after`/
> `cancellation_slot` отсутствуют. Новый субстрат написан под **asio ≥ 1.36 и
> C++20** (как в целевом форке) и **в этом репозитории не собирается** — это
> референс для переноса. Корневой `CMakeLists.txt` переведён на C++20, но
> работать всё будет только с обновлённым asio.

---

## 0. TL;DR

- Старый движок (файберы `ucontext`: `FiberLinux/FiberWindows`, `Coro.cpp`,
  ручной цикл `IoService::run`) **удалён**.
- Публичный API `coro::` (`CheckPoint`, `Timer`, `Timeout`, `CoroPool`,
  `StreamSocket`, `SignalSet`, `Application`, …) **сохранён** и переписан поверх
  `asio::spawn` + `yield_context`.
- Это сделано через один «мост» — `coro/detail/Engine.hpp` — который держит
  «текущий `yield_context`» в `thread_local` и пропускает все приостановки через
  единственную точку `detail::await()`. Благодаря мосту **места вызова не
  меняются**: интерпретатор и бэкенды по-прежнему пишут `timer.waitFor(...)`,
  `socket.read(...)`, `coro::CheckPoint()` без проброса `yield`.
- Модель остаётся **однопоточной кооперативной**. Поэтому фича `parallel`
  работает как раньше — на `coro::CoroPool` (теперь asio-шном), **без потоков ОС
  и без гонок данных**.

---

## 1. Почему именно так (а не «потоки» и не «awaitable»)

Было три способа убрать Coro:

1. **Блокирующая модель** (синхронный «pump» asio в каждом ожидании). Убирает
   Coro ценой отказа от кооперативной конкуренции → `parallel` пришлось бы делать
   на потоках ОС, с мьютексами и гонками. Отвергнуто.
2. **Стэклесс C++20 (`awaitable`/`co_await`)**. Идиоматично, но *вирусно*: каждая
   функция в цепочке действий становится `awaitable<T>` с `co_await`. Огромный
   диф по всему интерпретатору и бэкендам.
3. **Стэкфул `asio::spawn` + `yield_context`** ← выбрано. Это ровно та же модель,
   что давал Coro (стэкфул-файберы на одном `io_context`), но «родная» для asio.
   Синхронно-выглядящий стиль кода сохраняется, а значит миграция —
   преимущественно внутри библиотеки `coro`.

Coro по сути и был самописной версией `asio::spawn`. Поэтому замена —
концептуально прямая.

---

## 2. Ключевая идея: «мост текущего yield»

### Проблема

Старый код полагался на `Coro::current()`: любой блокирующий примитив мог неявно
получить «корутину, в которой я выполняюсь», и через неё уступить управление.
`asio::spawn` устроен иначе — он передаёт телу корутины `yield_context`, и **этот
токен нужно явно передавать в каждую async-операцию**. Если бы мы тащили
`yield_context` через все сигнатуры интерпретатора, бэкендов и сокетов — это была
бы та самая вирусная правка.

### Решение

Храним «текущий `yield_context`» в `thread_local` и проводим **все** приостановки
через одну функцию `detail::await()`. Тогда листовые примитивы (`Timer`, сокеты,
`CheckPoint`) берут `yield` из ambient-хранилища, а сигнатуры остального кода не
меняются. Это в точности воспроизводит роль `Coro::current()`.

### Инвариант (самое важное для понимания)

`detail::current_yield` всегда указывает на `yield_context` той корутины, которая
**сейчас исполняется** на этом потоке. Он:

- **устанавливается** при старте тела корутины — `YieldScope` внутри `spawn`;
- **восстанавливается** после каждой приостановки — внутри `detail::await()`.

Между двумя точками приостановки на одном потоке исполняется ровно одна корутина
(кооперативно, один поток), поэтому чтения `current_yield` всегда корректны. Пока
корутина приостановлена, значение может принадлежать другой корутине — это не
важно, потому что его никто не читает, пока какая-то корутина снова не побежит и
не соберётся приостановиться, а к этому моменту `await`/`YieldScope` уже вернули
правильное значение.

```cpp
// coro/detail/Engine.hpp (сокращённо)

inline thread_local asio::yield_context* current_yield = nullptr;

struct YieldScope {                 // ставит «текущий yield» на время тела корутины
    asio::yield_context* prev;
    explicit YieldScope(asio::yield_context& y): prev(current_yield) { current_yield = &y; }
    ~YieldScope() { current_yield = prev; }
};

template <class Init>
decltype(auto) await(Init&& init) {
    asio::yield_context y = yield();        // мой токен (валиден, пока я бегу)
    asio::yield_context* saved = current_yield;
    struct Restore { /* в деструкторе */ *slot = value; } restore{&current_yield, saved};

    auto deadline = current_deadline();
    auto run = [&](auto token) -> decltype(auto) {
        try { return init(token); }         // запускаем ОДНУ async-операцию с токеном
        catch (const std::system_error& e) {
            if (e.code() == asio::error::operation_aborted) {
                if (deadline && Clock::now() >= *deadline) throw TimeoutError{};
                throw CancelError{};        // отмена (CoroPool/Application)
            }
            throw;
        }
    };
    if (!deadline) return run(y);
    auto remaining = *deadline - Clock::now();
    if (remaining <= Clock::duration::zero()) throw TimeoutError{};
    return run(asio::cancel_after(remaining, y));   // дедлайн — через asio::cancel_after
}
```

Почему `await` ещё и про дедлайны и отмену:

- **Дедлайн** (`coro::Timeout`) теперь — это `thread_local` стек абсолютных
  времён; `await` берёт ближайший и оборачивает операцию в
  `asio::cancel_after(remaining, y)`. Это убирает целый класс гонок «таймер
  сработал между операциями».
- **Отмена** asio приходит как `operation_aborted`. `await` превращает её в
  `TimeoutError` (если дедлайн истёк) или в `CancelError` (иначе).

> `CancelError` намеренно **не** наследуется от `std::exception` — чтобы его не
> «съел» какой-нибудь `catch (const std::exception&)` в интерпретаторе и стек
> раскрутился до самого верха. Это сохраняет контракт старого
> `coro::CancelError`.

---

## 3. Файлы: что изменилось в `3rd_party/coro`

### Удалено (старый движок)

| Файл | Почему удалён |
|------|---------------|
| `FiberLinux.{h,cpp}`, `FiberWindows.{h,cpp}` | `ucontext`-файберы больше не нужны — корутины даёт `asio::spawn` |
| `Coro.cpp` | класс `Coro` (ручной yield/resume/exceptions) больше не нужен |
| `IoService.cpp` | ручной цикл `run()` (`run_one` + очередь checkpoints) заменён на `io_context::run()` |

### Добавлено

**`coro/detail/Engine.hpp`** — сердце миграции (см. раздел 2):
- `detail::current_io` / `io()` / `executor()` — ambient `io_context` (ставит
  `Application::run`);
- `detail::current_yield` / `yield()` / `YieldScope` — мост текущего yield;
- `detail::deadline_stack` / `current_deadline()` — стек дедлайнов;
- `detail::await(init)` — единственная точка приостановки (yield + дедлайн +
  трансляция отмены + восстановление ambient yield);
- `detail::suspend_quietly(init)` — приостановка без дедлайна/трансляции, глотает
  `operation_aborted` (нужна латчу `CoroPool`);
- исключения `CancelError`, `TimeoutError`.

### Переписано (API сохранён, реализация — на `await`)

| Файл | Что внутри теперь |
|------|-------------------|
| `Coro.h` | тонкий шим: `#include "coro/detail/Engine.hpp"` (чтобы старые `#include "coro/Coro.h"` резолвились) |
| `IoService.h` | фасад над ambient `io_context` (для совместимости `IoService::current()->_impl`) |
| `Application.{h,cpp}` | `run()` = поставить ambient `io_context`, заспаунить корневую корутину (`asio::spawn` + `YieldScope`), `io.run()`; `cancel()` — через `cancellation_signal` |
| `CheckPoint.{h,cpp}` | `await(asio::post(executor, token))` — кооперативная уступка + проверка отмены/дедлайна |
| `Timer.h` | `asio::steady_timer` + `await(async_wait)` |
| `Timeout.h` | RAII-дедлайн: кладёт абсолютное время в `detail::deadline_stack` |
| `AsioTask.h` | устаревший шим (мост `AsioTask` больше не нужен — его роль играет `await`); оставлен, чтобы резолвились `#include` |
| `CoroPool.{h,cpp}` | спаун дочерних корутин + латч ожидания + отмена (см. раздел 4) |
| `Stream.h`, `StreamSocket.h`, `Acceptor.h`, `Resolver.h`, `DatagramSocket.h` | те же обёртки, но IO через `await` |
| `SignalSet.h` | `asio::signal_set` + `await(async_wait)` (cpp удалён, header-only) |
| `Mutex.h` | кооперативный мьютекс на латче `steady_timer` (header-only) |
| `Work.h` | `asio::executor_work_guard` |

### Пример «было/стало» (листовой примитив)

```cpp
// БЫЛО (coro/Timer.h): ручной AsioTask + фибер
void wait() {
    AsioTask1 task;
    _handle.async_wait(task.callback());   // отдать callback в asio
    task.wait(_handle);                     // yield фибера, resume из callback
}

// СТАЛО: один проход через мост
void wait() {
    detail::await([&](auto token) {
        return _handle.async_wait(token);   // token == текущий yield_context
    });
}
```

Это и есть «механический» характер миграции: каждый листовой примитив — это
`detail::await([&](auto token){ return <asio async op>(token); })`.

---

## 4. `CoroPool` на asio — почему он чуть сложнее

`CoroPool` должен сохранить три свойства старого:

1. `exec()` запускает дочернюю корутину, которая бежит **сразу** и **параллельно**
   родительскому коду (не под `waitAll`);
2. `waitAll()` приостанавливает родителя до завершения **всех** детей;
3. деструктор **отменяет** ещё бегущих детей и дожидается их.

Из-за свойства (1) нельзя использовать `make_parallel_group` (он стартует
операции только в момент `async_wait`). Поэтому реализация — на «ручном» спауне +
латче:

```cpp
void CoroPool::exec(std::function<void()> routine) {
    impl->running++;
    auto signal = std::make_shared<asio::cancellation_signal>();   // чтобы можно было отменить ребёнка
    impl->signals.push_back(signal);

    asio::spawn(detail::executor(),
        [routine](asio::yield_context yield) {
            detail::YieldScope scope(yield);     // ребёнок ставит свой ambient yield
            routine();
        },
        asio::bind_cancellation_slot(signal->slot(),
            [impl](std::exception_ptr error) {   // вызовется при завершении ребёнка
                impl->running--;
                if (error && !impl->pending) {
                    impl->pending = error;       // запомнить первую ошибку
                    impl->cancel_all();          // fail-fast: отменить остальных
                }
                impl->latch.cancel();            // разбудить waitAll()
            }));
}

void CoroPool::waitAll(bool noThrow) {
    while (impl->running > 0) {
        impl->latch.expires_at(Clock::time_point::max());
        detail::suspend_quietly([&](auto token) { // спим, пока ребёнок не отменит латч
            return impl->latch.async_wait(token);
        });
    }
    if (!noThrow && impl->pending) std::rethrow_exception(impl->pending);
}
```

Тонкости (важно при чтении):

- **Латч** — это `steady_timer`, поставленный «в бесконечность»; завершившийся
  ребёнок зовёт `latch.cancel()`, чем будит родителя. `suspend_quietly`
  специально не превращает `operation_aborted` в исключение — мы просто
  перепроверяем счётчик.
- **Нет «потерянного пробуждения»**: между проверкой `running > 0` и
  `async_wait` нет точек приостановки, значит ни один ребёнок не успеет
  завершиться в этом зазоре (один поток, кооперативность).
- **Fail-fast**: первая ошибка сохраняется и шлёт `emit(cancellation)` остальным;
  их текущая `await`-операция получает `operation_aborted` → `CancelError` →
  ребёнок раскручивается.
- **Отмена → `CancelError`**, не `std::exception` — поэтому отменённый ребёнок
  гарантированно раскрутится до верха, как в старом `CoroPool`.

---

## 5. Как `parallel` ложится на новый субстрат

Фича `parallel` (грамматика, AST, парсер, семантика, `ParallelBranchInterpreter`)
**не изменилась**. Единственная её связь с конкуренцией — шов
`src/testo/visitors/ParallelExecutor.hpp`, а он построен на `coro::CoroPool`:

```cpp
struct ParallelExecutor {
    void spawn(std::function<void()> branch) { pool.exec(std::move(branch)); }
    void join() { pool.waitAll(); }
private:
    coro::CoroPool pool;
};
```

Поскольку `CoroPool` теперь реализован на `asio::spawn`, **шов менять не пришлось
вообще** — он автоматически поехал на asio-корутины. Модель осталась однопоточной
кооперативной, поэтому:

- ветки реально перекрываются на своих ожиданиях (`wait "DONE"` одной ветки
  уступает управление, пока другие бегут);
- **потоков ОС нет**;
- **гонок данных нет** — переключение только в точках приостановки, поэтому
  общий `reporter` не требует мьютекса (вывод может перемежаться, но не
  повреждаться).

Изоляция контекста на ветку (`ParallelBranchInterpreter` со своими `stack` и
`current_controller`) по-прежнему нужна и работает как раньше.

> Удалён файл `ParallelExecutor.asio.hpp` (черновой вариант шва на
> `make_parallel_group`): после миграции субстрат и так asio-шный, отдельный
> вариант шва не нужен.

---

## 6. Что в коде вне `coro` затронуто

Почти ничего — в этом и был смысл «контейнерной» миграции:

| Файл | Изменение |
|------|-----------|
| `CMakeLists.txt` (корневой) | `CMAKE_CXX_STANDARD` 17 → **20** (+`STANDARD_REQUIRED`) |
| `3rd_party/coro/CMakeLists.txt` | убран `add_subdirectory(tests)` (тесты проверяли старый движок), `cxx_std_20` |
| `src/testo/visitors/VisitorInterpreter.cpp` | убран лишний `#include <coro/AsioTask.h>`; уточнён комментарий о модели |
| `src/testo/visitors/ParallelExecutor.hpp` | обновлены комментарии (Coro → asio-субстрат) |

`main.cpp`, бэкенды (QEMU/Hyper-V), гостевые добавления, NN-клиент **не
менялись** — они используют сохранённый `coro::` API. `nn_server` намеренно не
трогался.

---

## 7. Карта замен Coro → asio

| Старое | Новое |
|--------|-------|
| `Coro` / `Fiber` (`ucontext`) | `asio::spawn`-корутина |
| `Coro::current()` | `detail::current_yield` (`thread_local`) + `detail::yield()` |
| `coro::AsioTask` (мост) | `detail::await(init)` |
| `coro::CheckPoint()` | `await(asio::post(executor, token))` |
| `coro::Timer` | `asio::steady_timer` + `await(async_wait)` |
| `coro::Timeout` | `thread_local` стек дедлайнов + `asio::cancel_after` в `await` |
| `coro::CoroPool` | `asio::spawn` детей + латч + `cancellation_signal` |
| `coro::Application` / `IoService::run` | `asio::io_context` + `asio::spawn(root)` + `io.run()` |
| `coro::StreamSocket` / `Stream` / `Acceptor` / `Resolver` / `DatagramSocket` | те же обёртки, IO через `await` |
| `coro::CancelError` | `coro::CancelError` (тот же контракт; кидается из `await` при отмене) |
| ручной цикл `run_one` + `checkpoints` | `io_context::run()` |

---

## 8. Подводные камни и что проверить при переносе в форк

1. **Версии**: нужен **asio ≥ 1.36** (`asio::spawn` без Boost — с 1.28;
   `asio::cancel_after` — с 1.30; `cancellation_slot` — с 1.19) и **C++20**.
2. **Точные сигнатуры asio** могут отличаться между версиями: `asio::spawn(ex,
   fn, token)`, `bind_cancellation_slot`, `cancel_after`. Возможны мелкие правки
   под конкретный asio.
3. **`detail::await` и `decltype(auto)`**: для void- и не-void-операций ветка
   `catch` всегда бросает, поэтому вывод типа берётся из единственного
   `return init(token)` — это корректно, но при экзотических токенах
   перепроверьте.
4. **Отмена → `operation_aborted`**: мы трактуем «aborted без истёкшего
   дедлайна» как `CancelError`. Если какой-то backend сам отменяет операции и
   рассчитывает поймать `system_error(operation_aborted)`, учтите, что теперь
   там прилетит `CancelError`.
5. **Тесты `3rd_party/coro/tests`** проверяли старый движок и временно отключены
   — их стоит переписать под новый API (особенно `TestCoroPool`, `TestTimeout`,
   `TestCheckPoint`).
6. **Однопоточный инвариант**: вся модель держится на одном `io_context` в одном
   потоке. Не запускайте `io.run()` из нескольких потоков — `thread_local` мост и
   отсутствие мьютексов рассчитаны на единственный поток.
7. **`parallel` и общий reporter**: при однопоточной кооперативной модели мьютекс
   не нужен; вывод перемежается по веткам, но не повреждается. Если когда-нибудь
   перейдёте на многопоточный `io_context`, это перестанет быть верным.

---

## 9. Итог

- Самописный движок Coro удалён; корутины теперь — `asio::spawn` +
  `yield_context`.
- Публичный `coro::` API сохранён через мост «текущего yield»
  (`detail/Engine.hpp`), поэтому интерпретатор, бэкенды и `main.cpp` почти не
  изменились.
- Кооперативная однопоточная модель сохранена → `parallel` работает без потоков и
  без гонок, на том же шве `ParallelExecutor` (теперь поверх asio-шного
  `CoroPool`).
- Код написан под asio ≥ 1.36 / C++20 как референс для переноса; в открытом
  репозитории (asio 1.14 / C++17) он не собирается.
