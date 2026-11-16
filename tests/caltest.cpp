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
#include "../src/calview.h"
#include "../src/calsync.h"

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

// ---- вид «месяц» (src\calview.h): сетка 6x7, номера недель, диапазон запроса, события по дням ----
static BOOL ShowNotX(const Event *e) { return !e->source || lstrcmpW(e->source, L"x"); }
static int DayCnt(const CalView::DayLists &dl, I64 first, int y, int m, int d) { int i = (int)(DaysFromCivil(y, m, d) - first); return dl.off[i + 1] - dl.off[i]; }
static const Occ &DayOcc(const CalView::DayLists &dl, const OccList &ol, I64 first, int y, int m, int d, int k) { int i = (int)(DaysFromCivil(y, m, d) - first); return ol.a[dl.items[dl.off[i] + k]]; }
static void TestView(void) {
    using namespace CalView;
    CHECK(GridStart(2026, 11, 0) == DaysFromCivil(2026, 10, 26) && GridStart(2026, 11, 1) == DaysFromCivil(2026, 11, 1));      // 1 ноября 2026 - воскресенье
    CHECK(GridStart(2027, 2, 0) == DaysFromCivil(2027, 2, 1) && GridStart(2027, 2, 1) == DaysFromCivil(2027, 1, 31));         // 1 февраля 2027 - понедельник
    for (int ws = 0; ws < 2; ws++) for (int y = 2024; y <= 2028; y++) for (int m = 1; m <= 12; m++) {                           // месяц всегда целиком в сетке из 6 недель
        I64 g = GridStart(y, m, ws), f = MonthFirst(y, m);
        CHECK(g <= f && f - g < 7 && Weekday(g) == (ws ? 6 : 0) && f + DaysInMonth(y, m) - 1 < g + GRID);
    }
    CHECK(IsoWeek(DaysFromCivil(2026, 1, 1)) == 1 && IsoWeek(DaysFromCivil(2026, 12, 31)) == 53 && IsoWeek(DaysFromCivil(2021, 1, 3)) == 53);
    CHECK(IsoWeek(DaysFromCivil(2024, 12, 30)) == 1 && IsoWeek(DaysFromCivil(2026, 11, 16)) == 47 && IsoWeek(DaysFromCivil(2026, 11, 22)) == 47 && IsoWeek(DaysFromCivil(2026, 11, 23)) == 48);
    CHECK(RowWeek(GridStart(2026, 11, 0), 0) == 44 && RowWeek(GridStart(2026, 11, 1), 1) == 45 && RowWeek(GridStart(2026, 11, 1) + 7, 1) == 46);     // с воскресенья - по четвергу строки
    int y = 2026, m = 12; AddMonths(&y, &m, 1); CHECK(y == 2027 && m == 1);
    y = 2026; m = 1; AddMonths(&y, &m, -1); CHECK(y == 2025 && m == 12);
    y = 2026; m = 11; AddMonths(&y, &m, -23); CHECK(y == 2024 && m == 12);
    CHECK(ShiftMonths(DaysFromCivil(2026, 1, 31), 1) == DaysFromCivil(2026, 2, 28) && ShiftMonths(DaysFromCivil(2028, 1, 31), 1) == DaysFromCivil(2028, 2, 29));
    CHECK(ShiftMonths(DaysFromCivil(2026, 3, 31), -1) == DaysFromCivil(2026, 2, 28) && ShiftMonths(DaysFromCivil(2026, 11, 16), 12) == DaysFromCivil(2027, 11, 16));
    I64 first = GridStart(2026, 11, 0), from, to; Range(first, GRID, &from, &to);
    CHECK(from == WallToUtc(first * 86400, L"") && to - from >= GRID * 86400LL - 7200 && to - from <= GRID * 86400LL + 7200);
    CHECK(TodayLocal() == FloorDiv(UtcToWall(NowUtc(), L""), 86400));

    const wchar_t *ics = L"BEGIN:VCALENDAR\r\n"
        L"BEGIN:VEVENT\r\nUID:trip\r\nDTSTART;VALUE=DATE:20261120\r\nDTEND;VALUE=DATE:20261123\r\nSUMMARY:trip\r\nEND:VEVENT\r\n"
        L"BEGIN:VEVENT\r\nUID:timed\r\nDTSTART:20261118T100000\r\nDTEND:20261118T110000\r\nSUMMARY:timed\r\nEND:VEVENT\r\n"
        L"BEGIN:VEVENT\r\nUID:oneday\r\nDTSTART;VALUE=DATE:20261118\r\nSUMMARY:oneday\r\nEND:VEVENT\r\n"
        L"BEGIN:VEVENT\r\nUID:night\r\nDTSTART:20261124T220000\r\nDTEND:20261125T020000\r\nSUMMARY:night\r\nEND:VEVENT\r\n"
        L"BEGIN:VEVENT\r\nUID:weekly\r\nDTSTART:20261005T090000\r\nDTEND:20261005T100000\r\nRRULE:FREQ=WEEKLY;BYDAY=MO\r\nSUMMARY:weekly\r\nEND:VEVENT\r\n"
        L"BEGIN:VEVENT\r\nUID:edge\r\nDTSTART;VALUE=DATE:20261024\r\nDTEND;VALUE=DATE:20261028\r\nSUMMARY:edge\r\nEND:VEVENT\r\n"
        L"BEGIN:VEVENT\r\nUID:after\r\nDTSTART:20261207T100000\r\nDTEND:20261207T110000\r\nSUMMARY:after\r\nEND:VEVENT\r\n"
        L"BEGIN:VEVENT\r\nUID:hid\r\nDTSTART:20261118T120000\r\nDTEND:20261118T130000\r\nSUMMARY:hid\r\nEND:VEVENT\r\n"
        L"END:VCALENDAR\r\n";
    Store st; memset(&st, 0, sizeof st); CHECK(Parse(&st, ics, L"t") && st.n == 8);
    for (int i = 0; i < st.n; i++) if (!lstrcmpW(st.ev[i].uid, L"hid")) SetStr(&st.ev[i].source, L"x");
    OccList ol; Query(&st, from, to, &ol);
    CHECK(ol.n == 12);                                                           // trip, timed, oneday, night, weekly x6, edge, hid; after - вне сетки
    DayLists dl; BuildDays(&st, &ol, first, ShowNotX, &dl);
    CHECK(dl.off[GRID] == 15);                                                   // записей по дням: trip 3 + timed + oneday + night 2 + weekly 6 + edge 2; hid скрыт по источнику
    CHECK(DayCnt(dl, first, 2026, 11, 18) == 2 && DayOcc(dl, ol, first, 2026, 11, 18, 0).allDay && !DayOcc(dl, ol, first, 2026, 11, 18, 1).allDay);    // целодневное раньше
    CHECK(DayCnt(dl, first, 2026, 11, 20) == 1 && DayCnt(dl, first, 2026, 11, 21) == 1 && DayCnt(dl, first, 2026, 11, 22) == 1 && DayCnt(dl, first, 2026, 11, 23) == 1);   // trip 20-22 (конец не включается) + weekly 23-го
    CHECK(DayOcc(dl, ol, first, 2026, 11, 23, 0).allDay == FALSE && DayOcc(dl, ol, first, 2026, 11, 22, 0).allDay);
    CHECK(DayCnt(dl, first, 2026, 11, 24) == 1 && DayCnt(dl, first, 2026, 11, 25) == 1 && DayCnt(dl, first, 2026, 11, 26) == 0);    // ночное событие на обеих датах
    CHECK(DayCnt(dl, first, 2026, 10, 26) == 2 && DayCnt(dl, first, 2026, 10, 27) == 1 && DayCnt(dl, first, 2026, 10, 28) == 0);    // edge обрезан границей сетки
    CHECK(DayCnt(dl, first, 2026, 11, 2) == 1 && DayCnt(dl, first, 2026, 11, 16) == 1 && DayCnt(dl, first, 2026, 11, 30) == 1 && DayCnt(dl, first, 2026, 12, 6) == 0);
    DaysFree(&dl); BuildDays(&st, &ol, first, NULL, &dl); CHECK(dl.off[GRID] == 16); DaysFree(&dl);
    OccFree(&ol); StoreFree(&st);

    Store ds; memset(&ds, 0, sizeof ds); DemoFill(&ds, DaysFromCivil(2026, 11, 16)); CHECK(ds.n == 14);
    Query(&ds, from, to, &ol); CHECK(ol.n >= 14); BuildDays(&ds, &ol, first, NULL, &dl);
    CHECK(DayCnt(dl, first, 2026, 11, 19) == 8 && DayOcc(dl, ol, first, 2026, 11, 19, 0).allDay && !DayOcc(dl, ol, first, 2026, 11, 19, 1).allDay);     // отпуск + 6 встреч + обед
    CHECK(DayCnt(dl, first, 2026, 11, 20) == 3 && DayOcc(dl, ol, first, 2026, 11, 20, 1).allDay && !DayOcc(dl, ol, first, 2026, 11, 20, 2).allDay);   // отпуск, день рождения, обед
    CHECK(DayCnt(dl, first, 2026, 11, 22) == 2 && DayCnt(dl, first, 2026, 11, 23) == 2);     // 22-го: последний обед (COUNT=10 с 13 ноября) и начало ночной смены; 23-го (понедельник): планёрка и конец смены
    DaysFree(&dl); OccFree(&ol); StoreFree(&ds);
}

