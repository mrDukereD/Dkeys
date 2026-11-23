// cutter.h - нарезка видео и звука без перекодирования (перепаковка через Media Foundation).
// Режем только по ключевым кадрам видео: начало каждого куска - первый ключевой кадр не раньше
// запрошенного времени. B-кадры поддерживаются (пишем и PTS, и DTS). Без интерфейса, без CRT.
#ifndef CUTTER_H
#define CUTTER_H
#include <windows.h>
#ifndef CUT_NO_MF
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#endif

namespace CutCore {

struct Seg { long long a, b; };                                  // отрезок исходника, 100 нс (храним - вырезаем остальное)
struct Key { long long pts, dts; };                              // ключевой кадр видео: время показа и декодирования
struct Info {
    long long dur;                                               // длительность исходника, 100 нс
    int nkeys; Key *keys;                                        // ключевые кадры (HeapAlloc), освобождать FreeInfo
    int nvideo, naudio;
};
struct Job {
    volatile LONG *stop;                                         // != 0 - отменить
    void (*progress)(void *ctx, int pct);                        // 0..100
    void *ctx;
};

enum { CUT_OK = 0, CUT_OPEN = 1, CUT_NOSTREAM = 2, CUT_WRITER = 3, CUT_CANCEL = 4, CUT_WRITE = 5, CUT_FINAL = 6, CUT_EMPTY = 7 };

static void FreeInfo(Info *in) {
    if (in->keys) { HeapFree(GetProcessHeap(), 0, in->keys); in->keys = NULL; }
    in->nkeys = 0; in->nvideo = in->naudio = 0; in->dur = 0;
}
// Отмечаемый отрезок -> хранимые куски: [t0,t1] минус вырезанные [a,b]. Куски сортируются и склеиваются.
static int MakeSegs(long long t0, long long t1, const Seg *cuts, int ncut, Seg *out, int max) {
    if (t1 <= t0 || max <= 0) return 0;
    Seg c[64]; int n = 0;
    for (int i = 0; i < ncut && i < 64; i++) {                       // отбросить пустые и лишние, обрезать по [t0,t1]
        long long a = cuts[i].a < t0 ? t0 : cuts[i].a, b = cuts[i].b > t1 ? t1 : cuts[i].b;
        if (b > a) { c[n].a = a; c[n].b = b; n++; }
    }
    for (int i = 1; i < n; i++) {                                   // сортировка вставками
        Seg x = c[i]; int j = i - 1;
        while (j >= 0 && c[j].a > x.a) { c[j + 1] = c[j]; j--; }
        c[j + 1] = x;
    }
    int m = 0;
    for (int i = 1; i < n; i++) {                                   // склейка пересекающихся
        if (c[i].a <= c[m].b) { if (c[i].b > c[m].b) c[m].b = c[i].b; }
        else c[++m] = c[i];
    }
    int k = 0; long long pos = t0;
    if (n == 0) { if (t1 > t0) { out[0].a = t0; out[0].b = t1; return 1; } return 0; }
    for (int i = 0; i <= m && k < max; i++) {
        if (c[i].a > pos) { out[k].a = pos; out[k].b = c[i].a > t1 ? t1 : c[i].a; k++; }
        if (c[i].b > pos) pos = c[i].b;
    }
    if (k < max && t1 > pos) { out[k].a = pos; out[k].b = t1; k++; }
    return k;
}
// Первый ключевой кадр не раньше t (-1 - нет такого).
static long long SnapNext(const Info *in, long long t) {
    for (int i = 0; i < in->nkeys; i++) if (in->keys[i].pts >= t) return in->keys[i].pts;
    return -1;
}
static long long OutLen(const Seg *segs, int n) { long long s = 0; for (int i = 0; i < n; i++) s += segs[i].b - segs[i].a; return s; }

#ifndef CUT_NO_MF
struct Src { DWORD idx; BOOL video; IMFMediaType *nt; DWORD sink; IMFSample *cur; long long t, dts, fdur; BOOL eos; };
struct Ctx {
    IMFSourceReader *rd; IMFSinkWriter *wr; Src st[8]; int n; int vi;
    Key *keys; int nkeys, kcap; BOOL co; long long durTmp;
    Job *job; long long total, done; int pct;
};

static void Prog(Ctx *c, int pct) {
    if (pct > 100) pct = 100; if (pct < 0) pct = 0;
    if (pct == c->pct) return; c->pct = pct;
    if (c->job && c->job->progress) c->job->progress(c->job->ctx, pct);
}
static BOOL Stopped(Ctx *c) { return c->job && c->job->stop && *c->job->stop; }

static void Seek(Ctx *c, long long t) {
    PROPVARIANT p; PropVariantInit(&p); p.vt = VT_I8; p.hVal.QuadPart = t;
    c->rd->SetCurrentPosition(GUID_NULL, p);
}
static long long SDur(IMFSample *sm, long long def) { LONGLONG d = 0; if (SUCCEEDED(sm->GetSampleDuration(&d)) && d > 0) return d; return def; }

// Следующий сэмпл потока не раньше from; у видео при needKey - пропускать до ключевого кадра.
static BOOL Next(Ctx *c, Src *x, long long from, BOOL needKey) {
    if (x->cur) { x->cur->Release(); x->cur = NULL; }
    for (;;) {
        DWORD fl = 0, ai = 0; LONGLONG ts = 0; IMFSample *sm = NULL;
        HRESULT h = c->rd->ReadSample(x->idx, 0, &ai, &fl, &ts, &sm);
        if (FAILED(h)) return FALSE;
        if (fl & MF_SOURCE_READERF_ENDOFSTREAM) { if (sm) sm->Release(); x->eos = TRUE; return FALSE; }
        if (!sm) continue;
        if (ts < from) { sm->Release(); continue; }
        if (x->video && needKey) { UINT32 cp = 0; sm->GetUINT32(MFSampleExtension_CleanPoint, &cp); if (!cp) { sm->Release(); continue; } }
        x->cur = sm; x->t = ts; return TRUE;
    }
}
static BOOL AddKey(Ctx *c, long long pts, long long cum) {
    if (c->nkeys == c->kcap) {
        int nc = c->kcap ? c->kcap * 2 : 256; SIZE_T sz = (SIZE_T)nc * sizeof(Key);
        Key *p = c->keys ? (Key *)HeapReAlloc(GetProcessHeap(), 0, c->keys, sz) : (Key *)HeapAlloc(GetProcessHeap(), 0, sz);
        if (!p) return FALSE;
        c->keys = p; c->kcap = nc;
    }
    c->keys[c->nkeys].pts = pts; c->keys[c->nkeys].dts = cum; c->nkeys++;
    return TRUE;
}
// Один проход по видео: длительность, список ключевых кадров и DTS каждого (по сумме длительностей).
static int ScanVideo(Ctx *c, Src *v, long long *dur) {
    long long cum = 0, minOff = 0x7FFFFFFFFFFFFFFFLL, lastEnd = 0;
    Seek(c, 0);
    for (;;) {
        if (Stopped(c)) return CUT_CANCEL;
        DWORD fl = 0, ai = 0; LONGLONG ts = 0; IMFSample *sm = NULL;
        HRESULT h = c->rd->ReadSample(v->idx, 0, &ai, &fl, &ts, &sm);
        if (FAILED(h) || (fl & MF_SOURCE_READERF_ENDOFSTREAM)) { if (sm) sm->Release(); break; }
        if (!sm) continue;
        long long off = (long long)ts - cum; if (off < minOff) minOff = off;
        UINT32 cp = 0; sm->GetUINT32(MFSampleExtension_CleanPoint, &cp);
        if (cp && !AddKey(c, ts, cum)) { sm->Release(); return CUT_WRITER; }
        long long d = SDur(sm, v->fdur); cum += d; lastEnd = (long long)ts + d;
        sm->Release();
    }
    for (int i = 0; i < c->nkeys; i++) c->keys[i].dts += minOff;
    *dur = lastEnd; return CUT_OK;
}
// Длительность по первому звуковому потоку (видео нет).
static int ScanAudioDur(Ctx *c, Src *a, long long *dur) {
    long long last = 0; Seek(c, 0);
    for (;;) {
        if (Stopped(c)) return CUT_CANCEL;
        DWORD fl = 0, ai = 0; LONGLONG ts = 0; IMFSample *sm = NULL;
        HRESULT h = c->rd->ReadSample(a->idx, 0, &ai, &fl, &ts, &sm);
        if (FAILED(h) || (fl & MF_SOURCE_READERF_ENDOFSTREAM)) { if (sm) sm->Release(); break; }
        if (!sm) continue;
        long long e = (long long)ts + SDur(sm, 0); if (e > last) last = e;
        sm->Release();
    }
    *dur = last; return CUT_OK;
}
static void CloseCtx(Ctx *c) {
    for (int i = 0; i < c->n; i++) { if (c->st[i].cur) { c->st[i].cur->Release(); c->st[i].cur = NULL; } if (c->st[i].nt) { c->st[i].nt->Release(); c->st[i].nt = NULL; } }
    if (c->wr) { c->wr->Release(); c->wr = NULL; }
    if (c->rd) { c->rd->Release(); c->rd = NULL; }
}
static void Finish(Ctx *c) { CloseCtx(c); MFShutdown(); if (c->co) CoUninitialize(); }
static int Open(Ctx *c, const wchar_t *path) {
    HRESULT ci = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    BOOL mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    memset(c, 0, sizeof *c); c->vi = -1; c->co = SUCCEEDED(ci) ? TRUE : FALSE;
    if (!mf) { if (SUCCEEDED(ci)) CoUninitialize(); return CUT_OPEN; }
    if (FAILED(MFCreateSourceReaderFromURL(path, NULL, &c->rd))) { Finish(c); return CUT_OPEN; }
    // Отбор потоков: только видео и звук. Выбираем все сразу - иначе чтение "невыбранного" даёт MF_E_INVALIDREQUEST.
    c->rd->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    for (DWORD i = 0; i < 8; i++) {
        IMFMediaType *t = NULL;
        if (FAILED(c->rd->GetNativeMediaType(i, 0, &t))) continue;
        GUID mj; t->GetGUID(MF_MT_MAJOR_TYPE, &mj);
        if (mj != MFMediaType_Audio && mj != MFMediaType_Video) { t->Release(); continue; }
        Src *x = &c->st[c->n++];
        x->idx = i; x->video = mj == MFMediaType_Video; x->nt = t; x->fdur = 333333; x->dts = 0; x->cur = NULL; x->eos = FALSE;
        if (x->video) {
            UINT32 num = 0, den = 0;
            if (SUCCEEDED(MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &num, &den)) && num) x->fdur = 10000000LL * den / num;
            if (c->vi < 0) c->vi = c->n - 1;
        }
        c->rd->SetStreamSelection(i, TRUE);
        c->rd->SetCurrentMediaType(i, NULL, t);
    }
    if (!c->n) { Finish(c); return CUT_NOSTREAM; }
    return CUT_OK;
}

