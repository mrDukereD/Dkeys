// calendar.h - ядро календаря DKeys: события, ICS, повторы (RRULE), часовые пояса. Без CRT и без интерфейса.
// Подключается из dkeys.cpp после util. От хозяина нужны: Alloc, Realloc, Free, Dup, DupN, FromUtf8, ToUtf8, ToInt, ReadAll, WriteAtomic, Buf, BufAdd.
// Проверка: tests\run.cmd (отдельная программа с CRT, те же заголовки).
#ifndef DK_CALENDAR_H
#define DK_CALENDAR_H

namespace Cal {

typedef long long I64;

/* ---------- даты: гражданский календарь <-> дни от 1970-01-01, "стенные" секунды ---------- */
static I64 FloorDiv(I64 a, I64 b) { I64 q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) q--; return q; }
static I64 DaysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    I64 era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static void CivilFromDays(I64 z, int *py, int *pm, int *pd) {
    z += 719468;
    I64 era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = (int)(z - era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = (int)(yoe + era * 400);
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    int d = doy - (153 * mp + 2) / 5 + 1;
    int m = mp < 10 ? mp + 3 : mp - 9;
    *py = y + (m <= 2); *pm = m; *pd = d;
}
static int Weekday(I64 days) { return (int)(((days + 3) % 7 + 7) % 7); }      // 0 = понедельник
static BOOL IsLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int DaysInMonth(int y, int m) { static const int k[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}; return m == 2 && IsLeap(y) ? 29 : k[m - 1]; }
struct Tm { int y, mo, d, h, mi, s; };
static I64 TmToSec(const Tm &t) { return DaysFromCivil(t.y, t.mo, t.d) * 86400LL + t.h * 3600 + t.mi * 60 + t.s; }
static Tm SecToTm(I64 s) {
    I64 days = FloorDiv(s, 86400); int r = (int)(s - days * 86400);
    Tm t; CivilFromDays(days, &t.y, &t.mo, &t.d); t.h = r / 3600; t.mi = r / 60 % 60; t.s = r % 60; return t;
}
static I64 NowUtc(void) { FILETIME ft; GetSystemTimeAsFileTime(&ft); return (I64)((((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000000ULL) - 11644473600LL; }

/* ---------- часовые пояса: IANA (TZID из ICS) -> имя Windows -> API системы ---------- */
// Таблица IANA -> имя Windows лежит в функции: пока ею не пользуются, в exe её нет.
struct ZMap { const char *iana, *win; };
static int ZoneMapGet(int i, const char **iana, const char **win) {
    static const ZMap k[] = {
        {"Europe/Moscow", "Russian Standard Time"}, {"Europe/Kaliningrad", "Kaliningrad Standard Time"}, {"Europe/Samara", "Russia Time Zone 3"},
        {"Europe/Volgograd", "Volgograd Standard Time"}, {"Europe/Saratov", "Saratov Standard Time"}, {"Europe/Astrakhan", "Astrakhan Standard Time"},
        {"Europe/Ulyanovsk", "Astrakhan Standard Time"}, {"Europe/Kirov", "Russian Standard Time"}, {"Europe/Simferopol", "Russian Standard Time"},
        {"Europe/Minsk", "Belarus Standard Time"}, {"Europe/Kyiv", "FLE Standard Time"}, {"Europe/Kiev", "FLE Standard Time"},
        {"Europe/Chisinau", "E. Europe Standard Time"}, {"Asia/Yekaterinburg", "Ekaterinburg Standard Time"}, {"Asia/Omsk", "Omsk Standard Time"},
        {"Asia/Novosibirsk", "N. Central Asia Standard Time"}, {"Asia/Barnaul", "Altai Standard Time"}, {"Asia/Tomsk", "Tomsk Standard Time"},
        {"Asia/Krasnoyarsk", "North Asia Standard Time"}, {"Asia/Novokuznetsk", "North Asia Standard Time"}, {"Asia/Irkutsk", "North Asia East Standard Time"},
        {"Asia/Yakutsk", "Yakutsk Standard Time"}, {"Asia/Chita", "Transbaikal Standard Time"}, {"Asia/Vladivostok", "Vladivostok Standard Time"},
        {"Asia/Magadan", "Magadan Standard Time"}, {"Asia/Sakhalin", "Sakhalin Standard Time"}, {"Asia/Srednekolymsk", "Russia Time Zone 10"},
        {"Asia/Kamchatka", "Russia Time Zone 11"}, {"Asia/Anadyr", "Russia Time Zone 11"}, {"Asia/Almaty", "Central Asia Standard Time"},
        {"Asia/Qyzylorda", "Qyzylorda Standard Time"}, {"Asia/Aqtau", "West Asia Standard Time"}, {"Asia/Aqtobe", "West Asia Standard Time"},
        {"Asia/Oral", "West Asia Standard Time"}, {"Asia/Tashkent", "West Asia Standard Time"}, {"Asia/Bishkek", "Central Asia Standard Time"},
        {"Asia/Tbilisi", "Georgian Standard Time"}, {"Asia/Yerevan", "Caucasus Standard Time"}, {"Asia/Baku", "Azerbaijan Standard Time"},
        {"Asia/Dubai", "Arabian Standard Time"}, {"Asia/Tehran", "Iran Standard Time"}, {"Asia/Kabul", "Afghanistan Standard Time"},
        {"Asia/Karachi", "Pakistan Standard Time"}, {"Asia/Kolkata", "India Standard Time"}, {"Asia/Calcutta", "India Standard Time"},
        {"Asia/Kathmandu", "Nepal Standard Time"}, {"Asia/Dhaka", "Bangladesh Standard Time"}, {"Asia/Bangkok", "SE Asia Standard Time"},
        {"Asia/Ho_Chi_Minh", "SE Asia Standard Time"}, {"Asia/Jakarta", "SE Asia Standard Time"}, {"Asia/Shanghai", "China Standard Time"},
        {"Asia/Hong_Kong", "China Standard Time"}, {"Asia/Singapore", "Singapore Standard Time"}, {"Asia/Kuala_Lumpur", "Singapore Standard Time"},
        {"Asia/Manila", "Singapore Standard Time"}, {"Asia/Taipei", "Taipei Standard Time"}, {"Asia/Seoul", "Korea Standard Time"},
        {"Asia/Tokyo", "Tokyo Standard Time"}, {"Asia/Jerusalem", "Israel Standard Time"}, {"Asia/Beirut", "Middle East Standard Time"},
        {"Asia/Riyadh", "Arab Standard Time"}, {"Asia/Baghdad", "Arabic Standard Time"}, {"Europe/Istanbul", "Turkey Standard Time"},
        {"Asia/Istanbul", "Turkey Standard Time"}, {"Europe/London", "GMT Standard Time"}, {"Europe/Dublin", "GMT Standard Time"},
        {"Europe/Lisbon", "GMT Standard Time"}, {"Atlantic/Reykjavik", "Greenwich Standard Time"}, {"Europe/Berlin", "W. Europe Standard Time"},
        {"Europe/Amsterdam", "W. Europe Standard Time"}, {"Europe/Rome", "W. Europe Standard Time"}, {"Europe/Vienna", "W. Europe Standard Time"},
        {"Europe/Zurich", "W. Europe Standard Time"}, {"Europe/Stockholm", "W. Europe Standard Time"}, {"Europe/Oslo", "W. Europe Standard Time"},
        {"Europe/Paris", "Romance Standard Time"}, {"Europe/Madrid", "Romance Standard Time"}, {"Europe/Brussels", "Romance Standard Time"},
        {"Europe/Copenhagen", "Romance Standard Time"}, {"Europe/Warsaw", "Central European Standard Time"}, {"Europe/Belgrade", "Central Europe Standard Time"},
        {"Europe/Prague", "Central Europe Standard Time"}, {"Europe/Budapest", "Central Europe Standard Time"}, {"Europe/Athens", "GTB Standard Time"},
        {"Europe/Bucharest", "GTB Standard Time"}, {"Europe/Helsinki", "FLE Standard Time"}, {"Europe/Riga", "FLE Standard Time"},
        {"Europe/Tallinn", "FLE Standard Time"}, {"Europe/Vilnius", "FLE Standard Time"}, {"Europe/Sofia", "FLE Standard Time"},
        {"Africa/Cairo", "Egypt Standard Time"}, {"Africa/Johannesburg", "South Africa Standard Time"}, {"Africa/Lagos", "W. Central Africa Standard Time"},
        {"Africa/Nairobi", "E. Africa Standard Time"}, {"Africa/Casablanca", "Morocco Standard Time"}, {"America/New_York", "Eastern Standard Time"},
        {"America/Toronto", "Eastern Standard Time"}, {"America/Chicago", "Central Standard Time"}, {"America/Denver", "Mountain Standard Time"},
        {"America/Phoenix", "US Mountain Standard Time"}, {"America/Los_Angeles", "Pacific Standard Time"}, {"America/Vancouver", "Pacific Standard Time"},
        {"America/Anchorage", "Alaskan Standard Time"}, {"Pacific/Honolulu", "Hawaiian Standard Time"}, {"America/Halifax", "Atlantic Standard Time"},
        {"America/Mexico_City", "Central Standard Time (Mexico)"}, {"America/Sao_Paulo", "E. South America Standard Time"},
        {"America/Argentina/Buenos_Aires", "Argentina Standard Time"}, {"America/Bogota", "SA Pacific Standard Time"}, {"America/Lima", "SA Pacific Standard Time"},
        {"America/Santiago", "Pacific SA Standard Time"}, {"Australia/Sydney", "AUS Eastern Standard Time"}, {"Australia/Melbourne", "AUS Eastern Standard Time"},
        {"Australia/Brisbane", "E. Australia Standard Time"}, {"Australia/Perth", "W. Australia Standard Time"}, {"Australia/Adelaide", "Cen. Australia Standard Time"},
        {"Pacific/Auckland", "New Zealand Standard Time"},
};
    if (i < 0 || i >= (int)(sizeof k / sizeof k[0])) return 0;
    *iana = k[i].iana; *win = k[i].win; return 1;
}

static BOOL EqNI(const wchar_t *a, const wchar_t *b, int n) {
    for (int i = 0; i < n; i++) {
        wchar_t x = a[i], y = b[i];
        if (x >= L'a' && x <= L'z') x -= 32;
        if (y >= L'a' && y <= L'z') y -= 32;
        if (x != y || !x) return FALSE;
    }
    return TRUE;
}
static BOOL EqI(const wchar_t *a, const wchar_t *b) { int n = lstrlenW(b); return lstrlenW(a) == n && EqNI(a, b, n); }
static BOOL EqIA(const wchar_t *a, const char *b) { for (; *a && *b; a++, b++) { wchar_t x = *a, y = (wchar_t)(BYTE)*b; if (x >= L'a' && x <= L'z') x -= 32; if (y >= L'a' && y <= L'z') y -= 32; if (x != y) return FALSE; } return !*a && !*b; }
static void Widen(wchar_t *o, const char *s, int cap) { int n = 0; while (*s && n < cap - 1) o[n++] = (wchar_t)(BYTE)*s++; o[n] = 0; }
static BOOL IsUtcName(const wchar_t *z) { return EqI(z, L"UTC") || EqI(z, L"Z") || EqI(z, L"Etc/UTC") || EqI(z, L"GMT") || EqI(z, L"Etc/GMT") || EqI(z, L"UTC+00:00"); }
struct ZoneEnt { wchar_t key[80]; DYNAMIC_TIME_ZONE_INFORMATION d; BOOL utc; };
static ZoneEnt g_zc[24]; static int g_nzc, g_zcNext;
static ZoneEnt *ZoneGet(const wchar_t *tzid) {                 // "" - местный пояс; неизвестный TZID тоже даёт местный
    if (!tzid) tzid = L"";
    for (int i = 0; i < g_nzc; i++) if (EqI(g_zc[i].key, tzid)) return &g_zc[i];
    int slot = g_nzc < 24 ? g_nzc++ : (g_zcNext++ % 24);
    ZoneEnt *z = &g_zc[slot]; memset(z, 0, sizeof *z); lstrcpynW(z->key, tzid, 80);
    if (IsUtcName(tzid)) { z->utc = TRUE; return z; }
    BOOL found = FALSE;
    if (*tzid) {                                               // правила пояса система берёт из реестра по имени ключа (TimeZoneKeyName)
        DYNAMIC_TIME_ZONE_INFORMATION d; memset(&d, 0, sizeof d); lstrcpynW(d.TimeZoneKeyName, tzid, 128);      // имя Windows тоже подходит как есть
        const char *ia, *wn;
        for (int i = 0; ZoneMapGet(i, &ia, &wn); i++) if (EqIA(tzid, ia)) { Widen(d.TimeZoneKeyName, wn, 128); break; }
        TIME_ZONE_INFORMATION t;
        if (GetTimeZoneInformationForYear(2026, &d, &t) && t.StandardName[0]) { z->d = d; found = TRUE; }
    }
    if (!found) GetDynamicTimeZoneInformation(&z->d);
    return z;
}
static SYSTEMTIME TmToSt(const Tm &t) { SYSTEMTIME s; memset(&s, 0, sizeof s); s.wYear = (WORD)t.y; s.wMonth = (WORD)t.mo; s.wDay = (WORD)t.d; s.wHour = (WORD)t.h; s.wMinute = (WORD)t.mi; s.wSecond = (WORD)t.s; return s; }
static Tm StToTm(const SYSTEMTIME &s) { Tm t; t.y = s.wYear; t.mo = s.wMonth; t.d = s.wDay; t.h = s.wHour; t.mi = s.wMinute; t.s = s.wSecond; return t; }
static I64 WallToUtc(I64 wall, const wchar_t *tzid) {          // стенное время в поясе tzid -> секунды UTC
    ZoneEnt *z = ZoneGet(tzid); if (z->utc) return wall;
    Tm tm = SecToTm(wall); TIME_ZONE_INFORMATION t; SYSTEMTIME lt = TmToSt(tm), st;
    if (!GetTimeZoneInformationForYear((USHORT)tm.y, &z->d, &t) || !TzSpecificLocalTimeToSystemTime(&t, &lt, &st)) return wall;
    return TmToSec(StToTm(st));
}
static I64 UtcToWall(I64 utc, const wchar_t *tzid) {
    ZoneEnt *z = ZoneGet(tzid); if (z->utc) return utc;
    Tm tm = SecToTm(utc); TIME_ZONE_INFORMATION t; SYSTEMTIME st = TmToSt(tm), lt;
    if (!GetTimeZoneInformationForYear((USHORT)tm.y, &z->d, &t) || !SystemTimeToTzSpecificLocalTime(&t, &st, &lt)) return utc;
    return TmToSec(StToTm(lt));
}
static void LocalTzid(wchar_t *out, int cap) {                 // IANA-имя местного пояса для новых событий ("" - не нашли, событие будет плавающим)
    out[0] = 0; DYNAMIC_TIME_ZONE_INFORMATION d; memset(&d, 0, sizeof d); GetDynamicTimeZoneInformation(&d);
    const char *ia, *wn;
    for (int i = 0; ZoneMapGet(i, &ia, &wn); i++) if (EqIA(d.TimeZoneKeyName, wn)) { Widen(out, ia, cap); return; }
}

/* ---------- события ---------- */
struct Event {
    wchar_t *uid, *summary, *desc, *loc, *tzid, *rrule, *source, *etag, *extra;
    I64 start, end;      // стенные секунды в поясе tzid ("как UTC"); целый день - полночь даты, end не включается
    I64 recId;           // для замены одного вхождения серии (hasRec): стенное время заменяемого вхождения
    I64 stamp;           // LAST-MODIFIED, UTC
    I64 *exd; int nexd;  // исключённые вхождения (стенное время начала)
    int *alarms; int nal;// напоминания: за сколько минут до начала
    int seq;
    int synced;          // seq на момент последней синхронизации с источником: seq != synced - есть несинхронизированная правка
    BOOL allDay, hasRec, cancelled, hidden, deleted;   // hidden - скрыто только в DKeys; deleted - удалено, ждёт отправки в источник
};
struct Store { Event *ev; int n, cap; };

static void SetStr(wchar_t **p, const wchar_t *s) { Free(*p); *p = s ? Dup(s) : NULL; }
static void EventFree(Event *e) {
    Free(e->uid); Free(e->summary); Free(e->desc); Free(e->loc); Free(e->tzid); Free(e->rrule); Free(e->source); Free(e->etag); Free(e->extra); Free(e->exd); Free(e->alarms);
    memset(e, 0, sizeof *e);
}
static void StoreFree(Store *s) { for (int i = 0; i < s->n; i++) EventFree(&s->ev[i]); Free(s->ev); memset(s, 0, sizeof *s); }
static Event *StoreNew(Store *s) {                              // указатель действует до следующего добавления
    if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 16; s->ev = (Event *)Realloc(s->ev, s->cap * sizeof(Event)); }
    Event *e = &s->ev[s->n++]; memset(e, 0, sizeof *e); return e;
}
static void StoreRemove(Store *s, int i) { EventFree(&s->ev[i]); memmove(s->ev + i, s->ev + i + 1, (s->n - i - 1) * sizeof(Event)); s->n--; memset(&s->ev[s->n], 0, sizeof(Event)); }
static int StoreFind(const Store *s, const wchar_t *uid, BOOL hasRec, I64 recId) {
    for (int i = 0; i < s->n; i++) { const Event *e = &s->ev[i]; if (e->uid && !lstrcmpW(e->uid, uid) && e->hasRec == hasRec && (!hasRec || e->recId == recId)) return i; }
    return -1;
}
static void NewUid(wchar_t *out, int cap) {
    GUID g; CoCreateGuid(&g);
    wchar_t t[80]; wsprintfW(t, L"%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x@dkeys", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    lstrcpynW(out, t, cap);
}
static void Touch(Event *e) { e->stamp = NowUtc(); e->seq++; }      // изменили событие: время правки и версия
static void AddExtra(Event *e, const wchar_t *line, int n) {         // нераспознанная строка ICS сохраняется как есть
    int old = e->extra ? lstrlenW(e->extra) : 0;
    e->extra = (wchar_t *)Realloc(e->extra, (old + n + 2) * sizeof(wchar_t));
    memcpy(e->extra + old, line, n * sizeof(wchar_t)); e->extra[old + n] = L'\n'; e->extra[old + n + 1] = 0;
}
static void AddAlarm(Event *e, int minutes) {
    for (int i = 0; i < e->nal; i++) if (e->alarms[i] == minutes) return;
    e->alarms = (int *)Realloc(e->alarms, (e->nal + 1) * sizeof(int)); e->alarms[e->nal++] = minutes;
}
static void AddExd(Event *e, I64 wall) { e->exd = (I64 *)Realloc(e->exd, (e->nexd + 1) * sizeof(I64)); e->exd[e->nexd++] = wall; }

/* ---------- разбор ICS ---------- */
struct Line { const wchar_t *raw; int nameLen, parLen; const wchar_t *val; };
static BOOL SplitLine(const wchar_t *s, Line *L) {
    L->raw = s; int i = 0; while (s[i] && s[i] != L';' && s[i] != L':') i++;
    L->nameLen = i; BOOL q = FALSE; int j = i;
    while (s[j]) { if (s[j] == L'"') q = !q; else if (s[j] == L':' && !q) break; j++; }
    if (!s[j]) return FALSE;
    L->parLen = j - i; L->val = s + j + 1; return TRUE;
}
static BOOL NameIs(const Line &L, const wchar_t *n) { return L.nameLen == lstrlenW(n) && EqNI(L.raw, n, L.nameLen); }
static BOOL Param(const Line &L, const wchar_t *key, wchar_t *out, int cap) {
    const wchar_t *p = L.raw + L.nameLen, *end = p + L.parLen; int kl = lstrlenW(key);
    while (p < end) {
        if (*p == L';') p++;
        const wchar_t *s = p; BOOL q = FALSE;
        while (p < end && (q || *p != L';')) { if (*p == L'"') q = !q; p++; }
        if (p - s > kl && s[kl] == L'=' && EqNI(s, key, kl)) {
            const wchar_t *v = s + kl + 1; int n = (int)(p - v);
            if (n >= 2 && v[0] == L'"' && v[n - 1] == L'"') { v++; n -= 2; }
            if (n > cap - 1) n = cap - 1;
            memcpy(out, v, n * sizeof(wchar_t)); out[n] = 0; return TRUE;
        }
    }
    return FALSE;
}
static wchar_t *Unesc(const wchar_t *s) {
    wchar_t *o = (wchar_t *)Alloc((lstrlenW(s) + 1) * sizeof(wchar_t)); int n = 0;
    for (; *s; s++) {
        if (*s == L'\\' && s[1]) { s++; o[n++] = (*s == L'n' || *s == L'N') ? L'\n' : *s; }
        else if (*s != L'\r') o[n++] = *s;
    }
    o[n] = 0; return o;
}
static int Digits(const wchar_t *s, int n) { int v = 0; for (int i = 0; i < n; i++) { if (s[i] < L'0' || s[i] > L'9') return -1; v = v * 10 + (s[i] - L'0'); } return v; }
static BOOL ParseDt(const wchar_t *v, I64 *wall, BOOL *isDate, BOOL *isUtc) {      // 20261116 | 20261116T100000 | ...Z
    Tm t; memset(&t, 0, sizeof t); *isDate = *isUtc = FALSE;
    int y = Digits(v, 4), m = Digits(v + 4, 2), d = Digits(v + 6, 2);
    if (y < 0 || m < 1 || m > 12 || d < 1 || d > 31) return FALSE;
    t.y = y; t.mo = m; t.d = d;
    if (v[8] != L'T' && v[8] != L't') { *isDate = TRUE; *wall = TmToSec(t); return TRUE; }
    int h = Digits(v + 9, 2), mi = Digits(v + 11, 2), s = 0, len = 13;
    if (h < 0 || mi < 0) return FALSE;
    if (v[13] >= L'0' && v[13] <= L'9') { s = Digits(v + 13, 2); if (s < 0) return FALSE; len = 15; }
    t.h = h; t.mi = mi; t.s = s; *isUtc = v[len] == L'Z' || v[len] == L'z';
    *wall = TmToSec(t); return TRUE;
}
static BOOL ParseDur(const wchar_t *v, I64 *out) {                                  // [-]P1DT2H3M4S, P1W
    int sg = 1; if (*v == L'-') { sg = -1; v++; } else if (*v == L'+') v++;
    if (*v != L'P' && *v != L'p') return FALSE;
    v++; I64 t = 0; BOOL inT = FALSE;
    while (*v) {
        if (*v == L'T' || *v == L't') { inT = TRUE; v++; continue; }
        if (*v < L'0' || *v > L'9') return FALSE;
        I64 n = 0; while (*v >= L'0' && *v <= L'9') n = n * 10 + (*v++ - L'0');
        wchar_t u = *v++; if (u >= L'a') u -= 32;
        if (u == L'W') t += n * 604800; else if (u == L'D') t += n * 86400; else if (u == L'H') t += n * 3600;
        else if (u == L'M' && inT) t += n * 60; else if (u == L'S') t += n; else return FALSE;
    }
    *out = sg * t; return TRUE;
}
static void ParseExdates(Event *e, const Line &L) {
    wchar_t tz[80]; BOOL hasTz = Param(L, L"TZID", tz, 80);
    const wchar_t *p = L.val;
    while (*p) {
        const wchar_t *q = p; while (*q && *q != L',') q++;
        wchar_t one[40]; int n = (int)(q - p); if (n > 39) n = 39; memcpy(one, p, n * sizeof(wchar_t)); one[n] = 0;
        I64 w; BOOL isDate, isUtc;
        if (ParseDt(one, &w, &isDate, &isUtc)) {
            if (isUtc) w = UtcToWall(w, e->tzid ? e->tzid : L"");
            else if (hasTz && !isDate && e->tzid && !EqI(tz, e->tzid)) w = UtcToWall(WallToUtc(w, tz), e->tzid);
            AddExd(e, w);
        }
        p = *q ? q + 1 : q;
    }
}
static const wchar_t *EvZone(const Event *e) { return e->allDay ? L"" : (e->tzid ? e->tzid : L""); }

// Добавляет в хранилище все VEVENT из текста ICS; source - имя источника (NULL - не задавать).
static BOOL Parse(Store *st, const wchar_t *text, const wchar_t *source) {
    int n = lstrlenW(text), nl = 2; for (int i = 0; i < n; i++) if (text[i] == L'\n') nl++;
    wchar_t *w = (wchar_t *)Alloc((n + 2) * sizeof(wchar_t)); const wchar_t **lines = (const wchar_t **)Alloc(nl * sizeof(wchar_t *)); int nlines = 0, o = 0;
    lines[nlines++] = w;                                                                        // разворачиваем сложенные строки
    for (int i = 0; i < n; i++) {
        if (text[i] == L'\r') continue;
        if (text[i] == L'\n') {
            if (text[i + 1] == L' ' || text[i + 1] == L'\t') { i++; continue; }
            if (text[i + 1] == L'\r' && text[i + 2] == L'\n' && (text[i + 3] == L' ' || text[i + 3] == L'\t')) { i += 3; continue; }
            w[o++] = 0; lines[nlines++] = w + o; continue;
        }
        w[o++] = text[i];
    }
    w[o] = 0;
    int cur = -1, skip = 0, rawDepth = 0; BOOL inAlarm = FALSE, alarmOther = FALSE; int alarmMin = 0; BOOL alarmHas = FALSE;
    Buf ab = {0};
    I64 endWall = 0, durSecs = 0; BOOL haveEnd = FALSE, endDate = FALSE, endUtc = FALSE, haveDur = FALSE; wchar_t endTz[80]; endTz[0] = 0;
    int got = 0;
    for (int li = 0; li < nlines; li++) {
        const wchar_t *s = lines[li]; if (!*s) continue;
        Line L; if (!SplitLine(s, &L)) continue;
        BOOL isBegin = NameIs(L, L"BEGIN"), isEnd = NameIs(L, L"END");
        if (skip) { if (isBegin) skip++; else if (isEnd) skip--; continue; }
        if (cur < 0) {
            if (isBegin) { if (EqI(L.val, L"VEVENT")) {
                    Event *e = StoreNew(st); cur = (int)(e - st->ev); if (source) e->source = Dup(source);
                    haveEnd = endDate = endUtc = haveDur = FALSE; endTz[0] = 0; endWall = durSecs = 0;
                } else if (!EqI(L.val, L"VCALENDAR")) skip = 1; }
            continue;
        }
        Event *e = &st->ev[cur];
        if (rawDepth) { AddExtra(e, s, lstrlenW(s)); if (isBegin) rawDepth++; else if (isEnd) rawDepth--; continue; }
        if (inAlarm) {
            if (isEnd && EqI(L.val, L"VALARM")) {
                if (alarmHas && !alarmOther) AddAlarm(e, alarmMin);
                else if (ab.n) { AddExtra(e, L"BEGIN:VALARM", 12); for (const wchar_t *p = ab.p; *p; ) { const wchar_t *q = p; while (*q && *q != L'\n') q++; AddExtra(e, p, (int)(q - p)); p = *q ? q + 1 : q; } AddExtra(e, L"END:VALARM", 10); }
                inAlarm = FALSE; ab.n = 0; if (ab.p) ab.p[0] = 0; continue;
            }
            if (NameIs(L, L"TRIGGER")) {
                wchar_t rel[16], val[16]; I64 d;
                BOOL relEnd = Param(L, L"RELATED", rel, 16) && EqI(rel, L"END"), abs = Param(L, L"VALUE", val, 16) && EqI(val, L"DATE-TIME");
                if (!relEnd && !abs && ParseDur(L.val, &d) && d <= 0 && (-d) % 60 == 0) { alarmMin = (int)(-d / 60); alarmHas = TRUE; } else alarmOther = TRUE;
            } else if (NameIs(L, L"ACTION")) { if (!EqI(L.val, L"DISPLAY") && !EqI(L.val, L"AUDIO")) alarmOther = TRUE; }
            else if (!NameIs(L, L"DESCRIPTION") && !NameIs(L, L"SUMMARY")) { /* прочие свойства напоминания теряются, если оно разобрано */ }
            BufAdd(&ab, s); BufAdd(&ab, L"\n"); continue;
        }
        if (isBegin) {
            if (EqI(L.val, L"VALARM")) { inAlarm = TRUE; alarmOther = alarmHas = FALSE; alarmMin = 0; ab.n = 0; if (ab.p) ab.p[0] = 0; }
            else { AddExtra(e, s, lstrlenW(s)); rawDepth = 1; }
            continue;
        }
        if (isEnd) {
            if (EqI(L.val, L"VEVENT")) {                                                        // событие закончено: достраиваем конец
                if (haveEnd) {
                    I64 ew = endWall;
                    if (!e->allDay && !endDate && endTz[0] && !EqI(endTz, EvZone(e))) ew = UtcToWall(WallToUtc(endWall, endTz), EvZone(e));
                    e->end = ew;
                } else if (haveDur) e->end = e->start + durSecs;
                else e->end = e->allDay ? e->start + 86400 : e->start;
                if (e->end < e->start) e->end = e->start;
                if (!e->uid) { wchar_t u[80]; NewUid(u, 80); e->uid = Dup(u); }
                if (!e->tzid && !e->allDay) e->tzid = Dup(L"");
                cur = -1; got++;
            }
            continue;
        }
        if (NameIs(L, L"UID")) { Free(e->uid); e->uid = Unesc(L.val); }
        else if (NameIs(L, L"SUMMARY")) { Free(e->summary); e->summary = Unesc(L.val); }
        else if (NameIs(L, L"DESCRIPTION")) { Free(e->desc); e->desc = Unesc(L.val); }
        else if (NameIs(L, L"LOCATION")) { Free(e->loc); e->loc = Unesc(L.val); }
        else if (NameIs(L, L"DTSTART") || NameIs(L, L"DTEND")) {
            BOOL isStart = NameIs(L, L"DTSTART"); I64 wall; BOOL isDate, isUtc; wchar_t tz[80]; tz[0] = 0; wchar_t vt[16];
            if (!ParseDt(L.val, &wall, &isDate, &isUtc)) { AddExtra(e, s, lstrlenW(s)); continue; }
            if (Param(L, L"VALUE", vt, 16) && EqI(vt, L"DATE")) isDate = TRUE;
            if (isUtc) lstrcpyW(tz, L"UTC"); else Param(L, L"TZID", tz, 80);
            if (isStart) { e->start = wall; e->allDay = isDate; Free(e->tzid); e->tzid = isDate ? NULL : Dup(tz); }
            else { endWall = wall; haveEnd = TRUE; endDate = isDate; endUtc = isUtc; lstrcpyW(endTz, tz); }
        }
        else if (NameIs(L, L"DURATION")) { if (ParseDur(L.val, &durSecs)) haveDur = TRUE; else AddExtra(e, s, lstrlenW(s)); }
        else if (NameIs(L, L"RRULE")) { if (!e->rrule) e->rrule = Dup(L.val); else AddExtra(e, s, lstrlenW(s)); }
        else if (NameIs(L, L"EXDATE")) ParseExdates(e, L);
        else if (NameIs(L, L"RECURRENCE-ID")) { I64 wall; BOOL d, u; if (ParseDt(L.val, &wall, &d, &u)) {
                wchar_t tz[80]; if (Param(L, L"TZID", tz, 80) && e->tzid && !EqI(tz, e->tzid)) wall = UtcToWall(WallToUtc(wall, tz), e->tzid);
                if (u) wall = UtcToWall(wall, e->tzid ? e->tzid : L"");
                e->recId = wall; e->hasRec = TRUE; } else AddExtra(e, s, lstrlenW(s)); }
        else if (NameIs(L, L"LAST-MODIFIED")) { I64 wall; BOOL d, u; if (ParseDt(L.val, &wall, &d, &u)) e->stamp = wall; }
        else if (NameIs(L, L"SEQUENCE")) e->seq = ToInt(L.val);
        else if (NameIs(L, L"STATUS")) { if (EqI(L.val, L"CANCELLED")) e->cancelled = TRUE; else AddExtra(e, s, lstrlenW(s)); }
        else if (NameIs(L, L"DTSTAMP")) { if (!e->stamp) { I64 wall; BOOL d, u; if (ParseDt(L.val, &wall, &d, &u)) e->stamp = wall; } }
        else if (NameIs(L, L"X-DKEYS-HIDDEN")) e->hidden = ToInt(L.val) != 0;
        else if (NameIs(L, L"X-DKEYS-DELETED")) e->deleted = ToInt(L.val) != 0;
        else if (NameIs(L, L"X-DKEYS-SYNCED")) e->synced = ToInt(L.val);
        else if (NameIs(L, L"X-DKEYS-ETAG")) { Free(e->etag); e->etag = Unesc(L.val); }
        else AddExtra(e, s, lstrlenW(s));
    }
    if (cur >= 0) StoreRemove(st, cur);                                                         // оборванное событие в конце файла
    Free(ab.p); Free(lines); Free(w);
    return got > 0;
}

/* ---------- запись ICS ---------- */
static void FoldAdd(Buf *b, const wchar_t *s) {                 // строка не длиннее 75 байт UTF-8, продолжение с пробела
    int used = 0;
    for (; *s; s++) {
        int len = 1, cb = *s < 0x80 ? 1 : *s < 0x800 ? 2 : 3;
        if (*s >= 0xD800 && *s < 0xDC00 && s[1] >= 0xDC00 && s[1] < 0xE000) { len = 2; cb = 4; }
        if (used + cb > 75) { BufAdd(b, L"\r\n "); used = 1; }
        BufAdd(b, s, len); used += cb; s += len - 1;
    }
    BufAdd(b, L"\r\n");
}
static void AddProp(Buf *out, const wchar_t *name, const wchar_t *val, BOOL escape) {
    if (!val || !*val) return;
    Buf t = {0}; BufAdd(&t, name); BufAdd(&t, L":");
    if (!escape) BufAdd(&t, val);
    else for (const wchar_t *p = val; *p; p++) {
        if (*p == L'\r') continue;
        if (*p == L'\n') BufAdd(&t, L"\\n");
        else { if (*p == L'\\' || *p == L';' || *p == L',') BufAdd(&t, L"\\"); BufAdd(&t, p, 1); }
    }
    FoldAdd(out, t.p); Free(t.p);
}
static void FmtDt(wchar_t *o, I64 wall, BOOL dateOnly, BOOL utc) {
    Tm t = SecToTm(wall);
    if (dateOnly) wsprintfW(o, L"%04d%02d%02d", t.y, t.mo, t.d);
    else wsprintfW(o, L"%04d%02d%02dT%02d%02d%02d%s", t.y, t.mo, t.d, t.h, t.mi, t.s, utc ? L"Z" : L"");
}
static void AddDt(Buf *out, const wchar_t *name, I64 wall, const Event *e) {
    wchar_t v[40], ln[160]; BOOL utc = !e->allDay && e->tzid && IsUtcName(e->tzid);
    FmtDt(v, wall, e->allDay, utc);
    if (e->allDay) wsprintfW(ln, L"%s;VALUE=DATE:%s", name, v);
    else if (e->tzid && *e->tzid && !utc) wsprintfW(ln, L"%s;TZID=%s:%s", name, e->tzid, v);
    else wsprintfW(ln, L"%s:%s", name, v);
    FoldAdd(out, ln);
}
static void AddEvent(Buf *out, const Event *e) {
    wchar_t v[48], ln[200];
    BufAdd(out, L"BEGIN:VEVENT\r\n");
    AddProp(out, L"UID", e->uid, TRUE);
    FmtDt(v, e->stamp ? e->stamp : NowUtc(), FALSE, TRUE); wsprintfW(ln, L"DTSTAMP:%s", v); FoldAdd(out, ln);
    if (e->stamp) { wsprintfW(ln, L"LAST-MODIFIED:%s", v); FoldAdd(out, ln); }
    if (e->seq > 0) { wsprintfW(ln, L"SEQUENCE:%d", e->seq); FoldAdd(out, ln); }
    AddDt(out, L"DTSTART", e->start, e);
    if (e->end != e->start || e->allDay) AddDt(out, L"DTEND", e->end, e);
    AddProp(out, L"SUMMARY", e->summary, TRUE); AddProp(out, L"DESCRIPTION", e->desc, TRUE); AddProp(out, L"LOCATION", e->loc, TRUE);
    AddProp(out, L"RRULE", e->rrule, FALSE);
    for (int i = 0; i < e->nexd; i++) {
        wchar_t d[40], l2[160]; BOOL utc = !e->allDay && e->tzid && IsUtcName(e->tzid);
        FmtDt(d, e->exd[i], e->allDay, utc);
        if (e->allDay) wsprintfW(l2, L"EXDATE;VALUE=DATE:%s", d);
        else if (e->tzid && *e->tzid && !utc) wsprintfW(l2, L"EXDATE;TZID=%s:%s", e->tzid, d);
        else wsprintfW(l2, L"EXDATE:%s", d);
        FoldAdd(out, l2);
    }
    if (e->hasRec) {
        wchar_t d[40], l2[160]; BOOL utc = !e->allDay && e->tzid && IsUtcName(e->tzid);
        FmtDt(d, e->recId, e->allDay, utc);
        if (e->allDay) wsprintfW(l2, L"RECURRENCE-ID;VALUE=DATE:%s", d);
        else if (e->tzid && *e->tzid && !utc) wsprintfW(l2, L"RECURRENCE-ID;TZID=%s:%s", e->tzid, d);
        else wsprintfW(l2, L"RECURRENCE-ID:%s", d);
        FoldAdd(out, l2);
    }
    if (e->cancelled) BufAdd(out, L"STATUS:CANCELLED\r\n");
    if (e->hidden) BufAdd(out, L"X-DKEYS-HIDDEN:1\r\n");
    if (e->deleted) BufAdd(out, L"X-DKEYS-DELETED:1\r\n");
    AddProp(out, L"X-DKEYS-ETAG", e->etag, TRUE);
    if (e->synced) { wsprintfW(ln, L"X-DKEYS-SYNCED:%d", e->synced); FoldAdd(out, ln); }
    for (int i = 0; i < e->nal; i++) {
        int m = e->alarms[i];
        if (m == 0) wsprintfW(ln, L"TRIGGER:PT0S"); else if (m % 1440 == 0) wsprintfW(ln, L"TRIGGER:-P%dD", m / 1440); else if (m % 60 == 0) wsprintfW(ln, L"TRIGGER:-PT%dH", m / 60); else wsprintfW(ln, L"TRIGGER:-PT%dM", m);
        BufAdd(out, L"BEGIN:VALARM\r\nACTION:DISPLAY\r\n"); AddProp(out, L"DESCRIPTION", e->summary && *e->summary ? e->summary : L"DKeys", TRUE);
        FoldAdd(out, ln); BufAdd(out, L"END:VALARM\r\n");
    }
    if (e->extra) for (const wchar_t *p = e->extra; *p; ) {
        const wchar_t *q = p; while (*q && *q != L'\n') q++;
        wchar_t *one = DupN(p, (int)(q - p)); FoldAdd(out, one); Free(one); p = *q ? q + 1 : q;
    }
    BufAdd(out, L"END:VEVENT\r\n");
}
// ICS-текст (CRLF) со всеми событиями источника source (NULL - всех). Освобождать Free.
static wchar_t *Serialize(const Store *st, const wchar_t *source) {
    Buf b = {0};
    BufAdd(&b, L"BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//DKeys//Calendar//EN\r\nCALSCALE:GREGORIAN\r\n");
    for (int i = 0; i < st->n; i++) {
        const Event *e = &st->ev[i];
        if (source && (!e->source || lstrcmpW(e->source, source))) continue;
        AddEvent(&b, e);
    }
    BufAdd(&b, L"END:VCALENDAR\r\n");
    return b.p;
}
static BOOL LoadFile(Store *st, const wchar_t *path, const wchar_t *source) {
    DWORD sz = 0; BYTE *raw = ReadAll(path, &sz); if (!raw) return FALSE;
    wchar_t *t = FromUtf8((char *)raw, (int)sz); Free(raw);
    BOOL ok = Parse(st, t, source); Free(t); return ok;
}
static BOOL SaveFile(const Store *st, const wchar_t *path, const wchar_t *source) {
    wchar_t dir[MAX_PATH]; lstrcpynW(dir, path, MAX_PATH);
    int l = lstrlenW(dir); while (l > 0 && dir[l - 1] != L'\\' && dir[l - 1] != L'/') l--;
    if (l > 0) { dir[l - 1] = 0; CreateDirectoryW(dir, NULL); }
    wchar_t *t = Serialize(st, source); int n; char *u = ToUtf8(t, &n); Free(t);
    BOOL ok = WriteAtomic(path, u, (DWORD)n); Free(u); return ok;
}

/* ---------- повторы (RRULE: DAILY/WEEKLY/MONTHLY/YEARLY, INTERVAL, COUNT, UNTIL, BYDAY, BYMONTHDAY, BYMONTH, BYSETPOS, WKST) ---------- */
struct Rule { int freq, interval, count; I64 until; BOOL hasUntil; int nbd, bd[24], bo[24]; int nmd, md[32]; int nmo, mo[12]; int nsp, sp[12]; int wkst; };
static BOOL KeyIs(const wchar_t *k, int kl, const wchar_t *n) { return kl == lstrlenW(n) && EqNI(k, n, kl); }
static int WdCode(const wchar_t *s) {
    static const wchar_t *k[7] = {L"MO", L"TU", L"WE", L"TH", L"FR", L"SA", L"SU"};
    for (int i = 0; i < 7; i++) if (EqNI(s, k[i], 2)) return i;
    return -1;
}
static BOOL ParseRule(const wchar_t *s, Rule *r, const Event *e) {
    memset(r, 0, sizeof *r); r->interval = 1;
    const wchar_t *p = s;
    while (*p) {
        const wchar_t *k = p; while (*p && *p != L'=' && *p != L';') p++;
        int kl = (int)(p - k); wchar_t val[220]; val[0] = 0;
        if (*p == L'=') { const wchar_t *v = ++p; while (*p && *p != L';') p++; int vl = (int)(p - v); if (vl > 219) vl = 219; memcpy(val, v, vl * sizeof(wchar_t)); val[vl] = 0; }
        if (*p == L';') p++;
        if (KeyIs(k, kl, L"FREQ")) r->freq = EqI(val, L"DAILY") ? 1 : EqI(val, L"WEEKLY") ? 2 : EqI(val, L"MONTHLY") ? 3 : EqI(val, L"YEARLY") ? 4 : 0;
        else if (KeyIs(k, kl, L"INTERVAL")) { r->interval = ToInt(val); if (r->interval < 1) r->interval = 1; }
        else if (KeyIs(k, kl, L"COUNT")) r->count = ToInt(val);
        else if (KeyIs(k, kl, L"UNTIL")) {
            I64 w; BOOL d, u;
            if (ParseDt(val, &w, &d, &u)) {
                if (u) w = UtcToWall(w, EvZone(e)); else if (d) w += 86399;
                r->until = w; r->hasUntil = TRUE;
            }
        }
        else if (KeyIs(k, kl, L"WKST")) { int c = WdCode(val); if (c >= 0) r->wkst = c; }
        else if (KeyIs(k, kl, L"BYDAY") || KeyIs(k, kl, L"BYMONTHDAY") || KeyIs(k, kl, L"BYMONTH") || KeyIs(k, kl, L"BYSETPOS")) {
            for (const wchar_t *q = val; *q; ) {
                const wchar_t *t = q; while (*q && *q != L',') q++;
                if (KeyIs(k, kl, L"BYDAY")) {
                    const wchar_t *x = t; int sg = 1, ord = 0;
                    if (*x == L'-') { sg = -1; x++; } else if (*x == L'+') x++;
                    while (*x >= L'0' && *x <= L'9') ord = ord * 10 + (*x++ - L'0');
                    int c = WdCode(x);
                    if (c >= 0 && r->nbd < 24) { r->bd[r->nbd] = c; r->bo[r->nbd] = ord * sg; r->nbd++; }
                } else {
                    int v = ToInt(t);
                    if (KeyIs(k, kl, L"BYMONTHDAY")) { if (v && r->nmd < 32) r->md[r->nmd++] = v; }
                    else if (KeyIs(k, kl, L"BYMONTH")) { if (v >= 1 && v <= 12 && r->nmo < 12) r->mo[r->nmo++] = v; }
                    else if (v && r->nsp < 12) r->sp[r->nsp++] = v;
                }
                if (*q) q++;
            }
        }
    }
    return r->freq != 0;
}
static BOOL InList(const int *a, int n, int v) { for (int i = 0; i < n; i++) if (a[i] == v) return TRUE; return FALSE; }
static BOOL MdMatch(const Rule &r, int d, int dim) { for (int i = 0; i < r.nmd; i++) { int m = r.md[i]; if (m > 0 ? d == m : d == dim + m + 1) return TRUE; } return FALSE; }
static BOOL BdMatch(const Rule &r, int wd, int d, int dim, BOOL useOrd) {
    for (int i = 0; i < r.nbd; i++) {
        if (r.bd[i] != wd) continue;
        int o = r.bo[i];
        if (!o || !useOrd) return TRUE;
        if (o > 0 ? (d - 1) / 7 + 1 == o : (dim - d) / 7 + 1 == -o) return TRUE;
    }
    return FALSE;
}
static int MonthCands(const Rule &r, int y, int m, int startD, int *out) {      // дни месяца по правилам BYMONTHDAY/BYDAY (дни от 1970)
    int dim = DaysInMonth(y, m), n = 0; I64 first = DaysFromCivil(y, m, 1);
    for (int d = 1; d <= dim; d++) {
        BOOL ok = TRUE;
        if (r.nmd) ok = MdMatch(r, d, dim);
        if (ok && r.nbd) ok = BdMatch(r, Weekday(first + d - 1), d, dim, TRUE);
        if (!r.nmd && !r.nbd) ok = d == startD;
        if (ok) out[n++] = (int)(first + d - 1);
    }
    return n;
}
typedef void (*OccFn)(const Event *e, I64 wallStart, I64 utcStart, I64 utcEnd, void *ctx);
struct Emitter { const Event *e; I64 from, to, dur; const wchar_t *zone; OccFn fn; void *ctx; };
static void EmitOne(const Emitter &em, I64 wall) {
    const Event *e = em.e;
    for (int i = 0; i < e->nexd; i++) if (e->exd[i] == wall) return;
    I64 s = WallToUtc(wall, em.zone), en = e->allDay ? WallToUtc(wall + em.dur, em.zone) : s + em.dur;
    if (s < em.to && (en > em.from || s >= em.from)) em.fn(e, wall, s, en, em.ctx);
}
// Вхождения события, пересекающие [fromUtc, toUtc). Замены вхождений (recId) здесь не учитываются - их ведёт Query.
static void Expand(const Event *e, I64 fromUtc, I64 toUtc, OccFn fn, void *ctx) {
    Emitter em; em.e = e; em.from = fromUtc; em.to = toUtc; em.zone = EvZone(e); em.fn = fn; em.ctx = ctx;
    em.dur = e->allDay ? e->end - e->start : WallToUtc(e->end, em.zone) - WallToUtc(e->start, em.zone);
    if (em.dur < 0) em.dur = 0;
    Rule r;
    if (e->hasRec || !e->rrule || !*e->rrule || !ParseRule(e->rrule, &r, e)) { EmitOne(em, e->start); return; }
    I64 startDay = FloorDiv(e->start, 86400), tod = e->start - startDay * 86400;
    I64 limitWall = toUtc + 2 * 86400, fromDay = FloorDiv(fromUtc - em.dur - 2 * 86400, 86400);
    int sy, sm, sd; CivilFromDays(startDay, &sy, &sm, &sd);
    int fy, fm, fd; CivilFromDays(fromDay, &fy, &fm, &fd);
    I64 ws0 = startDay - ((Weekday(startDay) - r.wkst + 7) % 7), mi0 = (I64)sy * 12 + (sm - 1), fmi = (I64)fy * 12 + (fm - 1);
    I64 k0 = 0;
    if (!r.count) {
        I64 iv = r.interval;
        k0 = r.freq == 1 ? FloorDiv(fromDay - startDay, iv) : r.freq == 2 ? FloorDiv(fromDay - ws0, 7 * iv) : r.freq == 3 ? FloorDiv(fmi - mi0, iv) : FloorDiv((I64)fy - sy, iv);
        if (k0 < 0) k0 = 0;
    }
    int emitted = 0, cand[400];
    for (I64 k = k0, guard = 0; guard < 100000; k++, guard++) {
        int nc = 0; I64 periodFirst;
        if (r.freq == 1) {
            I64 day = startDay + k * r.interval; periodFirst = day;
            int y, m, d; CivilFromDays(day, &y, &m, &d); int dim = DaysInMonth(y, m);
            BOOL ok = (!r.nmo || InList(r.mo, r.nmo, m)) && (!r.nmd || MdMatch(r, d, dim)) && (!r.nbd || BdMatch(r, Weekday(day), d, dim, FALSE));
            if (ok) cand[nc++] = (int)day;
        } else if (r.freq == 2) {
            I64 wk = ws0 + k * 7 * r.interval; periodFirst = wk;
            for (int i = 0; i < 7; i++) {
                I64 day = wk + i; int wd = Weekday(day), y, m, d; CivilFromDays(day, &y, &m, &d);
                BOOL ok = r.nbd ? InList(r.bd, r.nbd, wd) : wd == Weekday(startDay);
                if (ok && r.nmo && !InList(r.mo, r.nmo, m)) ok = FALSE;
                if (ok) cand[nc++] = (int)day;
            }
        } else if (r.freq == 3) {
            I64 mi = mi0 + k * r.interval; int y = (int)FloorDiv(mi, 12), m = (int)(mi - (I64)y * 12) + 1; periodFirst = DaysFromCivil(y, m, 1);
            if (!r.nmo || InList(r.mo, r.nmo, m)) nc = MonthCands(r, y, m, sd, cand);
        } else {
            int y = (int)(sy + k * r.interval); periodFirst = DaysFromCivil(y, 1, 1);
            if (r.nmo) { for (int m = 1; m <= 12; m++) if (InList(r.mo, r.nmo, m)) nc += MonthCands(r, y, m, sd, cand + nc); }
            else nc = MonthCands(r, y, sm, sd, cand);
        }
        if (periodFirst * 86400 > limitWall) return;
        if (r.nsp && nc) {                                                  // BYSETPOS: оставляем указанные по счёту
            int sel[12], ns = 0;
            for (int i = 0; i < r.nsp; i++) { int p = r.sp[i] > 0 ? r.sp[i] - 1 : nc + r.sp[i]; if (p >= 0 && p < nc) sel[ns++] = cand[p]; }
            for (int i = 1; i < ns; i++) { int v = sel[i], j = i - 1; while (j >= 0 && sel[j] > v) { sel[j + 1] = sel[j]; j--; } sel[j + 1] = v; }
            nc = 0; for (int i = 0; i < ns; i++) if (!nc || cand[nc - 1] != sel[i]) cand[nc++] = sel[i];
        }
        for (int i = 0; i < nc; i++) {
            I64 wall = (I64)cand[i] * 86400 + tod;
            if (wall < e->start) continue;
            if (r.hasUntil && wall > r.until) return;
            if (r.count && emitted >= r.count) return;
            emitted++;
            if (wall > limitWall) return;
            EmitOne(em, wall);
        }
    }
}

/* ---------- запрос вхождений за период ---------- */
struct Occ { int ev; I64 s, e, wall; BOOL allDay; };
struct OccList { Occ *a; int n, cap; };
struct QCtx { const Store *st; OccList *out; int ev; BOOL checkOverrides; };
static void QueryCb(const Event *e, I64 wall, I64 s, I64 en, void *c) {
    QCtx *q = (QCtx *)c;
    if (q->checkOverrides && !e->hasRec) for (int i = 0; i < q->st->n; i++) { const Event *o = &q->st->ev[i]; if (o->hasRec && o->recId == wall && o->uid && !lstrcmpW(o->uid, e->uid)) return; }   // это вхождение заменено
    OccList *l = q->out;
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 64; l->a = (Occ *)Realloc(l->a, l->cap * sizeof(Occ)); }
    Occ *o = &l->a[l->n++]; o->ev = q->ev; o->s = s; o->e = en; o->wall = wall; o->allDay = e->allDay;
}
static void OccFree(OccList *l) { Free(l->a); memset(l, 0, sizeof *l); }
// Все видимые вхождения (без скрытых, удалённых и отменённых; showHidden - со скрытыми), отсортированные по началу; целодневные раньше.
static void Query(const Store *st, I64 fromUtc, I64 toUtc, OccList *out, BOOL showHidden = FALSE) {
    memset(out, 0, sizeof *out);
    QCtx q; q.st = st; q.out = out; q.checkOverrides = FALSE;
    for (int i = 0; i < st->n; i++) if (st->ev[i].hasRec) q.checkOverrides = TRUE;
    for (int i = 0; i < st->n; i++) {
        const Event *e = &st->ev[i];
        if ((e->hidden && !showHidden) || e->deleted || e->cancelled) continue;
        q.ev = i; Expand(e, fromUtc, toUtc, QueryCb, &q);
    }
    for (int i = 1; i < out->n; i++) {
        Occ v = out->a[i]; int j = i - 1;
        while (j >= 0 && (out->a[j].s > v.s || (out->a[j].s == v.s && !out->a[j].allDay && v.allDay))) { out->a[j + 1] = out->a[j]; j--; }
        out->a[j + 1] = v;
    }
}

/* ---------- напоминания: срабатывания, журнал показанных, отложенные (чистая логика, без интерфейса) ----------
   Срабатывание (Fire) = вхождение события + напоминание (минуты до начала). Целодневное: напоминание кратно суткам (в том числе 0) считается от
   allDayTod (секунды от местной полуночи, по умолчанию 09:00), прочее - как в ICS, от полуночи. Показанное запоминается в журнале (SeenList):
   ключ uid + начало вхождения (UTC) + минуты; отложенное - та же запись с until. */
struct Fire { int ev; I64 occ, end, wall, due; int alarm; BOOL allDay; };      // occ/end - UTC; wall - стенное начало (целодневное: полночь даты); due - когда показать (UTC)
struct FireList { Fire *a; int n, cap; };
static void FireFree(FireList *l) { Free(l->a); memset(l, 0, sizeof *l); }
static I64 FireDue(I64 occ, I64 wall, BOOL allDay, int alarm, int allDayTod) {
    if (allDay && alarm % 1440 == 0) return WallToUtc(wall - (I64)alarm * 60 + allDayTod, L"");
    return occ - (I64)alarm * 60;
}
static const I64 kRemBack = 86400, kRemAhead = 8 * 86400, kRemMax = 400 * 86400, kRemKeep = 3600, kRemLog = 3 * 86400;
// Все срабатывания вхождений в окне [now-1 сутки, now+8 суток] (шире, если у кого-то напоминание за больше недели). Скрытые, удалённые и отменённые не берутся.
static void RemBuild(const Store *st, I64 now, int allDayTod, FireList *out) {
    memset(out, 0, sizeof *out);
    I64 maxAl = 0;
    for (int i = 0; i < st->n; i++) for (int j = 0; j < st->ev[i].nal; j++) if (st->ev[i].alarms[j] > maxAl) maxAl = st->ev[i].alarms[j];
    I64 ahead = maxAl * 60 + 86400; if (ahead < kRemAhead) ahead = kRemAhead; if (ahead > kRemMax) ahead = kRemMax;
    OccList ol; Query(st, now - kRemBack, now + ahead, &ol);
    for (int i = 0; i < ol.n; i++) {
        const Occ &o = ol.a[i]; const Event *e = &st->ev[o.ev];
        for (int j = 0; j < e->nal; j++) {
            if (out->n == out->cap) { out->cap = out->cap ? out->cap * 2 : 64; out->a = (Fire *)Realloc(out->a, out->cap * sizeof(Fire)); }
            Fire *f = &out->a[out->n++]; f->ev = o.ev; f->occ = o.s; f->end = o.e; f->wall = o.wall; f->alarm = e->alarms[j]; f->allDay = o.allDay;
            f->due = FireDue(o.s, o.wall, o.allDay, f->alarm, allDayTod);
        }
    }
    OccFree(&ol);
}
// Журнал показанного и отложенного
struct Seen { wchar_t *uid; I64 occ, until; int alarm; };                 // until > 0 - отложено до этого момента (UTC)
struct SeenList { Seen *a; int n, cap; };
static void SeenFree(SeenList *l) { for (int i = 0; i < l->n; i++) Free(l->a[i].uid); Free(l->a); memset(l, 0, sizeof *l); }
static int SeenFind(const SeenList *l, const wchar_t *uid, I64 occ, int alarm) {
    for (int i = 0; i < l->n; i++) if (l->a[i].occ == occ && l->a[i].alarm == alarm && !lstrcmpW(l->a[i].uid, uid)) return i;
    return -1;
}
static int SeenAdd(SeenList *l, const wchar_t *uid, I64 occ, int alarm) {
    int i = SeenFind(l, uid, occ, alarm); if (i >= 0) return i;
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 32; l->a = (Seen *)Realloc(l->a, l->cap * sizeof(Seen)); }
    Seen *s = &l->a[l->n]; memset(s, 0, sizeof *s); s->uid = Dup(uid); s->occ = occ; s->alarm = alarm; return l->n++;
}
static void SeenMark(SeenList *l, const wchar_t *uid, I64 occ, int alarm) { l->a[SeenAdd(l, uid, occ, alarm)].until = 0; }
static void SeenSnooze(SeenList *l, const wchar_t *uid, I64 occ, int alarm, I64 until) { l->a[SeenAdd(l, uid, occ, alarm)].until = until; }
static BOOL SeenPrune(SeenList *l, I64 now) {                              // убрать записи о вхождениях, начавшихся больше трёх суток назад
    BOOL ch = FALSE;
    for (int i = l->n - 1; i >= 0; i--) if (l->a[i].occ < now - kRemLog) { Free(l->a[i].uid); memmove(l->a + i, l->a + i + 1, (l->n - i - 1) * sizeof(Seen)); l->n--; ch = TRUE; }
    return ch;
}
static I64 ToI64(const wchar_t *s, int n) { I64 v = 0; int sg = 1, i = 0; if (n > 0 && s[0] == L'-') { sg = -1; i = 1; } for (; i < n && s[i] >= L'0' && s[i] <= L'9'; i++) v = v * 10 + (s[i] - L'0'); return v * sg; }
static void FmtI64(wchar_t *o, I64 v) {                                     // wsprintf не знает 64-битных чисел
    wchar_t t[24]; int n = 0, k = 0; BOOL neg = v < 0; unsigned long long u = neg ? 0ULL - (unsigned long long)v : (unsigned long long)v;
    do { t[n++] = (wchar_t)(L'0' + (int)(u % 10)); u /= 10; } while (u);
    if (neg) o[k++] = L'-';
    while (n) o[k++] = t[--n];
    o[k] = 0;
}
// Текст журнала: строка "uid|начало|минуты" (snooze: ещё "|до"). uid может содержать "|": поля разбираются с конца.
static wchar_t *SeenSave(const SeenList *l, BOOL snooze) {
    Buf b = {0};
    for (int i = 0; i < l->n; i++) {
        const Seen *s = &l->a[i]; if (snooze && s->until <= 0) continue;
        wchar_t t[24]; BufAdd(&b, s->uid); BufAdd(&b, L"|");
        FmtI64(t, s->occ); BufAdd(&b, t); BufAdd(&b, L"|"); wsprintfW(t, L"%d", s->alarm); BufAdd(&b, t);
        if (snooze) { BufAdd(&b, L"|"); FmtI64(t, s->until); BufAdd(&b, t); }
        BufAdd(&b, L"\n");
    }
    return b.p ? b.p : Dup(L"");
}
static void SeenLoad(SeenList *l, const wchar_t *text, BOOL snooze) {
    for (const wchar_t *p = text; *p; ) {
        const wchar_t *q = p; while (*q && *q != L'\n') q++;
        int n = (int)(q - p); while (n > 0 && p[n - 1] == L'\r') n--;
        int cut[4], nc = 0, need = snooze ? 3 : 2;                         // позиции "|" с конца
        for (int i = n - 1; i >= 0 && nc < need; i--) if (p[i] == L'|') cut[nc++] = i;
        if (nc == need && cut[need - 1] > 0) {                              // uid не пустой, числа с конца: [до], минуты, начало
            I64 v[3]; int e = n, nv = 0; BOOL ok = TRUE;
            for (int k = 0; k < need && ok; k++) { int a = cut[k] + 1; if (e - a < 1) ok = FALSE; else { v[nv++] = ToI64(p + a, e - a); e = cut[k]; } }
            if (ok) {
                wchar_t *uid = DupN(p, cut[need - 1]);
                if (snooze) SeenSnooze(l, uid, v[2], (int)v[1], v[0]); else SeenMark(l, uid, v[1], (int)v[0]);
                Free(uid);
            }
        }
        p = *q ? q + 1 : q;
    }
}
struct Pick { int fire; BOOL late; };                                       // fire - индекс в FireList; late - опоздание больше grace (показывать сводкой)
struct PickList { Pick *a; int n, cap; };
static void PickFree(PickList *l) { Free(l->a); memset(l, 0, sizeof *l); }
static BOOL RemStale(const Fire &f, I64 t) { return t >= f.occ + (f.allDay ? 86400 : kRemKeep); }      // событие уже началось давно (час; целодневное - сутки): напоминать поздно
static BOOL SameOcc(const Store *st, const Fire &a, const Fire &b) { return a.occ == b.occ && !lstrcmpW(st->ev[a.ev].uid, st->ev[b.ev].uid); }
static void PickAdd(PickList *l, int fire, BOOL late) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 8; l->a = (Pick *)Realloc(l->a, l->cap * sizeof(Pick)); }
    l->a[l->n].fire = fire; l->a[l->n].late = late; l->n++;
}
// Что показать сейчас: срок прошёл, ещё не показывалось, событие не началось или началось не больше часа назад (+ отложенное, чей срок настал).
// Несколько напоминаний одного вхождения - показывается самое позднее, остальные тихо отмечаются. Показанное отмечается в журнале sn; результат по началу вхождения.
// grace - сколько секунд опоздания считается «вовремя». Возвращает TRUE, если журнал изменился (надо сохранить).
static BOOL RemPick(const Store *st, const FireList *fl, SeenList *sn, I64 now, I64 grace, PickList *out) {
    memset(out, 0, sizeof *out); BOOL ch = FALSE;
    int *c = (int *)Alloc((fl->n + 1) * sizeof(int)), nc = 0;
    for (int i = 0; i < fl->n; i++) {
        const Fire &f = fl->a[i]; if (f.due > now || RemStale(f, now)) continue;
        if (SeenFind(sn, st->ev[f.ev].uid, f.occ, f.alarm) >= 0) continue;
        c[nc++] = i;
    }
    for (int k = 0; k < nc; k++) {
        const Fire &f = fl->a[c[k]]; BOOL win = TRUE;
        for (int m = 0; m < nc && win; m++) {
            const Fire &g = fl->a[c[m]];
            if (m != k && SameOcc(st, f, g) && (g.due > f.due || (g.due == f.due && m < k))) win = FALSE;
        }
        SeenMark(sn, st->ev[f.ev].uid, f.occ, f.alarm); ch = TRUE;
        if (win) PickAdd(out, c[k], now - f.due > grace);
    }
    Free(c);
    for (int j = 0; j < sn->n; j++) {                                        // отложенные, чей срок настал
        Seen &s = sn->a[j]; if (s.until <= 0 || s.until > now) continue;
        I64 until = s.until; s.until = 0; ch = TRUE;
        for (int i = 0; i < fl->n; i++) {
            const Fire &f = fl->a[i];
            if (f.occ != s.occ || f.alarm != s.alarm || lstrcmpW(st->ev[f.ev].uid, s.uid)) continue;
            if (now < (f.occ > until ? f.occ : until) + (f.allDay ? 86400 : kRemKeep)) PickAdd(out, i, now - until > grace);
            break;                                                           // события не нашлось (удалено, перенесено) - отложенное снято
        }
    }
    for (int i = 1; i < out->n; i++) {
        Pick v = out->a[i]; int j = i - 1;
        while (j >= 0 && fl->a[out->a[j].fire].occ > fl->a[v.fire].occ) { out->a[j + 1] = out->a[j]; j--; }
        out->a[j + 1] = v;
    }
    return ch;
}
// Ближайший будущий момент (UTC), когда что-то может сработать; 0 - ждать нечего.
static I64 RemNext(const Store *st, const FireList *fl, const SeenList *sn, I64 now) {
    I64 best = 0;
    for (int i = 0; i < fl->n; i++) {
        const Fire &f = fl->a[i]; if (f.due <= now || RemStale(f, f.due) || (best && f.due >= best)) continue;
        if (SeenFind(sn, st->ev[f.ev].uid, f.occ, f.alarm) < 0) best = f.due;
    }
    for (int j = 0; j < sn->n; j++) if (sn->a[j].until > now && (!best || sn->a[j].until < best)) best = sn->a[j].until;
    return best;
}

