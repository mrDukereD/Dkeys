# -*- coding: utf-8 -*-
"""Ключ приложения DKeys в Google Cloud (тип «Desktop app»). Для приложений ПК Google не считает client_secret секретом:
он нужен только чтобы Google узнал приложение, доступ к данным пользователя даёт его собственный вход (токены лежат у него на диске).
Если у пользователя свой ключ (google.state\client.json или DKEYS_GOOGLE_CLIENT_ID), он главнее этого."""
CLIENT_ID = ''
CLIENT_SECRET = ''
