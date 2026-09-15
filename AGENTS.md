# PowerDriveSim — инструкции для Codex

## Источники требований

- [Полное ТЗ](docs/specifications/PowerDriveSim_Technical_Specification_RU.md)
- [Инструкции разработки и уточнение README](docs/specifications/PowerDriveSim_Codex_Instructions_RU.md)
- [План и карта критериев](docs/plan.md)

## Назначение проекта

Настольный симулятор силовой электроники, преобразователей, электрических машин и электроприводов.

## Обязательные архитектурные инварианты

Visual Schematic → Model Graph → Validation/Flattening → Topology Analysis →
Equation Generation → Simulation IR → Optimization → Backend Execution → Results.

- GUI не содержит solver logic; компоненты не зависят от widgets
- IR не зависит от CPU/GPU API; Reference CPU остаётся проверяемым эталоном
- Любая поддерживаемая атомарная схема имеет generic execution path
- Составные преобразователи раскрываются до редактируемых атомов
- Controller/ADC/MCU/PWM/C++ runtime только в Milestone 6; инвертор принимает готовые gates
- Запрещены скрытые стабилизаторы, изменение физики и подмена незавершённых функций заглушками

## Текущий этап

Milestone 0 выполнен по всем четырём критериям. Активен Milestone 1, он не завершён.
Реализован первый блок M1: Trapezoidal, ideal diode и bounded active-set solve.
Точка остановки: core/solver/reference/reference.cpp проверен для RLC/V/I/S/D.
core/model/model.hpp пока описывает плоский electrical graph со ссылками на node UUID.
Первый невыполненный критерий M1 — создание, соединение и запуск схемы в GUI.
Desktop target, typed signal/gate ports, hierarchy, Inspector и Scope ещё отсутствуют.

## Выполнено и проверено

- C++20 Reference CPU: sparse MNA stamps, R/L/C/V/I/S/D, исходные C/L состояния
- Backward Euler и Trapezoidal; два порядка сходимости проверены по аналитике
- Точные recorded gate edges, одновременные события, повторяемость и порядок UUID
- Ideal diode: forward/reverse, current-driven, RC charge/hold, bridge обеих полярностей, RL freewheel
- Формат v3; последовательные миграции v1→v2→v3; UUID, геометрия, extensions, solver profile
- CSV с units, UUID, method/backend/precision и nonlinear settings; CLI и benchmark
- Последняя Release-сборка MSVC 19.29 успешна без предупреждений
- Последний CTest: 7/7 групп, 1.28 s (unit/numerical/serialization/topology/regression/examples/diode)
- RC BE error 0.000183863 V; RLC BE 0.00038693 V
- RC Trap error 3.0657e-6 V; RLC Trap 3.89624e-6 V; LC energy и gate history проверены
- Пять bundled examples проходят assertions; freewheel CLI: 1500 steps, residual 1.7764e-16
- CI всех четырёх функциональных/обзорных коммитов, включая e659b31, успешен на Windows и Linux
- Отчёты: docs/verification-m0.md и docs/verification-m1-core.md

## Следующие действия

1. Добавить типизированные electrical/gate/signal ports, connections и probes в document model; обеспечить совместимость v3 и тесты connectivity
2. Подготовить Qt 6 toolchain и desktop target, который использует то же ядро; создать add/connect/edit/run без правок C++
3. Реализовать command history и save/load/autosave для editor document
4. Добавить hierarchy/public ports/open internals/edit definition/detach/flatten с тестами UUID и независимого экземпляра
5. Создать атомарные 2L VSI и 3L NPC с отдельными diodes/snubber/DC-link/gates; проверить изменённые топологии
6. Scope u/i/gates, zoom/cursors/CSV; worker Run/Stop, UI tests и остальные критерии M1
7. Не переходить к M2 до полной приёмки M1; не начинать Controller раньше M6

## Сборка и запуск

CMake >=3.20 и C++20 compiler. Проверен MSVC 19.29 (Visual Studio 2019).
Core собирается без внешних зависимостей, GUI, GPU или сети.
Для будущего GUI выбран Qt 6; локально обнаруженный Qt 5.15 не является GUI-зависимостью проекта.

    cmake -S . -B build
    cmake --build build --config Release
    ctest --test-dir build -C Release --output-on-failure