/* ---------- правка: копия, перенос между источниками, удаление/скрытие ---------- */
static wchar_t *DupOr0(const wchar_t *s) { return s ? Dup(s) : NULL; }
static void EventClone(Event *d, const Event *s) {                  // глубокая копия
    *d = *s;
    d->uid = DupOr0(s->uid); d->summary = DupOr0(s->summary); d->desc = DupOr0(s->desc); d->loc = DupOr0(s->loc); d->tzid = DupOr0(s->tzid);
    d->rrule = DupOr0(s->rrule); d->source = DupOr0(s->source); d->etag = DupOr0(s->etag); d->extra = DupOr0(s->extra);
    d->exd = NULL; d->alarms = NULL;
    if (s->nexd) { d->exd = (I64 *)Alloc(s->nexd * sizeof(I64)); memcpy(d->exd, s->exd, s->nexd * sizeof(I64)); }
    if (s->nal) { d->alarms = (int *)Alloc(s->nal * sizeof(int)); memcpy(d->alarms, s->alarms, s->nal * sizeof(int)); }
}
static int StoreAddClone(Store *st, const Event *src) {            // src может лежать в самом хранилище; возвращает индекс копии
    Event t; EventClone(&t, src); Event *e = StoreNew(st); *e = t; return (int)(e - st->ev);
}
static void EventTake(Event *dst, Event *src) { EventFree(dst); *dst = *src; memset(src, 0, sizeof *src); }      // dst получает содержимое src (src обнуляется)
static BOOL SameSrc(const wchar_t *a, const wchar_t *b) { return !lstrcmpW(a ? a : L"", b ? b : L""); }
static BOOL SameSeries(const Event *e, const wchar_t *uid, const wchar_t *src) { return e->uid && !lstrcmpW(e->uid, uid) && SameSrc(e->source, src); }
// Перенос события (всей серии: главное событие и замены вхождений) в другой источник: в новом создаётся копия с НОВЫМ UID; в прежнем событие
// помечается удалённым (oldSyncs - источник синхронизируется, надгробие ждёт отправки) либо сразу убирается; keepCopy - прежнее остаётся как есть.
// Возвращает индекс новой копии события idx. Тот же источник + keepCopy даёт обычное дублирование.
static int MoveEvent(Store *st, int idx, const wchar_t *newSrc, BOOL keepCopy, BOOL oldSyncs) {
    wchar_t *uid = Dup(st->ev[idx].uid), *old = Dup(st->ev[idx].source); wchar_t nu[80]; NewUid(nu, 80);
    int n0 = st->n, res = -1;
    for (int i = 0; i < n0; i++) {
        if (!SameSeries(&st->ev[i], uid, old) || st->ev[i].deleted) continue;
        int k = StoreAddClone(st, &st->ev[i]); Event *c = &st->ev[k];
        SetStr(&c->uid, nu); SetStr(&c->source, newSrc); SetStr(&c->etag, NULL); c->hidden = c->deleted = FALSE; c->synced = 0; Touch(c);
        if (i == idx) res = k;
    }
    if (!keepCopy) for (int i = n0 - 1; i >= 0; i--) {
        Event *o = &st->ev[i];
        if (!SameSeries(o, uid, old) || o->deleted) continue;
        if (oldSyncs) { o->deleted = TRUE; Touch(o); } else { StoreRemove(st, i); if (res > i) res--; }
    }
    Free(uid); Free(old); return res;
}
// Удаление серии события idx: everywhere - «удалить везде» (источник синхронизируется - надгробие deleted, иначе сразу из хранилища);
// иначе «убрать только из DKeys» (hidden: остаётся в источнике, не возвращается при синхронизации; stamp не трогаем - в источник это не уходит).
static void DeleteEvent(Store *st, int idx, BOOL everywhere, BOOL syncs) {
    wchar_t *uid = Dup(st->ev[idx].uid), *src = Dup(st->ev[idx].source);
    for (int i = st->n - 1; i >= 0; i--) {
        Event *e = &st->ev[i]; if (!SameSeries(e, uid, src)) continue;
        if (!everywhere) e->hidden = TRUE;
        else if (syncs) { e->deleted = TRUE; Touch(e); }
        else StoreRemove(st, i);
    }
    Free(uid); Free(src);
}
static void SetHidden(Store *st, int idx, BOOL hide) {              // вернуть скрытое (hide FALSE) или скрыть серию
    wchar_t *uid = Dup(st->ev[idx].uid), *src = Dup(st->ev[idx].source);
    for (int i = 0; i < st->n; i++) if (SameSeries(&st->ev[i], uid, src)) st->ev[i].hidden = hide;
    Free(uid); Free(src);
}