// ---- этап 3: правка (клон, перенос между источниками, удаление/скрытие) ----
static Event *AddEv(Store *st, const wchar_t *uid, const wchar_t *src, const wchar_t *sum, I64 start) {
    Event *e = StoreNew(st); e->uid = Dup(uid); e->source = Dup(src); e->summary = Dup(sum); e->tzid = Dup(L"UTC"); e->start = start; e->end = start + 3600; return e;
}
static int CountSrc(const Store &st, const wchar_t *src, BOOL del) { int n = 0; for (int i = 0; i < st.n; i++) if (!lstrcmpW(st.ev[i].source, src) && st.ev[i].deleted == del) n++; return n; }
static int Visible(const Store &st, BOOL showHidden) { OccList o; Query(&st, W(2026, 1, 1), W(2027, 1, 1), &o, showHidden); int n = o.n; OccFree(&o); return n; }
static void TestEdit(void) {
    Store st; memset(&st, 0, sizeof st);
    Event *a = AddEv(&st, L"a", L"local", L"Серия", W(2026, 11, 2, 10)); a->rrule = Dup(L"FREQ=WEEKLY;COUNT=3"); AddAlarm(a, 10); AddExd(a, W(2026, 11, 9, 10)); a->extra = Dup(L"X-FOO:bar\n");
    Event *o = AddEv(&st, L"a", L"local", L"Замена", W(2026, 11, 16, 12)); o->hasRec = TRUE; o->recId = W(2026, 11, 16, 10);
    AddEv(&st, L"b", L"local", L"Одиночное", W(2026, 11, 20, 9));
    CHECK(st.n == 3 && Visible(st, FALSE) == 3);                                           // серия (2 ноября; 9-го - EXDATE; 16-е заменено) + замена + «b»
    // клон независим от оригинала
    Event c; EventClone(&c, &st.ev[0]); SetStr(&c.summary, L"Другое"); c.alarms[0] = 99; c.exd[0] = 1; c.extra[0] = L'Y';
    CHECK(!lstrcmpW(st.ev[0].summary, L"Серия") && st.ev[0].alarms[0] == 10 && st.ev[0].exd[0] == W(2026, 11, 9, 10) && st.ev[0].extra[0] == L'X' && c.nal == 1 && c.nexd == 1 && !lstrcmpW(c.rrule, st.ev[0].rrule));
    EventFree(&c);
    // дублирование = перенос в тот же источник с копией
    int d = MoveEvent(&st, 2, L"local", TRUE, FALSE);
    CHECK(st.n == 4 && d == 3 && lstrcmpW(st.ev[d].uid, L"b") && !lstrcmpW(st.ev[d].source, L"local") && !st.ev[2].deleted && st.ev[d].seq == 1 && st.ev[d].stamp > 0);
    // перенос серии в другой источник с синхронизацией старого: новый UID, замена переезжает вместе, в старом - надгробия
    st.ev[0].etag = Dup(L"E1"); st.ev[0].hidden = TRUE;
    int m = MoveEvent(&st, 0, L"google", FALSE, TRUE);
    CHECK(st.n == 6 && m == 4 && CountSrc(st, L"google", FALSE) == 2 && CountSrc(st, L"local", TRUE) == 2);
    CHECK(!st.ev[m].etag && !st.ev[m].hidden && !st.ev[m].deleted && lstrcmpW(st.ev[m].uid, L"a") && !lstrcmpW(st.ev[m].summary, L"Серия") && !lstrcmpW(st.ev[m].rrule, L"FREQ=WEEKLY;COUNT=3"));
    CHECK(!lstrcmpW(st.ev[5].uid, st.ev[m].uid) && st.ev[5].hasRec && st.ev[5].recId == W(2026, 11, 16, 10) && !lstrcmpW(st.ev[5].summary, L"Замена"));
    CHECK(st.ev[0].deleted && st.ev[1].deleted && st.ev[0].seq == 1);
    CHECK(Visible(st, FALSE) == 4);                                                         // новая серия (2 ноября и замена) + «b» и его копия
    { wchar_t *t = Serialize(&st, L"local"); Store s2; memset(&s2, 0, sizeof s2); Parse(&s2, t, L"local"); Free(t);               // надгробие переживает запись в файл
      CHECK(s2.n == 4 && s2.ev[StoreFind(&s2, L"a", FALSE, 0)].deleted && !s2.ev[StoreFind(&s2, L"b", FALSE, 0)].deleted); StoreFree(&s2); }
    // перенос без синхронизации прежнего: прежнее сразу убирается; «оставить копию» - остаётся
    int m2 = MoveEvent(&st, m, L"apple", FALSE, FALSE);
    CHECK(st.n == 6 && CountSrc(st, L"google", FALSE) == 0 && CountSrc(st, L"apple", FALSE) == 2 && !lstrcmpW(st.ev[m2].source, L"apple"));
    int m3 = MoveEvent(&st, m2, L"google", TRUE, FALSE);
    CHECK(st.n == 8 && CountSrc(st, L"google", FALSE) == 2 && CountSrc(st, L"apple", FALSE) == 2 && lstrcmpW(st.ev[m3].uid, st.ev[m2].uid));
    // удаление
    int b = StoreFind(&st, L"b", FALSE, 0);
    int vis = Visible(st, FALSE);
    DeleteEvent(&st, b, FALSE, TRUE);                                                       // «только из DKeys»: остаётся в файле, не показывается
    CHECK(st.ev[b].hidden && !st.ev[b].deleted && Visible(st, FALSE) == vis - 1 && Visible(st, TRUE) == vis);
    { wchar_t *t = Serialize(&st, L"local"); Store s2; memset(&s2, 0, sizeof s2); Parse(&s2, t, L"local"); Free(t); CHECK(s2.ev[StoreFind(&s2, L"b", FALSE, 0)].hidden); StoreFree(&s2); }
    SetHidden(&st, b, FALSE); CHECK(!st.ev[b].hidden && Visible(st, FALSE) == vis);
    DeleteEvent(&st, b, TRUE, TRUE);                                                        // «везде» при синхронизации - надгробие
    CHECK(st.ev[b].deleted && st.ev[b].seq == 1 && Visible(st, FALSE) == vis - 1 && Visible(st, TRUE) == vis - 1);
    int n0 = st.n; DeleteEvent(&st, StoreFind(&st, L"a", FALSE, 0), TRUE, FALSE);          // «везде» без синхронизации - сразу из хранилища, вместе с заменой
    CHECK(st.n == n0 - 2 && StoreFind(&st, L"a", FALSE, 0) == -1);
    StoreFree(&st);
}

