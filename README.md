# kmod-qemu-template

> Шаблон репозитория для разработки модулей ядра Linux с автономным QEMU-окружением: собирает собственное минимальное ядро, грузит модуль в виртуальную машину и даёт отладку через GDB. Работает под WSL2 без хостовых заголовков.

Копируешь шаблон — и сразу пишешь модуль: окружение для сборки, запуска и отладки уже внутри. Ключевая идея: модуль собирается и грузится **не в ядро хоста, а в отдельное ядро в QEMU**. Поэтому шаблон одинаково работает на «обычном» Linux и под WSL2, где хостовых заголовков ядра нет и `insmod` в живое ядро невозможен.

## Структура

```
.
├── Makefile            # сборка под хост + цели qemu-* + compile_commands
├── src/
│   ├── main.c          # отлаживаемый char-device (misc /dev/template, read/write)
│   └── Kbuild          # имя модуля и список объектов
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

Все артефакты сборки (`*.ko`, `*.o`, `*.mod.c`, `Module.symvers`, `modules.order`, `.*.cmd`) попадают только в `build/`. Корень проекта и `src/` остаются чистыми — в `src/` лежат лишь исходники и `Kbuild`.

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
# внутри гостя (модуль создаёт /dev/template):
insmod /mnt/host/build/template.ko
echo hello > /dev/template     # -> template_write
cat /dev/template              # -> template_read
rmmod template
poweroff -f         # выйти из гостя
```

`make help` покажет все цели.

## Развёртывание, запуск, отладка

### 1. Развёртывание окружения (разово)

```bash
make qemu-setup        # = devtools/setup.sh
```
Скрипт скачивает исходники ядра (по умолчанию **6.18.37**, LTS) и BusyBox, собирает минимальное ядро с отладочной информацией (`vmlinux` для GDB, `nokaslr`, debugfs/proc/sys, 9p) и пакует initramfs из статического BusyBox. Всё кладётся в `devtools/.cache/` (в `.gitignore`). Шаги идемпотентны: повторный запуск ничего не пересобирает, пока не менялись конфиги. После смены `KERNEL_VERSION` запусти `setup.sh` снова. Бери версию из LTS-серии (6.18, 6.12, ...): обычный stable после EOL вычищается с kernel.org и URL начинает отдавать 404.

### 2. Сборка модуля

```bash
make qemu-build        # = devtools/build.sh: сборка против QEMU-ядра -> build/template.ko
```
Под нативным Linux с установленными заголовками можно собрать и под хостовое ядро обычным `make` (тогда `make load` / `make unload` грузят в хост). Под WSL2 используй только `qemu-*`.

### 3. Запуск и проверка

```bash
make qemu-boot         # собрать + загрузить интерактивного гостя
```
Корень проекта виден в госте как `/mnt/host/` через 9p — правки на хосте сразу доступны в VM, образ пересобирать не нужно. Собранный модуль лежит в `/mnt/host/build/`. После `insmod` модуль создаёт `/dev/template` (misc-устройство) — читай/пиши его `cat`/`echo`, чтобы дёргать `read`/`write`.

Автотест без интерактива (для CI; выходит с ненулевым кодом при ошибке):
```bash
make qemu-test         # insmod -> dmesg -> rmmod -> poweroff
```

### 4. Отладка через GDB

**Как устроено.** Ядро в QEMU отдаёт отладку по gdbstub на `:1234`. Символы `vmlinux` есть сразу, а символы **модуля** появляются только после его загрузки и вызова `lx-symbols` (он читает адреса секций модуля из памяти ядра и делает `add-symbol-file`). Поэтому брейк на функции модуля до `insmod` + `lx-symbols` не к чему привязать. `nokaslr` в cmdline делает адреса стабильными между перезагрузками.

**Два пути — выбери один за раз (один клиент на gdbstub!):**
- **VS Code (F5)** — графика, брейки кликом. `make qemu-debug`, затем F5 «Kernel: attach to QEMU». Сырые команды gdb — в Debug Console с префиксом `-exec`.
- **Терминал** — `make qemu-debug` в одном терминале, `make gdb-attach` в другом. Надёжнее в моменты, когда cppdbg конфликтует с `lx-symbols` (см. ниже).

