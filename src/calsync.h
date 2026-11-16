// calsync.h - синхронизация календаря с источниками-скриптами: запуск скрипта, сбор push, применение ответов push/pull (слияние), состояние. Без CRT и без интерфейса.
// Подключается после calendar.h. Договор провайдера - calendars\README.md. Проверка: tests\run.cmd (caltest.cpp, sync_test.py).
// Схема: главный поток собирает задание (PushBuild) -> фоновый поток только запускает скрипт (SyncIO) -> главный поток применяет результат (SyncApply).
// Так хранилище трогает один поток, а скрипт можно ждать до минуты, не замораживая окна.
#ifndef DK_CALSYNC_H
#define DK_CALSYNC_H

namespace Cal {

/* ---------- запуск процесса с захватом stdout/stderr и stdin ---------- */
struct ProcOut { char *out, *err; int nout, nerr; DWORD code; BOOL started, timedOut; };      // out/err - Free
struct ProcIn { HANDLE h; const char *d; int n; };
static DWORD WINAPI ProcInThread(void *a) {
    ProcIn *p = (ProcIn *)a; int off = 0;
    while (off < p->n) { DWORD w = 0; if (!WriteFile(p->h, p->d + off, (DWORD)(p->n - off), &w, NULL) || !w) break; off += (int)w; }
    CloseHandle(p->h); return 0;
}
static void PipeDrain(HANDLE h, char **buf, int *n, int *cap, int limit, BOOL *open) {
    while (*open) {
        DWORD av = 0;
        if (!PeekNamedPipe(h, NULL, 0, NULL, &av, NULL)) { *open = FALSE; return; }
        if (!av) return;
        if (*n + (int)av + 1 > *cap) { int c = *cap ? *cap : 4096; while (c < *n + (int)av + 1) c *= 2; *buf = (char *)Realloc(*buf, c); *cap = c; }
        DWORD got = 0; if (!ReadFile(h, *buf + *n, av, &got, NULL) || !got) { *open = FALSE; return; }
        *n += (int)got; if (*n > limit) { *open = FALSE; return; }
    }
}
// Запускает cmd (строка меняется), пишет in в stdin, ждёт не дольше timeoutMs; по таймауту убивает процесс и его потомков (job). FALSE - не удалось запустить.
static BOOL ProcRun(wchar_t *cmd, const wchar_t *dir, const char *in, int nin, DWORD timeoutMs, ProcOut *o) {
    memset(o, 0, sizeof *o);
    SECURITY_ATTRIBUTES sa; sa.nLength = sizeof sa; sa.lpSecurityDescriptor = NULL; sa.bInheritHandle = TRUE;
    HANDLE inR = NULL, inW = NULL, outR = NULL, outW = NULL, errR = NULL, errW = NULL;
    if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 0) || !CreatePipe(&errR, &errW, &sa, 0)) {
        HANDLE all[6] = {inR, inW, outR, outW, errR, errW}; for (int i = 0; i < 6; i++) if (all[i]) CloseHandle(all[i]);
        return FALSE;
    }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0); SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0); SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si; PROCESS_INFORMATION pi; memset(&si, 0, sizeof si); memset(&pi, 0, sizeof pi);
    si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = inR; si.hStdOutput = outW; si.hStdError = errW;
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job) { JOBOBJECT_EXTENDED_LIMIT_INFORMATION li; memset(&li, 0, sizeof li); li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE; SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li); }
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, dir && *dir ? dir : NULL, &si, &pi);
    CloseHandle(inR); CloseHandle(outW); CloseHandle(errW);
    if (!ok) { CloseHandle(inW); CloseHandle(outR); CloseHandle(errR); if (job) CloseHandle(job); return FALSE; }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread); CloseHandle(pi.hThread); o->started = TRUE;
    ProcIn pin; pin.h = inW; pin.d = in; pin.n = nin; HANDLE ti = NULL;
    if (nin > 0) ti = CreateThread(NULL, 0, ProcInThread, &pin, 0, NULL); else CloseHandle(inW);
    int ocap = 0, ecap = 0; BOOL oo = TRUE, eo = TRUE; DWORD t0 = GetTickCount();
    for (;;) {
        int n0 = o->nout + o->nerr;
        PipeDrain(outR, &o->out, &o->nout, &ocap, 64 << 20, &oo); PipeDrain(errR, &o->err, &o->nerr, &ecap, 1 << 20, &eo);
        if (o->nout + o->nerr != n0) continue;
        if (WaitForSingleObject(pi.hProcess, 15) == WAIT_OBJECT_0) {
            PipeDrain(outR, &o->out, &o->nout, &ocap, 64 << 20, &oo); PipeDrain(errR, &o->err, &o->nerr, &ecap, 1 << 20, &eo);
            break;
        }
        if (GetTickCount() - t0 > timeoutMs) { o->timedOut = TRUE; if (job) TerminateJobObject(job, 1); else TerminateProcess(pi.hProcess, 1); WaitForSingleObject(pi.hProcess, 2000); break; }
    }
    GetExitCodeProcess(pi.hProcess, &o->code);
    CloseHandle(pi.hProcess); CloseHandle(outR); CloseHandle(errR);
    if (ti) { WaitForSingleObject(ti, 5000); CloseHandle(ti); }
    if (job) CloseHandle(job);
    return TRUE;
}

