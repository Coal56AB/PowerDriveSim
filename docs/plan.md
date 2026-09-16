# План и карта приёмки

## Milestone 0 — последовательность
1. Graph + schema → core/model, formats/project; serialization и topology tests
2. Stamp API + independent IR → core/ir, core/compiler; unit tests
3. R/L/C/V/I + Backward Euler → core/solver/reference; RC/RLC analytic tests
4. Ideal switch + events → reference scheduler; regression на commutation
5. Повторяемость → два исполнения одного IR, перестановка входных объектов
6. Benchmark + решения о GUI/sparse/GPU → benchmarks, architecture.md
7. Сборка, все группы CTest, examples, отчёт и публикация
8. Только после полной приёмки M0 — работа M1

## Milestone 1 — модули и обязательные проверки
| Критерий ТЗ | Модули | Проверка |
|---|---|---|
| 1, 14: создание схемы без C++ | apps/desktop, editor model | UI add/connect/run |
| 2: атомарный 2L | hierarchy, converters | open/edit всех 6 ключей/diodes/snubber/gates |
| 3: атомарный NPC | hierarchy, converters | edit clamping/DC caps/neutral |
| 4: произвольная поддерживаемая схема | compiler, reference | mutate converter и generic run |
| 5: RC/RLC/diode BE+Trap | reference, primitives | аналитика, convergence, complementarity |
| 6: одновременные gates | scheduler | file-order permutation, off-grid edges |
| 7: Scope | results, desktop | u/i/gates, zoom/cursors/export |
| 8: диагностика | graph validation, topology | floating/type/ground/source/singular + UUID |
| 9: save/load | versioned format | geometry/hierarchy/UUID/Scope round trip |
| 10: undo/redo | editor command stack | add/delete/connect/move/parameters/hierarchy |
| 11: responsiveness/Stop | worker, cancellation | benchmark + UI input + bounded Stop |
| 12: четыре bundled examples | examples | RC/RLC/2L/NPC assertions |
| 13: CI | .github/workflows | unit/numerical/serialization/examples |

Trapezoidal, ideal diode / active-set solve, иерархический редактор и атомарные
2L/NPC и адресная топологическая диагностика реализованы и проверены.
Все критерии M1 проверены локально; [таблица приёмки и измерения](verification-m1-acceptance.md).
Активен M2. Остальные Milestone строго по полному ТЗ, без раннего Controller.

## Milestone 2 — порядок реализации

1. Источники и модели, необходимые для библиотеки: AC/piecewise/pulse/sine,
   затем явные параметры piecewise-linear полупроводников. Старые ideal-модели сохраняются.
   Sine/pulse/PWL, импорт таблицы и три RC-примера реализованы и проверены;
   PWL S/D с Ron/Roff/Vf и динамика обратного восстановления диода также готовы.
   Добавлен [тиристор T](thyristor.md): идеальная/PWL ветви, gate и удержание по Ih,
   schema 11 и пример управляемого полупериодного выпрямителя.
   Добавлены [MOSFET/IGBT](transistors.md) как редактируемые библиотечные подсхемы
   с отдельным body/antiparallel диодом и атомом канала IGBT (schema 12).
   См. [источники](sources.md), [полупроводники](semiconductors.md).
2. Редактируемые библиотечные AC/DC, DC/DC и DC/AC из приоритетного набора §12;
   precharge/DC-link/braking/discharge и готовые примеры с проверенными метриками.
   [Однофазный и трёхфазный диодные мосты](rectifiers.md) доступны в библиотеке;
   проверены форма и среднее напряжение, ток, мощность, раскрытие и UI.
   Однофазный тиристорный мост с отдельными gate-входами проверен при нескольких
   углах открытия, включая фронты вне основной сетки шага и короткие импульсы.
   Трёхфазный тиристорный мост проверен с шестью внешними записанными gates:
   коммутация, форма/среднее, пассивность и баланс мощности, ideal/PWL и раскрытие.
   [Buck, boost и инвертирующий buck-boost](dcdc.md) имеют публичные L/C/ESR
   и начальные состояния; проверены средние напряжения и баланс энергии.
   [Полумост и полный мост](bridges.md) используют общее определение плеча;
   проверены RL-аналитика, dead time, диоды, энергия и диагностика сквозного тока.
   Двунаправленный DC/DC на общем полумосте проверен в режимах заряда и возврата
   энергии, с внешними комплементарными gates и точной RL-аналитикой.
   [Precharge/DC-link и коммутируемый резистор](dc-link.md) добавлены с примерами
   заряда/обхода/разряда и торможения; проверены RC-аналитика, мощность и энергия.
   [Регулятор переменного напряжения](ac-voltage-controller.md) проверен с двумя
   встречно-параллельными тиристорами: ideal/PWL, форма, RMS, удержание и мощность.
   [2L VSI и 3L NPC](three-phase-inverters.md) доступны в библиотеке с публичными
   C/uC(0)/ESR разделённого DC-link. Проверены совпадение с примерами, мощность,
   сквозное замыкание и нативный UI. [Двухстороннее питание обмоток](open-end-winding.md)
   использует шесть общих полумостов; проверены RL-фазы, уровни, мощность и энергия.
3. Initial conditions, snapshot/continue и улучшения шага с учётом событий.
   [Снимки состояния](snapshots.md) реализованы в ядре и CLI: файл состояния,
   совместимость модели, продолжение и побитовое совпадение восьми схем.
   Следом — desktop save/load/continue/step, затем остальные начальные режимы и шаг.
4. Расширенный анализ Scope: FFT/THD и измерения энергии; scenario/parameter sweep.
5. Документация компонентов, energy balance/steady-state/fault assertions,
   общая проверка критериев M2. Ускоренные варианты сравнивать с generic там, где реализованы.

Не переходить к M3 до проверки всей приёмки M2. Controller/ADC/MCU остаются M6.
## Состояние после плоского редактора

Модель явных проводов, типизированные порты, probes, Document transactions, Qt desktop, Inspector, фоновый Run/Stop и Scope реализованы. UI-тест создаёт RC мышью и проверяет расчёт, историю, сохранение, восстановление, gates и probes. Подробности в [отчёте редактора](verification-m1-desktop.md).

Плоский редактор закрывает соответствующую часть критериев 1, 7, 9–11, 14; наличие общего solver и Scope само по себе не означает приёмку полного M1.

Ядро и визуальные операции иерархии реализованы и проверены: определения/экземпляры schema 7, public ports, числовые параметры, вложенность, flatten с картой происхождения, deep detach, grouping, история, breadcrumbs, дерево и диалог интерфейса. Запуск и сохранение из вложенного уровня используют полный проект. Окна графиков разных уровней остаются открытыми; экземпляры имеют независимые настройки, сохраняемые при группировке/копировании/раскрытии. См. [контракт и проверки](hierarchy.md).

Атомарные 2L VSI / 3L NPC поставляются как редактируемые иерархические проекты.
Проверены аналитические токи RL, уровни напряжения, диоды, перестановки gate-событий,
изменение внутренней топологии и GUI open/edit/run/save. Полный набор Windows Release:
15/15 CTest. [Отчёт преобразователей](verification-m1-converters.md).
