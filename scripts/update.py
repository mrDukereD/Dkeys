# Обновить DKeys из D:\Repo\DKeys\obj\DKeys.exe
"""
Закрывает работающий DKeys (DKeys.exe do exit), копирует свежую сборку в папку программы и запускает DKeys заново.

Запуск без окна из DKeys (хоткей, меню или [Schedule]):  run pythonw.exe "D:\\Progs\\DKeys\\scripts\\update.py"
Вручную:  python update.py [--src путь\\к\\DKeys.exe] [--norun]
Нужно: [Remote] Enabled=1 в DKeys.ini (иначе DKeys не закрыть командой - закройте его сами и запустите скрипт ещё раз).
DKeys должен работать от администратора: если скрипт запущен без прав администратора, запуск пройдёт через запрос UAC.
"""
import ctypes
import os
import shutil
import subprocess
import sys
import time

SRC = r'D:\Repo\DKeys\obj\DKeys.exe'
DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))       # папка программы: scripts\ лежит внутри неё
EXE = os.path.join(DIR, 'DKeys.exe')


def alert(text, icon=0x10):
    ctypes.windll.user32.MessageBoxW(None, text, 'Обновление DKeys', icon | 0x40000)


def arg(name, default=None):
    a = sys.argv
    return a[a.index(name) + 1] if name in a and a.index(name) + 1 < len(a) else default


def main():
    src = arg('--src', SRC)
    if not os.path.isfile(src):
        alert('Нет файла сборки:\n%s\n\nСначала соберите DKeys.' % src)
        return 2
    NOWIN = 0x08000000
    if os.path.isfile(EXE):
        r = subprocess.run([EXE, 'do', 'exit'], creationflags=NOWIN).returncode      # 0 принято, 2 не запущен, 3 приём выключен
        if r == 3:
            alert('DKeys не принимает команды: включите [Remote] Enabled=1 в DKeys.ini или закройте DKeys вручную.')
            return 3
    tmp = EXE + '.new'
    shutil.copyfile(src, tmp)
    ok = False
    for _ in range(100):                                                               # ждём, пока DKeys закроется и отпустит файл (до 20 с)
        try:
            os.replace(tmp, EXE)
            ok = True
            break
        except PermissionError:
            time.sleep(0.2)
    if not ok:
        os.remove(tmp)
        alert('DKeys не закрылся за 20 секунд, файл не заменён.\nЗакройте DKeys вручную и запустите скрипт ещё раз.')
        return 4
    if '--norun' in sys.argv:
        return 0
    if ctypes.windll.shell32.IsUserAnAdmin():
        subprocess.Popen([EXE], cwd=DIR, creationflags=NOWIN | 0x00000008)           # DETACHED_PROCESS
    else:
        ctypes.windll.shell32.ShellExecuteW(None, 'runas', EXE, None, DIR, 1)        # запрос UAC: DKeys нужно запускать от администратора
    return 0


if __name__ == '__main__':
    sys.exit(main())
