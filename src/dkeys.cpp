// DKeys - крошечный трей-комбайн: мульти-буфер обмена + хоткеи + быстрый запуск.
// Чистый WinAPI, без CRT. Сборка: build.cmd
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#define NO_DSHOW_STRSAFE
#include <dshow.h>
#include <stdarg.h>

#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "uuid.lib")

#pragma optimize("", off)
extern "C" {
void *memset(void *d, int c, size_t n) { BYTE *p = (BYTE *)d; while (n--) *p++ = (BYTE)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { BYTE *p = (BYTE *)d; const BYTE *q = (const BYTE *)s; while (n--) *p++ = *q++; return d; }
int memcmp(const void *a, const void *b, size_t n) {
    const BYTE *p = (const BYTE *)a, *q = (const BYTE *)b;
    while (n--) { if (*p != *q) return *p < *q ? -1 : 1; p++; q++; }
    return 0;
}
void *memmove(void *d, const void *s, size_t n) {
    BYTE *p = (BYTE *)d; const BYTE *q = (const BYTE *)s;
    if (p < q) { while (n--) *p++ = *q++; } else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}
}
#pragma optimize("", on)

#define DKMARK ((ULONG_PTR)0x444B4559)
enum { DM_CTRL = 1, DM_ALT = 2, DM_SHIFT = 4, DM_WIN = 8 };
enum { A_NONE, A_COPY, A_PASTE, A_BUFFER, A_HIST, A_LAUNCH, A_RUN, A_ADMIN, A_PY, A_PYRUN, A_TOPMOST, A_TOTRAY, A_LAYOUT, A_PAUSE, A_WEBCAM };
enum { PM_BUF, PM_HIST, PM_LAUNCH };
enum {
    WM_APP_HOT = WM_APP + 1, WM_APP_KEY, WM_APP_CLOSE, WM_APP_PCLICK, WM_APP_PTAB, WM_TRAY, WM_APP_RENDERED,
    TM_MOUSE = WM_APP + 20, TM_REHOOK
};
#define TID_RESTORE 1
#define TID_POPWATCH 2
#define TID_LAYOUT 3
#define TID_POPFG 4
#define TID_TRAY 5
#define VISROWS 10

struct Bind { UINT mods, key; int act, n, n2; wchar_t *arg, *dir; };
struct KV { wchar_t *k, *v; };
struct Row { wchar_t *label, *title, *text; int ref; };
struct Snap { UINT fmt[64]; HGLOBAL h[64]; int n; };
struct Buf { wchar_t *p; int n, cap; };
static void CamOpen(void);
static BOOL RegClass(const wchar_t *name, WNDPROC proc, UINT style);
struct BB { BYTE *p; DWORD n, cap; };

/* ---------- globals ---------- */
static HANDLE g_heap;
static HINSTANCE g_inst;
static HWND g_hwnd, g_hwndPop, g_hwndToast, g_popFg; static int g_popGrace;
static HWND g_hwndClip;   // владелец буфера обмена: окно потока хука, который всегда качает сообщения
static wchar_t g_dir[MAX_PATH], g_ini[MAX_PATH], g_memDir[MAX_PATH], g_pyDir[MAX_PATH], g_exe[MAX_PATH];
static volatile LONG g_modBits, g_popup, g_paused, g_modal;
static volatile LONG g_caps;
static DWORD g_hookTid;
static UINT g_msgTaskbar;
static BOOL g_elev;
static HICON g_icon;
static DWORD g_ignoreSeq, g_ownSeq;

static int g_histOn, g_histMax = 50, g_pasteDelay = 300, g_copyTimeout = 600, g_pyPause = 1, g_toasts = 1;
static wchar_t g_python[MAX_PATH] = L"python.exe";

static wchar_t *g_buf[10][10]; static int g_bufN[10];   // буферы 1..9, в каждом до 10 записей, новые сверху
static int g_bufMax = 10, g_curBuf = 1;
static int g_layoutWord, g_layoutSwitch = 1;
static wchar_t **g_hist; static int g_nhist, g_histCap;
static KV *g_launch; static int g_nlaunch;

static Bind *g_bind; static int g_nbind;
static Bind *g_bnew; static int g_nbnew, g_bnewCap;
static SRWLOCK g_bindLock = SRWLOCK_INIT;

static HWND g_hid[32]; static int g_nhid;
static Snap g_ps; static BOOL g_psPending;
static wchar_t *g_pasteTxt; static SRWLOCK g_pasteLock = SRWLOCK_INIT;   // текст вставки, отдаётся приложению по требованию (WM_RENDERFORMAT)

/* ---------- util ---------- */
static void *Alloc(SIZE_T n) { return HeapAlloc(g_heap, HEAP_ZERO_MEMORY, n ? n : 1); }
static void *Realloc(void *p, SIZE_T n) { return p ? HeapReAlloc(g_heap, HEAP_ZERO_MEMORY, p, n) : Alloc(n); }
static void Free(void *p) { if (p) HeapFree(g_heap, 0, p); }
static wchar_t *DupN(const wchar_t *s, int n) { wchar_t *p = (wchar_t *)Alloc((n + 1) * sizeof(wchar_t)); memcpy(p, s, n * sizeof(wchar_t)); return p; }
static wchar_t *Dup(const wchar_t *s) { if (!s) s = L""; return DupN(s, lstrlenW(s)); }
static void Cat(wchar_t *d, int cap, const wchar_t *s) { int n = lstrlenW(d); lstrcpynW(d + n, s, cap - n); }

static void BufAdd(Buf *b, const wchar_t *s, int n = -1) {
    if (n < 0) n = lstrlenW(s);
    if (b->n + n + 1 > b->cap) {
        int c = b->cap ? b->cap : 256;
        while (c < b->n + n + 1) c *= 2;
        b->p = (wchar_t *)Realloc(b->p, c * sizeof(wchar_t)); b->cap = c;
    }
    memcpy(b->p + b->n, s, n * sizeof(wchar_t)); b->n += n; b->p[b->n] = 0;
}
static void BBAdd(BB *b, const void *s, DWORD n) {
    if (b->n + n > b->cap) {
        DWORD c = b->cap ? b->cap : 1024;
        while (c < b->n + n) c *= 2;
        b->p = (BYTE *)Realloc(b->p, c); b->cap = c;
    }
    memcpy(b->p + b->n, s, n); b->n += n;
}
static int ToInt(const wchar_t *s) {
    while (*s == L' ') s++;
    int sg = 1; if (*s == L'-') { sg = -1; s++; }
    int v = 0; while (*s >= L'0' && *s <= L'9') v = v * 10 + (*s++ - L'0');
    return v * sg;
}
static wchar_t Low(wchar_t c) { return (wchar_t)(UINT_PTR)CharLowerW((LPWSTR)(UINT_PTR)c); }
static BOOL ContainsI(const wchar_t *h, const wchar_t *nd) {
    if (!*nd) return TRUE;
    int nl = lstrlenW(nd);
    for (; *h; h++) {
        int i = 0;
        while (i < nl && h[i] && Low(h[i]) == Low(nd[i])) i++;
        if (i == nl) return TRUE;
    }
    return FALSE;
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
    wchar_t tmp[MAX_PATH + 8];
    lstrcpynW(tmp, path, MAX_PATH); Cat(tmp, MAX_PATH + 8, L".tmp");
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD w = 0;
    BOOL ok = WriteFile(h, data, size, &w, NULL) && w == size;
    if (ok) FlushFileBuffers(h);
    CloseHandle(h);
    if (ok) ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(tmp);
    return ok;
}
static void WriteText(const wchar_t *path, const wchar_t *text) {
    int n; char *u = ToUtf8(text, &n);
    WriteAtomic(path, u, n); Free(u);
}
static void MemPath(wchar_t *out, const wchar_t *name) { lstrcpynW(out, g_memDir, MAX_PATH); Cat(out, MAX_PATH, name); }

static BOOL IsDark(void) {
    DWORD v = 1, sz = 4;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &sz);
    return v == 0;
}

#ifdef DKDEBUG
static void Log(const wchar_t *fmt, ...) {
    wchar_t b[600], p[MAX_PATH]; va_list ap; va_start(ap, fmt); int n = wvsprintfW(b, fmt, ap); va_end(ap);
    lstrcpyW(b + n, L"\r\n"); n += 2;
    GetTempPathW(MAX_PATH, p); lstrcatW(p, L"dkeys.log");
    char u[1300]; int l = WideCharToMultiByte(CP_UTF8, 0, b, n, u, 1300, NULL, NULL);
    HANDLE h = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, u, l, &w, NULL); CloseHandle(h); }
}
#else
#define Log(...) ((void)0)
#endif

/* ---------- toast ---------- */
static wchar_t g_toastText[200];
static HFONT g_toastFont;
static BOOL g_toastDark;

static LRESULT CALLBACK ToastProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(g_toastDark ? RGB(45, 45, 45) : RGB(250, 250, 250));
        HBRUSH bd = CreateSolidBrush(g_toastDark ? RGB(90, 90, 90) : RGB(160, 160, 160));
        FillRect(dc, &rc, bg); FrameRect(dc, &rc, bd);
        DeleteObject(bg); DeleteObject(bd);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g_toastDark ? RGB(235, 235, 235) : RGB(30, 30, 30));
        SelectObject(dc, g_toastFont);
        DrawTextW(dc, g_toastText, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        EndPaint(h, &ps); return 0;
    }
    case WM_TIMER: KillTimer(h, 1); ShowWindow(h, SW_HIDE); return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_NCHITTEST: return HTTRANSPARENT;
    }
    return DefWindowProcW(h, m, w, l);
}
static void Toast(const wchar_t *t) {
    if (!g_toasts) return;
    int dpi = GetDpiForSystem();
    if (!g_toastFont) g_toastFont = CreateFontW(-MulDiv(11, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    if (!g_hwndToast)
        g_hwndToast = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"DKToast", L"", WS_POPUP, 0, 0, 10, 10, NULL, NULL, g_inst, NULL);
    lstrcpynW(g_toastText, t, 200);
    g_toastDark = IsDark();
    HDC dc = GetDC(NULL); HGDIOBJ o = SelectObject(dc, g_toastFont);
    SIZE sz; GetTextExtentPoint32W(dc, g_toastText, lstrlenW(g_toastText), &sz);
    SelectObject(dc, o); ReleaseDC(NULL, dc);
    int w = sz.cx + MulDiv(36, dpi, 96), h = sz.cy + MulDiv(18, dpi, 96);
    POINT pt; GetCursorPos(&pt);
    MONITORINFO mi; mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    int x = (mi.rcWork.left + mi.rcWork.right - w) / 2, y = mi.rcWork.bottom - h - MulDiv(60, dpi, 96);
    SetWindowPos(g_hwndToast, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_hwndToast, NULL, TRUE);
    SetTimer(g_hwndToast, 1, 1100, NULL);
}

/* ---------- INI ---------- */
typedef void (*IniCb)(const wchar_t *sect, const wchar_t *key, const wchar_t *val);

static void IniParse(wchar_t *text, IniCb cb) {
    wchar_t sect[64] = L"";
    wchar_t *p = text;
    while (*p) {
        wchar_t *ls = p; while (*p && *p != L'\n') p++;
        wchar_t *le = p; if (*p) p++;
        while (le > ls && le[-1] == L'\r') le--;
        while (ls < le && (*ls == L' ' || *ls == L'\t')) ls++;
        while (le > ls && (le[-1] == L' ' || le[-1] == L'\t')) le--;
        if (ls == le || *ls == L';' || *ls == L'#') continue;
        if (*ls == L'[') {
            wchar_t *r = ls + 1; int i = 0;
            while (r < le && *r != L']' && i < 63) sect[i++] = *r++;
            sect[i] = 0; continue;
        }
        wchar_t *eq = ls; while (eq < le && *eq != L'=') eq++;
        if (eq == le) continue;
        wchar_t *ke = eq; while (ke > ls && (ke[-1] == L' ' || ke[-1] == L'\t')) ke--;
        wchar_t *vs = eq + 1; while (vs < le && (*vs == L' ' || *vs == L'\t')) vs++;
        wchar_t save = *le; *ke = 0; *le = 0;
        cb(sect, ls, vs);
        *le = save;
    }
}

static void IniSet(const wchar_t *sect, const wchar_t *key, const wchar_t *val) {
    DWORD sz = 0; BYTE *raw = ReadAll(g_ini, &sz);
    wchar_t *t = raw ? FromUtf8((char *)raw, sz) : Dup(L"");
    Free(raw);
    Buf out = {0};
    BOOL inSect = FALSE, done = FALSE, found = FALSE;
    int klen = lstrlenW(key), slen = lstrlenW(sect);
    wchar_t *p = t;
    while (*p) {
        wchar_t *ls = p; while (*p && *p != L'\n') p++;
        wchar_t *le = p; BOOL hasNl = (*p == L'\n'); if (hasNl) p++;
        wchar_t *ce = le; if (ce > ls && ce[-1] == L'\r') ce--;
        wchar_t *a = ls; while (a < ce && (*a == L' ' || *a == L'\t')) a++;
        if (a < ce && *a == L'[') {
            if (inSect && !done) { BufAdd(&out, key); BufAdd(&out, L"="); BufAdd(&out, val); BufAdd(&out, L"\r\n"); done = TRUE; }
            wchar_t *r = a + 1; wchar_t *rs = r;
            while (r < ce && *r != L']') r++;
            inSect = ((r - rs) == slen && CompareStringOrdinal(rs, slen, sect, slen, TRUE) == CSTR_EQUAL);
            if (inSect) found = TRUE;
            BufAdd(&out, ls, (int)(p - ls)); continue;
        }
        if (inSect && !done && a < ce && *a != L';' && *a != L'#') {
            wchar_t *eq = a; while (eq < ce && *eq != L'=') eq++;
            if (eq < ce) {
                wchar_t *ke = eq; while (ke > a && (ke[-1] == L' ' || ke[-1] == L'\t')) ke--;
                if ((ke - a) == klen && CompareStringOrdinal(a, klen, key, klen, TRUE) == CSTR_EQUAL) {
                    BufAdd(&out, key); BufAdd(&out, L"="); BufAdd(&out, val);
                    if (hasNl) BufAdd(&out, L"\r\n");
                    done = TRUE; continue;
                }
            }
        }
        BufAdd(&out, ls, (int)(p - ls));
    }
    if (inSect && !done) {
        if (out.n && out.p[out.n - 1] != L'\n') BufAdd(&out, L"\r\n");
        BufAdd(&out, key); BufAdd(&out, L"="); BufAdd(&out, val); BufAdd(&out, L"\r\n");
    } else if (!found) {
        if (out.n && out.p[out.n - 1] != L'\n') BufAdd(&out, L"\r\n");
        BufAdd(&out, L"["); BufAdd(&out, sect); BufAdd(&out, L"]\r\n");
        BufAdd(&out, key); BufAdd(&out, L"="); BufAdd(&out, val); BufAdd(&out, L"\r\n");
    }
    if (out.p) WriteText(g_ini, out.p);
    Free(out.p); Free(t);
}

static const char *kIni1 = u8R"INI(; DKeys - настройки. Комментарии - только на отдельных строках (начинаются с ; или #).
; После правки: значок в трее -> "Перечитать настройки".
;
; [Hotkeys]  Клавиши = действие
;   Модификаторы: Win Ctrl Alt Shift
;   Клавиши: A-Z 0-9 F1-F24 Num0-Num9 NumDot NumMul NumAdd NumSub NumDiv Space Enter Tab Esc Back
;            Ins Del Home End PgUp PgDn Left Right Up Down Tilde Minus Equals Comma Period Slash
;            Backslash Semicolon Quote LBracket RBracket PrintScreen Pause Apps
;   Действия:
;     copy N                    выделенное - в буфер N (1-9), записью наверх; основной буфер Windows не трогается
;     buffer [N]                меню буфера N (без N - последний): цифра 1-9,0 или Enter вставляет запись
;     paste N M                 сразу вставить запись M (1-10) буфера N
;     history                   меню истории (работает при HistoryEnabled=1, иначе клавиша уходит системе)
;     launcher                  меню быстрого запуска (секция [Launcher])
;     layout                    исправить раскладку: последний набранный текст, а если его нет - выделенный (строка, если выделения нет);
;                               повторное нажатие сразу после этого возвращает текст обратно
;     run КОМАНДА [@ ПАПКА]     запустить программу/файл/ссылку (ПАПКА - рабочая папка)
;     admin КОМАНДА [@ ПАПКА]   то же от администратора
;     py СКРИПТ [арг] [@ ПАПКА] скрипт из папки py. Выделенный текст: переменные окружения DKEYS_SEL и DKEYS_SEL_FILE
;     pyrun                     выделенный текст выполнить как скрипт Python (py\tmp.py)
;     topmost                   окно "поверх всех" вкл/выкл
;     totray                    спрятать окно (вернуть - из меню значка в трее)
;     pause                     пауза/продолжить хоткеи
;     webcam                    окно предпросмотра веб-камеры (есть и в меню значка)
; Win+цифры буферов перекрывают запуск закреплённых программ на панели задач.
; В меню буфера: Del - удалить, F2 - править, Ins - новая запись, Ctrl+Вверх/Вниз - переставить,
; Влево/Вправо или Win+цифра - другой буфер, Ctrl+цифра - скопировать запись в буфер, Tab - история/запуск.

[General]
; Своя история буфера (Win+V). 0 - выключена и Win+V работает как в Windows. Переключается из меню в трее.
HistoryEnabled=0
HistoryMax=50
; Записей в каждом буфере 1-9 (не больше 10: цифры 1-9 и 0)
BufferSize=10
; Через сколько мс после того, как приложение забрало вставляемый текст, возвращать прежнее содержимое буфера
PasteDelay=300
; Сколько мс ждать, пока приложение скопирует выделенное
CopyTimeout=600
; Смена раскладки: 0 - весь набранный текст (с последнего Enter/клика/стрелки), 1 - только последнее слово
LayoutWord=0
; После смены раскладки текста переключить и раскладку клавиатуры
LayoutSwitch=1
; Чем запускать py-макросы: python.exe, pythonw.exe (без окна) или полный путь
Python=python.exe
; 1 - после pyrun окно остаётся открытым (pause)
PyRunPause=1
; Всплывающие подсказки внизу экрана
Toasts=1

[Hotkeys]
Ctrl+Num1=copy 1
Ctrl+Num2=copy 2
Ctrl+Num3=copy 3
Ctrl+Num4=copy 4
Ctrl+Num5=copy 5
Ctrl+Num6=copy 6
Ctrl+Num7=copy 7
Ctrl+Num8=copy 8
Ctrl+Num9=copy 9
Win+Num1=buffer 1
Win+Num2=buffer 2
Win+Num3=buffer 3
Win+Num4=buffer 4
Win+Num5=buffer 5
Win+Num6=buffer 6
Win+Num7=buffer 7
Win+Num8=buffer 8
Win+Num9=buffer 9
Win+Num0=buffer
Win+V=history
Win+Tilde=launcher
Ctrl+Tilde=layout
Win+C=run calc.exe
Win+O=topmost
Win+NumMul=totray
Win+W=run python.exe wm.py @ D:\woodman
Win+Y=pyrun
)INI";
static const char *kIni2 = u8R"INI(
[Launcher]
Калькулятор=calc.exe
Блокнот=notepad.exe
Диспетчер задач=taskmgr.exe
Командная строка=cmd.exe
PowerShell=powershell.exe
Редактор реестра=regedit.exe
Редактор групповой политики=admin gpedit.msc
Службы=admin services.msc
Диспетчер устройств=devmgmt.msc
Управление дисками=admin diskmgmt.msc
Управление компьютером=compmgmt.msc
Планировщик заданий=taskschd.msc
Просмотр событий=eventvwr.msc
Локальные пользователи и группы=lusrmgr.msc
Монитор ресурсов=resmon.exe
Конфигурация системы=msconfig.exe
Сведения о системе=msinfo32.exe
Панель управления=control.exe
Программы и компоненты=appwiz.cpl
Сетевые подключения=ncpa.cpl
Электропитание=powercfg.cpl
Звук=mmsys.cpl
Свойства системы=sysdm.cpl
Переменные среды=rundll32.exe sysdm.cpl,EditEnvironmentVariables
Параметры Windows=ms-settings:
Параметры: Экран=ms-settings:display
Параметры: Сеть=ms-settings:network
Параметры: Обновление Windows=ms-settings:windowsupdate
Автозагрузка (папка)=shell:startup
Папка SendTo=shell:sendto
)INI";