// Сведения о файле: длительность и ключевые кадры (для таймлайна).
static int Scan(const wchar_t *path, Info *in) {
    memset(in, 0, sizeof *in);
    Ctx c; int rc = Open(&c, path);
    if (rc != CUT_OK) return rc;
    for (int i = 0; i < c.n; i++) { if (c.st[i].video) in->nvideo++; else in->naudio++; }
    if (c.vi >= 0) rc = ScanVideo(&c, &c.st[c.vi], &in->dur);
    else {
        for (int i = 0; i < c.n; i++) if (!c.st[i].video) { rc = ScanAudioDur(&c, &c.st[i], &in->dur); break; }
    }
    in->keys = c.keys; in->nkeys = c.nkeys;                           // передаём массивы вызывающему
    c.keys = NULL; c.nkeys = c.kcap = 0;
    Finish(&c);
    if (rc != CUT_OK) { FreeInfo(in); return rc; }
    return CUT_OK;
}

static int WriteSeg(Ctx *c, int g, const Seg *segs, long long *acc) {
    Seg sg = segs[g];
    long long start = sg.a, origin = sg.a;
    if (c->vi >= 0) {
        int k = -1;
        for (int i = 0; i < c->nkeys; i++) if (c->keys[i].pts >= sg.a) { k = i; break; }
        if (k < 0 || c->keys[k].pts >= sg.b) return CUT_OK;           // после последнего ключевого кадра резать нечего
        start = c->keys[k].pts; origin = c->keys[k].dts;
        c->st[c->vi].dts = c->keys[k].dts;
    }
    long long end = sg.b; if (end <= start) return CUT_OK;
    long long base = *acc; *acc += end - origin;
    if (*acc > c->total) c->total = *acc;
    Seek(c, origin);
    for (int i = 0; i < c->n; i++) Next(c, &c->st[i], c->st[i].video ? start : origin, c->st[i].video);
    for (;;) {
        if (Stopped(c)) return CUT_CANCEL;
        int best = -1;
        for (int i = 0; i < c->n; i++) { if (!c->st[i].cur || c->st[i].t >= end) continue; if (best < 0 || c->st[i].t < c->st[best].t) best = i; }
        if (best < 0) break;
        Src *x = &c->st[best];
        long long out = x->t - origin + base;
        x->cur->SetSampleTime(out);
        long long dur = 0;
        if (x->video) { x->cur->SetUINT64(MFSampleExtension_DecodeTimestamp, (UINT64)(x->dts - origin + base)); dur = SDur(x->cur, x->fdur); }
        HRESULT h = c->wr->WriteSample(x->sink, x->cur);
        if (FAILED(h)) return CUT_WRITE;
        if (x->video) x->dts += dur;
        if (c->total > 0) Prog(c, (int)((x->t - origin + base) * 100 / c->total));
        Next(c, x, x->video ? start : origin, FALSE);
    }
    return CUT_OK;
}

