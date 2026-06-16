# Модернизация библиотеки `coro` (asio 1.14 → 1.36, boost-free)

> ⚠️ **Внимание: этот документ описывает ПЕРВЫЙ этап (однопоточная boost-free модель на
> собственном stackful-`Fiber` с identity-токенами).** Затем ядро было переведено на
> **многопоточную модель поверх `asio::spawn` + `strand` (Boost.Context)**, чтобы запускать
> тысячи корутин на нескольких потоках общего `io_context` (см. `tests/benchmark/`). В
> актуальном коде нет ни `Fiber`, ни ручного `Coro::suspend/wake`: корутины запускаются через
> `CoroPool`/`Application(main, threads)`, а суспенд идёт через `yield_context`. Разделы ниже
> про identity-токены и `Fiber` отражают промежуточный этап и оставлены как история решения;
> неизменными между этапами остались мотивация (раздел 1–3) и переход на asio 1.36 (раздел 8).

---

## Оглавление

1. [Зачем вообще это делалось](#1-зачем-вообще-это-делалось)
2. [Что такое `coro` и как она работала](#2-что-такое-coro-и-как-она-работала)
3. [Проблемы старой архитектуры](#3-проблемы-старой-архитектуры)
4. [Развилка решений и почему выбран boost-free путь](#4-развилка-решений-и-почему-выбран-boost-free-путь)
5. [Ядро: новая модель `Coro`](#5-ядро-новая-модель-coro)
6. [Асинхронные операции: `cancellation_slot` вместо callback-токенов](#6-асинхронные-операции-cancellation_slot-вместо-callback-токенов)
7. [Перевод примитивов на новую модель](#7-перевод-примитивов-на-новую-модель)
8. [Обновления API самой asio (1.36)](#8-обновления-api-самой-asio-136)
9. [Адаптация юнит-тестов](#9-адаптация-юнит-тестов)
10. [Что НЕ менялось (граница невирусности)](#10-что-не-менялось-граница-невирусности)
11. [Проверка и результаты](#11-проверка-и-результаты)
12. [Краткая шпаргалка по соответствию старого и нового API](#12-краткая-шпаргалка-по-соответствию-старого-и-нового-api)

---

## 1. Зачем вообще это делалось

Исходная задача — поднять вендоренную asio с **1.14** до **1.36** и решить накопившиеся
проблемы в `coro`, не ломая код, который ей пользуется (`src/`, `lib/`).

При этом сразу всплыли два важных факта про современную asio, которые задали направление:

- **`asio::spawn` в 1.36 требует Boost.Context.** Файл `asio/impl/spawn.hpp` делает
  `#include <boost/context/fiber.hpp>`. То есть «штатный» способ сделать stackful-корутины
  на современной asio тянет за собой компилируемую библиотеку Boost.Context. Проект же
  сейчас принципиально **boost-free** (`-DASIO_STANDALONE`, без Boost).
- **В asio 1.36 ряд старых API не просто помечен deprecated, а полностью удалён.** Под
  нашими флагами сборки больше не существуют: `asio::io_service`, `asio::io_service::work`,
  `asio::ip::address::from_string`, `steady_timer::expires_from_now`,
  `basic_resolver::query`, `basic_resolver::iterator`.

Вывод: нельзя просто «подменить заголовки asio» — старый код `coro` перестанет
компилироваться. И нельзя «переписать на `asio::spawn`», не притащив Boost. Поэтому была
выбрана **модернизация на месте, boost-free** (подробнее — раздел 4).

---

## 2. Что такое `coro` и как она работала

`coro` — это маленькая библиотека stackful-корутин поверх asio. Идея: писать **синхронный
по виду** код (`socket.read(...)`, `queue.pop()`, `mutex.lock()`), который под капотом
неблокирующе ждёт завершения асинхронной операции, отдавая управление циклу событий asio.

Технически корутина — это отдельный стек (`Fiber`, реализованный через `ucontext` на Linux
и Windows Fibers на Windows). Когда корутина «ждёт», она переключается обратно в цикл
событий; когда событие наступило — переключается обратно в корутину.

Весь проект пользуется только **высокоуровневым** API: `Application`, `CoroPool`,
`Timeout`, `Timer`, `StreamSocket`, `Acceptor`, `Mutex`, `Queue`, `CheckPoint`, `Finally`.
А вот **низкоуровневый** механизм планирования (`Coro::resume/yield`) исторически был
завязан на строковые токены — и именно он был главным источником проблем.

---

## 3. Проблемы старой архитектуры

### 3.1. Планирование на строковых токенах

Сердце старого `Coro` — это пара `resume(token)` / `yield({tokens})`, где `token` — это
**строка**. Корутина засыпала, объявляя список строк, на которые её «разрешено» будить;
разбудить её можно было, только назвав строку из этого списка.

```cpp
// БЫЛО: Coro.h
static constexpr auto TokenStart = "__start__";
static constexpr auto TokenThrow = "__throw__";

void resume(const std::string& token);
void yield(std::vector<std::string> tokens);
```

А сами токены генерировались форматированием адреса объекта в строку — **в каждом
примитиве**:

```cpp
// БЫЛО: Mutex.cpp
std::string Mutex::token() const {
    return "Mutex " + std::to_string((uint64_t)this);
}
```

```cpp
// БЫЛО: AsioTask.h
std::string token() const {
    return "AsioTask " + std::to_string((uint64_t)this);
}
```

Проблемы:

- **Аллокации и форматирование строк на каждое переключение корутины.** На каждый
  `read`/`write`/`pop`/`lock` создавалась временная `std::string`, а `yield` принимал
  `std::vector<std::string>` (ещё аллокация). Для библиотеки, чья единственная задача —
  быстро переключать контексты, это чистые накладные расходы.
- **Сравнение по строкам в горячем пути.** `resume`/`yield` делали `std::find` по вектору
  строк, то есть посимвольное сравнение, чтобы понять «моё ли это пробуждение».
- **Стрингли-типизированный протокол.** Связь «кто кого будит» держалась на договорённости
  «строка `"Mutex 0x563..."` совпадёт со строкой `"Mutex 0x563..."`». Никакой проверки
  типов, легко ошибиться.

По сути строковый токен решал ровно одну задачу: дать каждому ожиданию **уникальный
идентификатор**, чтобы чужое пробуждение игнорировалось. Для уникального идентификатора
строка не нужна — достаточно адреса объекта-инициатора.

### 3.2. `std::function`-callbacks и ручная отмена в каждой асинхронной операции

Каждая асинхронная операция (`AsioTask`) оборачивала колбэк asio в `std::function` и
вручную отменяла операцию через `handle.cancel()` при заброшенном исключении:

```cpp
// БЫЛО: AsioTask.h
std::function<void(const std::error_code&)> callback() {
    return [=](const std::error_code& errorCode) {
        _isCallbackExecuted = true;
        _errorCode = errorCode;
        _coro->resume(token());     // строковый токен
    };
}

template <typename Handle>
void doWait(Handle& handle) {
    try {
        _coro->yield({token(), TokenThrow});
    }
    catch (...) {
        auto exception = std::current_exception();
        handle.cancel();            // отменяет ВСЕ операции на хэндле
        _coro->yield({token()});
        std::rethrow_exception(exception);
    }
}
```

Проблемы:

- `std::function` — это аллокация и type-erasure ради одного колбэка.
- `handle.cancel()` отменяет **все** операции, висящие на сокете/таймере, а не конкретную
  нашу. В современной asio для этого есть точечный механизм — **`cancellation_slot`**.

### 3.3. Старый код опирался на API asio, удалённые в 1.36

`IoService` держал `asio::io_service`, `Work` — `asio::io_service::work`, `Timeout`/`Timer`
звали `expires_from_now`, `Resolver` — `basic_resolver::query`/`iterator`. Всё это в 1.36
**удалено**, поэтому без правок библиотека просто не собралась бы на новой asio.

---

## 4. Развилка решений и почему выбран boost-free путь

Рассматривались три варианта:

| Вариант | Суть | Минус |
|--------|------|-------|
| **A.** `asio::spawn` + `yield_context` | Самый «каноничный» способ на современной asio | Тянет **Boost.Context** в boost-free проект |
| **B.** Модернизация `coro` на месте | Оставить свой `Fiber`, выкинуть токены-строки и `std::function`, перейти на современные API asio | Свой stackful-движок остаётся (но он и так уже есть и работает) |
| **C.** `awaitable<T>` / `co_await` | Лучшее по памяти (stackless) | **Вирусно**: меняется код всех потребителей (~30 файлов) |

Выбран **вариант B**, потому что он:

- **не добавляет Boost** (остаёмся `ASIO_STANDALONE`, без новых системных зависимостей);
- **не вирусный** — публичный API `coro` не меняется, потребители (`src/`, `lib/`) трогать
  не нужно;
- при этом решает реальные проблемы (строковые токены, `std::function`, ручной `cancel`,
  устаревшие API asio).

Стэкфул-`Fiber` (ucontext / Windows Fibers) сознательно **оставлен как есть** — он рабочий,
и его замена не входила в задачу.

---

## 5. Ядро: новая модель `Coro`

Это центральное изменение. Строковые токены заменены на **identity-токен** — обычный
`const void*`, равный адресу объекта-инициатора ожидания. Особый случай «жду только
исключения» закодирован нулевым токеном. А «можно ли меня прервать заброшенным
исключением» вынесено в явный флаг `interruptible`.

### 5.1. Новый тип токена и публичный интерфейс

```cpp
// СТАЛО: Coro.h
/// Токен пробуждения корутины: адрес объекта-инициатора ожидания.
using WaitToken = const void*;

class Coro {
public:
    static Coro* current();

    explicit Coro(std::function<void()> routine);

    void start();
    void suspend(WaitToken token, bool interruptible = true);
    void wake(WaitToken token);

    void propagateException(std::exception_ptr exception);
    template <typename Exception>
    void propagateException(Exception exception) {
        propagateException(std::make_exception_ptr(std::move(exception)));
    }
    void cancel();

    void rethrowPendingException();
    const std::list<std::exception_ptr>& exceptions() const { return _exceptions; }
    bool done() const { return _state == State::Done; }

private:
    enum class State { NotStarted, Suspended, Running, Done };
    void switchIn();

    std::function<void()> _routine;
    Fiber _fiber;
    Coro* _previousCoro = nullptr;
    std::list<std::exception_ptr> _exceptions;
    State _state = State::NotStarted;
    WaitToken _waitToken = nullptr;
    bool _interruptible = false;

public:
    void run();
};
```

Что изменилось по сравнению со старым `Coro`:

- `resume(const std::string&)` → `wake(WaitToken)` — «разбуди корутину, если она ждёт
  именно это событие».
- `yield(std::vector<std::string>)` → `suspend(WaitToken, bool interruptible)` — «усыпи
  текущую корутину».
- Появилось **явное состояние** `State { NotStarted, Suspended, Running, Done }`. Раньше
  состояние было неявным (выводилось из содержимого `_tokens`), что было хрупко.
- Вместо `std::vector<std::string> _tokens` теперь два дешёвых поля: `WaitToken _waitToken`
  и `bool _interruptible`.

### 5.2. Как идентификатор-токен заменяет строку

Раньше «кто кого будит» определялось совпадением строк. Теперь — совпадением адресов.
Каждый примитив передаёт в качестве токена свой `this`:

```cpp
// СТАЛО: Mutex.cpp — засыпаем на адресе мьютекса
Coro::current()->suspend(this);
...
// и будим того, кто ждёт именно этот мьютекс
_coros.front()->wake(this);
```

Адрес объекта уникален, пока объект жив, поэтому он идеально подходит на роль токена и не
требует ни аллокаций, ни форматирования, ни сравнения строк.

### 5.3. `suspend` — усыпление корутины

```cpp
// СТАЛО: Coro.cpp
void Coro::suspend(WaitToken token, bool interruptible) {
    if (interruptible) {
        rethrowPendingException();   // (1) если исключение уже ждёт — бросаем сразу
    }

    _waitToken = token;              // (2) запоминаем, на что нас можно разбудить
    _interruptible = interruptible;
    _state = State::Suspended;

    if (_previousCoro) {             // (3) переключаемся обратно к тому, кто нас запустил
        _fiber.switchTo(_previousCoro->_fiber);
    } else {
        _fiber.exit();
    }

    _state = State::Running;         // (4) сюда мы попадаем уже после пробуждения
    _waitToken = nullptr;
    _interruptible = false;

    if (interruptible) {
        rethrowPendingException();   // (5) проснулись — снова проверяем, не пора ли бросить
    }
}
```

Ключевые моменты:

- **`interruptible`** — это бывшее «есть ли `TokenThrow` в списке токенов». Если `true`, то
  заброшенное в корутину исключение (таймаут, `cancel`) прервёт ожидание. Проверка делается
  дважды — до засыпания (5.3.1) и после пробуждения (5.3.5), ровно как в старом `yield`.
- **`token == nullptr`** означает «жду только инъекции исключения, обычным `wake` меня не
  будить» — это бывшее `yield({TokenThrow})`.
- Сама механика переключения стека (`switchTo`/`exit`/`_previousCoro`) **дословно
  перенесена** из старого `yield`, чтобы не сломать тонкости работы со стэкфул-фибрами.

### 5.4. `wake` — пробуждение корутины

```cpp
// СТАЛО: Coro.cpp
void Coro::wake(WaitToken token) {
    // Нулевой токен зарезервирован под "ожидание только исключения" и wake() не пробуждается.
    if (token == nullptr) {
        return;
    }
    if (_state == State::Suspended && _waitToken == token) {
        switchIn();
    }
}
```

Это прямой аналог старого `resume`, но защита от чужих пробуждений теперь — сравнение
указателей вместо `std::find` по вектору строк:

```cpp
// БЫЛО: Coro.cpp
void Coro::resume(const std::string& token) {
    if (std::find(_tokens.begin(), _tokens.end(), token) == _tokens.end()) {
        return;   // не нас будят — игнорируем
    }
    ... переключение ...
}
```

Логика «если корутина приостановлена именно на этом токене — войти в неё, иначе
проигнорировать» сохранена один-в-один, изменился лишь способ сравнения.

### 5.5. `switchIn` — выделенное переключение в корутину

Раньше код переключения был «вшит» в `resume`. Теперь он вынесен в приватный `switchIn()`,
потому что им пользуются три места: `start`, `wake` и `propagateException`. Тело —
дословно из старого `resume`:

```cpp
// СТАЛО: Coro.cpp
void Coro::switchIn() {
    _previousCoro = t_currentCoro;
    t_currentCoro = this;
    if (_previousCoro) {
        _previousCoro->_fiber.switchTo(_fiber);
    } else {
        _fiber.enter();
    }
    t_currentCoro = _previousCoro;
    _previousCoro = nullptr;
}

void Coro::start() {
    assert(_state == State::NotStarted);
    switchIn();
}
```

### 5.6. `propagateException` — заброс исключения в корутину

```cpp
// СТАЛО: Coro.cpp
void Coro::propagateException(std::exception_ptr exception) {
    assert(exception);
    _exceptions.push_back(exception);

    // Если корутина сейчас приостановлена и готова принять исключение — будим её немедленно.
    // Иначе исключение дождётся ближайшей прерываемой приостановки.
    if (_state == State::Suspended && _interruptible) {
        switchIn();
    }
}
```

Сравните со старым кодом, где роль «готова ли принять исключение» играло наличие строки
`TokenThrow` в списке токенов:

```cpp
// БЫЛО: Coro.cpp
void Coro::propagateException(std::exception_ptr exception) {
    _exceptions.push_back(exception);
    resume(TokenThrow);   // войдёт, только если _tokens содержит TokenThrow
}
```

Семантика идентична: исключение кладётся в очередь `_exceptions`; если корутина прямо сейчас
ждёт прерываемо — её будим, и она выбросит исключение (через `rethrowPendingException` в
`suspend`). Если ждёт непрерываемо или ещё бежит — исключение полежит в очереди и
«выстрелит» при следующем прерываемом `suspend`.

### 5.7. Завершение корутины

Старый `run()` заканчивался «вечным» `yield({})` (токенов нет → никто не разбудит). Теперь
явно выставляется `State::Done`, что делает дальнейшие `wake`/`propagateException`
безопасными no-op'ами:

```cpp
// СТАЛО: Coro.cpp
void Coro::run() {
    _state = State::Running;
    try {
        _routine();
    }
    catch (const CancelError&) { /* do nothing */ }
    catch (...) {
        _exceptions.push_front(std::current_exception());
    }
    _routine = nullptr;
    _state = State::Done;

    // Финальное переключение обратно к инициатору. Сюда мы больше не вернёмся.
    if (_previousCoro) {
        _fiber.switchTo(_previousCoro->_fiber);
    } else {
        _fiber.exit();
    }
}
```

---

## 6. Асинхронные операции: `cancellation_slot` вместо callback-токенов

`AsioTask` был переписан полностью. Вместо `std::function`-колбэка со строковым токеном и
`handle.cancel()` теперь используется небольшой helper, который:

1. привязывает к completion-handler'у asio **`cancellation_slot`**;
2. при заброшенном в корутину исключении отменяет **именно эту** операцию через
   `signal.emit(cancellation_type::terminal)`.

### 6.1. Новый helper

```cpp
// СТАЛО: AsioTask.h
namespace detail {

template <typename... Results>
class AsyncOp {
public:
    // Completion handler для передачи в инициирующую функцию asio.
    auto handler() {
        return asio::bind_cancellation_slot(_signal.slot(),
            [this](const std::error_code& errorCode, Results... results) {
                _finished = true;
                _errorCode = errorCode;
                _results = std::tuple<Results...>(std::move(results)...);
                _coro->wake(this);          // токен = адрес самого AsyncOp
            });
    }

    void wait() {
        try {
            _coro->suspend(this, /* interruptible = */ true);
        }
        catch (...) {
            auto exception = std::current_exception();
            _signal.emit(asio::cancellation_type::terminal);  // точечная отмена ОПЕРАЦИИ
            // Ждём фактического завершения, чтобы handler не обратился к уничтоженному AsyncOp.
            _coro->suspend(this, /* interruptible = */ false);
            assert(_finished);
            std::rethrow_exception(exception);
        }
        if (_errorCode) {
            throw std::system_error(_errorCode);
        }
    }

    template <std::size_t I>
    auto&& result() { return std::get<I>(std::move(_results)); }

private:
    Coro* _coro = Coro::current();
    asio::cancellation_signal _signal;
    std::error_code _errorCode;
    std::tuple<Results...> _results;
    bool _finished = false;
};

} // namespace detail

// Операция без возвращаемого значения, handler (const std::error_code&).
template <typename Initiate>
void awaitOp(Initiate&& initiate) {
    detail::AsyncOp<> op;
    initiate(op.handler());
    op.wait();
}

// Операция с одним возвращаемым значением, handler (const std::error_code&, Result).
template <typename Result, typename Initiate>
Result awaitValue(Initiate&& initiate) {
    detail::AsyncOp<Result> op;
    initiate(op.handler());
    op.wait();
    return op.template result<0>();
}
```

Почему так:

- **`bind_cancellation_slot`** — это и есть «современный `handle.cancel()`, но адресный».
  Когда мы делаем `_signal.emit(terminal)`, отменяется ровно та операция, к чьему handler'у
  привязан слот, а не весь сокет.
- **`AsyncOp` лежит на стеке корутины**, и handler держит на него `this`. Это безопасно,
  потому что на пути отмены мы вторым `suspend(this, false)` дожидаемся фактического вызова
  handler'а (с `operation_aborted`), прежде чем выйти из `wait()` и разрушить `AsyncOp`.
  Эта двухфазность («отменить → дождаться колбэка → перебросить исключение») — ровно та же,
  что была в старом `doWait`, только без строковых токенов.
- **`std::function` исчез**: handler — это лямбда, обёрнутая `bind_cancellation_slot`, без
  type-erasure.

### 6.2. Как это выглядит в `Stream`

Высокоуровневый код стал короче и единообразнее. Сравните:

```cpp
// БЫЛО: Stream.h
template <typename ...T>
size_t read(T&&... t) {
    AsioTask2<size_t> task;
    asio::async_read(_handle, asio::buffer(std::forward<T>(t)...), task.callback());
    return task.wait(_handle);
}
```

```cpp
// СТАЛО: Stream.h
template <typename ...T>
size_t read(T&&... t) {
    return awaitValue<size_t>([&](auto&& handler) {
        asio::async_read(_handle, asio::buffer(std::forward<T>(t)...),
                         std::forward<decltype(handler)>(handler));
    });
}
```

Тот же приём применён в `StreamSocket::connect`, `Acceptor::accept`,
`DatagramSocket::send/receive`, `Timer`, `SignalSet`, `Resolver`.

---

## 7. Перевод примитивов на новую модель

Все примитивы синхронизации перешли с `yield({token(), TokenThrow})` / `resume(token())` на
`suspend(this)` / `wake(this)`. Ниже — самые показательные.

### 7.1. `Mutex`

```cpp
// СТАЛО: Mutex.cpp
void Mutex::lock() {
    if (_owner) {
        Finally cleanup([&] {
            _coros.remove(Coro::current());
        });
        _coros.push_back(Coro::current());
        Coro::current()->suspend(this);     // было: yield({token(), TokenThrow})
    }
    assert(_owner == nullptr);
    _owner = Coro::current();
}

void Mutex::unlock() {
    assert(_owner == Coro::current());
    _owner = nullptr;
    if (!_coros.empty()) {
        _coros.front()->wake(this);         // было: resume(token())
    }
}
```

Метод `token()`, который раньше форматировал строку `"Mutex 0x..."`, удалён полностью —
вместо него просто `this`.

### 7.2. `Timeout`

`Timeout` ставит таймер asio; по срабатыванию он забрасывает в корутину `TimeoutError`.
Здесь два изменения: `expires_from_now` → `expires_after` (удалённый API) и токены →
`wake`/`suspend`.

```cpp
// СТАЛО: Timeout.h
template <typename Duration>
Timeout(Duration duration): _timer(IoService::current()->_impl) {
    _timer.expires_after(duration);                  // было: expires_from_now
    _timer.async_wait([this](const std::error_code& errorCode) {
        _callbackExecuted = true;
        if (_timerCanceled) {
            return _coro->wake(this);                // было: resume(token())
        }
        if (errorCode) {
            return _coro->propagateException(std::system_error(errorCode));
        }
        _coro->propagateException(TimeoutError(this));
    });
}

~Timeout() {
    if (!_callbackExecuted) {
        _timerCanceled = true;
        _timer.cancel();
        // Ждём фактического вызова колбэка таймера (operation_aborted), не принимая прерываний.
        _coro->suspend(this, /* interruptible = */ false);   // было: yield({token()})
    }
}
```

Обратите внимание на `suspend(this, false)` в деструкторе: при отмене таймаута мы должны
**дождаться** колбэка таймера, но при этом **не** хотим, чтобы нас в этот момент прервало
чужое исключение — поэтому `interruptible = false`. Это бывшее `yield({token()})` (где
не было `TokenThrow`).

### 7.3. `CheckPoint`

`CheckPoint` — это «точка справедливости»: корутина уступает управление циклу событий, давая
ему обработать накопившиеся задачи, и просыпается обратно. Уникальный токен здесь — адрес
самой корутины (в каждый момент активна максимум одна контрольная точка на корутину):

```cpp
// СТАЛО: CheckPoint.cpp
void CheckPoint() {
    auto coro = Coro::current();
    IoService::current()->post([coro] {
        IoService::current()->checkpoints.push([coro] {
            coro->wake(coro);          // токен = сама корутина
        });
    });
    coro->suspend(coro);               // было: yield({CheckPointToken, TokenThrow})
}
```

### 7.4. `CoroPool`

`CoroPool` управляет группой дочерних корутин. Здесь токен — адрес самого пула, а флаг
`interruptible` напрямую выражает прежнюю разницу между `{token(), TokenThrow}` и
`{token()}`:

```cpp
// СТАЛО: CoroPool.cpp
void CoroPool::waitAll(bool noThrow) {
    if (_childCoros.empty()) {
        return;
    }
    // noThrow == true  -> ждём непрерываемо  (бывшее {token()})
    // noThrow == false -> ждём прерываемо    (бывшее {token(), TokenThrow})
    _parentCoro->suspend(this, /* interruptible = */ !noThrow);
    assert(_childCoros.empty());
}

void CoroPool::cancelAll() {
    for (auto coro : _childCoros) {
        if (_childCoros.find(coro) != _childCoros.end()) {
            coro->propagateException(CancelError());
        }
    }
}
```

А при завершении дочерней корутины пул, как и раньше, **откладывает** разбор через
`IoService::post` (нельзя удалять `Coro` прямо на её собственном стеке) и в конце будит
родителя:

```cpp
// СТАЛО: CoroPool.cpp
void CoroPool::onCoroDone(Coro* childCoro) {
    IoService::current()->post([=] {
        while (childCoro->exceptions().size()) {
            try {
                childCoro->rethrowPendingException();   // было: childCoro->propagateException()
            }
            catch (const CancelError&) { /* отмену не пробрасываем в родителя */ }
            catch (...) {
                _parentCoro->propagateException(std::current_exception());
            }
        }
        _childCoros.erase(childCoro);
        delete childCoro;
        if (_childCoros.empty()) {
            _parentCoro->wake(this);                    // было: resume(token())
        }
    });
}
```

(Бывший no-arg `Coro::propagateException()`, который «вытащить и перебросить ближайшее
исключение», получил говорящее имя `rethrowPendingException()`.)

---

## 8. Обновления API самой asio (1.36)

Часть правок — не про модель корутин, а про то, что в 1.36 удалены старые API. Их пришлось
заменить на современные эквиваленты.

### 8.1. `io_service` → `io_context`, member-`post` → свободные функции

```cpp
// СТАЛО: IoService.h
class IoService {
public:
    static IoService* current();
    void run();

    template <typename T> void post(T&& t)     { asio::post(_impl, std::forward<T>(t)); }
    template <typename T> void dispatch(T&& t) { asio::dispatch(_impl, std::forward<T>(t)); }

    std::queue<std::function<void()>> checkpoints;
    asio::io_context _impl;     // было: asio::io_service
};
```

`asio::io_service` удалён (это был лишь deprecated-алиас `io_context`), а member-функции
`io_service::post/dispatch` — тоже. Современный способ — свободные `asio::post(ctx, h)` /
`asio::dispatch(ctx, h)`. Публичная сигнатура `IoService::post/dispatch` при этом
**не изменилась**, так что потребители ничего не заметили.

### 8.2. `io_service::work` → `executor_work_guard`

```cpp
// СТАЛО: Work.h
class Work {
private:
    asio::executor_work_guard<asio::io_context::executor_type> _impl =
        asio::make_work_guard(IoService::current()->_impl);   // было: asio::io_service::work(...)
};
```

`Work` держит цикл событий «живым», пока есть незавершённая работа. Класс `io_service::work`
удалён; современный аналог — `executor_work_guard` через `make_work_guard`.

### 8.3. `Resolver`: `query`/`iterator` удалены → современный `async_resolve`

Это самое заметное изменение API на уровне сигнатуры (но `Resolver` используется только
внутри `coro` и в тестах, поэтому потребители не затронуты):

```cpp
// СТАЛО: Resolver.h
template <typename InternetProtocol>
class Resolver {
public:
    typedef asio::ip::basic_resolver<InternetProtocol> Impl;
    typedef typename Impl::results_type Results;       // было: Impl::iterator / Impl::query

    Resolver(): _handle(IoService::current()->_impl) {}

    Results resolve(const InternetProtocol& protocol,
                    const std::string& host, const std::string& service) {
        return awaitValue<Results>([&](auto&& handler) {
            _handle.async_resolve(protocol, host, service,
                                  std::forward<decltype(handler)>(handler));
        });
    }
private:
    Impl _handle;
};
```

В asio 1.36 `basic_resolver::query` и `basic_resolver::iterator` удалены. Современный
`async_resolve(protocol, host, service)` возвращает `results_type` — диапазон, по которому
можно итерироваться (`results.begin()->endpoint()`).

---

## 9. Адаптация юнит-тестов

Большинство тестов (TCP/UDP/CheckPoint/Queue) пользуются высокоуровневым API и **не
менялись по сути** — только `address::from_string` → `make_address` (тоже удалённый в 1.36
API).

Но несколько тестов сознательно дёргали **внутренний** token-API напрямую (это нормально —
это белоящичные тесты самого механизма). Их пришлось перевести на новую модель. Например,
тест мьютекса, который вручную имитировал внешнее пробуждение:

```cpp
// БЫЛО: TestMutex.cpp
Coro::current()->yield({"test"});
...
coro1.resume("test");
```

```cpp
// СТАЛО: TestMutex.cpp
int token;   // адрес локальной переменной как токен ручного пробуждения
...
Coro::current()->suspend(&token);
...
coro1.wake(&token);
```

А `yield({TokenThrow})` (засыпание «жду только исключения/отмены») превратилось в
`suspend(nullptr)`:

```cpp
// БЫЛО: TestCoro.cpp / TestTimeout.cpp / TestCoroPool.cpp
Coro::current()->yield({TokenThrow});
```

```cpp
// СТАЛО:
Coro::current()->suspend(nullptr);
```

---

## 10. Что НЕ менялось (граница невирусности)

Главное обещание этой переделки — **потребители `coro` не трогаются**. Это было проверено
поиском по `src/`, `lib/`, `nn/`:

- весь продуктовый код использует только высокоуровневый API (`Application`, `CoroPool`,
  `Timeout`, `Timer`, `StreamSocket`, `Acceptor`, `CheckPoint`, `Finally`, `SignalSet`,
  `IoService`), сигнатуры которого сохранены **байт-в-байт**;
- низкоуровневый `Coro::yield/resume`/токены нигде вне `coro` и тестов не использовались;
- `VisitorInterpreter.cpp` лишь `#include <coro/AsioTask.h>`, но не использует его классы —
  достаточно, чтобы заголовок продолжал компилироваться (он компилируется).

Единственная правка вне `coro` — следствие самого бампа asio, а не смены модели корутин:

```cpp
// src/testo/Utils.cpp
// было: asio::ip::address::from_string(ip)
return asio::ip::tcp::endpoint(asio::ip::make_address(ip), uport);
```

Также **намеренно не менялись**:

- `Fiber` (ucontext / Windows Fibers) — рабочий стэкфул-движок оставлен как есть;
- `Application`, `Finally`, `IoService::run()` (цикл `run_one`/`poll_one`/`checkpoints`);
- C++-стандарт проекта остался **C++17**: вариант B не требует C++20 (он нужен был только
  для отклонённого `awaitable`-подхода), а `cancellation_slot` прекрасно работает в C++17.

---

## 11. Проверка и результаты

Библиотека `coro` и её тесты собраны и прогнаны изолированно (сборка повторяет CMake-таргеты
`coro` + `coro_tests`: те же исходники, те же флаги, линковка с `pthread`):

- **Сборка чистая** под флагами проекта `-Wall -Wextra -Wpedantic` — без предупреждений в
  коде `coro`.
- **Тесты проходят** и в Debug (с включёнными `assert`), и в Release (`-O2 -DNDEBUG`):
  **40 assertions в 26 test cases**, стабильно на повторных прогонах.

Покрыты, в частности: базовые корутины и вложенность, отмена и заброс исключений, `Mutex`,
`Queue` (включая отмену потребителя), `CoroPool` (ожидание/отмена), все сценарии `Timeout`
(в т.ч. таймаут на `Queue`/`Acceptor`/TCP/UDP), TCP-эхо клиент-сервер, UDP, резолвинг,
`CheckPoint`.

> Замечание про более широкую сборку: `src/`, `lib/`, `nn/` в этом окружении не собираются
> из-за внешних зависимостей (qemu/hyperv/onnx и т.п.), поэтому их компиляция против asio
> 1.36 здесь не проверялась. Сканирование показало, что из удалённых в 1.36 API вне `coro`
> встречался только `address::from_string` (в одном месте, исправлено); `io_service`,
> `expires_from_now`, `resolver::query` не используются нигде.

---

## 12. Краткая шпаргалка по соответствию старого и нового API

| Старое (token-модель) | Новое (identity + interruptible) |
|---|---|
| `coro->yield({obj->token(), TokenThrow})` | `coro->suspend(obj /* interruptible=true */)` |
| `coro->yield({obj->token()})` | `coro->suspend(obj, /*interruptible=*/false)` |
| `coro->yield({TokenThrow})` | `coro->suspend(nullptr)` |
| `coro->resume(obj->token())` | `coro->wake(obj)` |
| `coro->resume(TokenStart)` / стартовый токен | `coro->start()` + `State::NotStarted` |
| `coro->propagateException()` (no-arg) | `coro->rethrowPendingException()` |
| `std::string token()` в каждом примитиве | просто `this` |
| `AsioTask1/2` + `std::function` + `handle.cancel()` | `awaitOp` / `awaitValue` + `cancellation_slot` |
| `asio::io_service` | `asio::io_context` |
| `asio::io_service::work` | `asio::executor_work_guard` (`make_work_guard`) |
| `steady_timer::expires_from_now` | `steady_timer::expires_after` |
| `basic_resolver::query` / `iterator` | `async_resolve(protocol, host, service)` → `results_type` |
| `asio::ip::address::from_string` | `asio::ip::make_address` |
