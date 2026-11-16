// Создано src/mkscripts.py из scripts/calendar_add_selection.py - не править руками.
static const wchar_t kEx5[] = LR"PY(# Выделенный текст -> событие в календаре
"""
Берёт выделенный текст (DKeys кладёт его в переменную окружения DKEYS_SEL) и добавляет событие в календарь DKeys,
так же, как быстрый ввод в окне календаря: «завтра 15:00 встреча с Иваном», «через 2 часа позвонить», «25 декабря день рождения».
Результат виден во всплывающей подсказке DKeys.

Подключение: в [Hotkeys] DKeys.ini, например   Win+Shift+K=py calendar_add_selection.py
Нужно: [Remote] Enabled=1 в DKeys.ini (скрипт передаёт команду запущенному DKeys так же, как  DKeys.exe do "calendar add ...").
"""
import ctypes
import os
import subprocess
import sys

text = ' '.join(os.environ.get('DKEYS_SEL', '').replace('"', "'").split())
if not text:
    sys.exit(1)                                                                  # ничего не выделено
exe = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'DKeys.exe')    # папка программы: scripts\ лежит внутри неё
code = subprocess.run([exe, 'do', 'calendar add ' + text], creationflags=0x08000000).returncode
if code == 3:                                                                    # 0 принято, 2 DKeys не запущен, 3 приём команд выключен
    ctypes.windll.user32.MessageBoxW(None, 'DKeys не принимает команды: включите [Remote] Enabled=1 в DKeys.ini.', 'DKeys', 0x30 | 0x40000)
sys.exit(code)
)PY";
