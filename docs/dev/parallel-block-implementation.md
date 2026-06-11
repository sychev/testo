# Реализация блока `parallel` — разбор изменений

> Документ для middle-разработчиков. Объясняет **каждое** изменение в коде:
> что добавлено, зачем и как оно вписывается в существующую архитектуру Testo.

## 1. Что мы добавляем

Новую конструкцию языка Testo — блок `parallel`. Внутри него каждая команда
выполняется **конкурентно**, а блок завершается, только когда завершились все
команды. Это нужно, чтобы настраивать несколько виртуальных машин одновременно,
а не одну за другой.

```testo
test long_setup {
    parallel {
        configure("vm_server1")   # эти четыре вызова
        configure("vm_server2")   # выполняются
        configure("vm_client1")   # одновременно
        configure("vm_client2")
    }
}
```

Ключевая идея реализации: Testo **уже** работает на кооперативных корутинах
(библиотека Coro поверх asio), в одном системном потоке. Долгие действия
(`wait`, `sleep`, `exec`, сетевой IO) уже умеют «уступать» управление в точках
ожидания. Поэтому нам не нужны потоки ОС — достаточно запустить каждую ветку
блока в своей корутине, и пока одна ветка ждёт в `wait "DONE"`, остальные
работают.

## 2. Карта: как Testo обрабатывает скрипт

Чтобы понимать, куда что добавляется, держите в голове конвейер обработки
`.testo`-файла:

```
текст скрипта
   │
   ▼
[Лексер]      разбивает текст на токены (Token)
   │
   ▼
[Парсер]      строит AST (узлы из namespace AST)
   │
   ▼
[VisitorSemantic]   проверяет смысл, регистрирует машины, считает контрольную
   │                сумму теста (для кэширования)
   ▼
[VisitorInterpreter]  собственно выполняет тест: гоняет действия по ВМ
```

Новая фича прошивается через все четыре слоя. Разберём по порядку.

---

## 3. Лексер: новое ключевое слово `parallel`

Лексер превращает строку `parallel` в отдельный токен-ключевое-слово (а не в
обычный идентификатор). Это нужно, чтобы парсер мог однозначно отличить начало
блока от имени машины/макроса.

### 3.1. `src/testo/lexer/Token.hpp`

```cpp
        param,
        macro,
        parallel,    // ← новая категория токена
        if_,
```

**Что это?** Новый элемент в `enum class category` — перечислении всех видов
токенов.
**Зачем?** Чтобы у ключевого слова `parallel` был собственный тип токена
`Token::category::parallel`, отличный от `id` (идентификатор). Дальше парсер
будет сравнивать именно с этой категорией.

```cpp
        case category::macro:
            return "MACRO";
        case category::parallel:        // ← человекочитаемое имя категории
            return "PARALLEL";
```

**Что это?** Ветка в `type_to_string()` — функции, которая переводит категорию
токена в строку.
**Зачем?** Эта строка попадает в тексты ошибок парсера (например, «expected X
but got PARALLEL»). Без неё категория печаталась бы как «UNKNOWN TYPE».

### 3.2. `src/testo/lexer/Lexer.hpp`

```cpp
    Token macro();
    Token parallel_();   // ← объявление метода-распознавателя
    Token if_();
```

**Что это?** Объявление приватного метода `parallel_()`.
**Зачем?** По соглашению в этом лексере каждое ключевое слово имеет собственный
метод, который «съедает» его буквы и возвращает готовый токен. Подчёркивание в
имени — потому что `parallel` могло бы конфликтовать с чем-то; стиль взят из
соседних `if_()`, `for_()`.

### 3.3. `src/testo/lexer/Lexer.cpp`

```cpp
    } else if (value == "macro") {
        return macro();
    } else if (value == "parallel") {   // ← распознаём ключевое слово
        return parallel_();
    } else if (value == "if") {
```

**Что это?** Новая ветка в `Lexer::id()`. Этот метод сначала вычитывает «слово»
(последовательность букв/цифр/`_`), а затем проверяет: не является ли оно
зарезервированным ключевым словом.
**Зачем?** Если набранное слово равно `parallel`, мы возвращаем токен-ключевое-
слово, а не обычный идентификатор. Порядок проверок здесь не важен — слова
сравниваются на полное равенство.

> ⚠️ Побочный эффект: после этого `parallel` больше нельзя использовать как имя
> машины/макроса/параметра — он стал зарезервированным словом. Это нормально и
> ожидаемо (так же ведут себя `if`, `for`, `test`).

