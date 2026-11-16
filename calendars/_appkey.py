# -*- coding: utf-8 -*-
"""Ключ приложения DKeys в Google Cloud (тип «Desktop app»). Для приложений ПК Google не считает client_secret секретом:
он нужен только чтобы Google узнал приложение, доступ к данным пользователя даёт его собственный вход (токены лежат у него на диске).
Если у пользователя свой ключ (google.state\client.json или DKEYS_GOOGLE_CLIENT_ID), он главнее этого."""
CLIENT_ID = '659988705238-ln3580io3broisu8u5km6lmpcnu91sb7.apps.googleusercontent.com'
CLIENT_SECRET = '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'
