# PowerDriveSim project format 3

UTF-8 (BOM допустим), LF или CRLF, текстовые записи, десятичные числа SI с точностью double.
Первая строка: PowerDriveSim 3. Комментарии начинаются с # в первой колонке.
Строки в кавычках используют escaping std::quoted: \" и \\.
Идентификаторы — 36-символьные UUID в нижнем регистре; уникальны в проекте.
Геометрия не задаёт электрическое соединение. Связность задаётся ссылками на nodes.

Обязательные одиночные записи:
- project "uuid" "name"
- profile stop_seconds step_seconds integration_method
- nonlinear max_iterations voltage_absolute_tolerance current_absolute_tolerance relative_tolerance

Повторяющиеся записи:
- node "uuid" "name" ground_boolean
- component "uuid" "name" kind "positive_node_uuid" "negative_node_uuid" value initial x y closed_boolean
- event time_seconds "switch_uuid" closed_boolean

integration_method: BackwardEuler или Trapezoidal. kind: R, L, C, V, I, S, D. Boolean — 0 или 1.
R: value в ohm; L в H; C в F; V в V; I в A. У S value зарезервирован. У D value=0, initial=0, closed=0; positive — анод.
initial задаёт uC в V или iL в A; для остальных зарезервирован.
closed используется только у S. x/y — сохранённые координаты будущего редактора.
Дублирующие gates одного switch/time отклоняются, даже с одинаковым значением.
stop и step положительные конечные числа. Интервал начинается с 0.
Прочие общие поля зарезервированы и не меняют физику.

Записи с префиксом x- сохраняются дословно как непрозрачные расширения.
Например x-scope в примерах — сохранённое пожелание выбора канала;
реального Scope в Milestone 0 ещё нет.
Неизвестные основные записи, лишние поля, неверные boolean и версии
отклоняются. Миграции последовательны: v1→v2 добавляет BackwardEuler, v2→v3 — default nonlinear profile (64, 1e-9 V, 1e-12 A, 1e-9). Геометрия, UUID и extensions сохраняются. Запись только v3. Старые схемы не могут содержать D или nonlinear-записи.
Не комментируйте новую семантику только через x-: новый физический смысл
требует новой версии и миграционных тестов.

CSV содержит metadata-комментарий, время, именованные каналы с SI units и UUID,
затем исходные samples. Это экспорт, не формат snapshot/restart.
Для полного воспроизведения сохраните исходный .pds и версию ядра вместе с CSV.
Потоки должны использовать classic locale (стандартный CLI не меняет locale).
Сохранение на диск с atomic replace и autosave относится к редактору Milestone 1.
max_iterations: 1..1024. Absolute tolerances — положительные конечные числа;
relative tolerance — конечное неотрицательное. Эти значения управляют
проверкой complementarity, не добавляют электрическую проводимость.
Имена и UUID в record format должны быть однострочными.
Writer явно задаёт формат double и boolean независимо от stream flags.

## Schema 4 — документ редактора

Текущий writer выдаёт PowerDriveSim 4; loader читает v1..v4.
Старые схемы остаются в режиме nets до преобразования для редактора.
Новые записи:
- wiring nets|wires
- node "id" "name" ground x y (v4 добавляет координаты)
- wire "id" "from_object" "from_port" "to_object" "to_port" bend_count x1 y1 ...
- pattern "id" "name" x y initial
- scope "channel_key"
- scopeview time_begin time_end cursor_a cursor_b (-1 означает auto/unset)

В wires-режиме authoritative connectivity задаётся только wire endpoints.
Поля positive/negative у компонентов пустые и формируются компилятором.
У electrical component есть p/n; switch имеет gate input; pattern — out.
VP/IP имеют scalar out, который нельзя подключать к electrical/gate.
Ground/junction — node endpoint. Пересечения геометрии ничего не соединяют.
Gate events могут адресовать pattern; компилятор раздаёт их подключённым switches.
Два драйвера одного gate отклоняются. Node UUID сохраняется для именованной сети;
для соединённых только выводов используется детерминированный application-defined UUIDv8.
Probe VP не нагружает сеть; IP вставляет идеальную ветвь с нулевым напряжением.
