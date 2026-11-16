# Пример скрипта события календаря (OnReminder / OnCreate / OnChange)
"""
DKeys запускает его, когда сработало напоминание календаря. Подключение в DKeys.ini, секция [Calendar]:
    OnReminder=py onreminder.py
Так же можно подключить OnCreate (событие создано) и OnChange (событие изменено или перетащено).

Сведения о событии - в переменных окружения: DKEYS_EVENT_TITLE (название), DKEYS_EVENT_START и DKEYS_EVENT_END
(ГГГГ-ММ-ДД чч:мм по местному времени, у целодневного - только дата), DKEYS_EVENT_PLACE, DKEYS_EVENT_SOURCE (источник: local, google ...), DKEYS_EVENT_UID.

Этот пример только дописывает строку в reminders.log рядом со скриптом. Замените его своим: сообщение в мессенджер,
звук, запуск программы. Событие, которое скрипт сам добавил через  DKeys.exe do "calendar add ...", OnCreate не вызывает.
"""
import os
import time

env = os.environ.get
line = '\t'.join([time.strftime('%Y-%m-%d %H:%M:%S'), env('DKEYS_EVENT_START', ''), env('DKEYS_EVENT_TITLE', ''), env('DKEYS_EVENT_PLACE', ''), env('DKEYS_EVENT_SOURCE', '')])
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'reminders.log'), 'a', encoding='utf-8') as f:
    f.write(line + '\n')
