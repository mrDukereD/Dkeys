"""Собирает встроенные копии скриптов для exe: src/check_update.inc (из scripts/check_update.py) и src/whisper_fetch.inc (из src/whisper_fetch.py).
Запуск: python src/mkscripts.py (build.cmd запускает сам, если найден python)."""
import io, os
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
for src, inc, name in (('scripts/check_update.py', 'src/check_update.inc', 'kEx3'), ('src/whisper_fetch.py', 'src/whisper_fetch.inc', 'kEx4')):
    s = io.open(os.path.join(root, src), encoding='utf-8', newline='').read().replace('\r\n', '\n')
    assert ')PY"' not in s
    io.open(os.path.join(root, inc), 'w', encoding='utf-8', newline='\n').write(
        '// Создано src/mkscripts.py из %s - не править руками.\nstatic const wchar_t %s[] = LR"PY(%s)PY";\n' % (src, name, s))