// ---- правило повтора из полей и обратно ----
static void TestRuleSpec(void) {
    wchar_t r[200]; RuleSpec s; memset(&s, 0, sizeof s); s.interval = 1;
    s.freq = 1; RuleBuild(s, FALSE, L"", r, 200); CHECK(!lstrcmpW(r, L"FREQ=DAILY"));
    s.freq = 0; RuleBuild(s, FALSE, L"", r, 200); CHECK(!r[0]);
    s.freq = 2; s.interval = 2; s.wdMask = 1 | 4; RuleBuild(s, FALSE, L"", r, 200); CHECK(!lstrcmpW(r, L"FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,WE"));
    s.wdMask = 64 | 32; s.interval = 1; s.endKind = 2; s.count = 5; RuleBuild(s, FALSE, L"", r, 200); CHECK(!lstrcmpW(r, L"FREQ=WEEKLY;BYDAY=SA,SU;COUNT=5"));
    s.freq = 3; s.wdMask = 5; s.endKind = 1; s.untilDay = DaysFromCivil(2026, 12, 31);
    RuleBuild(s, TRUE, NULL, r, 200); CHECK(!lstrcmpW(r, L"FREQ=MONTHLY;UNTIL=20261231"));                    // BYDAY только у недельного
    RuleBuild(s, FALSE, L"", r, 200); CHECK(!lstrcmpW(r, L"FREQ=MONTHLY;UNTIL=20261231T235959"));
    RuleBuild(s, FALSE, L"UTC", r, 200); CHECK(!lstrcmpW(r, L"FREQ=MONTHLY;UNTIL=20261231T235959Z"));
    RuleBuild(s, FALSE, L"Europe/Moscow", r, 200); CHECK(!lstrcmpW(r, L"FREQ=MONTHLY;UNTIL=20261231T205959Z"));
    s.endKind = 2; s.count = 0; RuleBuild(s, FALSE, L"", r, 200); CHECK(!lstrcmpW(r, L"FREQ=MONTHLY"));                  // COUNT=0 не пишется
    // разбор: простые правила воспроизводятся, сложные - нет
    Event e; memset(&e, 0, sizeof e); e.tzid = (wchar_t *)L"Europe/Moscow"; RuleSpec o;
    CHECK(RuleSpecParse(NULL, &e, &o) && o.freq == 0 && RuleSpecParse(L"", &e, &o) && o.freq == 0);
    CHECK(RuleSpecParse(L"FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,WE;COUNT=3", &e, &o) && o.freq == 2 && o.interval == 2 && o.wdMask == 5 && o.endKind == 2 && o.count == 3);
    RuleBuild(o, FALSE, L"Europe/Moscow", r, 200); CHECK(!lstrcmpW(r, L"FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,WE;COUNT=3"));
    CHECK(RuleSpecParse(L"FREQ=WEEKLY;WKST=MO;BYDAY=FR;UNTIL=20261231T205959Z", &e, &o) && o.freq == 2 && o.wdMask == 16 && o.endKind == 1 && o.untilDay == DaysFromCivil(2026, 12, 31));
    RuleBuild(o, FALSE, L"Europe/Moscow", r, 200); CHECK(!lstrcmpW(r, L"FREQ=WEEKLY;BYDAY=FR;UNTIL=20261231T205959Z"));
    CHECK(RuleSpecParse(L"FREQ=YEARLY", &e, &o) && o.freq == 4 && o.interval == 1 && o.endKind == 0);
    CHECK(RuleSpecParse(L"FREQ=DAILY;UNTIL=20261105", &e, &o) && o.endKind == 1 && o.untilDay == DaysFromCivil(2026, 11, 5));
    CHECK(!RuleSpecParse(L"FREQ=MONTHLY;BYDAY=2TU", &e, &o) && !RuleSpecParse(L"FREQ=MONTHLY;BYMONTHDAY=15", &e, &o) && !RuleSpecParse(L"FREQ=YEARLY;BYMONTH=3", &e, &o));
    CHECK(!RuleSpecParse(L"FREQ=HOURLY", &e, &o) && !RuleSpecParse(L"FREQ=DAILY;COUNT=2;UNTIL=20261231", &e, &o) && !RuleSpecParse(L"FREQ=DAILY;BYDAY=MO", &e, &o) && !RuleSpecParse(L"FREQ=DAILY;BYHOUR=9", &e, &o));
}

// ---- даты и время в полях ввода ----
static void TestFields(void) {
    I64 base = DaysFromCivil(2026, 11, 16), d; int t;
    CHECK(ParseDayStr(L"20.11.2026", base, &d) && d == DaysFromCivil(2026, 11, 20));
    CHECK(ParseDayStr(L"  5.1.27 ", base, &d) && d == DaysFromCivil(2027, 1, 5));
    CHECK(ParseDayStr(L"20/11/2026", base, &d) && d == DaysFromCivil(2026, 11, 20) && ParseDayStr(L"2026-11-20", base, &d) && d == DaysFromCivil(2026, 11, 20));
    CHECK(ParseDayStr(L"20.11", base, &d) && d == DaysFromCivil(2026, 11, 20) && ParseDayStr(L"1.1", base, &d) && d == DaysFromCivil(2027, 1, 1) && ParseDayStr(L"16.11", base, &d) && d == base);
    CHECK(ParseDayStr(L"29.2", base, &d) && d == DaysFromCivil(2028, 2, 29) && !ParseDayStr(L"29.02.2027", base, &d));
    CHECK(!ParseDayStr(L"32.01.2026", base, &d) && !ParseDayStr(L"20.13.2026", base, &d) && !ParseDayStr(L"abc", base, &d) && !ParseDayStr(L"", base, &d) && !ParseDayStr(L"20.11.20260", base, &d) && !ParseDayStr(L"20.11.202", base, &d));
    CHECK(ParseTodStr(L"9:05", &t) && t == 9 * 3600 + 300 && ParseTodStr(L"09:05", &t) && t == 9 * 3600 + 300 && ParseTodStr(L" 23:59 ", &t) && t == 86340 && ParseTodStr(L"7", &t) && t == 7 * 3600 && ParseTodStr(L"0:00", &t) && t == 0);
    CHECK(!ParseTodStr(L"24:00", &t) && !ParseTodStr(L"12:60", &t) && !ParseTodStr(L"12:5", &t) && !ParseTodStr(L"", &t) && !ParseTodStr(L"12:30:00", &t) && !ParseTodStr(L"ab", &t));
    wchar_t o[24]; FmtDay(o, DaysFromCivil(2026, 1, 5)); CHECK(!lstrcmpW(o, L"05.01.2026")); FmtTod(o, 9 * 3600 + 300); CHECK(!lstrcmpW(o, L"09:05"));
    CHECK(DayAddMonths(DaysFromCivil(2026, 1, 31), 1) == DaysFromCivil(2026, 2, 28) && DayAddMonths(DaysFromCivil(2026, 11, 16), -12) == DaysFromCivil(2025, 11, 16));
}

