# kernel_pc_demo

> Модуль ядра Linux: паттерн producer/consumer на hrtimer + kfifo с двумя вариантами bottom-half — **tasklet** и **workqueue**. Собирается и грузится в собственное ядро в QEMU, поэтому работает и под WSL2, где хостовых заголовков нет.

Producer — колбэк высокоточного таймера (`hrtimer`), который генерирует случайные числа и кладёт их в `kfifo`. Consumer вычитывает очередь и копит статистику; способ выбирается на лету параметром `consumer_type`. Смысл демонстрации — увидеть разницу между атомарным и процессным контекстом на живых цифрах: tasklet не имеет права спать и на переполнении очереди теряет события, workqueue может подождать через `msleep()` и довычитать всё.

## Что демонстрирует

- **hrtimer как top-half**: колбэк никогда не спит и не ждёт; полный `kfifo` — событие немедленно дропается.
- **kfifo без блокировок**: один producer и один consumer — `kfifo_put`/`kfifo_get` потокобезопасны сами по себе.
- **tasklet (атомарный контекст)**: нельзя `msleep`, `mutex_lock`, `kmalloc(GFP_KERNEL)`; `sum`/`last_value` не нужны блокировки — tasklet не выполняется параллельно сам с собой.
- **workqueue (процессный контекст)**: можно спать, поэтому `dropped` стремится к нулю; но `queue_work()` может поставить работу повторно, поэтому `sum`/`last_value` защищены мьютексом.
- **sysfs-интерфейс через `module_param_cb`**: запуск теста, чтение результата и статистики, переключение consumer-а, сброс.

## Архитектура

```
      hrtimer (top-half, атомарный контекст, interval_us)
           │  val = get_random_u32() % 1000
           ▼
      kfifo_put ──── полон? ──► dropped++          produced++
           │                                            │
           │ schedule bottom-half                       │
           ├──────────────► consumer_type=0: tasklet_schedule()
           └──────────────► consumer_type=1: queue_work(wq)
                                    │
                                    ▼
                             kfifo_get цикл ─► consumed++, sum += val, last_value = val
                                                     │
                    tasklet:   пусто ─► выйти сразу (без ожидания)
                    workqueue: пусто ─► msleep(1), пока producer жив
```

Соответствие файлов:

| Файл | Роль |
| --- | --- |
| [src/pc_demo.h](src/pc_demo.h) | `struct pc_ctx`, коды возврата, границы параметров, внутренний API |
| [src/main.c](src/main.c) | `init`/`exit`, параметры загрузки, `kfifo_alloc`, `hrtimer_setup` |
| [src/params.c](src/params.c) | `module_param_cb`: `run`/`result`/`stats`/`consumer_type`/`reset`, валидация |
| [src/producer.c](src/producer.c) | колбэк hrtimer (top-half) |
| [src/consumer.c](src/consumer.c) | tasklet- и workqueue-обработчики (bottom-half) |

## Статус

Сейчас в репозитории **заготовка**: каркас собирается без предупреждений, грузится и выгружается, все пять параметров видны в sysfs. Реализованы валидация параметров, аллокация `kfifo`, установка hrtimer, сброс счётчиков, чтение/запись `consumer_type`.

Не реализовано (тела помечены `TODO` по шагам задания):

- `pc_producer_timer_fn()` — генерация значения, `kfifo_put`, планирование bottom-half, перезапуск таймера;
- `pc_tasklet_consumer()` / `pc_work_consumer()` — циклы вычитывания;
- `pc_consumer_setup()/teardown()/schedule()` — `tasklet_init`/`create_singlethread_workqueue`, `tasklet_kill`/`flush`+`destroy`;
- `run` — оркестрация запуска и ожидание через `wait_event_timeout()`;
- `reset` — остановка и очистка;
- форматирование `result`/`stats` (`avg` через `div_u64`, `warn: lost=N`).

Примеры вывода ниже — это **целевой** формат из задания, а не текущий вывод заготовки.