/* ---------- hotkey/config parsing ---------- */
struct KeyName { const wchar_t *n; UINT vk; };
static const KeyName kKeys[] = {
    {L"NumDot", VK_DECIMAL}, {L"NumDecimal", VK_DECIMAL}, {L"NumMul", VK_MULTIPLY}, {L"NumAdd", VK_ADD}, {L"NumPlus", VK_ADD},
    {L"NumSub", VK_SUBTRACT}, {L"NumMinus", VK_SUBTRACT}, {L"NumDiv", VK_DIVIDE}, {L"Space", VK_SPACE}, {L"Enter", VK_RETURN},
    {L"Tab", VK_TAB}, {L"Esc", VK_ESCAPE}, {L"Back", VK_BACK}, {L"Ins", VK_INSERT}, {L"Del", VK_DELETE}, {L"Home", VK_HOME},
    {L"End", VK_END}, {L"PgUp", VK_PRIOR}, {L"PgDn", VK_NEXT}, {L"Left", VK_LEFT}, {L"Right", VK_RIGHT}, {L"Up", VK_UP},
    {L"Down", VK_DOWN}, {L"Tilde", VK_OEM_3}, {L"Minus", VK_OEM_MINUS}, {L"Equals", VK_OEM_PLUS}, {L"Comma", VK_OEM_COMMA},
    {L"Period", VK_OEM_PERIOD}, {L"Slash", VK_OEM_2}, {L"Backslash", VK_OEM_5}, {L"Semicolon", VK_OEM_1}, {L"Quote", VK_OEM_7},
    {L"LBracket", VK_OEM_4}, {L"RBracket", VK_OEM_6}, {L"PrintScreen", VK_SNAPSHOT}, {L"Pause", VK_PAUSE}, {L"Apps", VK_APPS},
};

static BOOL ParseKeyName(const wchar_t *t, UINT *vk) {
    int n = lstrlenW(t);
    if (n == 1) {
        wchar_t c = t[0]; if (c >= L'a' && c <= L'z') c -= 32;
        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) { *vk = c; return TRUE; }
    }
    if ((t[0] == L'F' || t[0] == L'f') && n >= 2 && n <= 3 && t[1] >= L'0' && t[1] <= L'9') {
        int v = ToInt(t + 1); if (v >= 1 && v <= 24) { *vk = VK_F1 + v - 1; return TRUE; }
    }
    if (n == 4 && CompareStringOrdinal(t, 3, L"Num", 3, TRUE) == CSTR_EQUAL && t[3] >= L'0' && t[3] <= L'9') {
        *vk = VK_NUMPAD0 + (t[3] - L'0'); return TRUE;
    }
    for (int i = 0; i < (int)(sizeof kKeys / sizeof kKeys[0]); i++)
        if (!lstrcmpiW(t, kKeys[i].n)) { *vk = kKeys[i].vk; return TRUE; }
    return FALSE;
}
static BOOL ParseHotkey(const wchar_t *spec, UINT *mods, UINT *key) {
    wchar_t tok[32]; *mods = 0; *key = 0;
    const wchar_t *p = spec;
    for (;;) {
        while (*p == L' ') p++;
        int i = 0;
        while (*p && *p != L'+' && i < 31) tok[i++] = *p++;
        while (i > 0 && tok[i - 1] == L' ') i--;
        tok[i] = 0;
        BOOL last = (*p != L'+');
        if (!last) p++;
        if (!i) return FALSE;
        if (!lstrcmpiW(tok, L"Win")) *mods |= DM_WIN;
        else if (!lstrcmpiW(tok, L"Ctrl") || !lstrcmpiW(tok, L"Control")) *mods |= DM_CTRL;
        else if (!lstrcmpiW(tok, L"Alt")) *mods |= DM_ALT;
        else if (!lstrcmpiW(tok, L"Shift")) *mods |= DM_SHIFT;
        else { if (!ParseKeyName(tok, key)) return FALSE; if (!last) return FALSE; }
        if (last) break;
    }
    return *key != 0;
}
static void SplitDir(const wchar_t *v, Bind *b) {
    int n = lstrlenW(v), at = -1;
    for (int i = 0; i + 2 < n; i++) if (v[i] == L' ' && v[i + 1] == L'@' && v[i + 2] == L' ') { at = i; break; }
    if (at < 0) { b->arg = Dup(v); return; }
    b->arg = DupN(v, at);
    const wchar_t *d = v + at + 3; while (*d == L' ') d++;
    b->dir = Dup(d);
}
static BOOL ParseAction(const wchar_t *v, Bind *b) {
    while (*v == L' ') v++;
    const wchar_t *start = v, *w = v;
    while (*v && *v != L' ') v++;
    wchar_t name[16]; int wl = (int)(v - w); if (wl > 15) wl = 15;
    memcpy(name, w, wl * sizeof(wchar_t)); name[wl] = 0;
    while (*v == L' ') v++;
    struct Act { const wchar_t *n; int a, kind; };
    static const Act T[] = {
        {L"copy", A_COPY, 1}, {L"paste", A_PASTE, 3}, {L"buffer", A_BUFFER, 4}, {L"slots", A_BUFFER, 4}, {L"layout", A_LAYOUT, 0}, {L"history", A_HIST, 0}, {L"launcher", A_LAUNCH, 0},
        {L"run", A_RUN, 2}, {L"admin", A_ADMIN, 2}, {L"py", A_PY, 2}, {L"pyrun", A_PYRUN, 0}, {L"topmost", A_TOPMOST, 0},
        {L"totray", A_TOTRAY, 0}, {L"pause", A_PAUSE, 0}, {L"webcam", A_WEBCAM, 0},
    };
    for (int i = 0; i < (int)(sizeof T / sizeof T[0]); i++) {
        if (lstrcmpiW(name, T[i].n)) continue;
        b->act = T[i].a;
        if (T[i].kind == 1) { b->n = ToInt(v); if (b->n < 1 || b->n > 9) return FALSE; }
        else if (T[i].kind == 2) SplitDir(v, b);
        else if (T[i].kind == 3) {
            b->n = ToInt(v); while (*v >= L'0' && *v <= L'9') v++;
            b->n2 = ToInt(v);
            if (b->n < 1 || b->n > 9 || b->n2 < 1 || b->n2 > 10) return FALSE;
        } else if (T[i].kind == 4) { b->n = *v ? ToInt(v) : 0; if (b->n < 0 || b->n > 9) return FALSE; }
        return TRUE;
    }
    b->act = A_RUN; SplitDir(start, b);
    return *start != 0;
}

static void CfgCb(const wchar_t *s, const wchar_t *k, const wchar_t *v) {
    if (!lstrcmpiW(s, L"General")) {
        if (!lstrcmpiW(k, L"HistoryEnabled")) g_histOn = ToInt(v) != 0;
        else if (!lstrcmpiW(k, L"HistoryMax")) { g_histMax = ToInt(v); if (g_histMax < 5) g_histMax = 5; if (g_histMax > 1000) g_histMax = 1000; }
        else if (!lstrcmpiW(k, L"PasteDelay")) { g_pasteDelay = ToInt(v); if (g_pasteDelay < 50) g_pasteDelay = 50; }
        else if (!lstrcmpiW(k, L"CopyTimeout")) { g_copyTimeout = ToInt(v); if (g_copyTimeout < 100) g_copyTimeout = 100; }
        else if (!lstrcmpiW(k, L"Python")) { if (*v) lstrcpynW(g_python, v, MAX_PATH); }
        else if (!lstrcmpiW(k, L"PyRunPause")) g_pyPause = ToInt(v) != 0;
        else if (!lstrcmpiW(k, L"Toasts")) g_toasts = ToInt(v) != 0;
        else if (!lstrcmpiW(k, L"BufferSize")) { g_bufMax = ToInt(v); if (g_bufMax < 1) g_bufMax = 1; if (g_bufMax > 10) g_bufMax = 10; }
        else if (!lstrcmpiW(k, L"LayoutWord")) g_layoutWord = ToInt(v) != 0;
        else if (!lstrcmpiW(k, L"LayoutSwitch")) g_layoutSwitch = ToInt(v) != 0;
    } else if (!lstrcmpiW(s, L"Hotkeys")) {
        Bind b; memset(&b, 0, sizeof b);
        if (!ParseHotkey(k, &b.mods, &b.key) || !ParseAction(v, &b)) { Free(b.arg); Free(b.dir); return; }
        if (g_nbnew >= g_bnewCap) { g_bnewCap = g_bnewCap ? g_bnewCap * 2 : 64; g_bnew = (Bind *)Realloc(g_bnew, g_bnewCap * sizeof(Bind)); }
        g_bnew[g_nbnew++] = b;
    } else if (!lstrcmpiW(s, L"Launcher")) {
        g_launch = (KV *)Realloc(g_launch, (g_nlaunch + 1) * sizeof(KV));
        g_launch[g_nlaunch].k = Dup(k); g_launch[g_nlaunch].v = Dup(v); g_nlaunch++;
    }
}

static void FreeBinds(Bind *b, int n) {
    for (int i = 0; i < n; i++) { Free(b[i].arg); Free(b[i].dir); }
    Free(b);
}
static void LoadConfig(void) {
    DWORD sz = 0; BYTE *raw = ReadAll(g_ini, &sz);
    if (!raw) {
        BB d = {0}; BBAdd(&d, kIni1, lstrlenA(kIni1)); BBAdd(&d, kIni2, lstrlenA(kIni2));
        WriteAtomic(g_ini, d.p, d.n); Free(d.p);
        raw = ReadAll(g_ini, &sz);
        if (!raw) return;
    }
    wchar_t *t = FromUtf8((char *)raw, sz); Free(raw);
    for (int i = 0; i < g_nlaunch; i++) { Free(g_launch[i].k); Free(g_launch[i].v); }
    Free(g_launch); g_launch = NULL; g_nlaunch = 0;
    g_bnew = NULL; g_nbnew = g_bnewCap = 0;
    IniParse(t, CfgCb);
    Free(t);
    Bind *old = g_bind; int oldn = g_nbind;
    AcquireSRWLockExclusive(&g_bindLock);
    g_bind = g_bnew; g_nbind = g_nbnew;
    ReleaseSRWLockExclusive(&g_bindLock);
    FreeBinds(old, oldn);
    g_bnew = NULL;
}