Windows multi-config:

    build/Release/powerdrive-cli examples/rc.pds build/rc.csv
    build/Release/powerdrive-cli examples/rlc.pds build/rlc.csv
    build/Release/powerdrive-cli examples/switch.pds build/switch.csv
    build/Release/powerdrive-cli examples/rc-trapezoidal.pds build/rc-trapezoidal.csv
    build/Release/powerdrive-cli examples/diode-freewheel.pds build/diode-freewheel.csv

Linux single-config: убрать Release из пути; для Release задать -DCMAKE_BUILD_TYPE=Release.

## Тестирование

    ctest --test-dir build -C Release --output-on-failure
    ctest --test-dir build -C Release -R "numerical|diode" --output-on-failure
    build/Release/powerdrive-benchmark examples/rlc.pds 10
    build/Release/powerdrive-benchmark examples/diode-freewheel.pds 10

Нельзя запускать старые test binaries после проваленной сборки и считать это проверкой изменений.
Workflow .github/workflows/ci.yml выполняет все CTest-группы на Windows/Linux.

## Структура проекта

- core/model, core/compiler, core/ir — граф, проверка и формирование уравнений
- core/solver/reference — sparse stamps, численный решатель и events
- formats/project, results — формат, миграции и CSV
- apps/cli — headless-приложение; apps/desktop ещё не создан
- tests, examples, benchmarks — автоматическая верификация и измерения
- docs — архитектура, схемы формата, отчёты и исходные требования

## Принятые решения

См. docs/architecture.md: std::map sparse reference, будущий Eigen SparseLU,
целевой Qt 6, независимый от API GPU contract и кандидат Vulkan Compute.
Это архитектурный выбор; Eigen/Qt/GPU implementation пока не поставляются.
Нет fast paths: все реализованные схемы рассчитываются generic solver.
Format v1/v2 fixtures сохранять для регрессионной проверки миграций.

## Известные проблемы и ограничения

- Нет GUI, hierarchy, typed ports, probes UI, Scope, 2L/NPC, машин и ускоренных backend
- Идеальные DAE loops с неуникальными токами/импульсами диагностируются; index reduction отсутствует
- Поиск diode states ограничен iteration budget; общая сходимость сложных идеальных сетей не гарантирована
- Естественные diode zero crossings разрешаются на концах шагов; scheduled gate edges — точно
- Результаты в RAM; Stop проверяется между шагами и не прерывает отдельную факторизацию
- Только DC источники и записанные switch events; waveform/signal blocks ещё не добавлены
- Диагностика singular/ideal sources ещё требует отдельного подробного topology analysis для M1
- Benchmark показывает оценку result payload, не peak RAM; согласованных performance gates ещё нет

## Стиль README

README обзорный: назначение, особенности, реальные иллюстрации, готовые возможности
и короткая установка. Не добавлять раздел «Технологии» и не приписывать работу пользователю.
График docs/images/reference-examples.png построен из CLI CSV и визуально проверен.
Воспроизведение: создать rc-trapezoidal/rlc/switch CSV в build, затем
python docs/render_examples.py (только для иллюстрации нужны matplotlib и numpy).

## Git

Основная ветка main, origin — https://github.com/Coal56AB/PowerDriveSim.git.
Осмысленные коммиты на русском, без Conventional Commits; прямой push main.
Опубликованы в порядке создания:
- ee8b421 — Добавлен начальный каркас PowerDriveSim
- e8b1fee — Добавлен метод интегрирования Trapezoidal
- 872a3db — Переработан обзор проекта в README
- e659b31 — Реализованы идеальные диоды и нелинейный расчёт схем

Последний опубликованный функциональный шаг — e659b31: идеальные диоды и schema v3.
CI: https://github.com/Coal56AB/PowerDriveSim/actions/runs/34981148062 — обе платформы success.
Этот документальный шаг фиксирует приёмку CI и handoff; исходники после e659b31 не менялись.
Перед завершением следующего сеанса обновить этот файл, собрать, проверить и push.