/* ---------- запуск скрипта-провайдера ---------- */
struct Script { wchar_t python[MAX_PATH], path[MAX_PATH], dir[MAX_PATH], profile[48]; DWORD timeoutMs; };
// Коды возврата: код выхода скрипта; -1 - не запустился; -2 - таймаут. out/err - Free (могут быть NULL).
static int ScriptRun(void *ctx, const wchar_t *verb, const wchar_t *since, const wchar_t *in, wchar_t **out, wchar_t **err) {
    const Script *s = (const Script *)ctx; Buf c = {0};
    BufAdd(&c, L"\""); BufAdd(&c, s->python); BufAdd(&c, L"\" \""); BufAdd(&c, s->path); BufAdd(&c, L"\" "); BufAdd(&c, verb);
    if (since) { BufAdd(&c, L" --since \""); for (const wchar_t *p = since; *p; p++) if (*p != L'"') BufAdd(&c, p, 1); BufAdd(&c, L"\""); }
    if (s->profile[0]) { BufAdd(&c, L" --profile \""); for (const wchar_t *p = s->profile; *p; p++) if (*p != L'"') BufAdd(&c, p, 1); BufAdd(&c, L"\""); }
    int nin = 0; char *u = in && *in ? ToUtf8(in, &nin) : NULL;
    ProcOut o; BOOL ran = ProcRun(c.p, s->dir, u, nin, s->timeoutMs ? s->timeoutMs : 60000, &o);
    Free(c.p); Free(u);
    *out = *err = NULL;
    if (!ran) return -1;
    if (o.out) *out = FromUtf8(o.out, o.nout);
    if (o.err) *err = FromUtf8(o.err, o.nerr);
    Free(o.out); Free(o.err);
    return o.timedOut ? -2 : (int)o.code;
}

