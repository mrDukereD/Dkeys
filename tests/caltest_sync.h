// Проверки синхронизации (src\calsync.h), подключается из caltest.cpp: TestSync - движок на заготовленных ответах провайдера,
// TestSyncStub - сквозные проверки с настоящим calendars\stub.py (если найден python).

/* ---------- заготовки ---------- */
static wchar_t *SyEv(const wchar_t *uid, const wchar_t *sum, const wchar_t *etag, const wchar_t *extra = L"", const wchar_t *lm = L"20261101T100000Z") {
    Buf b = {0}; BufAdd(&b, L"BEGIN:VEVENT\r\nUID:"); BufAdd(&b, uid); BufAdd(&b, L"\r\nSUMMARY:"); BufAdd(&b, sum);
    BufAdd(&b, L"\r\nDTSTART:20261120T100000Z\r\nDTEND:20261120T110000Z\r\nLAST-MODIFIED:"); BufAdd(&b, lm); BufAdd(&b, L"\r\n");
    if (*etag) { BufAdd(&b, L"X-DKEYS-ETAG:"); BufAdd(&b, etag); BufAdd(&b, L"\r\n"); }
    BufAdd(&b, extra); BufAdd(&b, L"END:VEVENT\r\n"); return b.p;
}
static wchar_t *SyCal(const wchar_t *head, const wchar_t *a = L"", const wchar_t *b2 = L"", const wchar_t *c = L"") {
    Buf b = {0}; BufAdd(&b, L"BEGIN:VCALENDAR\r\nVERSION:2.0\r\n"); BufAdd(&b, head); BufAdd(&b, a); BufAdd(&b, b2); BufAdd(&b, c); BufAdd(&b, L"END:VCALENDAR\r\n"); return b.p;
}
static Event *SyF(Store *st, const wchar_t *uid, const wchar_t *src) { int i = FindIn(st, uid, src, FALSE, 0); return i >= 0 ? &st->ev[i] : NULL; }
static Event *SyNew(Store *st, const wchar_t *uid, const wchar_t *src, const wchar_t *sum) {      // новое местное событие (как после «+ Событие»)
    Event *e = StoreNew(st); e->uid = Dup(uid); e->source = Dup(src); e->summary = Dup(sum); e->tzid = Dup(L"UTC"); e->start = W(2026, 11, 21, 10); e->end = W(2026, 11, 21, 11); Touch(e); return e;
}
static BOOL SyHas(const wchar_t *h, const wchar_t *n) { return h && FindStr(h, n) != NULL; }

struct Canned { int pushCode, pullCode, npush, npull; wchar_t *pushOut, *pullOut, *err, *lastPush, *lastSince; };
static Canned g_cn;
static int CannedFn(void *, const wchar_t *verb, const wchar_t *since, const wchar_t *in, wchar_t **out, wchar_t **err) {
    BOOL push = !lstrcmpW(verb, L"push"); *out = *err = NULL;
    if (push) { g_cn.npush++; g_cn.lastPush = in ? Dup(in) : NULL; *out = g_cn.pushOut ? Dup(g_cn.pushOut) : NULL; }
    else { g_cn.npull++; g_cn.lastSince = Dup(since ? since : L""); *out = g_cn.pullOut ? Dup(g_cn.pullOut) : NULL; }
    if (g_cn.err) *err = Dup(g_cn.err);
    return push ? g_cn.pushCode : g_cn.pullCode;
}
static void CnReset(void) { memset(&g_cn, 0, sizeof g_cn); }
static int SyncOne(Store *st, const wchar_t *src, const wchar_t *since, SyncStat *r, ProvFn fn = CannedFn, void *ctx = NULL) {
    SyncJob j; int groups = PushBuild(st, src, since, &j); SyncOut o; SyncIO(&j, fn, ctx, &o);
    SyncApply(st, src, &j, &o, r); JobFree(&j); OutFree(&o); return groups;
}