## Параметры загрузки

| Параметр | Тип | По умолчанию | Ограничения |
| --- | --- | --- | --- |
| `fifo_size` | uint | 64 | степень двойки, 4..1024 |
| `num_events` | uint | 200 | 1..50000 |
| `interval_us` | uint | 1000 | 100..1000000 |
| `consumer_type` | uint | 0 | 0 — tasklet, 1 — workqueue |

Нарушение любого ограничения — `insmod` падает с `-EINVAL` и понятной строкой в `dmesg`.

## Интерфейс sysfs

Всё лежит в `/sys/module/kernel_pc_demo/parameters/`:

| Параметр | Доступ | Что делает |
| --- | --- | --- |
| `run` | w (0200) | `echo 1 > run` — прогнать тест, блокируется до конца; `-EBUSY`, если тест уже идёт |
| `result` | r (0444) | `produced=497 consumed=497 dropped=3 consumer=tasklet ok` (при `consumed < produced` добавляется `warn: lost=N`) |
| `stats` | r (0444) | `produced=497 consumed=497 dropped=3 sum=248312 last=612 avg=499` |
| `consumer_type` | rw (0644) | `echo 1 > consumer_type` — переключить на workqueue (нельзя во время теста) |
| `reset` | w (0200) | `echo 1 > reset` — остановить таймер и consumer, очистить очередь и счётчики |

Коды возврата: `PC_OK 0`, `PC_INVALID -EINVAL`, `PC_NOMEM -ENOMEM`, `PC_BUSY -EBUSY`, `PC_TIMEOUT -ETIMEDOUT`.

## Требования (Debian/Ubuntu, в т.ч. WSL2)

```bash
sudo apt-get update
sudo apt-get install build-essential flex bison bc libelf-dev libssl-dev \
                     cpio qemu-system-x86 gdb
# опционально для разработки:
sudo apt-get install clang-format bear
```

## Быстрый старт

```bash
make qemu-setup     # разово: качает и собирает ядро + initramfs (долго, ~10-20 мин)
make qemu-boot      # собрать модуль и загрузить гостя
# внутри гостя:
insmod /mnt/host/build/kernel_pc_demo.ko fifo_size=64 num_events=500 interval_us=500
echo 1 > /sys/module/kernel_pc_demo/parameters/run
cat /sys/module/kernel_pc_demo/parameters/result
cat /sys/module/kernel_pc_demo/parameters/stats
rmmod kernel_pc_demo
poweroff -f         # выйти из гостя
```

`make help` покажет все цели.

## Проверка сценария: tasklet против workqueue

Целевой прогон целиком, внутри гостя:

```bash
insmod /mnt/host/build/kernel_pc_demo.ko fifo_size=64 num_events=500 interval_us=500 consumer_type=0
P=/sys/module/kernel_pc_demo/parameters

echo 1 > $P/run
cat $P/result     # produced=497 consumed=497 dropped=3 consumer=tasklet ok
cat $P/stats      # produced=497 consumed=497 dropped=3 sum=248312 last=612 avg=499

echo 1 > $P/reset
echo 1 > $P/consumer_type
echo 1 > $P/run
cat $P/result     # produced=500 consumed=500 dropped=0 consumer=workqueue ok

rmmod kernel_pc_demo
```

Что смотреть:

- **`dropped` у workqueue должен быть заметно меньше (в идеале 0)** — consumer спит на пустой очереди вместо потери событий.
- **Уменьшая `fifo_size` (4..8) и `interval_us` (100)** легко воспроизвести переполнение и увидеть рост `dropped` у tasklet.
- **`consumed` не должен отставать от `produced`** после завершения теста: `run` дожидается producer-а и дренирует consumer (`tasklet_kill` / `flush_workqueue`) до чтения статистики.

## Структура репозитория

