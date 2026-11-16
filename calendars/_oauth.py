# -*- coding: utf-8 -*-
"""_oauth - общее для облачных провайдеров календаря (google.py, outlook.py; не источник, имя на «_»): HTTP с повторами, вход OAuth 2.0
(браузер на 127.0.0.1 + PKCE, для Microsoft ещё код устройства), токены, настройки профилей. Только стандартная библиотека.
Секреты (токены, ключи приложения) не печатаются; файлы с ними доступны только владельцу. В проверках --selftest вместо настоящего
HTTP подставляется функция HTTP, сеть не используется."""
import base64
import configparser
import hashlib
import json
import os
import re
import secrets
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.parse as up
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dkcal as D  # noqa: E402

BUDGET = 48.0                                # секунд на весь процесс (вызов провайдера ограничен 60 с)
T0 = time.time()
NEED_LOGIN = "нет входа в аккаунт или он устарел: нажмите «Подключить…» в календаре (setup) и войдите заново"


class NetError(Exception):
    pass


class AuthError(Exception):
    pass


class ApiError(Exception):
    def __init__(self, status, message="", data=None):
        Exception.__init__(self, ("HTTP %s: %s" % (status, message)).strip(": "))
        self.status, self.message, self.data = status, message, data


def api_message(data):
    """Текст ошибки из ответа Google / Microsoft Graph / OAuth."""
    if isinstance(data, dict):
        e = data.get("error")
        if isinstance(e, dict):
            return str(e.get("message") or e.get("code") or "")
        if e:
            return str(data.get("error_description") or e).splitlines()[0]
    return data if isinstance(data, str) else ""