// ---- быстрый ввод ----
// «сейчас» в тестах - понедельник 16 ноября 2026, 12:00; defDay (выбранный день) - то же число
static BOOL QRunAt(const wchar_t *t, I64 now, Quick *q) { return QuickParse(t, now, FloorDiv(now, 86400), q); }
static BOOL QRun(const wchar_t *t, Quick *q) { return QRunAt(t, W(2026, 11, 16, 12), q); }
static BOOL QTimed(const Quick &q, I64 s, I64 e) { return !q.allDay && q.start == s && q.end == e; }
static BOOL QDay(const Quick &q, int y, int m, int d, int n = 1) { return q.allDay && q.start == W(y, m, d) && q.end == W(y, m, d) + n * 86400LL; }
static BOOL QTitle(const Quick &q, const wchar_t *s) { return !lstrcmpW(q.summary, s); }
static void TestQuick(void) {
    Quick q; wchar_t r[100];
    CHECK(QRun(L"завтра 15:00 встреча", &q) && QTimed(q, W(2026, 11, 17, 15), W(2026, 11, 17, 16)) && QTitle(q, L"Встреча") && !q.rule.freq); Free(q.summary);
    CHECK(QRun(L"20.11 14:30-16 планёрка", &q) && QTimed(q, W(2026, 11, 20, 14, 30), W(2026, 11, 20, 16)) && QTitle(q, L"Планёрка")); Free(q.summary);
    CHECK(QRun(L"каждый понедельник 10:00 летучка", &q) && QTimed(q, W(2026, 11, 16, 10), W(2026, 11, 16, 11)) && QTitle(q, L"Летучка") && q.rule.freq == 2 && q.rule.wdMask == 1 && q.rule.interval == 1);
    RuleBuild(q.rule, FALSE, L"", r, 100); CHECK(!lstrcmpW(r, L"FREQ=WEEKLY;BYDAY=MO")); Free(q.summary);
    CHECK(QRun(L"через 2 часа позвонить", &q) && QTimed(q, W(2026, 11, 16, 14), W(2026, 11, 16, 15)) && QTitle(q, L"Позвонить")); Free(q.summary);
    CHECK(QRun(L"в пятницу весь день отпуск", &q) && QDay(q, 2026, 11, 20) && QTitle(q, L"Отпуск")); Free(q.summary);
    // даты: слова, формы, год, прошедшие даты
    CHECK(QRun(L"сегодня обед", &q) && QDay(q, 2026, 11, 16) && QTitle(q, L"Обед")); Free(q.summary);
    CHECK(QRun(L"послезавтра обед", &q) && QDay(q, 2026, 11, 18)); Free(q.summary);
    CHECK(QRun(L"вчера обед", &q) && QDay(q, 2026, 11, 15)); Free(q.summary);
    CHECK(QRun(L"5 декабря 18:00 ужин", &q) && QTimed(q, W(2026, 12, 5, 18), W(2026, 12, 5, 19)) && QTitle(q, L"Ужин")); Free(q.summary);
    CHECK(QRun(L"1 января отпуск", &q) && QDay(q, 2027, 1, 1)); Free(q.summary);                                // без года - ближайшая будущая дата
    CHECK(QRun(L"16 ноября отчёт", &q) && QDay(q, 2026, 11, 16)); Free(q.summary);                               // сегодняшняя дата остаётся в этом году
    CHECK(QRun(L"15 ноября отчёт", &q) && QDay(q, 2027, 11, 15)); Free(q.summary);
    CHECK(QRun(L"3 мар 2028 отчёт", &q) && QDay(q, 2028, 3, 3) && QTitle(q, L"Отчёт")); Free(q.summary);
    CHECK(QRun(L"31.12.2026 салют", &q) && QDay(q, 2026, 12, 31) && QRun(L"2026-12-31 салют", &q) && QDay(q, 2026, 12, 31)); Free(q.summary);
    CHECK(QRun(L"29.02 високосный", &q) && QDay(q, 2028, 2, 29)); Free(q.summary);
    CHECK(QRun(L"30.02 что-то", &q) && QTitle(q, L"30.02 что-то") && QDay(q, 2026, 11, 16)); Free(q.summary);        // несуществующая дата остаётся в названии
    CHECK(QRun(L"на 20.11 планёрка", &q) && QDay(q, 2026, 11, 20) && QTitle(q, L"Планёрка")); Free(q.summary);
    // дни недели: ближайший после сегодня; «следующий» - на следующей неделе
    CHECK(QRun(L"в среду звонок", &q) && QDay(q, 2026, 11, 18)); Free(q.summary);
    CHECK(QRun(L"понедельник звонок", &q) && QDay(q, 2026, 11, 23)); Free(q.summary);                           // сегодня понедельник - следующий
    CHECK(QRun(L"в пт 18:00 пиво", &q) && QTimed(q, W(2026, 11, 20, 18), W(2026, 11, 20, 19)) && QTitle(q, L"Пиво")); Free(q.summary);
    CHECK(QRun(L"в следующий вторник планёрка", &q) && QDay(q, 2026, 11, 24) && QTitle(q, L"Планёрка")); Free(q.summary);
    CHECK(QRun(L"в следующую пятницу планёрка", &q) && QDay(q, 2026, 11, 27)); Free(q.summary);
    CHECK(QRun(L"в воскресенье отдых", &q) && QDay(q, 2026, 11, 22) && QRun(L"в субботу отдых", &q) && QDay(q, 2026, 11, 21)); Free(q.summary);
    // время: 12/24 часа, части суток, диапазоны, пересечение полуночи
    CHECK(QRun(L"сегодня в 9 вечера ужин", &q) && QTimed(q, W(2026, 11, 16, 21), W(2026, 11, 16, 22)) && QTitle(q, L"Ужин")); Free(q.summary);
    CHECK(QRun(L"в 9 утра зарядка", &q) && QTimed(q, W(2026, 11, 16, 9), W(2026, 11, 16, 10))); Free(q.summary);
    CHECK(QRun(L"в 3 дня обед", &q) && q.start == W(2026, 11, 16, 15) && QRun(L"в 12 дня обед", &q) && q.start == W(2026, 11, 16, 12)); Free(q.summary);
    CHECK(QRun(L"в 12 ночи салют", &q) && q.start == W(2026, 11, 16, 0) && QRun(L"в 2 часа ночи сон", &q) && q.start == W(2026, 11, 16, 2)); Free(q.summary);
    CHECK(QRun(L"в 15 совещание", &q) && q.start == W(2026, 11, 16, 15) && QTitle(q, L"Совещание") && QRun(L"в 15 часов совещание", &q) && q.start == W(2026, 11, 16, 15) && QTitle(q, L"Совещание")); Free(q.summary);
    CHECK(QRun(L"в полдень обед", &q) && q.start == W(2026, 11, 16, 12) && QRun(L"в полночь тост", &q) && q.start == W(2026, 11, 16, 0)); Free(q.summary);
    CHECK(QRun(L"22:00-02:00 смена", &q) && QTimed(q, W(2026, 11, 16, 22), W(2026, 11, 17, 2))); Free(q.summary);
    CHECK(QRun(L"завтра в 15:00 до 17:00 встреча", &q) && QTimed(q, W(2026, 11, 17, 15), W(2026, 11, 17, 17)) && QTitle(q, L"Встреча")); Free(q.summary);
    CHECK(QRun(L"завтра 15:00 - 16:30 встреча", &q) && QTimed(q, W(2026, 11, 17, 15), W(2026, 11, 17, 16, 30))); Free(q.summary);
    CHECK(QRun(L"завтра 15:00–16:30 встреча", &q) && QTimed(q, W(2026, 11, 17, 15), W(2026, 11, 17, 16, 30))); Free(q.summary);          // длинное тире
    CHECK(QRun(L"с 14 до 16 урок", &q) && QTimed(q, W(2026, 11, 16, 14), W(2026, 11, 16, 16)) && QTitle(q, L"Урок")); Free(q.summary);
    CHECK(QRun(L"с 2 до 4 дня урок", &q) && QTimed(q, W(2026, 11, 16, 14), W(2026, 11, 16, 16))); Free(q.summary);
    CHECK(QRun(L"завтра 9:05 врач", &q) && q.start == W(2026, 11, 17, 9, 5)); Free(q.summary);
    CHECK(QRun(L"завтра 24:00 плохо", &q) && q.allDay && QTitle(q, L"24:00 плохо")); Free(q.summary);                    // 24:00 - не время
    CHECK(QRun(L"завтра 14:30-14:30 ноль", &q) && QTimed(q, W(2026, 11, 17, 14, 30), W(2026, 11, 17, 15, 30))); Free(q.summary);
    // длительность и «через»
    CHECK(QRun(L"завтра 15:00 на 2 часа встреча", &q) && QTimed(q, W(2026, 11, 17, 15), W(2026, 11, 17, 17)) && QTitle(q, L"Встреча")); Free(q.summary);
    CHECK(QRun(L"завтра 15:00 на полчаса встреча", &q) && QTimed(q, W(2026, 11, 17, 15), W(2026, 11, 17, 15, 30))); Free(q.summary);
    CHECK(QRun(L"завтра 15:00 на 45 минут встреча", &q) && QTimed(q, W(2026, 11, 17, 15), W(2026, 11, 17, 15, 45))); Free(q.summary);
    CHECK(QRun(L"завтра на 3 дня поездка", &q) && QDay(q, 2026, 11, 17, 3) && QTitle(q, L"Поездка")); Free(q.summary);
    CHECK(QRun(L"завтра на весь день поездка", &q) && QDay(q, 2026, 11, 17) && QTitle(q, L"Поездка")); Free(q.summary);
    CHECK(QRun(L"завтра 15:00 целый день поездка", &q) && QDay(q, 2026, 11, 17)); Free(q.summary);                        // «весь день» главнее времени
    CHECK(QRun(L"через полчаса чай", &q) && QTimed(q, W(2026, 11, 16, 12, 30), W(2026, 11, 16, 13, 30))); Free(q.summary);
    CHECK(QRun(L"через час чай", &q) && q.start == W(2026, 11, 16, 13) && QRun(L"через 90 минут чай", &q) && q.start == W(2026, 11, 16, 13, 30)); Free(q.summary);
    CHECK(QRunAt(L"через 30 минут чай", W(2026, 11, 16, 23, 50, 45), &q) && QTimed(q, W(2026, 11, 17, 0, 20), W(2026, 11, 17, 1, 20))); Free(q.summary);   // через полночь, секунды отбрасываются
    CHECK(QRun(L"через неделю созвон", &q) && QDay(q, 2026, 11, 23) && QRun(L"через 3 дня 10:00 врач", &q) && QTimed(q, W(2026, 11, 19, 10), W(2026, 11, 19, 11))); Free(q.summary);
    CHECK(QRun(L"через месяц платёж", &q) && QDay(q, 2026, 12, 16) && QRun(L"через 2 года платёж", &q) && QDay(q, 2028, 11, 16)); Free(q.summary);
    CHECK(QRunAt(L"через месяц платёж", W(2026, 1, 31, 12), &q) && QDay(q, 2026, 2, 28)); Free(q.summary);                 // 31 января + месяц
    CHECK(QRun(L"через дом", &q) && QTitle(q, L"Через дом")); Free(q.summary);                                            // «через» не про время остаётся в названии
    // повторы
    CHECK(QRun(L"каждый день зарядка", &q) && q.rule.freq == 1 && q.rule.interval == 1 && QDay(q, 2026, 11, 16) && QTitle(q, L"Зарядка")); Free(q.summary);
    CHECK(QRun(L"каждую неделю 10:00 отчёт", &q) && q.rule.freq == 2 && q.rule.wdMask == 0 && q.start == W(2026, 11, 16, 10)); Free(q.summary);
    CHECK(QRun(L"каждый месяц оплата", &q) && q.rule.freq == 3 && QRun(L"каждый год отчёт", &q) && q.rule.freq == 4 && QRun(L"ежегодно 20.11 день рождения", &q) && q.rule.freq == 4 && QDay(q, 2026, 11, 20) && QTitle(q, L"День рождения")); Free(q.summary);
    CHECK(QRun(L"ежедневно 8:00 таблетка", &q) && q.rule.freq == 1 && q.start == W(2026, 11, 16, 8)); Free(q.summary);
    CHECK(QRun(L"еженедельно планёрка", &q) && q.rule.freq == 2 && QRun(L"ежемесячно оплата", &q) && q.rule.freq == 3); Free(q.summary);
    CHECK(QRun(L"каждые 2 недели по пн и ср 9:00 стендап", &q) && q.rule.freq == 2 && q.rule.interval == 2 && q.rule.wdMask == 5 && q.start == W(2026, 11, 16, 9) && QTitle(q, L"Стендап"));
    RuleBuild(q.rule, FALSE, L"", r, 100); CHECK(!lstrcmpW(r, L"FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,WE")); Free(q.summary);
    CHECK(QRun(L"каждый вторник и четверг 18:30 зал", &q) && q.rule.wdMask == 2 + 8 && q.start == W(2026, 11, 17, 18, 30) && QTitle(q, L"Зал")); Free(q.summary);   // первое вхождение - ближайший подходящий день
    CHECK(QRun(L"по понедельникам, средам и пятницам 7:00 бег", &q) && q.rule.wdMask == 1 + 4 + 16 && q.start == W(2026, 11, 16, 7) && QTitle(q, L"Бег")); Free(q.summary);
    CHECK(QRun(L"по будням 9:00 стендап", &q) && q.rule.wdMask == 31 && QRun(L"по выходным сон", &q) && q.rule.wdMask == 96 && QDay(q, 2026, 11, 21)); Free(q.summary);
    CHECK(QRun(L"каждый будний день 9:00 работа", &q) && q.rule.wdMask == 31 && QTitle(q, L"Работа")); Free(q.summary);
    CHECK(QRun(L"каждые 3 дня полив", &q) && q.rule.freq == 1 && q.rule.interval == 3 && QRun(L"раз в 2 недели уборка", &q) && q.rule.freq == 2 && q.rule.interval == 2 && QRun(L"раз в месяц уборка", &q) && q.rule.freq == 3); Free(q.summary);
    CHECK(QRun(L"каждое 15 число оплата", &q) && q.rule.freq == 3 && QDay(q, 2026, 12, 15) && QTitle(q, L"Оплата")); Free(q.summary);
    CHECK(QRun(L"каждое 20 число оплата", &q) && QDay(q, 2026, 11, 20)); Free(q.summary);
    CHECK(QRun(L"каждый день до 31.12 зарядка", &q) && q.rule.freq == 1 && q.rule.endKind == 1 && q.rule.untilDay == DaysFromCivil(2026, 12, 31) && QTitle(q, L"Зарядка"));
    RuleBuild(q.rule, TRUE, NULL, r, 100); CHECK(!lstrcmpW(r, L"FREQ=DAILY;UNTIL=20261231")); Free(q.summary);
    CHECK(QRun(L"каждый день 10 раз таблетка 8:00", &q) && q.rule.endKind == 2 && q.rule.count == 10 && q.start == W(2026, 11, 16, 8) && QTitle(q, L"Таблетка")); Free(q.summary);
    CHECK(QRun(L"каждую пятницу до 20 ноября 2027 отчёт", &q) && q.rule.endKind == 1 && q.rule.untilDay == DaysFromCivil(2027, 11, 20) && q.rule.wdMask == 16); Free(q.summary);
    CHECK(QRun(L"каждый день до 31.12 с 14 до 16 урок", &q) && q.rule.endKind == 1 && QTimed(q, W(2026, 11, 16, 14), W(2026, 11, 16, 16))); Free(q.summary);      // «до» диапазона времени - не конец повтора
    CHECK(QRun(L"каждый день в 10:00 зарядка", &q) && q.start == W(2026, 11, 16, 10) && QTitle(q, L"Зарядка")); Free(q.summary);
    CHECK(QRun(L"каждый кот", &q) && !q.rule.freq && QTitle(q, L"Каждый кот")); Free(q.summary);                           // «каждый» не про повтор
    CHECK(QRun(L"каждый день 20.11 зарядка", &q) && q.rule.freq == 1 && QDay(q, 2026, 11, 20)); Free(q.summary);          // дата начала серии
    // прочее: регистр, знаки, пустой ввод, только дата, названия с цифрами и предлогами
    CHECK(QRun(L"ЗАВТРА 15:00 ВСТРЕЧА с Иваном", &q) && QTimed(q, W(2026, 11, 17, 15), W(2026, 11, 17, 16)) && QTitle(q, L"ВСТРЕЧА с Иваном")); Free(q.summary);
    CHECK(QRun(L"завтра позвонить, маме!", &q) && QTitle(q, L"Позвонить, маме!")); Free(q.summary);
    CHECK(QRun(L"встреча с Иваном в кафе", &q) && QDay(q, 2026, 11, 16) && QTitle(q, L"Встреча с Иваном в кафе")); Free(q.summary);   // ничего не распознано: целый день, выбранный день
    CHECK(QRun(L"  завтра   15:00    встреча   ", &q) && QTimed(q, W(2026, 11, 17, 15), W(2026, 11, 17, 16)) && QTitle(q, L"Встреча")); Free(q.summary);
    CHECK(QRun(L"завтра", &q) && QDay(q, 2026, 11, 17) && QTitle(q, L"")); Free(q.summary);
    CHECK(!QRun(L"", &q) && QTitle(q, L"") && !QRun(L"   ", &q)); Free(q.summary);
    CHECK(QRun(L"в 5 раз больше", &q) && q.start == W(2026, 11, 16, 5) && QTitle(q, L"Раз больше")); Free(q.summary);      // известное ограничение: «в N» - это время
    CHECK(QRun(L"встреча 2027 года", &q) && QTitle(q, L"Встреча 2027 года") && q.allDay); Free(q.summary);
    // другой defDay (выбран день в окне): без даты событие ложится на него
    CHECK(QuickParse(L"15:00 встреча", W(2026, 11, 16, 12), DaysFromCivil(2026, 12, 3), &q) && QTimed(q, W(2026, 12, 3, 15), W(2026, 12, 3, 16))); Free(q.summary);
    CHECK(QuickParse(L"каждый понедельник планёрка", W(2026, 11, 16, 12), DaysFromCivil(2026, 12, 3), &q) && QDay(q, 2026, 12, 7)); Free(q.summary);
    // готовое событие из разобранного: сохраняется, читается и разворачивается
    Quick q2; QRun(L"каждые 2 недели по пн и ср 9:00 стендап", &q2);
    Store st; memset(&st, 0, sizeof st); Event *e = StoreNew(&st); wchar_t u[80]; NewUid(u, 80); e->uid = Dup(u); e->summary = Dup(q2.summary); e->tzid = Dup(L"UTC");
    e->start = q2.start; e->end = q2.end; RuleBuild(q2.rule, FALSE, L"UTC", r, 100); e->rrule = Dup(r);
    OccList ol; Query(&st, W(2026, 11, 16), W(2026, 12, 4), &ol);                          // 16 и 18 ноября, неделя пропущена, 30 ноября и 2 декабря
    CHECK(ol.n == 4 && DayOf(ol.a[0].s) == 16 && DayOf(ol.a[1].s) == 18 && DayOf(ol.a[2].s) == 30 && DayOf(ol.a[3].s) == 2);
    OccFree(&ol); StoreFree(&st); Free(q2.summary);
}

