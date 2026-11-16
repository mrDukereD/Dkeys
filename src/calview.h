// calview.h - логика вида календаря DKeys без интерфейса: сетка месяца 6x7, номера недель, диапазон запроса, события по дням, тестовые события.
// Подключается после calendar.h (и из dkeys.cpp, и из tests\caltest.cpp). Все дни - целые числа от 1970-01-01 в МЕСТНОМ времени.
#ifndef DK_CALVIEW_H
#define DK_CALVIEW_H

namespace CalView {

using Cal::I64;
static const int GRID = 42;                                    // 6 недель по 7 дней

static I64 MonthFirst(int y, int m) { return Cal::DaysFromCivil(y, m, 1); }
// Первый день сетки месяца: ws 0 - неделя с понедельника, 1 - с воскресенья.
static I64 GridStart(int y, int m, int ws) {
    I64 f = MonthFirst(y, m); int wd = Cal::Weekday(f);
    return f - (ws ? (wd + 1) % 7 : wd);
}
static int IsoWeek(I64 day) {                                  // номер недели ISO 8601
    I64 th = day - Cal::Weekday(day) + 3; int y, m, d; Cal::CivilFromDays(th, &y, &m, &d);
    return (int)((th - Cal::DaysFromCivil(y, 1, 1)) / 7) + 1;
}
static int RowWeek(I64 rowFirst, int ws) { return IsoWeek(rowFirst + (ws ? 4 : 3)); }   // номер недели строки сетки - по её четвергу
static void AddMonths(int *y, int *m, int delta) {
    I64 mi = (I64)*y * 12 + (*m - 1) + delta; I64 yy = Cal::FloorDiv(mi, 12);
    *y = (int)yy; *m = (int)(mi - yy * 12) + 1;
}
static I64 ShiftMonths(I64 day, int delta) {                   // тот же день через delta месяцев (31-е -> последний день месяца)
    int y, m, d; Cal::CivilFromDays(day, &y, &m, &d); AddMonths(&y, &m, delta);
    int dim = Cal::DaysInMonth(y, m); if (d > dim) d = dim;
    return Cal::DaysFromCivil(y, m, d);
}
static I64 TodayLocal(void) { return Cal::FloorDiv(Cal::UtcToWall(Cal::NowUtc(), L""), 86400); }
// Диапазон запроса Cal::Query (UTC) для n дней сетки с first.
static void Range(I64 first, int n, I64 *fromUtc, I64 *toUtc) {
    *fromUtc = Cal::WallToUtc(first * 86400, L""); *toUtc = Cal::WallToUtc((first + n) * 86400, L"");
}
// Дни, которых касается вхождение: целодневное - по дате, остальные - по местному времени (конец не включается).
static void OccSpan(const Cal::Store *st, const Cal::Occ &o, I64 *first, I64 *last) {
    if (o.allDay) {
        const Cal::Event &e = st->ev[o.ev]; I64 n = (e.end - e.start) / 86400; if (n < 1) n = 1;
        *first = Cal::FloorDiv(o.wall, 86400); *last = *first + n - 1; return;
    }
    I64 ws = Cal::UtcToWall(o.s, L""), we = Cal::UtcToWall(o.e, L"");
    *first = Cal::FloorDiv(ws, 86400); *last = we > ws ? Cal::FloorDiv(we - 1, 86400) : *first;
}

// События по дням сетки: off[d]..off[d+1] - индексы вхождений (в OccList) дня d; целодневные идут раньше остальных.
struct DayLists { int off[GRID + 1]; int *items; };
typedef BOOL (*ShowFn)(const Cal::Event *e);
static void DaysFree(DayLists *dl) { Free(dl->items); memset(dl, 0, sizeof *dl); }
static void BuildDays(const Cal::Store *st, const Cal::OccList *ol, I64 first, ShowFn show, DayLists *dl) {
    memset(dl, 0, sizeof *dl);
    int cnt[GRID]; memset(cnt, 0, sizeof cnt);
    for (int pass = 0; pass < 2; pass++) {                     // проход 0 - счёт, 1 - заполнение
        int pos[GRID];
        if (pass) { for (int d = 0; d < GRID; d++) pos[d] = dl->off[d]; }
        else dl->off[0] = 0;
        for (int half = 0; half < (pass ? 2 : 1); half++) {    // при заполнении: сначала целодневные (half 0), потом остальные
            for (int i = 0; i < ol->n; i++) {
                const Cal::Occ &o = ol->a[i];
                if (pass && (half == 0) != (o.allDay != 0)) continue;
                if (show && !show(&st->ev[o.ev])) continue;
                I64 a, b; OccSpan(st, o, &a, &b);
                for (I64 d = a < first ? first : a; d <= b && d < first + GRID; d++) {
                    int k = (int)(d - first);
                    if (pass) dl->items[pos[k]++] = i; else cnt[k]++;
                }
            }
        }
        if (!pass) {
            for (int d = 0; d < GRID; d++) dl->off[d + 1] = dl->off[d] + cnt[d];
            dl->items = (int *)Alloc((dl->off[GRID] + 1) * sizeof(int));
        }
    }
}

// ---- сетка по часам (неделя/день): раскладка блоков и привязка к 15 минутам ----
static const int SNAP = 900;                                   // шаг привязки, секунд
static I64 SnapSec(I64 s) { return Cal::FloorDiv(s + SNAP / 2, SNAP) * SNAP; }
// Блоки одного дня (минуты от полуночи, e > s): перекрывающиеся получают колонки рядом. col - колонка, ncol - число колонок в группе перекрытий.
struct Blk { int s, e, col, ncol; };
static const int MAXBLK = 64;
static void LayoutBlocks(Blk *b, int n) {
    int ord[MAXBLK]; if (n > MAXBLK) n = MAXBLK;
    for (int i = 0; i < n; i++) { int j = i; while (j > 0 && (b[ord[j - 1]].s > b[i].s || (b[ord[j - 1]].s == b[i].s && b[ord[j - 1]].e < b[i].e))) { ord[j] = ord[j - 1]; j--; } ord[j] = i; }
    int i = 0;
    while (i < n) {
        int colEnd[MAXBLK], nc = 0, gEnd = -1, j = i;               // группа: цепочка перекрытий
        for (; j < n; j++) {
            Blk &x = b[ord[j]]; if (j > i && x.s >= gEnd) break;
            int c = 0; while (c < nc && colEnd[c] > x.s) c++;
            if (c == nc) nc++;
            colEnd[c] = x.e; x.col = c; if (x.e > gEnd) gEnd = x.e;
        }
        for (int k = i; k < j; k++) b[ord[k]].ncol = nc;
        i = j;
    }
}
// Перетаскивание блока: mode 0 - перенос, 1 - длительность (нижний край). s, e, delta - местные секунды; lockDay - день менять нельзя (повтор).
static void DragApply(int mode, I64 s, I64 e, I64 delta, BOOL lockDay, I64 *ns, I64 *ne) {
    if (mode == 0) {
        I64 n = SnapSec(s + delta);
        if (lockDay) n = Cal::FloorDiv(s, 86400) * 86400 + (n - Cal::FloorDiv(n, 86400) * 86400);
        *ns = n; *ne = n + (e - s);
    } else {
        I64 n = SnapSec(e + delta); if (n < s + SNAP) n = s + SNAP;
        *ns = s; *ne = n;
    }
}

// Тестовые события (источник demo - в файлы не пишутся) вокруг дня today (в память, не в файл): повторы, целодневные, несколько дней, разные пояса, много событий в один день.
static void DemoAdd(Cal::Store *st, const wchar_t *summary, BOOL allDay, const wchar_t *tz, I64 start, I64 end, const wchar_t *rrule) {
    Cal::Event *e = Cal::StoreNew(st); wchar_t u[80]; Cal::NewUid(u, 80);
    e->uid = Dup(u); e->summary = Dup(summary); e->source = Dup(L"demo"); e->allDay = allDay;
    e->tzid = allDay ? NULL : Dup(tz); e->start = start; e->end = end; if (rrule) e->rrule = Dup(rrule);
}
static void DemoFill(Cal::Store *st, I64 today) {
    I64 D = today * 86400;
    int y, m, d; Cal::CivilFromDays(today, &y, &m, &d);
    I64 m0 = MonthFirst(y, m) * 86400;                                            // начало этого месяца
    DemoAdd(st, L"Планёрка", FALSE, L"Europe/Moscow", m0 - 30 * 86400 + 10 * 3600, m0 - 30 * 86400 + 10 * 3600 + 1800, L"FREQ=WEEKLY;BYDAY=MO,WE");
    DemoAdd(st, L"Обед", FALSE, L"", D - 3 * 86400 + 13 * 3600, D - 3 * 86400 + 14 * 3600, L"FREQ=DAILY;COUNT=10");
    DemoAdd(st, L"Отпуск", TRUE, NULL, D + 2 * 86400, D + 5 * 86400, NULL);
    DemoAdd(st, L"День рождения", TRUE, NULL, Cal::DaysFromCivil(y - 3, m, 20) * 86400, Cal::DaysFromCivil(y - 3, m, 21) * 86400, L"FREQ=YEARLY");
    DemoAdd(st, L"Отчёт", FALSE, L"", m0 + 17 * 3600, m0 + 18 * 3600, L"FREQ=MONTHLY;BYMONTHDAY=1,15");
    DemoAdd(st, L"Звонок (Нью-Йорк)", FALSE, L"America/New_York", D + 86400 + 9 * 3600, D + 86400 + 10 * 3600, NULL);
    DemoAdd(st, L"Звонок (Токио)", FALSE, L"Asia/Tokyo", D + 86400 + 20 * 3600, D + 86400 + 21 * 3600, NULL);
    DemoAdd(st, L"Ночная смена", FALSE, L"", D + 6 * 86400 + 22 * 3600, D + 7 * 86400 + 2 * 3600, NULL);
    for (int i = 0; i < 6; i++) { wchar_t t[40]; wsprintfW(t, L"Встреча %d", i + 1); DemoAdd(st, t, FALSE, L"", D + 3 * 86400 + (9 + i) * 3600, D + 3 * 86400 + (9 + i) * 3600 + 2700, NULL); }
}

} // namespace CalView
#endif
