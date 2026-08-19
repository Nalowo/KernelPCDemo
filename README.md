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