/* ---------- напоминания ---------- */
static void RemStore(Store *st, const wchar_t *vevents, const wchar_t *src = L"t") {
    Buf b = {0}; BufAdd(&b, L"BEGIN:VCALENDAR\r\nVERSION:2.0\r\n"); BufAdd(&b, vevents); BufAdd(&b, L"END:VCALENDAR\r\n");
    memset(st, 0, sizeof *st); Parse(st, b.p, src); Free(b.p);
}
static const wchar_t *kAl60_10 = L"BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-PT1H\r\nEND:VALARM\r\nBEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-PT10M\r\nEND:VALARM\r\n";
static wchar_t *Ev(const wchar_t *uid, const wchar_t *start, const wchar_t *alarmsMin, const wchar_t *extra = L"") {      // UTC-событие на час; alarmsMin - "10,0"
    Buf b = {0}; BufAdd(&b, L"BEGIN:VEVENT\r\nUID:"); BufAdd(&b, uid); BufAdd(&b, L"\r\nSUMMARY:"); BufAdd(&b, uid); BufAdd(&b, L"\r\nDTSTART:"); BufAdd(&b, start); BufAdd(&b, L"\r\n"); BufAdd(&b, extra);
    for (const wchar_t *p = alarmsMin; *p; ) {
        int m = 0; while (*p >= L'0' && *p <= L'9') m = m * 10 + (*p++ - L'0'); if (*p == L',') p++;
        wchar_t t[96]; wsprintfW(t, L"BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-PT%dM\r\nEND:VALARM\r\n", m); BufAdd(&b, t);
    }
    BufAdd(&b, L"END:VEVENT\r\n"); return b.p;
}
static int RPick(const Store *st, const FireList *fl, SeenList *sn, I64 now, PickList *pk, I64 grace = 120) { RemPick(st, fl, sn, now, grace, pk); return pk->n; }
static void TestReminders(void) {
    Store st; FireList fl; SeenList sn; PickList pk; const I64 AD = 9 * 3600;
    // одно событие, два напоминания: 60 и 10 минут
    { wchar_t *e = Ev(L"a1", L"20261116T150000Z", L"60,10"); RemStore(&st, e); Free(e); }
    I64 now = W(2026, 11, 16, 12); RemBuild(&st, now, AD, &fl); memset(&sn, 0, sizeof sn);
    CHECK(fl.n == 2 && RemNext(&st, &fl, &sn, now) == W(2026, 11, 16, 14));
    CHECK(RPick(&st, &fl, &sn, W(2026, 11, 16, 13, 59, 59), &pk) == 0); PickFree(&pk);
    CHECK(RPick(&st, &fl, &sn, W(2026, 11, 16, 14), &pk) == 1 && !pk.a[0].late && fl.a[pk.a[0].fire].alarm == 60); PickFree(&pk);
    CHECK(RPick(&st, &fl, &sn, W(2026, 11, 16, 14, 0, 5), &pk) == 0); PickFree(&pk);                        // уже показано
    CHECK(RemNext(&st, &fl, &sn, W(2026, 11, 16, 14, 1)) == W(2026, 11, 16, 14, 50));
    CHECK(RPick(&st, &fl, &sn, W(2026, 11, 16, 14, 50), &pk) == 1 && fl.a[pk.a[0].fire].alarm == 10); PickFree(&pk);
    CHECK(RPick(&st, &fl, &sn, W(2026, 11, 16, 15), &pk) == 0 && RemNext(&st, &fl, &sn, W(2026, 11, 16, 15)) == 0); PickFree(&pk);
    // запаздывание: граница grace
    SeenFree(&sn);
    CHECK(RPick(&st, &fl, &sn, W(2026, 11, 16, 14, 0, 0) + 120, &pk) == 1 && !pk.a[0].late); PickFree(&pk);
    SeenFree(&sn);
    CHECK(RPick(&st, &fl, &sn, W(2026, 11, 16, 14, 0, 0) + 121, &pk) == 1 && pk.a[0].late); PickFree(&pk);
    // оба срока прошли (сон): показывается одно, позднее (10 мин), второе отмечено тихо
    SeenFree(&sn);
    CHECK(RPick(&st, &fl, &sn, W(2026, 11, 16, 14, 58), &pk) == 1 && fl.a[pk.a[0].fire].alarm == 10 && pk.a[0].late); PickFree(&pk);
    CHECK(sn.n == 2 && RPick(&st, &fl, &sn, W(2026, 11, 16, 14, 59), &pk) == 0); PickFree(&pk);
    SeenFree(&sn);                                                                                      // событие началось давно - поздно
    CHECK(RPick(&st, &fl, &sn, W(2026, 11, 16, 15, 50), &pk) == 1 && pk.a[0].late); PickFree(&pk);       // началось 50 минут назад - ещё показываем
    SeenFree(&sn);
    CHECK(RPick(&st, &fl, &sn, W(2026, 11, 16, 16, 0, 1), &pk) == 0 && sn.n == 0); PickFree(&pk);        // больше часа
    FireFree(&fl); StoreFree(&st);
    // несколько пропущенных после сна: по порядку начала, устаревшие и будущие не берутся
    { Buf b = {0}; const wchar_t *s[5] = {L"20261116T100500Z", L"20261116T100000Z", L"20261116T101000Z", L"20261116T080000Z", L"20261116T110000Z"}, *al[5] = {L"0", L"0", L"0", L"0", L"5"}, *id[5] = {L"b", L"a", L"c", L"old", L"future"};
      for (int i = 0; i < 5; i++) { wchar_t *e = Ev(id[i], s[i], al[i]); BufAdd(&b, e); Free(e); }
      RemStore(&st, b.p); Free(b.p); }
    RemBuild(&st, W(2026, 11, 16, 9), AD, &fl); memset(&sn, 0, sizeof sn);
    CHECK(RPick(&st, &fl, &sn, W(2026, 11, 16, 10, 20), &pk) == 3 && pk.a[0].late && pk.a[2].late);
    CHECK(!lstrcmpW(st.ev[fl.a[pk.a[0].fire].ev].uid, L"a") && !lstrcmpW(st.ev[fl.a[pk.a[1].fire].ev].uid, L"b") && !lstrcmpW(st.ev[fl.a[pk.a[2].fire].ev].uid, L"c")); PickFree(&pk);
    CHECK(RemNext(&st, &fl, &sn, W(2026, 11, 16, 10, 20)) == W(2026, 11, 16, 10, 55));
    SeenFree(&sn); FireFree(&fl); StoreFree(&st);
    // дубль: то же событие в двух источниках и дважды в одном
    { wchar_t *e = Ev(L"d1", L"20261116T150000Z", L"10"); Buf b = {0}; BufAdd(&b, e); BufAdd(&b, e); Free(e); RemStore(&st, b.p, L"x");
      Buf c = {0}; BufAdd(&c, L"BEGIN:VCALENDAR\r\n"); BufAdd(&c, b.p); BufAdd(&c, L"END:VCALENDAR\r\n"); Parse(&st, c.p, L"y"); Free(c.p); Free(b.p); }
    RemBuild(&st, W(2026, 11, 16, 12), AD, &fl); memset(&sn, 0, sizeof sn);
    CHECK(fl.n == 4 && RPick(&st, &fl, &sn, W(2026, 11, 16, 14, 50), &pk) == 1 && sn.n == 1); PickFree(&pk);
    SeenFree(&sn); FireFree(&fl); StoreFree(&st);
    // отложенное: до срока, в срок, после; журнал переживает перезапуск
    { wchar_t *e = Ev(L"s|1", L"20261116T150000Z", L"10"); RemStore(&st, e); Free(e); }              // "|" в uid
    RemBuild(&st, W(2026, 11, 16, 12), AD, &fl); memset(&sn, 0, sizeof sn);
    CHECK(RPick(&st, &fl, &sn, W(2026, 11, 16, 14, 50), &pk) == 1); PickFree(&pk);
    SeenSnooze(&sn, L"s|1", W(2026, 11, 16, 15), 10, W(2026, 11, 16, 14, 55));
    CHECK(RemNext(&st, &fl, &sn, W(2026, 11, 16, 14, 51)) == W(2026, 11, 16, 14, 55));
    { wchar_t *f = SeenSave(&sn, FALSE), *z = SeenSave(&sn, TRUE); SeenList sn2; memset(&sn2, 0, sizeof sn2); SeenLoad(&sn2, f, FALSE); SeenLoad(&sn2, z, TRUE);
      CHECK(sn2.n == 1 && sn2.a[0].until == W(2026, 11, 16, 14, 55) && sn2.a[0].alarm == 10 && sn2.a[0].occ == W(2026, 11, 16, 15) && !lstrcmpW(sn2.a[0].uid, L"s|1"));
      CHECK(RPick(&st, &fl, &sn2, W(2026, 11, 16, 14, 54), &pk) == 0); PickFree(&pk);
      CHECK(RPick(&st, &fl, &sn2, W(2026, 11, 16, 14, 55), &pk) == 1 && !pk.a[0].late && sn2.a[0].until == 0); PickFree(&pk);
      CHECK(RPick(&st, &fl, &sn2, W(2026, 11, 16, 14, 56), &pk) == 0); PickFree(&pk);
      SeenFree(&sn2);
      memset(&sn2, 0, sizeof sn2); SeenLoad(&sn2, L"x|1|5\r\nбез полей\r\n|5|5\r\nпусто||\n", FALSE); CHECK(sn2.n == 1 && sn2.a[0].occ == 1 && sn2.a[0].alarm == 5); SeenFree(&sn2);      // мусор пропускается
      Free(f); Free(z); }
    SeenSnooze(&sn, L"s|1", W(2026, 11, 16, 15), 10, W(2026, 11, 16, 15));                              // «до начала»
    CHECK(RPick(&st, &fl, &sn, W(2026, 11, 16, 14, 59), &pk) == 0); PickFree(&pk);
    CHECK(RPick(&st, &fl, &sn, W(2026, 11, 16, 15), &pk) == 1 && !pk.a[0].late); PickFree(&pk);
    SeenSnooze(&sn, L"s|1", W(2026, 11, 16, 15), 10, W(2026, 11, 16, 15, 5));                           // долго спали: отложенное устарело
    CHECK(RPick(&st, &fl, &sn, W(2026, 11, 16, 17), &pk) == 0 && sn.a[0].until == 0); PickFree(&pk);
    SeenSnooze(&sn, L"s|1", W(2026, 11, 16, 15), 10, W(2026, 11, 16, 15, 5));                           // событие удалили: отложенное снимается
    FireList fl0; memset(&fl0, 0, sizeof fl0);
    CHECK(RPick(&st, &fl0, &sn, W(2026, 11, 16, 15, 6), &pk) == 0 && sn.a[0].until == 0); PickFree(&pk);
    // событие перенесли: новое время - новый ключ, напоминание показывается снова
    FireFree(&fl); StoreFree(&st);
    { wchar_t *e = Ev(L"s|1", L"20261116T160000Z", L"10"); RemStore(&st, e); Free(e); }
    RemBuild(&st, W(2026, 11, 16, 15, 1), AD, &fl);
    CHECK(RPick(&st, &fl, &sn, W(2026, 11, 16, 15, 50), &pk) == 1); PickFree(&pk);
    // чистка журнала: старше трёх суток от начала вхождения
    SeenMark(&sn, L"old", W(2026, 11, 10, 10), 5);
    CHECK(SeenPrune(&sn, W(2026, 11, 16, 15)) && SeenFind(&sn, L"old", W(2026, 11, 10, 10), 5) < 0 && SeenFind(&sn, L"s|1", W(2026, 11, 16, 16), 10) >= 0 && !SeenPrune(&sn, W(2026, 11, 16, 15)));
    SeenFree(&sn); FireFree(&fl); StoreFree(&st);
    // повтор каждый день 09:00 с заменой 18-го (на 11:00), исключением 19-го и напоминанием за 15 минут
    { const wchar_t *al = L"BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-PT15M\r\nEND:VALARM\r\n";
      Buf b = {0}; BufAdd(&b, L"BEGIN:VEVENT\r\nUID:r1\r\nDTSTART:20261110T090000Z\r\nDTEND:20261110T093000Z\r\nRRULE:FREQ=DAILY\r\nEXDATE:20261119T090000Z\r\n"); BufAdd(&b, al); BufAdd(&b, L"END:VEVENT\r\n");
      BufAdd(&b, L"BEGIN:VEVENT\r\nUID:r1\r\nDTSTART:20261118T110000Z\r\nDTEND:20261118T113000Z\r\nRECURRENCE-ID:20261118T090000Z\r\n"); BufAdd(&b, al); BufAdd(&b, L"END:VEVENT\r\n");
      RemStore(&st, b.p); Free(b.p); }
    RemBuild(&st, W(2026, 11, 16), AD, &fl);                                                          // 15..23 ноября без 19-го, 18-е в 11:00
    CHECK(fl.n == 8);
    { BOOL has1118_11 = FALSE, has1118_09 = FALSE, has1119 = FALSE, has1116 = FALSE;
      for (int i = 0; i < fl.n; i++) {
          if (fl.a[i].occ == W(2026, 11, 18, 11)) has1118_11 = fl.a[i].due == W(2026, 11, 18, 10, 45);
          if (fl.a[i].occ == W(2026, 11, 18, 9)) has1118_09 = TRUE;
          if (fl.a[i].occ == W(2026, 11, 19, 9)) has1119 = TRUE;
          if (fl.a[i].occ == W(2026, 11, 16, 9)) has1116 = fl.a[i].due == W(2026, 11, 16, 8, 45);
      }
      CHECK(has1118_11 && !has1118_09 && !has1119 && has1116); }
    memset(&sn, 0, sizeof sn);                                                                        // запуск программы в 09:00: пропущено напоминание 08:45 сегодняшнего вхождения (15-го уже вчера - устарело)
    CHECK(RPick(&st, &fl, &sn, W(2026, 11, 16, 9), &pk) == 1 && pk.a[0].late && fl.a[pk.a[0].fire].occ == W(2026, 11, 16, 9)); PickFree(&pk);
    SeenFree(&sn); FireFree(&fl); StoreFree(&st);
    // целодневное: от 09:00 местного (напоминания кратные суткам), прочие - от полуночи
    RemStore(&st, L"BEGIN:VEVENT\r\nUID:ad\r\nDTSTART;VALUE=DATE:20261117\r\n"
                  L"BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:PT0S\r\nEND:VALARM\r\nBEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-P1D\r\nEND:VALARM\r\nBEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-PT10H\r\nEND:VALARM\r\nEND:VEVENT\r\n");
    I64 mid = WallToUtc(W(2026, 11, 17), L""), nowL = WallToUtc(W(2026, 11, 15, 12), L"");
    RemBuild(&st, nowL, AD, &fl); CHECK(fl.n == 3);
    { BOOL a0 = FALSE, a1 = FALSE, a600 = FALSE;
      for (int i = 0; i < fl.n; i++) {
          if (fl.a[i].alarm == 0) a0 = fl.a[i].due == WallToUtc(W(2026, 11, 17, 9), L"") && fl.a[i].allDay && fl.a[i].occ == mid;
          if (fl.a[i].alarm == 1440) a1 = fl.a[i].due == WallToUtc(W(2026, 11, 16, 9), L"");
          if (fl.a[i].alarm == 600) a600 = fl.a[i].due == mid - 600 * 60;
      }
      CHECK(a0 && a1 && a600); }
    FireFree(&fl); RemBuild(&st, nowL, 0, &fl);                                                       // AllDayTime=00:00: ровно от полуночи
    { BOOL ok = TRUE; for (int i = 0; i < fl.n; i++) ok = ok && fl.a[i].due == mid - (I64)fl.a[i].alarm * 60; CHECK(ok); }
    FireFree(&fl); RemBuild(&st, nowL, AD, &fl); memset(&sn, 0, sizeof sn);
    CHECK(RPick(&st, &fl, &sn, WallToUtc(W(2026, 11, 17, 20), L""), &pk) == 1 && pk.a[0].late); PickFree(&pk);                 // целодневное: сутки не устаревает
    SeenFree(&sn);
    CHECK(RPick(&st, &fl, &sn, WallToUtc(W(2026, 11, 18, 0, 30), L""), &pk) == 0); PickFree(&pk);                              // а на следующий день уже поздно
    SeenFree(&sn); FireFree(&fl); StoreFree(&st);
    // пояс и переход на зимнее время (Нью-Йорк, 1 ноября 2026 в 02:00): напоминание за час - от истинного начала вхождения
    RemStore(&st, L"BEGIN:VEVENT\r\nUID:ny\r\nDTSTART;TZID=America/New_York:20261030T090000\r\nDTEND;TZID=America/New_York:20261030T100000\r\nRRULE:FREQ=DAILY;COUNT=5\r\nBEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-PT1H\r\nEND:VALARM\r\nEND:VEVENT\r\n");
    RemBuild(&st, W(2026, 10, 30), AD, &fl); CHECK(fl.n == 5);
    { I64 want[5] = {W(2026, 10, 30, 12), W(2026, 10, 31, 12), W(2026, 11, 1, 13), W(2026, 11, 2, 13), W(2026, 11, 3, 13)}; BOOL ok = TRUE;
      for (int i = 0; i < 5; i++) { BOOL f = FALSE; for (int k = 0; k < fl.n; k++) if (fl.a[k].due == want[i]) f = TRUE; ok = ok && f; }
      CHECK(ok); }
    FireFree(&fl); StoreFree(&st);
    RemStore(&st, L"BEGIN:VEVENT\r\nUID:ny2\r\nDTSTART;TZID=America/New_York:20261101T090000\r\nDTEND;TZID=America/New_York:20261101T100000\r\nBEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-P1D\r\nEND:VALARM\r\nEND:VEVENT\r\n");
    RemBuild(&st, W(2026, 10, 30), AD, &fl); CHECK(fl.n == 1 && fl.a[0].occ == W(2026, 11, 1, 14) && fl.a[0].due == W(2026, 10, 31, 14)); FireFree(&fl); StoreFree(&st);   // ровно 24 часа
    // переход через полночь: 00:10 и напоминание за 30 минут - накануне 23:40
    { wchar_t *e = Ev(L"m1", L"20261117T001000Z", L"30"); RemStore(&st, e); Free(e); }
    RemBuild(&st, W(2026, 11, 16, 20), AD, &fl); memset(&sn, 0, sizeof sn);
    CHECK(fl.n == 1 && fl.a[0].due == W(2026, 11, 16, 23, 40));
    CHECK(RPick(&st, &fl, &sn, W(2026, 11, 16, 23, 39, 59), &pk) == 0); PickFree(&pk);
    CHECK(RPick(&st, &fl, &sn, W(2026, 11, 16, 23, 40), &pk) == 1 && !pk.a[0].late); PickFree(&pk);
    SeenFree(&sn); FireFree(&fl); StoreFree(&st);
    // скрытое, удалённое и отменённое не напоминает; без напоминаний - тоже
    { wchar_t *a = Ev(L"h", L"20261116T150000Z", L"10", L"X-DKEYS-HIDDEN:1\r\n"), *b = Ev(L"x", L"20261116T150000Z", L"10", L"X-DKEYS-DELETED:1\r\n"), *c = Ev(L"c", L"20261116T150000Z", L"10", L"STATUS:CANCELLED\r\n"), *d = Ev(L"n", L"20261116T150000Z", L"");
      Buf t = {0}; BufAdd(&t, a); BufAdd(&t, b); BufAdd(&t, c); BufAdd(&t, d); Free(a); Free(b); Free(c); Free(d); RemStore(&st, t.p); Free(t.p); }
    RemBuild(&st, W(2026, 11, 16, 12), AD, &fl); CHECK(st.n == 4 && fl.n == 0); FireFree(&fl); StoreFree(&st);
    // напоминание за 14 суток: окно расширяется, пропущенное за 4 дня показывается; событие дальше окна - нет
    { wchar_t *a = Ev(L"far", L"20261126T100000Z", L"20160"), *b = Ev(L"farther", L"20261226T100000Z", L"1440"); Buf t = {0}; BufAdd(&t, a); BufAdd(&t, b); Free(a); Free(b); RemStore(&st, t.p); Free(t.p); }
    RemBuild(&st, W(2026, 11, 16, 12), AD, &fl); memset(&sn, 0, sizeof sn);
    CHECK(fl.n == 1 && fl.a[0].due == W(2026, 11, 12, 10) && RPick(&st, &fl, &sn, W(2026, 11, 16, 12), &pk) == 1 && pk.a[0].late); PickFree(&pk);
    SeenFree(&sn); FireFree(&fl); StoreFree(&st);
}

