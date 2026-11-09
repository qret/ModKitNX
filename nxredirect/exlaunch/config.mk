#----------------------------- User configuration -----------------------------

# Common settings
#------------------------

# How you're loading your module. Used to determine how to find the target module. (AsRtld/Module/Kip)
LOAD_KIND := Module

# Program you're targetting. Used to determine where to deploy your files.
#
# ВНИМАНИЕ: на сам модуль этот номер НЕ влияет. nxredirect универсален: TitleID он
# спрашивает у ядра во время работы (svcGetInfo ProgramId), а путь мода собирает сам.
# PROGRAM_ID остаётся только для служебных целей шаблона (путь `make deploy-*`).
# Поэтому здесь стоит заведомо нейтральное значение.
PROGRAM_ID := 0100000000010000

# Имя выходного NSO внутри exefs-мода = СЛОТ СУБМОДУЛЯ (решение гибрида №4).
# Loader грузит subsdk0..subsdk9. По умолчанию берём старший свободный — subsdk9:
# игры занимают слоты с начала (у Trails the 2nd занят только subsdk0), поэтому
# конфликт со штатным субмодулем игры или с чужим модом менее вероятен.
# Переопределяется из командной строки:  make MODULE_NAME=subsdk5
MODULE_NAME := subsdk9

# Optional path to copy the final ELF to, for convenience.
ELF_EXTRACT :=

# Python command to use. Must be Python 3.4+.
PYTHON := python3

# JSON to use to make .npdm
NPDM_JSON := application.json

# Additional C/C++ flags to use.
# GCC 14 из devkitA64 ругается на конструкцию в шаблоне exlaunch
# (hook_impl.cpp, anonymous typedef struct) — у шаблона включён -Werror,
# поэтому понижаем именно это предупреждение, остальные остаются ошибками.
C_FLAGS := -Wno-error=non-c-typedef-for-linkage
CXX_FLAGS := 

# AsRtld settings
#------------------------

# Path to the SD card. Used to mount and deploy files on SD, likely with hekate UMS.
MOUNT_PATH := /mnt/k

# Module settings
#------------------------

# Settings for deploying over FTP. Used by the deploy-ftp.py script.
FTP_IP := 192.168.0.235
FTP_PORT := 5000
FTP_USERNAME := anonymous
FTP_PASSWORD :=

# Settings for deploying to Ryu. Used by the deploy-ryu.sh script.
# Путь не зашит: make deploy-ryu RYU_PATH=<папка данных Ryujinx> (в сборке модуля не участвует).
RYU_PATH ?=