```
.
├── Makefile            # сборка под хост + цели qemu-* + compile_commands
├── Kbuild              # имя модуля и список объектов
├── src/
│   ├── pc_demo.h       # struct pc_ctx, коды возврата, внутренний API
│   ├── main.c          # init/exit, параметры загрузки, kfifo_alloc
│   ├── params.c        # module_param_cb: run/result/stats/consumer_type/reset
│   ├── producer.c      # hrtimer callback (top-half)
│   └── consumer.c      # tasklet и workqueue обработчики (bottom-half)
├── build/              # сюда складываются ВСЕ артефакты сборки (.ko и пр.)
├── .vscode/            # графический дебаг через QEMU из коробки
│   ├── launch.json     # подключение отладчика к gdbstub QEMU
│   ├── tasks.json      # сборка / автозапуск QEMU
│   ├── settings.json   # IntelliSense (C, gnu11, compile_commands)
│   └── extensions.json # рекомендация ms-vscode.cpptools
└── devtools/           # автономное QEMU-окружение
    ├── config.defaults # версии ядра/busybox, параметры QEMU, KERNEL_DEBUG
    ├── setup.sh        # сборка минимального ядра + initramfs (разово)
    ├── build.sh        # сборка ЭТОГО модуля против QEMU-ядра
    ├── boot.sh         # запуск QEMU (+ --gdb / --test)
    ├── gdb.sh          # подключение GDB, брейк на инициализации модуля
    ├── test.sh         # авто insmod/rmmod (для CI)
    ├── kernel.config       # тонкий профиль конфигурации ядра
    ├── kernel.debug.config # debug-профиль (KASAN/lockdep/kmemleak), опционально
    └── initramfs/init  # PID 1 гостя: монтирует 9p, даёт shell
```

Все артефакты сборки (`*.ko`, `*.o`, `*.mod.c`, `Module.symvers`, `modules.order`, `.*.cmd`) попадают только в `build/`. Корень проекта и `src/` остаются чистыми — в `src/` лежат лишь исходники.

Имя модуля задаётся в двух местах: `obj-m`/`kernel_pc_demo-y` в `Kbuild` и `MODULE_NAME` в `Makefile`. Новые файлы добавляй объектами в `kernel_pc_demo-y`.

## Замечания по API ядра

Проект собирается против ядра **6.18.37**, и часть API из формулировки задания там уже изменилась:

- **`hrtimer_init()` удалён** (начиная с 6.15). Вместо связки `hrtimer_init()` + присваивание `timer.function` используется один вызов: `hrtimer_setup(&ctx->timer, pc_producer_timer_fn, CLOCK_MONOTONIC, HRTIMER_MODE_REL)`.
- **`tasklet_init(t, func(unsigned long), data)` ещё есть**, поэтому сигнатура `pc_tasklet_consumer(unsigned long data)` оставлена как в задании. Современная альтернатива — `tasklet_setup()` с колбэком `void (*)(struct tasklet_struct *)`.
- **`create_singlethread_workqueue()` и `flush_workqueue()` на месте** — используются как в задании.
- `consumer_type` обслуживается `module_param_cb` и потому выставляется **до** `init` (параметры разбираются раньше). `pc_init()` его валидирует, но не перезаписывает.

## Развёртывание, запуск, отладка

### 1. Развёртывание окружения (разово)

```bash
make qemu-setup        # = devtools/setup.sh
```
Скрипт скачивает исходники ядра (по умолчанию **6.18.37**, LTS) и BusyBox, собирает минимальное ядро с отладочной информацией (`vmlinux` для GDB, `nokaslr`, debugfs/proc/sys, 9p) и пакует initramfs из статического BusyBox. Всё кладётся в `devtools/.cache/` (в `.gitignore`). Шаги идемпотентны: повторный запуск ничего не пересобирает, пока не менялись конфиги. После смены `KERNEL_VERSION` запусти `setup.sh` снова. Бери версию из LTS-серии (6.18, 6.12, ...): обычный stable после EOL вычищается с kernel.org и URL начинает отдавать 404.