/* ---------- правило повтора из полей диалога <-> RRULE ---------- */
struct RuleSpec { int freq, interval, wdMask, endKind, count; I64 untilDay; };      // freq 0 - нет, 1..4 - день/неделя/месяц/год; wdMask - биты дней (0 = пн); endKind 0 - никогда, 1 - до даты (untilDay включительно), 2 - N раз
static BOOL RuleSpecEq(const RuleSpec &a, const RuleSpec &b) { return a.freq == b.freq && a.interval == b.interval && a.wdMask == b.wdMask && a.endKind == b.endKind && a.count == b.count && a.untilDay == b.untilDay; }
static void RuleBuild(const RuleSpec &r, BOOL allDay, const wchar_t *tzid, wchar_t *out, int cap) {
    out[0] = 0; if (r.freq < 1 || r.freq > 4) return;
    static const wchar_t *fr[4] = {L"DAILY", L"WEEKLY", L"MONTHLY", L"YEARLY"}, *wd[7] = {L"MO", L"TU", L"WE", L"TH", L"FR", L"SA", L"SU"};
    wchar_t t[160], x[48]; wsprintfW(t, L"FREQ=%s", fr[r.freq - 1]);
    if (r.interval > 1) { wsprintfW(x, L";INTERVAL=%d", r.interval); lstrcatW(t, x); }
    if (r.freq == 2 && r.wdMask) {
        lstrcatW(t, L";BYDAY="); BOOL first = TRUE;
        for (int i = 0; i < 7; i++) if (r.wdMask & (1 << i)) { if (!first) lstrcatW(t, L","); lstrcatW(t, wd[i]); first = FALSE; }
    }
    if (r.endKind == 1) {                                               // конец включительно: последняя секунда дня в зоне события
        I64 w = r.untilDay * 86400 + 86399; wchar_t d[40];
        if (allDay) FmtDt(d, w, TRUE, FALSE); else if (!tzid || !*tzid) FmtDt(d, w, FALSE, FALSE); else FmtDt(d, WallToUtc(w, tzid), FALSE, TRUE);
        lstrcatW(t, L";UNTIL="); lstrcatW(t, d);
    } else if (r.endKind == 2 && r.count > 0) { wsprintfW(x, L";COUNT=%d", r.count); lstrcatW(t, x); }
    lstrcpynW(out, t, cap);
}
// RRULE -> поля. TRUE - правило простое (поля воспроизводят его); FALSE - сложнее поддерживаемого: такую строку надо хранить как есть.
static BOOL RuleSpecParse(const wchar_t *rrule, const Event *e, RuleSpec *o) {
    memset(o, 0, sizeof *o); o->interval = 1;
    if (!rrule || !*rrule) return TRUE;
    BOOL hasC = FALSE, hasU = FALSE;
    for (const wchar_t *p = rrule; *p; ) {
        const wchar_t *k = p; while (*p && *p != L'=' && *p != L';') p++;
        int kl = (int)(p - k); const wchar_t *v = p; if (*p == L'=') { v = ++p; while (*p && *p != L';') p++; }
        int vl = (int)(p - v); if (*p == L';') p++;
        BOOL ok = KeyIs(k, kl, L"FREQ") || KeyIs(k, kl, L"INTERVAL") || KeyIs(k, kl, L"WKST");
        if (KeyIs(k, kl, L"COUNT")) { ok = TRUE; hasC = TRUE; } else if (KeyIs(k, kl, L"UNTIL")) { ok = TRUE; hasU = TRUE; }
        else if (KeyIs(k, kl, L"BYDAY")) { ok = TRUE; for (int i = 0; i < vl; i++) if (v[i] >= L'0' && v[i] <= L'9') ok = FALSE; }
        if (!ok) return FALSE;
    }
    Rule r; if ((hasC && hasU) || !ParseRule(rrule, &r, e) || (hasU && !r.hasUntil) || (r.nbd && r.freq != 2)) return FALSE;
    o->freq = r.freq; o->interval = r.interval; for (int i = 0; i < r.nbd; i++) o->wdMask |= 1 << r.bd[i];
    if (r.hasUntil) { o->endKind = 1; o->untilDay = FloorDiv(r.until, 86400); } else if (r.count > 0) { o->endKind = 2; o->count = r.count; }
    return TRUE;
}