/* ---------- clipboard ---------- */
static UINT g_fCanHist, g_fCanCloud, g_fExclude, g_fIgnore, g_fMsdevLine, g_fVsLine, g_fChromium;
static void InitFormats(void) {
    g_fMsdevLine = RegisterClipboardFormatW(L"MSDEVLineSelect");
    g_fVsLine = RegisterClipboardFormatW(L"VisualStudioEditorOperationsLineCutCopyClipboardTag");
    g_fChromium = RegisterClipboardFormatW(L"Chromium Web Custom MIME Data Format");
    g_fCanHist = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    g_fCanCloud = RegisterClipboardFormatW(L"CanUploadToCloudClipboard");
    g_fExclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    g_fIgnore = RegisterClipboardFormatW(L"Clipboard Viewer Ignore");
}
static BOOL ClipOpen(void) {
    for (int i = 0; i < 30; i++) { if (OpenClipboard(g_hwndClip ? g_hwndClip : g_hwnd)) return TRUE; Sleep(10); }
    return FALSE;
}
static HGLOBAL MakeGlobal(const void *d, SIZE_T n) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n ? n : 1);
    if (!h) return NULL;
    void *p = GlobalLock(h);
    if (d) memcpy(p, d, n); else memset(p, 0, n);
    GlobalUnlock(h);
    return h;
}
static void SetHideFormats(void) {
    DWORD zero = 0;
    SetClipboardData(g_fCanHist, MakeGlobal(&zero, 4));
    SetClipboardData(g_fCanCloud, MakeGlobal(&zero, 4));
    SetClipboardData(g_fExclude, MakeGlobal(&zero, 4));
}
// Окно-владелец буфера обмена (живёт в потоке хука): отдаёт текст вставки в тот момент, когда приложение его реально запросило.
static LRESULT CALLBACK ClipProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_RENDERFORMAT && w == CF_UNICODETEXT) {
        AcquireSRWLockShared(&g_pasteLock);
        if (g_pasteTxt) {
            HGLOBAL g = MakeGlobal(g_pasteTxt, (lstrlenW(g_pasteTxt) + 1) * sizeof(wchar_t));
            if (g && !SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
        }
        ReleaseSRWLockShared(&g_pasteLock);
        Log(L"RENDER text=%d seq=%u tick=%u", g_pasteTxt ? lstrlenW(g_pasteTxt) : -1, GetClipboardSequenceNumber(), GetTickCount());
        g_ownSeq = g_ignoreSeq = GetClipboardSequenceNumber();
        PostMessageW(g_hwnd, WM_APP_RENDERED, 0, 0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static wchar_t *ClipGetText(void) {
    if (!ClipOpen()) return NULL;
    wchar_t *r = NULL;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) { const wchar_t *p = (const wchar_t *)GlobalLock(h); if (p) { r = Dup(p); GlobalUnlock(h); } }
    CloseClipboard();
    return r;
}
static BOOL SkipFormat(UINT f) {
    if (f == CF_BITMAP || f == CF_METAFILEPICT || f == CF_PALETTE || f == CF_ENHMETAFILE || f == CF_OWNERDISPLAY) return TRUE;
    if (f >= CF_DSPTEXT && f <= CF_DSPMETAFILEPICT) return TRUE;
    if (f == CF_DSPENHMETAFILE) return TRUE;
    if (f >= CF_PRIVATEFIRST && f <= CF_GDIOBJLAST) return TRUE;
    if (f == g_fCanHist || f == g_fCanCloud || f == g_fExclude) return TRUE;
    return FALSE;
}
static void SnapFree(Snap *s) { for (int i = 0; i < s->n; i++) if (s->h[i]) GlobalFree(s->h[i]); s->n = 0; }
static void SnapTake(Snap *s) {
    s->n = 0;
    if (!ClipOpen()) return;
    UINT f = 0; SIZE_T total = 0;
    while ((f = EnumClipboardFormats(f)) != 0 && s->n < 64) {
        if (SkipFormat(f)) continue;
        HANDLE h = GetClipboardData(f);
        if (!h) continue;
        SIZE_T sz = GlobalSize(h);
        if (!sz || total + sz > (SIZE_T)64 << 20) continue;
        void *src = GlobalLock(h);
        if (!src) continue;
        HGLOBAL c = MakeGlobal(src, sz);
        GlobalUnlock(h);
        if (!c) continue;
        s->fmt[s->n] = f; s->h[s->n] = c; s->n++; total += sz;
    }
    CloseClipboard();
}
static void SnapRestore(Snap *s) {
    if (!ClipOpen()) { SnapFree(s); return; }
    EmptyClipboard();
    for (int i = 0; i < s->n; i++) {
        if (SetClipboardData(s->fmt[i], s->h[i])) s->h[i] = NULL;
    }
    SetHideFormats();
    CloseClipboard();
    SnapFree(s);
}

static int AddKey(INPUT *in, int n, WORD vk, BOOL up) {
    memset(&in[n], 0, sizeof(INPUT));
    in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = vk;
    in[n].ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    DWORD f = up ? KEYEVENTF_KEYUP : 0;
    if (vk == VK_LWIN || vk == VK_RWIN || vk == VK_RMENU || vk == VK_RCONTROL || (vk >= VK_PRIOR && vk <= VK_DOWN) || vk == VK_INSERT || vk == VK_DELETE) f |= KEYEVENTF_EXTENDEDKEY;
    in[n].ki.dwFlags = f; in[n].ki.dwExtraInfo = DKMARK;
    return n + 1;
}
// Нажать vk count раз (mod - VK_CONTROL/VK_SHIFT/0: вместе с этим модификатором); остальные удерживаемые модификаторы временно отпускаются.
static void SendKeysN(WORD vk, int count, WORD mod) {
    struct Mod { LONG bit; WORD vk; };
    static const Mod M[] = {{64, VK_LWIN}, {128, VK_RWIN}, {4, VK_LMENU}, {8, VK_RMENU}, {16, VK_LSHIFT}, {32, VK_RSHIFT}, {1, VK_LCONTROL}, {2, VK_RCONTROL}};
    for (int i = 0; i < 60 && g_modBits; i++) Sleep(10);   // даём отпустить хоткей: иначе его Ctrl может исчезнуть посреди нашей комбинации
    LONG b = g_modBits;
    INPUT *in = (INPUT *)Alloc(sizeof(INPUT) * (24 + 2 * count)); int n = 0;
    LONG mb = mod == VK_CONTROL ? 3 : mod == VK_SHIFT ? 48 : 0;
    WORD own = mod == VK_CONTROL ? VK_LCONTROL : VK_LSHIFT;
    BOOL held = (b & mb) != 0;
    for (int i = 0; i < 8; i++) if ((b & M[i].bit) && !(mb & M[i].bit)) n = AddKey(in, n, M[i].vk, TRUE);
    if (mod && !held) n = AddKey(in, n, own, FALSE);
    for (int c = 0; c < count; c++) { n = AddKey(in, n, vk, FALSE); n = AddKey(in, n, vk, TRUE); }
    if (mod && !held) n = AddKey(in, n, own, TRUE);
    for (int i = 0; i < 8; i++) if ((b & M[i].bit) && !(mb & M[i].bit)) n = AddKey(in, n, M[i].vk, FALSE);
    if (b & 192) { n = AddKey(in, n, 0xE8, FALSE); n = AddKey(in, n, 0xE8, TRUE); }
    SendInput(n, in, sizeof(INPUT));
    Free(in);
}
static void SendChord(WORD vk) { SendKeysN(vk, 1, VK_CONTROL); }

static BOOL HasBytes(const BYTE *p, SIZE_T n, const void *needle, int m) {
    for (SIZE_T i = 0; i + m <= n; i++) {
        int j = 0; while (j < m && p[i + j] == ((const BYTE *)needle)[j]) j++;
        if (j == m) return TRUE;
    }
    return FALSE;
}
// Ctrl+C без выделения редакторы понимают как "копировать строку": Notepad++/Visual Studio помечают это своими форматами,
// VS Code кладёт флаг isFromEmptySelection в данные Chromium.
static BOOL ClipIsLineCopy(void) {
    if (IsClipboardFormatAvailable(g_fMsdevLine) || IsClipboardFormatAvailable(g_fVsLine)) return TRUE;
    if (!IsClipboardFormatAvailable(g_fChromium) || !ClipOpen()) return FALSE;
    BOOL r = FALSE;
    HANDLE h = GetClipboardData(g_fChromium);
    const BYTE *p = h ? (const BYTE *)GlobalLock(h) : NULL;
    if (p) {
        static const char a[] = "\"isFromEmptySelection\":true";
        static const wchar_t w[] = L"\"isFromEmptySelection\":true";
        SIZE_T sz = GlobalSize(h);
        r = HasBytes(p, sz, a, sizeof a - 1) || HasBytes(p, sz, w, sizeof w - sizeof(wchar_t));
        GlobalUnlock(h);
    }
    CloseClipboard();
    return r;
}

// Скопировать выделенное в приложении, не меняя основной буфер. *line - редактор скопировал всю строку (выделения не было).
static wchar_t *GrabSelection(BOOL *line = NULL) {
    Snap s; memset(&s, 0, sizeof s);
    SnapTake(&s);
    DWORD seq0 = GetClipboardSequenceNumber();
    SendChord('C');
    wchar_t *t = NULL; DWORD t0 = GetTickCount();
    for (;;) {
        if (GetClipboardSequenceNumber() != seq0) {
            t = ClipGetText();
            if (t && *t) { if (line) *line = ClipIsLineCopy(); break; }
            Free(t); t = NULL;
        }
        if (GetTickCount() - t0 > (DWORD)g_copyTimeout) break;
        Sleep(10);
    }
    Log(L"Grab seq0=%u now=%u t=%d elapsed=%u snap=%d mods=%x", seq0, GetClipboardSequenceNumber(), t ? lstrlenW(t) : -1, GetTickCount() - t0, s.n, (int)g_modBits);
    if (GetClipboardSequenceNumber() != seq0) SnapRestore(&s); else SnapFree(&s);
    g_ignoreSeq = GetClipboardSequenceNumber();
    return t;
}

static void PasteText(const wchar_t *t) {
    Log(L"PasteText len=%d mods=%x", t ? lstrlenW(t) : -1, (int)g_modBits);
    if (!t || !*t) return;
    if (!g_psPending) { memset(&g_ps, 0, sizeof g_ps); SnapTake(&g_ps); g_psPending = TRUE; }
    wchar_t *c = Dup(t);
    AcquireSRWLockExclusive(&g_pasteLock); Free(g_pasteTxt); g_pasteTxt = c; ReleaseSRWLockExclusive(&g_pasteLock);
    // Текст кладём "по требованию": прежний буфер вернём только после того, как приложение его реально прочитало.
    BOOL ok = ClipOpen();
    if (ok) { EmptyClipboard(); SetClipboardData(CF_UNICODETEXT, NULL); SetHideFormats(); CloseClipboard(); }
    Log(L"paste ok=%d snap n=%d first=%x last=%x tick=%u", ok, g_ps.n, t[0], t[lstrlenW(t) - 1], GetTickCount());
    if (!ok) { g_psPending = FALSE; SnapFree(&g_ps); return; }
    g_ownSeq = g_ignoreSeq = GetClipboardSequenceNumber();
    // Слушатели буфера (мессенджеры и т.п.) сразу открывают его для чтения; если Ctrl+V попадёт в этот момент, приложение вставит пустоту.
    Sleep(25);
    for (int i = 0; i < 30 && GetOpenClipboardWindow(); i++) Sleep(10);
    SendChord('V');
    Log(L"chord V sent mods=%x tick=%u", (int)g_modBits, GetTickCount());
    SetTimer(g_hwnd, TID_RESTORE, 3000, NULL);   // запасной срок, если приложение так и не запросило текст
}
static void OnRestoreTimer(void) {
    KillTimer(g_hwnd, TID_RESTORE);
    if (!g_psPending) return;
    g_psPending = FALSE;
    Log(L"RESTORE seq=%u own=%u tick=%u", GetClipboardSequenceNumber(), g_ownSeq, GetTickCount());
    if (GetClipboardSequenceNumber() == g_ownSeq) { SnapRestore(&g_ps); g_ignoreSeq = GetClipboardSequenceNumber(); }
    else SnapFree(&g_ps);
}

/* ---------- slots & history ---------- */
static void BufPath(int n, wchar_t *out) { wchar_t nm[8]; wsprintfW(nm, L"%d.txt", n); MemPath(out, nm); }
// Файл списка: записи "@<байт>\n<текст UTF-8>\n" подряд.
static void PackItems(BB *b, wchar_t **it, int n) {
    for (int i = 0; i < n; i++) {
        int l; char *u = ToUtf8(it[i], &l);
        char hd[24]; int hl = wsprintfA(hd, "@%d\n", l);
        BBAdd(b, hd, hl); BBAdd(b, u, l); BBAdd(b, "\n", 1);
        Free(u);
    }
}
typedef void (*ItemCb)(wchar_t *t, void *ctx);
static void UnpackItems(BYTE *raw, DWORD sz, ItemCb cb, void *ctx) {
    DWORD i = 0;
    while (i < sz) {
        if (raw[i] != '@') { i++; continue; }
        i++; DWORD n = 0;
        while (i < sz && raw[i] >= '0' && raw[i] <= '9') { n = n * 10 + (raw[i] - '0'); i++; }
        while (i < sz && raw[i] != '\n') i++;
        i++;
        if (i + n > sz) break;
        cb(FromUtf8((char *)raw + i, (int)n), ctx);
        i += n;
    }
}

static void BufSave(int n) {
    wchar_t p[MAX_PATH]; BufPath(n, p);
    BB b = {0}; PackItems(&b, g_buf[n], g_bufN[n]);
    if (b.n) WriteAtomic(p, b.p, b.n); else DeleteFileW(p);
    Free(b.p);
}
static void BufLoadCb(wchar_t *t, void *ctx) {
    int n = (int)(INT_PTR)ctx;
    if (g_bufN[n] < 10) g_buf[n][g_bufN[n]++] = t; else Free(t);
}
static void BuffersLoad(void) {
    for (int n = 1; n <= 9; n++) {
        wchar_t p[MAX_PATH]; BufPath(n, p);
        DWORD sz; BYTE *raw = ReadAll(p, &sz);
        if (!raw) continue;
        if (sz && raw[0] != '@') BufLoadCb(FromUtf8((char *)raw, sz), (void *)(INT_PTR)n);   // старый формат: один текст
        else UnpackItems(raw, sz, BufLoadCb, (void *)(INT_PTR)n);
        Free(raw);
    }
}
// Новая запись наверх (владение переходит буферу); повтор поднимается наверх, лишнее снизу вытесняется.
static void BufInsert(int n, wchar_t *t) {
    wchar_t **b = g_buf[n];
    for (int i = 0; i < g_bufN[n]; i++) {
        if (lstrcmpW(b[i], t)) continue;
        wchar_t *x = b[i]; memmove(b + 1, b, i * sizeof(wchar_t *)); b[0] = x;
        Free(t); BufSave(n); return;
    }
    if (g_bufN[n] >= g_bufMax) { Free(b[--g_bufN[n]]); b[g_bufN[n]] = NULL; }
    memmove(b + 1, b, g_bufN[n] * sizeof(wchar_t *)); b[0] = t; g_bufN[n]++;
    BufSave(n);
}
static void BufRemove(int n, int i) {
    wchar_t **b = g_buf[n];
    Free(b[i]);
    memmove(b + i, b + i + 1, (g_bufN[n] - i - 1) * sizeof(wchar_t *));
    b[--g_bufN[n]] = NULL;
    BufSave(n);
}
static void BufMove(int n, int i, int d) {
    int j = i + d;
    if (j < 0 || j >= g_bufN[n]) return;
    wchar_t *x = g_buf[n][i]; g_buf[n][i] = g_buf[n][j]; g_buf[n][j] = x;
    BufSave(n);
}

static void HistSave(void) {
    BB b = {0}; PackItems(&b, g_hist, g_nhist);
    wchar_t p[MAX_PATH]; MemPath(p, L"history.txt");
    if (b.n) WriteAtomic(p, b.p, b.n); else DeleteFileW(p);
    Free(b.p);
}
static void HistPush(wchar_t *t, void * = NULL) {
    if (g_nhist >= g_histCap) { g_histCap = g_histCap ? g_histCap * 2 : 64; g_hist = (wchar_t **)Realloc(g_hist, g_histCap * sizeof(wchar_t *)); }
    g_hist[g_nhist++] = t;
}
static void HistLoad(void) {
    wchar_t p[MAX_PATH]; MemPath(p, L"history.txt");
    DWORD sz; BYTE *raw = ReadAll(p, &sz);
    if (!raw) return;
    UnpackItems(raw, sz, HistPush, NULL);
    Free(raw);
    while (g_nhist > g_histMax) Free(g_hist[--g_nhist]);
}
static void HistRemove(int i) {
    Free(g_hist[i]);
    memmove(g_hist + i, g_hist + i + 1, (g_nhist - i - 1) * sizeof(wchar_t *));
    g_nhist--;
}
static void HistAdd(wchar_t *t) {
    BOOL blank = TRUE; int len = lstrlenW(t);
    for (int i = 0; i < len; i++) if (t[i] > L' ') { blank = FALSE; break; }
    if (blank || len > 200000) { Free(t); return; }
    for (int i = 0; i < g_nhist; i++) {
        if (!lstrcmpW(g_hist[i], t)) {
            if (i == 0) { Free(t); return; }
            wchar_t *x = g_hist[i]; memmove(g_hist + 1, g_hist, i * sizeof(wchar_t *)); g_hist[0] = x;
            Free(t); HistSave(); return;
        }
    }
    HistPush(t);
    memmove(g_hist + 1, g_hist, (g_nhist - 1) * sizeof(wchar_t *)); g_hist[0] = t;
    while (g_nhist > g_histMax) Free(g_hist[--g_nhist]);
    HistSave();
}
static void OnClip(void) {
    if (!g_histOn || g_psPending) return;
    DWORD seq = GetClipboardSequenceNumber();
    if ((LONG)(seq - g_ignoreSeq) <= 0) return;
    if (!ClipOpen()) return;
    BOOL skip = IsClipboardFormatAvailable(g_fExclude) || IsClipboardFormatAvailable(g_fIgnore);
    if (!skip && IsClipboardFormatAvailable(g_fCanHist)) {
        HANDLE h = GetClipboardData(g_fCanHist);
        if (h && GlobalSize(h) >= 4) { DWORD *p = (DWORD *)GlobalLock(h); if (p) { if (*p == 0) skip = TRUE; GlobalUnlock(h); } }
    }
    wchar_t *t = NULL;
    if (!skip) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) { const wchar_t *p = (const wchar_t *)GlobalLock(h); if (p) { t = Dup(p); GlobalUnlock(h); } }
    }
    CloseClipboard();
    if (t) HistAdd(t);
}
static void SetHistory(BOOL on) {
    g_histOn = on;
    if (on) AddClipboardFormatListener(g_hwnd); else RemoveClipboardFormatListener(g_hwnd);
    IniSet(L"General", L"HistoryEnabled", on ? L"1" : L"0");
}

