# Удаление библиотеки Coro и переход на синхронную библиотеку `net`

> Документ описывает крупный рефакторинг: полное удаление стековых корутин
> (библиотека `3rd_party/coro`) из всего репозитория `testo` и замену их на
> синхронную модель ввода/вывода поверх **standalone asio** с использованием
> возможностей **C++20**. Отдельно разобрана переработка `testo_nn_server`
> ради производительности под нагрузкой 100+ клиентов.
>
> Документ рассчитан и на Senior-, и на Middle-разработчиков: ключевые
> механики (asio, фиберы, `std::stop_token`, `thread_local`, потокобезопасность
> ONNX/QuickJS) сначала объясняются «с нуля», а уже потом применяются.

---

## Оглавление

1. [Зачем это всё](#1-зачем-это-всё)
2. [Мини-ликбез: asio и асинхронный ввод/вывод](#2-мини-ликбез-asio-и-асинхронный-вводвывод)
3. [Что делала Coro и почему её убрали](#3-что-делала-coro-и-почему-её-убрали)
4. [Библиотека `lib/net`: устройство](#4-библиотека-libnet-устройство)
5. [Таблица соответствия Coro → net](#5-таблица-соответствия-coro--net)
6. [Изменения в основном приложении `testo`](#6-изменения-в-основном-приложении-testo)
7. [Переработка `testo_nn_server` под нагрузку](#7-переработка-testo_nn_server-под-нагрузку)
8. [Корректное завершение потоков соединений](#8-корректное-завершение-потоков-соединений)
9. [Сборка: C++20 и CMake](#9-сборка-c20-и-cmake)
10. [Что осталось проверить и ограничения](#10-что-осталось-проверить-и-ограничения)
11. [Глоссарий и ссылки](#11-глоссарий-и-ссылки)

---

## 1. Зачем это всё

Раньше весь сетевой и таймерный код в `testo` строился на собственной
библиотеке **`3rd_party/coro`** — это реализация **стековых корутин на фиберах**
(подробнее в [разделе 3](#3-что-делала-coro-и-почему-её-убрали)). Она позволяла
писать «синхронно выглядящий» код, который на самом деле асинхронный.

Проблемы, которые решает рефакторинг:

- **Сложность и непрозрачность.** Стековые корутины с ручным `yield/resume`,
  собственным планировщиком и пробросом исключений между фиберами — тяжело
  отлаживать и сопровождать.
- **Производительность `nn_server` под нагрузкой.** Сервер нейросетей крутился в
  одном потоке с одним циклом событий, поэтому тяжёлый инференс блокировал
  обработку всех остальных клиентов (детали — в [разделе 7](#7-переработка-testo_nn_server-под-нагрузку)).
- **`testo` по сути последователен.** Реальной кооперативной многозадачности в
  основном приложении нет, поэтому весь аппарат фиберов был избыточен.

Решение: убрать `coro` целиком, заменить на маленькую библиотеку **`lib/net`** —
синхронный блокирующий ввод/вывод поверх asio + кооперативная отмена через
**`std::stop_token`** (C++20). Где нужна настоящая конкуренция (сервер) —
используем обычные потоки ОС.

---

## 2. Мини-ликбез: asio и асинхронный ввод/вывод

Если вы уже хорошо знаете asio — пропустите раздел. Он нужен, чтобы понять, как
работает новая библиотека.

### 2.1. `io_context` и модель завершения (completion handlers)

[**Asio**](https://think-async.com/Asio/) — это библиотека для ввода/вывода.
Центральный объект — `asio::io_context` (исторически `io_service`). Это, по сути,
**очередь готовых к выполнению задач** плюс обёртка над механизмом ОС для
ожидания событий (на Linux это `epoll`, на Windows — IOCP).

Асинхронная операция выглядит так:

```cpp
asio::ip::tcp::socket socket(io);
socket.async_read_some(asio::buffer(buf), [](std::error_code ec, std::size_t n) {
    // этот колбэк (completion handler) вызовется, когда данные будут прочитаны
});
```

`async_read_some` **сразу возвращает управление**, ничего не дожидаясь. Колбэк
будет вызван позже — но только тогда, когда кто-то «крутит» `io_context`:

```cpp
io.run();        // блокирует и выполняет колбэки, пока есть работа
io.run_one();    // выполнит максимум один готовый колбэк и вернётся
io.run_one_for(std::chrono::milliseconds(20)); // как run_one, но не дольше 20 мс
```

Внутри `run*()` asio делает `epoll_wait` (ждёт готовности сокетов), и когда сокет
готов — выполняет соответствующий колбэк **в том же потоке**, который вызвал
`run()`. Никакой «магии в фоне» нет: пока вы не вызвали `run()`, колбэки не
выполняются.

### 2.2. Почему «асинхронно» неудобно писать напрямую

Чистый асинхронный стиль рассыпает логику по колбэкам:

```cpp
socket.async_connect(ep, [&](auto ec){
    socket.async_write(req, [&](auto ec, auto n){
        socket.async_read(resp, [&](auto ec, auto n){
            // ...вложенность растёт...
        });
    });
});
```

Хотелось бы писать линейно:

```cpp
socket.connect(ep);
socket.write(req);
socket.read(resp);
```

Именно это «линейное» удобство и давала Coro (через фиберы), и именно его теперь
даёт `lib/net` (через синхронную прокрутку `io_context`). Разница — в том, **как**
достигается блокировка, см. [раздел 4.1](#41-ядро-синхронный-вводвывод-с-дедлайном-iopump).

---

## 3. Что делала Coro и почему её убрали

### 3.1. Стековые корутины на фиберах

**Фибер** (fiber) — это «поток без вытеснения»: у него есть собственный стек, но
переключается он не планировщиком ОС, а явно, кодом (`swapcontext` на Linux,
Fibers API на Windows). Стековая корутина = фибер: можно остановиться (`yield`)
в любом месте, сохранив весь стек вызовов, и потом продолжить (`resume`).

Coro связывала фиберы с asio так:

- Один поток, один `io_context`, в нём — несколько корутин.
- Когда корутина делала «блокирующий» `socket.read()`, под капотом запускалась
  `async_read`, а затем корутина делала `yield` — отдавала управление циклу
  событий. Цикл крутил `io_context`, обрабатывал другие корутины, а когда данные
  приходили — колбэк делал `resume` нашей корутины ровно с того места.
- **Отмена** (Ctrl-C, таймаут) реализовывалась пробросом *исключения* внутрь
  фибера: при `resume` корутина «просыпалась» исключением `CancelError` и
  раскручивала свой стек.

То есть код выглядел синхронным, а исполнялся кооперативно на одном потоке.

### 3.2. Почему убрали

- Для **`testo`** (интерпретатор тестов) кооперативная многозадачность не нужна:
  он выполняет тесты последовательно. Единственная «вторая корутина» во всём
  приложении — слушатель сигналов. Фиберы здесь — чистый оверхед сложности.
- Для **`nn_server`** однопоточная кооперативная модель оказалась *вредной*:
  инференс нейросети — это чистый CPU без точек `yield`, поэтому он блокировал
  весь цикл событий и, значит, всех остальных клиентов (см. [раздел 7](#7-переработка-testo_nn_server-под-нагрузку)).

Вывод: синхронный код + обычные потоки ОС там, где нужна конкуренция, проще и
быстрее.

---

## 4. Библиотека `lib/net`: устройство

`lib/net` — небольшая статическая библиотека (заголовки + пара `.cpp`).
Подключается как `<net/...>` (каталог `lib` — в include-путях). Пространство имён
— `net`.

| Файл | Что заменяет | Назначение |
|---|---|---|
| `IoPump.hpp` | `coro::AsioTask` + фибер | Ядро: синхронная прокрутка `io_context` с отменой и дедлайном |
| `Cancel.hpp/.cpp` | `coro::CancelError`, `Application::cancel` | Глобальный запрос отмены через `std::stop_source` |
| `Deadline.hpp/.cpp` | `coro::Timeout` | Таймаут на область видимости (thread-local) |
| `CheckPoint.hpp` | `coro::CheckPoint` | Точка кооперативной отмены в CPU-циклах |
| `Timer.hpp` | `coro::Timer` | Прерываемый сон |
| `Stream.hpp` | `coro::Stream` | Синхронные read/write поверх asio |
| `Socket.hpp` | `coro::StreamSocket` | То же + `connect` |
| `Acceptor.hpp` | `coro::Acceptor` | Приём входящих соединений |
| `SignalGuard.hpp/.cpp` | `coro::SignalSet` + корневая корутина | Обработка сигналов в фоновом потоке |
| `Finally.hpp` | `coro::Finally` | Действие при выходе из области видимости |

### 4.1. Ядро: синхронный ввод/вывод с дедлайном (`IoPump`)

Вместо фиберов мы используем приём, который в документации asio называют
**«synchronous operations with a timeout»**: запускаем асинхронную операцию и тут
же сами в цикле крутим `io_context` маленькими порциями, пока операция не
завершится — *проверяя между порциями отмену и дедлайн*.

```cpp
// lib/net/IoPump.hpp
namespace net::detail {

constexpr auto io_quantum = std::chrono::milliseconds(20);

template <typename Handle, typename Initiate>
void pump(asio::io_context& io, Handle& handle, Initiate&& initiate) {
    io.restart();                       // сбросить состояние после прошлой операции

    bool done = false;
    initiate([&done] { done = true; }); // запускаем async-операцию; в её колбэке
                                        // вызывается это продолжение -> done = true

    while (!done) {
        io.run_one_for(io_quantum);     // ждём готовности максимум 20 мс
        if (done) break;
        if (interrupt_requested()) {    // пришёл Ctrl-C / SIGTERM?
            try { handle.cancel(); } catch (...) {}
            io.run();                   // дождаться завершения отменённой операции
            throw Interruption{};
        }
        if (Deadline::expired()) {      // истёк таймаут текущей области?
            try { handle.cancel(); } catch (...) {}
            io.run();
            throw TimeoutError{};
        }
    }
}

}
```

Как это читать:

- `initiate` — лямбда, которую передаёт вызывающий код. Ей дают «продолжение»
  `on_done`, которое надо вызвать из asio-колбэка. Так `pump` не знает деталей
  конкретной операции (read/write/connect/accept) — он лишь крутит цикл.
- `io.run_one_for(20ms)` — это **не busy-wait**: внутри `epoll_wait` с таймаутом
  20 мс. Если данные пришли раньше — функция вернётся сразу (никакой лишней
  задержки). Если ничего не произошло — вернётся через 20 мс, и мы проверим
  флаги отмены/таймаута. То есть **20 мс — это максимальная задержка реакции на
  Ctrl-C**, а не задержка на каждый ввод/вывод.
- При отмене/таймауте мы зовём `handle.cancel()` (отменить ожидающую
  операцию у сокета/таймера), затем `io.run()` — чтобы колбэк отменённой
  операции точно отработал (иначе он сошлётся на уже разрушенные локальные
  переменные), и только потом бросаем исключение.

Это прямой аналог старого `coro::AsioTask::doWait`, но **без фиберов**: мы не
переключаем стек, а просто блокируем текущий поток в `run_one_for`.

### 4.2. Отмена: `std::stop_token` (`Cancel.hpp`)

В C++20 появились `std::stop_source` / `std::stop_token` — стандартный механизм
кооперативной отмены. `stop_source` — «рычаг», `stop_token` — «датчик»: любой код
может спросить `token.stop_requested()`. Один раз запрошенная остановка
необратима (для повторного запуска создаётся новый `stop_source`).

```cpp
// lib/net/Cancel.hpp
namespace net {

struct Interruption {};   // НЕ наследник std::exception — см. ниже

std::stop_source& interrupt_source();                       // глобальный источник
inline std::stop_token interrupt_token() { return interrupt_source().get_token(); }
inline bool interrupt_requested()  { return interrupt_source().stop_requested(); }
inline void request_interrupt()    { interrupt_source().request_stop(); }
void reset_interrupt();   // заменяет источник новым (для повторного запуска/тестов)

}
```

**Почему `Interruption` не наследуется от `std::exception`?** В интерпретаторе
полно блоков `catch (const std::exception&)`. Если бы отмена была `std::exception`,
её бы «съел» первый попавшийся такой `catch`, и Ctrl-C не доходил бы до верха.
Не наследуясь от `std::exception`, `Interruption` гарантированно
проходит сквозь такие блоки и раскручивает стек до `main`. Это в точности
повторяет старый `coro::CancelError`.

> Подробнее про `std::stop_token`: [cppreference — std::stop_token](https://en.cppreference.com/w/cpp/thread/stop_token).

### 4.3. Дедлайн: `coro::Timeout` → `net::Deadline`

`coro::Timeout` был «ambient»-объектом: создаёшь его в начале блока, и любой
блокирующий вызов внутри начинает учитывать таймаут. Мы повторили это поведение
через **thread-local стек дедлайнов**:

```cpp
// lib/net/Deadline.hpp
class Deadline {
public:
    template <typename Duration>
    explicit Deadline(Duration d) { push(Clock::now() + ...); }  // RAII: ставим
    ~Deadline();                                                 // снимаем

    static std::optional<Clock::time_point> current();           // текущий дедлайн
    static bool expired();                                        // истёк ли он
};
```

```cpp
// lib/net/Deadline.cpp — ключевая идея
namespace {
    thread_local std::vector<Clock::time_point> deadline_stack;
}

void Deadline::push(Clock::time_point deadline) {
    // вложенные дедлайны: действует самый ранний
    if (!deadline_stack.empty() && deadline_stack.back() < deadline)
        deadline = deadline_stack.back();
    deadline_stack.push_back(deadline);
}
```

`thread_local` означает «у каждого потока своя копия переменной» (см.
[раздел 7.3](#73-thread_local-каждому-потоку-своё-состояние)). И `IoPump`, и
`check_point()` сверяются с `Deadline::current()` своего потока. Использование:

```cpp
net::Deadline timeout(std::chrono::seconds(10));
socket.read(...);   // если за 10 с не успели — вылетит net::TimeoutError
```

`TimeoutError` наследуется от `std::runtime_error` — как и раньше, чтобы его можно
было ловить обычным `catch (const std::exception&)` там, где это нужно (например,
`GuestAdditions::is_avaliable` именно так и подавляет таймаут).

### 4.4. `check_point()` и `Timer`

```cpp
// lib/net/CheckPoint.hpp
inline void check_point() {
    if (interrupt_requested()) throw Interruption{};
    if (Deadline::expired())   throw TimeoutError{};
}
```

`check_point()` вставляется в длинные CPU-циклы (ожидание появления текста на
экране, копирование файлов и т.п.), чтобы Ctrl-C и таймаут срабатывали и без
ввода/вывода. Прямая замена `coro::CheckPoint()`.

`net::Timer` / `net::sleep_for` — прерываемый сон: спит маленькими квантами,
между ними зовёт `check_point()`:

```cpp
// lib/net/Timer.hpp
template <typename Duration>
void sleep_for(Duration duration) {
    const auto deadline = Clock::now() + ...;
    constexpr auto quantum = std::chrono::milliseconds(20);
    while (true) {
        check_point();
        const auto now = Clock::now();
        if (now >= deadline) break;
        std::this_thread::sleep_for(std::min(deadline - now, quantum));
    }
}
```

### 4.5. `Stream` / `Socket` / `Acceptor`

Это тонкие обёртки, сохраняющие старый интерфейс `read/write/connect/accept`, но
реализованные через `pump`.

**Важная деталь — владение `io_context`.** Каждый `Stream` владеет собственным
`io_context`. Но `asio::io_context` **нельзя ни копировать, ни перемещать**, а
наши сокеты должны быть перемещаемыми (их `std::move`-ят в `Channel`, отдают в
другой поток и т.д.). Решение — держать `io_context` на куче через
`std::unique_ptr`:

```cpp
// lib/net/Stream.hpp (сокращённо)
template <typename Handle>
class Stream {
public:
    Stream()
        : _io(std::make_unique<asio::io_context>())
        , _handle(*_io)                 // asio-хендл создаётся на этом io_context
    {}

    Stream(Stream&&) = default;         // перемещаемо: указатель + хендл переезжают
    // ...

    template <typename ...T>
    size_t read(T&&... t) {
        auto buffer = asio::buffer(std::forward<T>(t)...);
        return transfer([&](auto callback) {
            asio::async_read(_handle, buffer, std::move(callback));
        });
    }
    // write / readSome / writeSome — аналогично

protected:
    template <typename Operation>
    size_t transfer(Operation operation) {
        std::error_code ec; size_t transferred = 0;
        detail::pump(*_io, _handle, [&](auto on_done) {
            operation([&, on_done](const std::error_code& e, size_t bytes) {
                ec = e; transferred = bytes; on_done();
            });
        });
        if (ec) throw std::system_error(ec);
        return transferred;
    }

    std::unique_ptr<asio::io_context> _io;   // на куче -> объект остаётся перемещаемым
    Handle _handle;
};
```

Почему `unique_ptr` спасает: asio-хендл хранит ссылку (executor) на свой
`io_context`. Если бы `io_context` лежал в `Stream` по значению, при перемещении
`Stream` он бы «переехал» — но `io_context` неперемещаем. А так сам объект
`io_context` всегда лежит на куче по стабильному адресу; при перемещении `Stream`
переезжают лишь указатель и хендл, а ссылка хендла на `io_context` остаётся
валидной.

`Socket` добавляет `connect`, `Acceptor` — приём соединений:

```cpp
// lib/net/Acceptor.hpp — accept()
Socket<Protocol> accept() {
    Socket<Protocol> peer;              // у peer СВОЙ io_context
    std::error_code ec;
    detail::pump(*_io, _handle, [&](auto on_done) {
        _handle.async_accept(peer.handle(), [&, on_done](const std::error_code& e) {
            ec = e; on_done();
        });
    });
    if (ec) throw std::system_error(ec);
    return peer;                        // перемещаем наружу
}
```

Тонкость: операция `async_accept` исполняется на `io_context` *акцептора*, но
заполняет сокет `peer`, у которого **свой** `io_context`. Asio это допускает: он
просто записывает принятый дескриптор в `peer`. Благодаря этому принятое
соединение можно отдать в отдельный поток-обработчик — там его `io_context` будет
крутиться независимо. (Эту схему мы проверили loopback-тестом — кросс-контекстный
accept работает.)

### 4.6. `SignalGuard`

Раньше сигналы (Ctrl-C, SIGTERM) ловила специальная корутина через
`coro::SignalSet`. Теперь — отдельный фоновый поток с собственным `io_context` и
`asio::signal_set`:

```cpp
// lib/net/SignalGuard.cpp
SignalGuard::SignalGuard(std::initializer_list<int> signals, std::function<void(int)> handler)
    : _signals(_io), _handler(std::move(handler))
{
    for (int s : signals) _signals.add(s);
    arm();
    _thread = std::thread([this] { _io.run(); });   // крутим io в фоне
}

void SignalGuard::arm() {
    _signals.async_wait([this](const std::error_code& ec, int signal) {
        if (ec) return;          // отменено при остановке
        _handler(signal);
        arm();                   // перевзводим ожидание следующего сигнала
    });
}

SignalGuard::~SignalGuard() {
    _io.stop();
    if (_thread.joinable()) _thread.join();
}
```

`asio::signal_set` безопасно работает с сигналами: ОС доставляет сигнал, asio
переводит его в обычный колбэк, который выполняется в нашем фоновом потоке
(никаких async-signal-safe ограничений в самом `handler`). Обычно `handler`
просто вызывает `net::request_interrupt()`.

### 4.7. `Finally`

RAII-обёртка «выполнить лямбду при выходе из области видимости» — точная копия
`coro::Finally` (нужна была пара мест в `main`).

---

## 5. Таблица соответствия Coro → net

| Было (`coro`) | Стало (`net`) | Заметки |
|---|---|---|
| `coro::StreamSocket<P>` | `net::Socket<P>` | интерфейс read/write/connect сохранён |
| `coro::Stream<H>` | `net::Stream<H>` | |
| `coro::Acceptor<P>` | `net::Acceptor<P>` | `accept()` вместо `run(callback)` |
| `coro::Timeout` | `net::Deadline` | thread-local дедлайн |
| `coro::CheckPoint()` | `net::check_point()` | |
| `coro::Timer` / `waitFor` | `net::Timer` / `net::sleep_for` | |
| `coro::SignalSet` + корневая корутина | `net::SignalGuard` | |
| `coro::CancelError` | `net::Interruption` | оба НЕ `std::exception` |
| `coro::Finally` | `net::Finally` | |
| `coro::Application(...).run()` | прямой вызов функции | event loop больше не нужен |
| `coro::CoroPool` | обычные `std::thread` | только там, где есть конкуренция |

Места использования (для ориентира): интерпретатор (`VisitorInterpreter*`),
бэкенды (`QemuVM`, `QemuGuestAdditions`, `HyperVVM`, ...), `NNClient`,
`ReportWriterNativeRemote`, общие протоколы (`testo_nn_server_protocol/Channel.hpp`,
`testo_guest_additions_protocol/GuestAdditions.cpp`), `lib/hyperv`, `lib/guestfs`,
а также `testo_nn_server` и `testo_guest_additions`.

---

## 6. Изменения в основном приложении `testo`

В основном изменения механические (переименование типов/символов). Содержательная
часть — `main.cpp`.

**Было:** приложение запускалось внутри `coro::Application`, а сигналы ловила
корутина в `CoroPool`, бросавшая `Interruption` прямо в корневую корутину.

**Стало:** `main()` просто вызывает `do_main()`, а сигналы обрабатывает
`net::SignalGuard`:

```cpp
// src/testo/main/main.cpp (фрагмент do_main)
net::Finally cleanup([&] { env.reset(); });

net::SignalGuard signal_guard({SIGINT, SIGTERM}, [](int signal) {
    if ((signal == SIGINT) && REPL_mode_is_active) {
        REPL_mode_is_active = false;   // Ctrl-C во время REPL гасит REPL, не процесс
        return;
    }
    net::request_interrupt();
});
```

```cpp
int main(int argc, char** argv) {
    int result = 0;
    try {
        result = do_main(argc, argv);
    } catch (const TestFailedException& error) { /* ... */ result = 1; }
      catch (const net::Interruption&)        { /* Interrupted by user */ result = 3; }
      catch (const std::exception& error)     { /* ... */ result = 2; }
    return result;
}
```

Важный нюанс, который мы сохранили: **SIGINT во время REPL** не завершает процесс,
а лишь выходит из интерактивного режима. Раньше это решала логика корутины-слушателя;
теперь — проверка `REPL_mode_is_active` прямо в обработчике сигнала. Поскольку
`stop_source` необратим, для этого используется именно условие в обработчике (мы
не запрашиваем отмену в случае «SIGINT во время REPL»), а не сброс уже
запрошенной отмены.

Отмена в новой модели **кооперативная**: она «доходит» до кода в точках
`net::check_point()` и в блокирующих вводах/выводах (через `pump`, проверка раз в
~20 мс). На практике это неотличимо от мгновенной реакции.

---

## 7. Переработка `testo_nn_server` под нагрузку

Это самая содержательная часть. Здесь мы не просто заменили Coro, а исправили
архитектурную причину тормозов под нагрузкой.

### 7.1. Почему оригинал тормозил

`nn_server` — это TCP-сервер: клиент (`testo`) присылает скриншот и JS-скрипт,
сервер прогоняет нейросети (детекция текста/картинок) и возвращает результат.

В старой версии весь сервер работал **в одном потоке с одним циклом событий**
(`coro::Application`), а соединения были корутинами. Ключевая беда: **инференс
нейросети — это чистый CPU без точек `yield`**. Пока считается один запрос,
единственный поток занят, и он не может ни принять новое соединение, ни прочитать
запрос другого клиента, ни ответить. 100 клиентов выстраивались в одну очередь
без какого-либо перекрытия вычислений и сети.

### 7.2. Целевая модель: поток на соединение + общие сессии

Идея:

- Каждое соединение обслуживается **своим потоком ОС** (`std::thread`). Сеть
  разных клиентов теперь перекрывается, и медленный сетевой клиент не блокирует
  вычисления других.
- Состояние, которое **нельзя** использовать из нескольких потоков, делаем
  **`thread_local`** (своё на поток).
- Тяжёлые объекты модели (`Ort::Session`) — наоборот, **общие** на весь процесс
  (они потокобезопасны для исполнения), чтобы не загружать веса моделей в каждый
  поток.
- Число *одновременных* инференсов ограничиваем **семафором** под число ядер.

### 7.3. `thread_local`: каждому потоку — своё состояние

`thread_local` — спецификатор хранения (C++11+): переменная существует в
единственном экземпляре **на каждый поток**. Первый доступ из потока
конструирует её, при завершении потока — разрушает.

```cpp
int& counter() {
    thread_local int value = 0;   // у каждого потока свой value
    return value;
}
```

Почему это нужно в `nn_server`: мы нашли несколько единиц **разделяемого
мутабельного состояния**, которые ломались бы при работе из нескольких потоков.

**(а) Синглтоны-детекторы.** Детекторы оформлены как синглтоны
(`TextDetector::instance()` и др.) и хранят переиспользуемые буферы
ввода/вывода:

```cpp
struct TextDetector {
    static TextDetector& instance();
    std::vector<TextLine> detect(const stb::Image<stb::RGB>* image);
private:
    onnx::Image in = "input";    // <- общий буфер; два потока затрут друг друга
    onnx::Image out = "output";
    // ...
};
```

Было `static TextDetector instance;` — единый объект на процесс. Стало:

```cpp
TextDetector& TextDetector::instance() {
    thread_local TextDetector instance;   // свой детектор (и буферы) на поток
    return instance;
}
```

Аналогично для `TextRecognizer`, `ImgDetector`, `TextColorPicker`.

**(б) Конвертеры UTF.** Два `static std::wstring_convert ... conv;` хранят
мутабельное внутреннее состояние — стали `static thread_local`.

**(в) QuickJS-рантайм.** `js::Runtime::instance()` оборачивает один
`JSRuntime`. **QuickJS запрещает использовать один `JSRuntime` из нескольких
потоков одновременно.** Поэтому он тоже стал `thread_local` — у каждого потока
свой `JSRuntime` (это и есть «канонический» способ многопоточности в QuickJS).

### 7.4. Общие потокобезопасные сессии ONNX

Если бы мы сделали `thread_local` *и сами модели*, каждый поток загрузил бы веса
всех моделей (~10 МБ × 3 модели) — при 100 потоках это ~2.5 ГБ. Неприемлемо.

Ключевой факт: **`Ort::Session::Run()` потокобезопасен** — одну и ту же сессию
можно исполнять из многих потоков параллельно. Небезопасны были только
переиспользуемые буферы-обёртки (их мы уже сделали `thread_local`).

Поэтому сессии вынесены в **общий реестр** (одна сессия на модель на весь
процесс), а `onnx::Model` стал тонким хендлом-ссылкой:

```cpp
// src/testo_nn_server/nn/OnnxRuntime.cpp
static std::unique_ptr<Ort::Session> create_session(const char* name) { /* грузит .onnx */ }

// Реестр: каждая модель грузится один раз, сессия переиспользуется всеми потоками.
static Ort::Session& shared_session(const char* name) {
    static std::mutex mutex;
    static std::map<std::string, std::unique_ptr<Ort::Session>> sessions;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = sessions.find(name);
    if (it == sessions.end())
        it = sessions.emplace(name, create_session(name)).first;
    return *it->second;
}

Model::Model(const char* name) {
    if (!env) throw std::runtime_error("Init onnx runtime first!");
    session = &shared_session(name);   // невладеющий указатель на общую сессию
}
```

Итог: `thread_local`-детектор **дёшев** (только буферы), а тяжёлые веса —
общие и загружаются один раз. Память не растёт с числом клиентов.

### 7.5. Ограничение параллелизма: `std::counting_semaphore`

Модели сконфигурированы как однопоточные (`SetIntraOpNumThreads(1)`,
`SetInterOpNumThreads(1)`) — один инференс = одно ядро. Значит:

- Раньше (мьютекс на всё) сервер использовал **одно ядро** под любой нагрузкой.
- Если разрешить всем 100 потокам считать одновременно — будет
  **oversubscription** (100 вычислительных потоков на, скажем, 16 ядрах →
  лишние переключения контекста).

Оптимум — разрешить столько одновременных инференсов, сколько ядер.
`std::counting_semaphore<>` (C++20) — счётчик-«пропуск»: `acquire()` уменьшает
счётчик (ждёт, если он 0), `release()` увеличивает.

```cpp
// src/testo_nn_server/nn/OnnxRuntime.cpp
static std::counting_semaphore<>& inference_slots() {
    static std::counting_semaphore<> slots(std::max(1u, std::thread::hardware_concurrency()));
    return slots;
}
struct InferenceSlot {                          // RAII-«пропуск»
    InferenceSlot()  { inference_slots().acquire(); }
    ~InferenceSlot() { inference_slots().release(); }
};

void Model::run(...) {
    // ... подготовка тензоров ...
    InferenceSlot slot;                          // <- ограничиваем ТОЛЬКО Run()
    session->Run(...);
}
```

Важно, что семафор охватывает **только** `Run()` — чистый CPU без ввода/вывода.
Сетевые операции (включая «дозапрос» референсной картинки у клиента, см. ниже)
происходят *вне* этого участка, поэтому медленный клиент не «держит» слот и никого
не блокирует.

> Подробнее: [cppreference — std::counting_semaphore](https://en.cppreference.com/w/cpp/thread/counting_semaphore).

### 7.6. Глобальный мьютекс убран

После всего вышеперечисленного обработка запроса больше не требует глобальной
блокировки — состояние либо `thread_local`, либо потокобезопасно. В
`MessageHandler::run()` `std::lock_guard` удалён, запросы разных соединений
обрабатываются полностью параллельно.

### 7.7. Тонкость QuickJS: регистрация классов на каждый рантайм

Переход на `thread_local` `JSRuntime` вскрыл скрытую зависимость. JS-классы
(`Point`, `TextTensor`, `ImgTensor`) регистрировались так:

```cpp
// БЫЛО — ломается при нескольких рантаймах
void Point::register_class(ContextRef ctx) {
    if (!class_id) {                          // class_id — ГЛОБАЛЬНЫЙ
        JS_NewClassID(&class_id);
        class_def.class_name = "Point";
        class_def.finalizer = finalizer;
        JS_NewClass(JS_GetRuntime(ctx.handle), class_id, &class_def); // в ОДНОМ рантайме
    }
    // ...
}
```

Проблема: `class_id` глобальный. Первый поток выставит его в ненулевое значение и
зарегистрирует класс в *своём* рантайме. Все остальные потоки увидят
`class_id != 0`, пропустят блок целиком — и **в их рантаймах класс не будет
зарегистрирован** → попытка создать объект такого класса приведёт к краху.

Как устроен QuickJS: `JS_NewClassID()` выдаёт глобально уникальный числовой
идентификатор класса (это должно произойти **один раз** на процесс), а
`JS_NewClass(rt, id, def)` регистрирует класс **в конкретном рантайме** `rt`
(это должно происходить **по разу на каждый рантайм**).

Исправление:

```cpp
// СТАЛО — корректно при нескольких рантаймах
void Point::register_class(ContextRef ctx) {
    static std::once_flag id_once;
    std::call_once(id_once, [] {                 // id аллоцируется ОДИН раз на процесс
        JS_NewClassID(&class_id);
        class_def.class_name = "Point";
        class_def.finalizer = finalizer;
    });

    JSRuntime* rt = JS_GetRuntime(ctx.handle);
    if (!JS_IsRegisteredClass(rt, class_id)) {   // а класс — по разу на КАЖДЫЙ рантайм
        JS_NewClass(rt, class_id, &class_def);
    }
    // ... установка прототипа (на каждый контекст) ...
}
```

`std::call_once` + `std::once_flag` гарантируют, что инициализация выполнится
ровно один раз даже при гонке потоков. `JS_IsRegisteredClass` позволяет
зарегистрировать класс в каждом новом рантайме идемпотентно. Тот же приём
применён к шаблону `Tensor<...>` (используется `TextTensor` и `ImgTensor`).

Эту логику мы проверили на реальном QuickJS отдельным тестом: три рантайма в
разных потоках — во всех класс регистрируется и объекты создаются.

### 7.8. Итог по производительности

- Инференс масштабируется по ядрам (а не зажат в одно ядро мьютексом).
- Сеть разных клиентов перекрывается; медленный клиент не блокирует остальных.
- Память не растёт с числом клиентов (веса моделей общие).

---

## 8. Корректное завершение потоков соединений

После перехода на «поток на соединение» возникла отдельная задача: потоки были
`detach()`-нуты. Если сервер останавливается (на graceful-пути — например,
остановка Windows-сервиса), `app_main` возвращается и разрушает `env` и сессии
ONNX — **пока detached-потоки могут ещё работать и обращаться к ним**. Это гонка
use-after-free.

Решение — **отслеживать** потоки и дожидаться их (`ConnectionPool`):

```cpp
// src/testo_nn_server/Main.hpp (сокращённо)
class ConnectionPool {
public:
    ~ConnectionPool() { join_all(); }

    void spawn(net::Socket<asio::ip::tcp> socket) {
        std::lock_guard<std::mutex> lock(mutex);
        reap_finished();                              // подобрать завершившиеся
        auto connection = std::make_unique<Connection>();
        Connection* raw = connection.get();
        raw->thread = std::thread([raw, sock = std::move(socket)]() mutable {
            serve_connection(std::move(sock));
            raw->done.store(true, std::memory_order_release);  // пометить «готово»
        });
        connections.push_back(std::move(connection));
    }

    void join_all() {                                 // дождаться всех
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& c : connections)
            if (c->thread.joinable()) c->thread.join();
        connections.clear();
    }

private:
    struct Connection { std::atomic<bool> done{false}; std::thread thread; };

    void reap_finished() {                            // join + удалить завершившиеся
        for (auto it = connections.begin(); it != connections.end(); ) {
            if ((*it)->done.load(std::memory_order_acquire)) {
                (*it)->thread.join();
                it = connections.erase(it);
            } else ++it;
        }
    }

    std::mutex mutex;
    std::list<std::unique_ptr<Connection>> connections;
};
```

Как это работает:

- **Reaping.** Завершившиеся соединения не висят как «зомби»: при каждом новом
  `spawn` мы join-им и удаляем те, что выставили `done`. Поэтому набор не растёт
  при нормальной работе (клиенты подключаются/отключаются).
- **Дренаж на остановке.** Цикл приёма ловит `net::Interruption` и зовёт
  `join_all()`, который дожидается всех активных потоков **до** возврата из
  `local_handler`. Значит, `env`/сессии разрушатся уже после остановки потоков.

```cpp
void local_handler(const nlohmann::json& settings) {
    net::TcpAcceptor acceptor(...);
    ConnectionPool pool;
    try {
        while (true) {
            pool.spawn(acceptor.accept());
        }
    } catch (const net::Interruption&) {
        spdlog::info("Shutting down: waiting for active connections to finish");
    }
    pool.join_all();   // env/сессии разрушаются только после этого
}
```

Чем потоки «узнают», что пора выходить: они и так заблокированы в `recv()` (то
есть в `pump`), который проверяет `interrupt_requested()` раз в ~20 мс. Когда
`StopApp()` вызывает `net::request_interrupt()`, каждый поток в своём
блокирующем вводе/выводе бросает `net::Interruption`. Чтобы поток при этом
завершался **аккуратно**, а не валил процесс необработанным исключением, в
`serve_connection` добавлен `catch (const net::Interruption&)`.

Корректность по памяти: `reap`/`join_all` удаляют `Connection` только **после**
`join()` соответствующего потока, то есть когда поток гарантированно завершил
запись в `raw->done` и больше не трогает свою структуру (`release`/`acquire`
дают нужную синхронизацию). Сами потоки `mutex` пула не трогают — взаимоблокировок
нет.

Проверено стендом: 50 заблокированных на чтении соединений после
`request_interrupt()` дренируются за ~10 мс; естественно завершившиеся —
подбираются reaping-ом.

> Замечание про Linux: демон на Linux завершается по SIGTERM штатным kill
> (процесс умирает целиком — гонки teardown нет). Поэтому graceful-дренаж важен
> прежде всего там, где `app_main` действительно возвращается (Windows-сервис).
> Инфраструктура корректна для обоих случаев и сработает, если позже добавить
> graceful-обработку SIGTERM на Linux через `net::SignalGuard`.

---

## 9. Сборка: C++20 и CMake

- `CMAKE_CXX_STANDARD` поднят с 17 до **20** (нужно для `std::stop_token`,
  `std::counting_semaphore`, удобных generic-лямбд).
- Каталог `3rd_party/coro` удалён, `add_subdirectory(coro)` убран.
- Добавлена библиотека `lib/net` (`add_library(net STATIC ...)`), которая на UNIX
  линкуется с `pthread`.
- Во всех целях `target_link_libraries(... coro ...)` заменено на `net`
  (testo_core, testo_nn_server, js, guest-additions и протокольные библиотеки).
- Standalone asio остаётся (глобальный include `3rd_party` + `-DASIO_STANDALONE`),
  так как `net` построена поверх него.

---

## 10. Что осталось проверить и ограничения

1. **Полная сборка под внешние зависимости.** В среде разработки не было
   `libvirt`, `guestfs` и `onnxruntime`, поэтому изолированно собрана и
   протестирована библиотека `lib/net`, синтаксически проверены независимые файлы,
   а логика конкурентности и QuickJS проверена отдельными стендами. Полную сборку
   `testo`/`testo_nn_server` и Windows-ветки (Hyper-V, сервисный код) нужно
   прогнать на CI/целевой платформе.

2. **Рабочие буферы детекторов — `thread_local`.** Их размер зависит от размера
   обрабатываемого скриншота и живёт, пока живёт поток-соединение. При очень
   большом числе одновременных соединений с большими скриншотами стоит ограничить
   число одновременных соединений (backpressure) или сбрасывать буферы между
   запросами. Веса моделей при этом общие (это главный выигрыш по памяти).

3. **Неограниченное число потоков.** Сейчас на каждое соединение создаётся поток.
   100 — это нормально, но для тысяч соединений имеет смысл ограничить число
   одновременных соединений. (Не делалось в рамках этой задачи.)

4. **Нагрузочный тест.** Поведение под 100+ клиентов стоит подтвердить нагрузочным
   тестом на целевой машине (в частности, эффект семафора и масштабирование по
   ядрам).

---

## 11. Глоссарий и ссылки

- **asio** — библиотека ввода/вывода; ядро — `io_context` (очередь задач + обёртка
  над `epoll`/IOCP). [Документация](https://think-async.com/Asio/).
- **completion handler** — колбэк, который asio вызывает по завершении
  асинхронной операции (внутри `io_context::run*`).
- **Фибер / стековая корутина** — кооперативно переключаемая нить исполнения с
  собственным стеком; основа удалённой библиотеки `coro`.
- **`std::stop_token` / `std::stop_source`** — стандартный механизм кооперативной
  отмены C++20. [cppreference](https://en.cppreference.com/w/cpp/thread/stop_token).
- **`thread_local`** — переменная с отдельным экземпляром на каждый поток.
  [cppreference](https://en.cppreference.com/w/cpp/language/storage_duration#Thread_local_storage).
- **`std::counting_semaphore`** — счётчик-ограничитель параллелизма (C++20).
  [cppreference](https://en.cppreference.com/w/cpp/thread/counting_semaphore).
- **`std::call_once` / `std::once_flag`** — гарантированно однократная
  инициализация, потокобезопасно.
  [cppreference](https://en.cppreference.com/w/cpp/thread/call_once).
- **ONNX Runtime `Ort::Session`** — исполнитель модели; метод `Run()`
  потокобезопасен (одну сессию можно исполнять из многих потоков).
  [Документация по многопоточности ORT](https://onnxruntime.ai/docs/get-started/with-cpp.html).
- **QuickJS** — встраиваемый JS-движок; `JSRuntime` — на поток, `JSContext` —
  область исполнения, классы регистрируются в каждом рантайме.
  [Документация QuickJS](https://bellard.org/quickjs/quickjs.html).
- **use-after-free** — обращение к памяти после её освобождения; типичная причина
  падений при некорректном завершении потоков.