**Канонический кейс — брейк в обычной функции модуля (read/write).**
Функции `template_read`/`template_write` лежат в `.text` — привязываются штатно. Терминальный путь:
```
make gdb-attach                 # подключился, взвёл break do_init_module, continue
# в госте:
insmod /mnt/host/build/template.ko   # -> останов на do_init_module
# если break на do_init_module НЕ сработал на insmod: пауза (Ctrl-C),
# затем -exec lx-symbols (перезагружает символы vmlinux и переармирует
# брейкпоинты), после чего повтори insmod
```
```
(gdb) lx-symbols                # подгрузить символы template
(gdb) break template_read
(gdb) break template_write
(gdb) continue
# в госте:
echo hi > /dev/template         # -> останов в template_write
```
Дальше как в обычном дебаге: `bt`, `next`/`step`, `info args`, `info locals`, `p count`, `p *ppos`. В VS Code то же самое: после `insmod` → `do_init_module` сделай `-exec lx-symbols`, затем поставь брейк кликом на строке в `template_read` (после загрузки символов он привяжется) и `cat /dev/template`.

**Разобранные кейсы:**
- *Проследить копирование в user space:* брейк в `template_write`, `next` до `copy_from_user`, `p count`, после копирования `x/16xb template_buf` — увидеть, что реально записалось.
- *Поймать неверное значение:* `watch template_len` — останов на любой записи в переменную, `bt` покажет кто изменил.
- *Проверить утечку:* собрать debug-профиль (`make qemu-setup-debug`), убрать `kfree` из `template_exit`, `insmod`/`rmmod`, затем в госте `echo scan > /sys/kernel/debug/kmemleak; cat /sys/kernel/debug/kmemleak` — kmemleak покажет утёкший `kzalloc`.
- *Стек в точке останова:* `bt` в `template_read` покажет путь `vfs_read → template_read`.

**Тяжёлый случай — брейк в `__init` (`template_init`).** Стараются избегать: функция в `.init.text`, которую ядро **освобождает сразу после init**, `lx-symbols` эту секцию не всегда мапит (`info symbol mod->init` → «No symbol»), а cppdbg об нёй спотыкается. Если всё же нужно — стой на `do_init_module` и ставь брейк **по адресу**, без имени:
```
(gdb) break *mod->init          # mod доступен в кадре do_init_module
(gdb) continue                  # останов на входе template_init
```

**Когда cppdbg шумит `No breakpoint number N` / `-var-create: unable to create variable object`** — это cppdbg дерётся с `lx-symbols` (тот удаляет и пересоздаёт символы/брейки модуля на каждый хук). Не твой баг. Пройди этот момент в терминальном `make gdb-attach` — там этой бухгалтерии нет.

> TUI в терминальном gdb: `TUI=1 make gdb-attach` (или `Ctrl-X A` в сессии). По умолчанию off — с `target remote` и выводом `lx-symbols` панели легко «съезжают» (`Ctrl-L` перерисовывает).

### 5. Профили ядра: тонкий и debug

По умолчанию собирается **тонкий** профиль (`devtools/kernel.config`) — быстрый, с базовым ftrace и символами для GDB. Когда нужны санитайзеры, собери **debug**-профиль, который домешивает `devtools/kernel.debug.config`:

```bash
make qemu-setup-debug          # = KERNEL_DEBUG=1 devtools/setup.sh
```
Debug-профиль добавляет KASAN (use-after-free / out-of-bounds в памяти ядра; на x86 — обязательно `KASAN_GENERIC`), `DEBUG_KMEMLEAK` (забытый `kfree` при `rmmod`), lockdep (`PROVE_LOCKING`) + `DEBUG_ATOMIC_SLEEP` (дедлоки, сон под спинлоком), kprobes и `IKCONFIG`. KASAN примерно удваивает расход памяти — подними `QEMU_MEM` до `2G` в `devtools/config.local`. Переключение профиля `setup.sh` замечает по стемпу и пересобирает ядро. Чтобы debug был постоянным, добавь `KERNEL_DEBUG=1` в `devtools/config.local`.

> KGDB-по-serial намеренно не включён: отладку даёт gdbstub QEMU (`make qemu-debug`), serial-путь в этом окружении избыточен.

## Создание нового модуля из шаблона

