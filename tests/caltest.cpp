// Проверка ядра календаря (src\calendar.h): отдельная программа с CRT. Запуск: tests\run.cmd
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")

// ---- то, что в dkeys.cpp даёт хозяин (копии оттуда) ----
static HANDLE g_heap;
struct Buf { wchar_t *p; int n, cap; };
static void *Alloc(SIZE_T n) { return HeapAlloc(g_heap, HEAP_ZERO_MEMORY, n ? n : 1); }
static void *Realloc(void *p, SIZE_T n) { return p ? HeapReAlloc(g_heap, HEAP_ZERO_MEMORY, p, n) : Alloc(n); }
static void Free(void *p) { if (p) HeapFree(g_heap, 0, p); }
static wchar_t *DupN(const wchar_t *s, int n) { wchar_t *p = (wchar_t *)Alloc((n + 1) * sizeof(wchar_t)); memcpy(p, s, n * sizeof(wchar_t)); return p; }
static wchar_t *Dup(const wchar_t *s) { if (!s) s = L""; return DupN(s, lstrlenW(s)); }
static void BufAdd(Buf *b, const wchar_t *s, int n = -1) {
    if (n < 0) n = lstrlenW(s);
    if (b->n + n + 1 > b->cap) { int c = b->cap ? b->cap : 256; while (c < b->n + n + 1) c *= 2; b->p = (wchar_t *)Realloc(b->p, c * sizeof(wchar_t)); b->cap = c; }
    memcpy(b->p + b->n, s, n * sizeof(wchar_t)); b->n += n; b->p[b->n] = 0;
}
static int ToInt(const wchar_t *s) {
    while (*s == L' ') s++;
    int sg = 1; if (*s == L'-') { sg = -1; s++; }
    int v = 0; while (*s >= L'0' && *s <= L'9') v = v * 10 + (*s++ - L'0');
    return v * sg;
}
static wchar_t *FromUtf8(const char *s, int n) {
    if (n >= 3 && (BYTE)s[0] == 0xEF && (BYTE)s[1] == 0xBB && (BYTE)s[2] == 0xBF) { s += 3; n -= 3; }
    int w = n > 0 ? MultiByteToWideChar(CP_UTF8, 0, s, n, NULL, 0) : 0;
    wchar_t *p = (wchar_t *)Alloc((w + 1) * sizeof(wchar_t));
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, n, p, w);
    return p;
}
static char *ToUtf8(const wchar_t *s, int *len) {
    int n = lstrlenW(s);
    int b = n ? WideCharToMultiByte(CP_UTF8, 0, s, n, NULL, 0, NULL, NULL) : 0;
    char *p = (char *)Alloc(b + 1);
    if (b) WideCharToMultiByte(CP_UTF8, 0, s, n, p, b, NULL, NULL);
    *len = b; return p;
}
static BYTE *ReadAll(const wchar_t *path, DWORD *size) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD sz = GetFileSize(h, NULL), got = 0;
    BYTE *p = (BYTE *)Alloc(sz + 2);
    if (!ReadFile(h, p, sz, &got, NULL)) got = 0;
    CloseHandle(h); *size = got; return p;
}
static BOOL WriteAtomic(const wchar_t *path, const void *data, DWORD size) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD w = 0; BOOL ok = WriteFile(h, data, size, &w, NULL) && w == size; CloseHandle(h); return ok;
}
#include "../src/calendar.h"

using namespace Cal;
static int g_tests, g_fail;
#define CHECK(c) do { g_tests++; if (!(c)) { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static I64 W(int y, int mo, int d, int h = 0, int mi = 0, int s = 0) { Tm t = {y, mo, d, h, mi, s}; return TmToSec(t); }
static wchar_t *Fold(const wchar_t *s) { return Dup(s); }

