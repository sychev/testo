# `lib/net`: подробный разбор каждого класса и почти каждой строки

> Это «глубокий» технический разбор библиотеки `lib/net`, которая заменила
> стековые корутины `coro`. Документ написан для подготовки к детальному
> техническому разбору (code review / собеседование): по каждому файлу идёт
> построчное объяснение **что** делает строка и **почему так, а не иначе**, плюс
> врезки по концепциям и отдельный раздел с каверзными вопросами и ответами.
>
> Если вы ещё не читали обзорный документ
> [«Удаление Coro и переход на net»](./Coro%20to%20net%20migration.md) — начните
> с него: там общая картина. Здесь — детали реализации.

## Оглавление

- [0. Общая модель за 2 минуты](#0-общая-модель-за-2-минуты)
- [1. Базовые концепции (нужны для всего остального)](#1-базовые-концепции-нужны-для-всего-остального)
- [2. `Cancel.hpp` / `Cancel.cpp` — отмена](#2-cancelhpp--cancelcpp--отмена)
- [3. `Deadline.hpp` / `Deadline.cpp` — таймауты](#3-deadlinehpp--deadlinecpp--таймауты)
- [4. `CheckPoint.hpp` — точка отмены](#4-checkpointhpp--точка-отмены)
- [5. `IoPump.hpp` — сердце библиотеки](#5-iopumphpp--сердце-библиотеки)
- [6. `Stream.hpp` — синхронный поток](#6-streamhpp--синхронный-поток)
- [7. `Socket.hpp` — сокет](#7-sockethpp--сокет)
- [8. `Acceptor.hpp` — приём соединений](#8-acceptorhpp--приём-соединений)
- [9. `Timer.hpp` — прерываемый сон](#9-timerhpp--прерываемый-сон)
- [10. `SignalGuard.hpp` / `.cpp` — сигналы](#10-signalguardhpp--cpp--сигналы)
- [11. `Finally.hpp` — scope guard](#11-finallyhpp--scope-guard)
- [12. `CMakeLists.txt`](#12-cmakeliststxt)
- [13. Сквозной пример: что происходит при `socket.read()`](#13-сквозной-пример-что-происходит-при-socketread)
- [14. Каверзные вопросы «на допросе»](#14-каверзные-вопросы-на-допросе)
- [15. Ссылки](#15-ссылки)

---

## 0. Общая модель за 2 минуты

Coro давала «синхронно выглядящий» код, переключая **фиберы** (нити со своим
стеком) на точках ввода/вывода. `lib/net` достигает того же удобства иначе: код
действительно **блокирует текущий поток**, но вместо «тупого» системного вызова
`recv()` мы крутим `asio::io_context` маленькими порциями и между ними проверяем
два условия — **запрошена ли отмена** и **не истёк ли таймаут**. Если да —
бросаем исключение.

Три кита:

1. **`IoPump`** — превращает асинхронную операцию asio в блокирующий вызов с
   поддержкой отмены и дедлайна. Всё остальное построено на нём.
2. **`Cancel`** (`std::stop_source`) — глобальный «рычаг» отмены, который дёргает
   обработчик сигналов.
3. **`Deadline`** (thread-local) — «окружающий» таймаут на область видимости.

`Stream`/`Socket`/`Acceptor`/`Timer` — тонкие обёртки поверх `IoPump`.
`SignalGuard`/`Finally` — вспомогательные.

---

## 1. Базовые концепции (нужны для всего остального)

Эти понятия используются во всех файлах. Если знаете — листайте дальше.

### 1.1. `asio::io_context`

`io_context` — это **очередь готовых к выполнению колбэков** плюс обёртка над
механизмом ОС ожидания готовности дескрипторов (на Linux — `epoll`, на Windows —
IOCP). Асинхронная операция (`async_read`, `async_connect`, ...) **ничего не
ждёт**: она регистрирует интерес к событию и сразу возвращает управление. Колбэк
(**completion handler**) выполнится позже — и только когда кто-то «крутит»
`io_context`:

- `io.run()` — выполняет колбэки, **пока есть работа**; возвращается, когда
  работы не осталось (или вызвали `stop()`).
- `io.run_one()` — выполнит максимум **один** колбэк и вернётся.
- `io.run_one_for(d)` — как `run_one`, но ждёт не дольше `d`; если за это время
  ничего не готово — вернёт `0`.
- `io.restart()` — сбрасывает внутренний флаг «остановлен», который выставляется,
  когда `run()` вернулся из-за отсутствия работы. Без `restart()` следующий
  `run*()` сразу вернётся, ничего не сделав.

> Документация: [asio io_context](https://think-async.com/Asio/asio-1.28.0/doc/asio/reference/io_context.html).

**Важно про потокобезопасность:** один `io_context` нельзя «крутить» из двух
потоков одновременно без синхронизации, и сокет нельзя использовать из двух
потоков сразу. В `lib/net` мы это обходим тем, что **у каждого сокета свой
`io_context`**, и его в любой момент времени трогает только один поток.

### 1.2. Completion handler и его сигнатура

Колбэк, который asio вызовет по завершении. Сигнатура зависит от операции:

- `async_connect`, `async_wait`, `async_accept` → `void(std::error_code)`.
- `async_read`, `async_write`, `async_read_some` → `void(std::error_code, std::size_t)`
  (второй аргумент — сколько байт передано).

`std::error_code` — это «код ошибки без исключения»: пара {категория, число}.
`if (ec)` истинно, если произошла ошибка. Мы превращаем её в `std::system_error`
там, где удобнее работать с исключениями.

> [std::error_code](https://en.cppreference.com/w/cpp/error/error_code),
> [std::system_error](https://en.cppreference.com/w/cpp/error/system_error).

### 1.3. Фибер vs наш подход (одной картинкой)

```
Coro (фибер):                          net (IoPump):
  read() {                               read() {
    async_read(...);                       async_read(..., set done);
    yield();   // переключить стек          while(!done) {
  }            // другой фибер бежит          io.run_one_for(20ms); // блокируем ЭТОТ поток
               // колбэк -> resume()          // проверить отмену/таймаут
                                            }
                                          }
```

Coro переключала **стек** (дёшево, но сложно и однопоточно). `net` просто
**блокирует поток** в `run_one_for`. Когда нужна конкуренция — берём настоящие
потоки ОС.

---

## 2. `Cancel.hpp` / `Cancel.cpp` — отмена

Отвечает за кооперативную отмену: «попросить весь код в процессе завершиться»
(Ctrl-C, SIGTERM, остановка сервиса).

### 2.1. `Cancel.hpp` построчно

```cpp
#pragma once                    // (1)

#include <stop_token>           // (2)

namespace net {

struct Interruption {};         // (3)

std::stop_source& interrupt_source();                                  // (4)

inline std::stop_token interrupt_token() {                             // (5)
    return interrupt_source().get_token();
}

inline bool interrupt_requested() {                                    // (6)
    return interrupt_source().stop_requested();
}

inline void request_interrupt() {                                      // (7)
    interrupt_source().request_stop();
}

void reset_interrupt();                                                // (8)

}
```

**(1) `#pragma once`** — защита от повторного включения заголовка. Не стандарт, но
поддерживается всеми компиляторами проекта; короче и быстрее классических
include-guard'ов.
[Подробнее](https://en.cppreference.com/w/cpp/preprocessor/impl#.23pragma_once).

**(2) `#include <stop_token>`** — заголовок C++20 с `std::stop_source` /
`std::stop_token`. Это стандартный механизм **кооперативной** отмены: один
объект-источник умеет «запросить остановку», а токены — её «увидеть».
[std::stop_token](https://en.cppreference.com/w/cpp/thread/stop_token).

**(3) `struct Interruption {};`** — пустой тип-исключение для отмены. Два ключевых
решения:
- **Почему вообще отдельный тип, а не `bool`-проверки везде?** Чтобы отмена
  «раскручивала» стек как исключение: вызвали — и управление само ушло наверх
  через все RAII-деструкторы.
- **Почему НЕ наследуем от `std::exception`?** В интерпретаторе очень много
  `catch (const std::exception&)`. Если бы `Interruption` была наследником
  `std::exception`, первый такой `catch` её бы «проглотил», и Ctrl-C не дошёл бы
  до `main`. Не будучи `std::exception`, наша отмена **проходит сквозь** такие
  блоки и доходит до верхнего обработчика. Это сознательное повторение поведения
  старого `coro::CancelError` (он по той же причине не наследовался от
  `std::exception`).

**(4) `std::stop_source& interrupt_source();`** — доступ к единственному
процесс-глобальному источнику отмены. Возвращаем **ссылку** (не значение), потому
что `stop_source` шарится: все читатели должны видеть один и тот же объект.
Определение — в `.cpp` (см. ниже), чтобы был ровно один экземпляр на всю
программу (иначе при включении заголовка в разные единицы трансляции получили бы
разные `static`-объекты).

**(5) `interrupt_token()`** — выдаёт `stop_token`. Нужен, если кто-то хочет
интегрироваться со `std::jthread` или `std::condition_variable_any`, которые
умеют просыпаться по `stop_token`. `inline`, потому что определение в заголовке
(иначе нарушение ODR при включении в несколько `.cpp`).

**(6) `interrupt_requested()`** — «попросили ли уже остановиться?». Это **горячая
проверка**: её зовут в `IoPump` каждые ~20 мс и в `check_point()`. `stop_requested()`
— дешёвая атомарная операция, потокобезопасная по стандарту.

**(7) `request_interrupt()`** — «попросить остановиться». Зовёт обычно обработчик
сигналов (`SignalGuard`). `request_stop()` потокобезопасен и идемпотентен.

**(8) `reset_interrupt()`** — сбросить запрос отмены. `stop_source` **необратим**:
раз запрошенную остановку нельзя «отозвать». Поэтому «сброс» = замена источника
новым (см. `.cpp`). Нужно в основном для тестов и для гипотетического повторного
запуска. В рабочем коде `testo` отмена означает «процесс завершается», так что
сбрасывать не приходится.

### 2.2. `Cancel.cpp` построчно

```cpp
#include <net/Cancel.hpp>

namespace net {

std::stop_source& interrupt_source() {
    static std::stop_source source;     // (1)
    return source;
}

void reset_interrupt() {
    interrupt_source() = std::stop_source();   // (2)
}

}
```

**(1) `static std::stop_source source;`** — функция-локальный `static`. Тонкость,
которую любят спрашивать: **инициализация функции-локального `static`
потокобезопасна** по стандарту C++11 (так называемые «magic statics»: компилятор
оборачивает первую инициализацию в потокобезопасную проверку). Поэтому даже если
два потока впервые вызовут `interrupt_source()` одновременно — `source`
сконструируется ровно один раз.
[Подробнее про потокобезопасность статиков](https://en.cppreference.com/w/cpp/language/storage_duration#Static_local_variables).

Почему функция-локальный статик, а не глобальная переменная на уровне
пространства имён? Чтобы избежать **проблемы порядка инициализации статиков между
единицами трансляции** (static initialization order fiasco): глобальные объекты в
разных `.cpp` инициализируются в неопределённом порядке, а функция-локальный
статик инициализируется лениво — при первом обращении, что гарантированно
безопасно.
[Static init order fiasco](https://en.cppreference.com/w/cpp/language/siof).

**(2) `interrupt_source() = std::stop_source();`** — присваиваем источнику новый,
ещё «не остановленный» источник. Это **инвалидирует прежние токены** (они теперь
смотрят на старый источник). Поскольку в продакшене мы не держим долгоживущих
токенов, это безопасно; в тестах удобно «начать с чистого листа».

> ⚠️ Честная оговорка для допроса: `reset_interrupt()` сам по себе **не**
> потокобезопасен относительно одновременных `request_interrupt()` (мы заменяем
> объект целиком). Его вызывают только в «спокойных» точках (между тестами/в
> тестах), не во время гонок. Если бы потребовалась полная потокобезопасность —
> следовало бы защитить мьютексом или хранить `shared_ptr<std::stop_source>`.

---

## 3. `Deadline.hpp` / `Deadline.cpp` — таймауты

Реализует «окружающий» таймаут: создаёшь `Deadline` в начале блока, и любой
блокирующий вызов внутри начинает учитывать дедлайн. Прямая замена `coro::Timeout`.

### 3.1. `Deadline.hpp` построчно

```cpp
#include <chrono>
#include <optional>
#include <stdexcept>

namespace net {

using Clock = std::chrono::steady_clock;                 // (1)

class TimeoutError: public std::runtime_error {          // (2)
public:
    TimeoutError(): std::runtime_error("Timeout was triggered") {}
};

class Deadline {
public:
    template <typename Duration>
    explicit Deadline(Duration duration) {               // (3)
        push(Clock::now() + std::chrono::duration_cast<Clock::duration>(duration));
    }
    ~Deadline();                                         // (4)

    Deadline(const Deadline&) = delete;                  // (5)
    Deadline& operator=(const Deadline&) = delete;

    static std::optional<Clock::time_point> current();   // (6)
    static bool expired();

private:
    void push(Clock::time_point deadline);               // (7)
};

}
```

**(1) `using Clock = std::chrono::steady_clock;`** — выбираем **монотонные** часы.
Это важно: `steady_clock` никогда не «идёт назад» и не прыгает (в отличие от
`system_clock`, который может скакать при коррекции времени, NTP, переводе часов).
Для измерения интервалов/таймаутов нужны именно монотонные часы — иначе перевод
системного времени мог бы «удлинить» или «сорвать» таймаут.
[Почему steady_clock для таймаутов](https://en.cppreference.com/w/cpp/chrono/steady_clock).

**(2) `class TimeoutError: public std::runtime_error`** — а вот это исключение,
**в отличие от `Interruption`, наследник `std::exception`**. Это сознательная
асимметрия:
- Отмена (Ctrl-C) должна пробивать любые `catch(std::exception&)` и доходить до
  верха → `Interruption` НЕ `std::exception`.
- Таймаут — это «штатная» ошибка операции, которую код часто **хочет** поймать
  локально (например, `GuestAdditions::is_avaliable()` ловит таймаут и возвращает
  `false`). Поэтому `TimeoutError` — обычное `std::runtime_error`.

  Будьте готовы объяснить эту разницу — её любят спрашивать.

**(3) Шаблонный конструктор `Deadline(Duration duration)`** — принимает любой
`std::chrono::duration` (секунды, миллисекунды и т.п.).
- `Clock::now() + duration_cast<Clock::duration>(duration)` — вычисляем
  **абсолютный момент** дедлайна. `duration_cast` приводит переданную длительность
  к «тикам» наших часов (наносекунды у `steady_clock`).
- `explicit` — чтобы случайно не сконструировать `Deadline` из числа в неявном
  преобразовании.
- Почему храним абсолютный `time_point`, а не относительную длительность? Потому
  что проверка «истёк ли» (`now >= deadline`) должна работать в любой момент
  позже, независимо от того, когда её делают.

**(4) `~Deadline();`** — деструктор (определён в `.cpp`) снимает дедлайн со стека.
Это и есть RAII: дедлайн действует ровно в пределах области видимости объекта.

**(5) Копирование запрещено** — `Deadline` управляет записью в thread-local стеке
(пара «push в конструкторе / pop в деструкторе»). Копия привела бы к двойному
`pop` или повисшему элементу. Перемещение тоже не нужно (объект всегда локальный
в области видимости), поэтому его не объявляем — при наличии явного деструктора
move не генерируется автоматически, остаётся только запрещённое копирование.

**(6) `current()` / `expired()`** — статические: дедлайн «окружающий», его читают
из глубины стека вызовов (`IoPump`, `check_point`), не имея ссылки на конкретный
объект `Deadline`. `current()` возвращает `std::optional` — дедлайна может и не
быть (`std::nullopt`).
[std::optional](https://en.cppreference.com/w/cpp/utility/optional).

**(7) `push(...)` приватный** — внутренний помощник конструктора; наружу не нужен.

### 3.2. `Deadline.cpp` построчно

```cpp
#include <net/Deadline.hpp>
#include <vector>

namespace net {

namespace {                                                        // (1)
    thread_local std::vector<Clock::time_point> deadline_stack;    // (2)
}

void Deadline::push(Clock::time_point deadline) {
    if (!deadline_stack.empty() && deadline_stack.back() < deadline) {   // (3)
        deadline = deadline_stack.back();
    }
    deadline_stack.push_back(deadline);                                  // (4)
}

Deadline::~Deadline() {
    if (!deadline_stack.empty()) {                                       // (5)
        deadline_stack.pop_back();
    }
}

std::optional<Clock::time_point> Deadline::current() {
    if (deadline_stack.empty()) {
        return std::nullopt;                                            // (6)
    }
    return deadline_stack.back();                                       // (7)
}

bool Deadline::expired() {
    auto deadline = current();
    return deadline.has_value() && (Clock::now() >= *deadline);         // (8)
}

}
```

**(1) Анонимное пространство имён** — даёт `deadline_stack` **внутреннюю
компоновку** (internal linkage): переменная видна только в этом `.cpp`, не
конфликтует с символами из других файлов. Современная замена `static` для
файловой области.
[Unnamed namespaces](https://en.cppreference.com/w/cpp/language/namespace#Unnamed_namespaces).

**(2) `thread_local std::vector<Clock::time_point> deadline_stack;`** — ключевое
решение. `thread_local` означает «**у каждого потока своя копия**».
- Почему `thread_local`, а не глобально? Дедлайн должен быть «свой» для каждого
  логического потока выполнения: в `nn_server` сотни потоков-соединений, и таймаут
  одного запроса не должен влиять на другой. У Coro дедлайн был привязан к
  корутине; здесь — к потоку.
- Почему **стек** (`vector`), а не одно значение? Чтобы дедлайны **вкладывались**:
  ```cpp
  net::Deadline outer(10s);
  {
      net::Deadline inner(2s);   // внутри действует 2s
      ...
  }                              // снова действует 10s
  ```
[thread_local](https://en.cppreference.com/w/cpp/language/storage_duration#Thread_local_storage).

**(3) «Схлопывание» с родителем** — если уже есть внешний дедлайн (`back()`) и он
**раньше** нового, то вложенный дедлайн не может «продлить» внешний. Берём более
ранний (`deadline = back()`). Инвариант: после этого `back()` — всегда **самый
ранний** из активных дедлайнов, поэтому `current()` = `back()` без перебора всего
стека.

Пример: внешний `2s`, вложенный `10s` → реально вложенный ограничен теми же `2s`
(внешний таймаут «главнее»). Это логично: нельзя внутри более строгого таймаута
попросить более мягкий.

**(4) `push_back(deadline)`** — кладём (уже «схлопнутый») дедлайн на вершину.

**(5) Деструктор: `pop_back()` с проверкой непустоты** — снимаем свой дедлайн.
Проверка `!empty()` — страховка (при корректном RAII стек не может быть пустым в
момент деструктора, но защищаемся от теоретического неправильного использования,
чтобы не словить UB на пустом векторе).

**(6)/(7) `current()`** — нет дедлайнов → `nullopt`; иначе вершина стека (самый
ранний).

**(8) `expired()`** — `has_value() && now >= *deadline`. Короткое замыкание: если
дедлайна нет, `now` даже не сравниваем. `*deadline` разыменовывает `optional`.

> Тонкость для допроса: порядок конструирования/разрушения `Deadline` обязан быть
> строго вложенным (LIFO) — это гарантируется тем, что `Deadline` всегда локальная
> переменная в области видимости. Если бы кто-то завёл `Deadline` в куче и удалял
> в произвольном порядке — инвариант стека сломался бы. Поэтому копирование
> запрещено, а создавать его динамически нет смысла.

---

## 4. `CheckPoint.hpp` — точка отмены

```cpp
#include <net/Cancel.hpp>
#include <net/Deadline.hpp>

namespace net {

inline void check_point() {                  // (1)
    if (interrupt_requested()) {             // (2)
        throw Interruption{};
    }
    if (Deadline::expired()) {               // (3)
        throw TimeoutError{};
    }
}

}
```

**(1) `inline void check_point()`** — свободная функция, `inline` (определена в
заголовке). Её вставляют в **длинные CPU-циклы**, где нет ввода/вывода (ожидание
появления текста на экране, копирование файлов, polling состояния). Без неё такие
циклы не реагировали бы ни на Ctrl-C, ни на таймаут. Прямая замена
`coro::CheckPoint()`.

**(2) Сначала проверяем отмену** — приоритет у Ctrl-C/SIGTERM: если пользователь
прерывает, неважно, истёк ли заодно таймаут — бросаем `Interruption`.

**(3) Потом таймаут** — если активный `Deadline` истёк, бросаем `TimeoutError`.

Почему функция, а не макрос? Чтобы было типобезопасно, отлаживаемо и без сюрпризов
препроцессора. `inline` устраняет накладные расходы вызова.

---

## 5. `IoPump.hpp` — сердце библиотеки

Здесь живёт вся «магия» превращения асинхронной операции в блокирующую с отменой
и дедлайном. Это самый важный файл — на нём держится всё остальное.

```cpp
#include <asio.hpp>
#include <chrono>

#include <net/Cancel.hpp>
#include <net/Deadline.hpp>

namespace net::detail {                                   // (1)

constexpr auto io_quantum = std::chrono::milliseconds(20);   // (2)

template <typename Handle, typename Initiate>             // (3)
void pump(asio::io_context& io, Handle& handle, Initiate&& initiate) {
    io.restart();                                         // (4)

    bool done = false;                                    // (5)
    initiate([&done] { done = true; });                  // (6)

    while (!done) {                                       // (7)
        io.run_one_for(io_quantum);                       // (8)
        if (done) {                                       // (9)
            break;
        }
        if (interrupt_requested()) {                      // (10)
            try { handle.cancel(); } catch (...) {}       // (11)
            io.run();                                     // (12)
            throw Interruption{};                         // (13)
        }
        if (Deadline::expired()) {                        // (14)
            try { handle.cancel(); } catch (...) {}
            io.run();
            throw TimeoutError{};
        }
    }
}

}
```

**(1) `namespace net::detail`** — `detail` по соглашению означает «внутренняя
кухня, не часть публичного API». Пользователи `net` вызывают `socket.read()`, а не
`pump` напрямую. C++17 позволяет писать вложенные пространства имён одной строкой.

**(2) `constexpr auto io_quantum = 20ms;`** — размер «кванта» прокрутки.
**Это центральный компромисс, его обязательно спросят.** Что он означает:
- Когда блокирующая операция реально ждёт данных, мы сидим в `run_one_for(20ms)`,
  то есть в `epoll_wait` с таймаутом 20 мс. Если данные пришли раньше — выходим
  **сразу** (никакой лишней задержки на ввод/вывод). Если ничего не пришло —
  выходим через 20 мс и проверяем отмену/дедлайн.
- Поэтому **20 мс — это максимальная задержка реакции на Ctrl-C/таймаут**, а не
  задержка на каждую операцию.
- Меньше квант → отзывчивее отмена, но чаще «пустые» пробуждения потока. Больше
  квант → реже пробуждения, но медленнее реакция. 20 мс — практичный баланс
  (человек не замечает, а накладные расходы на пробуждения ничтожны).

**(3) Шаблон `pump(io, handle, initiate)`** — обобщённый по двум типам:
- `Handle` — любой asio-объект с методом `cancel()` (сокет, акцептор, таймер,
  windows stream handle). Шаблон позволяет одному `pump` обслуживать их все.
- `Initiate` — вызывающий передаёт лямбду, которая **запускает** конкретную
  async-операцию. `Initiate&&` — universal reference (perfect forwarding), чтобы
  принять лямбду без лишних копий.
- Почему `pump` не знает про конкретную операцию? Разделение ответственности:
  `pump` отвечает только за цикл «крути io + проверяй отмену/дедлайн», а *какую*
  операцию запускать — задаёт вызывающий. Это устраняет дублирование (один цикл на
  read/write/connect/accept/wait).

**(4) `io.restart();`** — **обязательная** строка, частый вопрос «зачем?».
`io_context` после того, как `run()`/`run_one()` вернулся из-за отсутствия работы
(а в нашем коде путь отмены вызывает `io.run()`, строка 12), переходит в
состояние «остановлен». В этом состоянии следующий `run*()` немедленно вернёт `0`,
ничего не сделав. `restart()` сбрасывает этот флаг. Так как один и тот же
`io_context` переиспользуется для многих операций подряд (один сокет — много
read/write), перед каждой операцией нужно «завести» его заново.
[io_context::restart](https://think-async.com/Asio/asio-1.28.0/doc/asio/reference/io_context/restart.html).

**(5) `bool done = false;`** — флаг завершения, живёт на стеке `pump`. Не атомарный
— и не должен быть: `io_context` крутится в **этом же** потоке, колбэк выполнится
синхронно внутри `run_one_for` тем же потоком. Никакой межпоточной гонки нет.

**(6) `initiate([&done]{ done = true; });`** — запускаем операцию. Передаём
вызывающему «продолжение» `on_done` — лямбду `[&done]{done=true;}`. Вызывающий
обязан вызвать `on_done()` из своего asio-колбэка. Так `pump` узнаёт о завершении,
не зная деталей операции. Лямбда захватывает `done` по ссылке — это безопасно,
потому что `pump` не вернётся, пока операция не завершится (а значит, `done`
жив всё это время).

**(7) `while (!done)`** — крутим, пока операция не завершилась.

**(8) `io.run_one_for(io_quantum);`** — выполнить максимум один готовый колбэк,
ждать не дольше 20 мс. Именно здесь поток **блокируется** (в `epoll_wait`). Если
наш колбэк готов — он выполнится здесь же и выставит `done`.

**(9) `if (done) break;`** — колбэк мог только что выставить `done`. Проверяем
сразу после `run_one_for`, **до** проверок отмены/дедлайна — чтобы не «отменять»
уже завершившуюся операцию (иначе словили бы ложный `Interruption` на ровном
месте при гонке «отмена ⟷ завершение»).

**(10) `if (interrupt_requested())`** — пришёл Ctrl-C/SIGTERM (флаг выставил
обработчик сигналов в другом потоке). Реагируем.

**(11) `try { handle.cancel(); } catch (...) {}`** — отменяем ожидающую
async-операцию на хендле. Две тонкости:
- `cancel()` заставит asio вызвать наш колбэк с ошибкой `operation_aborted`.
- **Почему `try/catch(...)`?** У разных asio-хендлов разные перегрузки `cancel()`:
  у некоторых `cancel()` без аргумента может **бросить** `system_error` (например,
  если дескриптор уже закрыт). Мы уже на пути к выбросу своего исключения, и
  падение в `cancel()` не должно его маскировать или утащить нас в `std::terminate`.
  Бросаемое из `cancel()` мы намеренно глотаем. Безаргументный `cancel()` выбран
  как «наименьший общий знаменатель», работающий для сокета, акцептора, таймера и
  windows-handle одинаково.

**(12) `io.run();`** — **критически важная** строка. После `cancel()` колбэк
операции ещё не выполнен — он лишь поставлен в очередь с `operation_aborted`.
`io.run()` крутит `io_context`, пока вся работа не закончится, то есть пока этот
колбэк не отработает. Зачем дожидаться? Потому что колбэк **захватывает по ссылке**
локальные переменные (`error_code`, `transferred`, `done` — см. `Stream::transfer`).
Если бы мы бросили исключение **до** выполнения колбэка, стек `pump`/`transfer`
начал бы раскручиваться, локальные переменные — разрушаться, а отложенный колбэк
позже обратился бы к уже несуществующей памяти (use-after-free). `io.run()`
гарантирует, что колбэк отработал, пока его «среда» ещё жива.

**(13) `throw Interruption{};`** — только теперь, когда async-операция точно
завершена (отменена), бросаем отмену. Она раскрутит стек до верхнего обработчика.

**(14) Блок дедлайна** — полностью аналогичен блоку отмены, но условие
`Deadline::expired()` и исключение `TimeoutError`. Тот же приём: `cancel()` →
`io.run()` → `throw`.

> **Порядок проверок** (отмена раньше дедлайна) — если одновременно и Ctrl-C, и
> истёк таймаут, приоритет у отмены пользователя.

### 5.1. Почему это корректно и безопасно (резюме инвариантов)

- **Однопоточность на `io_context`.** Каждый `Stream`/`Acceptor` владеет своим
  `io_context`, и в момент `pump` его крутит ровно один поток. Гонок на
  `io_context` нет, поэтому `done` — обычный `bool`.
- **Время жизни колбэка ⊆ время жизни его окружения.** Мы никогда не покидаем
  `pump`, пока колбэк не выполнен (либо штатно через `run_one_for`, либо после
  `cancel()` через `io.run()`). Значит, захваты по ссылке всегда валидны.
- **Идемпотентность завершения.** Колбэк выполняется ровно один раз (одна
  async-операция → один колбэк), повторного `done=true` не бывает.

---

## 6. `Stream.hpp` — синхронный поток

Обёртка, дающая привычный `read/write/...` поверх `pump`. Базовый класс для
`Socket`.

```cpp
template <typename Handle>
class Stream {
public:
    Stream()
        : _io(std::make_unique<asio::io_context>())     // (1)
        , _handle(*_io)                                 // (2)
    {}

    explicit Stream(std::unique_ptr<asio::io_context> io, Handle handle)   // (3)
        : _io(std::move(io)), _handle(std::move(handle)) {}

    Stream(Stream&&) = default;                         // (4)
    Stream& operator=(Stream&&) = default;
    Stream(const Stream&) = delete;                     // (5)
    Stream& operator=(const Stream&) = delete;

    template <typename ...T>
    size_t write(T&&... t) {                            // (6)
        auto buffer = asio::buffer(std::forward<T>(t)...);
        return transfer([&](auto callback) {
            asio::async_write(_handle, buffer, std::move(callback));
        });
    }
    // read / writeSome / readSome — по той же схеме                 // (7)

    Handle& handle() { return _handle; }                // (8)
    const Handle& handle() const { return _handle; }
    asio::io_context& io() { return *_io; }

protected:
    template <typename Operation>
    size_t transfer(Operation operation) {              // (9)
        std::error_code error_code;
        size_t transferred = 0;
        detail::pump(*_io, _handle, [&](auto on_done) {            // (10)
            operation([&, on_done](const std::error_code& ec, size_t bytes) {  // (11)
                error_code = ec;
                transferred = bytes;
                on_done();
            });
        });
        if (error_code) {                               // (12)
            throw std::system_error(error_code);
        }
        return transferred;
    }

    std::unique_ptr<asio::io_context> _io;              // (13)
    Handle _handle;                                     // (14)
};
```

**(1)–(2) Конструктор по умолчанию.** Самое спрашиваемое место — **почему
`io_context` лежит в `unique_ptr` на куче, а не по значению?**

`asio::io_context` **запрещено копировать и перемещать** (он содержит мьютексы,
дескриптор epoll и т.п.). Но наши `Stream`/`Socket` обязаны быть **перемещаемыми**
— их `std::move`-ят в `Channel`, возвращают из `accept()`, передают в другой
поток. Если бы `io_context` лежал по значению, `Stream` стал бы неперемещаемым.

Решение: держим `io_context` **на куче** через `unique_ptr`. Тогда сам объект
`io_context` всегда находится по **стабильному адресу**, а при перемещении
`Stream` переезжает лишь указатель. Это критично, потому что asio-хендл (`_handle`)
хранит **исполнитель (executor) со ссылкой на свой `io_context`**: если бы
`io_context` физически переехал, эта ссылка стала бы висячей. А так — `io_context`
не двигается, ссылка остаётся валидной.

`_handle(*_io)` — конструируем asio-хендл, привязывая его к нашему `io_context`
(разыменовываем `unique_ptr`). Например, для `Protocol::socket` это
`asio::ip::tcp::socket(io_context&)` — создаёт **незакрытый, но неподключённый**
сокет на этом контексте.

> **Порядок инициализации** — ещё один любимый вопрос. Члены инициализируются в
> порядке их **объявления в классе**, а не в порядке списка инициализации.
> Здесь `_io` объявлен раньше `_handle` (строки 13–14), поэтому `_io`
> сконструируется первым, и `*_io` в инициализации `_handle` уже валиден. Если бы
> порядок объявления был обратный — было бы UB (разыменование ещё не созданного
> `unique_ptr`). Совпадение порядка в списке и в объявлении здесь не случайно, а
> необходимо.

**(3) Конструктор из готовых `io` и `handle`** — на случай, если хендл создаётся
снаружи (используется как точка расширения). `explicit`, чтобы не было неявных
преобразований.

**(4) Перемещение — `= default`.** Компилятор сгенерирует корректное перемещение:
`unique_ptr` переедет (передаст владение кучей), asio-хендл переедет (он
move-конструируемый). После перемещения исходный `Stream` имеет `_io == nullptr` —
им больше пользоваться нельзя, но это нормально для «опустошённого» объекта.

**(5) Копирование запрещено** — `io_context` и сокеты в принципе некопируемы;
явный `delete` делает намерение очевидным и даёт понятную ошибку компиляции.

**(6) `write(T&&... t)` — вариативный шаблон.** Зачем variadic? Чтобы прозрачно
прокинуть аргументы в `asio::buffer(...)`, у которого много перегрузок:
`asio::buffer(ptr, size)`, `asio::buffer(std::vector<...>)`, `asio::buffer(array)`
и т.д. Так наш `write` принимает всё, что принимает `asio::buffer`.
- `auto buffer = asio::buffer(std::forward<T>(t)...);` — строим буфер **один раз**.
  Важно: `asio::buffer` возвращает **лёгкий дескриптор** (`const_buffer` /
  `mutable_buffer`) — это «вид» на чужую память (указатель + размер), а не копия
  данных. Поэтому данные, на которые он смотрит, обязаны жить всё время операции —
  и они живут, потому что `transfer`/`pump` блокирующие.
- `return transfer([&](auto callback){ asio::async_write(_handle, buffer, std::move(callback)); });`
  — передаём в `transfer` лямбду-«операцию», которая запускает конкретный
  `async_write`. `buffer` захвачен по ссылке (`[&]`) — он жив на стеке `write` всё
  время блокирующего `transfer`.

  `async_write` (в отличие от `write_some`) гарантирует передачу **всех** байт
  буфера (повторяет запись, пока не отправит всё или не возникнет ошибка). Это то,
  что обычно и нужно для протокола «отправить сообщение целиком».

**(7) `read`/`writeSome`/`readSome`** — по той же схеме, но `async_read` /
`async_write_some` / `async_read_some`. `read`/`write` — «полные» (передать ровно
запрошенное), `*Some` — «сколько получится за раз» (вернуть после первой удачной
порции). Оба варианта нужны: например, гостевые каналы используют `*Some`.

**(8) Аксессоры.** `handle()` нужен, например, чтобы спросить `remote_endpoint()`
у сокета; `io()` использует `Socket::connect` и `Acceptor`. Две перегрузки
`handle()` (const/не-const) — стандартная практика, чтобы работать и с const-объектом.

**(9)–(12) `transfer(operation)` — мостик между `pump` и конкретной операцией.**
- `std::error_code error_code; size_t transferred = 0;` — сюда колбэк положит
  результат. Живут на стеке `transfer` всё время операции.
- `detail::pump(*_io, _handle, initiate_lambda)` — запускаем цикл прокрутки.
- **(10) `initiate_lambda = [&](auto on_done){ operation(real_handler); }`** —
  `pump` вызовет её, передав `on_done`. Внутри мы зовём `operation(...)` —
  переданную из `write/read` лямбду, которая и запускает `async_*`.
- **(11) Настоящий колбэк asio:**
  `[&, on_done](const std::error_code& ec, size_t bytes){ error_code = ec; transferred = bytes; on_done(); }`.
  - `[&, on_done]` — захват: `error_code`/`transferred` по ссылке (чтобы записать
    результат туда, где его прочитает `transfer`), а `on_done` — **по значению**
    (копия). Почему `on_done` копией? Этот колбэк живёт **внутри asio-операции**
    (asio хранит его до завершения), он должен пережить лямбду-`initiate`, которая
    к тому моменту уже вернулась. Захват по значению копирует замыкание `on_done`
    (внутри которого ссылка на `done` из `pump`), и оно живёт столько, сколько
    живёт операция.
  - Тело: сохраняем код ошибки и число байт, затем `on_done()` → `done = true` в
    `pump`.
- **(12)** После `pump` (операция завершена): если `error_code` ненулевой —
  бросаем `std::system_error` (превращаем «код ошибки» в исключение, удобное для
  верхнего кода). Иначе возвращаем число переданных байт.

  Заметьте: `error_code` мог быть `operation_aborted`, если сработала отмена — но
  в этом случае `pump` уже бросил `Interruption`/`TimeoutError` и до этой проверки
  мы не дойдём. Сюда `error_code` приходит только при «настоящем» завершении
  операции (успех или сетевая ошибка вроде «соединение разорвано»).

**(13)–(14) Поля.** Порядок объявления (`_io` перед `_handle`) — обязателен, см.
разбор (1)–(2).

> Тонкость для допроса: **`Stream` не потокобезопасен** — нельзя из двух потоков
> одновременно читать/писать в один `Stream` (как и любой сокет). Модель такая:
> один `Stream` обслуживает один поток в каждый момент времени. В `nn_server`
> каждое соединение живёт в своём потоке со своим `Stream` — конфликтов нет.

---

## 7. `Socket.hpp` — сокет

```cpp
#include <net/Stream.hpp>

namespace net {

template <typename Protocol>
class Socket: public Stream<typename Protocol::socket> {     // (1)
public:
    using BaseType = Stream<typename Protocol::socket>;
    using Endpoint = typename Protocol::endpoint;
    using BaseType::BaseType;                                // (2)

    void connect(const Endpoint& endpoint) {                 // (3)
        std::error_code error_code;
        detail::pump(this->io(), this->handle(), [&](auto on_done) {     // (4)
            this->handle().async_connect(endpoint, [&, on_done](const std::error_code& ec) {
                error_code = ec;
                on_done();
            });
        });
        if (error_code) {
            throw std::system_error(error_code);
        }
    }
};

using TcpSocket = Socket<asio::ip::tcp>;                      // (5)

}
```

**(1) `Socket<Protocol> : public Stream<Protocol::socket>`** — параметризуется
**протоколом** asio (`asio::ip::tcp`, `asio::local::stream_protocol`,
`hyperv::VSocketProtocol`). У каждого протокола есть вложенные типы `::socket`,
`::endpoint`, `::acceptor`. Наследуясь от `Stream<Protocol::socket>`, `Socket`
автоматически получает `read/write/...` для нужного типа сокета. Один шаблон —
все виды сокетов (TCP, unix-domain, vsock).

**(2) `using BaseType::BaseType;`** — **наследование конструкторов**. Без этой
строки `Socket` не имел бы ни конструктора по умолчанию, ни конструктора из
`(io, handle)` — пришлось бы их переписывать. Так мы переиспользуем конструкторы
`Stream`. Конструктор по умолчанию создаёт неподключённый сокет на своём
`io_context`.
[Inheriting constructors](https://en.cppreference.com/w/cpp/language/using_declaration#Inheriting_constructors).

**(3) `connect(endpoint)`** — устанавливает соединение. Почему не через
`transfer()` (как read/write)? Потому что у `async_connect` колбэк другой
сигнатуры — `void(error_code)` без числа байт, а `transfer` рассчитан на
`void(error_code, size_t)`. Поэтому `connect` зовёт `pump` напрямую.

**(4)** Структура та же, что в `transfer`: `pump(io, handle, initiate)`, внутри —
`async_connect(endpoint, real_handler)`, где `real_handler` сохраняет `ec` и зовёт
`on_done`. Захваты по той же логике: `error_code` по ссылке, `on_done` по
значению.

`this->io()` / `this->handle()` — почему `this->`? В шаблоне, наследующем от
шаблонного базового класса, имена базы не видны без `this->` (или
`BaseType::`) — это правило «зависимых имён» (two-phase lookup). Без `this->`
компилятор не нашёл бы `io()`/`handle()` из `Stream`.
[Dependent names / why `this->`](https://en.cppreference.com/w/cpp/language/dependent_name).

**(5) `using TcpSocket = Socket<asio::ip::tcp>;`** — удобный псевдоним для самого
частого случая.

---

## 8. `Acceptor.hpp` — приём соединений

```cpp
template <typename Protocol>
class Acceptor {
public:
    using Endpoint = typename Protocol::endpoint;

    Acceptor(const Endpoint& endpoint)
        : _io(std::make_unique<asio::io_context>())          // (1)
        , _handle(*_io)
    {
        _handle.open(endpoint.protocol());                   // (2)
        asio::socket_base::reuse_address option(true);       // (3)
        _handle.set_option(option);
        _handle.bind(endpoint);                              // (4)
        _handle.listen();                                    // (5)
    }

    Acceptor(Acceptor&&) = default;                          // (6)
    Acceptor& operator=(Acceptor&&) = default;

    Socket<Protocol> accept() {                              // (7)
        Socket<Protocol> peer;                               // (8)
        std::error_code error_code;
        detail::pump(*_io, _handle, [&](auto on_done) {      // (9)
            _handle.async_accept(peer.handle(), [&, on_done](const std::error_code& ec) {
                error_code = ec;
                on_done();
            });
        });
        if (error_code) {
            throw std::system_error(error_code);
        }
        return peer;                                         // (10)
    }

    typename Protocol::acceptor& handle() { return _handle; }

private:
    std::unique_ptr<asio::io_context> _io;
    typename Protocol::acceptor _handle;
};

using TcpAcceptor = Acceptor<asio::ip::tcp>;
```

**(1)** Тот же приём: `io_context` на куче (акцептор тоже должен быть
перемещаемым), хендл — `Protocol::acceptor`, привязанный к нему.

**(2) `_handle.open(endpoint.protocol())`** — открываем «слушающий» сокет нужного
семейства (IPv4/IPv6/unix/vsock). `endpoint.protocol()` достаёт протокол из
эндпойнта.

**(3) `reuse_address(true)` → `SO_REUSEADDR`** — позволяет переиспользовать
адрес/порт сразу после перезапуска процесса, не дожидаясь, пока ОС «отпустит» порт
из состояния `TIME_WAIT`. Без этого перезапуск сервера на том же порту падал бы с
«Address already in use».
[SO_REUSEADDR](https://man7.org/linux/man-pages/man7/socket.7.html).

**(4) `bind(endpoint)`** — привязываем сокет к адресу/порту.

**(5) `listen()`** — переводим сокет в режим прослушивания (ОС начинает принимать
входящие соединения в очередь backlog).

**(6) Перемещение `= default`** — по тем же причинам, что и у `Stream`.

**(7)–(10) `accept()` — самое интересное место акцептора.**

**(8) `Socket<Protocol> peer;`** — создаём **пустой** сокет под будущее соединение.
Ключевая деталь: `peer` сконструирован конструктором по умолчанию, а значит, у
него **свой собственный `io_context`** (не тот, что у акцептора!).

**(9)** Запускаем `async_accept(peer.handle(), handler)`, но крутим при этом
**`io_context` акцептора** (`pump(*_io, ...)` — это `_io` акцептора). Принятый
от ОС дескриптор соединения asio записывает в `peer`.

**Кросс-контекстный accept — самый вероятный каверзный вопрос.** «Как так:
операция исполняется на `io_context` акцептора, а сокет принадлежит другому
`io_context`?» Asio это **разрешает**: `async_accept(peer, handler)` просто
заполняет `peer` принятым дескриптором; ассоциация самой операции — с исполнителем
акцептора. После завершения `peer` — полноценный сокет, работающий уже на **своём**
`io_context`.

**Зачем `peer` нужен свой `io_context`?** Чтобы принятое соединение можно было
**отдать в отдельный поток-обработчик** и крутить там независимо. Именно так
устроен `nn_server`: цикл `accept()` живёт в одном потоке, а каждое соединение —
в своём, со своим `io_context`. Если бы `peer` делил `io_context` с акцептором,
обслуживать его в другом потоке было бы небезопасно (два потока крутили бы один
`io_context`). Эту схему мы проверили loopback-тестом — она работает.

**(10) `return peer;`** — возвращаем по значению. Сработает перемещение
(NRVO/move), `unique_ptr<io_context>` и сокет переедут в вызывающий код.

---

## 9. `Timer.hpp` — прерываемый сон

```cpp
template <typename Duration>
void sleep_for(Duration duration) {                                   // (1)
    const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(duration);
    constexpr auto quantum = std::chrono::milliseconds(20);
    while (true) {
        check_point();                                                // (2)
        const auto now = Clock::now();
        if (now >= deadline) break;
        std::this_thread::sleep_for(std::min<Clock::duration>(deadline - now, quantum));  // (3)
    }
}

class Timer {                                                         // (4)
public:
    template <typename Duration>
    void waitFor(Duration duration) { sleep_for(duration); }

    template <typename Timestamp>
    void waitUntil(Timestamp timestamp) { /* как sleep_for, но до абсолютного момента */ }
};
```

**(1) `sleep_for` — прерываемый сон.** Обычный `std::this_thread::sleep_for`
нельзя прервать (Ctrl-C/таймаут его не «разбудят»). Поэтому мы спим **маленькими
квантами** (20 мс) в цикле.

**(2) `check_point()`** между квантами — вот что делает сон прерываемым: если
запрошена отмена или истёк дедлайн, `check_point` бросит исключение прямо из сна.

**(3) `sleep_for(min(остаток, quantum))`** — спим либо оставшееся время, либо
квант (что меньше). `min` нужен, чтобы в конце не «переспать» сверх запрошенного и
чтобы короткие сны (меньше кванта) отрабатывали точно.

**(4) `class Timer`** — обёртка над `sleep_for`. Зачем класс, если есть функция?
Чтобы **не менять код**, где `coro::Timer` хранился как **поле** объекта (например,
в визиторах интерпретатора `net::Timer timer;` — поле). `waitFor`/`waitUntil`
повторяют старый интерфейс. По сути это API-совместимость с `coro::Timer`.

> Почему не asio-таймер (`steady_timer`)? Можно было бы, но тогда `sleep_for`
> потребовал бы `io_context` и весь аппарат `pump`. Простой polling-сон проще,
> не требует контекста и даёт ровно нужную семантику (учёт `check_point`). Цена —
> до 20 мс «передержки», что для пауз несущественно.

---

## 10. `SignalGuard.hpp` / `.cpp` — сигналы

Ловит сигналы ОС (SIGINT/SIGTERM) и превращает их в `net::request_interrupt()`.

### 10.1. `SignalGuard.hpp`

```cpp
class SignalGuard {
public:
    SignalGuard(std::initializer_list<int> signals, std::function<void(int)> handler);  // (1)
    ~SignalGuard();

    SignalGuard(const SignalGuard&) = delete;          // (2)
    SignalGuard& operator=(const SignalGuard&) = delete;

private:
    void arm();                                        // (3)

    asio::io_context _io;                              // (4) порядок полей важен!
    asio::signal_set _signals;
    std::function<void(int)> _handler;
    std::thread _thread;
};
```

**(1) Конструктор** принимает список сигналов и обработчик `void(int)`. Обработчик
— функция, которую вызовут с номером пойманного сигнала (обычно она зовёт
`request_interrupt()`).

**(2) Некопируемый** — управляет потоком и `io_context`, копировать бессмысленно и
опасно.

**(3) `arm()`** — поставить (и переставлять) ожидание следующего сигнала.

**(4) Порядок полей — тонкость, которую любят спрашивать.** Поля
инициализируются в порядке **объявления**. `_signals(_io)` (в `.cpp`) требует,
чтобы `_io` был сконструирован раньше — поэтому `_io` объявлен **первым**.
`_thread` объявлен **последним** и стартует в теле конструктора — к этому моменту
`_io`, `_signals`, `_handler` уже готовы (поток сразу начнёт их использовать).

### 10.2. `SignalGuard.cpp`

```cpp
SignalGuard::SignalGuard(std::initializer_list<int> signals, std::function<void(int)> handler)
    : _signals(_io)                                    // (1)
    , _handler(std::move(handler))
{
    for (int signal: signals) {
        _signals.add(signal);                          // (2)
    }
    arm();                                             // (3)
    _thread = std::thread([this] { _io.run(); });      // (4)
}

SignalGuard::~SignalGuard() {
    _io.stop();                                        // (5)
    if (_thread.joinable()) {
        _thread.join();                                // (6)
    }
}

void SignalGuard::arm() {
    _signals.async_wait([this](const std::error_code& error_code, int signal) {  // (7)
        if (error_code) {
            return;                                    // (8)
        }
        _handler(signal);                              // (9)
        arm();                                         // (10)
    });
}
```

**(1) `_signals(_io)`** — `asio::signal_set` привязывается к `io_context`. Через
него asio будет доставлять сигналы как обычные колбэки.

**(2) `_signals.add(signal)`** — регистрируем интересующие сигналы (SIGINT,
SIGTERM). Под капотом asio устанавливает обработчики сигналов ОС.

**(3) `arm()`** — ставим первое ожидание (до старта потока, чтобы к моменту
`_io.run()` уже была работа).

**(4) `_thread = std::thread([this]{ _io.run(); });`** — **отдельный фоновый
поток**, который крутит `io_context` сигналов. Зачем отдельный поток? Сигнал может
прийти, когда главный поток занят тяжёлой работой (например, инференсом или CPU-
циклом) и не крутит никакой `io_context`. Выделенный поток всегда готов обработать
сигнал немедленно.

**(5)–(6) Деструктор** — корректно гасим: `_io.stop()` заставляет `_io.run()` в
фоновом потоке вернуться (и отменяет ожидание `async_wait`, см. (8)); затем
`join()` дожидается завершения потока. Без этого поток продолжил бы крутиться, а
объект бы разрушился → обращение к уничтоженным полям.

**(7) `async_wait(handler)`** — ждём сигнал. Колбэк получает `error_code` и номер
сигнала. **Важно:** этот колбэк выполняется в **фоновом потоке** (том, что крутит
`_io`). Поэтому он не ограничен правилами async-signal-safe — asio внутри
использует self-pipe и доставляет сигнал как обычный колбэк, а не в контексте
реального обработчика сигнала ОС.
[asio signal_set](https://think-async.com/Asio/asio-1.28.0/doc/asio/reference/signal_set.html).

**(8) `if (error_code) return;`** — при остановке (`_io.stop()` в деструкторе)
ожидание отменяется с ошибкой `operation_aborted`; в этом случае просто выходим,
**не** перевзводя ожидание (иначе зациклились бы).

**(9) `_handler(signal)`** — вызываем пользовательский обработчик (обычно
`request_interrupt()`, иногда с логикой «SIGINT во время REPL не завершает
процесс»).

**(10) `arm();`** — **перевзводим** ожидание, чтобы поймать следующий сигнал.
`async_wait` одноразовый: после срабатывания его надо ставить заново. Рекурсивный
вызов `arm()` здесь не приводит к глубокой рекурсии — это лишь регистрация нового
ожидания, текущий колбэк сразу завершается.

---

## 11. `Finally.hpp` — scope guard

RAII-обёртка «выполнить действия при выходе из области видимости». Копия
`coro::Finally`. Нужна была паре мест в `main` (например, `env.reset()` на выходе).

```cpp
class Finally {
public:
    Finally() {}
    Finally(std::function<void()> fn) { _fnList.push_back(std::move(fn)); }   // (1)

    ~Finally() {
        try {                                                                // (2)
            while (!_fnList.empty()) {
                _fnList.back()();                                            // (3)
                _fnList.pop_back();
            }
        } catch (...) {}                                                     // (4)
    }

    Finally(const Finally&) = delete;                                        // (5)
    Finally& operator=(const Finally&) = delete;
    Finally(Finally&& other): _fnList(std::move(other._fnList)) {}           // (6)
    Finally& operator=(Finally&& other) { _fnList = std::move(other._fnList); return *this; }

    void operator<<(std::function<void()> fn) { _fnList.push_back(std::move(fn)); }  // (7)
    void discard() { _fnList.clear(); }                                      // (8)

private:
    std::list<std::function<void()>> _fnList;                                // (9)
};
```

**(1) Конструктор из лямбды** — самый частый случай: `net::Finally cleanup([&]{ ... });`.

**(2)–(4) Деструктор выполняет действия.** Тонкости:
- **(3)** Выполняем в порядке **обратном** добавлению (`back()` → `pop_back()`) —
  как при раскрутке стека: что добавили последним, отменяем первым (естественный
  порядок отката).
- **(4) `catch (...) {}`** — **обязательно**. Деструктор не должен выпускать
  исключение: если исключение «выйдет» из деструктора во время раскрутки стека
  (когда уже летит другое исключение), C++ вызовет `std::terminate`. Поэтому любые
  исключения из cleanup-функций глушим.
  [Почему деструкторы не должны бросать](https://en.cppreference.com/w/cpp/language/destructor#Exceptions).

**(5) Некопируемый** — иначе действия выполнились бы дважды (у оригинала и копии).

**(6) Перемещаемый** — можно передавать владение списком откатов (полезно для
паттерна «накапливаем откаты, потом `discard()` при успехе»).

**(7) `operator<<`** — добавить ещё одно действие: `rollbacks << []{ undo(); };`.
Синтаксический сахар для накопления откатов.

**(8) `discard()`** — «передумать»: очистить список, ничего не выполнять. Паттерн:
накапливаем откаты по ходу транзакции, а при успешном завершении зовём `discard()`
— тогда деструктор ничего не откатит.

**(9) `std::list`** — а не `vector`? Список даёт стабильность ссылок и дешёвые
вставки в конец; здесь это некритично (взяли как в оригинале `coro::Finally`).
Можно было бы и `vector`.

---

## 12. `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.0)

if (WIN32)
    add_definitions(-D__PRETTY_FUNCTION__=__FUNCSIG__)   # (1)
endif()

file(GLOB_RECURSE SRCS *.cpp)                            # (2)
add_library(net STATIC ${SRCS})                          # (3)

if (UNIX)
    target_link_libraries(net pthread)                   # (4)
endif()
```

**(1)** На MSVC нет `__PRETTY_FUNCTION__` (там `__FUNCSIG__`). Определяем макрос
ради единообразия с остальным проектом (общая конвенция кодовой базы).

**(2) `file(GLOB_RECURSE SRCS *.cpp)`** — собираем все `.cpp` (`Cancel.cpp`,
`Deadline.cpp`, `SignalGuard.cpp`). GLOB удобен, но имеет известный минус: при
добавлении нового файла нужно перезапустить CMake (он не отслеживает появление
файлов автоматически). Для маленькой стабильной библиотеки это приемлемо.

**(3) `add_library(net STATIC ...)`** — статическая библиотека. Большая часть
`net` — header-only шаблоны (`Stream`/`Socket`/`Acceptor`/`Timer`/`CheckPoint`/
`IoPump`/`Finally`), но `Cancel`/`Deadline`/`SignalGuard` имеют `.cpp` с
определениями (глобальный `stop_source`, thread-local стек, реализация
`SignalGuard`). Поэтому всем потребителям нужно **слинковаться с `net`**, иначе
будут неопределённые ссылки на `interrupt_source`, `Deadline::*`, `SignalGuard::*`.

**(4) `pthread` на UNIX** — `std::thread`, `thread_local`, атомарные операции и
asio требуют потоковой библиотеки. Без неё линковка/рантайм падали бы.

---

## 13. Сквозной пример: что происходит при `socket.read()`

Соберём всё вместе. Пусть код делает `channel.recv()`, который внутри зовёт
`socket.read((uint8_t*)&size, 4)` под активным `net::Deadline timeout(3s)`.

1. `Stream::read` строит `asio::buffer(ptr, 4)` и зовёт
   `transfer(initiate)`.
2. `transfer` заводит `error_code`/`transferred` и вызывает
   `pump(*_io, _handle, initiate)`.
3. `pump`:
   - `io.restart()` — готовим контекст.
   - `initiate(on_done)` → внутри `async_read(_handle, buffer, real_handler)`
     регистрирует операцию в `io_context` (ничего пока не ждём).
   - цикл `while(!done)`:
     - `io.run_one_for(20ms)` — поток блокируется в `epoll_wait` максимум на 20 мс.
     - Пришли 4 байта → asio вызывает `real_handler(ec=0, bytes=4)` → пишет
       результат, `on_done()` → `done=true`. Цикл выходит.
     - Если за 20 мс ничего не пришло → `run_one_for` вернул 0. Проверяем:
       - отмену (`interrupt_requested()`) — нет;
       - дедлайн (`Deadline::expired()`) — `now >= now0+3s`? пока нет → крутим
         дальше. Если бы 3 секунды прошли — `handle.cancel()`, `io.run()` (дать
         отработать колбэку с `operation_aborted`), `throw TimeoutError`.
4. `transfer`: `error_code` пустой → возвращает `4`.

Если в этот момент пользователь нажал Ctrl-C: `SignalGuard` в фоновом потоке
поймал SIGINT → `request_interrupt()` → `stop_source` помечен. На следующем витке
(в пределах 20 мс) `pump` увидит `interrupt_requested()` → отменит чтение → бросит
`net::Interruption`, которая раскрутит стек до `main`.

---

## 14. Каверзные вопросы «на допросе»

**Q: Это же busy-wait? Вы жжёте CPU в цикле?**
Нет. `run_one_for(20ms)` внутри — это `epoll_wait` с таймаутом, то есть поток
**спит** в ядре, пока не появятся данные или не пройдут 20 мс. Между квантами мы
лишь дёшево проверяем два флага. При простаивающем соединении это ~50 пробуждений
в секунду на поток — пренебрежимо мало.

**Q: Почему `done` — обычный `bool`, а не `std::atomic`?**
Потому что `io_context` данного сокета крутит **только текущий поток**, и колбэк,
который пишет `done`, выполняется **синхронно внутри `run_one_for` тем же
потоком**. Межпоточного доступа к `done` нет. (А вот глобальный флаг отмены —
атомарный/потокобезопасный, потому что его пишет другой поток — обработчик
сигналов.)

**Q: Зачем `io.run()` после `handle.cancel()`? Нельзя просто бросить исключение?**
Нельзя. `cancel()` лишь **планирует** колбэк операции с `operation_aborted`. Этот
колбэк захватывает по ссылке локальные переменные (`error_code`, `done`). Если
бросить исключение сразу, стек начнёт раскручиваться, переменные разрушатся, а
отложенный колбэк позже обратится к мёртвой памяти. `io.run()` гарантирует, что
колбэк отработал, пока его окружение ещё живо. Это защита от use-after-free.

**Q: Зачем `io.restart()` в начале каждого `pump`?**
Потому что путь отмены вызывает `io.run()`, после которого `io_context` остаётся в
состоянии «остановлен». Один сокет переиспользуется для многих операций, и без
`restart()` следующий `run_one_for` сразу вернул бы 0, ничего не сделав, — мы бы
зациклились.

**Q: Почему `io_context` в `unique_ptr`, а не по значению?**
`io_context` неперемещаем и некопируем, а `Stream`/`Socket` должны быть
перемещаемыми (их `move`-ят в `Channel`, возвращают из `accept`). Держим
`io_context` на куче → при перемещении `Stream` переезжает только указатель, а сам
`io_context` остаётся по стабильному адресу, и ссылка asio-хендла на него не
протухает.

**Q: Почему `Interruption` не `std::exception`, а `TimeoutError` — `std::exception`?**
Отмена должна **пробивать** все `catch(std::exception&)` и доходить до верха
(иначе Ctrl-C застрял бы в первом попавшемся `catch`). Таймаут — «штатная» ошибка,
которую код часто ловит локально (`is_avaliable` → `false`), поэтому он обычный
`std::exception`.

**Q: Кросс-контекстный `async_accept` — это легально?**
Да. `async_accept(peer, handler)` исполняется на `io_context` акцептора, но просто
**записывает принятый дескриптор** в `peer`, у которого свой `io_context`. После
завершения `peer` работает на своём контексте — это позволяет обслуживать
соединение в отдельном потоке. Проверено loopback-тестом.

**Q: Какова задержка реакции на отмену/таймаут?**
До одного кванта — 20 мс. Для интерактива и таймаутов это незаметно. Уменьшение
кванта повысит отзывчивость ценой более частых пробуждений.

**Q: `net::Stream` потокобезопасен?**
Нет, как и любой сокет: один `Stream` — один поток в каждый момент времени. Модель
конкуренции — «поток на соединение», каждый со своим `Stream`/`io_context`.

**Q: Что с `thread_local` дедлайном при переходе работы между потоками?**
Дедлайн привязан к потоку. Поскольку в нашей модели одна логическая операция
выполняется в одном потоке (мы не «перепрыгиваем» между потоками внутри запроса),
этого достаточно. Это сознательная замена «per-corutine» контекста Coro на
«per-thread».

**Q: Почему у каждого сокета свой `io_context` — это не дорого (epoll fd на сокет)?**
Это осознанный компромисс: своя «петля» на сокет даёт (а) перемещаемость объекта,
(б) возможность увести соединение в другой поток, (в) простоту (никакой общей
синхронизации). Для `testo` (мало сокетов) и `nn_server` (поток на соединение)
накладные расходы на лишние epoll-дескрипторы несущественны. Если бы понадобились
десятки тысяч соединений в одном потоке — стоило бы перейти на общий `io_context`,
но это другая модель и другая сложность.

---

## 15. Ссылки

- **Asio**: [io_context](https://think-async.com/Asio/asio-1.28.0/doc/asio/reference/io_context.html),
  [io_context::restart](https://think-async.com/Asio/asio-1.28.0/doc/asio/reference/io_context/restart.html),
  [signal_set](https://think-async.com/Asio/asio-1.28.0/doc/asio/reference/signal_set.html),
  общий [tutorial](https://think-async.com/Asio/asio-1.28.0/doc/asio/tutorial.html).
- **C++20 отмена**: [std::stop_token / std::stop_source](https://en.cppreference.com/w/cpp/thread/stop_token).
- **Хранение по потокам**: [thread_local](https://en.cppreference.com/w/cpp/language/storage_duration#Thread_local_storage).
- **Потокобезопасность статиков**: [magic statics](https://en.cppreference.com/w/cpp/language/storage_duration#Static_local_variables),
  [static init order fiasco](https://en.cppreference.com/w/cpp/language/siof).
- **Часы**: [std::chrono::steady_clock](https://en.cppreference.com/w/cpp/chrono/steady_clock).
- **Ошибки**: [std::error_code](https://en.cppreference.com/w/cpp/error/error_code),
  [std::system_error](https://en.cppreference.com/w/cpp/error/system_error).
- **Шаблоны**: [зависимые имена / зачем `this->`](https://en.cppreference.com/w/cpp/language/dependent_name),
  [наследование конструкторов](https://en.cppreference.com/w/cpp/language/using_declaration#Inheriting_constructors).
- **RAII / исключения в деструкторах**: [destructor & exceptions](https://en.cppreference.com/w/cpp/language/destructor#Exceptions).
- **Сокеты**: [SO_REUSEADDR (man socket)](https://man7.org/linux/man-pages/man7/socket.7.html).