/* ---------- дата и время в текстовых полях ---------- */
static BOOL ScanInt(const wchar_t *s, int n, int *pos, int maxDig, int *v) {
    int i = *pos, c = 0, x = 0;
    while (i < n && s[i] >= L'0' && s[i] <= L'9' && c < maxDig) { x = x * 10 + (s[i] - L'0'); i++; c++; }
    if (!c) return FALSE;
    *v = x; *pos = i; return TRUE;
}
static BOOL MkDay(int y, int m, int d, I64 *day) { if (m < 1 || m > 12 || d < 1 || y < 1900 || y > 2200 || d > DaysInMonth(y, m)) return FALSE; *day = DaysFromCivil(y, m, d); return TRUE; }
static BOOL MkDayNear(int d, int m, int y, I64 base, I64 *out) {     // y < 0 - без года: ближайшая дата не раньше base
    if (y >= 0) return MkDay(y, m, d, out);
    int by, bm, bd; CivilFromDays(base, &by, &bm, &bd);
    for (int yy = by; yy <= by + 8; yy++) { I64 x; if (MkDay(yy, m, d, &x) && x >= base) { *out = x; return TRUE; } }
    return FALSE;
}
static BOOL IsDash(wchar_t c) { return c == L'-' || c == 0x2013 || c == 0x2014; }
// дд.мм[.гг[гг]] | дд/мм/гггг | гггг-мм-дд; весь текст s[0..n) должен быть датой
static BOOL ScanDate(const wchar_t *s, int n, I64 base, I64 *out) {
    int pos = 0, a, b, c;
    if (!ScanInt(s, n, &pos, 4, &a) || pos >= n) return FALSE;
    if (pos == 4 && s[pos] == L'-') {
        pos++; if (!ScanInt(s, n, &pos, 2, &b) || pos >= n || s[pos] != L'-') return FALSE;
        pos++; if (!ScanInt(s, n, &pos, 2, &c) || pos != n) return FALSE;
        return MkDay(a, b, c, out);
    }
    if ((s[pos] != L'.' && s[pos] != L'/') || a < 1 || a > 31) return FALSE;
    wchar_t sep = s[pos++]; int y = -1;
    if (!ScanInt(s, n, &pos, 2, &b)) return FALSE;
    if (pos < n) {
        if (s[pos] != sep) return FALSE;
        pos++; int ys = pos; if (!ScanInt(s, n, &pos, 4, &c)) return FALSE;
        if (pos - ys == 2) y = 2000 + c; else if (pos - ys == 4) y = c; else return FALSE;
    }
    return pos == n && MkDayNear(a, b, y, base, out);
}
static BOOL ScanTime(const wchar_t *s, int n, int *pos, BOOL needColon, int *tod) {   // ч:мм (или просто ч, если needColon FALSE) -> секунды от полуночи
    int p = *pos, h, m = 0; if (!ScanInt(s, n, &p, 2, &h)) return FALSE;
    if (p < n && s[p] == L':') { p++; int ms = p; if (!ScanInt(s, n, &p, 2, &m) || p - ms != 2) return FALSE; }
    else if (needColon) return FALSE;
    if (h > 23 || m > 59) return FALSE;
    *tod = h * 3600 + m * 60; *pos = p; return TRUE;
}
static void Trim(const wchar_t *&s, int &n) { while (n > 0 && s[n - 1] <= L' ') n--; while (n > 0 && *s <= L' ') { s++; n--; } }
static BOOL ParseDayStr(const wchar_t *s, I64 base, I64 *day) { int n = lstrlenW(s); Trim(s, n); return ScanDate(s, n, base, day); }
static BOOL ParseTodStr(const wchar_t *s, int *tod) { int n = lstrlenW(s), pos = 0; Trim(s, n); return ScanTime(s, n, &pos, FALSE, tod) && pos == n; }
static void FmtDay(wchar_t *o, I64 day) { int y, m, d; CivilFromDays(day, &y, &m, &d); wsprintfW(o, L"%02d.%02d.%04d", d, m, y); }
static void FmtTod(wchar_t *o, int tod) { wsprintfW(o, L"%02d:%02d", tod / 3600, tod / 60 % 60); }
static I64 DayAddMonths(I64 day, int n) {                            // тот же день через n месяцев (31-е -> последний день месяца)
    int y, m, d; CivilFromDays(day, &y, &m, &d); I64 mi = (I64)y * 12 + (m - 1) + n, yy = FloorDiv(mi, 12);
    y = (int)yy; m = (int)(mi - yy * 12) + 1; int dim = DaysInMonth(y, m); if (d > dim) d = dim;
    return DaysFromCivil(y, m, d);
}