/* ---------- popup ---------- */
struct Pal { COLORREF bg, hdr, fg, dim, sel, acc, bd; };
static const Pal kDark = {RGB(32, 32, 32), RGB(40, 40, 40), RGB(230, 230, 230), RGB(140, 140, 140), RGB(38, 79, 120), RGB(90, 170, 250), RGB(85, 85, 85)};
static const Pal kLight = {RGB(250, 250, 250), RGB(240, 240, 240), RGB(28, 28, 28), RGB(125, 125, 125), RGB(204, 228, 247), RGB(0, 95, 184), RGB(170, 170, 170)};
// вкладки: 0..8 - буферы 1..9, 9 - история, 10 - запуск
#define NTABS 11
static const wchar_t *kTabName[2] = {L"История", L"Запуск"};

static Row *g_rows; static int g_nrows;
static int *g_vis, g_nvis, g_sel, g_top, g_pmode;
static wchar_t g_filt[64]; static int g_nfilt;
static int g_dpi = 96, g_pw, g_ph, g_rh, g_hh, g_prh, g_pad;
static HFONT g_fnt, g_fntB;
static BOOL g_dark;
static RECT g_tabRc[NTABS];

static int Sc(int v) { return MulDiv(v, g_dpi, 96); }

static wchar_t *Collapse(const wchar_t *s) {
    int n = lstrlenW(s);
    wchar_t *o = (wchar_t *)Alloc((n + 1) * sizeof(wchar_t));
    int j = 0; BOOL sp = TRUE;
    for (int i = 0; i < n && j < 300; i++) {
        wchar_t c = s[i];
        if (c == L'\r' || c == L'\n' || c == L'\t' || c == L' ') { if (!sp) { o[j++] = L' '; sp = TRUE; } }
        else { o[j++] = c; sp = FALSE; }
    }
    while (j && o[j - 1] == L' ') j--;
    o[j] = 0; return o;
}
static void FreeRows(void) {
    for (int i = 0; i < g_nrows; i++) { Free(g_rows[i].label); Free(g_rows[i].title); Free(g_rows[i].text); }
    Free(g_rows); Free(g_vis);
    g_rows = NULL; g_vis = NULL; g_nrows = g_nvis = 0;
}
static void Refilter(void) {
    g_nvis = 0;
    for (int i = 0; i < g_nrows; i++)
        if (!g_nfilt || ContainsI(g_rows[i].title, g_filt) || ContainsI(g_rows[i].text, g_filt)) g_vis[g_nvis++] = i;
    if (g_sel >= g_nvis) g_sel = g_nvis - 1;
    if (g_sel < 0) g_sel = 0;
    if (g_top > g_sel) g_top = g_sel;
    if (g_top < 0) g_top = 0;
}
static void BuildRows(void) {
    FreeRows();
    int n = g_pmode == PM_BUF ? g_bufN[g_curBuf] : g_pmode == PM_HIST ? g_nhist : g_nlaunch;
    g_rows = (Row *)Alloc(sizeof(Row) * (n ? n : 1)); g_vis = (int *)Alloc(sizeof(int) * (n ? n : 1));
    for (int i = 0; i < n; i++) {
        Row *r = &g_rows[i];
        r->ref = i;
        if (g_pmode == PM_BUF) {
            wchar_t l[2] = {(wchar_t)(i < 9 ? L'1' + i : L'0'), 0};
            r->label = Dup(l); r->text = Dup(g_buf[g_curBuf][i]); r->title = Collapse(r->text);
        } else if (g_pmode == PM_HIST) {
            r->label = Dup(L""); r->text = Dup(g_hist[i]); r->title = Collapse(r->text);
        } else {
            r->label = Dup(L""); r->title = Dup(g_launch[i].k); r->text = Dup(g_launch[i].v);
        }
    }
    g_nrows = n;
    Refilter();
}