```cpp
Token Lexer::parallel_() {
    Pos tmp_pos = current_pos;              // запоминаем начало токена
    std::string value("parallel");         // текст токена известен заранее
    advance(value.length());               // сдвигаем курсор за слово
    return Token(Token::category::parallel, value, tmp_pos, previous_pos);
}
```

**Что это?** Реализация распознавателя, дословная копия `macro()`/`if_()`.
**Построчно:**
- `tmp_pos` — позиция начала ключевого слова в исходнике (нужна, чтобы в токене
  хранились координаты для сообщений об ошибках).
- `value("parallel")` — само значение токена; раз мы попали сюда, текст уже
  известен.
- `advance(value.length())` — продвигает внутренний курсор лексера на 8 символов
  вперёд (за слово `parallel`).
- `return Token(...)` — собирает токен: категория, значение, начало и конец
  (`previous_pos` — позиция последнего съеденного символа).

---

## 4. AST: узел `ParallelBlock`

### 4.1. `src/testo/parser/AST.hpp`

```cpp
//Runs every command inside the block concurrently (each command gets
//its own cooperative coroutine). Used to set up several virtual machines
//in parallel instead of one after another.
struct ParallelBlock: public Cmd {
    ParallelBlock(Token parallel_, std::shared_ptr<Block<Cmd>> block_):
        parallel(std::move(parallel_)), block(std::move(block_)) {}

    Pos begin() const override {
        return parallel.begin();
    }

    Pos end() const override {
        return block->end();
    }

    std::string to_string() const override {
        return parallel.value() + " " + block->to_string();
    }

    Token parallel;
    std::shared_ptr<Block<Cmd>> block;
};
```

**Что это?** Новый узел абстрактного синтаксического дерева.

**Почему `: public Cmd`?** В Testo тело теста — это список «команд» (`Cmd`).
Команда — это либо `RegularCmd` (`машина действие`), либо `MacroCall<Cmd>` (вызов
макроса). Делая `ParallelBlock` наследником `Cmd`, мы позволяем ему стоять там
же, где стоят обычные команды — внутри тела теста или внутри другого
`parallel`-блока. Никаких изменений в типе списка команд не потребовалось.

**Построчно по полям и методам:**
- Конструктор принимает токен `parallel` и уже разобранный блок команд
  (`Block<Cmd>`), сохраняет их (`std::move` — чтобы не копировать
  `shared_ptr`/токен зря).
- `begin()` / `end()` — границы узла в исходнике (начало — от ключевого слова,
  конец — от закрывающей `}` блока). Используются движком для подсветки места
  ошибки.
- `to_string()` — обратная сериализация узла в текст. Нужна, в частности, для
  отладки и для round-trip-тестов парсера.
- `Token parallel` — сам токен ключевого слова (хранит позицию).
- `std::shared_ptr<Block<Cmd>> block` — содержимое блока. `Block<Cmd>` — это уже
  существующий контейнер команд (тот же тип, что и тело теста). **Мы не
  изобретаем новый контейнер — переиспользуем имеющийся.**

> Важный момент про `Block<Cmd>`: он сам по себе является наследником `Cmd`
> (через `template<Item> struct Block: public Item`). Но мы заворачиваем его в
> `ParallelBlock`, а не используем напрямую, потому что нам нужно (а) ключевое
> слово как маркер «выполнять конкурентно» и (б) отдельный тип, который
> интерпретатор сможет распознать через `dynamic_pointer_cast`.

---

## 5. Парсер: разбор `parallel { ... }`

### 5.1. `src/testo/parser/Parser.hpp`

```cpp
    std::shared_ptr<AST::Controller> controller();
    std::shared_ptr<AST::Cmd> command();
    std::shared_ptr<AST::ParallelBlock> parallel_block();   // ← новый метод
```

**Что это?** Объявление метода парсинга `parallel`-блока.
**Зачем?** Парсер — это набор взаимно-рекурсивных методов, по методу на
грамматическую конструкцию. `parallel_block()` встаёт в один ряд с `command()`,
`action()` и т.д.

### 5.2. `src/testo/parser/Parser.cpp` — `test_command()`

```cpp
bool Parser::test_command(size_t index) const {
    return (LA(index) == Token::category::id ||
        LA(index) == Token::category::parallel ||   // ← добавили
        test_string(index));
}
```