// Нарезать: keep - что оставить (времена исходника, начало кусков округляется вперёд до ключевого кадра).
static int Remux(const wchar_t *src, const wchar_t *dst, const Seg *segs, int nseg, Job *job) {
    if (nseg <= 0) return CUT_EMPTY;
    Ctx c; int rc = Open(&c, src);
    if (rc != CUT_OK) return rc;
    c.job = job; c.total = OutLen(segs, nseg); c.done = 0; c.pct = -1;
    if (c.vi >= 0) rc = ScanVideo(&c, &c.st[c.vi], &c.durTmp);
    if (rc != CUT_OK) { if (c.keys) HeapFree(GetProcessHeap(), 0, c.keys); Finish(&c); return rc; }
    if (lstrcmpiW(src, dst) && GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES) DeleteFileW(dst);
    if (FAILED(MFCreateSinkWriterFromURL(dst, NULL, NULL, &c.wr))) { if (c.keys) HeapFree(GetProcessHeap(), 0, c.keys); Finish(&c); return CUT_WRITER; }
    // Все потоки - на выход с тем же типом (passthrough, без перекодирования).
    for (int i = 0; i < c.n; i++) {
        IMFMediaType *ot = NULL, *it = NULL;
        HRESULT h = MFCreateMediaType(&ot);
        if (SUCCEEDED(h)) h = c.st[i].nt->CopyAllItems(ot);
        if (SUCCEEDED(h)) h = MFCreateMediaType(&it);
        if (SUCCEEDED(h)) h = c.st[i].nt->CopyAllItems(it);
        if (SUCCEEDED(h)) h = c.wr->AddStream(ot, &c.st[i].sink);
        if (SUCCEEDED(h)) h = c.wr->SetInputMediaType(c.st[i].sink, it, NULL);
        if (ot) ot->Release(); if (it) it->Release();
        if (FAILED(h)) { if (c.keys) HeapFree(GetProcessHeap(), 0, c.keys); Finish(&c); DeleteFileW(dst); return CUT_WRITER; }
    }
    if (FAILED(c.wr->BeginWriting())) { if (c.keys) HeapFree(GetProcessHeap(), 0, c.keys); Finish(&c); DeleteFileW(dst); return CUT_WRITER; }
    long long acc = 0;
    for (int g = 0; g < nseg; g++) {
        int r = WriteSeg(&c, g, segs, &acc);
        if (r != CUT_OK) { c.wr->Finalize(); if (c.keys) HeapFree(GetProcessHeap(), 0, c.keys); Finish(&c); DeleteFileW(dst); return r; }
        Prog(&c, (g + 1) * 100 / nseg);
    }
    HRESULT hf = c.wr->Finalize();
    if (c.keys) HeapFree(GetProcessHeap(), 0, c.keys);
    Finish(&c);
    if (FAILED(hf)) { DeleteFileW(dst); return CUT_FINAL; }
    Prog(&c, 100);
    return CUT_OK;
}
// Форма волны: пики 0..255 по n корзинам для первого звукового потока (PCM без пересэмплинга).
static int Waveform(const wchar_t *path, long long dur, unsigned char *peaks, int n) {
    if (!peaks || n <= 0) return CUT_EMPTY;
    memset(peaks, 0, n);
    HRESULT ci = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) { if (SUCCEEDED(ci)) CoUninitialize(); return CUT_OPEN; }
    IMFSourceReader *rd = NULL; int rc = SUCCEEDED(MFCreateSourceReaderFromURL(path, NULL, &rd)) ? CUT_OK : CUT_OPEN;
    DWORD ai = (DWORD)-1; UINT32 rate = 48000, ch = 2;
    if (rc == CUT_OK) {
        for (DWORD i = 0; i < 8; i++) {
            IMFMediaType *t = NULL;
            if (FAILED(rd->GetNativeMediaType(i, 0, &t))) continue;
            GUID mj; t->GetGUID(MF_MT_MAJOR_TYPE, &mj);
            if (mj == MFMediaType_Audio) {
                ai = i;
                if (FAILED(t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate)) || !rate) rate = 48000;
                if (FAILED(t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch)) || !ch) ch = 2;
                t->Release(); break;
            }
            t->Release();
        }
        if (ai == (DWORD)-1) rc = CUT_NOSTREAM;
    }
    if (rc == CUT_OK) {
        rd->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        rd->SetStreamSelection(ai, TRUE);
        IMFMediaType *pcm = NULL;
        if (SUCCEEDED(MFCreateMediaType(&pcm))) {
            pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio); pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16); pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
            pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch); pcm->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2 * ch);
            pcm->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * 2 * ch);
            rc = SUCCEEDED(rd->SetCurrentMediaType(ai, NULL, pcm)) ? CUT_OK : CUT_OPEN;
            pcm->Release();
        } else rc = CUT_OPEN;
    }
    long long total = dur > 0 ? dur * rate / 10000000 : 0;            // кадров звука всего
    long long done = 0;
    while (rc == CUT_OK) {
        DWORD fl = 0, idx = 0; LONGLONG ts = 0; IMFSample *sm = NULL;
        if (FAILED(rd->ReadSample(ai, 0, &idx, &fl, &ts, &sm))) break;
        if (fl & MF_SOURCE_READERF_ENDOFSTREAM) { if (sm) sm->Release(); break; }
        if (!sm) continue;
        IMFMediaBuffer *mb = NULL;
        if (SUCCEEDED(sm->ConvertToContiguousBuffer(&mb)) && mb) {
            BYTE *p = NULL; DWORD len = 0;
            if (SUCCEEDED(mb->Lock(&p, NULL, &len)) && p) {
                short *q = (short *)p; DWORD frames = len / (2 * ch);
                for (DWORD k = 0; k < frames; k++) {
                    int v = 0;
                    for (UINT32 cc = 0; cc < ch; cc++) { int a = q[k * ch + cc]; if (a < 0) a = -a; if (a > v) v = a; }
                    int b = total > 0 ? (int)((done + k) * n / total) : 0;
                    if (b < 0) b = 0; if (b >= n) b = n - 1;
                    unsigned char pv = (unsigned char)(v / 128); if (pv > peaks[b]) peaks[b] = pv;
                }
                done += frames;
                mb->Unlock();
            }
            mb->Release();
        }
        sm->Release();
        if (total > 0 && done >= total) break;
    }
    for (int i = 0; i < n; i++) { int v = peaks[i], q = v * 255, r = 0; while ((r + 1) * (r + 1) <= q) r++; peaks[i] = (unsigned char)r; }   // sqrt: тихое видно, громкое не заливает всё
    if (rd) rd->Release();
    MFShutdown(); if (SUCCEEDED(ci)) CoUninitialize();
    return rc;
}
#else
static int Scan(const wchar_t *, Info *) { return CUT_OPEN; }
static int Remux(const wchar_t *, const wchar_t *, const Seg *, int, Job *) { return CUT_EMPTY; }
static int Waveform(const wchar_t *, long long, unsigned char *, int) { return CUT_OPEN; }
#endif
}   // namespace CutCore
#endif







