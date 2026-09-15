# PowerDriveSim — инструкции для Codex

## Источники требований
- [Полное ТЗ](docs/specifications/PowerDriveSim_Technical_Specification_RU.md)
- [Инструкции разработки](docs/specifications/PowerDriveSim_Codex_Instructions_RU.md)

## Назначение проекта
Настольный симулятор силовой электроники, преобразователей, машин и электроприводов.

## Обязательные архитектурные инварианты
Visual Schematic → Model Graph → Validation/Flattening → Topology Analysis →
Equation Generation → Simulation IR → Optimization → Backend Execution → Results.
GUI не содержит численную логику. IR не зависит от GUI и API ускорителей.
Любая поддерживаемая атомарная схема имеет generic execution path.
Все составные преобразователи должны раскрываться до редактируемых атомов.
Controller/ADC/MCU/PWM/C++ runtime только в Milestone 6; инвертор принимает готовые gates.
Стабилизаторы запрещено добавлять скрыто. Reference CPU — проверяемый эталон.

## Текущий этап
Milestone 0 выполнен по четырём критериям приёмки. Следующий активный этап — Milestone 1.
Точка работы: Reference CPU поддерживает Backward Euler и Trapezoidal. Следующий модуль — ideal diode и active-set iteration.

## Выполнено и проверено
Сохранены полные исходные документы. Инициализирован main и правильный origin.
MSVC 19.29, Release: сборка успешна, CTest 6/6 (unit/numerical/serialization/topology/regression/examples).
RC: max error 0.000183863 V < 0.0002 V; RLC: 0.00038693 V < 0.001 V.
Идеальные ключи, atomic/off-grid events, повторный запуск IR, перестановка объектов проверены.
Backward Euler: первый порядок, RC initial state, пассивная RLC energy и residual проверены.
Три CLI examples завершаются; RLC benchmark 200000 steps, 205028 steps/s (локальный baseline).
Trapezoidal: RC error 3.0657e-6 V (h=10 μs), RLC error 3.89624e-6 V (h=20 μs).
Второй порядок, сохранение энергии идеальной LC и история на gate edges проверены.
Формат v2 сохраняет integration method; миграция v1→v2 сохраняет BE и extensions.
Четвёртый пример rc-trapezoidal.pds проходит CLI и example tests.
Подробный отчёт: docs/verification-m0.md. Полный GUI Milestone 1 ещё не реализован.

## Следующие действия
1. Реализовать ideal diode и active-set iteration с контролируемой диагностикой несходимости
2. Проверить forward/reverse diode, RC charging и bridge rectification для обоих методов
3. Добавить nonlinear profile и миграционные тесты, затем desktop/editor и типизированный граф
4. Далее desktop/editor, иерархия и атомарные 2L VSI/3L NPC

## Сборка и запуск
C++20, CMake >= 3.20; headless без внешних зависимостей.
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
Windows multi-config: build/Release/powerdrive-cli examples/rc.pds build/rc.csv
Linux single-config: build/powerdrive-cli examples/rc.pds build/rc.csv

## Тестирование
ctest --test-dir build -C Release --output-on-failure
Группы: unit, numerical, serialization, topology, regression, examples.
Benchmark: build/Release/powerdrive-benchmark examples/rlc.pds 10
Для single-config убрать Release из пути.

## Структура проекта
core/model — граф; core/compiler — валидация и компиляция; core/ir — независимый IR;
core/solver/reference — разреженные stamps и эталонное исполнение;
formats/project — формат; results — экспорт; apps/cli — headless;
tests — проверки; benchmarks — измерения; examples — проверяемые схемы.

## Принятые решения
См. docs/architecture.md. C++20; эталонная sparse elimination; Qt 6 как целевой GUI.
GPU не реализуется раньше Milestone 4.

## Известные проблемы и ограничения
Desktop, иерархия и diode пока отсутствуют. Сложные идеальные DAE loops диагностируются; импульсы не поддержаны.
GitHub доступен вне песочницы. Локальная среда требовала расширенного запуска из-за setup refresh error.

## Git
Основная ветка main. Прямые осмысленные коммиты на русском; без Conventional Commits.
После законченного блока обновлять этот файл, проверять diff/tests, commit и push origin main.
Опубликован ee8b421 — «Добавлен начальный каркас PowerDriveSim». Следующий готовый шаг: Trapezoidal и миграция формата v1→v2. Проверять git log и origin/main.