**Что это?** `test_command()` отвечает на вопрос «начинается ли в позиции
`index` новая команда?». `LA(index)` (look-ahead) — категория токена впереди.
**Зачем?** Цикл разбора тела блока (`command_block()`) работает так:
`while (test_command()) { ... }`. Раньше команда могла начинаться только с
идентификатора (имя машины) или строки (имя машины в кавычках, например
`"${vmname}"`). Теперь команда может начинаться и с ключевого слова `parallel`.
**Что было бы без этой строки:** встретив `parallel`, цикл `command_block()`
решил бы, что команды кончились, и упал бы на проверке закрывающей `}`.

### 5.3. `src/testo/parser/Parser.cpp` — `command()`

```cpp
std::shared_ptr<Cmd> Parser::command() {
    if (LA(1) == Token::category::parallel) {       // ← новая ветка, первой
        return parallel_block();
    } else if (test_macro_call()) {
        return macro_call<AST::Cmd>();
    } else {
        auto entity = id();
        std::shared_ptr<Action> act = action();
        return std::make_shared<AST::RegularCmd>(entity, act);
    }
}
```

**Что это?** Диспетчер «какую именно команду разбирать».
**Зачем проверка `parallel` стоит первой?** Потому что она однозначна: токен
`parallel` имеет собственную категорию и не пересекается ни с `test_macro_call()`
(там ждут `id` + `(`), ни с веткой `id()` + `action()`. Порядок гарантирует, что
`parallel` уйдёт в правильную ветку.

### 5.4. `src/testo/parser/Parser.cpp` — сам `parallel_block()`

```cpp
std::shared_ptr<AST::ParallelBlock> Parser::parallel_block() {
    Token parallel = eat(Token::category::parallel);   // 1
    newline_list();                                    // 2
    auto block = command_block();                      // 3
    return std::make_shared<AST::ParallelBlock>(parallel, block);  // 4
}
```

**Построчно:**
1. `eat(...)` — «съедает» ожидаемый токен и возвращает его; если впереди не
   `parallel` — кидает ошибку с позицией. Здесь мы точно знаем, что он там есть
   (нас сюда позвал `command()`), но `eat` ещё и продвигает курсор.
2. `newline_list()` — пропускает переводы строк. Это позволяет писать как
   `parallel {`, так и `parallel` на одной строке и `{` на следующей.
3. `command_block()` — **переиспользуем существующий** разборщик блока команд
   `{ ... }`. Он сам съест `{`, в цикле разберёт команды (каждая — снова
   `command()`, поэтому **вложенные `parallel` работают автоматически**) и съест
   `}`.
4. Заворачиваем токен и разобранный блок в наш AST-узел.

**Главный вывод по парсеру:** мы добавили буквально одну новую конструкцию,
переиспользовав `command_block()`. Синтаксис `машина { действия }` и вызовы
макросов внутри блока заработали «бесплатно», потому что это всё те же команды.

---

## 6. Семантический визитор: регистрация машин и контрольная сумма

`VisitorSemantic` проходит по AST до выполнения и решает две важные задачи:
1. **Регистрирует** все машины, упомянутые в тесте (`mentioned_machines`) — без
   этого Testo не будет знать, какие ВМ создавать, для каких делать снапшоты и
   т.д.
2. **Считает контрольную сумму** теста (`cksum_input`) — по ней работает
   кэширование: если текст теста не менялся, тест не перезапускается.

Если бы мы не научили семантику спускаться внутрь `parallel`, машины внутри
блока остались бы «невидимыми» — это была бы серьёзная скрытая ошибка.

### 6.1. `src/testo/visitors/VisitorSemantic.hpp`

```cpp
    void visit_command(std::shared_ptr<AST::Cmd> cmd);
    void visit_parallel_block(std::shared_ptr<AST::ParallelBlock> parallel);  // ←
    void visit_regular_command(const IR::RegularCommand& regular_cmd);
```

**Что это?** Объявление нового метода обхода.

### 6.2. `src/testo/visitors/VisitorSemantic.cpp` — диспетчер

```cpp
void VisitorSemantic::visit_command(std::shared_ptr<AST::Cmd> cmd) {
    if (auto p = std::dynamic_pointer_cast<AST::ParallelBlock>(cmd)) {   // ←
        visit_parallel_block(p);
    } else if (auto p = std::dynamic_pointer_cast<AST::RegularCmd>(cmd)) {
        visit_regular_command({p, stack});
    } else if (auto p = std::dynamic_pointer_cast<AST::MacroCall<AST::Cmd>>(cmd)) {
        visit_cmd_macro_call({p, stack});
    } else {
        throw Exception("Should never happen");
    }
}
```