# ---------- HTTP ----------
def http_real(method, url, headers, body, timeout):
    req = urllib.request.Request(url, data=body, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, {k.lower(): v for k, v in r.headers.items()}, r.read()
    except urllib.error.HTTPError as e:
        try:
            raw = e.read()
        except Exception:
            raw = b""
        return e.code, {k.lower(): v for k, v in e.headers.items()}, raw
    except (urllib.error.URLError, OSError) as e:
        raise NetError("нет связи с %s: %s" % (up.urlparse(url).netloc, getattr(e, "reason", None) or e))


HTTP = http_real                               # подменяется в --selftest: (method, url, headers, body_bytes, timeout) -> (status, headers, bytes)
SLEEP = time.sleep


def _decode(raw):
    if not raw:
        return None
    try:
        return json.loads(raw.decode("utf-8"))
    except Exception:
        return raw.decode("utf-8", "replace")[:300]


def _rate_limited(status, data):
    return status == 403 and "ratelimit" in json.dumps(data).lower().replace("_", "")


def call(method, url, params=None, form=None, body=None, headers=None, bearer=None, ok=(), tries=4):
    """Один запрос -> (status, данные JSON|текст|None, заголовки). Повторы с паузой: сеть, 429, 5xx, превышение квоты Google;
    статусы из ok возвращаются, остальные >=400 - ApiError."""
    if params:
        url += ("&" if "?" in url else "?") + up.urlencode(params, doseq=True)
    h = {"Accept": "application/json"}
    data = None
    if form is not None:
        data = up.urlencode(form).encode("utf-8")
        h["Content-Type"] = "application/x-www-form-urlencoded"
    elif body is not None:
        data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        h["Content-Type"] = "application/json; charset=utf-8"
    if bearer:
        h["Authorization"] = "Bearer " + bearer
    h.update(headers or {})
    for attempt in range(tries):
        left = BUDGET - (time.time() - T0)
        if attempt and left < 3:
            raise NetError("не хватило времени на повторы (60 с на вызов)")
        try:
            st, hd, raw = HTTP(method, url, h, data, max(5.0, min(25.0, left)))
        except NetError:
            if attempt == tries - 1:
                raise
            SLEEP(min(8, 1 << attempt))
            continue
        d = _decode(raw)
        if st < 400 or st in ok:
            return st, d, hd
        if (st in (429, 500, 502, 503, 504) or _rate_limited(st, d)) and attempt < tries - 1:
            try:
                wait = float(hd.get("retry-after", ""))
            except ValueError:
                wait = float(1 << attempt)
            wait = min(15.0, max(0.5, wait))
            if BUDGET - (time.time() - T0) > wait + 5:
                SLEEP(wait)
                continue
        raise ApiError(st, api_message(d), d)
    raise NetError("нет ответа")


# ---------- файлы с секретами ----------
def secure(path):
    """Доступ к файлу только владельцу (Windows: icacls, иначе chmod 600). Ошибки не критичны."""
    try:
        if os.name == "nt":
            user = os.environ.get("USERNAME") or ""
            if user:
                subprocess.run(["icacls", path, "/inheritance:r", "/grant:r", "%s:F" % user], capture_output=True, timeout=15)
        else:
            os.chmod(path, 0o600)
    except Exception:
        pass


def write_secret(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False)
    secure(tmp)
    os.replace(tmp, path)


# ---------- токены ----------
def _tok_from(data, old=None):
    tok = dict(old or {})
    tok["access_token"] = data["access_token"]
    tok["expires_at"] = time.time() + int(data.get("expires_in", 3600))
    if data.get("refresh_token"):
        tok["refresh_token"] = data["refresh_token"]
    return tok


def token_request(oa, form):
    f = {"client_id": oa["client_id"]}
    if oa.get("client_secret"):
        f["client_secret"] = oa["client_secret"]
    f.update(form)
    st, d, _ = call("POST", oa["token_url"], form=f, ok=(400, 401))
    return st, d


class Client(object):
    """Запросы с токеном доступа: берёт из файла, обновляет при истечении и при 401."""
    def __init__(self, oa, tokfile):
        self.oa, self.tokfile, self.tok = oa, tokfile, None

    def token(self, force=False):
        if self.tok is None:
            try:
                with open(self.tokfile, encoding="utf-8") as f:
                    self.tok = json.load(f)
            except (OSError, ValueError):
                raise AuthError(NEED_LOGIN)
        t = self.tok
        if not t.get("refresh_token"):
            raise AuthError(NEED_LOGIN)
        if force or not t.get("access_token") or t.get("expires_at", 0) < time.time() + 120:
            form = {"grant_type": "refresh_token", "refresh_token": t["refresh_token"]}
            if self.oa.get("scope_on_refresh"):
                form["scope"] = self.oa["scope"]
            st, d = token_request(self.oa, form)
            if st >= 400 or not isinstance(d, dict) or "access_token" not in d:
                err = d.get("error") if isinstance(d, dict) else ""
                raise AuthError("%s (%s)" % (NEED_LOGIN, err or st))
            self.tok = _tok_from(d, t)
            write_secret(self.tokfile, self.tok)
        return self.tok["access_token"]

    def req(self, method, url, params=None, body=None, headers=None, ok=()):
        for attempt in (0, 1):
            try:
                return call(method, url, params=params, body=body, headers=headers, bearer=self.token(force=attempt == 1), ok=ok)
            except ApiError as e:
                if e.status == 401 and attempt == 0:
                    continue
                if e.status == 401:
                    raise AuthError(NEED_LOGIN)
                raise


# ---------- вход: браузер + PKCE ----------
def _b64(b):
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode("ascii")


def open_browser(url):
    try:
        import webbrowser
        return webbrowser.open(url)
    except Exception:
        return False


OPEN = open_browser                            # подменяется в --selftest


def login_browser(oa, say=print, timeout=300):
    """Вход в браузере: страница на 127.0.0.1, код авторизации с PKCE. Возвращает токены {refresh_token, access_token, expires_at}."""
    verifier = _b64(secrets.token_bytes(48))
    state = secrets.token_urlsafe(16)
    box = {}

    class H(BaseHTTPRequestHandler):
        def do_GET(self):
            q = dict(up.parse_qsl(up.urlparse(self.path).query))
            if "code" in q or "error" in q:
                box.update(q)
                text = "Готово: можно закрыть эту вкладку и вернуться в окно DKeys." if "code" in q else "Вход не выполнен: %s" % q.get("error")
                code = 200
            else:
                text, code = "", 404
            raw = ("<!doctype html><meta charset=utf-8><title>DKeys</title><p>%s</p>" % text).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)

        def log_message(self, *a):
            pass

    srv = HTTPServer(("127.0.0.1", 0), H)
    srv.timeout = 1
    redirect = "http://%s:%d" % (oa.get("redirect_host", "127.0.0.1"), srv.server_address[1])
    p = {"client_id": oa["client_id"], "redirect_uri": redirect, "response_type": "code", "scope": oa["scope"], "state": state,
         "code_challenge": _b64(hashlib.sha256(verifier.encode("ascii")).digest()), "code_challenge_method": "S256"}
    p.update(oa.get("auth_params", {}))
    url = oa["auth_url"] + "?" + up.urlencode(p)
    say("Открываю браузер для входа. Если он не открылся, скопируйте адрес в браузер вручную:\n" + url)
    OPEN(url)
    end = time.time() + timeout
    try:
        while not box and time.time() < end:
            srv.handle_request()
    finally:
        srv.server_close()
    if not box:
        raise AuthError("вход не завершён за %d с" % timeout)
    if box.get("state") != state:
        raise AuthError("вход отклонён: неверный параметр state")
    if "code" not in box:
        raise AuthError("вход не выполнен: %s" % (box.get("error_description") or box.get("error")))
    form = {"grant_type": "authorization_code", "code": box["code"], "redirect_uri": redirect, "code_verifier": verifier}
    if oa.get("scope_on_refresh"):
        form["scope"] = oa["scope"]
    st, d = token_request(oa, form)
    if st >= 400 or not isinstance(d, dict) or "access_token" not in d:
        raise AuthError("не удалось получить токен: %s" % (api_message(d) or st))
    if not d.get("refresh_token"):
        raise AuthError("сервер не выдал refresh-токен (повторите вход; у Google в Консоли приложение должно быть «для ПК»)")
    return _tok_from(d)