static void PopupPaint(HWND h) {
    PAINTSTRUCT ps; HDC wdc = BeginPaint(h, &ps);
    RECT cr; GetClientRect(h, &cr);
    int W = cr.right, H = cr.bottom;
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, W, H);
    HGDIOBJ ob = SelectObject(dc, bmp);
    const Pal &P = g_dark ? kDark : kLight;
    HBRUSH bg = CreateSolidBrush(P.bg), hb = CreateSolidBrush(P.hdr), sb = CreateSolidBrush(P.sel), bd = CreateSolidBrush(P.bd);
    FillRect(dc, &cr, bg);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_fnt);
    // header: поле поиска слева, вкладки справа
    RECT hr = {1, 1, W - 1, g_hh}; FillRect(dc, &hr, hb);
    int right = W - g_pad;
    for (int i = NTABS - 1; i >= 0; i--) {
        SetRectEmpty(&g_tabRc[i]);
        if (i == 9 && !g_histOn) continue;
        BOOL cur = i < 9 ? (g_pmode == PM_BUF && g_curBuf == i + 1) : (g_pmode == (i == 9 ? PM_HIST : PM_LAUNCH));
        wchar_t dg[2] = {(wchar_t)(L'1' + i), 0};
        const wchar_t *tx = i < 9 ? dg : kTabName[i - 9];
        SelectObject(dc, cur ? g_fntB : g_fnt);
        SIZE sz; GetTextExtentPoint32W(dc, tx, lstrlenW(tx), &sz);
        int tw = i < 9 ? Sc(24) : sz.cx + Sc(16);
        RECT tr = {right - tw, Sc(6), right, g_hh - Sc(6)};
        g_tabRc[i] = tr;
        if (cur) FillRect(dc, &tr, sb);
        SetTextColor(dc, cur ? P.fg : (i < 9 ? (g_bufN[i + 1] ? P.acc : P.dim) : P.dim));
        DrawTextW(dc, tx, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        right -= tw + Sc(i < 9 ? 2 : 6);
    }
    SelectObject(dc, g_fnt);
    RECT fr = {g_pad, 0, right - Sc(8), g_hh};
    if (g_nfilt) { SetTextColor(dc, P.fg); DrawTextW(dc, g_filt, g_nfilt, &fr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS); }
    else { SetTextColor(dc, P.dim); DrawTextW(dc, L"Введите для поиска…", -1, &fr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS); }
    // rows
    int y0 = g_hh, lw = g_pmode == PM_BUF ? Sc(28) : 0;
    for (int i = 0; i < VISROWS; i++) {
        int vi = g_top + i; if (vi >= g_nvis) break;
        Row *r = &g_rows[g_vis[vi]];
        RECT rr = {1, y0 + i * g_rh, W - 1, y0 + (i + 1) * g_rh};
        if (vi == g_sel) FillRect(dc, &rr, sb);
        if (lw) {
            RECT lr = rr; lr.left += g_pad; lr.right = lr.left + lw;
            SelectObject(dc, g_fntB); SetTextColor(dc, P.acc);
            DrawTextW(dc, r->label, -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, g_fnt);
        }
        RECT tr = rr; tr.left += g_pad + lw; tr.right -= g_pad + Sc(6);
        SetTextColor(dc, P.fg);
        DrawTextW(dc, r->title, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    if (g_nvis > VISROWS) {
        int ah = VISROWS * g_rh, bh = ah * VISROWS / g_nvis; if (bh < Sc(14)) bh = Sc(14);
        int by = y0 + (ah - bh) * g_top / (g_nvis - VISROWS);
        RECT sr = {W - Sc(5), by, W - 1, by + bh}; FillRect(dc, &sr, bd);
    }
    // preview
    int py = y0 + VISROWS * g_rh;
    RECT sep = {1, py, W - 1, py + 1}; FillRect(dc, &sep, bd);
    RECT pr = {g_pad, py + Sc(6), W - g_pad, H - Sc(22)};
    if (g_nvis > 0) {
        Row *r = &g_rows[g_vis[g_sel]];
        int tl = lstrlenW(r->text);
        wchar_t *tmp = DupN(r->text, tl > 1500 ? 1500 : tl);
        SetTextColor(dc, P.fg);
        DrawTextW(dc, tmp, -1, &pr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS | DT_EDITCONTROL);
        Free(tmp);
        RECT ir = {g_pad, H - Sc(22), W - g_pad, H - Sc(3)};
        wchar_t info[128];
        if (g_pmode == PM_LAUNCH) lstrcpynW(info, L"Enter - запустить   Tab - режим   Esc - закрыть", 128);
        else if (g_pmode == PM_BUF) wsprintfW(info, L"%d симв.   цифра/Enter - вставить   Del - удалить   F2 - правка   Ins - новая   ←→ - буфер", tl);
        else wsprintfW(info, L"%d симв.   Enter - вставить   Del - удалить   F2 - правка   Ctrl+1..9 - в буфер", tl);
        SetTextColor(dc, P.dim);
        DrawTextW(dc, info, -1, &ir, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else {
        SetTextColor(dc, P.dim);
        wchar_t e[96];
        if (g_pmode == PM_BUF) wsprintfW(e, g_nfilt ? L"Ничего не найдено" : L"Буфер %d пуст. Ctrl+Num%d копирует выделенное сюда, Ins - ввести вручную", g_curBuf, g_curBuf);
        else lstrcpynW(e, g_pmode == PM_HIST && !g_nfilt ? L"История пуста" : L"Ничего не найдено", 96);
        DrawTextW(dc, e, -1, &pr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    }
    FrameRect(dc, &cr, bd);
    BitBlt(wdc, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob); DeleteObject(bmp); DeleteDC(dc);
    DeleteObject(bg); DeleteObject(hb); DeleteObject(sb); DeleteObject(bd);
    EndPaint(h, &ps);
}
static int RowAt(int x, int y) {
    if (x < 0 || x >= g_pw || y < g_hh || y >= g_hh + VISROWS * g_rh) return -1;
    int vi = g_top + (y - g_hh) / g_rh;
    return vi < g_nvis ? vi : -1;
}
static LRESULT CALLBACK PopProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: PopupPaint(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        int vi = RowAt((short)LOWORD(l), (short)HIWORD(l));
        static LPARAM last; if (l == last) return 0; last = l;
        if (vi >= 0 && vi != g_sel) { g_sel = vi; InvalidateRect(h, NULL, FALSE); }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        for (int i = 0; i < NTABS; i++) { POINT p = {x, y}; if (PtInRect(&g_tabRc[i], p)) { PostMessageW(g_hwnd, WM_APP_PTAB, i, 0); return 0; } }
        int vi = RowAt(x, y);
        if (vi >= 0) PostMessageW(g_hwnd, WM_APP_PCLICK, vi, 0);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int d = (short)HIWORD(w) > 0 ? -3 : 3;
        g_top += d;
        if (g_top > g_nvis - VISROWS) g_top = g_nvis - VISROWS;
        if (g_top < 0) g_top = 0;
        InvalidateRect(h, NULL, FALSE); return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}
static void PopupClose(void) {
    if (!g_hwndPop) return;
    InterlockedExchange(&g_popup, 0);
    KillTimer(g_hwnd, TID_POPWATCH); KillTimer(g_hwnd, TID_POPFG);
    DestroyWindow(g_hwndPop); g_hwndPop = NULL;
    if (g_fnt) { DeleteObject(g_fnt); g_fnt = NULL; }
    if (g_fntB) { DeleteObject(g_fntB); g_fntB = NULL; }
}
// buf: номер буфера для режима PM_BUF (0 - последний использованный)
static void PopupOpen(int mode, int buf = 0) {
    if (g_hwndPop) PopupClose();
    if (mode == PM_HIST && !g_histOn) mode = PM_BUF;
    if (buf >= 1 && buf <= 9) g_curBuf = buf;
    g_pmode = mode; g_nfilt = 0; g_filt[0] = 0; g_sel = g_top = 0;
    BuildRows();
    g_dark = IsDark();
    POINT pt; GetCursorPos(&pt);
    g_hwndPop = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"DKPop", L"", WS_POPUP, pt.x, pt.y, 10, 10, NULL, NULL, g_inst, NULL);
    if (!g_hwndPop) return;
    g_dpi = GetDpiForWindow(g_hwndPop);
    g_pw = Sc(620); g_rh = Sc(27); g_hh = Sc(36); g_prh = Sc(124); g_pad = Sc(12);
    g_ph = g_hh + VISROWS * g_rh + g_prh;
    g_fnt = CreateFontW(-MulDiv(10, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fntB = CreateFontW(-MulDiv(10, g_dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    MONITORINFO mi; mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    int x = pt.x + 8, y = pt.y + 8;
    if (x + g_pw > mi.rcWork.right) x = pt.x - g_pw - 8;
    if (y + g_ph > mi.rcWork.bottom) y = pt.y - g_ph - 8;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;
    SetWindowPos(g_hwndPop, HWND_TOPMOST, x, y, g_pw, g_ph, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_hwndPop, NULL, FALSE);
    InterlockedExchange(&g_popup, 1);
    SetTimer(g_hwnd, TID_POPWATCH, 60000, NULL);
    g_popFg = GetForegroundWindow(); g_popGrace = 12;
    SetTimer(g_hwnd, TID_POPFG, 40, NULL);
}
// Хуки не видят окна с более высокими правами (например, Total Commander от администратора): клик по ним и Esc до нас не доходят.
// Поэтому пока меню открыто, сами смотрим на смену активного окна и на нажатые кнопки мыши вне меню.
static void OnPopFg(void) {
    if (!g_hwndPop) return;
    HWND f = GetForegroundWindow();
    if (g_popGrace > 0) { g_popGrace--; if (f) g_popFg = f; return; }
    if (f && f != g_popFg && f != g_hwndPop) { PopupClose(); return; }
    if ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000) {
        POINT p; RECT r; GetCursorPos(&p);
        if (GetWindowRect(g_hwndPop, &r) && !PtInRect(&r, p)) PopupClose();
    }
}
static void PopupRedraw(void) { if (g_hwndPop) InvalidateRect(g_hwndPop, NULL, FALSE); }
static void PopupEnsureVisible(void) {
    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + VISROWS) g_top = g_sel - VISROWS + 1;
    if (g_top < 0) g_top = 0;
}

/* ---------- edit dialog ---------- */
static HWND g_edDlg, g_edEdit; static int g_edDone; static wchar_t *g_edText;
static LRESULT CALLBACK EdProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            int n = GetWindowTextLengthW(g_edEdit);
            g_edText = (wchar_t *)Alloc((n + 1) * sizeof(wchar_t));
            GetWindowTextW(g_edEdit, g_edText, n + 1);
            g_edDone = 1;
        } else if (LOWORD(w) == IDCANCEL) g_edDone = 2;
        return 0;
    case WM_SIZE: {
        int W = LOWORD(l), H = HIWORD(l), d = GetDpiForWindow(h), b = MulDiv(34, d, 96), p = MulDiv(8, d, 96);
        MoveWindow(g_edEdit, p, p, W - 2 * p, H - b - 2 * p, TRUE);
        MoveWindow(GetDlgItem(h, IDOK), W - p - MulDiv(250, d, 96), H - b - p + p, MulDiv(150, d, 96), b - p, TRUE);
        MoveWindow(GetDlgItem(h, IDCANCEL), W - p - MulDiv(94, d, 96), H - b - p + p, MulDiv(94, d, 96), b - p, TRUE);
        return 0;
    }
    case WM_CLOSE: g_edDone = 2; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static void ForceFg(HWND h) {
    HWND fg = GetForegroundWindow();
    DWORD ft = fg ? GetWindowThreadProcessId(fg, NULL) : 0, me = GetCurrentThreadId();
    if (ft && ft != me) AttachThreadInput(me, ft, TRUE);
    SetForegroundWindow(h); BringWindowToTop(h);
    if (ft && ft != me) AttachThreadInput(me, ft, FALSE);
}
// Возвращает новый текст (Alloc) или NULL, если отменено.
static wchar_t *EditDialog(const wchar_t *title, const wchar_t *text) {
    POINT pt; GetCursorPos(&pt);
    MONITORINFO mi; mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    g_edDone = 0; g_edText = NULL;
    g_edDlg = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, L"DKEdit", title, WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                              0, 0, 100, 100, NULL, NULL, g_inst, NULL);
    int d = GetDpiForWindow(g_edDlg);
    HFONT f = CreateFontW(-MulDiv(10, d, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_edEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                               0, 0, 10, 10, g_edDlg, (HMENU)3, g_inst, NULL);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"OK  (Ctrl+Enter)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, g_edDlg, (HMENU)IDOK, g_inst, NULL);
    HWND cn = CreateWindowExW(0, L"BUTTON", L"Отмена", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, g_edDlg, (HMENU)IDCANCEL, g_inst, NULL);
    SendMessageW(g_edEdit, WM_SETFONT, (WPARAM)f, TRUE); SendMessageW(ok, WM_SETFONT, (WPARAM)f, TRUE); SendMessageW(cn, WM_SETFONT, (WPARAM)f, TRUE);
    SendMessageW(g_edEdit, EM_SETLIMITTEXT, 4000000, 0);
    SetWindowTextW(g_edEdit, text);
    int W = MulDiv(640, d, 96), H = MulDiv(420, d, 96);
    SetWindowPos(g_edDlg, HWND_TOPMOST, (mi.rcWork.left + mi.rcWork.right - W) / 2, (mi.rcWork.top + mi.rcWork.bottom - H) / 2, W, H, SWP_SHOWWINDOW);
    ForceFg(g_edDlg); SetFocus(g_edEdit);
    InterlockedExchange(&g_modal, 1);
    MSG msg;
    while (!g_edDone && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_ESCAPE) { g_edDone = 2; break; }
            if (msg.wParam == VK_RETURN && (GetKeyState(VK_CONTROL) & 0x8000)) { SendMessageW(g_edDlg, WM_COMMAND, IDOK, 0); continue; }
        }
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    InterlockedExchange(&g_modal, 0);
    DestroyWindow(g_edDlg); DeleteObject(f);
    g_edDlg = NULL;
    if (g_edDone != 1) { Free(g_edText); g_edText = NULL; }
    return g_edText;
}

/* ---------- actions ---------- */
static void SplitCmd(wchar_t *s, wchar_t **file, wchar_t **par) {
    while (*s == L' ') s++;
    if (*s == L'"') {
        *file = ++s; while (*s && *s != L'"') s++;
        if (*s) *s++ = 0;
    } else {
        *file = s; while (*s && *s != L' ') s++;
        if (*s) *s++ = 0;
    }
    while (*s == L' ') s++;
    *par = s;
}
static void RunShell(const wchar_t *spec, const wchar_t *dir, BOOL admin) {
    wchar_t *s = Dup(spec), *file, *par;
    SplitCmd(s, &file, &par);
    SHELLEXECUTEINFOW e; memset(&e, 0, sizeof e);
    e.cbSize = sizeof e; e.fMask = SEE_MASK_DOENVSUBST | SEE_MASK_FLAG_NO_UI;
    e.lpVerb = admin ? L"runas" : NULL; e.lpFile = file; e.lpParameters = *par ? par : NULL;
    e.lpDirectory = (dir && *dir) ? dir : NULL; e.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&e) && GetLastError() != ERROR_CANCELLED) {
        wchar_t m[200]; wsprintfW(m, L"Не удалось запустить: %.120s", file); Toast(m);
    }
    Free(s);
}
static wchar_t *BuildEnv(const wchar_t *sel, const wchar_t *selFile) {
    wchar_t *e = GetEnvironmentStringsW(), *p = e;
    while (*p) p += lstrlenW(p) + 1;
    int n = (int)(p - e);
    wchar_t *out = (wchar_t *)Alloc((n + 40000) * sizeof(wchar_t));
    memcpy(out, e, n * sizeof(wchar_t));
    FreeEnvironmentStringsW(e);
    int o = n;
    if (selFile && *selFile) { lstrcpyW(out + o, L"DKEYS_SEL_FILE="); o += 15; int l = lstrlenW(selFile); memcpy(out + o, selFile, l * 2); o += l; out[o++] = 0; }
    if (sel && lstrlenW(sel) <= 16000) { lstrcpyW(out + o, L"DKEYS_SEL="); o += 10; int l = lstrlenW(sel); memcpy(out + o, sel, l * 2); o += l; out[o++] = 0; }
    out[o] = 0;
    return out;
}
static void RunProc(wchar_t *cmd, const wchar_t *dir, wchar_t *env) {
    STARTUPINFOW si; PROCESS_INFORMATION pi; memset(&si, 0, sizeof si); si.cb = sizeof si;
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_UNICODE_ENVIRONMENT, env, (dir && *dir) ? dir : g_pyDir, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    } else {
        wchar_t m[200]; wsprintfW(m, L"Не удалось запустить Python (%.80s)", g_python); Toast(m);
    }
}
static void RunPy(const wchar_t *arg, const wchar_t *dir) {
    wchar_t *s = Dup(arg), *file, *par;
    SplitCmd(s, &file, &par);
    wchar_t script[MAX_PATH];
    if (file[1] == L':' || file[0] == L'\\') lstrcpynW(script, file, MAX_PATH);
    else { lstrcpynW(script, g_pyDir, MAX_PATH); Cat(script, MAX_PATH, file); }
    wchar_t *sel = GrabSelection();
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp); Cat(tmp, MAX_PATH, L"dkeys_sel.txt");
    if (sel) WriteText(tmp, sel);
    wchar_t *env = BuildEnv(sel, sel ? tmp : NULL);
    Buf c = {0};
    BufAdd(&c, L"\""); BufAdd(&c, g_python); BufAdd(&c, L"\" \""); BufAdd(&c, script); BufAdd(&c, L"\"");
    if (*par) { BufAdd(&c, L" "); BufAdd(&c, par); }
    RunProc(c.p, dir, env);
    Free(c.p); Free(env); Free(sel); Free(s);
}
static void RunPySelection(void) {
    wchar_t *sel = GrabSelection();
    if (!sel || !*sel) { Free(sel); Toast(L"Нет выделенного текста"); return; }
    wchar_t script[MAX_PATH]; lstrcpynW(script, g_pyDir, MAX_PATH); Cat(script, MAX_PATH, L"tmp.py");
    WriteText(script, sel); Free(sel);
    Buf c = {0};
    if (g_pyPause) {
        BufAdd(&c, L"cmd.exe /d /c \"\""); BufAdd(&c, g_python); BufAdd(&c, L"\" \""); BufAdd(&c, script); BufAdd(&c, L"\" & pause\"");
    } else {
        BufAdd(&c, L"\""); BufAdd(&c, g_python); BufAdd(&c, L"\" \""); BufAdd(&c, script); BufAdd(&c, L"\"");
    }
    RunProc(c.p, g_pyDir, NULL);
    Free(c.p);
}
static HWND TopWin(void) {
    HWND h = GetForegroundWindow();
    if (!h) return NULL;
    h = GetAncestor(h, GA_ROOT);
    if (!h || h == g_hwnd) return NULL;
    wchar_t cls[64]; GetClassNameW(h, cls, 64);
    if (!lstrcmpW(cls, L"Progman") || !lstrcmpW(cls, L"WorkerW") || !lstrcmpW(cls, L"Shell_TrayWnd") || !lstrcmpW(cls, L"Shell_SecondaryTrayWnd")) return NULL;
    return h;
}
static void ToggleTopmost(void) {
    HWND h = TopWin(); if (!h) return;
    BOOL on = !(GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOPMOST);
    SetWindowPos(h, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    wchar_t t[64], m[160]; GetWindowTextW(h, t, 64);
    wsprintfW(m, on ? L"Поверх всех: ВКЛ  —  %s" : L"Поверх всех: ВЫКЛ  —  %s", t);
    Toast(m);
}
static void HideToTray(void) {
    HWND h = TopWin(); if (!h || g_nhid >= 32) return;
    wchar_t t[64], m[160]; GetWindowTextW(h, t, 64);
    ShowWindow(h, SW_HIDE); g_hid[g_nhid++] = h;
    wsprintfW(m, L"Спрятано в трей: %s", t); Toast(m);
}
static void ShowHidden(int i) {
    HWND h = g_hid[i];
    memmove(g_hid + i, g_hid + i + 1, (g_nhid - i - 1) * sizeof(HWND)); g_nhid--;
    if (!IsWindow(h)) return;
    ShowWindow(h, IsIconic(h) ? SW_RESTORE : SW_SHOW);
    ForceFg(h);
}

// Последний набранный текст (по нажатиям клавиш, для смены раскладки): копится хуком, сбрасывается Enter/стрелками/мышью.
struct Typed { WORD vk, scan; BYTE shift, caps; };
static Typed g_typed[256]; static int g_ntyped; static HWND g_typedWnd;
static SRWLOCK g_typedLock = SRWLOCK_INIT;
static int TypedCopy(Typed *out) {
    AcquireSRWLockShared(&g_typedLock);
    int n = g_ntyped;
    if (n && g_typedWnd != GetForegroundWindow()) n = 0;
    memcpy(out, g_typed, n * sizeof(Typed));
    ReleaseSRWLockShared(&g_typedLock);
    return n;
}

// Последняя смена раскладки у выделенного текста: повторное нажатие возвращает исходный текст (и снова туда).
// Действует, пока не было ввода, клика, другого хоткея или смены окна - всё это сбрасывает g_lastValid.
static LONG g_lastValid; static wchar_t *g_lfA, *g_lfB; static HKL g_lfHA, g_lfHB; static HWND g_lfWnd;   // A - сейчас в документе, B - второй вариант
static void LastFixSet(wchar_t *orig, wchar_t *conv, HKL from, HKL to) {
    Free(g_lfA); Free(g_lfB);
    g_lfA = conv; g_lfB = orig; g_lfHA = to; g_lfHB = from; g_lfWnd = GetForegroundWindow();
    InterlockedExchange(&g_lastValid, 1);
}
static int CaretLen(const wchar_t *s) {   // сколько раз Backspace стирает текст (CRLF - один шаг)
    int n = 0;
    for (; *s; s++) if (!(*s == L'\r' && s[1] == L'\n')) n++;
    return n;
}

static HKL FindHkl(WORD primary) {
    HKL l[32]; int n = GetKeyboardLayoutList(32, l);
    for (int i = 0; i < n; i++) if (PRIMARYLANGID(LOWORD((UINT_PTR)l[i])) == primary) return l[i];
    return NULL;
}
static WCHAR KeyChar(UINT vk, UINT scan, BOOL shift, BOOL caps, HKL hkl) {
    BYTE ks[256]; memset(ks, 0, 256);
    if (shift) ks[VK_SHIFT] = ks[VK_LSHIFT] = 0x80;
    if (caps) ks[VK_CAPITAL] = 1;
    WCHAR b[8]; int r = ToUnicodeEx(vk, scan, ks, b, 8, 4, hkl);
    return r == 1 ? b[0] : 0;
}
// Символ как если бы та же клавиша была нажата в другой раскладке.
static WCHAR ConvChar(WCHAR c, HKL src, HKL dst) {
    SHORT r = VkKeyScanExW(c, src);
    if (r == -1) return c;
    UINT vk = LOBYTE(r), sh = HIBYTE(r);
    if (sh & 6) return c;
    WCHAR o = KeyChar(vk, MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, dst), sh & 1, FALSE, dst);
    return o ? o : c;
}
// Раскладку переключаем с задержкой: иначе смена может обогнать только что отправленный Ctrl+V, и вставка не сработает.
static HKL g_swHkl; static HWND g_swWnd;
static void SwitchLayout(HKL h) {
    if (!g_layoutSwitch || !h) return;
    g_swHkl = h; g_swWnd = GetForegroundWindow();
    SetTimer(g_hwnd, TID_LAYOUT, 250, NULL);
}
static void OnLayoutTimer(void) {
    KillTimer(g_hwnd, TID_LAYOUT);
    if (g_swHkl && IsWindow(g_swWnd)) PostMessageW(g_swWnd, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)g_swHkl);
    g_swHkl = NULL;
}
// Исправить текст, набранный не в той раскладке: последний набранный, а если его нет - выделенный.
static void FixLayout(void) {
    HKL en = FindHkl(LANG_ENGLISH), ru = FindHkl(LANG_RUSSIAN);
    if (!en || !ru) { Toast(L"Для смены раскладки нужны раскладки EN и RU"); return; }
    static Typed t[256];
    int n = TypedCopy(t);
    HWND fg = GetForegroundWindow();
    if (n > 0) {
        HKL cur = GetKeyboardLayout(GetWindowThreadProcessId(fg, NULL));
        HKL dst = PRIMARYLANGID(LOWORD((UINT_PTR)cur)) == LANG_RUSSIAN ? en : ru;
        int s = 0;
        if (g_layoutWord) {
            int e = n; while (e > 0 && t[e - 1].vk == VK_SPACE) e--;
            s = e; while (s > 0 && t[s - 1].vk != VK_SPACE) s--;
        }
        int cnt = n - s;
        wchar_t *out = (wchar_t *)Alloc((cnt + 1) * sizeof(wchar_t));
        for (int i = 0; i < cnt; i++) {
            const Typed &k = t[s + i];
            UINT vk = k.vk;
            if (vk < VK_NUMPAD0 || vk > VK_DIVIDE) { UINT m = MapVirtualKeyExW(k.scan, MAPVK_VSC_TO_VK, dst); if (m) vk = m; }
            WCHAR c = KeyChar(vk, k.scan, k.shift, k.caps, dst);
            out[i] = c ? c : L'?';
        }
        SendKeysN(VK_BACK, cnt, FALSE);
        PasteText(out);
        SwitchLayout(dst);
        Free(out);
        return;
    }
    if (g_lastValid && g_lfA && g_lfWnd == fg) {   // сразу после смены раскладки выделенного: возвращаем как было
        SendKeysN(VK_BACK, CaretLen(g_lfA), 0);
        PasteText(g_lfB);
        SwitchLayout(g_lfHB);
        wchar_t *t2 = g_lfA; g_lfA = g_lfB; g_lfB = t2;
        HKL h = g_lfHA; g_lfHA = g_lfHB; g_lfHB = h;
        return;
    }
    BOOL line = FALSE;
    wchar_t *sel = GrabSelection(&line);
    if (line && sel) {   // выделения не было, редактор скопировал строку целиком: убираем перевод строки и выделяем строку, чтобы вставка её заменила
        int n = lstrlenW(sel);
        while (n && (sel[n - 1] == L'\n' || sel[n - 1] == L'\r')) sel[--n] = 0;
        if (n) { SendKeysN(VK_END, 1, 0); SendKeysN(VK_HOME, 2, VK_SHIFT); }
    }
    if (!sel || !*sel) { Free(sel); Toast(L"Нет текста для смены раскладки"); return; }
    int cyr = 0, lat = 0;
    for (wchar_t *p = sel; *p; p++) { if (*p >= 0x400 && *p <= 0x4FF) cyr++; else if ((*p | 32) >= L'a' && (*p | 32) <= L'z') lat++; }
    HKL from = cyr > lat ? ru : en, to = cyr > lat ? en : ru;
    wchar_t *orig = Dup(sel);
    for (wchar_t *p = sel; *p; p++) *p = ConvChar(*p, from, to);
    PasteText(sel);
    SwitchLayout(to);
    if (lstrcmpW(orig, sel)) LastFixSet(orig, sel, from, to);   // владение строками переходит к LastFixSet
    else { Free(orig); Free(sel); }
}