**Что это?** В диспетчер типов команды добавлена ветка для `ParallelBlock`.
**Зачем `dynamic_pointer_cast`?** `cmd` — это `shared_ptr<Cmd>` (базовый тип).
`dynamic_pointer_cast` пытается привести его к конкретному наследнику; если тип
не тот — возвращает `nullptr`, и `if` идёт дальше. Это стандартный для этого
кода способ «разобрать» тип узла.
**Почему ветка `ParallelBlock` стоит первой?** Принципиально неважно (типы
взаимоисключающие), но логично проверять более «структурные» узлы раньше.

### 6.3. `src/testo/visitors/VisitorSemantic.cpp` — сам обход

```cpp
void VisitorSemantic::visit_parallel_block(std::shared_ptr<AST::ParallelBlock> parallel) {
    current_test->cksum_input << "parallel {" << std::endl;   // 1
    visit_command_block(parallel->block);                     // 2
    current_test->cksum_input << "}" << std::endl;            // 3
}
```

**Построчно:**
1. Дописываем маркер `parallel {` в поток `cksum_input`. Контрольная сумма
   теста считается как хэш от этого текстового потока. Маркер нужен, чтобы тест
   с `parallel` и тест без него (но с тем же набором действий) имели **разные**
   суммы — иначе кэш мог бы ошибочно посчитать их одинаковыми.
2. `visit_command_block(parallel->block)` — рекурсивно обходим все команды
   внутри блока тем же методом, что и обычное тело теста. Именно здесь для каждой
   `машина действие` вызовется `visit_regular_command → visit_machine`, и машина
   попадёт в `mentioned_machines`. Действия тоже провалидируются.
3. Закрывающий маркер `}` — симметрично пункту 1.

> Машины регистрируются как побочный эффект обхода: `visit_machine()` делает
> `current_test->mentioned_machines.insert(machine)`. Поэтому достаточно просто
> **дойти** до каждой команды — отдельной логики «зарегистрировать машины из
> parallel» не нужно.

---

## 7. Интерпретатор: собственно конкурентное выполнение

Это самая содержательная часть. Здесь два слоя:
- `ParallelBranchInterpreter` — выполняет **одну** ветку блока в изоляции;
- `run_parallel_block` — запускает все ветки конкурентно и ждёт их.

### 7.1. Зачем вообще отдельный «интерпретатор ветки»?

Главный `VisitorInterpreter` хранит **изменяемое** состояние выполнения прямо в
своих полях:
- `stack` — текущий стек переменных (параметры макросов, `${...}`);
- `current_controller` — текущая машина (используется в обработке ошибок);
- `current_test`, `reporter`, `ignore_repl`.

Поле `stack` особенно опасно: вызов макроса временно подменяет `stack`
(механизм `StackPusher`), а потом восстанавливает. Если две ветки будут
конкурентно дёргать **один и тот же** `interpreter->stack`, они затрут контекст
друг друга в точках, где корутина уступает управление (между подменой стека и
его чтением). Возникнут трудноуловимые баги: ветка `vm_server1` внезапно увидит
`${vmname}` от ветки `vm_server2`.

Решение: **каждая ветка получает свою копию изменяемого контекста.** `reporter`
и `current_test` остаются общими (репортер — это просто вывод, тест — только
для чтения).

### 7.2. `src/testo/visitors/VisitorInterpreter.cpp` — подключение seam

```cpp
#include <coro/CheckPoint.h>
#include <coro/AsioTask.h>
#include "ParallelExecutor.hpp"   // ← вместо прямого <coro/CoroPool.h>
```

**Что это?** Подключаем адаптер `ParallelExecutor` (см. раздел 8) вместо
прямого использования `CoroPool`.
**Зачем?** Чтобы зависимость от библиотеки Coro жила в одном-единственном месте
(см. раздел 8 про будущую миграцию на asio).

### 7.3. `ParallelBranchInterpreter` — выполнение одной ветки

Структура объявлена в **анонимном namespace** (`namespace { ... }`) — то есть
видна только внутри этого `.cpp`. Это деталь реализации, наружу её выносить не
нужно.

```cpp
namespace {

// ... комментарий о кооперативной модели ...
struct ParallelBranchInterpreter {
    ParallelBranchInterpreter(Reporter& reporter,
                              std::shared_ptr<IR::Test> current_test,
                              bool ignore_repl,
                              std::shared_ptr<StackNode> stack):
        stack(std::move(stack)),
        reporter(reporter),
        current_test(std::move(current_test)),
        ignore_repl(ignore_repl) {}
```

**Конструктор.** Принимает то, что нужно ветке для работы:
- `reporter` — **по ссылке** (`Reporter&`): общий объект вывода на все ветки.
- `current_test` — копия `shared_ptr` (дёшево, и тест неизменяем во время
  выполнения).