// Разобрать ICS-кусок с одним событием и вернуть вхождения за [from, to) (UTC)
static void Run(const wchar_t *vevent, I64 from, I64 to, Store *st, OccList *occ) {
    Buf b = {0};
    BufAdd(&b, L"BEGIN:VCALENDAR\r\nVERSION:2.0\r\n"); BufAdd(&b, vevent); BufAdd(&b, L"END:VCALENDAR\r\n");
    memset(st, 0, sizeof *st); Parse(st, b.p, L"t"); Free(b.p);
    Query(st, from, to, occ);
}
static int DayOf(I64 utcSec) { Tm t = SecToTm(utcSec); return t.d; }
static int MonOf(I64 utcSec) { Tm t = SecToTm(utcSec); return t.mo; }

static const wchar_t *kSample =
    L"BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Google Inc//Google Calendar 70.9054//EN\r\n"
    L"BEGIN:VTIMEZONE\r\nTZID:Europe/Moscow\r\nBEGIN:STANDARD\r\nTZOFFSETFROM:+0300\r\nTZOFFSETTO:+0300\r\nTZNAME:MSK\r\nDTSTART:19700101T000000\r\nEND:STANDARD\r\nEND:VTIMEZONE\r\n"
    L"BEGIN:VEVENT\r\nDTSTART;TZID=Europe/Moscow:20261102T100000\r\nDTEND;TZID=Europe/Moscow:20261102T110000\r\n"
    L"RRULE:FREQ=WEEKLY;BYDAY=MO,WE\r\nEXDATE;TZID=Europe/Moscow:20261109T100000\r\nDTSTAMP:20261101T120000Z\r\nUID:weekly1@google.com\r\nSEQUENCE:2\r\n"
    L"SUMMARY:Планёрка\\, отдел\r\n"
    L"DESCRIPTION:Строка 1\\nСтрока 2 с очень длинным текстом чтобы\r\n"
    L"  проверить складывание строк в ICS - длинное описание длинное описание\r\n"
    L"LOCATION:Офис\\; кабинет 5\r\n"
    L"ATTENDEE;CN=\"Иван: старший\":mailto:ivan@example.com\r\nORGANIZER:mailto:me@example.com\r\nSTATUS:CONFIRMED\r\n"
    L"BEGIN:VALARM\r\nACTION:DISPLAY\r\nDESCRIPTION:r\r\nTRIGGER:-PT10M\r\nEND:VALARM\r\nEND:VEVENT\r\n"
    L"BEGIN:VEVENT\r\nDTSTART;TZID=Europe/Moscow:20261111T150000\r\nDTEND;TZID=Europe/Moscow:20261111T160000\r\nRECURRENCE-ID;TZID=Europe/Moscow:20261111T100000\r\n"
    L"DTSTAMP:20261101T120000Z\r\nUID:weekly1@google.com\r\nSUMMARY:override\r\nEND:VEVENT\r\n"
    L"BEGIN:VEVENT\r\nDTSTART;VALUE=DATE:20261120\r\nDTEND;VALUE=DATE:20261123\r\nDTSTAMP:20261101T120000Z\r\nSUMMARY:allday\r\nUID:allday1\r\nTRANSP:TRANSPARENT\r\nEND:VEVENT\r\n"
    L"BEGIN:VEVENT\r\nDTSTART:20261116T070000Z\r\nDURATION:PT1H30M\r\nDTSTAMP:20261101T120000Z\r\nUID:utc1\r\nSUMMARY:utc\r\n"
    L"BEGIN:VALARM\r\nACTION:EMAIL\r\nTRIGGER:-PT1H\r\nSUMMARY:mail\r\nEND:VALARM\r\n"
    L"BEGIN:VALARM\r\nACTION:AUDIO\r\nTRIGGER:-P1D\r\nEND:VALARM\r\nEND:VEVENT\r\n"
    L"END:VCALENDAR\r\n";