static void TestSync(void) {
    Store st; memset(&st, 0, sizeof st); SyncStat r; const wchar_t *S = L"s";
    // --- первая (полная) выгрузка, курсор, повтор без изменений ---
    CnReset(); g_cn.pullOut = SyCal(L"X-DKEYS-CURSOR:rev=2\r\n", SyEv(L"a", L"Alpha", L"e1"), SyEv(L"b", L"Beta", L"e2"));
    CHECK(SyncOne(&st, S, L"", &r) == 0 && r.ok && r.added == 2 && st.n == 2 && !lstrcmpW(r.cursor, L"rev=2") && !lstrcmpW(g_cn.lastSince, L""));
    CHECK(!Dirty(SyF(&st, L"a", S)) && !lstrcmpW(SyF(&st, L"a", S)->etag, L"e1") && !lstrcmpW(SyF(&st, L"a", S)->source, S) && SyF(&st, L"a", S)->stamp > 0);
    StatFree(&r); SyncOne(&st, S, L"rev=2", &r);
    CHECK(r.ok && r.added == 0 && r.updated == 0 && r.removed == 0 && r.conflicts == 0 && st.n == 2 && !lstrcmpW(g_cn.lastSince, L"rev=2")); StatFree(&r);
    // --- обновление с сервера (местных правок нет) ---
    g_cn.pullOut = SyCal(L"", SyEv(L"a", L"Alpha2", L"e3"));
    SyncOne(&st, S, L"rev=2", &r); CHECK(r.updated == 1 && !lstrcmpW(SyF(&st, L"a", S)->summary, L"Alpha2") && !lstrcmpW(SyF(&st, L"a", S)->etag, L"e3") && !Dirty(SyF(&st, L"a", S)) && r.cursor && r.cursor[4] == L'-' && r.cursor[lstrlenW(r.cursor) - 1] == L'Z'); StatFree(&r);   // без курсора в ответе - время запроса минус запас
    // --- местная правка уходит в push с OP:update и старым etag; ответ даёт новый etag; повтор ничего не шлёт ---
    { Event *a = SyF(&st, L"a", S); SetStr(&a->summary, L"Local"); Touch(a); CHECK(Dirty(a)); }
    CnReset(); g_cn.pushOut = SyCal(L"", SyEv(L"a", L"Local", L"e4")); g_cn.pullOut = SyCal(L"", SyEv(L"a", L"Local", L"e4"));
    CHECK(SyncOne(&st, S, L"rev=3", &r) == 1 && r.ok && r.pushed == 1 && g_cn.npush == 1 && SyHas(g_cn.lastPush, L"X-DKEYS-OP:update") && SyHas(g_cn.lastPush, L"X-DKEYS-ETAG:e3") && SyHas(g_cn.lastPush, L"SUMMARY:Local"));
    CHECK(!SyHas(g_cn.lastPush, L"X-DKEYS-SYNCED") && r.updated == 0 && !Dirty(SyF(&st, L"a", S)) && !lstrcmpW(SyF(&st, L"a", S)->etag, L"e4")); StatFree(&r);
    g_cn.npush = 0; CHECK(SyncOne(&st, S, L"rev=4", &r) == 0 && g_cn.npush == 0 && r.ok); StatFree(&r);                       // идемпотентность
    // --- новое событие: create; сервер присвоил другой UID (REF) ---
    SyNew(&st, L"n1", S, L"New");
    CnReset(); g_cn.pushOut = SyCal(L"", SyEv(L"srv1", L"New", L"e5", L"X-DKEYS-REF:n1\r\n")); g_cn.pullOut = SyCal(L"");
    CHECK(SyncOne(&st, S, L"rev=4", &r) == 1 && SyHas(g_cn.lastPush, L"X-DKEYS-OP:create") && r.pushed == 1 && !SyF(&st, L"n1", S) && SyF(&st, L"srv1", S) && !Dirty(SyF(&st, L"srv1", S)) && !lstrcmpW(SyF(&st, L"srv1", S)->etag, L"e5")); StatFree(&r);
    // --- правка во время отправки остаётся несинхронизированной ---
    { SyncJob j; Event *a = SyF(&st, L"a", S); SetStr(&a->summary, L"V1"); Touch(a); PushBuild(&st, S, L"x", &j); SetStr(&a->summary, L"V2"); Touch(a);
      SyncOut o; memset(&o, 0, sizeof o); o.pushRan = TRUE; o.pushOut = SyCal(L"", SyEv(L"a", L"V1", L"e6")); SyncApply(&st, S, &j, &o, &r);
      CHECK(r.pushed == 1 && !lstrcmpW(a->etag, L"e6") && Dirty(a)); StatFree(&r); JobFree(&j); OutFree(&o); }
    // --- удаление: синхронизированное - op delete; ни разу не отправленное - просто исчезает, без push ---
    { int bi = FindIn(&st, L"b", S, FALSE, 0); DeleteEvent(&st, bi, TRUE, TRUE); }
    Event *tmp = SyNew(&st, L"n2", S, L"Tmp"); (void)tmp; DeleteEvent(&st, FindIn(&st, L"n2", S, FALSE, 0), TRUE, TRUE);
    { Event *a = SyF(&st, L"a", S); a->synced = a->seq; }
    CnReset(); g_cn.pushOut = SyCal(L"", SyEv(L"b", L"Beta", L"", L"X-DKEYS-DELETED:1\r\n")); g_cn.pullOut = SyCal(L"");
    CHECK(SyncOne(&st, S, L"rev=4", &r) == 1 && SyHas(g_cn.lastPush, L"X-DKEYS-OP:delete") && !SyHas(g_cn.lastPush, L"UID:n2") && !SyF(&st, L"b", S) && !SyF(&st, L"n2", S) && r.pushed == 1); StatFree(&r);
    // --- hidden: ничего не отправляется; сервер обновил - содержимое новое, скрытость остаётся; сервер удалил - убирается ---
    { Event *a = SyF(&st, L"a", S); a->synced = a->seq; DeleteEvent(&st, FindIn(&st, L"a", S, FALSE, 0), FALSE, FALSE); }
    CnReset(); g_cn.pullOut = SyCal(L"", SyEv(L"a", L"ServerNew", L"e7"));
    CHECK(SyncOne(&st, S, L"rev=5", &r) == 0 && g_cn.npush == 0 && r.updated == 1 && SyF(&st, L"a", S)->hidden && !lstrcmpW(SyF(&st, L"a", S)->summary, L"ServerNew")); StatFree(&r);
    g_cn.pullOut = SyCal(L"", SyEv(L"a", L"", L"", L"X-DKEYS-DELETED:1\r\n"));
    SyncOne(&st, S, L"rev=6", &r); CHECK(r.removed == 1 && !SyF(&st, L"a", S)); StatFree(&r);
    // --- сервер удалил событие с несинхронизированной правкой (push вернул not found): оно создаётся заново ---
    CnReset(); g_cn.pullOut = SyCal(L"", SyEv(L"c", L"Gamma", L"e8")); SyncOne(&st, S, L"rev=6", &r); StatFree(&r);
    { Event *c = SyF(&st, L"c", S); SetStr(&c->summary, L"GammaLocal"); Touch(c); }
    g_cn.pushOut = SyCal(L"", SyEv(L"c", L"GammaLocal", L"", L"X-DKEYS-ERROR:not found\r\n")); g_cn.pullOut = SyCal(L"", SyEv(L"c", L"", L"", L"X-DKEYS-DELETED:1\r\n"));
    SyncOne(&st, S, L"rev=7", &r); CHECK(r.conflicts == 1 && SyF(&st, L"c", S) && !HasEtag(SyF(&st, L"c", S)) && Dirty(SyF(&st, L"c", S)) && SyHas(r.log.p, L"изменено здесь")); StatFree(&r);
    CnReset(); g_cn.pushOut = SyCal(L"", SyEv(L"c", L"GammaLocal", L"e9")); g_cn.pullOut = SyCal(L"");
    CHECK(SyncOne(&st, S, L"rev=8", &r) == 1 && SyHas(g_cn.lastPush, L"X-DKEYS-OP:create") && !Dirty(SyF(&st, L"c", S))); StatFree(&r);
    // --- конфликт: правили и тут, и на сервере (push отказал - conflict); побеждает более позднее изменение ---
    for (int round = 0; round < 2; round++) {
        StoreFree(&st); CnReset(); g_cn.pullOut = SyCal(L"", SyEv(L"d", L"Delta", L"d1")); SyncOne(&st, S, L"rev=8", &r); StatFree(&r);
        { Event *d = SyF(&st, L"d", S); SetStr(&d->summary, L"DeltaLocal"); Touch(d); }
        g_cn.pushOut = SyCal(L"", SyEv(L"d", L"DeltaLocal", L"", L"X-DKEYS-ERROR:conflict\r\n"));
        g_cn.pullOut = SyCal(L"", SyEv(L"d", L"DeltaServer", L"d2", L"", round == 0 ? L"20990101T000000Z" : L"20200101T000000Z"));
        SyncOne(&st, S, L"rev=9", &r);
        if (round == 0) CHECK(r.conflicts == 1 && !lstrcmpW(SyF(&st, L"d", S)->summary, L"DeltaServer") && !Dirty(SyF(&st, L"d", S)) && !r.repush && SyHas(r.log.p, L"версия сервера"));
        else {
            CHECK(r.conflicts == 1 && !lstrcmpW(SyF(&st, L"d", S)->summary, L"DeltaLocal") && Dirty(SyF(&st, L"d", S)) && r.repush && !lstrcmpW(SyF(&st, L"d", S)->etag, L"d2") && SyHas(r.log.p, L"местная версия"));
            SyncJob j; CHECK(PushBuild(&st, S, L"rev=9", &j) == 1 && SyHas(j.pushText, L"X-DKEYS-ETAG:d2") && SyHas(j.pushText, L"X-DKEYS-OP:update")); JobFree(&j);    // повторная отправка уже с новым etag
        }
        StatFree(&r);
    }
    // --- ошибки: нужное событие остаётся, сбой скрипта состояние не меняет ---
    CnReset(); g_cn.pushCode = -2; SyncOne(&st, S, L"rev=9", &r); CHECK(r.errKind == SYE_TIME && !r.ok && !r.cursor && Dirty(SyF(&st, L"d", S))); StatFree(&r);
    CnReset(); g_cn.pushCode = -1; SyncOne(&st, S, L"rev=9", &r); CHECK(r.errKind == SYE_RUN && !r.ok); StatFree(&r);
    CnReset(); g_cn.pushCode = 3; g_cn.err = Dup(L"\r\n  boom happened\r\nmore"); SyncOne(&st, S, L"rev=9", &r); CHECK(r.errKind == SYE_EXIT && r.errCode == 3 && !lstrcmpW(r.errText, L"boom happened") && !r.ok); StatFree(&r);
    CnReset(); g_cn.pushOut = SyCal(L""); g_cn.pullOut = SyCal(L"");                                    // push без ответа на событие
    SyncOne(&st, S, L"rev=9", &r); CHECK(r.ok && r.errKind == SYE_EVT && r.pushErr == 1 && Dirty(SyF(&st, L"d", S)) && r.cursor); StatFree(&r);
    CnReset(); g_cn.pushOut = SyCal(L"", SyEv(L"d", L"DeltaLocal", L"d3", L"X-DKEYS-ERROR:conflict\r\n")); g_cn.pullOut = SyCal(L"");
    SyncOne(&st, S, L"rev=9", &r); CHECK(r.ok && r.errKind == SYE_EVT && !lstrcmpW(r.errText, L"conflict") && Dirty(SyF(&st, L"d", S)) && !lstrcmpW(SyF(&st, L"d", S)->etag, L"d2") && SyHas(r.log.p, L"не отправлено")); StatFree(&r);
    CnReset(); g_cn.pushOut = SyCal(L"", SyEv(L"d", L"DeltaLocal", L"d4")); g_cn.pullOut = L"мусор, не ICS";
    SyncOne(&st, S, L"rev=9", &r); CHECK(r.errKind == SYE_BAD && !r.ok && !r.cursor && r.pushed == 1); StatFree(&r);   // push применился, pull - нет: курсор не двигается
    CnReset(); g_cn.pullOut = SyCal(L"", SyEv(L"x", L"NoEtag", L"")); SyncOne(&st, S, L"rev=9", &r); CHECK(r.errKind == SYE_BAD && !SyF(&st, L"x", S)); StatFree(&r);
    CnReset(); g_cn.pullCode = 5; g_cn.err = Dup(L"auth failed"); SyncOne(&st, S, L"rev=9", &r); CHECK(r.errKind == SYE_EXIT && !lstrcmpW(r.errText, L"auth failed")); StatFree(&r);
    CnReset(); g_cn.pullOut = SyCal(L"", L"BEGIN:VEVENT\r\nSUMMARY:no uid\r\nEND:VEVENT\r\n"); SyncOne(&st, S, L"rev=9", &r); CHECK(r.errKind == SYE_BAD); StatFree(&r);
    StoreFree(&st);
    // --- серии: главное событие и замены вхождений ---
    CnReset(); g_cn.pullOut = SyCal(L"", SyEv(L"m", L"Series", L"s1", L"RRULE:FREQ=WEEKLY\r\n"), SyEv(L"m", L"Moved", L"s1", L"RECURRENCE-ID:20261127T100000Z\r\n"));
    SyncOne(&st, S, L"", &r); CHECK(r.added == 2 && st.n == 2 && st.ev[1].hasRec); StatFree(&r);
    { Event *o = &st.ev[1]; SetStr(&o->summary, L"Moved2"); Touch(o); }
    CnReset(); g_cn.pushOut = SyCal(L"", SyEv(L"m", L"Series", L"s2", L"RRULE:FREQ=WEEKLY\r\n"), SyEv(L"m", L"Moved2", L"s2", L"RECURRENCE-ID:20261127T100000Z\r\n")); g_cn.pullOut = SyCal(L"");
    CHECK(SyncOne(&st, S, L"rev=1", &r) == 1 && CountLines(g_cn.lastPush, L"BEGIN:VEVENT") == 2 && CountLines(g_cn.lastPush, L"X-DKEYS-OP:update") == 2 && !Dirty(&st.ev[0]) && !Dirty(&st.ev[1]) && !lstrcmpW(st.ev[1].etag, L"s2")); StatFree(&r);
    g_cn.pullOut = SyCal(L"", SyEv(L"m", L"Series", L"s3", L"RRULE:FREQ=WEEKLY\r\n")); g_cn.pushOut = NULL;                            // замена пропала с сервера
    SyncOne(&st, S, L"rev=2", &r); CHECK(st.n == 1 && r.removed == 1 && !st.ev[0].hasRec && !lstrcmpW(st.ev[0].etag, L"s3")); StatFree(&r);
    g_cn.pullOut = SyCal(L"", SyEv(L"m", L"", L"", L"X-DKEYS-DELETED:1\r\n")); SyncOne(&st, S, L"rev=3", &r); CHECK(st.n == 0 && r.removed == 1); StatFree(&r);
    // --- полная выгрузка: чего нет в ответе - удаляется, несинхронизированное - создаётся заново; пустой ответ ничего не стирает ---
    CnReset(); g_cn.pullOut = SyCal(L"", SyEv(L"x", L"X", L"x1"), SyEv(L"y", L"Y", L"y1"), SyEv(L"z", L"Z", L"z1")); SyncOne(&st, S, L"", &r); StatFree(&r);
    { Event *z = SyF(&st, L"z", S); SetStr(&z->summary, L"Z2"); Touch(z); }
    g_cn.pushOut = SyCal(L"", SyEv(L"z", L"Z2", L"", L"X-DKEYS-ERROR:not found\r\n")); g_cn.pullOut = SyCal(L"X-DKEYS-CURSOR:rev=9\r\n", SyEv(L"x", L"X", L"x1"));
    SyncOne(&st, S, L"", &r); CHECK(r.removed == 1 && !SyF(&st, L"y", S) && SyF(&st, L"x", S) && SyF(&st, L"z", S) && !HasEtag(SyF(&st, L"z", S)) && r.conflicts == 1); StatFree(&r);
    StoreFree(&st);
    CnReset(); g_cn.pullOut = SyCal(L"", SyEv(L"x", L"X", L"x1"), SyEv(L"y", L"Y", L"y1"), SyEv(L"z", L"Z", L"z1")); SyncOne(&st, S, L"", &r); StatFree(&r);
    g_cn.pullOut = SyCal(L""); SyncOne(&st, S, L"", &r); CHECK(st.n == 3 && r.removed == 0 && SyHas(r.log.p, L"пропущено")); StatFree(&r);
    g_cn.pullOut = SyCal(L"X-DKEYS-FULL:1\r\n", SyEv(L"x", L"X", L"x1")); SyncOne(&st, S, L"rev=5", &r); CHECK(r.removed == 2 && st.n == 1); StatFree(&r);     // полный ответ по признаку FULL
    StoreFree(&st);
    // --- два источника: один UID независим; перенос - create в новом и delete в старом ---
    CnReset(); g_cn.pullOut = SyCal(L"", SyEv(L"dup", L"InA", L"a1")); SyncOne(&st, L"A", L"", &r); StatFree(&r);
    g_cn.pullOut = SyCal(L"", SyEv(L"dup", L"InB", L"b1")); SyncOne(&st, L"B", L"", &r); StatFree(&r);
    CHECK(st.n == 2 && !lstrcmpW(SyF(&st, L"dup", L"A")->summary, L"InA") && !lstrcmpW(SyF(&st, L"dup", L"B")->summary, L"InB"));
    g_cn.pullOut = SyCal(L"", SyEv(L"dup", L"InA2", L"a2")); SyncOne(&st, L"A", L"rev=1", &r); StatFree(&r);
    CHECK(!lstrcmpW(SyF(&st, L"dup", L"A")->summary, L"InA2") && !lstrcmpW(SyF(&st, L"dup", L"B")->summary, L"InB"));
    { int k = MoveEvent(&st, FindIn(&st, L"dup", L"A", FALSE, 0), L"B", FALSE, TRUE); CHECK(k >= 0 && !lstrcmpW(st.ev[k].source, L"B") && Dirty(&st.ev[k]) && !HasEtag(&st.ev[k]));
      SyncJob ja, jb; CHECK(PushBuild(&st, L"A", L"", &ja) == 1 && SyHas(ja.pushText, L"X-DKEYS-OP:delete") && SyHas(ja.pushText, L"X-DKEYS-ETAG:a2"));
      CHECK(PushBuild(&st, L"B", L"", &jb) == 1 && SyHas(jb.pushText, L"X-DKEYS-OP:create") && SyHas(jb.pushText, L"SUMMARY:InA2"));
      JobFree(&ja); JobFree(&jb); }
    StoreFree(&st);
    // --- форматы: synced/etag/deleted/hidden переживают запись и чтение; состояние источника; паузы ---
    { Event *e = StoreNew(&st); e->uid = Dup(L"k"); e->source = Dup(S); e->etag = Dup(L"et;ag"); e->seq = 4; e->synced = 3; e->hidden = e->deleted = TRUE; e->tzid = Dup(L"UTC");
      wchar_t *t = Serialize(&st, S); Store q; memset(&q, 0, sizeof q); Parse(&q, t, S); Free(t);
      CHECK(q.n == 1 && q.ev[0].seq == 4 && q.ev[0].synced == 3 && !lstrcmpW(q.ev[0].etag, L"et;ag") && q.ev[0].hidden && q.ev[0].deleted && Dirty(&q.ev[0])); StoreFree(&q); StoreFree(&st); }
    { wchar_t *t = SyncStateSave(L"rev=12", 1700000000), *c; I64 ok; SyncStateLoad(t, &c, &ok); CHECK(!lstrcmpW(c, L"rev=12") && ok == 1700000000); Free(t); Free(c);
      t = SyncStateSave(L"", 0); SyncStateLoad(t, &c, &ok); CHECK(!*c && ok == 0); Free(t); Free(c); SyncStateLoad(L"", &c, &ok); CHECK(!*c && ok == 0); Free(c); }
    CHECK(Backoff(0) == 0 && Backoff(1) == 60 && Backoff(2) == 120 && Backoff(3) == 240 && Backoff(7) == 3600 && Backoff(20) == 3600);
    { wchar_t t[40]; IsoFmt(t, W(2026, 11, 16, 9, 5, 7)); CHECK(!lstrcmpW(t, L"2026-11-16T09:05:07Z")); }
}