- `ignore_repl` — простой флаг-настройка (по значению).
- `stack` — **копия** указателя на текущий узел стека. Важно: это копия
  *указателя*, сами `StackNode` общие и только читаются; ветка будет менять
  лишь свой **слот** `stack` (поле объекта), а не чужой.

```cpp
    void visit_command(const std::shared_ptr<AST::Cmd>& cmd) {
        if (auto p = std::dynamic_pointer_cast<AST::ParallelBlock>(cmd)) {
            visit_parallel_block(p);
        } else if (auto p = std::dynamic_pointer_cast<AST::RegularCmd>(cmd)) {
            visit_regular_command({p, stack});
        } else if (auto p = std::dynamic_pointer_cast<AST::MacroCall<AST::Cmd>>(cmd)) {
            visit_macro_call({p, stack});
        } else {
            throw std::runtime_error("Should never happen");
        }
    }
```

**Что это?** Диспетчер типов команды — копия логики из главного
`VisitorInterpreter::visit_command`, но работающий с **локальным** `stack`
ветки.
**Почему дублируется?** Главный интерпретатор использует `this->stack` (общий),
а нам нужен изолированный. Дублирование ~10 строк — осознанный размен на
простоту и безопасность: альтернатива (вынести общий «исполнитель команд» с
параметризуемым стеком) — более крупный рефакторинг, рискованный для MVP.
**Ветка `ParallelBlock`** обеспечивает поддержку **вложенных** `parallel`.

```cpp
    void visit_command_block(const std::shared_ptr<AST::Block<AST::Cmd>>& block) {
        for (auto command: block->items) {
            visit_command(command);
        }
    }
```

**Что это?** Последовательный обход команд внутри одной ветки. Важно понимать:
**внутри** одной ветки команды идут по очереди (как в обычном тесте), конкурентны
между собой только сами ветки.

```cpp
    void visit_regular_command(const IR::RegularCommand& regular_command) {
        if (auto controller = IR::program->get_machine_or_null(regular_command.entity())) {
            current_controller = controller;
            VisitorInterpreterActionMachine(controller, stack, reporter, current_test, ignore_repl)
                .visit_action(regular_command.ast_node->action);
            current_controller = nullptr;
        } else if (auto controller = IR::program->get_flash_drive_or_null(regular_command.entity())) {
            current_controller = controller;
            VisitorInterpreterActionFlashDrive(controller, stack, reporter, current_test, ignore_repl)
                .visit_action(regular_command.ast_node->action);
            current_controller = nullptr;
        } else {
            throw std::runtime_error("Should never happen");
        }
    }
```

**Что это?** Выполнение одной команды `машина действие`.
**Построчно:**
- `get_machine_or_null(entity())` — по имени сущности (например `vm_server1`)
  ищем контроллер машины. Если это не машина — пробуем флешку.
- `current_controller = controller` — фиксируем «текущую машину» ветки (нужно
  для диагностики ошибок; у каждой ветки своё поле, конфликтов нет).
- Создаём **локальный** объект-визитор действия
  (`VisitorInterpreterActionMachine` или `...FlashDrive`) и просим его выполнить
  действие. Заметьте: ему передаётся **наш** `stack`. Этот объект — где
  происходит вся «магия» (нажатия клавиш, `wait`, скриншоты), и именно его
  методы уступают управление в точках ожидания.
- `current_controller = nullptr` — снимаем фиксацию после выполнения.

```cpp
    void visit_macro_call(const IR::MacroCall& macro_call) {
        reporter.macro_command_call(macro_call);
        macro_call.visit_interpreter<AST::Cmd>(this);
    }
```

**Что это?** Выполнение вызова макроса (например `configure("vm_server1")`).
**Построчно:**
- `reporter.macro_command_call(...)` — сообщаем в отчёт, что начался вызов
  макроса.
- `macro_call.visit_interpreter<AST::Cmd>(this)` — ключевая строка. Этот
  шаблонный метод (из `IR/Macro.hpp`):
  1. через `StackPusher` подменяет `this->stack` на новый кадр с аргументами
     макроса (`vmname = "vm_server1"`),
  2. вызывает `this->visit_macro_body(...)`,
  3. по выходу восстанавливает `stack`.
  Поскольку `this` — наш `ParallelBranchInterpreter` с **собственным** `stack`,
  подмена затрагивает только эту ветку. Именно поэтому у структуры поле `stack`
  и метод `visit_macro_body` сделаны публичными — шаблон обращается к ним по
  имени (`visitor->stack`, `visitor->visit_macro_body`).