### 2. Сборка модуля

```bash
make qemu-build        # = devtools/build.sh: сборка против QEMU-ядра -> build/kernel_pc_demo.ko
```
Под нативным Linux с установленными заголовками можно собрать и под хостовое ядро обычным `make` (тогда `make load` / `make unload` грузят в хост). Под WSL2 используй только `qemu-*`.

### 3. Запуск и проверка

```bash
make qemu-boot         # собрать + загрузить интерактивного гостя
```
Корень проекта виден в госте как `/mnt/host/` через 9p — правки на хосте сразу доступны в VM, образ пересобирать не нужно. Собранный модуль лежит в `/mnt/host/build/`. После `insmod` вся работа идёт через `/sys/module/kernel_pc_demo/parameters/` (см. «Проверка сценария»).

Автотест без интерактива (для CI; выходит с ненулевым кодом при ошибке):
```bash
make qemu-test         # insmod -> dmesg -> rmmod -> poweroff
```

### 4. Отладка через GDB

**Как устроено.** Ядро в QEMU отдаёт отладку по gdbstub на `:1234`. Символы `vmlinux` есть сразу, а символы **модуля** появляются только после его загрузки и вызова `lx-symbols` (он читает адреса секций модуля из памяти ядра и делает `add-symbol-file`). Поэтому брейк на функции модуля до `insmod` + `lx-symbols` не к чему привязать. `nokaslr` в cmdline делает адреса стабильными между перезагрузками.

**Два пути — выбери один за раз (один клиент на gdbstub!):**
- **VS Code (F5)** — графика, брейки кликом. `make qemu-debug`, затем F5 «Kernel: attach to QEMU». Сырые команды gdb — в Debug Console с префиксом `-exec`.
- **Терминал** — `make qemu-debug` в одном терминале, `make gdb-attach` в другом. Надёжнее в моменты, когда cppdbg конфликтует с `lx-symbols` (см. ниже).

**Канонический кейс — брейк в consumer-е.**
`pc_work_consumer` удобнее всего: он в `.text` и работает в процессном контексте, останов там ничего не ломает. Терминальный путь:
```
make gdb-attach                 # подключился, взвёл break do_init_module, continue
# в госте:
insmod /mnt/host/build/kernel_pc_demo.ko consumer_type=1   # -> останов на do_init_module
# если break на do_init_module НЕ сработал на insmod: пауза (Ctrl-C),
# затем -exec lx-symbols (перезагружает символы vmlinux и переармирует
# брейкпоинты), после чего повтори insmod
```
```
(gdb) lx-symbols                # подгрузить символы kernel_pc_demo
(gdb) break pc_work_consumer
(gdb) continue
# в госте:
echo 1 > /sys/module/kernel_pc_demo/parameters/run   # -> останов в pc_work_consumer
```
Дальше как в обычном дебаге: `bt`, `next`/`step`, `info locals`, `p val`, `p ctx->sum`, `p *ctx`.

**Разобранные кейсы:**
- *Посмотреть состояние очереди:* в останове `p ctx->fifo.kfifo.in - ctx->fifo.kfifo.out` (сколько элементов сейчас лежит) и `p ctx->fifo.kfifo.mask`.
- *Поймать дроп:* `watch ctx->dropped.counter` — останов на инкременте, `bt` покажет путь из колбэка таймера.
- *Проверить, что producer действительно top-half:* брейк в `pc_producer_timer_fn`, затем `bt` — в стеке будет `hrtimer_run_queues`/softirq, а не путь из процесса. Долгий останов там сбивает тайминг теста: `num_events` не наберётся за таймаут `run`, ожидай `-ETIMEDOUT`.
- *Проверить утечку:* собрать debug-профиль (`make qemu-setup-debug`), убрать `kfifo_free` из `pc_exit`, `insmod`/`rmmod`, затем в госте `echo scan > /sys/kernel/debug/kmemleak; cat /sys/kernel/debug/kmemleak`.
- *Поймать сон в атомарном контексте:* debug-профиль включает `DEBUG_ATOMIC_SLEEP` + lockdep — вызов `msleep()`/`mutex_lock()` из tasklet-обработчика сразу даст `BUG: sleeping function called from invalid context` в `dmesg`. Полезно как самопроверка ограничений задания.