static void CopyToBuf(int n) {
    wchar_t *t = GrabSelection();
    if (!t || !*t) { Free(t); Toast(L"Нет выделенного текста"); return; }
    int len = lstrlenW(t);
    BufInsert(n, t);
    g_curBuf = n;
    wchar_t m[96]; wsprintfW(m, L"→ буфер %d   (%d симв.)", n, len);
    Toast(m);
}
static void PasteBuf(int n, int item) {
    if (item < 1 || item > g_bufN[n]) { wchar_t m[48]; wsprintfW(m, L"В буфере %d нет записи %d", n, item); Toast(m); return; }
    PasteText(g_buf[n][item - 1]);
}
static void OpenPopup(int mode, int buf) {
    if (g_hwndPop && g_pmode == mode && (mode != PM_BUF || !buf || buf == g_curBuf)) PopupClose();
    else PopupOpen(mode, buf);
}
static void ExecBind(const Bind *b) {
    Log(L"ExecBind act=%d n=%d", b->act, b->n);
    if (b->act != A_LAYOUT) InterlockedExchange(&g_lastValid, 0);
    switch (b->act) {
    case A_COPY: CopyToBuf(b->n); break;
    case A_PASTE: PasteBuf(b->n, b->n2); break;
    case A_BUFFER: OpenPopup(PM_BUF, b->n); break;
    case A_HIST: OpenPopup(PM_HIST, 0); break;
    case A_LAUNCH: OpenPopup(PM_LAUNCH, 0); break;
    case A_RUN: RunShell(b->arg, b->dir, FALSE); break;
    case A_ADMIN: RunShell(b->arg, b->dir, TRUE); break;
    case A_PY: RunPy(b->arg, b->dir); break;
    case A_PYRUN: RunPySelection(); break;
    case A_TOPMOST: ToggleTopmost(); break;
    case A_TOTRAY: HideToTray(); break;
    case A_LAYOUT: FixLayout(); break;
    case A_WEBCAM: CamOpen(); break;
    case A_PAUSE: InterlockedExchange(&g_paused, !g_paused); Toast(g_paused ? L"DKeys: пауза" : L"DKeys: работает"); break;
    }
}
static void ExecSpec(const wchar_t *spec) {
    Bind b; memset(&b, 0, sizeof b);
    if (ParseAction(spec, &b)) { ExecBind(&b); }
    Free(b.arg); Free(b.dir);
}

/* ---------- popup key handling ---------- */
static void PopupChoose(int vi) {
    if (vi < 0 || vi >= g_nvis) return;
    Row *r = &g_rows[g_vis[vi]];
    int mode = g_pmode;
    wchar_t *text = Dup(r->text);
    PopupClose();
    if (mode == PM_LAUNCH) ExecSpec(text); else PasteText(text);
    Free(text);
}
static void PopupReselect(int keep) {
    BuildRows();
    g_sel = keep; if (g_sel >= g_nvis) g_sel = g_nvis - 1; if (g_sel < 0) g_sel = 0;
    PopupEnsureVisible(); PopupRedraw();
}
static void PopupDelete(void) {
    if (g_nvis <= 0) return;
    int ref = g_rows[g_vis[g_sel]].ref;
    if (g_pmode == PM_BUF) BufRemove(g_curBuf, ref);
    else if (g_pmode == PM_HIST) { HistRemove(ref); HistSave(); }
    else { Toast(L"Список запуска правится в DKeys.ini"); return; }
    PopupReselect(g_sel);
}
static void PopupEdit(BOOL isNew) {
    if (g_pmode == PM_LAUNCH || (!isNew && g_nvis <= 0)) return;
    int mode = g_pmode, buf = g_curBuf, ref = isNew ? -1 : g_rows[g_vis[g_sel]].ref;
    wchar_t *cur = Dup(isNew ? L"" : g_rows[g_vis[g_sel]].text);
    PopupClose();
    wchar_t title[64];
    if (mode == PM_BUF) wsprintfW(title, isNew ? L"Буфер %d: новая запись" : L"Буфер %d: запись %d", buf, ref + 1);
    else lstrcpyW(title, L"Запись истории");
    wchar_t *nt = EditDialog(title, cur);
    Free(cur);
    if (nt && !*nt) { Free(nt); nt = NULL; if (!isNew) { if (mode == PM_BUF) BufRemove(buf, ref); else { HistRemove(ref); HistSave(); } } }
    if (nt) {
        if (mode == PM_BUF) {
            if (isNew) BufInsert(buf, nt);
            else { Free(g_buf[buf][ref]); g_buf[buf][ref] = nt; BufSave(buf); }
        } else { Free(g_hist[ref]); g_hist[ref] = nt; HistSave(); }
    }
    PopupOpen(mode, buf);
}
static void PopupSetMode(int m, int buf) {
    if (m == PM_HIST && !g_histOn) return;
    g_pmode = m; if (buf) g_curBuf = buf;
    g_nfilt = 0; g_filt[0] = 0; g_sel = g_top = 0;
    BuildRows(); PopupRedraw();
}
static void PopupTab(int i) {
    if (i < 9) PopupSetMode(PM_BUF, i + 1); else PopupSetMode(i == 9 ? PM_HIST : PM_LAUNCH, 0);
}
static void PopupKey(UINT key, UINT mods, WCHAR ch) {
    Log(L"PopupKey key=%x mods=%x ch=%x", key, mods, (int)ch);
    if (!g_hwndPop) return;
    int dig = -1;
    if (key >= L'0' && key <= L'9') dig = key - L'0'; else if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) dig = key - VK_NUMPAD0;
    BOOL buf = g_pmode == PM_BUF, plain = !(mods & (DM_CTRL | DM_ALT | DM_WIN));
    switch (key) {
    case VK_ESCAPE: PopupClose(); return;
    case VK_RETURN: PopupChoose(g_sel); return;
    case VK_UP:
        if ((mods & DM_CTRL) && buf && !g_nfilt && g_sel > 0) { BufMove(g_curBuf, g_sel, -1); PopupReselect(g_sel - 1); return; }
        if (g_sel > 0) g_sel--; PopupEnsureVisible(); PopupRedraw(); return;
    case VK_DOWN:
        if ((mods & DM_CTRL) && buf && !g_nfilt && g_sel < g_nvis - 1) { BufMove(g_curBuf, g_sel, 1); PopupReselect(g_sel + 1); return; }
        if (g_sel < g_nvis - 1) g_sel++; PopupEnsureVisible(); PopupRedraw(); return;
    case VK_LEFT: case VK_RIGHT:
        if (buf && !g_nfilt) { int b = g_curBuf + (key == VK_RIGHT ? 1 : -1); if (b < 1) b = 9; if (b > 9) b = 1; PopupSetMode(PM_BUF, b); }
        return;
    case VK_PRIOR: g_sel -= VISROWS - 1; if (g_sel < 0) g_sel = 0; PopupEnsureVisible(); PopupRedraw(); return;
    case VK_NEXT: g_sel += VISROWS - 1; if (g_sel > g_nvis - 1) g_sel = g_nvis - 1; if (g_sel < 0) g_sel = 0; PopupEnsureVisible(); PopupRedraw(); return;
    case VK_HOME: g_sel = 0; PopupEnsureVisible(); PopupRedraw(); return;
    case VK_END: g_sel = g_nvis ? g_nvis - 1 : 0; PopupEnsureVisible(); PopupRedraw(); return;
    case VK_TAB: { int m = (g_pmode + 1) % 3; if (m == PM_HIST && !g_histOn) m = PM_LAUNCH; PopupSetMode(m, 0); return; }
    case VK_DELETE: PopupDelete(); return;
    case VK_F2: PopupEdit(FALSE); return;
    case VK_INSERT: if (buf) PopupEdit(TRUE); return;
    case VK_BACK:
        if (g_nfilt) { g_filt[--g_nfilt] = 0; g_sel = g_top = 0; Refilter(); PopupRedraw(); }
        return;
    }
    if ((mods & DM_WIN) && dig >= 1) { PopupSetMode(PM_BUF, dig); return; }
    if ((mods & DM_CTRL) && dig >= 1 && g_pmode != PM_LAUNCH && g_nvis > 0) {
        BufInsert(dig, Dup(g_rows[g_vis[g_sel]].text));
        wchar_t m[64]; wsprintfW(m, L"→ буфер %d", dig); Toast(m);
        if (buf) { g_sel = 0; PopupReselect(0); }
        return;
    }
    if (buf && !g_nfilt && plain && dig >= 0) {
        int idx = dig ? dig - 1 : 9;
        if (idx < g_nvis) PopupChoose(idx);
        return;
    }
    if (ch >= 32 && plain && g_nfilt < 62) {
        g_filt[g_nfilt++] = ch; g_filt[g_nfilt] = 0; g_sel = g_top = 0; Refilter(); PopupRedraw();
    }
}

/* ---------- keyboard / mouse hooks (отдельный поток) ---------- */
static UINT NormKey(const KBDLLHOOKSTRUCT *k) {
    UINT vk = k->vkCode; BOOL ext = (k->flags & LLKHF_EXTENDED) != 0;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return vk;
    if (!ext) {
        switch (vk) {
        case VK_INSERT: return VK_NUMPAD0; case VK_END: return VK_NUMPAD1; case VK_DOWN: return VK_NUMPAD2;
        case VK_NEXT: return VK_NUMPAD3; case VK_LEFT: return VK_NUMPAD4; case VK_CLEAR: return VK_NUMPAD5;
        case VK_RIGHT: return VK_NUMPAD6; case VK_HOME: return VK_NUMPAD7; case VK_UP: return VK_NUMPAD8;
        case VK_PRIOR: return VK_NUMPAD9; case VK_DELETE: return VK_DECIMAL;
        }
    }
    return vk;
}
static LONG ModBit(UINT vk) {
    switch (vk) {
    case VK_LCONTROL: case VK_CONTROL: return 1; case VK_RCONTROL: return 2;
    case VK_LMENU: case VK_MENU: return 4; case VK_RMENU: return 8;
    case VK_LSHIFT: case VK_SHIFT: return 16; case VK_RSHIFT: return 32;
    case VK_LWIN: return 64; case VK_RWIN: return 128;
    }
    return 0;
}
static UINT Mods(void) {
    LONG b = g_modBits;
    return ((b & 3) ? DM_CTRL : 0) | ((b & 12) ? DM_ALT : 0) | ((b & 48) ? DM_SHIFT : 0) | ((b & 192) ? DM_WIN : 0);
}
static int FindBind(UINT mods, UINT key) {
    int r = -1;
    AcquireSRWLockShared(&g_bindLock);
    for (int i = 0; i < g_nbind; i++)
        if (g_bind[i].key == key && g_bind[i].mods == mods && !(g_bind[i].act == A_HIST && !g_histOn)) { r = i; break; }
    ReleaseSRWLockShared(&g_bindLock);
    return r;
}
static void MaskStart(void) {
    INPUT in[2]; memset(in, 0, sizeof in);
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = 0xE8; in[0].ki.dwExtraInfo = DKMARK;
    in[1] = in[0]; in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}
static WCHAR TranslateChar(UINT key, const KBDLLHOOKSTRUCT *k, UINT mods) {
    if (mods & (DM_CTRL | DM_ALT | DM_WIN)) return 0;
    HWND fg = GetForegroundWindow();
    return KeyChar(key, k->scanCode, (mods & DM_SHIFT) != 0, (BOOL)g_caps, GetKeyboardLayout(fg ? GetWindowThreadProcessId(fg, NULL) : 0));
}
static void TypedClear(void) {
    InterlockedExchange(&g_lastValid, 0);
    AcquireSRWLockExclusive(&g_typedLock); g_ntyped = 0; ReleaseSRWLockExclusive(&g_typedLock);
}
static void TypedKey(const KBDLLHOOKSTRUCT *k, UINT mods) {
    UINT vk = k->vkCode;
    if (mods & (DM_CTRL | DM_ALT | DM_WIN)) { TypedClear(); return; }
    BOOL text = vk == VK_SPACE || (vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z') || (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE && vk != VK_SEPARATOR) ||
                (vk >= VK_OEM_1 && vk <= VK_OEM_3) || (vk >= VK_OEM_4 && vk <= VK_OEM_8) || vk == VK_OEM_102;
    if (!text && vk != VK_BACK && (vk == VK_RETURN || vk == VK_TAB || vk == VK_ESCAPE || vk == VK_DELETE || vk == VK_INSERT ||
                                   (vk >= VK_PRIOR && vk <= VK_DOWN))) { TypedClear(); return; }
    if (!text && vk != VK_BACK) return;
    InterlockedExchange(&g_lastValid, 0);
    AcquireSRWLockExclusive(&g_typedLock);
    if (vk == VK_BACK) { if (g_ntyped) g_ntyped--; }
    else {
        if (g_ntyped == 256) { memmove(g_typed, g_typed + 1, 255 * sizeof(Typed)); g_ntyped = 255; }
        Typed &t = g_typed[g_ntyped++];
        t.vk = (WORD)vk; t.scan = (WORD)k->scanCode; t.shift = (mods & DM_SHIFT) != 0; t.caps = (BYTE)g_caps;
        g_typedWnd = GetForegroundWindow();
    }
    ReleaseSRWLockExclusive(&g_typedLock);
}
static BYTE g_swallowed[256];
static HHOOK g_hkKbd, g_hkMouse;

static LRESULT CALLBACK LLKbd(int code, WPARAM wp, LPARAM lp) {
    KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lp;
    if (code != HC_ACTION || k->dwExtraInfo == DKMARK) return CallNextHookEx(NULL, code, wp, lp);
    BOOL down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
    UINT vk = k->vkCode;
    LONG mb = ModBit(vk);
    if (mb) {
        if (down) InterlockedOr(&g_modBits, mb); else InterlockedAnd(&g_modBits, ~mb);
        return CallNextHookEx(NULL, code, wp, lp);
    }
    if (vk == VK_CAPITAL && down) g_caps ^= 1;
    BYTE &sw = g_swallowed[vk & 0xFF];
    if (!down) { if (sw) { sw = 0; return 1; } return CallNextHookEx(NULL, code, wp, lp); }
    if (sw) return 1;
    if (g_paused || g_modal) return CallNextHookEx(NULL, code, wp, lp);
    UINT key = NormKey(k), mods = Mods();
    if (g_popup) {
        sw = 1;
        WCHAR ch = TranslateChar(vk, k, mods);
        PostMessageW(g_hwnd, WM_APP_KEY, vk, (LPARAM)(ch | (mods << 16)));
        return 1;
    }
    int i = FindBind(mods, key);
    if (i >= 0) {
        sw = 1;
        if (mods & DM_WIN) MaskStart();
        PostMessageW(g_hwnd, WM_APP_HOT, i, 0);
        return 1;
    }
    TypedKey(k, mods);
    return CallNextHookEx(NULL, code, wp, lp);
}
static LRESULT CALLBACK LLMouse(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN || wp == WM_XBUTTONDOWN)) {
        TypedClear();
        if (g_popup) {
            MSLLHOOKSTRUCT *m = (MSLLHOOKSTRUCT *)lp; RECT r;
            HWND p = g_hwndPop;
            if (p && GetWindowRect(p, &r) && !PtInRect(&r, m->pt)) PostMessageW(g_hwnd, WM_APP_CLOSE, 0, 0);
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}
static void InstallHooks(void) {
    if (g_hkKbd) UnhookWindowsHookEx(g_hkKbd);
    if (g_hkMouse) UnhookWindowsHookEx(g_hkMouse);
    g_hkKbd = SetWindowsHookExW(WH_KEYBOARD_LL, LLKbd, g_inst, 0);
    g_hkMouse = SetWindowsHookExW(WH_MOUSE_LL, LLMouse, g_inst, 0);
}
static DWORD WINAPI HookThread(LPVOID ev) {
    MSG m; PeekMessageW(&m, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    WNDCLASSW wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = ClipProc; wc.hInstance = g_inst; wc.lpszClassName = L"DKClip";
    RegisterClassW(&wc);
    g_hwndClip = CreateWindowExW(0, L"DKClip", NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, g_inst, NULL);
    InstallHooks();
    SetEvent((HANDLE)ev);
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.message == TM_REHOOK) InstallHooks();
        else DispatchMessageW(&m);
    }
    if (g_hkKbd) UnhookWindowsHookEx(g_hkKbd);
    if (g_hkMouse) UnhookWindowsHookEx(g_hkMouse);
    return 0;
}