```cpp
    void visit_macro_body(const std::shared_ptr<AST::Block<AST::Cmd>>& macro_body) {
        visit_command_block(macro_body);
    }

    void visit_parallel_block(const std::shared_ptr<AST::ParallelBlock>& parallel);
```

- `visit_macro_body` — тело макроса это просто блок команд; гоняем его через
  наш же `visit_command_block`.
- `visit_parallel_block` — только объявление (тело ниже), потому что оно зовёт
  `run_parallel_block`, который объявлен после структуры.

```cpp
    std::shared_ptr<StackNode> stack;
    std::shared_ptr<IR::Controller> current_controller;
    Reporter& reporter;
    std::shared_ptr<IR::Test> current_test;
    bool ignore_repl = false;
};
```

**Поля.** Это и есть «изолированный контекст ветки». `stack` и
`current_controller` — приватные для ветки (вот ради чего всё затевалось);
`reporter` (ссылка) и `current_test` — общие.

### 7.4. `run_parallel_block` — запуск и ожидание веток

```cpp
void run_parallel_block(
    const std::shared_ptr<AST::ParallelBlock>& parallel,
    Reporter& reporter,
    std::shared_ptr<IR::Test> current_test,
    bool ignore_repl,
    std::shared_ptr<StackNode> stack)
{
    ParallelExecutor executor;                                    // 1
    for (auto command: parallel->block->items) {                  // 2
        executor.spawn([command, &reporter, current_test, ignore_repl, stack] {  // 3
            ParallelBranchInterpreter(reporter, current_test, ignore_repl, stack)
                .visit_command(command);                          // 4
        });
    }
    executor.join();                                              // 5
}
```

**Построчно:**
1. `ParallelExecutor executor` — наш адаптер над планировщиком корутин
   (раздел 8). Внутри держит `coro::CoroPool`.
2. Перебираем команды внутри блока — каждая станет отдельной веткой.
3. `executor.spawn([...]{ ... })` — планируем ветку на конкурентное выполнение.
   **Разбор захватов лямбды:**
   - `command` — по значению (копия `shared_ptr`). Критично: переменная цикла
     меняется на каждой итерации, поэтому захватывать её надо **по значению**, а
     не по ссылке, иначе все ветки увидели бы последнюю команду.
   - `&reporter` — по ссылке (общий объект, живёт дольше блока).
   - `current_test`, `ignore_repl`, `stack` — по значению (дёшево и безопасно).
4. Внутри лямбды создаём для ветки свой `ParallelBranchInterpreter` и запускаем
   на нём команду. Каждая ветка — отдельный объект, отдельный `stack`.
5. `executor.join()` — блокируемся (кооперативно!), пока не завершатся **все**
   ветки. Если какая-то ветка кинула исключение — оно «всплывёт» здесь, а
   остальные ветки будут отменены (fail-fast). Подробности — в разделе 8.

> Почему `run_parallel_block` — свободная функция, а не метод? Чтобы один и тот
> же код переиспользовали оба вызывающих: и главный `VisitorInterpreter`, и
> вложенный `ParallelBranchInterpreter` (для вложенных блоков). Им просто
> передаются их собственные `reporter/current_test/stack`.

```cpp
void ParallelBranchInterpreter::visit_parallel_block(const std::shared_ptr<AST::ParallelBlock>& parallel) {
    run_parallel_block(parallel, reporter, current_test, ignore_repl, stack);
}

} // namespace
```

**Что это?** Отложенное определение метода ветки: вложенный `parallel` внутри
ветки просто снова зовёт `run_parallel_block`, передавая контекст этой ветки.
Закрываем анонимный namespace.

### 7.5. Интеграция в главный `VisitorInterpreter`

```cpp
void VisitorInterpreter::visit_command(const std::shared_ptr<AST::Cmd>& cmd) {
    if (auto p = std::dynamic_pointer_cast<AST::ParallelBlock>(cmd)) {   // ← новая ветка
        visit_parallel_block(p);
    } else if (auto p = std::dynamic_pointer_cast<AST::RegularCmd>(cmd)) {
        visit_regular_command({p, stack});
    } else if (auto p = std::dynamic_pointer_cast<AST::MacroCall<AST::Cmd>>(cmd)) {
        visit_macro_call({p, stack});
    } else {
        throw std::runtime_error("Should never happen");
    }
}

void VisitorInterpreter::visit_parallel_block(const std::shared_ptr<AST::ParallelBlock>& parallel) {
    run_parallel_block(parallel, reporter, current_test, ignore_repl, stack);
}
```