/* ---------- быстрый ввод по-русски: «завтра 15:00 встреча», «20.11 14:30-16 планёрка», «каждый понедельник 10:00 летучка» ----------
   Разбор по словам: распознанные слова (дата, время, повтор, длительность) забираются, остальное - название (с заглавной буквы).
   Без даты берётся defDay, без времени - целый день; конец по умолчанию - через час. Времена - местные стенные секунды. */
struct Quick { wchar_t *summary; RuleSpec rule; BOOL allDay; I64 start, end; };       // summary - Free; end для целодневного не включается
struct QTok { int off, len, clen; BOOL used; };
struct QP { const wchar_t *src; wchar_t *lc; QTok *t; int n; I64 now, today; };
static BOOL QAnyS(const wchar_t *s, int n, const wchar_t *list) {    // s[0..n) совпадает с одним из слов списка "а|б|в" (пустой вариант допустим)
    for (;;) {
        const wchar_t *e = list; while (*e && *e != L'|') e++;
        int l = (int)(e - list), i = 0; if (l == n) { while (i < n && s[i] == list[i]) i++; if (i == n) return TRUE; }
        if (!*e) return FALSE;
        list = e + 1;
    }
}
static BOOL QAny(const QP &p, int i, const wchar_t *list) { return i >= 0 && i < p.n && !p.t[i].used && QAnyS(p.lc + p.t[i].off, p.t[i].clen, list); }
static BOOL QNum(const QP &p, int i, int *v) {
    if (i < 0 || i >= p.n || p.t[i].used) return FALSE;
    const wchar_t *s = p.lc + p.t[i].off; int n = p.t[i].clen, x = 0; if (n < 1 || n > 4) return FALSE;
    for (int k = 0; k < n; k++) { if (s[k] < L'0' || s[k] > L'9') return FALSE; x = x * 10 + (s[k] - L'0'); }
    *v = x; return TRUE;
}
static void QUse(QP &p, int a, int b) { for (int i = a; i < b; i++) p.t[i].used = TRUE; }
static int QWd(const QP &p, int i) {                                 // день недели в любой форме («пятницу», «по средам», «пн»), 0 = пн
    static const wchar_t *st[7] = {L"понедельник", L"вторник", L"сред", L"четверг", L"пятниц", L"суббот", L"воскресень"}, *ab[7] = {L"пн", L"вт", L"ср", L"чт", L"пт", L"сб", L"вс"};
    if (i < 0 || i >= p.n || p.t[i].used) return -1;
    const wchar_t *s = p.lc + p.t[i].off; int n = p.t[i].clen;
    for (int d = 0; d < 7; d++) {
        if (QAnyS(s, n, ab[d])) return d;
        int sl = lstrlenW(st[d]), k = 0; if (n < sl) continue;
        while (k < sl && s[k] == st[d][k]) k++;
        if (k == sl && QAnyS(s + sl, n - sl, L"|а|у|ы|е|ой|ам|ах|ь|ю|я|ям|ях")) return d;
    }
    return -1;
}
static int QMon(const QP &p, int i) {
    static const wchar_t *m[12] = {L"января|январь|янв", L"февраля|февраль|фев", L"марта|март|мар", L"апреля|апрель|апр", L"мая|май", L"июня|июнь|июн", L"июля|июль|июл",
                                   L"августа|август|авг", L"сентября|сентябрь|сен|сент", L"октября|октябрь|окт", L"ноября|ноябрь|ноя|нояб", L"декабря|декабрь|дек"};
    if (i < 0 || i >= p.n || p.t[i].used) return 0;
    for (int k = 0; k < 12; k++) if (QAnyS(p.lc + p.t[i].off, p.t[i].clen, m[k])) return k + 1;
    return 0;
}
static int QUnit(const QP &p, int i) {                              // 1 минуты, 2 часы, 3 дни, 4 недели, 5 месяцы, 6 годы
    if (QAny(p, i, L"минуту|минуты|минут|минута|мин")) return 1;
    if (QAny(p, i, L"час|часа|часов|ч")) return 2;
    if (QAny(p, i, L"день|дня|дней|дни|дн")) return 3;
    if (QAny(p, i, L"неделю|недели|недель|неделя|нед")) return 4;
    if (QAny(p, i, L"месяц|месяца|месяцев|мес")) return 5;
    if (QAny(p, i, L"год|года|лет|г")) return 6;
    return 0;
}
static BOOL QDateAt(const QP &p, int i, I64 *day, int *nt) {          // дата в токене i: слово («завтра»), дд.мм[.гггг], «20 ноября [2027]»
    if (i < 0 || i >= p.n || p.t[i].used) return FALSE;
    const wchar_t *s = p.lc + p.t[i].off; int n = p.t[i].clen, a, pos = 0; *nt = 1;
    if (QAnyS(s, n, L"сегодня")) { *day = p.today; return TRUE; }
    if (QAnyS(s, n, L"завтра")) { *day = p.today + 1; return TRUE; }
    if (QAnyS(s, n, L"послезавтра")) { *day = p.today + 2; return TRUE; }
    if (QAnyS(s, n, L"вчера")) { *day = p.today - 1; return TRUE; }
    if (ScanDate(s, n, p.today, day)) return TRUE;
    if (ScanInt(s, n, &pos, 2, &a) && pos == n && a >= 1 && a <= 31) {
        int m = QMon(p, i + 1), v, y = -1; if (!m) return FALSE;
        *nt = 2; if (QNum(p, i + 2, &v) && v >= 2000 && v <= 2200) { y = v; *nt = 3; }
        return MkDayNear(a, m, y, p.today, day);
    }
    return FALSE;
}
static BOOL QTimeTok(const QP &p, int i, int *t1, int *t2) {         // «15:00» или «14:30-16[:00]» одним токеном; t2 = -1 без конца
    if (i < 0 || i >= p.n || p.t[i].used) return FALSE;
    const wchar_t *s = p.lc + p.t[i].off; int n = p.t[i].clen, pos = 0; *t2 = -1;
    if (!ScanTime(s, n, &pos, TRUE, t1)) return FALSE;
    if (pos == n) return TRUE;
    if (!IsDash(s[pos])) return FALSE;
    pos++; if (!ScanTime(s, n, &pos, FALSE, t2) || pos != n) return FALSE;
    return TRUE;
}
static BOOL QTimeEnd(const QP &p, int j, int *t2, int *nt) {          // конец после начала: «- 16:00», «до 16»
    if (j < 0 || j + 1 >= p.n || p.t[j].used || p.t[j + 1].used) return FALSE;
    BOOL dash = p.t[j].clen == 1 && IsDash(p.lc[p.t[j].off]);
    if (!dash && !QAny(p, j, L"до")) return FALSE;
    const wchar_t *s = p.lc + p.t[j + 1].off; int n = p.t[j + 1].clen, pos = 0;
    if (!ScanTime(s, n, &pos, FALSE, t2) || pos != n) return FALSE;
    *nt = 2; return TRUE;
}
static int QDayPart(const QP &p, int i) { return QAny(p, i, L"утра") ? 1 : QAny(p, i, L"дня") ? 2 : QAny(p, i, L"вечера") ? 3 : QAny(p, i, L"ночи") ? 4 : 0; }
static int QHour(int h, int dp) {                                    // «9 вечера» -> 21, «12 ночи» -> 0
    if ((dp == 2 || dp == 3) && h < 12) return h + 12;
    if ((dp == 1 || dp == 4) && h == 12) return 0;
    return h;
}
static int QWdList(QP &p, int j, int *mask) {                         // «пн, ср и пт»: забирает перечисление дней недели, возвращает индекс за ним
    int k = j, cnt = 0; *mask = 0;
    for (;;) {
        int w = QWd(p, k);
        if (w >= 0) { *mask |= 1 << w; k++; cnt++; }
        else if (cnt && QAny(p, k, L"и") && QWd(p, k + 1) >= 0) k++;
        else break;
    }
    QUse(p, j, k); return k;
}
static void QRecur(QP &p, int i, RuleSpec *r, int *mday) {           // повтор: «каждый день|понедельник|2 недели», «по пн и ср», «ежегодно», «раз в неделю»
    if (QAny(p, i, L"ежедневно|каждодневно")) { r->freq = 1; QUse(p, i, i + 1); return; }
    if (QAny(p, i, L"еженедельно")) { r->freq = 2; QUse(p, i, i + 1); return; }
    if (QAny(p, i, L"ежемесячно")) { r->freq = 3; QUse(p, i, i + 1); return; }
    if (QAny(p, i, L"ежегодно")) { r->freq = 4; QUse(p, i, i + 1); return; }
    int j = i + 1, v = 1, mask, u;
    if (QAny(p, i, L"каждый|каждую|каждое|каждые|каждого|каждой")) {
        BOOL num = QNum(p, j, &v); if (num) j++;
        if (!num && QAny(p, j, L"будний|будние|будни|рабочий|рабочие") && QAny(p, j + 1, L"день|дни")) { r->freq = 2; r->wdMask = 31; QUse(p, i, j + 2); return; }
        if (!num && QWd(p, j) >= 0) { QWdList(p, j, &mask); r->freq = 2; r->wdMask = mask; QUse(p, i, j); return; }
        if (num && QAny(p, j, L"число") && v >= 1 && v <= 31) { r->freq = 3; *mday = v; QUse(p, i, j + 1); return; }
        u = QUnit(p, j);
        if (u >= 3 && u <= 6) { r->freq = u - 2; r->interval = v < 1 ? 1 : v; QUse(p, i, j + 1); }
        return;
    }
    if (QAny(p, i, L"по")) {
        if (QAny(p, j, L"будням|будним")) { r->freq = 2; r->wdMask = 31; QUse(p, i, j + 1); }
        else if (QAny(p, j, L"выходным|выходные")) { r->freq = 2; r->wdMask = 96; QUse(p, i, j + 1); }
        else if (QWd(p, j) >= 0) { QWdList(p, j, &mask); r->freq = 2; r->wdMask = mask; QUse(p, i, j); }
        return;
    }
    if (QAny(p, i, L"раз") && QAny(p, j, L"в")) {
        j++; BOOL num = QNum(p, j, &v); if (num) j++;
        u = QUnit(p, j);
        if (u >= 3 && u <= 6) { r->freq = u - 2; r->interval = v < 1 ? 1 : v; QUse(p, i, j + 1); }
    }
}
static BOOL QEatPrev(QP &p, int i, const wchar_t *list) { if (i > 0 && QAny(p, i - 1, list)) { p.t[i - 1].used = TRUE; return TRUE; } return FALSE; }