/* ---------- tray ---------- */
static NOTIFYICONDATAW g_nid;
static void TrayAdd(void) {
    memset(&g_nid, 0, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid; g_nid.hWnd = g_hwnd; g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; g_nid.uCallbackMessage = WM_TRAY; g_nid.hIcon = g_icon;
    lstrcpyW(g_nid.szTip, g_elev ? L"DKeys (администратор)" : L"DKeys");
    // при автозапуске панель задач может быть ещё не готова - повторяем, пока значок не добавится
    if (Shell_NotifyIconW(NIM_ADD, &g_nid)) KillTimer(g_hwnd, TID_TRAY);
    else SetTimer(g_hwnd, TID_TRAY, 2000, NULL);
}
static BOOL IsElevated(void) {
    HANDLE t; TOKEN_ELEVATION e; DWORD n; BOOL r = FALSE;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) {
        if (GetTokenInformation(t, TokenElevation, &e, sizeof e, &n)) r = e.TokenIsElevated != 0;
        CloseHandle(t);
    }
    return r;
}
static int RunHidden(const wchar_t *cmd) {
    STARTUPINFOW si; PROCESS_INFORMATION pi; memset(&si, 0, sizeof si);
    si.cb = sizeof si; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    wchar_t *c = Dup(cmd); DWORD code = (DWORD)-1;
    if (CreateProcessW(NULL, c, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 15000); GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    Free(c); return (int)code;
}
static const wchar_t *kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static BOOL AutoGet(void) {
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"DKeys", RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS) return TRUE;
    return RunHidden(L"schtasks.exe /Query /TN DKeys") == 0;
}
// Из-под администратора - задача планировщика (запуск с повышенными правами без UAC), иначе - ключ Run.
static void AutoSet(BOOL on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) { RegDeleteValueW(k, L"DKeys"); RegCloseKey(k); }
    RunHidden(L"schtasks.exe /Delete /TN DKeys /F");
    if (!on) return;
    if (g_elev) {
        wchar_t dom[128], usr[128], user[260], tmp[MAX_PATH];
        GetEnvironmentVariableW(L"USERDOMAIN", dom, 128); GetEnvironmentVariableW(L"USERNAME", usr, 128);
        wsprintfW(user, L"%s\\%s", dom, usr);
        Buf x = {0};
        BufAdd(&x, L"\xFEFF<?xml version=\"1.0\" encoding=\"UTF-16\"?><Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">");
        BufAdd(&x, L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>"); BufAdd(&x, user); BufAdd(&x, L"</UserId></LogonTrigger></Triggers>");
        BufAdd(&x, L"<Principals><Principal id=\"A\"><UserId>"); BufAdd(&x, user); BufAdd(&x, L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>");
        BufAdd(&x, L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Enabled>true</Enabled></Settings>");
        BufAdd(&x, L"<Actions Context=\"A\"><Exec><Command>"); BufAdd(&x, g_exe); BufAdd(&x, L"</Command><WorkingDirectory>"); BufAdd(&x, g_dir); BufAdd(&x, L"</WorkingDirectory></Exec></Actions></Task>");
        GetTempPathW(MAX_PATH, tmp); Cat(tmp, MAX_PATH, L"dkeys_task.xml");
        WriteAtomic(tmp, x.p, x.n * sizeof(wchar_t)); Free(x.p);
        wchar_t cmd[2 * MAX_PATH]; wsprintfW(cmd, L"schtasks.exe /Create /TN DKeys /XML \"%s\" /F", tmp);
        if (RunHidden(cmd) != 0) Toast(L"Не удалось создать задачу автозапуска");
        DeleteFileW(tmp);
    } else if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        wchar_t v[MAX_PATH + 4]; v[0] = L'"'; v[1] = 0; Cat(v, MAX_PATH + 4, g_exe); Cat(v, MAX_PATH + 4, L"\"");
        RegSetValueExW(k, L"DKeys", 0, REG_SZ, (const BYTE *)v, (lstrlenW(v) + 1) * sizeof(wchar_t));
        RegCloseKey(k);
    }
}
static void RestartElevated(void) {
    SHELLEXECUTEINFOW e; memset(&e, 0, sizeof e);
    e.cbSize = sizeof e; e.fMask = SEE_MASK_FLAG_NO_UI; e.lpVerb = L"runas"; e.lpFile = g_exe; e.lpParameters = L"/wait"; e.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&e)) DestroyWindow(g_hwnd);
}
static void Reload(void) {
    BOOL was = g_histOn;
    LoadConfig();
    if (g_histOn != was) { if (g_histOn) AddClipboardFormatListener(g_hwnd); else RemoveClipboardFormatListener(g_hwnd); }
    PostThreadMessageW(g_hookTid, TM_REHOOK, 0, 0);
    Toast(L"Настройки перечитаны");
}
/* ---------- веб-камера (DirectShow): предпросмотр в окне ---------- */
struct CamRes { int w, h, idx; LONGLONG iv; };
#define CAMSTYLE (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX)
enum { CAM_DEV = 0x6000, CAM_RES = 0x6100, CAM_FULL = 0x6200 };
static HWND g_hwndCam;
static HMENU g_camMenu, g_camPopDev, g_camPopRes;
static IGraphBuilder *g_gb; static ICaptureGraphBuilder2 *g_cgb; static IBaseFilter *g_capf;
static IMediaControl *g_mc; static IVideoWindow *g_vwin;
static wchar_t g_camName[16][80];
static int g_ncam, g_curDev, g_curRes, g_nres, g_vidW, g_vidH;
static CamRes g_res[64];
static BOOL g_camFull; static RECT g_camSaved;