**Что это?** Точка входа на верхнем уровне: когда `parallel` встречается прямо в
теле теста, главный интерпретатор вызывает тот же `run_parallel_block`, передавая
**свои** поля. Обратите внимание: главный `this->stack` при этом не подменяется —
каждая ветка работает на своей копии, так что контекст главного интерпретатора в
безопасности.

### 7.6. `src/testo/visitors/VisitorInterpreter.hpp`

```cpp
    void visit_command(const std::shared_ptr<AST::Cmd>& cmd);
    void visit_parallel_block(const std::shared_ptr<AST::ParallelBlock>& parallel);  // ←
    void visit_macro_call(const IR::MacroCall& macro_call);
```

**Что это?** Объявление нового метода в интерфейсе главного интерпретатора.

---

## 8. `ParallelExecutor` — «шов» (seam), изолирующий Coro

### 8.1. `src/testo/visitors/ParallelExecutor.hpp`

```cpp
#pragma once

#include <functional>
#include <coro/CoroPool.h>

// ==== большой комментарий-инструкция ====

struct ParallelExecutor {
    void spawn(std::function<void()> branch) {
        pool.exec(std::move(branch));
    }

    void join() {
        pool.waitAll();
    }

private:
    coro::CoroPool pool;
};
```

**Что это и зачем?** Это единственное место во всей фиче, которое знает про
библиотеку Coro. Он предоставляет крошечный примитив «запустить N веток и
дождаться всех»:
- `spawn(branch)` — `pool.exec(...)` запускает `branch` как новую дочернюю
  корутину в текущем strand'е (один системный поток, кооперативно).
- `join()` — `pool.waitAll()` уступает управление до тех пор, пока все дочерние
  корутины не завершатся. Если ветка кинула исключение, `CoroPool` пробрасывает
  его в родителя, а оставшиеся ветки отменяются при разрушении `pool`
  (деструктор `~CoroPool` вызывает `cancelAll()`). Это и есть семантика
  fail-fast.

**Почему это вынесено в отдельный файл?** Из-за планов отказаться от Coro
(сохранив asio). Вся остальная машинерия фичи (грамматика, семантика,
`ParallelBranchInterpreter`) **не знает**, как именно планируются ветки — она
общается только с этим интерфейсом. Когда придёт время мигрировать, нужно будет
переписать **только этот struct** поверх `asio::spawn` +
`asio::experimental::make_parallel_group`:
- `spawn` будет накапливать по одной отложенной (`deferred`) операции на ветку,
- `join` — запускать `make_parallel_group(branches).async_wait(wait_for_one_error(), yield)`.

Семантика (конкуренция, fail-fast, отмена) сохранится, а интерфейс
`spawn`/`join` останется прежним. Инструкция дословно записана в комментарии
внутри файла, чтобы будущему мигрирующему не пришлось реконструировать замысел.

---

## 9. Почему это работает без потоков ОС (и без гонок)

Это ключевая концепция, на которой держится вся фича. Зафиксируем её отдельно:

1. **Один поток, кооперативные корутины.** Весь интерпретатор крутится в
   `coro::Application` — это один системный поток с event loop. Переключение
   между корутинами происходит **только** в явных точках уступки управления
   (`coro::CheckPoint`, ожидание таймера/сокета). Нет вытеснения → нет гонок
   данных «из ниоткуда»: между двумя точками уступки код атомарен.

2. **Долгие действия уже уступают управление.** Например, `wait "DONE"`
   реализован как цикл, который между снимками экрана делает `timer.waitFor(...)`
   (асинхронный таймер). Пока ветка `vm_server1` «спит» секунду в ожидании, event
   loop отдаёт управление веткам `vm_server2/3/4`.

3. **Настоящая тяжёлая работа идёт внутри гостей.** `configure.sh` выполняется
   на самой ВМ асинхронно; Testo лишь параллельно **ждёт** результат. Поэтому
   четыре `wait "DONE"` действительно «накладываются» во времени.

4. **Изоляция контекста делает конкуренцию безопасной.** Поскольку каждая ветка
   имеет собственные `stack` и `current_controller`, переключение между ветками
   в точке уступки не портит чужой контекст.

**Следствие — ограничение:** две ветки одного `parallel` не должны работать с
**одной и той же** ВМ. Иначе их нажатия клавиш и обращения к гостевому каналу
перемешаются. Сейчас это на ответственности автора скрипта (зафиксировано в
туториале). Статическую проверку «уникальная ВМ на ветку» можно добавить позже —
это нетривиально из-за раскрытия макросов.