**Тяжёлый случай — брейк в `__init` (`pc_init`).** Стараются избегать: функция в `.init.text`, которую ядро **освобождает сразу после init**, `lx-symbols` эту секцию не всегда мапит (`info symbol mod->init` → «No symbol»), а cppdbg об неё спотыкается. Если всё же нужно — стой на `do_init_module` и ставь брейк **по адресу**, без имени:
```
(gdb) break *mod->init          # mod доступен в кадре do_init_module
(gdb) continue                  # останов на входе pc_init
```

**Когда cppdbg шумит `No breakpoint number N` / `-var-create: unable to create variable object`** — это cppdbg дерётся с `lx-symbols` (тот удаляет и пересоздаёт символы/брейки модуля на каждый хук). Не твой баг. Пройди этот момент в терминальном `make gdb-attach` — там этой бухгалтерии нет.

> TUI в терминальном gdb: `TUI=1 make gdb-attach` (или `Ctrl-X A` в сессии). По умолчанию off — с `target remote` и выводом `lx-symbols` панели легко «съезжают» (`Ctrl-L` перерисовывает).

### 5. Профили ядра: тонкий и debug

По умолчанию собирается **тонкий** профиль (`devtools/kernel.config`) — быстрый, с базовым ftrace и символами для GDB. Когда нужны санитайзеры, собери **debug**-профиль, который домешивает `devtools/kernel.debug.config`:

```bash
make qemu-setup-debug          # = KERNEL_DEBUG=1 devtools/setup.sh
```
Debug-профиль добавляет KASAN (use-after-free / out-of-bounds в памяти ядра; на x86 — обязательно `KASAN_GENERIC`), `DEBUG_KMEMLEAK` (забытый `kfifo_free` при `rmmod`), lockdep (`PROVE_LOCKING`) + `DEBUG_ATOMIC_SLEEP` (дедлоки, сон под спинлоком и в tasklet), kprobes и `IKCONFIG`. Для этого модуля debug-профиль особенно полезен: он ловит ровно те ошибки, от которых предостерегает задание. KASAN примерно удваивает расход памяти — подними `QEMU_MEM` до `2G` в `devtools/config.local`. Переключение профиля `setup.sh` замечает по стемпу и пересобирает ядро. Чтобы debug был постоянным, добавь `KERNEL_DEBUG=1` в `devtools/config.local`.

> KGDB-по-serial намеренно не включён: отладку даёт gdbstub QEMU (`make qemu-debug`), serial-путь в этом окружении избыточен.

## WSL2: важные нюансы

- **Держи проект в ext4 WSL** (`~/projects/...`), а не в `/mnt/c/...`. Сборка ядра на диске Windows через drvfs работает в разы медленнее и ломается на правах/регистре имён.
- **Заголовки ставить не нужно.** `apt install linux-headers-$(uname -r)` под WSL2 падает (ядро кастомное, заголовков в репозитории нет) — и не требуется: `setup.sh` собирает заголовки для своего ядра сам.
- **Ускорение KVM.** Проверь `ls -l /dev/kvm`. Есть и доступно — `boot.sh` сам добавит `-enable-kvm`. Нет — поднимется TCG (программная эмуляция, медленнее, но рабочая). На таймингах это заметно: под TCG `interval_us=100` даёт другую картину дропов, чем под KVM. На Windows 11 nested virtualization для WSL2 включён по умолчанию; при необходимости добавь в `%UserProfile%\.wslconfig`:
  ```ini
  [wsl2]
  nestedVirtualization=true
  ```
  затем `wsl --shutdown`.

## VS Code: графический дебаг и IntelliSense