static void FreeMT(AM_MEDIA_TYPE *mt) {
    if (mt->cbFormat) CoTaskMemFree(mt->pbFormat);
    if (mt->pUnk) mt->pUnk->Release();
    CoTaskMemFree(mt);
}
// перечисляет камеры (имена в g_camName); если bind >= 0 - отдаёт её фильтр в *pf
static int CamEnum(int bind, IBaseFilter **pf) {
    ICreateDevEnum *de; IEnumMoniker *em; int n = 0;
    if (pf) *pf = NULL;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum, (void **)&de))) return 0;
    if (de->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &em, 0) == S_OK) {
        IMoniker *mk;
        while (n < 16 && em->Next(1, &mk, NULL) == S_OK) {
            IPropertyBag *pb; g_camName[n][0] = 0;
            if (SUCCEEDED(mk->BindToStorage(NULL, NULL, IID_IPropertyBag, (void **)&pb))) {
                VARIANT v; VariantInit(&v);
                if (SUCCEEDED(pb->Read(L"FriendlyName", &v, NULL)) && v.vt == VT_BSTR) lstrcpynW(g_camName[n], v.bstrVal, 80);
                VariantClear(&v); pb->Release();
            }
            if (!g_camName[n][0]) lstrcpyW(g_camName[n], L"Камера");
            if (n == bind && pf) mk->BindToObject(NULL, NULL, IID_IBaseFilter, (void **)pf);
            mk->Release(); n++;
        }
        em->Release();
    }
    de->Release();
    return n;
}
static void CamStop(void) {
    if (g_mc) g_mc->Stop();
    if (g_vwin) { g_vwin->put_Visible(OAFALSE); g_vwin->put_Owner(NULL); g_vwin->Release(); g_vwin = NULL; }
    if (g_mc) { g_mc->Release(); g_mc = NULL; }
    if (g_cgb) { g_cgb->Release(); g_cgb = NULL; }
    if (g_gb) { g_gb->Release(); g_gb = NULL; }
    if (g_capf) { g_capf->Release(); g_capf = NULL; }
    g_vidW = g_vidH = 0;
}
static IAMStreamConfig *CamCfg(void) {
    IAMStreamConfig *c = NULL;
    if (FAILED(g_cgb->FindInterface(&PIN_CATEGORY_PREVIEW, &MEDIATYPE_Video, g_capf, IID_IAMStreamConfig, (void **)&c)))
        g_cgb->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, g_capf, IID_IAMStreamConfig, (void **)&c);
    return c;
}
// список разрешений: на каждый размер - режим с самой высокой частотой кадров, от большего к меньшему
static void CamLoadRes(void) {
    g_nres = 0;
    IAMStreamConfig *c = CamCfg(); if (!c) return;
    int n = 0, sz = 0; BYTE caps[512];
    c->GetNumberOfCapabilities(&n, &sz);
    if (sz > (int)sizeof caps) { c->Release(); return; }
    for (int i = 0; i < n; i++) {
        AM_MEDIA_TYPE *mt;
        if (c->GetStreamCaps(i, &mt, caps) != S_OK) continue;
        if (mt->formattype == FORMAT_VideoInfo && mt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
            VIDEOINFOHEADER *vi = (VIDEOINFOHEADER *)mt->pbFormat;
            int w = vi->bmiHeader.biWidth, h = vi->bmiHeader.biHeight; if (h < 0) h = -h;
            LONGLONG iv = ((VIDEO_STREAM_CONFIG_CAPS *)caps)->MinFrameInterval;
            if (iv <= 0) iv = vi->AvgTimePerFrame;
            if (iv <= 0) iv = 1000000000;
            int k = 0; while (k < g_nres && !(g_res[k].w == w && g_res[k].h == h)) k++;
            if (k < g_nres) { if (iv < g_res[k].iv) { g_res[k].idx = i; g_res[k].iv = iv; } }
            else if (g_nres < 64 && w > 0 && h > 0) { CamRes r = {w, h, i, iv}; g_res[g_nres++] = r; }
        }
        FreeMT(mt);
    }
    c->Release();
    for (int i = 1; i < g_nres; i++) {            // вставками, по убыванию числа пикселей
        CamRes t = g_res[i]; int j = i - 1;
        while (j >= 0 && (LONGLONG)g_res[j].w * g_res[j].h < (LONGLONG)t.w * t.h) { g_res[j + 1] = g_res[j]; j--; }
        g_res[j + 1] = t;
    }
}
static void CamSetFmt(int ri) {
    IAMStreamConfig *c = CamCfg(); if (!c) return;
    AM_MEDIA_TYPE *mt; BYTE caps[512];
    if (c->GetStreamCaps(g_res[ri].idx, &mt, caps) == S_OK) {
        if (mt->formattype == FORMAT_VideoInfo && mt->cbFormat >= sizeof(VIDEOINFOHEADER)) ((VIDEOINFOHEADER *)mt->pbFormat)->AvgTimePerFrame = g_res[ri].iv;
        c->SetFormat(mt); FreeMT(mt);
    }
    c->Release();
}
static void CamLayout(void) {            // картинка по центру окна с сохранением пропорций, по бокам чёрное
    if (!g_vwin || g_vidW <= 0 || g_vidH <= 0) return;
    RECT c; GetClientRect(g_hwndCam, &c);
    int w = c.right, h = c.bottom;
    if (MulDiv(w, g_vidH, g_vidW) > h) w = MulDiv(h, g_vidW, g_vidH); else h = MulDiv(w, g_vidH, g_vidW);
    g_vwin->SetWindowPosition((c.right - w) / 2, (c.bottom - h) / 2, w, h);
}
static void CamMenuBuild(void) {
    while (GetMenuItemCount(g_camPopDev) > 0) DeleteMenu(g_camPopDev, 0, MF_BYPOSITION);
    while (GetMenuItemCount(g_camPopRes) > 0) DeleteMenu(g_camPopRes, 0, MF_BYPOSITION);
    for (int i = 0; i < g_ncam; i++) AppendMenuW(g_camPopDev, MF_STRING, CAM_DEV + i, g_camName[i]);
    if (g_ncam) CheckMenuRadioItem(g_camPopDev, 0, g_ncam - 1, g_curDev, MF_BYPOSITION);
    for (int i = 0; i < g_nres; i++) {
        wchar_t t[48]; int fps = (int)((10000000 + g_res[i].iv / 2) / g_res[i].iv);
        wsprintfW(t, L"%d×%d  %d fps", g_res[i].w, g_res[i].h, fps);
        AppendMenuW(g_camPopRes, MF_STRING, CAM_RES + i, t);
    }
    if (g_nres) CheckMenuRadioItem(g_camPopRes, 0, g_nres - 1, g_curRes, MF_BYPOSITION);
    DrawMenuBar(g_hwndCam);
}
static BOOL CamStart(int dev, int ri) {
    CamStop();
    g_curDev = dev; g_curRes = 0;
    CamEnum(dev, &g_capf);
    BOOL ok = g_capf != NULL;
    if (ok) ok = SUCCEEDED(CoCreateInstance(CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER, IID_IGraphBuilder, (void **)&g_gb))
              && SUCCEEDED(CoCreateInstance(CLSID_CaptureGraphBuilder2, NULL, CLSCTX_INPROC_SERVER, IID_ICaptureGraphBuilder2, (void **)&g_cgb));
    if (ok) {
        g_cgb->SetFiltergraph(g_gb); g_gb->AddFilter(g_capf, L"Cam");
        CamLoadRes();
        if (ri >= g_nres) ri = 0;
        if (g_nres) CamSetFmt(ri);
        g_curRes = ri;
        ok = SUCCEEDED(g_cgb->RenderStream(&PIN_CATEGORY_PREVIEW, &MEDIATYPE_Video, g_capf, NULL, NULL))
          && SUCCEEDED(g_gb->QueryInterface(IID_IVideoWindow, (void **)&g_vwin))
          && SUCCEEDED(g_gb->QueryInterface(IID_IMediaControl, (void **)&g_mc));
    }
    if (!ok) { CamStop(); g_nres = 0; Toast(L"Не удалось открыть камеру (занята другим приложением?)"); return FALSE; }
    g_vidW = g_nres ? g_res[ri].w : 640; g_vidH = g_nres ? g_res[ri].h : 480;
    IBasicVideo *bv;
    if (SUCCEEDED(g_gb->QueryInterface(IID_IBasicVideo, (void **)&bv))) {
        long a = 0, b = 0;
        if (SUCCEEDED(bv->GetVideoSize(&a, &b)) && a > 0 && b > 0) { g_vidW = a; g_vidH = b; }
        bv->Release();
    }
    g_vwin->put_Owner((OAHWND)g_hwndCam);
    g_vwin->put_MessageDrain((OAHWND)g_hwndCam);
    g_vwin->put_WindowStyle(WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
    CamLayout();
    g_vwin->put_Visible(OATRUE);
    g_mc->Run();
    wchar_t t[120]; wsprintfW(t, L"Вебкамера - %s", g_camName[dev]); SetWindowTextW(g_hwndCam, t);
    return TRUE;
}
// отступы рамки окна и предельный размер клиентской области: 3/4 монитора
static void CamLimits(HMONITOR mon, int *dw, int *dh, int *mcw, int *mch) {
    MONITORINFO mi; mi.cbSize = sizeof mi; GetMonitorInfoW(mon, &mi);
    RECT r = {0, 0, 0, 0}; AdjustWindowRectEx(&r, CAMSTYLE, TRUE, 0);
    *dw = r.right - r.left; *dh = r.bottom - r.top;
    *mcw = (mi.rcMonitor.right - mi.rcMonitor.left) * 3 / 4 - *dw;
    *mch = (mi.rcMonitor.bottom - mi.rcMonitor.top) * 3 / 4 - *dh;
}
static void CamSizeWindow(BOOL center) {      // родной размер картинки, но не больше 3/4 монитора; не увеличиваем
    if (g_camFull || g_vidW <= 0) return;
    POINT pt; GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (!center) mon = MonitorFromWindow(g_hwndCam, MONITOR_DEFAULTTONEAREST);
    int dw, dh, mcw, mch; CamLimits(mon, &dw, &dh, &mcw, &mch);
    int cw = g_vidW, ch = g_vidH;
    if (cw > mcw) { cw = mcw; ch = MulDiv(cw, g_vidH, g_vidW); }
    if (ch > mch) { ch = mch; cw = MulDiv(ch, g_vidW, g_vidH); }
    int W = cw + dw, H = ch + dh;
    if (center) {
        MONITORINFO mi; mi.cbSize = sizeof mi; GetMonitorInfoW(mon, &mi);
        SetWindowPos(g_hwndCam, NULL, (mi.rcMonitor.left + mi.rcMonitor.right - W) / 2, (mi.rcMonitor.top + mi.rcMonitor.bottom - H) / 2, W, H, SWP_NOZORDER | SWP_NOACTIVATE);
    } else SetWindowPos(g_hwndCam, NULL, 0, 0, W, H, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
    CamLayout();
}
static void CamFull(BOOL on) {
    if (on == g_camFull) return;
    if (on) {
        MONITORINFO mi; mi.cbSize = sizeof mi;
        GetMonitorInfoW(MonitorFromWindow(g_hwndCam, MONITOR_DEFAULTTONEAREST), &mi);
        GetWindowRect(g_hwndCam, &g_camSaved);
        g_camFull = TRUE;
        SetMenu(g_hwndCam, NULL);
        SetWindowLongW(g_hwndCam, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hwndCam, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        g_camFull = FALSE;
        SetWindowLongW(g_hwndCam, GWL_STYLE, CAMSTYLE | WS_VISIBLE);
        SetMenu(g_hwndCam, g_camMenu);
        SetWindowPos(g_hwndCam, NULL, g_camSaved.left, g_camSaved.top, g_camSaved.right - g_camSaved.left, g_camSaved.bottom - g_camSaved.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
    }
    CamLayout();
}
static LRESULT CALLBACK CamProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id >= CAM_DEV && id < CAM_DEV + 16) { if (CamStart(id - CAM_DEV, 0)) CamSizeWindow(FALSE); CamMenuBuild(); }
        else if (id >= CAM_RES && id < CAM_RES + 64) { if (CamStart(g_curDev, id - CAM_RES)) CamSizeWindow(FALSE); CamMenuBuild(); }
        else if (id == CAM_FULL) CamFull(!g_camFull);
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_F11) CamFull(!g_camFull); else if (w == VK_ESCAPE) CamFull(FALSE);
        return 0;
    case WM_SIZE: CamLayout(); return 0;
    case WM_ERASEBKGND: { RECT r; GetClientRect(h, &r); FillRect((HDC)w, &r, (HBRUSH)GetStockObject(BLACK_BRUSH)); return 1; }
    case WM_GETMINMAXINFO:
        if (!g_camFull) {
            MINMAXINFO *mm = (MINMAXINFO *)l; int dw, dh, mcw, mch;
            CamLimits(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &dw, &dh, &mcw, &mch);
            mm->ptMaxTrackSize.x = mcw + dw; mm->ptMaxTrackSize.y = mch + dh;
            mm->ptMinTrackSize.x = 360; mm->ptMinTrackSize.y = 160;
        }
        return 0;
    case WM_SIZING:
        if (!g_camFull && g_vidW > 0) {          // тянем рамку - держим пропорции картинки
            RECT *r = (RECT *)l, wr, cr; GetWindowRect(h, &wr); GetClientRect(h, &cr);
            int dw = wr.right - wr.left - cr.right, dh = wr.bottom - wr.top - cr.bottom, a, b, mcw, mch;
            CamLimits(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &a, &b, &mcw, &mch);
            int cw = r->right - r->left - dw, ch = r->bottom - r->top - dh;
            if (w == WMSZ_TOP || w == WMSZ_BOTTOM) cw = MulDiv(ch, g_vidW, g_vidH); else ch = MulDiv(cw, g_vidH, g_vidW);
            if (cw > mcw) { cw = mcw; ch = MulDiv(cw, g_vidH, g_vidW); }
            if (ch > mch) { ch = mch; cw = MulDiv(ch, g_vidW, g_vidH); }
            if (w == WMSZ_LEFT || w == WMSZ_TOPLEFT || w == WMSZ_BOTTOMLEFT) r->left = r->right - (cw + dw); else r->right = r->left + cw + dw;
            if (w == WMSZ_TOP || w == WMSZ_TOPLEFT || w == WMSZ_TOPRIGHT) r->top = r->bottom - (ch + dh); else r->bottom = r->top + ch + dh;
            return TRUE;
        }
        break;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY:
        CamStop();
        if (g_camFull) DestroyMenu(g_camMenu);
        g_camFull = FALSE; g_hwndCam = NULL;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static void CamOpen(void) {
    if (g_hwndCam) { if (IsIconic(g_hwndCam)) ShowWindow(g_hwndCam, SW_RESTORE); SetForegroundWindow(g_hwndCam); return; }
    g_ncam = CamEnum(-1, NULL);
    if (!g_ncam) { Toast(L"Камеры не найдены"); return; }
    RegClass(L"DKCam", CamProc, 0);
    g_camMenu = CreateMenu(); g_camPopDev = CreatePopupMenu(); g_camPopRes = CreatePopupMenu();
    AppendMenuW(g_camMenu, MF_POPUP, (UINT_PTR)g_camPopDev, L"Камера");
    AppendMenuW(g_camMenu, MF_POPUP, (UINT_PTR)g_camPopRes, L"Разрешение");
    AppendMenuW(g_camMenu, MF_STRING, CAM_FULL, L"Полный экран (F11)");
    g_hwndCam = CreateWindowExW(0, L"DKCam", L"Вебкамера", CAMSTYLE, CW_USEDEFAULT, CW_USEDEFAULT, 640, 480, NULL, g_camMenu, g_inst, NULL);
    if (!g_hwndCam) { DestroyMenu(g_camMenu); return; }
    CamStart(0, 0);
    CamMenuBuild();
    CamSizeWindow(TRUE);
    ShowWindow(g_hwndCam, SW_SHOW);
    SetForegroundWindow(g_hwndCam);
}

static void TrayMenu(void) {
    POINT pt; GetCursorPos(&pt);
    HMENU m = CreatePopupMenu(), hm = NULL;
    AppendMenuW(m, MF_STRING, 1, L"Буферы…");
    AppendMenuW(m, g_histOn ? MF_STRING : MF_STRING | MF_GRAYED, 2, L"История…");
    AppendMenuW(m, MF_STRING, 3, L"Быстрый запуск…");
    AppendMenuW(m, MF_STRING, 4, L"Вебкамеры…");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_histOn ? MF_CHECKED : 0), 10, L"Своя история буфера (Win+V)");
    AppendMenuW(m, MF_STRING | (AutoGet() ? MF_CHECKED : 0), 11, L"Автозапуск");
    AppendMenuW(m, MF_STRING | (g_paused ? MF_CHECKED : 0), 15, L"Пауза");
    if (!g_elev) AppendMenuW(m, MF_STRING, 17, L"Перезапустить от администратора");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    for (int i = g_nhid - 1; i >= 0; i--) if (!IsWindow(g_hid[i])) { memmove(g_hid + i, g_hid + i + 1, (g_nhid - i - 1) * sizeof(HWND)); g_nhid--; }
    if (g_nhid) {
        hm = CreatePopupMenu();
        for (int i = 0; i < g_nhid; i++) {
            wchar_t t[64], e[140]; GetWindowTextW(g_hid[i], t, 50);
            int o = 0; for (int j = 0; t[j] && o < 130; j++) { if (t[j] == L'&') e[o++] = L'&'; e[o++] = t[j]; }
            e[o] = 0;
            AppendMenuW(hm, MF_STRING, 200 + i, *e ? e : L"(без названия)");
        }
        AppendMenuW(m, MF_POPUP, (UINT_PTR)hm, L"Спрятанные окна");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    }
    AppendMenuW(m, MF_STRING, 12, L"Открыть DKeys.ini");
    AppendMenuW(m, MF_STRING, 13, L"Открыть папку memory");
    AppendMenuW(m, MF_STRING, 14, L"Перечитать настройки");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 16, L"Выход");
    SetForegroundWindow(g_hwnd);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (c) {
    case 1: PopupOpen(PM_BUF); break;
    case 2: PopupOpen(PM_HIST); break;
    case 3: PopupOpen(PM_LAUNCH); break;
    case 4: CamOpen(); break;
    case 10: SetHistory(!g_histOn); break;
    case 11: AutoSet(!AutoGet()); break;
    case 15: InterlockedExchange(&g_paused, !g_paused); break;
    case 17: RestartElevated(); break;
    case 12: ShellExecuteW(NULL, NULL, L"notepad.exe", g_ini, NULL, SW_SHOWNORMAL); break;
    case 13: ShellExecuteW(NULL, L"open", g_memDir, NULL, NULL, SW_SHOWNORMAL); break;
    case 14: Reload(); break;
    case 16: DestroyWindow(g_hwnd); break;
    default: if (c >= 200 && c < 200 + 32) ShowHidden(c - 200);
    }
}

/* ---------- main window ---------- */
static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_APP_HOT: {
        if (g_modal || (int)w >= g_nbind) return 0;
        Bind b = g_bind[w];
        ExecBind(&b);
        return 0;
    }
    case WM_APP_KEY: PopupKey((UINT)w, (UINT)(l >> 16) & 0xFF, (WCHAR)(l & 0xFFFF)); return 0;
    case WM_APP_CLOSE: PopupClose(); return 0;
    case WM_APP_PCLICK: PopupChoose((int)w); return 0;
    case WM_APP_PTAB: PopupTab((int)w); return 0;
    case WM_TRAY:
        if (l == WM_LBUTTONUP) { if (g_hwndPop) PopupClose(); else PopupOpen(PM_BUF); }
        else if (l == WM_RBUTTONUP) TrayMenu();
        return 0;
    case WM_TIMER:
        if (w == TID_RESTORE) OnRestoreTimer();
        else if (w == TID_POPWATCH) PopupClose();
        else if (w == TID_LAYOUT) OnLayoutTimer();
        else if (w == TID_POPFG) OnPopFg();
        else if (w == TID_TRAY) TrayAdd();
        return 0;
    case WM_CLIPBOARDUPDATE: OnClip(); return 0;
    case WM_APP_RENDERED: if (g_psPending) SetTimer(g_hwnd, TID_RESTORE, g_pasteDelay, NULL); return 0;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION: if (w) { for (int i = 0; i < g_nhid; i++) if (IsWindow(g_hid[i])) ShowWindow(g_hid[i], SW_SHOW); } return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    if (m == g_msgTaskbar && m) { TrayAdd(); return 0; }
    return DefWindowProcW(h, m, w, l);
}

static BOOL RegClass(const wchar_t *name, WNDPROC proc, UINT style) {
    WNDCLASSW wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = proc; wc.hInstance = g_inst; wc.lpszClassName = name; wc.style = style;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    return RegisterClassW(&wc) != 0;
}
static void InitPaths(void) {
    GetModuleFileNameW(NULL, g_exe, MAX_PATH);
    lstrcpynW(g_dir, g_exe, MAX_PATH);
    wchar_t *p = g_dir + lstrlenW(g_dir);
    while (p > g_dir && p[-1] != L'\\') p--;
    *p = 0;
    lstrcpynW(g_ini, g_dir, MAX_PATH); Cat(g_ini, MAX_PATH, L"DKeys.ini");
    lstrcpynW(g_memDir, g_dir, MAX_PATH); Cat(g_memDir, MAX_PATH, L"memory\\");
    lstrcpynW(g_pyDir, g_dir, MAX_PATH); Cat(g_pyDir, MAX_PATH, L"py\\");
}

static int Run(void) {
    g_heap = GetProcessHeap();
    g_inst = GetModuleHandleW(NULL);
    BOOL wait = ContainsI(GetCommandLineW(), L"/wait");
    HANDLE mx = NULL;
    for (int i = 0; i < 50; i++) {
        mx = CreateMutexW(NULL, TRUE, L"Local\\DKeys_single_instance");
        if (GetLastError() != ERROR_ALREADY_EXISTS) break;
        CloseHandle(mx); mx = NULL;
        if (!wait) return 0;
        Sleep(100);
    }
    if (!mx) return 0;
    g_elev = IsElevated();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    InitPaths();
    CreateDirectoryW(g_memDir, NULL); CreateDirectoryW(g_pyDir, NULL);
    LoadConfig(); BuffersLoad(); HistLoad();
    RegClass(L"DKMain", MainProc, 0); RegClass(L"DKToast", ToastProc, CS_DROPSHADOW);
    RegClass(L"DKPop", PopProc, CS_DROPSHADOW); RegClass(L"DKEdit", EdProc, 0);
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"DKMain", L"DKeys", WS_POPUP, 0, 0, 0, 0, NULL, NULL, g_inst, NULL);
    if (!g_hwnd) return 1;
    InitFormats();
    g_msgTaskbar = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_hwnd, g_msgTaskbar, MSGFLT_ALLOW, NULL);   // от админа иначе не получим сообщение от explorer
    g_icon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    TrayAdd();
    g_caps = GetKeyState(VK_CAPITAL) & 1;
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE ht = CreateThread(NULL, 0, HookThread, ev, 0, &g_hookTid);
    WaitForSingleObject(ev, 3000); CloseHandle(ev);
    SetThreadPriority(ht, THREAD_PRIORITY_ABOVE_NORMAL);
    if (g_histOn) AddClipboardFormatListener(g_hwnd);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    for (int i = 0; i < g_nhid; i++) if (IsWindow(g_hid[i])) ShowWindow(g_hid[i], SW_SHOW);
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    PostThreadMessageW(g_hookTid, WM_QUIT, 0, 0);
    WaitForSingleObject(ht, 1000);
    CloseHandle(mx);
    return 0;
}

extern "C" void start(void) { ExitProcess(Run()); }