---

## 10. Тесты

### `src/testo/unit_tests/TestParser.cpp`

Добавлены два теста парсера (фреймворк Catch).

```cpp
TEST_CASE("parse parallel block") {
    auto block = Parser(".", R"({
        parallel {
            vm_server1 {
                start
                type "configure.sh"; press Enter
                wait "DONE"
            }
            vm_server2 { start; wait "DONE"; }
            configure("vm_client1")
        }
        vm_server1 { stop; }
    })").command_block();

    REQUIRE(block->items.size() == 2);

    auto parallel = std::dynamic_pointer_cast<AST::ParallelBlock>(block->items.at(0));
    REQUIRE(parallel != nullptr);
    REQUIRE(parallel->block->items.size() == 3);

    //a regular command after the parallel block is still parsed
    REQUIRE(std::dynamic_pointer_cast<AST::RegularCmd>(block->items.at(1)) != nullptr);
}
```

**Что проверяет:**
- Блок из двух команд: сам `parallel` и идущая после него обычная команда
  `vm_server1 { stop; }` → `block->items.size() == 2`. Это заодно доказывает,
  что после `parallel`-блока разбор корректно продолжается.
- Первый элемент действительно `ParallelBlock` (каст не `nullptr`).
- Внутри блока ровно три ветки (две `машина { ... }` и один вызов макроса).

```cpp
TEST_CASE("parse nested parallel block") {
    auto block = Parser(".", R"({
        parallel {
            vm_a { start; }
            parallel {
                vm_b { start; }
                vm_c { start; }
            }
        }
    })").command_block();

    auto parallel = std::dynamic_pointer_cast<AST::ParallelBlock>(block->items.at(0));
    REQUIRE(parallel != nullptr);
    REQUIRE(std::dynamic_pointer_cast<AST::ParallelBlock>(parallel->block->items.at(1)) != nullptr);
}
```

**Что проверяет:** вложенный `parallel` разбирается (второй элемент внешнего
блока — снова `ParallelBlock`). Это подтверждает, что переиспользование
`command_block()` дало вложенность «бесплатно».

> Используется `R"(...)"` (raw string literal), чтобы писать `.testo`-код с
> кавычками без экранирования. `command_block()` вызывается напрямую, потому что
> это публичный метод парсера и для теста удобнее, чем оборачивать всё в полный
> `test { ... }`.

---

## 11. Документация для пользователей

- `docs/tutorials/17 - parallel/README.md` — туториал: мотивация, синтаксис,
  пример с макросом, объяснение модели выполнения и **список подводных камней**
  (одна машина на ветку, перемешанный вывод, fail-fast, вложенность).
- `docs/tutorials/17 - parallel/parallel.testo` — минимальный рабочий пример.

---

## 12. Итоговая карта «файл → роль»

| Файл | Что добавили | Роль в фиче |
|------|--------------|-------------|
| `lexer/Token.hpp` | категория `parallel` | новый вид токена |
| `lexer/Lexer.hpp/.cpp` | `parallel_()` | распознавание ключевого слова |
| `parser/AST.hpp` | `ParallelBlock` | узел дерева |
| `parser/Parser.hpp/.cpp` | `parallel_block()`, правки `command()`/`test_command()` | разбор синтаксиса |
| `visitors/VisitorSemantic.hpp/.cpp` | `visit_parallel_block` | регистрация машин + контрольная сумма |
| `visitors/VisitorInterpreter.hpp/.cpp` | `ParallelBranchInterpreter`, `run_parallel_block`, `visit_parallel_block` | конкурентное выполнение |
| `visitors/ParallelExecutor.hpp` | `ParallelExecutor` | «шов», изолирующий Coro |
| `unit_tests/TestParser.cpp` | 2 теста | проверка парсинга, в т.ч. вложенности |
| `docs/tutorials/17 - parallel/*` | туториал + пример | пользовательская документация |

**Главные принципы, которые стоит унести из этого разбора:**
1. Новая конструкция языка проходит через все слои: лексер → AST → парсер →
   семантика → интерпретатор. Пропустишь слой (например, семантику) — получишь
   скрытый баг (незарегистрированные машины).
2. Переиспользование существующих кирпичиков (`command_block`,
   `машина { действия }`, макросы) минимизирует объём и риск изменений.
3. Конкурентность здесь — кооперативная (без потоков ОС), а безопасность
   обеспечивается изоляцией изменяемого контекста на каждую ветку.
4. Зависимость от конкретной библиотеки корутин спрятана за один маленький
   адаптер — это удешевит будущую миграцию на asio.
