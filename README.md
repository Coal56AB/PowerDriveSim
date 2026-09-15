# PowerDriveSim

Специализированный настольный симулятор силовой электроники и электропривода.
Milestone 0 проверен: C++20 headless-ядро. Начат переход к Milestone 1.
Готового настольного редактора пока нет.

## Требования и состояние
- [Полное техническое задание](docs/specifications/PowerDriveSim_Technical_Specification_RU.md)
- [Инструкции разработки](docs/specifications/PowerDriveSim_Codex_Instructions_RU.md)
- [Текущая точка работы и продолжение](AGENTS.md)
- [План приёмки](docs/plan.md)
- [Архитектура и численные соглашения](docs/architecture.md)
- [Формат проекта](docs/project-format.md)
- [Готовые примеры](examples/README.md)

## Сборка
Требуются CMake >=3.20 и C++20 compiler (проверяется MSVC 19.29).
Ядро не требует GUI, GPU, сети и скачивания зависимостей.

    cmake -S . -B build
    cmake --build build --config Release
    ctest --test-dir build -C Release --output-on-failure

Для Visual Studio 2019 можно явно указать генератор:

    cmake -S . -B build -G "Visual Studio 16 2019" -A x64

Windows multi-config:

    build/Release/powerdrive-cli examples/rc.pds build/rc.csv
    build/Release/powerdrive-cli examples/rlc.pds build/rlc.csv
    build/Release/powerdrive-cli examples/switch.pds build/switch.csv
    build/Release/powerdrive-benchmark examples/rlc.pds 10

Linux/single-config: путь build/powerdrive-cli и build/powerdrive-benchmark.
Настройки CMake для Release: -DCMAKE_BUILD_TYPE=Release.

## Текущая реализация
Плоский атомарный граф R/L/C, DC voltage/current source и ideal switch;
UUID, геометрия, фиксированный шаг, записанные gate events, разреженные
MNA stamps, Reference CPU, исходные состояния C/L и CSV.
Ошибочные схемы завершаются диагностикой, без скрытого gmin.
Подробные ограничения и результаты проверки — в AGENTS.md.

## Дальнейшие этапы
Milestone 1: Qt 6 editor, иерархия, diode, Trapezoidal, Scope, 2L VSI и 3L NPC.
Затем преобразователи, машины, оптимизация и расширения.
Controller/ADC/MCU/PWM/C++ runtime начинается только в Milestone 6.