/* ---------- сквозные проверки: движок + настоящий stub.py ---------- */
static wchar_t g_py[MAX_PATH], g_stub[MAX_PATH], g_stubDir[MAX_PATH];
static int g_nrun;
static int CountFn(void *ctx, const wchar_t *verb, const wchar_t *since, const wchar_t *in, wchar_t **out, wchar_t **err) { g_nrun++; return ScriptRun(ctx, verb, since, in, out, err); }
static wchar_t *StubAdmin(const wchar_t *profile, const wchar_t *args, const wchar_t *in = NULL) {      // команда «другого клиента»; вывод - Free
    wchar_t cmd[900]; wsprintfW(cmd, L"\"%s\" \"%s\" admin %s --profile \"%s\"", g_py, g_stub, args, profile);
    int n = 0; char *u = in ? ToUtf8(in, &n) : NULL; ProcOut o; ProcRun(cmd, NULL, u, n, 20000, &o); Free(u);
    wchar_t *t = o.out ? FromUtf8(o.out, o.nout) : Dup(L""); Free(o.out); Free(o.err); return t;
}
struct Stub { Script sc; wchar_t src[8]; wchar_t *cursor; };
static void StubInit(Stub *s, const wchar_t *src, const wchar_t *profile) {
    memset(s, 0, sizeof *s); lstrcpyW(s->sc.python, g_py); lstrcpyW(s->sc.path, g_stub); lstrcpyW(s->sc.profile, profile); s->sc.timeoutMs = 60000; lstrcpyW(s->src, src); s->cursor = Dup(L"");
}
static int StubSync(Store *st, Stub *s, SyncStat *r) {                                                   // цикл как в программе: sync, при repush - ещё один push
    SyncJob j; int groups = PushBuild(st, s->src, s->cursor, &j); SyncOut o; SyncIO(&j, CountFn, &s->sc, &o); SyncApply(st, s->src, &j, &o, r); JobFree(&j); OutFree(&o);
    if (r->ok && r->cursor) { Free(s->cursor); s->cursor = Dup(r->cursor); }
    return groups;
}
static wchar_t *StubPull(const wchar_t *profile, const wchar_t *since) {
    Script sc; memset(&sc, 0, sizeof sc); lstrcpyW(sc.python, g_py); lstrcpyW(sc.path, g_stub); lstrcpyW(sc.profile, profile); sc.timeoutMs = 60000;
    wchar_t *o, *e; ScriptRun(&sc, L"pull", since, NULL, &o, &e); Free(e); return o;
}
static wchar_t *Lm(const wchar_t *uid, const wchar_t *sum, const wchar_t *lm = L"20261101T100000Z", const wchar_t *extra = L"") { return SyEv(uid, sum, L"", extra, lm); }

