# Бэкап DKeys на Google Диск (всё, кроме whisper)
"""
Копирует папку DKeys целиком (exe, DKeys.ini, memory, scripts, records, langs, overlays, runs...) в папку бэкапа.
Папка whisper (движок и модели, скачиваются заново) пропускается.
Копия добавочная: новые и изменённые файлы копируются, удалённые в бэкапе остаются (случайное удаление не пропадёт и из копии).

Запуск без окна (по расписанию DKeys, секция [Schedule]):
    run pythonw "D:\\Progs\\DKeys\\scripts\\backup.py"
Вручную с окном:  python backup.py [папка_бэкапа]
Если папка бэкапа недоступна (диск не подключён) или robocopy вернул ошибку - появится окно с сообщением, иначе тихо.
Журнал последнего запуска - backup.log в папке бэкапа.
"""
import ctypes
import os
import subprocess
import sys

DEST = r'L:\Мой диск\DukereD\Dkeys'
SRC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))     # папка DKeys: scripts\ лежит внутри неё
SKIP = ['whisper']


def alert(text):
    ctypes.windll.user32.MessageBoxW(None, text, 'Бэкап DKeys', 0x10 | 0x40000)    # MB_ICONERROR | MB_TOPMOST


def main():
    dest = sys.argv[1] if len(sys.argv) > 1 else DEST
    if not os.path.isdir(os.path.dirname(dest)):
        alert('Папка бэкапа недоступна:\n%s\n\nПроверьте, что Google Диск подключён.' % dest)
        return 2
    os.makedirs(dest, exist_ok=True)
    log = os.path.join(dest, 'backup.log')
    cmd = ['robocopy', SRC, dest, '/E', '/R:2', '/W:3', '/NP', '/NDL', '/DCOPY:T', '/MT:4', '/UNILOG:' + log, '/XD']
    cmd += [os.path.join(SRC, d) for d in SKIP]
    cmd += ['/XF', 'backup.log']
    flags = 0x08000000 if os.name == 'nt' else 0                                    # CREATE_NO_WINDOW
    code = subprocess.run(cmd, creationflags=flags).returncode
    if code >= 8:                                                                   # 0-7 - успех (разные варианты «что скопировано»)
        alert('Бэкап завершился с ошибкой (robocopy код %d).\nПодробности: %s' % (code, log))
        return code
    return 0


if __name__ == '__main__':
    sys.exit(main())