/* ---------- основа ---------- */
static BOOL Dirty(const Event *e) { return e->seq != e->synced; }                       // есть правка, не отправленная в источник
static BOOL HasEtag(const Event *e) { return e->etag && *e->etag; }
static const wchar_t *FindStr(const wchar_t *h, const wchar_t *n) {
    int nl = lstrlenW(n); if (!nl) return h;
    for (; *h; h++) if (*h == *n && !memcmp(h, n, nl * sizeof(wchar_t))) return h;
    return NULL;
}
static int CountLines(const wchar_t *t, const wchar_t *prefix) {                       // строк, начинающихся с prefix
    int pl = lstrlenW(prefix), n = 0;
    for (const wchar_t *p = t; *p; ) {
        if (!memcmp(p, prefix, pl * sizeof(wchar_t))) n++;
        while (*p && *p != L'\n') p++;
        if (*p) p++;
    }
    return n;
}
static BOOL IcsOk(const wchar_t *t) {                                                   // это ICS, и у каждого события есть UID
    return t && FindStr(t, L"BEGIN:VCALENDAR") && FindStr(t, L"END:VCALENDAR") && CountLines(t, L"BEGIN:VEVENT") == CountLines(t, L"UID:");
}
static wchar_t *ExtraTake(Event *e, const wchar_t *name) {                              // значение строки NAME:... из extra (строка убирается); Free
    if (!e->extra) return NULL;
    int nl = lstrlenW(name);
    for (wchar_t *p = e->extra; *p; ) {
        wchar_t *q = p; while (*q && *q != L'\n') q++;
        if ((int)(q - p) > nl && p[nl] == L':' && EqNI(p, name, nl)) {
            wchar_t *v = DupN(p + nl + 1, (int)(q - p) - nl - 1), *u = Unesc(v); Free(v);
            int rem = (int)(q - p) + (*q ? 1 : 0); memmove(p, p + rem, (lstrlenW(p + rem) + 1) * sizeof(wchar_t));
            return u;
        }
        p = *q ? q + 1 : q;
    }
    return NULL;
}
static int FindIn(const Store *st, const wchar_t *uid, const wchar_t *src, BOOL hasRec, I64 recId) {
    for (int i = 0; i < st->n; i++) {
        const Event *e = &st->ev[i];
        if (e->uid && !lstrcmpW(e->uid, uid) && SameSrc(e->source, src) && e->hasRec == hasRec && (!hasRec || e->recId == recId)) return i;
    }
    return -1;
}
static void IsoFmt(wchar_t *o, I64 utc) { Tm t = SecToTm(utc); wsprintfW(o, L"%04d-%02d-%02dT%02d:%02d:%02dZ", t.y, t.mo, t.d, t.h, t.mi, t.s); }
static int Backoff(int fails) { if (fails <= 0) return 0; int s = 60; for (int i = 1; i < fails && s < 3600; i++) s *= 2; return s > 3600 ? 3600 : s; }   // пауза после fails неудач подряд, секунды

/* ---------- задание, результат, статистика ---------- */
struct PushItem { wchar_t *uid; BOOL hasRec; I64 recId; int seq, op; };                   // op: 1 create, 2 update, 3 delete
struct SyncJob { wchar_t *src, *since, *pushText; PushItem *it; int nit, cap; BOOL doPull; };
struct SyncOut { int pushCode, pullCode; BOOL pushRan, pullRan; wchar_t *pushOut, *pushErr, *pullOut, *pullErr; I64 pullAt; };
enum { SYE_NONE, SYE_RUN, SYE_TIME, SYE_EXIT, SYE_BAD, SYE_EVT };                         // не запустился | таймаут | код выхода | некорректный ответ | часть событий не отправлена
struct SyncStat {
    int pushed, pushErr, added, updated, removed, conflicts, errKind, errCode;
    wchar_t errText[200]; BOOL ok, repush; wchar_t *cursor; Buf log;                       // ok - цикл выполнен (курсор годится); repush - стоит отправить ещё раз (победила местная версия)
};
static void JobFree(SyncJob *j) { Free(j->src); Free(j->since); Free(j->pushText); for (int i = 0; i < j->nit; i++) Free(j->it[i].uid); Free(j->it); memset(j, 0, sizeof *j); }
static void OutFree(SyncOut *o) { Free(o->pushOut); Free(o->pushErr); Free(o->pullOut); Free(o->pullErr); memset(o, 0, sizeof *o); }
static void StatFree(SyncStat *r) { Free(r->cursor); Free(r->log.p); memset(r, 0, sizeof *r); }
static void LogAdd(SyncStat *r, const wchar_t *kind, const wchar_t *title, const wchar_t *extra) {      // строка журнала sync.log: «вид: «название» - подробности»
    BufAdd(&r->log, kind); if (title && *title) { BufAdd(&r->log, L": \xAB"); BufAdd(&r->log, title); BufAdd(&r->log, L"\xBB"); }
    if (extra && *extra) { BufAdd(&r->log, L" - "); BufAdd(&r->log, extra); }
    BufAdd(&r->log, L"\n");
}
static void FirstLine(wchar_t *o, int cap, const wchar_t *s) {                          // первая непустая строка текста (stderr)
    o[0] = 0; if (!s) return;
    while (*s && *s <= L' ') s++;
    int n = 0; while (*s && *s != L'\n' && *s != L'\r' && n < cap - 1) o[n++] = *s++;
    while (n > 0 && o[n - 1] <= L' ') n--;
    o[n] = 0;
}
static void SetFail(SyncStat *r, int code, const wchar_t *err) {
    r->errKind = code == -1 ? SYE_RUN : code == -2 ? SYE_TIME : SYE_EXIT; r->errCode = code; FirstLine(r->errText, 200, err);
}
static const wchar_t *TitleOf(const Event *e) { return e->summary && *e->summary ? e->summary : L""; }