В репозитории лежит готовый `.vscode/` — брейкпоинты по клику, шаги, стек, watch. Нужно расширение `ms-vscode.cpptools` (VS Code предложит его поставить из `extensions.json`; ставь в WSL-remote).

IntelliSense без ложных ошибок на kernel-инклюдах:
```bash
make compdb            # bear -- make -> compile_commands.json (его подхватит cpptools)
```

Рекомендуемый поток отладки (надёжный):
1. В терминале VS Code: `make qemu-debug` — QEMU встаёт на паузу на `:1234`.
2. Ставишь брейкпоинт в `src/consumer.c` (например, в `pc_work_consumer`).
3. F5 с конфигурацией **«Kernel: attach to QEMU»** — отладчик подключается.
4. В Debug Console один раз: `-exec lx-symbols` — это включает автоподгрузку символов модуля при каждом `insmod` (на весь сеанс).
5. Continue — гость догружается до shell (его консоль в том же терминале). Там: `insmod /mnt/host/build/kernel_pc_demo.ko` и запись в `run`. Брейкпоинт связывается и срабатывает.

Конфигурация **«Kernel: build, boot & attach»** делает шаги 1 и 3 одной кнопкой (через `tasks.json`), но фоновый матчер задачи капризен между версиями VS Code — если ведёт себя странно, используй вариант с attach.

Отличия от отладки обычного userspace-приложения: ядро собрано с `-O2`, поэтому часть локальных переменных будет `<optimized out>`, а шаг иногда прыгает не по строкам; `insmod` и запись в sysfs ты инициируешь руками в консоли гостя. Управление потоком (брейкпоинты, шаги, стек) — один в один как в обычном дебаге.

## Траблшутинг

- **`insmod` падает с `Invalid parameters`** — смотри `dmesg`: модуль печатает, какое именно ограничение нарушено (`fifo_size` не степень двойки, выход за диапазон и т.д.).
- **`echo 1 > run` возвращает «Device or resource busy»** — тест уже идёт (`-EBUSY`). Дождись завершения или сделай `reset`.
- **`consumed < produced` после теста** — bottom-half не был дренирован до чтения статистики: в `run` нужен `tasklet_kill` / `flush_workqueue` после остановки таймера.
- **`setup.sh` падает на отсутствии пакета** — доустанови из списка требований (`flex`, `bison`, `bc`, `libelf-dev`, `libssl-dev`, `cpio`).
- **Сборка ядра упала на `-Werror`** — фрагмент уже передаёт `-Wno-error` и `-std=gnu11`; если всё равно падает, проверь версию GCC и при необходимости понизь `KERNEL_VERSION` в `config.local`.
- **9p mount failed в госте** — обычно ядро собрано без `CONFIG_NET_9P_VIRTIO`; пересобери (`make qemu-setup`), фрагмент его включает.
- **GDB не видит символы** — убедись, что грузишь `vmlinux` из `devtools/.cache/kernel-build/` (это делает `gdb.sh`), а не stripped-образ.
- **Мало места** — кэш ядра занимает несколько ГБ; чисти `devtools/.cache/` при смене версии.

## Формат сдачи

```
студент_фамилия_kernel_pc_demo.tar.gz
├── Makefile
├── Kbuild
└── src/
    ├── main.c
    ├── params.c
    ├── producer.c
    └── consumer.c
```

`src/pc_demo.h` идёт вместе с исходниками (общий контекст и прототипы). Собрать архив из чистого дерева:

```bash
make clean
tar czf ../студент_фамилия_kernel_pc_demo.tar.gz Makefile Kbuild src/
```

## Благодарности

QEMU-окружение (минимальное ядро + BusyBox-initramfs + 9p + gdbstub) смоделировано по `devtools/` из проекта [sysprog21/lkmpg](https://github.com/sysprog21/lkmpg) (The Linux Kernel Module Programming Guide). Код примеров lkmpg распространяется под GPL-2.