static void TestWeekLogic() {
    using namespace CalView;
    CHECK(SnapSec(0) == 0 && SnapSec(449) == 0 && SnapSec(450) == 900 && SnapSec(86400 + 1000) == 86400 + 900 && SnapSec(-440) == 0 && SnapSec(-460) == -900);
    Blk b[5] = {{600, 660, 0, 0}, {630, 700, 0, 0}, {640, 650, 0, 0}, {700, 760, 0, 0}, {900, 960, 0, 0}};
    LayoutBlocks(b, 5);
    CHECK(b[0].col == 0 && b[1].col == 1 && b[2].col == 2 && b[0].ncol == 3 && b[1].ncol == 3 && b[2].ncol == 3);
    CHECK(b[3].col == 0 && b[3].ncol == 1 && b[4].col == 0 && b[4].ncol == 1);
    I64 ns, ne, D = 20000LL * 86400;
    DragApply(0, D + 36000, D + 39600, 4000, FALSE, &ns, &ne); CHECK(ns == D + 39600 && ne == ns + 3600);
    DragApply(0, D + 36000, D + 39600, 86400 + 100, TRUE, &ns, &ne); CHECK(ns == D + 36000 + 0 && ne == ns + 3600);
    DragApply(0, D + 36000, D + 39600, 86400 + 3600, FALSE, &ns, &ne); CHECK(ns == D + 86400 + 39600);
    DragApply(1, D + 36000, D + 39600, 1000, FALSE, &ns, &ne); CHECK(ns == D + 36000 && ne == D + 40500);
    DragApply(1, D + 36000, D + 39600, -9000, FALSE, &ns, &ne); CHECK(ne == D + 36900);
}

#include "caltest_sync.h"

int main() {
    g_heap = GetProcessHeap(); CoInitialize(NULL); setvbuf(stdout, NULL, _IONBF, 0);
    TestDates(); TestZones(); TestParse(); TestRules(); TestStoreFile(); TestView(); TestWeekLogic(); TestEdit(); TestRuleSpec(); TestFields(); TestQuick(); TestReminders(); TestSync(); TestSyncStub();
    printf("%d checks, %d failed\n", g_tests, g_fail);
    return g_fail ? 1 : 0;
}
