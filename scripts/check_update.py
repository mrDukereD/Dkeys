# Проверка обновлений DKeys (GitHub Releases)
"""
Сам DKeys.exe в сеть не ходит - всё делает этот скрипт. Он смотрит последний релиз на GitHub, и если он новее установленной версии,
спрашивает, скачать ли. По «Да» скачивает DKeys.exe, закрывает работающий DKeys (DKeys.exe do exit), заменяет файл и запускает новый.
Старая версия остаётся рядом как DKeys.exe.old (на случай отката).

По умолчанию запускается раз в неделю из [Schedule] (строка «Проверка обновлений», выключается словом off в начале условия).
Вручную:  python check_update.py --manual   (скажет и «у вас последняя версия», и об ошибках; без --manual скрипт молчит, пока нет новой версии)
Нужно: [Remote] Enabled=1 в DKeys.ini (иначе DKeys не закрыть командой - скрипт попросит закрыть его вручную).
Скачивание - только с github.com, проверяются размер, SHA-256 (если GitHub его сообщает), заголовок exe и версия внутри файла.
"""
import ctypes
import hashlib
import json
import os
import subprocess
import sys
import time
import urllib.request
from ctypes import wintypes

REPO = 'mrDukereD/Dkeys'
DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))        # папка программы: scripts\ лежит внутри неё
EXE = os.path.join(DIR, 'DKeys.exe')
NOWIN = 0x08000000
MANUAL = '--manual' in sys.argv


def hide_console():                                                       # запуск из DKeys открывает чёрное окно: прячем, если оно только наше
    try:
        k = ctypes.windll.kernel32
        pl = (ctypes.c_uint * 4)()
        if k.GetConsoleProcessList(pl, 4) == 1 and k.GetConsoleWindow():
            ctypes.windll.user32.ShowWindow(k.GetConsoleWindow(), 0)
    except Exception:
        pass


def box(text, flags=0x40):                                                # 0x40 информация, 0x10 ошибка, 0x24 вопрос Да/Нет; 0x40000 - поверх окон
    return ctypes.windll.user32.MessageBoxW(None, text, 'Обновление DKeys', flags | 0x40000)


def version_of(path):
    """Версия из ресурса exe (FILEVERSION) -> (1, 6) или None."""
    try:
        v = ctypes.windll.version
        n = v.GetFileVersionInfoSizeW(path, None)
        if not n:
            return None
        buf = ctypes.create_string_buffer(n)
        if not v.GetFileVersionInfoW(path, 0, n, buf):
            return None
        p, ln = ctypes.c_void_p(), wintypes.UINT()
        if not v.VerQueryValueW(buf, '\\', ctypes.byref(p), ctypes.byref(ln)):
            return None
        d = (ctypes.c_uint * 4).from_address(p.value)                     # dwSignature, dwStrucVersion, dwFileVersionMS, dwFileVersionLS
        return (d[2] >> 16, d[2] & 0xFFFF)
    except Exception:
        return None


def parse_tag(tag):
    s = ''.join(c if (c.isdigit() or c == '.') else ' ' for c in tag.strip().lstrip('vV')).split()
    try:
        return tuple(int(x) for x in s[0].split('.'))[:2] if s else None
    except ValueError:
        return None


def get(url, timeout=20):
    return urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'DKeys-update', 'Accept': 'application/vnd.github+json'}), timeout=timeout)


def fail(text, code=1):
    if MANUAL:
        box(text, 0x10)
    return code


def main():
    hide_console()
    cur = version_of(EXE)
    try:
        rel = json.loads(get('https://api.github.com/repos/%s/releases/latest' % REPO).read().decode('utf-8'))
    except Exception as e:
        return fail('Не удалось проверить обновления (нет сети или GitHub недоступен):\n%s' % e)
    new = parse_tag(rel.get('tag_name', ''))
    if not new:
        return fail('Непонятная версия релиза: %s' % rel.get('tag_name'))
    if cur and new <= cur:
        if MANUAL:
            box('У вас последняя версия DKeys %d.%d.' % cur)
        return 0
    asset = next((a for a in rel.get('assets', []) if a.get('name', '').lower() == 'dkeys.exe'), None)
    if not asset or not str(asset.get('browser_download_url', '')).startswith('https://github.com/'):
        return fail('В релизе %s нет файла DKeys.exe.' % rel.get('tag_name'))
    notes = (rel.get('body') or '').strip().replace('\r', '')
    notes = notes.split('\n## ')[0][:700]
    q = 'Вышла новая версия DKeys %d.%d (у вас %s).\n\n%s\n\nСкачать и установить сейчас? DKeys перезапустится.' % (
        new[0], new[1], '%d.%d' % cur if cur else 'неизвестно', notes)
    if box(q, 0x24) != 6:                                                  # 6 = «Да»
        return 0
    tmp = EXE + '.new'
    try:
        h = hashlib.sha256()
        size = 0
        with get(asset['browser_download_url'], 60) as r, open(tmp, 'wb') as f:
            while True:
                b = r.read(65536)
                if not b:
                    break
                f.write(b)
                h.update(b)
                size += len(b)
        dig = str(asset.get('digest') or '')
        if size != asset.get('size', size) or size < 100000:
            raise ValueError('неполный файл (%d байт)' % size)
        if dig.startswith('sha256:') and dig[7:].lower() != h.hexdigest():
            raise ValueError('не совпала контрольная сумма SHA-256')
        with open(tmp, 'rb') as f:
            if f.read(2) != b'MZ':
                raise ValueError('это не exe')
        if (version_of(tmp) or new) < new:
            raise ValueError('версия внутри файла старее заявленной')
    except Exception as e:
        try:
            os.remove(tmp)
        except OSError:
            pass
        box('Не удалось скачать обновление: %s' % e, 0x10)
        return 2
    if os.path.isfile(EXE):
        r = subprocess.run([EXE, 'do', 'exit'], creationflags=NOWIN).returncode      # 0 принято, 2 не запущен, 3 приём выключен
        if r == 3:
            box('DKeys не принимает команды: включите [Remote] Enabled=1 в DKeys.ini или закройте DKeys вручную и нажмите OK.', 0x30)
    old = EXE + '.old'
    done = False
    for _ in range(150):                                                   # ждём, пока DKeys закроется и отпустит файл (до 30 с)
        try:
            if os.path.isfile(EXE):
                os.replace(EXE, old)
            os.replace(tmp, EXE)
            done = True
            break
        except PermissionError:
            time.sleep(0.2)
    if not done:
        if os.path.isfile(old) and not os.path.isfile(EXE):
            os.replace(old, EXE)
        box('DKeys не закрылся, файл не заменён. Закройте DKeys и запустите проверку ещё раз (новый файл: %s).' % tmp, 0x10)
        return 3
    if os.environ.get('DKEYS_ADMIN') == '0':                                         # [General] Admin=0: без прав (DKeys сам сбросит их, если запущен от админа)
        subprocess.Popen([EXE, '/noadmin'], cwd=DIR, creationflags=NOWIN | 0x00000008)
    elif ctypes.windll.shell32.IsUserAnAdmin():
        subprocess.Popen([EXE], cwd=DIR, creationflags=NOWIN | 0x00000008)        # DETACHED_PROCESS
    else:
        ctypes.windll.shell32.ShellExecuteW(None, 'runas', EXE, None, DIR, 1)     # DKeys нужно запускать от администратора
    return 0


if __name__ == '__main__':
    sys.exit(main())
