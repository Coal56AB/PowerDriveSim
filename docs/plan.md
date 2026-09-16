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

Trapezoidal и ideal diode / active-set solve реализованы и проверены.
Ближайший шаг M1 — типизированные порты, документ редактора и Qt 6 GUI поверх ядра
этих операций; полный M1 нельзя объявлять по наличию solver или красивой оболочки.
Остальные Milestone строго по полному ТЗ, без параллельного раннего Controller.
## Состояние после плоского редактора

Модель явных проводов, типизированные порты, probes, Document transactions, Qt desktop, Inspector, фоновый Run/Stop и Scope реализованы. UI-тест создаёт RC мышью и проверяет расчёт, историю, сохранение, восстановление, gates и probes. Подробности в [отчёте редактора](verification-m1-desktop.md).

Следующий обязательный блок — hierarchy/public ports/instances/flatten и атомарные 2L/NPC. Плоский редактор закрывает только соответствующую часть критериев 1, 7, 9–11, 14; наличие общего solver и Scope не означает приёмку полного M1.

Ядро и визуальные операции иерархии реализованы и проверены: определения/экземпляры schema 7, public ports, числовые параметры, вложенность, flatten с картой происхождения, deep detach, grouping, история, breadcrumbs, дерево и диалог интерфейса. Запуск и сохранение из вложенного уровня используют полный проект. Следующий шаг — независимые окна и настройки графиков вложенных экземпляров; затем атомарные 2L/NPC. Полный набор Windows Release: 14/14 CTest. См. [контракт и проверки](hierarchy.md).