/* ---------- сбор push (главный поток) ---------- */
static void ItemAdd(SyncJob *j, const Event *e, int op) {
    if (j->nit == j->cap) { j->cap = j->cap ? j->cap * 2 : 16; j->it = (PushItem *)Realloc(j->it, j->cap * sizeof(PushItem)); }
    PushItem *p = &j->it[j->nit++]; p->uid = Dup(e->uid); p->hasRec = e->hasRec; p->recId = e->recId; p->seq = e->seq; p->op = op;
}
// Собирает задание для источника src: события с правками (seq != synced) группами по UID (серия уходит целиком). hidden не отправляется;
// удалённое, не бывавшее в источнике (нет etag), убирается сразу. Возвращает число отправляемых групп; j->pushText - ICS с X-DKEYS-OP.
static int PushBuild(Store *st, const wchar_t *src, const wchar_t *since, SyncJob *j) {
    memset(j, 0, sizeof *j); j->src = Dup(src); j->since = Dup(since ? since : L""); j->doPull = TRUE;
    for (int i = st->n - 1; i >= 0; i--) if (SameSrc(st->ev[i].source, src) && st->ev[i].deleted && !HasEtag(&st->ev[i])) StoreRemove(st, i);
    Store tmp; memset(&tmp, 0, sizeof tmp); int groups = 0; const wchar_t **seen = (const wchar_t **)Alloc((st->n + 1) * sizeof(wchar_t *)); int ns = 0;
    for (int i = 0; i < st->n; i++) {
        const Event *e = &st->ev[i];
        if (!e->uid || !SameSrc(e->source, src) || !Dirty(e) || (e->hidden && !e->deleted)) continue;
        BOOL dup = FALSE; for (int k = 0; k < ns && !dup; k++) dup = !lstrcmpW(seen[k], e->uid);
        if (dup) continue;
        seen[ns++] = e->uid;
        BOOL live = FALSE, etag = FALSE, hid = FALSE;
        for (int k = 0; k < st->n; k++) if (SameSeries(&st->ev[k], e->uid, src)) { if (!st->ev[k].deleted) { live = TRUE; if (st->ev[k].hidden) hid = TRUE; } if (HasEtag(&st->ev[k])) etag = TRUE; }
        if (hid || (!live && !etag)) continue;
        int op = !live ? 3 : etag ? 2 : 1;
        for (int k = 0; k < st->n; k++) {
            const Event *m = &st->ev[k]; if (!SameSeries(m, e->uid, src) || (op != 3 && m->deleted)) continue;
            int c = StoreAddClone(&tmp, m); Event *x = &tmp.ev[c]; x->hidden = FALSE; x->synced = 0;
            const wchar_t *ol = op == 1 ? L"X-DKEYS-OP:create" : op == 2 ? L"X-DKEYS-OP:update" : L"X-DKEYS-OP:delete"; AddExtra(x, ol, lstrlenW(ol));
            ItemAdd(j, m, op);
        }
        groups++;
    }
    Free(seen);
    if (groups) j->pushText = Serialize(&tmp, NULL);
    StoreFree(&tmp); return groups;
}