static void TestDates(void) {
    for (I64 d = -20000; d < 80000; d += 37) { int y, m, dd; CivilFromDays(d, &y, &m, &dd); CHECK(DaysFromCivil(y, m, dd) == d); }
    CHECK(Weekday(DaysFromCivil(1970, 1, 1)) == 3);
    CHECK(Weekday(DaysFromCivil(2026, 11, 16)) == 0);
    CHECK(DaysInMonth(2024, 2) == 29 && DaysInMonth(2026, 2) == 28 && DaysInMonth(1900, 2) == 28 && DaysInMonth(2000, 2) == 29);
    CHECK(W(1970, 1, 1) == 0 && W(2000, 1, 1) == 946684800LL);
    Tm t = SecToTm(W(2026, 11, 16, 13, 45, 59)); CHECK(t.y == 2026 && t.mo == 11 && t.d == 16 && t.h == 13 && t.mi == 45 && t.s == 59);
    t = SecToTm(-1); CHECK(t.y == 1969 && t.mo == 12 && t.d == 31 && t.h == 23 && t.mi == 59 && t.s == 59);
    I64 now = NowUtc(); CHECK(now > W(2025, 1, 1) && now < W(2100, 1, 1));
}
static void TestZones(void) {
    CHECK(WallToUtc(W(2026, 11, 16, 10), L"Europe/Moscow") == W(2026, 11, 16, 7));
    CHECK(UtcToWall(W(2026, 11, 16, 7), L"Europe/Moscow") == W(2026, 11, 16, 10));
    CHECK(WallToUtc(W(2026, 11, 16, 10), L"Asia/Yekaterinburg") == W(2026, 11, 16, 5));
    CHECK(WallToUtc(W(2026, 11, 16, 10), L"America/New_York") == W(2026, 11, 16, 15));
    CHECK(WallToUtc(W(2026, 7, 15, 10), L"America/New_York") == W(2026, 7, 15, 14));
    CHECK(WallToUtc(W(2026, 7, 15, 10), L"Europe/Berlin") == W(2026, 7, 15, 8));
    CHECK(WallToUtc(W(2026, 11, 16, 10), L"UTC") == W(2026, 11, 16, 10));
    CHECK(WallToUtc(W(2026, 11, 16, 10), L"Russian Standard Time") == W(2026, 11, 16, 7));   // имя Windows тоже понимаем
    CHECK(UtcToWall(WallToUtc(W(2026, 3, 29, 12), L"Europe/London"), L"Europe/London") == W(2026, 3, 29, 12));
    wchar_t id[80]; LocalTzid(id, 80); printf("local tz: %S\n", id);
    I64 loc = UtcToWall(W(2026, 6, 1, 12), L""); TIME_ZONE_INFORMATION z; GetTimeZoneInformation(&z); (void)z; CHECK(loc != 0);
}
static void TestParse(void) {
    Store st; memset(&st, 0, sizeof st);
    CHECK(Parse(&st, kSample, L"google"));
    CHECK(st.n == 4);
    Event *e = &st.ev[0];
    CHECK(!lstrcmpW(e->uid, L"weekly1@google.com") && e->seq == 2 && !e->allDay && !e->hasRec);
    CHECK(!lstrcmpW(e->summary, L"Планёрка, отдел"));
    CHECK(!lstrcmpW(e->loc, L"Офис; кабинет 5"));
    CHECK(e->desc && e->desc[7] == L'1' && e->desc[8] == L'\n' && wcsstr(e->desc, L"длинное описание") != NULL);
    CHECK(wcsstr(e->desc, L"чтобы проверить") != NULL);     // сложенная строка склеена: пробел после сгиба убран, второй остался
    CHECK(!lstrcmpW(e->tzid, L"Europe/Moscow") && e->start == W(2026, 11, 2, 10) && e->end == W(2026, 11, 2, 11));
    CHECK(e->nal == 1 && e->alarms[0] == 10 && e->nexd == 1 && e->exd[0] == W(2026, 11, 9, 10));
    CHECK(e->extra && wcsstr(e->extra, L"ATTENDEE;CN=\"") && wcsstr(e->extra, L"ORGANIZER:mailto:me@example.com") && wcsstr(e->extra, L"STATUS:CONFIRMED"));
    CHECK(!lstrcmpW(e->rrule, L"FREQ=WEEKLY;BYDAY=MO,WE") && !lstrcmpW(e->source, L"google"));
    Event *o = &st.ev[1];
    CHECK(o->hasRec && o->recId == W(2026, 11, 11, 10) && o->start == W(2026, 11, 11, 15) && !lstrcmpW(o->uid, e->uid));
    Event *a = &st.ev[2];
    CHECK(a->allDay && a->tzid == NULL && a->start == W(2026, 11, 20) && a->end == W(2026, 11, 23) && a->extra && wcsstr(a->extra, L"TRANSP:TRANSPARENT"));
    Event *u = &st.ev[3];
    CHECK(!lstrcmpW(u->tzid, L"UTC") && u->start == W(2026, 11, 16, 7) && u->end == W(2026, 11, 16, 8, 30));
    CHECK(u->nal == 1 && u->alarms[0] == 1440);                                        // AUDIO -P1D разобрано, EMAIL осталось сырым блоком
    CHECK(u->extra && wcsstr(u->extra, L"BEGIN:VALARM") && wcsstr(u->extra, L"ACTION:EMAIL") && wcsstr(u->extra, L"END:VALARM"));

    // запись и повторное чтение дают то же самое
    wchar_t *s1 = Serialize(&st, NULL);
    Store st2; memset(&st2, 0, sizeof st2); Parse(&st2, s1, L"google");
    wchar_t *s2 = Serialize(&st2, NULL);
    CHECK(st2.n == 4 && !lstrcmpW(s1, s2));
    CHECK(!lstrcmpW(st2.ev[0].summary, e->summary) && !lstrcmpW(st2.ev[0].desc, e->desc) && st2.ev[2].allDay && st2.ev[1].hasRec);
    // ни одна строка не длиннее 75 байт UTF-8 и сгибы начинаются с пробела
    int maxLen = 0; BOOL badFold = FALSE;
    for (const wchar_t *p = s1; *p; ) {
        const wchar_t *q = p; while (*q && *q != L'\n') q++;
        int n = (int)(q - p); wchar_t *ln = DupN(p, n); if (n && ln[n - 1] == L'\r') ln[n - 1] = 0;
        int ub; char *u8 = ToUtf8(ln, &ub); if (ub > maxLen) maxLen = ub; Free(u8); Free(ln);
        p = *q ? q + 1 : q;
    }
    CHECK(maxLen <= 75 && !badFold);
    CHECK(wcsstr(s1, L"\r\n ") != NULL);                                                // описание длинное - складывается
    CHECK(wcsstr(s1, L"DTSTART;TZID=Europe/Moscow:20261102T100000") && wcsstr(s1, L"DTSTART;VALUE=DATE:20261120") && wcsstr(s1, L"DTSTART:20261116T070000Z"));
    CHECK(wcsstr(s1, L"RECURRENCE-ID;TZID=Europe/Moscow:20261111T100000") && wcsstr(s1, L"TRIGGER:-PT10M") && wcsstr(s1, L"TRIGGER:-P1D"));
    // фильтр по источнику
    wchar_t *s3 = Serialize(&st, L"nothing"); CHECK(wcsstr(s3, L"BEGIN:VEVENT") == NULL); Free(s3);
    Free(s1); Free(s2); StoreFree(&st2);

    // вхождения: ноябрь 2026
    OccList occ; Query(&st, W(2026, 11, 1), W(2026, 12, 1), &occ);
    CHECK(occ.n == 10);
    BOOL sorted = TRUE; for (int i = 1; i < occ.n; i++) if (occ.a[i].s < occ.a[i - 1].s) sorted = FALSE;
    CHECK(sorted);
    int weekly = 0, overrides = 0; BOOL has9 = FALSE, has11master = FALSE;
    for (int i = 0; i < occ.n; i++) {
        if (occ.a[i].ev == 0) { weekly++; if (DayOf(occ.a[i].s) == 9) has9 = TRUE; if (DayOf(occ.a[i].s) == 11) has11master = TRUE; CHECK(occ.a[i].e - occ.a[i].s == 3600); }
        if (occ.a[i].ev == 1) { overrides++; CHECK(occ.a[i].s == W(2026, 11, 11, 12)); }
    }
    CHECK(weekly == 7 && overrides == 1 && !has9 && !has11master);
    int firstAll = -1; for (int i = 0; i < occ.n; i++) if (occ.a[i].allDay) firstAll = i;
    CHECK(firstAll >= 0 && occ.a[firstAll].e - occ.a[firstAll].s >= 3 * 86400 - 3600);
    OccFree(&occ);
    // скрытое и удалённое не показываются
    st.ev[2].hidden = TRUE; st.ev[3].deleted = TRUE; Query(&st, W(2026, 11, 1), W(2026, 12, 1), &occ); CHECK(occ.n == 8); OccFree(&occ);
    StoreFree(&st);
}
static void TestRules(void) {
    Store st; OccList o;
    // daily COUNT
    Run(L"BEGIN:VEVENT\r\nUID:a\r\nDTSTART:20260101T090000Z\r\nRRULE:FREQ=DAILY;COUNT=5\r\nEND:VEVENT\r\n", W(2025, 1, 1), W(2030, 1, 1), &st, &o);
    CHECK(o.n == 5 && o.a[4].s == W(2026, 1, 5, 9)); OccFree(&o); StoreFree(&st);
    // daily INTERVAL + UNTIL
    Run(L"BEGIN:VEVENT\r\nUID:a\r\nDTSTART:20260101T090000Z\r\nRRULE:FREQ=DAILY;INTERVAL=3;UNTIL=20260131T000000Z\r\nEND:VEVENT\r\n", W(2025, 1, 1), W(2030, 1, 1), &st, &o);
    CHECK(o.n == 10 && DayOf(o.a[9].s) == 28); OccFree(&o); StoreFree(&st);
    // monthly last Friday
    Run(L"BEGIN:VEVENT\r\nUID:a\r\nDTSTART:20260130T090000Z\r\nRRULE:FREQ=MONTHLY;BYDAY=-1FR\r\nEND:VEVENT\r\n", W(2026, 1, 1), W(2027, 1, 1), &st, &o);
    CHECK(o.n == 12 && DayOf(o.a[1].s) == 27 && DayOf(o.a[2].s) == 27 && DayOf(o.a[3].s) == 24); OccFree(&o); StoreFree(&st);
    // monthly second Tuesday
    Run(L"BEGIN:VEVENT\r\nUID:a\r\nDTSTART:20260113T090000Z\r\nRRULE:FREQ=MONTHLY;BYDAY=2TU\r\nEND:VEVENT\r\n", W(2026, 1, 1), W(2027, 1, 1), &st, &o);
    CHECK(o.n == 12 && DayOf(o.a[1].s) == 10 && DayOf(o.a[11].s) == 8); OccFree(&o); StoreFree(&st);
    // monthly day 31: только месяцы, где он есть
    Run(L"BEGIN:VEVENT\r\nUID:a\r\nDTSTART:20260131T090000Z\r\nRRULE:FREQ=MONTHLY;BYMONTHDAY=31\r\nEND:VEVENT\r\n", W(2026, 1, 1), W(2027, 1, 1), &st, &o);
    CHECK(o.n == 7); OccFree(&o); StoreFree(&st);
    // monthly последний день (-1)
    Run(L"BEGIN:VEVENT\r\nUID:a\r\nDTSTART:20260131T090000Z\r\nRRULE:FREQ=MONTHLY;BYMONTHDAY=-1\r\nEND:VEVENT\r\n", W(2026, 1, 1), W(2027, 1, 1), &st, &o);
    CHECK(o.n == 12 && DayOf(o.a[1].s) == 28 && DayOf(o.a[3].s) == 30); OccFree(&o); StoreFree(&st);
    // yearly 29 февраля
    Run(L"BEGIN:VEVENT\r\nUID:a\r\nDTSTART:20240229T090000Z\r\nRRULE:FREQ=YEARLY\r\nEND:VEVENT\r\n", W(2024, 1, 1), W(2034, 1, 1), &st, &o);
    CHECK(o.n == 3); OccFree(&o); StoreFree(&st);
    // последний рабочий день месяца: BYSETPOS
    Run(L"BEGIN:VEVENT\r\nUID:a\r\nDTSTART:20260130T090000Z\r\nRRULE:FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;BYSETPOS=-1\r\nEND:VEVENT\r\n", W(2026, 1, 1), W(2027, 1, 1), &st, &o);
    CHECK(o.n == 12 && DayOf(o.a[2].s) == 31 && DayOf(o.a[4].s) == 29 && DayOf(o.a[7].s) == 31); OccFree(&o); StoreFree(&st);
    // каждые 2 недели по вт и чт, 6 раз
    Run(L"BEGIN:VEVENT\r\nUID:a\r\nDTSTART:20260106T090000Z\r\nRRULE:FREQ=WEEKLY;INTERVAL=2;BYDAY=TU,TH;COUNT=6\r\nEND:VEVENT\r\n", W(2026, 1, 1), W(2027, 1, 1), &st, &o);
    CHECK(o.n == 6 && MonOf(o.a[5].s) == 2 && DayOf(o.a[5].s) == 5 && DayOf(o.a[2].s) == 20); OccFree(&o); StoreFree(&st);
    // еженедельно со стартом в середине недели: пн/ср, старт в ср - понедельник той же недели не считается
    Run(L"BEGIN:VEVENT\r\nUID:a\r\nDTSTART:20260107T090000Z\r\nRRULE:FREQ=WEEKLY;BYDAY=MO,WE;COUNT=3\r\nEND:VEVENT\r\n", W(2026, 1, 1), W(2027, 1, 1), &st, &o);
    CHECK(o.n == 3 && DayOf(o.a[0].s) == 7 && DayOf(o.a[1].s) == 12 && DayOf(o.a[2].s) == 14); OccFree(&o); StoreFree(&st);
    // зимнее/летнее время: 10:00 в Нью-Йорке остаётся 10:00 по стене
    Run(L"BEGIN:VEVENT\r\nUID:a\r\nDTSTART;TZID=America/New_York:20261026T100000\r\nDTEND;TZID=America/New_York:20261026T110000\r\nRRULE:FREQ=WEEKLY;COUNT=3\r\nEND:VEVENT\r\n", W(2026, 1, 1), W(2027, 1, 1), &st, &o);
    CHECK(o.n == 3 && o.a[0].s == W(2026, 10, 26, 14) && o.a[1].s == W(2026, 11, 2, 15) && o.a[2].s == W(2026, 11, 9, 15) && o.a[2].e - o.a[2].s == 3600); OccFree(&o); StoreFree(&st);
    // давний бесконечный повтор, запрос на неделю: быстро и точно
    Run(L"BEGIN:VEVENT\r\nUID:a\r\nDTSTART:20100101T090000Z\r\nRRULE:FREQ=DAILY\r\nEND:VEVENT\r\n", W(2026, 11, 1), W(2026, 11, 8), &st, &o);
    CHECK(o.n == 7); OccFree(&o); StoreFree(&st);
    Run(L"BEGIN:VEVENT\r\nUID:a\r\nDTSTART:20100101T090000Z\r\nRRULE:FREQ=MONTHLY;INTERVAL=5;BYMONTHDAY=3\r\nEND:VEVENT\r\n", W(2026, 1, 1), W(2027, 1, 1), &st, &o);
    CHECK(o.n >= 2 && o.n <= 3); OccFree(&o); StoreFree(&st);
    // день рождения: целодневное ежегодное
    Run(L"BEGIN:VEVENT\r\nUID:a\r\nDTSTART;VALUE=DATE:20000315\r\nRRULE:FREQ=YEARLY\r\nEND:VEVENT\r\n", W(2026, 1, 1), W(2027, 1, 1), &st, &o);
    CHECK(o.n == 1 && o.a[0].allDay && DayOf(UtcToWall(o.a[0].s, L"")) == 15); OccFree(&o); StoreFree(&st);
    // событие, начавшееся до диапазона, но идущее в нём
    Run(L"BEGIN:VEVENT\r\nUID:a\r\nDTSTART:20261116T230000Z\r\nDTEND:20261117T020000Z\r\nEND:VEVENT\r\n", W(2026, 11, 17), W(2026, 11, 18), &st, &o);
    CHECK(o.n == 1); OccFree(&o); StoreFree(&st);
    Run(L"BEGIN:VEVENT\r\nUID:a\r\nDTSTART:20261116T230000Z\r\nDTEND:20261117T020000Z\r\nEND:VEVENT\r\n", W(2026, 11, 17, 2), W(2026, 11, 18), &st, &o);
    CHECK(o.n == 0); OccFree(&o); StoreFree(&st);
    // UNTIL целой датой включает последний день
    Run(L"BEGIN:VEVENT\r\nUID:a\r\nDTSTART;VALUE=DATE:20261101\r\nRRULE:FREQ=DAILY;UNTIL=20261103\r\nEND:VEVENT\r\n", W(2026, 10, 1), W(2026, 12, 1), &st, &o);
    CHECK(o.n == 3); OccFree(&o); StoreFree(&st);
    // EXDATE не уменьшает COUNT
    Run(L"BEGIN:VEVENT\r\nUID:a\r\nDTSTART:20260101T090000Z\r\nRRULE:FREQ=DAILY;COUNT=5\r\nEXDATE:20260102T090000Z\r\nEND:VEVENT\r\n", W(2026, 1, 1), W(2027, 1, 1), &st, &o);
    CHECK(o.n == 4 && DayOf(o.a[1].s) == 3 && DayOf(o.a[3].s) == 5); OccFree(&o); StoreFree(&st);
}
static void TestStoreFile(void) {
    Store st; memset(&st, 0, sizeof st);
    Event *e = StoreNew(&st); wchar_t u[80]; NewUid(u, 80);
    e->uid = Dup(u); e->summary = Dup(L"Встреча"); e->allDay = FALSE; e->tzid = Dup(L"Europe/Moscow");
    e->start = W(2026, 12, 1, 15); e->end = W(2026, 12, 1, 16); e->source = Dup(L"local"); AddAlarm(e, 15); AddAlarm(e, 15); AddAlarm(e, 60); Touch(e);
    CHECK(e->nal == 2 && e->seq == 1 && e->stamp > 0);
    CHECK(StoreFind(&st, u, FALSE, 0) == 0 && StoreFind(&st, L"none", FALSE, 0) == -1);
    wchar_t path[MAX_PATH]; GetTempPathW(MAX_PATH, path); lstrcatW(path, L"dkeys_caltest\\local.ics");
    CHECK(SaveFile(&st, path, L"local"));
    Store st2; memset(&st2, 0, sizeof st2);
    CHECK(LoadFile(&st2, path, L"local") && st2.n == 1);
    CHECK(!lstrcmpW(st2.ev[0].summary, e->summary) && st2.ev[0].start == e->start && st2.ev[0].nal == 2 && st2.ev[0].alarms[1] == 60 && st2.ev[0].seq == 1);
    StoreRemove(&st2, 0); CHECK(st2.n == 0);
    StoreFree(&st); StoreFree(&st2);
    DeleteFileW(path);
}

int main() {
    g_heap = GetProcessHeap(); CoInitialize(NULL);
    TestDates(); TestZones(); TestParse(); TestRules(); TestStoreFile();
    printf("%d checks, %d failed\n", g_tests, g_fail);
    return g_fail ? 1 : 0;
}
