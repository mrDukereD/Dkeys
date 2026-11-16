"""Собирает src/check_update.inc из scripts/check_update.py (встроенная копия скрипта проверки обновлений для exe).
Запуск: python src/mkscripts.py (build.cmd запускает сам, если найден python)."""
import io, os
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
s = io.open(os.path.join(root, 'scripts', 'check_update.py'), encoding='utf-8', newline='').read().replace('\r\n', '\n')
assert ')PY"' not in s
io.open(os.path.join(root, 'src', 'check_update.inc'), 'w', encoding='utf-8', newline='\n').write(
    '// Создано src/mkscripts.py из scripts/check_update.py - не править руками.\nstatic const wchar_t kEx3[] = LR"PY(' + s + ')PY";\n')