typedef int (*ProvFn)(void *ctx, const wchar_t *verb, const wchar_t *since, const wchar_t *in, wchar_t **out, wchar_t **err);
// Ввод-вывод задания (можно из любого потока): push (если есть что), затем pull. Хранилища не трогает.
static void SyncIO(const SyncJob *j, ProvFn fn, void *ctx, SyncOut *o) {
    memset(o, 0, sizeof *o);
    if (j->pushText && *j->pushText) { o->pushRan = TRUE; o->pushCode = fn(ctx, L"push", NULL, j->pushText, &o->pushOut, &o->pushErr); if (o->pushCode != 0) return; }
    if (j->doPull) { o->pullRan = TRUE; o->pullAt = NowUtc(); o->pullCode = fn(ctx, L"pull", j->since ? j->since : L"", NULL, &o->pullOut, &o->pullErr); }
}

/* ---------- применение ответа push ---------- */
static void PushApply(Store *st, const wchar_t *src, const SyncJob *j, const wchar_t *text, SyncStat *r) {
    if (!IcsOk(text)) { r->errKind = SYE_BAD; return; }
    Store R; memset(&R, 0, sizeof R); Parse(&R, text, src);
    wchar_t **ref = (wchar_t **)Alloc((R.n + 1) * sizeof(wchar_t *)), **er = (wchar_t **)Alloc((R.n + 1) * sizeof(wchar_t *));
    for (int k = 0; k < R.n; k++) { ref[k] = ExtraTake(&R.ev[k], L"X-DKEYS-REF"); er[k] = ExtraTake(&R.ev[k], L"X-DKEYS-ERROR"); }
    for (int a = 0; a < j->nit; ) {
        int b = a; while (b < j->nit && !lstrcmpW(j->it[b].uid, j->it[a].uid)) b++;
        const wchar_t *uid = j->it[a].uid; int op = j->it[a].op, li = FindIn(st, uid, src, j->it[a].hasRec, j->it[a].recId);
        wchar_t title[80]; lstrcpynW(title, li >= 0 ? TitleOf(&st->ev[li]) : L"", 80);
        BOOL any = FALSE, bad = FALSE; const wchar_t *msg = L""; const wchar_t *nu = uid;
        for (int k = 0; k < R.n; k++) {
            if (lstrcmpW(ref[k] ? ref[k] : R.ev[k].uid, uid)) continue;
            if (!any) nu = R.ev[k].uid; any = TRUE;
            if (er[k] && *er[k]) { bad = TRUE; msg = er[k]; }
            else if (op != 3 && !R.ev[k].deleted && !HasEtag(&R.ev[k])) { bad = TRUE; msg = L"в ответе нет X-DKEYS-ETAG"; }
        }
        if (!any) { bad = TRUE; msg = L"нет ответа скрипта на событие"; }
        if (bad) {
            r->pushErr++; LogAdd(r, L"не отправлено", title, msg);
            if (r->errKind == SYE_NONE) { r->errKind = SYE_EVT; FirstLine(r->errText, 200, msg); }
            a = b; continue;
        }
        if (op == 3) { for (int i = st->n - 1; i >= 0; i--) if (SameSeries(&st->ev[i], uid, src) && st->ev[i].deleted) StoreRemove(st, i); }
        else {
            for (int m = a; m < b; m++) {
                int l = FindIn(st, uid, src, j->it[m].hasRec, j->it[m].recId); if (l < 0) continue;
                const wchar_t *et = NULL;                                                // etag того же вхождения в ответе, иначе - любого события серии
                for (int k = 0; k < R.n; k++) if (!lstrcmpW(ref[k] ? ref[k] : R.ev[k].uid, uid) && HasEtag(&R.ev[k])) { if (!et) et = R.ev[k].etag; if (R.ev[k].hasRec == j->it[m].hasRec && (!j->it[m].hasRec || R.ev[k].recId == j->it[m].recId)) { et = R.ev[k].etag; break; } }
                SetStr(&st->ev[l].etag, et); st->ev[l].synced = j->it[m].seq;           // правка, сделанная пока шла отправка, останется несинхронизированной
            }
            if (lstrcmpW(nu, uid)) for (int i = 0; i < st->n; i++) if (SameSeries(&st->ev[i], uid, src)) SetStr(&st->ev[i].uid, nu);
        }
        r->pushed++; a = b;
    }
    for (int k = 0; k < R.n; k++) { Free(ref[k]); Free(er[k]); }
    Free(ref); Free(er); StoreFree(&R);
}

