# -*- coding: utf-8 -*-
"""DKeys: скачивание движка и моделей Whisper (сам DKeys.exe в сеть не ходит). Запускает DKeys:
    python fetch.py engine <папка whisper>      - движок whisper-cli (zip с GitHub, распаковывается в папку)
    python fetch.py model <имя> <файл>          - модель ggml-<имя>.bin с Hugging Face в указанный файл
Код выхода 0 - успех. Прогресс DKeys берёт из размера файла."""
import os
import re
import sys
import urllib.request
import zipfile

ENG = 'https://github.com/mrDukereD/Dkeys/releases/download/whisper-engine/whisper-engine-x64.zip'
MOD = 'https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-%s.bin'


def dl(url, dst):
    req = urllib.request.Request(url, headers={'User-Agent': 'DKeys'})
    with urllib.request.urlopen(req, timeout=60) as r, open(dst, 'wb') as f:
        while True:
            b = r.read(1 << 20)
            if not b:
                break
            f.write(b)


def main():
    a = sys.argv[1:]
    if len(a) == 2 and a[0] == 'engine':
        z = os.path.join(a[1], 'engine.zip')
        try:
            dl(ENG, z)
            with zipfile.ZipFile(z) as f:
                f.extractall(a[1])
        finally:
            if os.path.exists(z):
                os.remove(z)
        return 0
    if len(a) == 3 and a[0] == 'model' and re.fullmatch(r'[A-Za-z0-9._-]+', a[1]):
        dl(MOD % a[1], a[2])
        return 0
    return 2


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as e:
        sys.stderr.write('%s\n' % e)
        sys.exit(1)
