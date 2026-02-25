# Подробный анализ библиотеки `coro`

## Оглавление

1. [Назначение и мотивация](#1-назначение-и-мотивация)
2. [Архитектура и структура файлов](#2-архитектура-и-структура-файлов)
3. [Уровень 1: Файберы (переключение контекста)](#3-уровень-1-файберы-переключение-контекста)
4. [Уровень 2: Корутины (Coro)](#4-уровень-2-корутины-coro)
5. [Уровень 3: Цикл событий (IoService)](#5-уровень-3-цикл-событий-ioservice)
6. [Уровень 4: Интеграция с asio (AsioTask)](#6-уровень-4-интеграция-с-asio-asiotask)
7. [Уровень 5: Высокоуровневые обёртки ввода/вывода](#7-уровень-5-высокоуровневые-обёртки-вводавывода)
8. [Примитивы синхронизации](#8-примитивы-синхронизации)
9. [Управление временем жизни корутин](#9-управление-временем-жизни-корутин)
10. [Обработка исключений и отмена](#10-обработка-исключений-и-отмена)
11. [Вспомогательные классы](#11-вспомогательные-классы)
12. [Полный пошаговый пример](#12-полный-пошаговый-пример)
13. [Сравнение с альтернативами](#13-сравнение-с-альтернативами)

---

## 1. Назначение и мотивация

### Проблема

Традиционный асинхронный ввод/вывод на основе `boost::asio` (или `asio`) требует
callback-стиля программирования. Это приводит к нескольким серьёзным проблемам:

**Фрагментация кода.** Логика операции разбивается на две части: инициация
асинхронной операции и обработчик (callback). Эти части могут находиться далеко
друг от друга, что затрудняет чтение и поддержку кода.

**Невозможность использования исключений.** В callback-модели asio ошибки
передаются через `error_code`, а не через исключения. Это лишает возможности
использовать идиому RAII для автоматического управления ресурсами при ошибках.

**Потеря стека вызовов.** Каждый callback вызывается из цикла событий, а не
из исходного места вызова. Это делает невозможным отладку через стек вызовов
и лишает возможности хранить локальные переменные между вызовами.

### Решение

Библиотека `coro` реализует **стэкфул (stackful) корутины** — каждая корутина
получает свой собственный стек вызовов. Это позволяет:

- Писать асинхронный код, который **выглядит как синхронный**
- Использовать **исключения** для обработки ошибок
- Сохранять **локальные переменные** между асинхронными операциями
- Иметь **осмысленный стек вызовов** для отладки

Вот так выглядит асинхронный TCP echo-сервер с использованием `coro`:

```cpp
Acceptor acceptor(endpoint);
acceptor.run([](TcpSocket socket) {
    std::vector<uint8_t> buffer(1024);
    while (true) {
        auto bytesTransfered = socket.readSome(asio::buffer(buffer));
        socket.write(asio::buffer(&buffer[0], bytesTransfered));
    }
});
```

Этот код выглядит как обычный синхронный код, но под капотом `readSome` и `write`
выполняют неблокирующие асинхронные операции, отдавая управление другим корутинам
во время ожидания.

---

## 2. Архитектура и структура файлов

Библиотека организована в виде слоёв, от низкоуровневого переключения контекста
до высокоуровневых абстракций ввода/вывода:

```
┌──────────────────────────────────────────────────┐
│  Уровень 5: I/O обёртки                         │
│  StreamSocket, Acceptor, Timer, Resolver, ...    │
├──────────────────────────────────────────────────┤
│  Уровень 4: Мост asio <-> корутины              │
│  AsioTask, AsioTask1, AsioTask2                  │
├──────────────────────────────────────────────────┤
│  Уровень 3: Цикл событий                        │
│  IoService, Application                          │
├──────────────────────────────────────────────────┤
│  Уровень 2: Корутины                             │
│  Coro (yield / resume / tokens)                  │
├──────────────────────────────────────────────────┤
│  Уровень 1: Переключение контекста               │
│  FiberLinux (ucontext) / FiberWindows (fibers)   │
└──────────────────────────────────────────────────┘
```

### Файловая структура

| Файл | Назначение |
|------|-----------|
| `FiberLinux.h/cpp` | Переключение контекста через `ucontext` (Linux) |
| `FiberWindows.h/cpp` | Переключение контекста через Windows Fibers |
| `Coro.h/cpp` | Ядро: корутина с механизмом токенов |
| `IoService.h/cpp` | Цикл событий (обёртка над `asio::io_service`) |
| `Application.h/cpp` | Точка входа приложения |
| `AsioTask.h` | Мост между callback-ами asio и корутинами |
| `Stream.h` | Шаблон потокового чтения/записи |
| `StreamSocket.h` | TCP-сокет |
| `DatagramSocket.h` | UDP-сокет |
| `Acceptor.h` | TCP-сервер (принятие соединений) |
| `Resolver.h` | DNS-резолвер |
| `Timer.h` | Таймер |
| `Timeout.h` | Таймаут с автоматической отменой по RAII |
| `SignalSet.h/cpp` | Обработка сигналов ОС |
| `Mutex.h/cpp` | Мьютекс для корутин |
| `Queue.h` | Очередь для корутин |
| `CoroPool.h/cpp` | Иерархический пул корутин |
| `CheckPoint.h/cpp` | Явная точка уступки управления |
| `Finally.h` | RAII-обёртка для cleanup-действий |
| `Work.h` | Обёртка над `asio::io_service::work` |

---

## 3. Уровень 1: Файберы (переключение контекста)

### Что такое файбер?

Файбер — это легковесный поток выполнения с собственным стеком, но без
собственного потока ОС. Переключение между файберами происходит **кооперативно**
(явно, в точках, определённых программистом), а не **вытесняюще** (как между
потоками ОС).

### Реализация на Linux (`FiberLinux.cpp`)

На Linux используется POSIX API `<ucontext.h>`:

```
┌─────────────────────────────────────────────────────────┐
│               Глобальное состояние (thread_local)       │
│                                                         │
│  mainContext ── ucontext_t основного потока              │
│  (инициализируется при Fiber::initialize())             │
└─────────────────────────────────────────────────────────┘
```

**Конструктор** — создание нового файбера:

```cpp
Fiber::Fiber(void (*startRoutine)(void*), void* parameter)
    : _buffer(1024 * 1024 * 4)  // 4 МБ стека
{
    getcontext(&_context);                          // (1)
    _context.uc_stack.ss_sp = _buffer.data();       // (2)
    _context.uc_stack.ss_size = _buffer.size();
    _context.uc_link = nullptr;                     // (3)
    makecontext(&_context, startRoutine, 1, parameter); // (4)
}
```

1. `getcontext` — заполняет `_context` текущим состоянием процессора (регистры, указатель стека и т.д.)
2. Подменяем стек на свой буфер размером 4 МБ
3. `uc_link = nullptr` — после завершения функции не переключаемся автоматически
4. `makecontext` — настраивает контекст так, чтобы при активации вызвалась `startRoutine(parameter)`

**Три операции переключения:**

```
                  enter()                    exit()
  mainContext ─────────────► Fiber    Fiber ──────────► mainContext
                            │    ▲
                switchTo()  │    │  switchTo()
                            ▼    │
                          другой Fiber
```

- **`enter()`** — вход в файбер из основного контекста:
  ```cpp
  swapcontext(mainContext, &_context);
  ```
  Сохраняет текущее состояние в `mainContext`, загружает состояние из `_context`.

- **`switchTo(fiber)`** — переключение между двумя файберами:
  ```cpp
  swapcontext(&_context, &fiber._context);
  ```
  Сохраняет текущее состояние в `this->_context`, переходит к `fiber._context`.

- **`exit()`** — возврат из файбера в основной контекст:
  ```cpp
  swapcontext(&_context, mainContext);
  ```

Ключевой момент: `swapcontext` — это **атомарная** операция сохранения-и-загрузки
контекста. Когда файбер будет возобновлён, выполнение продолжится ровно с того
места, где был вызван `swapcontext`, как будто этот вызов просто вернулся.

### Диаграмма памяти

```
Основной стек потока:          Стек файбера 1:         Стек файбера 2:
┌──────────────┐               ┌──────────────┐        ┌──────────────┐
│   main()     │               │  routine()   │        │  routine()   │
│   ...        │               │   ...        │        │   ...        │
│  run_one()   │               │  yield()     │ ◄──┐   │  yield()     │
│   callback() │ ──enter()──►  │  (suspended) │    │   │  (suspended) │
│  (suspended) │               │              │    │   │              │
│              │               │              │    └── │  switchTo()  │
│  4 МБ стека  │               │  4 МБ стека  │        │  4 МБ стека  │
└──────────────┘               └──────────────┘        └──────────────┘
```

---

## 4. Уровень 2: Корутины (Coro)

Класс `Coro` — это ядро библиотеки. Он надстраивает над файбером механизм
**токенов** для управления возобновлением, а также обработку исключений.

### Глобальное состояние

```cpp
thread_local Coro* t_currentCoro = nullptr;
```

В каждом потоке хранится указатель на текущую выполняющуюся корутину. `nullptr`
означает, что мы находимся в основном контексте (не в корутине).

### Создание корутины

```cpp
Coro::Coro(std::function<void()> routine)
    : _routine(std::move(routine)),
      _fiber(Run, this),       // файбер вызовет Run(this) при входе
      _previousCoro(nullptr),
      _tokens({TokenStart})    // начальный токен — "__start__"
{}
```

При создании корутина ещё не запускается. Она находится в состоянии ожидания
с токеном `"__start__"`.

### Механизм токенов

Токены — это строки, которые определяют, **при каком условии** корутина может
быть возобновлена. Это ключевая идея библиотеки.

Когда корутина засыпает (`yield`), она указывает набор токенов, на которые
она согласна проснуться:

```cpp
coro->yield({"AsioTask 0x7fff1234", "__throw__"});
//           ^^^^^^^^^^^^^^^^^^^^^^^^  ^^^^^^^^^^^
//           проснуться при завершении  или при
//           asio-операции              исключении
```

Когда кто-то вызывает `resume(token)`, корутина проснётся **только если** `token`
содержится в её текущем наборе ожидаемых токенов.

### `resume()` — возобновление корутины

```cpp
void Coro::resume(const std::string& token) {
    // (1) Проверяем, ожидает ли корутина этот токен
    if (std::find(_tokens.begin(), _tokens.end(), token) == _tokens.end())
        return;  // Не тот токен — ничего не делаем

    // (2) Сохраняем текущую корутину и устанавливаем себя как текущую
    _previousCoro = t_currentCoro;
    t_currentCoro = this;

    // (3) Переключаемся
    if (_previousCoro) {
        _previousCoro->_fiber.switchTo(_fiber);    // из корутины в корутину
    } else {
        _fiber.enter();                            // из основного контекста в корутину
    }

    // (4) Мы вернулись! Восстанавливаем предыдущую корутину
    t_currentCoro = _previousCoro;
    _previousCoro = nullptr;
}
```

Важный момент: строка (4) выполнится **только когда корутина отдаст управление
обратно** (через `yield` или завершение). Между строками (3) и (4) может пройти
произвольное количество времени и операций.

### `yield()` — приостановка корутины

```cpp
void Coro::yield(std::vector<std::string> tokens) {
    _tokens = std::move(tokens);
    Finally clearTokens([&] { _tokens.clear(); });

    // (1) Проверяем, нет ли запланированных исключений
    if (TokenThrow в tokens) propagateException();

    // (2) Отдаём управление
    if (_previousCoro) {
        _fiber.switchTo(_previousCoro->_fiber);  // к предыдущей корутине
    } else {
        _fiber.exit();                           // к основному контексту
    }

    // (3) Мы проснулись — проверяем исключения ещё раз
    if (TokenThrow в tokens) propagateException();
}
```

Двойная проверка исключений (до и после переключения) обеспечивает их немедленную
доставку:
- **До** `switchTo`: если исключение уже было запланировано — бросить сразу
- **После** `switchTo`: если исключение пришло пока мы спали — бросить при пробуждении

### `run()` — главная функция файбера

```cpp
void Coro::run() {
    try {
        _routine();      // Выполняем пользовательскую функцию
    }
    catch (const CancelError&) {
        // Отмена — штатная ситуация, просто завершаемся
    }
    catch (...) {
        // Все остальные исключения сохраняем для проброса родителю
        _exceptions.push_front(std::current_exception());
    }
    _routine = nullptr;
    yield({});           // Пустой набор токенов = корутина завершена
}
```

### Диаграмма жизненного цикла

```
    ┌──────────┐     start()      ┌──────────┐
    │ Создана  │─────────────────►│Выполняется│
    │(TokenStart)│                │           │
    └──────────┘                  └─────┬─────┘
                                        │
                                   yield(tokens)
                                        │
                                        ▼
                                  ┌───────────┐    resume(token)   ┌──────────┐
                                  │  Ожидает  │◄──────────────────►│Выполняется│
                                  │ (tokens)  │                    │           │
                                  └─────┬─────┘                    └──────────┘
                                        │
                                   yield({})
                                        │
                                        ▼
                                  ┌──────────┐
                                  │ Завершена│
                                  └──────────┘
```

---

## 5. Уровень 3: Цикл событий (IoService)

### `IoService` — обёртка над `asio::io_service`

```cpp
class IoService {
    static IoService* current();       // thread_local текущий сервис
    void run();
    void post(T&& t);                 // поставить задачу в очередь
    void dispatch(T&& t);             // выполнить немедленно или в очереди
    std::queue<std::function<void()>> checkpoints;
    asio::io_service _impl;
};
```

### Цикл событий

```cpp
void IoService::run() {
    Fiber::initialize();               // (1) Инициализация файберной системы
    t_ioService = this;

    while (_impl.run_one())            // (2) Ждём и выполняем одно событие
    {
        while (_impl.poll_one());      // (3) Выполняем все готовые события
        while (checkpoints.size()) {   // (4) Выполняем чекпоинты
            auto checkpoint = std::move(checkpoints.front());
            checkpoints.pop();
            checkpoint();
        }
    }

    t_ioService = nullptr;
    Fiber::deinitialize();             // (5) Очистка
}
```

Логика цикла:
1. `run_one()` — блокируется до завершения хотя бы одной asio-операции и выполняет её callback
2. `poll_one()` — выполняет все уже готовые callback-и **без блокировки**
3. Обрабатываем чекпоинты (отложенные переключения корутин)
4. Возвращаемся к `run_one()` — ждём следующего события

Цикл завершается, когда нет ни одного незавершённого asio-обработчика.

### Зачем нужны чекпоинты?

Чекпоинты — это механизм для **честного переключения** между корутинами.
Без чекпоинтов, корутина, выполняющая только вычисления (без I/O), могла бы
захватить управление навсегда.

```cpp
void CheckPoint() {
    auto coro = Coro::current();
    std::string token = "CheckPoint " + std::to_string((uint64_t)coro);

    IoService::current()->post([=] {          // (1) Ставим задачу в очередь asio
        IoService::current()->checkpoints.push([=] {
            coro->resume(token);              // (3) Возобновляем корутину
        });
    });

    coro->yield({token, TokenThrow});         // (2) Засыпаем
}
```

`CheckPoint()` работает в два этапа:
1. Через `post()` ставит в очередь asio задачу, которая добавит возобновление
   в `checkpoints`
2. Корутина засыпает
3. В следующей итерации цикла событий корутина возобновится

Это даёт возможность другим корутинам (и asio-обработчикам) выполниться между
вызовами `CheckPoint()`.

### `Application` — точка входа

```cpp
Application::Application(const std::function<void()>& main)
    : _coro([=] {
        Work work;      // Не даёт циклу событий завершиться
        main();         // Пользовательская функция
    })
{
    _ioService.post([=] {
        _coro.start();  // Запускаем корутину через очередь asio
    });
}
```

`Work` — это обёртка над `asio::io_service::work`, которая не даёт `io_service`
решить, что работа закончена, пока корневая корутина не завершится. Без `Work`
цикл `run_one()` мог бы завершиться преждевременно.

---

## 6. Уровень 4: Интеграция с asio (AsioTask)

Это **ключевой мост** между callback-миром asio и корутинным миром `coro`.

### Принцип работы

```
Корутина                          asio event loop
────────                          ───────────────
socket.async_read(..., callback)  ────►  зарегистрировать
yield(token) ─── засыпаем                  │
    .                                      │ (данные пришли)
    .                                      │
    .              resume(token) ◄─── callback вызван
    .         ─── просыпаемся
проверяем ошибку
возвращаем результат
```

### Базовый класс `AsioTask`

```cpp
class AsioTask {
protected:
    template <typename Handle>
    void doWait(Handle& handle) {
        try {
            _coro->yield({token(), TokenThrow});    // (1) Засыпаем
        }
        catch (...) {
            auto exception = std::current_exception();
            handle.cancel();                        // (2) Отменяем I/O
            _coro->yield({token()});                // (3) Ждём отмены
            std::rethrow_exception(exception);      // (4) Пробрасываем
        }
    }

    std::string token() const {
        return "AsioTask " + std::to_string((uint64_t)this);
    }
};
```

Токен формируется из адреса объекта `AsioTask`, что гарантирует его уникальность.

Обработка прерывания (шаги 2-4):
- Если во время ожидания I/O в корутину прилетает исключение (например, `CancelError`)
- Мы **не можем просто выйти** — asio-операция ещё выполняется
- Сначала отменяем I/O (`handle.cancel()`)
- Ждём, пока callback всё-таки вызовется (с кодом ошибки "cancelled")
- Только после этого пробрасываем исключение

### `AsioTask1` — операция без возвращаемого значения

```cpp
class AsioTask1 : public AsioTask {
    std::function<void(const std::error_code&)> callback() {
        return [=](const std::error_code& errorCode) {
            _isCallbackExecuted = true;
            _errorCode = errorCode;
            _coro->resume(token());    // Будим корутину!
        };
    }

    template <typename Handle>
    void wait(Handle& handle) {
        doWait(handle);
        if (_errorCode) throw std::system_error(_errorCode);
    }
};
```

### `AsioTask2<Result>` — операция с возвращаемым значением

```cpp
template <typename Result>
class AsioTask2 : public AsioTask {
    std::function<void(const std::error_code&, Result)> callback() {
        return [=](const std::error_code& errorCode, Result result) {
            _isCallbackExecuted = true;
            _errorCode = errorCode;
            _result = result;           // Сохраняем результат
            _coro->resume(token());
        };
    }

    template <typename Handle>
    Result wait(Handle& handle) {
        doWait(handle);
        if (_errorCode) throw std::system_error(_errorCode);
        return _result;                 // Возвращаем результат
    }
};
```

### Пример: как работает `socket.read()`

```cpp
// Stream<Handle>::read():
template <typename ...T>
size_t read(T&&... t) {
    AsioTask2<size_t> task;                                 // (1)
    asio::async_read(_handle, asio::buffer(...), task.callback()); // (2)
    return task.wait(_handle);                              // (3)
}
```

Развёрнутая последовательность:

```
Время   Корутина A              asio event loop
─────   ──────────              ───────────────
  t0    task = AsioTask2<size_t>
  t1    asio::async_read(handle, buffer, callback)
        │  регистрирует операцию чтения
  t2    task.wait(handle)
        │  → doWait(handle)
        │  → yield({"AsioTask 0x...", "__throw__"})
        │  ═══ корутина спит ═══
        │                       │
  t3    │                       (другие корутины работают)
  t4    │                       (данные пришли в сокет)
        │                       callback(error_code, bytes)
        │                       │  → _result = bytes
        │                       │  → resume("AsioTask 0x...")
        │  ═══ корутина проснулась ═══
  t5    │  проверяет _errorCode
  t6    │  return _result (= bytes)
  t7    вызывающий код получает количество прочитанных байт
```

---

## 7. Уровень 5: Высокоуровневые обёртки ввода/вывода

### `StreamSocket<Protocol>` — TCP-сокет

```cpp
template <typename Protocol>
class StreamSocket : public Stream<typename Protocol::socket> {
    StreamSocket();                                          // Создание
    StreamSocket(const endpoint& ep);                        // С привязкой
    void connect(const endpoint& ep);                        // Подключение
    // + read(), write(), readSome(), writeSome() из Stream
};

using TcpSocket = StreamSocket<asio::ip::tcp>;
```

Метод `connect` — типичный пример трансформации:

```cpp
void connect(const endpoint& endpoint) {
    AsioTask1 task;
    _handle.async_connect(endpoint, task.callback());
    task.wait(_handle);  // Бросает исключение при ошибке
}
```

### `Acceptor<Protocol>` — TCP-сервер

```cpp
template <typename Protocol>
class Acceptor {
    Acceptor(const endpoint& ep);        // bind + listen
    socket accept();                     // Принять одно соединение
    void run(callback);                  // Цикл принятия соединений
};
```

Метод `run` — паттерн «один обработчик на соединение»:

```cpp
void run(std::function<void(socket)> callback) {
    CoroPool coroPool;
    while (true) {
        auto socket = accept();          // Ждём соединения (неблокирующе!)
        coroPool.exec([&] {
            callback(std::move(socket)); // Обрабатываем в отдельной корутине
        });
    }
}
```

### `Timer` — таймер

```cpp
class Timer {
    void waitFor(Duration duration);    // Подождать N секунд/миллисекунд
    void waitUntil(Timestamp ts);       // Подождать до определённого момента
};
```

### `Timeout` — RAII-таймаут

```cpp
{
    Timeout timeout(std::chrono::seconds(5));
    socket.read(buffer);  // Бросит TimeoutError через 5 секунд
}
// Деструктор Timeout отменяет таймер
```

Реализация:
- В конструкторе запускается `async_wait` на таймере
- Если таймер срабатывает — в корутину бросается `TimeoutError`
- Деструктор отменяет таймер и **дожидается вызова callback** (это важно!)
- Без этого ожидания callback мог бы попытаться обратиться к уже разрушенным данным

### `Resolver<Protocol>` — DNS-резолвер

```cpp
Iterator resolve(const Query& query) {
    AsioTask2<Iterator> task;
    _handle.async_resolve(query, task.callback());
    return task.wait(_handle);
}
```

### `DatagramSocket<Protocol>` — UDP-сокет

```cpp
size_t send(const T& data, const endpoint& ep);
size_t receive(const T& data, endpoint& ep);
```

---

## 8. Примитивы синхронизации

### `Mutex` — мьютекс для корутин

**Не потокобезопасен** (рассчитан на однопоточную модель с корутинами).

```cpp
void Mutex::lock() {
    if (_owner) {
        // Мьютекс занят — встаём в очередь и засыпаем
        Finally cleanup([&] { _coros.remove(Coro::current()); });
        _coros.push_back(Coro::current());
        _coros.back()->yield({token(), TokenThrow});
    }
    _owner = Coro::current();
}

void Mutex::unlock() {
    _owner = nullptr;
    if (!_coros.empty()) {
        _coros.front()->resume(token());  // Будим следующего ожидающего
    }
}
```

Диаграмма:

```
Корутина A: lock()  → захватила (owner = A)
Корутина B: lock()  → owner != nullptr → yield({token, throw})
Корутина A: unlock() → owner = nullptr → resume(B)
Корутина B: (проснулась) → owner = B
```

`Finally` гарантирует, что при отмене корутины (через `CancelError`) она будет
удалена из очереди ожидания мьютекса.

### `Queue<T>` — очередь для корутин

Аналогичный принцип:

```cpp
T pop() {
    if (_data.empty()) {
        // Очередь пуста — встаём в очередь ожидающих и засыпаем
        Finally cleanup([&] { _coros.remove(Coro::current()); });
        _coros.push_back(Coro::current());
        _coros.back()->yield({token(), TokenThrow});
    }
    T t = std::move(_data.front());
    _data.pop();
    return t;
}

template <typename U>
void push(U&& u) {
    _data.push(std::forward<U>(u));
    if (!_coros.empty()) {
        _coros.front()->resume(token());  // Будим первого ожидающего
    }
}
```

---

## 9. Управление временем жизни корутин

### `CoroPool` — иерархический пул

`CoroPool` реализует отношение «родитель — дети» между корутинами:

```cpp
Coro* CoroPool::exec(std::function<void()> routine) {
    auto coro = new Coro([=] {
        Finally cleanup([=] {
            onCoroDone(Coro::current());  // Уведомляем при завершении
        });
        routine();
    });
    _childCoros.insert(coro);
    coro->start();     // Начинаем выполнение немедленно
    return coro;
}
```

**`waitAll()`** — родительская корутина засыпает, пока все дети не завершатся:

```cpp
void CoroPool::waitAll(bool noThrow) {
    if (_childCoros.empty()) return;
    if (noThrow)
        _parentCoro->yield({token()});            // Только ждём
    else
        _parentCoro->yield({token(), TokenThrow}); // Ждём + бросаем исключения
}
```

**`onCoroDone()`** — вызывается при завершении дочерней корутины:

```cpp
void CoroPool::onCoroDone(Coro* childCoro) {
    IoService::current()->post([=] {
        // Пробрасываем исключения дочерней корутины в родительскую
        while (childCoro->exceptions().size()) {
            try { childCoro->propagateException(); }
            catch (const CancelError&) { /* Игнорируем отмены */ }
            catch (...) {
                _parentCoro->propagateException(std::current_exception());
            }
        }
        _childCoros.erase(childCoro);
        delete childCoro;

        // Если все дети завершились — будим родителя
        if (_childCoros.empty()) {
            _parentCoro->resume(token());
        }
    });
}
```

Важно: `onCoroDone` выполняется через `post()`, а не напрямую. Это гарантирует,
что очистка происходит в основном контексте, а не внутри файбера дочерней корутины.

**Деструктор** — гарантирует завершение всех детей:

```cpp
CoroPool::~CoroPool() {
    cancelAll();          // Отменяем всех детей
    waitAll(true);        // Ждём завершения (без пробрасывания исключений)
}
```

---

## 10. Обработка исключений и отмена

### Механизм исключений

Исключения в корутину можно «послать» извне:

```cpp
void Coro::propagateException(std::exception_ptr exception) {
    _exceptions.push_back(exception);
    resume(TokenThrow);    // Пытаемся разбудить корутину токеном __throw__
}
```

Корутина проверяет исключения в `yield()`:

```cpp
void Coro::propagateException() {
    if (_exceptions.size()) {
        auto exception = _exceptions.front();
        _exceptions.pop_front();
        std::rethrow_exception(exception);  // Бросаем как обычное исключение!
    }
}
```

Таким образом, для пользовательского кода исключения из `coro` неотличимы
от обычных C++ исключений. Они корректно раскручивают стек, вызывают деструкторы,
могут быть пойманы через `try/catch`.

### `CancelError` — отмена корутины

```cpp
struct CancelError {};  // Специально НЕ наследуется от std::exception!
```

`CancelError` не наследуется от `std::exception` специально, чтобы:
- `catch (const std::exception&)` его **не ловил**
- Гарантированно раскручивал стек до самого `run()`
- Пользователь, который пишет `catch (...)`, должен **сознательно** это делать

```cpp
void Coro::run() {
    try {
        _routine();
    }
    catch (const CancelError&) {
        // Отмена — штатная ситуация
    }
    catch (...) {
        _exceptions.push_front(std::current_exception());
    }
    yield({});  // Корутина завершена
}
```

### Цепочка отмены

```
cancel() вызван на корутине
    │
    ▼
propagateException(CancelError())
    │
    ▼
resume(TokenThrow)
    │
    ▼
корутина просыпается в yield()
    │
    ▼
propagateException() → rethrow_exception → throw CancelError
    │
    ▼
стек раскручивается:
  - деструкторы Finally выполняются
  - деструкторы Timeout отменяют таймеры
  - деструкторы CoroPool отменяют детей
    │
    ▼
CancelError ловится в run()
    │
    ▼
yield({}) — корутина завершена
```

---

## 11. Вспомогательные классы

### `Finally` — RAII cleanup

```cpp
Finally cleanup([] { teardownLogging(); });
// или цепочка:
Finally rollbacks;
rollbacks << [] { undoAction1(); };
rollbacks << [] { undoAction2(); };
rollbacks.discard();  // Отмена — ничего не откатываем
```

Функции выполняются в **обратном порядке** (LIFO) при разрушении объекта.

### `Work` — удержание цикла событий

```cpp
class Work {
    asio::io_service::work _impl = ...(IoService::current()->_impl);
};
```

Пока объект `Work` существует, `io_service::run_one()` не возвращает 0 (т.е.
цикл не завершается).

---

## 12. Полный пошаговый пример

Рассмотрим, как работает TCP echo-сервер от начала до конца:

```cpp
int main() {
    coro::Application([&] {
        auto endpoint = asio::ip::tcp::endpoint(
            asio::ip::tcp::v4(), 8080);

        coro::Acceptor<asio::ip::tcp> acceptor(endpoint);
        acceptor.run([](asio::ip::tcp::socket rawSocket) {
            coro::StreamSocket<asio::ip::tcp> socket(std::move(rawSocket));
            std::vector<uint8_t> buffer(1024);
            while (true) {
                auto n = socket.readSome(asio::buffer(buffer));
                socket.write(asio::buffer(buffer.data(), n));
            }
        });
    }).run();
}
```

### Пошаговое выполнение

```
1.  Application создаётся:
    - Создаётся IoService
    - Создаётся Coro с main-лямбдой
    - post([=] { _coro.start(); }) — ставим запуск в очередь asio

2.  Application::run() → IoService::run():
    - Fiber::initialize() — создаём mainContext
    - run_one() → вытаскиваем задачу start()

3.  _coro.start() → resume("__start__"):
    - t_currentCoro = &_coro
    - fiber.enter() → переключаемся в стек корутины

4.  Внутри корутины:
    - Work создаётся (удерживает event loop)
    - Acceptor создаётся (bind + listen на порт 8080)
    - acceptor.run(callback):
      - CoroPool создаётся
      - accept():
        - AsioTask1 создаётся
        - async_accept(socket, callback)
        - task.wait(handle) → yield({"AsioTask 0x...", "__throw__"})
        - fiber.exit() → возвращаемся в mainContext

5.  Обратно в IoService::run():
    - run_one() блокируется... ждём подключения клиента

6.  Клиент подключился:
    - async_accept callback вызван
    - resume("AsioTask 0x...") → fiber.enter()
    - accept() возвращает сокет
    - coroPool.exec(обработчик):
      - Создаётся новая дочерняя Coro
      - coro->start() → fiber.enter() из корутины acceptor'а в дочернюю

7.  Внутри дочерней корутины:
    - socket.readSome():
      - async_read_some(buffer, callback)
      - yield() → fiber.switchTo(корутина acceptor'а)

8.  Корутина acceptor'а продолжает:
    - accept() → async_accept → yield() → fiber.exit()

9.  Обратно в event loop... ждём данных от клиента...

10. Данные пришли:
    - callback readSome → resume() → fiber.enter()
    - readSome() возвращает количество байт
    - socket.write() → async_write → yield() → exit

11. Запись завершена:
    - callback write → resume() → enter
    - Цикл продолжается: readSome() → ...
```

---

## 13. Сравнение с альтернативами

| Характеристика | `coro` | `boost::asio::spawn` | C++20 корутины |
|---|---|---|---|
| Тип корутин | Stackful | Stackful | Stackless |
| Контекст | ucontext / WinFibers | boost::context | Компилятор |
| Размер стека | 4 МБ на корутину | Настраиваемый | Минимальный (heap) |
| Интеграция с asio | Собственные обёртки | Встроенная (yield_context) | `co_await` |
| Отмена | CancelError (исключение) | Нет встроенной | cancellation_slot |
| Иерархия корутин | CoroPool | Нет | Нет (нужна библиотека) |
| Примитивы синхронизации | Mutex, Queue | Нет | Нет (нужна библиотека) |
| Сложность | Средняя | Низкая | Высокая (язык) |

### Ключевые отличия от boost::asio::spawn

- **`coro` предоставляет полную инфраструктуру**: пул корутин с иерархией,
  мьютексы, очереди, таймауты — всё, что нужно для построения приложения
- **Механизм токенов** для управления возобновлением — уникальная особенность
- **Явная отмена через исключения** вместо `error_code`

### Ключевые особенности архитектуры

1. **Однопоточность**: вся библиотека рассчитана на работу в одном потоке
   (как Node.js). `thread_local` переменные, отсутствие локов.
2. **Кооперативная многозадачность**: переключение происходит только в точках
   `yield` (I/O операции, CheckPoint, Mutex::lock, Queue::pop).
3. **Токен-фильтрация**: корутина просыпается только по «своему» токену,
   что предотвращает ложные пробуждения.
4. **Иерархическое управление**: родительская корутина контролирует
   дочерние через CoroPool.