```bash
cp -r kmod-qemu-template ~/projects/MyModule && cd ~/projects/MyModule
rm -rf .git && git init
```
Переименование модуля — два места:
1. `src/Kbuild`: `obj-m += template.o` и `template-y := main.o` → замени `template` на имя модуля.
2. `Makefile`: `MODULE_NAME := template` → то же имя.

Дальше пиши код в `src/`. Несколько файлов — добавляй объекты в `template-y` в `src/Kbuild`. Потом обнови этот README под свой модуль (теглайн, секции «Что демонстрирует / Архитектура / Проверка» — как в остальных репозиториях).

## WSL2: важные нюансы

- **Держи проект в ext4 WSL** (`~/projects/...`), а не в `/mnt/c/...`. Сборка ядра на диске Windows через drvfs работает в разы медленнее и ломается на правах/регистре имён.
- **Заголовки ставить не нужно.** `apt install linux-headers-$(uname -r)` под WSL2 падает (ядро кастомное, заголовков в репозитории нет) — и не требуется: `setup.sh` собирает заголовки для своего ядра сам.
- **Ускорение KVM.** Проверь `ls -l /dev/kvm`. Есть и доступно — `boot.sh` сам добавит `-enable-kvm`. Нет — поднимется TCG (программная эмуляция, медленнее, но рабочая). На Windows 11 nested virtualization для WSL2 включён по умолчанию; при необходимости добавь в `%UserProfile%\.wslconfig`:
  ```ini
  [wsl2]
  nestedVirtualization=true
  ```
  затем `wsl --shutdown`.

## VS Code: графический дебаг и IntelliSense

В шаблоне лежит готовый `.vscode/` — брейкпоинты по клику, шаги, стек, watch. Нужен расширение `ms-vscode.cpptools` (VS Code предложит его поставить из `extensions.json`; ставь в WSL-remote).

IntelliSense без ложных ошибок на kernel-инклюдах:
```bash
make compdb            # bear -- make -> compile_commands.json (его подхватит cpptools)
```

Рекомендуемый поток отладки (надёжный):
1. В терминале VS Code: `make qemu-debug` — QEMU встаёт на паузу на `:1234`.
2. Ставишь брейкпоинт в `src/main.c` (например, в `template_init`).
3. F5 с конфигурацией **«Kernel: attach to QEMU»** — отладчик подключается.
4. В Debug Console один раз: `-exec lx-symbols` — это включает автоподгрузку символов модуля при каждом `insmod` (на весь сеанс).
5. Continue — гость догружается до shell (его консоль в том же терминале). Там: `insmod /mnt/host/build/template.ko`. Брейкпоинт связывается и срабатывает.

Конфигурация **«Kernel: build, boot & attach»** делает шаги 1 и 3 одной кнопкой (через `tasks.json`), но фоновый матчер задачи капризен между версиями VS Code — если ведёт себя странно, используй вариант с attach.

Отличия от отладки обычного userspace-приложения: ядро собрано с `-O2`, поэтому часть локальных переменных будет `<optimized out>`, а шаг иногда прыгает не по строкам; `insmod` и запись в `/proc`/sysfs ты инициируешь руками в консоли гостя. Управление потоком (брейкпоинты, шаги, стек) — один в один как в обычном дебаге.

## Траблшутинг

- **`setup.sh` падает на отсутствии пакета** — доустанови из списка требований (`flex`, `bison`, `bc`, `libelf-dev`, `libssl-dev`, `cpio`).
- **Сборка ядра упала на `-Werror`** — фрагмент уже передаёт `-Wno-error` и `-std=gnu11`; если всё равно падает, проверь версию GCC и при необходимости понизь `KERNEL_VERSION` в `config.local`.
- **9p mount failed в госте** — обычно ядро собрано без `CONFIG_NET_9P_VIRTIO`; пересобери (`make qemu-setup`), фрагмент его включает.
- **GDB не видит символы** — убедись, что грузишь `vmlinux` из `devtools/.cache/kernel-build/` (это делает `gdb.sh`), а не stripped-образ.
- **Мало места** — кэш ядра занимает несколько ГБ; чисти `devtools/.cache/` при смене версии.

## Благодарности

QEMU-окружение (минимальное ядро + BusyBox-initramfs + 9p + gdbstub) смоделировано по `devtools/` из проекта [sysprog21/lkmpg](https://github.com/sysprog21/lkmpg) (The Linux Kernel Module Programming Guide). Код примеров lkmpg распространяется под GPL-2.