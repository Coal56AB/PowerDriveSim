# Проверка Milestone 0 — 2026-09-15

## Среда и команды
Intel Core i5-6400 2.70 GHz (4 cores), Windows 10 x64, MSVC 19.29.30148, CMake 4.4.0, Release, /fp:strict.
cmake -S . -B build -G "Visual Studio 16 2019" -A x64
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure

Сборка без предупреждений. 6/6 групп CTest прошли (0.86 s).
Это локальная проверка Windows; Windows workflow добавлен,
но удалённый запуск CI на момент записи ещё не подтверждён.

## Критерии приёмки
1. RC/RLC с аналитикой — выполнено
   - RC R=1000 Ω, C=1 μF, h=1 μs: max error 0.000183863 V, допуск 0.0002 V
   - RLC R=2 Ω, L=10 mH, C=1 mF, h=2 μs: max error 0.00038693 V, допуск 0.001 V
   - Проверены все отсчёты, первый порядок BE и убывание энергии пассивной RLC
2. Ideal switch изменяет топологию — выполнено
   - Два источника 5/10 V, нагрузка 10 Ω, gates на 4.3/8.1 ms при сетке 1 ms
   - Строго 5→10→0 V, ошибка <1e-12, фронты присутствуют точно
   - Открытие/закрытие одновременной пары не создаёт промежуточный идеальный loop
3. Один IR исполняется повторяемо — выполнено
   - Совпадают все времена, double vectors и gate states двух запусков
   - Перестановка nodes/components/events не меняет результат
4. Архитектурная запись — выполнено
   - docs/architecture.md: sparse Reference/Eigen, Qt 6, API-independent GPU plan

## Примеры и диагностика
RC: 5000 steps, residual 6.85167e-17.
RLC: 20000 steps, residual 2.69465e-16.
Switch: 12 steps, residual 0.
Проверены malformed files/schema/UUID/parameters, missing reference/terminal,
floating node, current cutset, open-switch island, conflicting ideal sources,
invalid/duplicate gates. Singular diagnostics привязаны к UUID неизвестной.
Save→load сохраняет геометрию, UUID, gates и unknown extension records.

## Начальный benchmark
Команда: build/Release/powerdrive-benchmark examples/rlc.pds 10
200000 steps; compile 0.000148 s; wall 0.975475 s; 205028 steps/s.
Wall/sim ratio 2.43869. Оценка result payload 2240112 bytes.
Это один локальный замер, не обещание производительности и не peak RAM.
Reference sparse row-map implementation ещё не оптимизирована.

## Пределы
Milestone 0 — архитектурный прототип, не готовое desktop-приложение.
Milestone 1 не принят: нет editor, hierarchy, diode, Trapezoidal, Scope, 2L/NPC.
Stop проверяется между шагами; ограничение длительности факторизации
большой сети и streamed results требуют следующих этапов.