def login_device(oa, say=print):
    """Вход по коду устройства (Microsoft): код вводится на странице входа на любом устройстве."""
    st, d, _ = call("POST", oa["device_url"], form={"client_id": oa["client_id"], "scope": oa["scope"]})
    say(d.get("message") or "Откройте %s и введите код %s" % (d.get("verification_uri"), d.get("user_code")))
    end = time.time() + int(d.get("expires_in", 900))
    wait = max(1, int(d.get("interval", 5)))
    while time.time() < end:
        SLEEP(wait)
        st, r = token_request(oa, {"grant_type": "urn:ietf:params:oauth:grant-type:device_code", "device_code": d["device_code"]})
        if isinstance(r, dict) and "access_token" in r:
            return _tok_from(r)
        err = r.get("error") if isinstance(r, dict) else ""
        if err == "authorization_pending":
            continue
        if err == "slow_down":
            wait += 5
            continue
        raise AuthError("вход по коду не выполнен: %s" % (api_message(r) or err or st))
    raise AuthError("код устройства истёк, начните вход заново")


# ---------- профили ----------
_TR = dict(zip("абвгдеёжзийклмнопрстуфхцчшщъыьэюя", ["a", "b", "v", "g", "d", "e", "e", "zh", "z", "i", "y", "k", "l", "m", "n", "o", "p", "r", "s", "t", "u", "f", "h", "c", "ch", "sh", "sch", "", "y", "", "e", "yu", "ya"]))


def slug(label, limit=15, default="cal"):
    """Имя профиля: латиница, цифры, _ и -; кириллица транслитерируется (ini читается Windows без Юникода)."""
    s = "".join(_TR.get(c, c) for c in (label or "").lower())
    s = re.sub(r"[^a-z0-9_-]+", "_", s).strip("_")
    return (s[:limit].strip("_")) or default


def unique(name, taken, limit=15):
    n, i = name, 2
    while n in taken:
        suf = "-%d" % i
        n, i = name[:limit - len(suf)] + suf, i + 1
    return n


def read_ini(path):
    cp = configparser.ConfigParser(interpolation=None)
    cp.read(path, encoding="utf-8")
    return cp


def write_ini(cp, path):
    with open(path, "w", encoding="utf-8") as f:
        cp.write(f)


def pick_profile(cp, profile):
    if profile:
        return profile if cp.has_section(profile) else ""
    if cp.has_section("default"):
        return "default"
    return cp.sections()[0] if len(cp.sections()) == 1 else ""


def ask(prompt, default=""):
    s = input(prompt + (" [%s]" % default if default else "") + ": ").strip()
    return s or default


def pick_many(items, label, say=print):
    """Нумерованный список -> выбранные номера через запятую ('1,3', 'все'); пусто - первый."""
    for i, it in enumerate(items, 1):
        say("  %d. %s" % (i, label(it)))
    s = ask("Какие календари синхронизировать (номера через запятую, «все»)", "1").lower()
    if s in ("все", "all", "*"):
        return list(items)
    idx = [int(x) for x in re.split(r"[ ,;]+", s) if x.isdigit() and 1 <= int(x) <= len(items)]
    return [items[i - 1] for i in dict.fromkeys(idx)] or [items[0]]


def ensure_tzdata(say=print):
    """Часовым поясам Python на Windows нужен пакет tzdata: проверить и при необходимости поставить."""
    try:
        from zoneinfo import ZoneInfo
        ZoneInfo("Europe/Minsk")
        return
    except Exception:
        pass
    say("Ставлю пакет tzdata (часовые пояса)...")
    rc = subprocess.call([sys.executable, "-m", "pip", "install", "--user", "tzdata"])
    if rc != 0:
        say("Не удалось поставить tzdata: выполните вручную: python -m pip install tzdata (без него события не в UTC не разобрать)")