static void TestSyncStub(void) {
    wchar_t cmd[MAX_PATH + 40]; ProcOut o; lstrcpyW(g_py, L"python.exe"); wsprintfW(cmd, L"\"%s\" -c \"import sys\"", g_py);
    if (!ProcRun(cmd, NULL, NULL, 0, 20000, &o) || o.code != 0) { printf("python not found: end-to-end sync checks skipped\n"); return; }
    wchar_t me[MAX_PATH]; MultiByteToWideChar(CP_UTF8, 0, __FILE__, -1, me, MAX_PATH);                      // <repo>\tests\caltest.cpp
    int l = lstrlenW(me); while (l > 0 && me[l - 1] != L'\\' && me[l - 1] != L'/') l--; me[l] = 0;
    wsprintfW(g_stub, L"%s..\\calendars\\stub.py", me);
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp); wsprintfW(g_stubDir, L"%sdkeys_stub_%u", tmp, GetTickCount()); CreateDirectoryW(g_stubDir, NULL);
    SetEnvironmentVariableW(L"DKEYS_STUB_DIR", g_stubDir); SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
    Store st; memset(&st, 0, sizeof st); SyncStat r; Stub A, B; StubInit(&A, L"a", L"pa"); StubInit(&B, L"b", L"pb");
    // другой клиент создал событие -> оно появилось здесь; повторная синхронизация ничего не меняет
    { wchar_t *e = SyCal(L"", Lm(L"s1", L"Server1")); wchar_t *t = StubAdmin(L"pa", L"put", e); Free(t); }
    StubSync(&st, &A, &r); CHECK(r.ok && r.added == 1 && SyF(&st, L"s1", L"a") && !Dirty(SyF(&st, L"s1", L"a")) && HasEtag(SyF(&st, L"s1", L"a")) && !lstrcmpW(A.cursor, L"rev=1")); StatFree(&r);
    { wchar_t *before = Serialize(&st, NULL); g_nrun = 0; StubSync(&st, &A, &r); wchar_t *after = Serialize(&st, NULL);
      CHECK(r.ok && g_nrun == 1 && r.added + r.updated + r.removed + r.conflicts == 0 && !lstrcmpW(before, after)); Free(before); Free(after); StatFree(&r); }
    // создали здесь -> на сервере; правка здесь -> на сервере; удаление здесь -> на сервере
    SyNew(&st, L"loc1", L"a", L"Local1");
    CHECK(StubSync(&st, &A, &r) == 1 && r.ok && r.pushed == 1 && !Dirty(SyF(&st, L"loc1", L"a")) && HasEtag(SyF(&st, L"loc1", L"a"))); StatFree(&r);
    { wchar_t *sv = StubPull(L"pa", L""); CHECK(SyHas(sv, L"UID:loc1") && SyHas(sv, L"SUMMARY:Local1")); Free(sv); }
    { Event *e = SyF(&st, L"loc1", L"a"); SetStr(&e->summary, L"Local1b"); Touch(e); }
    CHECK(StubSync(&st, &A, &r) == 1 && r.ok && r.pushed == 1 && !Dirty(SyF(&st, L"loc1", L"a"))); StatFree(&r);
    { wchar_t *sv = StubPull(L"pa", L""); CHECK(SyHas(sv, L"SUMMARY:Local1b") && !SyHas(sv, L"SUMMARY:Local1\r\n")); Free(sv); }
    DeleteEvent(&st, FindIn(&st, L"loc1", L"a", FALSE, 0), TRUE, TRUE);
    CHECK(StubSync(&st, &A, &r) == 1 && r.ok && !SyF(&st, L"loc1", L"a")); StatFree(&r);
    { wchar_t *sv = StubPull(L"pa", L""); CHECK(!SyHas(sv, L"UID:loc1")); Free(sv); }
    // сервер удалил -> удалилось здесь
    { wchar_t *t = StubAdmin(L"pa", L"delete s1"); Free(t); } StubSync(&st, &A, &r); CHECK(r.removed == 1 && !SyF(&st, L"s1", L"a")); StatFree(&r);
    // конфликт: «другой клиент» изменил позже, чем мы - побеждает сервер; раньше - побеждает здесь и уходит на сервер
    { wchar_t *e = SyCal(L"", Lm(L"c1", L"Orig")); wchar_t *t = StubAdmin(L"pa", L"put", e); Free(t); StubSync(&st, &A, &r); StatFree(&r);
      Event *c = SyF(&st, L"c1", L"a"); SetStr(&c->summary, L"Mine"); Touch(c);
      e = SyCal(L"", Lm(L"c1", L"Theirs", L"20990101T000000Z")); t = StubAdmin(L"pa", L"put", e); Free(t);
      StubSync(&st, &A, &r); CHECK(r.conflicts == 1 && !lstrcmpW(SyF(&st, L"c1", L"a")->summary, L"Theirs") && !Dirty(SyF(&st, L"c1", L"a")) && SyHas(r.log.p, L"версия сервера")); StatFree(&r);
      c = SyF(&st, L"c1", L"a"); SetStr(&c->summary, L"Mine2"); Touch(c);
      e = SyCal(L"", Lm(L"c1", L"Old", L"20200101T000000Z")); t = StubAdmin(L"pa", L"put", e); Free(t);
      StubSync(&st, &A, &r); CHECK(r.conflicts == 1 && r.repush && !lstrcmpW(SyF(&st, L"c1", L"a")->summary, L"Mine2") && Dirty(SyF(&st, L"c1", L"a")) && SyHas(r.log.p, L"местная версия")); StatFree(&r);
      StubSync(&st, &A, &r); CHECK(r.ok && r.pushed == 1 && !Dirty(SyF(&st, L"c1", L"a"))); StatFree(&r);                     // повторная отправка уже с новым etag
      wchar_t *sv = StubPull(L"pa", L""); CHECK(SyHas(sv, L"SUMMARY:Mine2")); Free(sv); }
    // перенос между двумя источниками: в старом удалено, в новом создано
    SyNew(&st, L"mv", L"a", L"Mover"); StubSync(&st, &A, &r); StatFree(&r);
    { int k = MoveEvent(&st, FindIn(&st, L"mv", L"a", FALSE, 0), L"b", FALSE, TRUE); CHECK(k >= 0);
      StubSync(&st, &A, &r); CHECK(r.ok && r.pushed == 1 && !SyF(&st, L"mv", L"a")); StatFree(&r);
      StubSync(&st, &B, &r); CHECK(r.ok && r.pushed == 1); StatFree(&r);
      wchar_t *sa = StubPull(L"pa", L""), *sb = StubPull(L"pb", L""); CHECK(!SyHas(sa, L"UID:mv") && SyHas(sb, L"SUMMARY:Mover")); Free(sa); Free(sb);
      int nb = 0; for (int i = 0; i < st.n; i++) if (SameSrc(st.ev[i].source, L"b") && !lstrcmpW(st.ev[i].summary, L"Mover")) { nb++; CHECK(HasEtag(&st.ev[i]) && !Dirty(&st.ev[i])); } CHECK(nb == 1); }
    // серия: главное событие + замена вхождения приходят и уходят целиком
    { wchar_t *e = SyCal(L"", Lm(L"ser", L"Weekly", L"20261101T100000Z", L"RRULE:FREQ=WEEKLY\r\n"), Lm(L"ser", L"Moved", L"20261101T100000Z", L"RECURRENCE-ID:20261127T100000Z\r\n")); wchar_t *t = StubAdmin(L"pa", L"put", e); Free(t); }
    StubSync(&st, &A, &r); CHECK(r.added == 2); StatFree(&r);
    { int n = 0; for (int i = 0; i < st.n; i++) if (!lstrcmpW(st.ev[i].uid, L"ser")) n++; CHECK(n == 2); }
    { int mi = FindIn(&st, L"ser", L"a", FALSE, 0); SetStr(&st.ev[mi].summary, L"Weekly2"); Touch(&st.ev[mi]); }
    StubSync(&st, &A, &r); CHECK(r.ok && r.pushed == 1); StatFree(&r);
    { wchar_t *sv = StubPull(L"pa", L""); CHECK(SyHas(sv, L"SUMMARY:Weekly2") && SyHas(sv, L"SUMMARY:Moved") && CountLines(sv, L"RECURRENCE-ID") == 1); Free(sv); }
    // режимы сбоев сервера
    SyNew(&st, L"fl", L"a", L"Flaky"); wchar_t *t;
    t = StubAdmin(L"pa", L"mode error"); Free(t); StubSync(&st, &A, &r); CHECK(r.errKind == SYE_EXIT && r.errCode == 3 && SyHas(r.errText, L"simulated") && !r.ok && Dirty(SyF(&st, L"fl", L"a"))); StatFree(&r);
    t = StubAdmin(L"pa", L"mode garbage"); Free(t); StubSync(&st, &A, &r); CHECK(r.errKind == SYE_BAD && !r.ok && Dirty(SyF(&st, L"fl", L"a"))); StatFree(&r);
    t = StubAdmin(L"pa", L"mode failevents"); Free(t); StubSync(&st, &A, &r); CHECK(r.ok && r.errKind == SYE_EVT && r.pushErr == 1 && Dirty(SyF(&st, L"fl", L"a"))); StatFree(&r);
    t = StubAdmin(L"pa", L"mode hang"); Free(t); A.sc.timeoutMs = 1500; { DWORD t0 = GetTickCount(); StubSync(&st, &A, &r); CHECK(r.errKind == SYE_TIME && !r.ok && GetTickCount() - t0 < 15000 && Dirty(SyF(&st, L"fl", L"a"))); } StatFree(&r); A.sc.timeoutMs = 60000;
    t = StubAdmin(L"pa", L"mode newuid"); Free(t); StubSync(&st, &A, &r); CHECK(r.ok && r.errKind == 0 && !SyF(&st, L"fl", L"a") && SyF(&st, L"srv-fl", L"a") && !Dirty(SyF(&st, L"srv-fl", L"a"))); StatFree(&r);
    t = StubAdmin(L"pa", L"mode delay 1"); Free(t); StubSync(&st, &A, &r); CHECK(r.ok); StatFree(&r);
    t = StubAdmin(L"pa", L"mode none"); Free(t); StubSync(&st, &A, &r); CHECK(r.ok && r.errKind == 0); StatFree(&r);
    Free(A.cursor); Free(B.cursor); StoreFree(&st);
}