static BOOL QuickParse(const wchar_t *text, I64 nowWall, I64 defDay, Quick *q) {
    memset(q, 0, sizeof *q); q->rule.interval = 1;
    int n = lstrlenW(text), cnt = 0;
    for (int i = 0; i < n; ) { while (i < n && text[i] <= L' ') i++; if (i >= n) break; cnt++; while (i < n && text[i] > L' ') i++; }
    if (!cnt) { q->summary = Dup(L""); return FALSE; }
    QP p; memset(&p, 0, sizeof p); p.src = text; p.lc = Dup(text); CharLowerBuffW(p.lc, n); p.now = nowWall; p.today = FloorDiv(nowWall, 86400);
    p.t = (QTok *)Alloc((cnt + 1) * sizeof(QTok));
    for (int i = 0; i < n; ) {
        while (i < n && text[i] <= L' ') i++; if (i >= n) break;
        int s = i; while (i < n && text[i] > L' ') i++;
        QTok *t = &p.t[p.n++]; t->off = s; t->len = i - s; int c = t->len;
        while (c > 1 && (text[s + c - 1] == L',' || text[s + c - 1] == L';' || text[s + c - 1] == L'!' || text[s + c - 1] == L'?' || text[s + c - 1] == L'.')) c--;
        t->clen = c;
    }
    RuleSpec &rs = q->rule; int mday = 0;
    for (int i = 0; i < p.n && !rs.freq; i++) if (!p.t[i].used) QRecur(p, i, &rs, &mday);
    if (rs.freq == 2 && !rs.wdMask) for (int i = 0; i + 1 < p.n; i++) if (QAny(p, i, L"по") && QWd(p, i + 1) >= 0) { int mask; QWdList(p, i + 1, &mask); rs.wdMask = mask; QUse(p, i, i + 1); break; }   // «каждые 2 недели по пн и ср»
    if (rs.freq) for (int i = 0; i < p.n; i++) {                        // конец повтора: «до 31.12», «10 раз»
        I64 d; int nt, v;
        if (QAny(p, i, L"до") && QDateAt(p, i + 1, &d, &nt)) { rs.endKind = 1; rs.untilDay = d; QUse(p, i, i + 1 + nt); }
        else if (QNum(p, i, &v) && v > 0 && QAny(p, i + 1, L"раз|раза")) { rs.endKind = 2; rs.count = v; QUse(p, i, i + 2); }
    }
    BOOL haveDay = FALSE, haveTime = FALSE, allDay = FALSE; I64 day = defDay, dur = 0; int tod = 0, endTod = -1;
    for (int i = 0; i + 1 < p.n; i++) if (QAny(p, i, L"весь|целый") && QAny(p, i + 1, L"день")) { allDay = TRUE; QUse(p, i, i + 2); QEatPrev(p, i, L"на"); break; }
    for (int i = 0; i < p.n; i++) {                                     // «через 2 часа|3 дня|неделю|полчаса»
        if (!QAny(p, i, L"через")) continue;
        int j = i + 1, v = 1, u; BOOL num = QNum(p, j, &v); if (num) j++;
        if (!num && QAny(p, j, L"полчаса")) { u = 1; v = 30; } else u = QUnit(p, j);
        if (!u) continue;
        QUse(p, i, j + 1);
        if (u <= 2) { I64 rel = FloorDiv(p.now, 60) * 60 + (I64)v * (u == 1 ? 60 : 3600); day = FloorDiv(rel, 86400); tod = (int)(rel - day * 86400); haveDay = haveTime = TRUE; }
        else { day = u == 3 ? p.today + v : u == 4 ? p.today + 7 * v : DayAddMonths(p.today, u == 5 ? v : 12 * v); haveDay = TRUE; }
        break;
    }
    for (int i = 0; i < p.n && !haveDay; i++) {                          // дата или день недели
        I64 d; int nt;
        if (QDateAt(p, i, &d, &nt)) { QUse(p, i, i + nt); QEatPrev(p, i, L"в|во|на"); day = d; haveDay = TRUE; }
        else if (QWd(p, i) >= 0) {
            int wd = QWd(p, i), td = Weekday(p.today); BOOL next = QAny(p, i - 1, L"следующий|следующую|следующее|след");
            day = next ? p.today - td + 7 + wd : p.today + 1 + (wd - td + 6) % 7;
            QUse(p, i, i + 1); if (next) p.t[i - 1].used = TRUE; QEatPrev(p, i - (next ? 1 : 0), L"в|во|на"); haveDay = TRUE;
        }
        if (haveDay) break;
    }
    if (!haveTime) for (int i = 0; i < p.n && !haveTime; i++) {          // время: «15:00», «14:30-16», «с 14 до 16», «в 9 вечера», «в полдень»
        int t1, t2, nt, h, h2;
        if (QTimeTok(p, i, &t1, &t2)) {
            QUse(p, i, i + 1); QEatPrev(p, i, L"в|во|с|со|к"); tod = t1; endTod = t2; haveTime = TRUE;
            if (t2 < 0 && QTimeEnd(p, i + 1, &t2, &nt)) { QUse(p, i + 1, i + 1 + nt); endTod = t2; }
        } else if (QAny(p, i, L"с|со") && QNum(p, i + 1, &h) && QAny(p, i + 2, L"до") && QNum(p, i + 3, &h2) && h <= 23 && h2 <= 23) {
            int j = i + 4; if (QAny(p, j, L"часов|часа|час|ч")) j++;
            int dp = QDayPart(p, j); if (dp) j++;
            QUse(p, i, j); tod = QHour(h, dp) * 3600; endTod = QHour(h2, dp) * 3600; haveTime = TRUE;
        } else if (QAny(p, i, L"в|во") && QAny(p, i + 1, L"полдень|полночь")) {
            tod = QAny(p, i + 1, L"полдень") ? 12 * 3600 : 0; QUse(p, i, i + 2); haveTime = TRUE;
        } else if (QAny(p, i, L"в|во") && QNum(p, i + 1, &h) && h <= 23) {
            int j = i + 2; if (QAny(p, j, L"часов|часа|час|ч")) j++;
            int dp = QDayPart(p, j); if (dp) j++;
            QUse(p, i, j); tod = QHour(h, dp) * 3600; haveTime = TRUE;
            if (QTimeEnd(p, j, &t2, &nt)) { QUse(p, j, j + nt); endTod = t2; }
        }
    }
    for (int i = 0; i < p.n; i++) {                                      // длительность: «на 2 часа», «на полчаса», «на 3 дня»
        if (!QAny(p, i, L"на")) continue;
        int j = i + 1, v = 1, u; BOOL num = QNum(p, j, &v); if (num) j++;
        if (!num && QAny(p, j, L"полчаса")) { dur = 1800; QUse(p, i, j + 1); break; }
        u = QUnit(p, j);
        if (u >= 1 && u <= 4) { dur = (I64)v * (u == 1 ? 60 : u == 2 ? 3600 : u == 3 ? 86400 : 604800); QUse(p, i, j + 1); break; }
    }
    if (rs.freq == 2 && !haveDay) {                                      // первое вхождение: ближайший подходящий день не раньше defDay
        int mask = rs.wdMask ? rs.wdMask : 1 << Weekday(defDay);
        for (day = defDay; !(mask & (1 << Weekday(day))); day++) {}
    } else if (rs.freq == 3 && mday && !haveDay) {
        int y, m, d0; CivilFromDays(defDay, &y, &m, &d0);
        for (int k = 0; k < 14; k++) {                                   // «каждое 15 число»: ближайшее 15-е не раньше defDay (месяцы без такого числа пропускаются)
            I64 mi = (I64)y * 12 + (m - 1) + k, yy = FloorDiv(mi, 12), d;
            if (MkDay((int)yy, (int)(mi - yy * 12) + 1, mday, &d) && d >= defDay) { day = d; break; }
        }
    }
    q->allDay = allDay || !haveTime;
    if (q->allDay) { I64 nd = dur >= 86400 ? dur / 86400 : 1; q->start = day * 86400; q->end = (day + nd) * 86400; }
    else {
        q->start = day * 86400 + tod;
        q->end = endTod > tod ? day * 86400 + endTod : endTod >= 0 && endTod < tod ? (day + 1) * 86400 + endTod : q->start + (dur ? dur : 3600);
    }
    Buf b = {0};
    for (int i = 0; i < p.n; i++) if (!p.t[i].used) { if (b.n) BufAdd(&b, L" "); BufAdd(&b, p.src + p.t[i].off, p.t[i].len); }
    q->summary = b.p ? b.p : Dup(L""); if (q->summary[0]) CharUpperBuffW(q->summary, 1);
    Free(p.lc); Free(p.t); return TRUE;
}

} // namespace Cal
#endif