/* ---------- слияние ответа pull ---------- */
static void TakeServer(Store *st, int li, const Event *p) {                            // местное событие заменяется серверным (скрытость остаётся)
    Event t; EventClone(&t, p); BOOL hid = st->ev[li].hidden;
    EventTake(&st->ev[li], &t); st->ev[li].hidden = hid; st->ev[li].deleted = FALSE; st->ev[li].synced = st->ev[li].seq;
}
static void Resurrect(Store *st, const wchar_t *uid, const wchar_t *src, SyncStat *r, const wchar_t *why) {     // сервер удалил, а здесь есть правки: событие будет создано заново
    wchar_t t[80]; t[0] = 0;
    for (int i = 0; i < st->n; i++) if (SameSeries(&st->ev[i], uid, src)) { if (!t[0]) lstrcpynW(t, TitleOf(&st->ev[i]), 80); SetStr(&st->ev[i].etag, NULL); st->ev[i].synced = st->ev[i].seq - 1; }
    r->conflicts++; LogAdd(r, why, t, L"отправлено заново");
}
static void Merge1(Store *st, const wchar_t *src, const Event *p, SyncStat *r) {
    int li = FindIn(st, p->uid, src, p->hasRec, p->recId);
    if (p->deleted) {
        if (p->hasRec) {                                                                 // удалена одна замена вхождения
            if (li < 0) return;
            if (Dirty(&st->ev[li]) && !st->ev[li].deleted) { Resurrect(st, p->uid, src, r, L"удалено на сервере, но изменено здесь"); return; }
            StoreRemove(st, li); r->removed++; return;
        }
        BOOL dirty = FALSE, any = FALSE;
        for (int i = 0; i < st->n; i++) if (SameSeries(&st->ev[i], p->uid, src)) { any = TRUE; if (Dirty(&st->ev[i]) && !st->ev[i].deleted) dirty = TRUE; }
        if (!any) return;
        if (dirty) { Resurrect(st, p->uid, src, r, L"удалено на сервере, но изменено здесь"); return; }
        for (int i = st->n - 1; i >= 0; i--) if (SameSeries(&st->ev[i], p->uid, src)) StoreRemove(st, i);
        r->removed++; return;
    }
    if (li < 0) { int k = StoreAddClone(st, p); st->ev[k].synced = st->ev[k].seq; st->ev[k].deleted = FALSE; r->added++; return; }
    Event *l = &st->ev[li];
    if (l->deleted) return;                                                              // местное удаление ждёт отправки
    if (HasEtag(l) && !lstrcmpW(l->etag, p->etag)) return;                               // та же версия
    if (!Dirty(l)) { TakeServer(st, li, p); r->updated++; return; }
    r->conflicts++;                                                                      // правили и здесь, и на сервере: побеждает более позднее изменение
    if (p->stamp >= l->stamp) { LogAdd(r, L"конфликт, победила версия сервера", TitleOf(l), NULL); TakeServer(st, li, p); }
    else { LogAdd(r, L"конфликт, победила местная версия", TitleOf(l), NULL); SetStr(&l->etag, p->etag); r->repush = TRUE; }
}
static BOOL PullApply(Store *st, const wchar_t *src, const SyncJob *j, const wchar_t *text, I64 pullAt, SyncStat *r) {
    if (!IcsOk(text)) { r->errKind = SYE_BAD; return FALSE; }
    BOOL full = !j->since || !*j->since || FindStr(text, L"X-DKEYS-FULL:1");
    Store P; memset(&P, 0, sizeof P); Parse(&P, text, src);
    for (int k = 0; k < P.n; k++) if (!P.ev[k].deleted && !HasEtag(&P.ev[k])) { r->errKind = SYE_BAD; StoreFree(&P); return FALSE; }
    for (int k = 0; k < P.n; k++) Merge1(st, src, &P.ev[k], r);
    for (int k = 0; k < P.n; k++) {                                                      // серия пришла целиком: замены, которых в ней больше нет, убираем
        const Event *m = &P.ev[k]; if (m->deleted || m->hasRec) continue;
        for (int i = st->n - 1; i >= 0; i--) {
            const Event *e = &st->ev[i]; if (!e->hasRec || !SameSeries(e, m->uid, src) || Dirty(e) || e->deleted) continue;
            BOOL has = FALSE; for (int q = 0; q < P.n && !has; q++) has = !P.ev[q].deleted && P.ev[q].hasRec && P.ev[q].recId == e->recId && !lstrcmpW(P.ev[q].uid, m->uid);
            if (!has) { StoreRemove(st, i); r->removed++; }
        }
    }
    if (full) {                                                                          // полная выгрузка: чего нет в ответе, того на сервере больше нет
        int synced = 0; for (int i = 0; i < st->n; i++) if (SameSrc(st->ev[i].source, src) && HasEtag(&st->ev[i]) && !st->ev[i].deleted) synced++;
        if (P.n == 0 && synced >= 3) LogAdd(r, L"пустой полный ответ", NULL, L"удаление пропущено");
        else for (int i = st->n - 1; i >= 0; i--) {
            Event *e = &st->ev[i]; if (!SameSrc(e->source, src) || !HasEtag(e) || e->deleted) continue;
            BOOL has = FALSE; for (int q = 0; q < P.n && !has; q++) has = !P.ev[q].deleted && P.ev[q].hasRec == e->hasRec && (!e->hasRec || P.ev[q].recId == e->recId) && !lstrcmpW(P.ev[q].uid, e->uid);
            if (has) continue;
            if (Dirty(e)) { wchar_t u[200]; lstrcpynW(u, e->uid, 200); Resurrect(st, u, src, r, L"нет на сервере, но изменено здесь"); }
            else { StoreRemove(st, i); r->removed++; }
        }
    }
    StoreFree(&P);
    const wchar_t *c = FindStr(text, L"\nX-DKEYS-CURSOR:");
    if (c) {
        c += 16; int n = 0; while (c[n] && c[n] != L'\r' && c[n] != L'\n') n++;
        while (n > 0 && c[n - 1] == L' ') n--;
        r->cursor = DupN(c, n);
    } else { wchar_t t[40]; IsoFmt(t, pullAt - 120); r->cursor = Dup(t); }                // запас на расхождение часов: повторно пришедшее - то же состояние, вреда нет
    return TRUE;
}
// Применяет результат ввода-вывода к хранилищу (главный поток). r->ok - цикл выполнен без сбоя скрипта (по событиям ошибки возможны - r->errKind == SYE_EVT).
static void SyncApply(Store *st, const wchar_t *src, const SyncJob *j, const SyncOut *o, SyncStat *r) {
    memset(r, 0, sizeof *r);
    if (o->pushRan) {
        if (o->pushCode != 0) { SetFail(r, o->pushCode, o->pushErr); return; }
        PushApply(st, src, j, o->pushOut, r); if (r->errKind == SYE_BAD) return;
    }
    if (o->pullRan) {
        if (o->pullCode != 0) { SetFail(r, o->pullCode, o->pullErr); return; }
        if (!PullApply(st, src, j, o->pullOut, o->pullAt, r)) return;
    }
    r->ok = TRUE;
}

/* ---------- файл состояния источника (<источник>.sync): курсор и время последней успешной синхронизации ---------- */
static wchar_t *SyncStateSave(const wchar_t *cursor, I64 okAt) {
    Buf b = {0}; wchar_t t[24];
    BufAdd(&b, cursor ? cursor : L""); BufAdd(&b, L"\n"); FmtI64(t, okAt); BufAdd(&b, t); BufAdd(&b, L"\n");
    return b.p;
}
static void SyncStateLoad(const wchar_t *text, wchar_t **cursor, I64 *okAt) {
    *cursor = NULL; *okAt = 0;
    const wchar_t *q = text; while (*q && *q != L'\n') q++;
    int n = (int)(q - text); while (n > 0 && text[n - 1] == L'\r') n--;
    *cursor = DupN(text, n);
    if (*q) { q++; const wchar_t *e = q; while (*e && *e != L'\n') e++; *okAt = ToI64(q, (int)(e - q)); }
}

} // namespace Cal
#endif
