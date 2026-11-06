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
#include <commdlg.h>
#include <xmmintrin.h>
#include <gdiplus.h>
#define NO_DSHOW_STRSAFE
#include <dshow.h>
#include <uiautomation.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propidl.h>
DEFINE_GUID(CLSID_MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(IID_IMMDeviceEnumerator, 0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(IID_IAudioClient, 0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(IID_IAudioCaptureClient, 0xC8ADBD64, 0xE71E, 0x48A0, 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17);
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
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "winmm.lib")

#pragma optimize("", off)
extern "C" {
void *memset(void *d, int c, size_t n) { BYTE *p = (BYTE *)d; while (n--) *p++ = (BYTE)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { BYTE *p = (BYTE *)d; const BYTE *q = (const BYTE *)s; while (n--) *p++ = *q++; return d; }
int _fltused = 0;      // нужен, раз используются float без CRT
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
enum { A_NONE, A_COPY, A_PASTE, A_BUFFER, A_HIST, A_LAUNCH, A_RUN, A_ADMIN, A_PY, A_PYRUN, A_TOPMOST, A_TOTRAY, A_LAYOUT, A_PAUSE, A_WEBCAM, A_STOPWATCH, A_TIMER, A_DRAW, A_PICKER, A_RECORD, A_SCRIPTS };
enum { PM_BUF, PM_HIST, PM_LAUNCH, PM_SCRIPTS };
enum {
    WM_APP_HOT = WM_APP + 1, WM_APP_KEY, WM_APP_CLOSE, WM_APP_PCLICK, WM_APP_PTAB, WM_TRAY, WM_APP_RENDERED,
    TM_MOUSE = WM_APP + 20, TM_REHOOK
};
#define TID_RESTORE 1
#define TID_POPWATCH 2
#define TID_LAYOUT 3
#define TID_POPFG 4
#define TID_TRAY 5
#define TID_NAV 6
#define VISROWS 10

struct Bind { UINT mods, key; int act, n, n2; wchar_t *arg, *dir; };
struct KV { wchar_t *k, *v; };
struct Row { wchar_t *label, *title, *text; int ref; };
struct Snap { UINT fmt[64]; HGLOBAL h[64]; int n; };
struct Buf { wchar_t *p; int n, cap; };
static void CamOpen(void);
static IUnknown *CamObj(int k);
static void TmToggle(int mode);
static void DrawOpen(void);
static void PickerOpen(void);
static void RecOpen(void);
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
static int g_mouseSlow = 4;   // [General] MouseSlow: во сколько раз замедлять мышь при зажатом Alt в пипетке (0/1 - выкл)
static volatile LONG g_pickActive;
static BOOL g_rNoTb = TRUE, g_rCur = TRUE; static int g_rFps = 30, g_rPos[2] = {-99999, -99999}, g_rScr;   // g_rScr: 0 - монитор с кнопкой, -1 - весь рабочий стол, N - монитор N
static BOOL g_rDummy;
struct OvCfg { wchar_t stem[64]; int mode, x, y, sc; };      // mode: 0 выкл, 1 на видео, 2 отдельный файл; x,y - доля кадра в ‰; sc - масштаб в %
static OvCfg g_ovc[24]; static int g_novc; static BOOL g_rPwd = TRUE, g_rShow;
static void RecShowSync(void);
static volatile BYTE g_ioKey[65536], g_ioMouse[8]; static volatile DWORD g_ioHit[65536], g_ioMHit[8]; static volatile DWORD g_ioWheelT; static volatile int g_ioWheelD;   // состояние ввода для оверлея (из хуков)
static OvCfg *OvCfgGet(const wchar_t *stem, BOOL create) {
    for (int i = 0; i < g_novc; i++) if (!lstrcmpiW(g_ovc[i].stem, stem)) return &g_ovc[i];
    if (!create || g_novc >= 24) return NULL;
    OvCfg *c = &g_ovc[g_novc++]; lstrcpynW(c->stem, stem, 64); c->mode = 0; c->x = 700; c->y = 650; c->sc = 100; return c;
} static wchar_t g_rMic[80], g_rDir[MAX_PATH];   // [Record] в ini: без панели задач, курсор, кадры/с, позиция кнопки
static BOOL g_dLive; static int g_dCol, g_dWid = 1; static wchar_t g_dSaveDir[MAX_PATH];   // [Draw] в ini: режим, цвет, толщина, папка
static COLORREF g_tmFg = RGB(255, 255, 255), g_tmBg = RGB(0, 0, 0);               // [Timer] в ini
static int g_tmFav[10], g_tmNFav;                                                  // избранное: секунды
static int g_tmAlpha = 60, g_tmSize = 48, g_tmEnd, g_tmPos[2] = {80, 80};          // прозрачность фона %, высота шрифта, действие по окончании, место
static wchar_t g_cfgCamDev[80]; static int g_cfgCamW, g_cfgCamH, g_cfgWin[4];   // [Camera] в ini: камера, разрешение, x,y,ширина,высота окна
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

/* ---------- тексты интерфейса (langs\<Language>.ini, секция [Text]; здесь - встроенные русские по умолчанию) ---------- */
#define LANG_LIST(X) \
X(L_TRAY_BUF, "tray.buffers", L"Буферы…") \
X(L_TRAY_HIST, "tray.history", L"История…") \
X(L_TRAY_LAUNCH, "tray.launcher", L"Быстрый запуск…") \
X(L_TRAY_CAM, "tray.webcam", L"Вебкамеры…") \
X(L_TRAY_DRAW, "tray.draw", L"Рисовалка на экране…") \
X(L_TRAY_PICK, "tray.picker", L"Пипетка (цвет в буфер)…") \
X(L_TRAY_REC, "tray.record", L"Запись экрана…") \
X(L_TRAY_RECSTOP, "tray.record_stop", L"Остановить запись") \
X(L_TRAY_RECSET, "tray.record_settings", L"Настройки записи") \
X(L_REC_NOTB, "rec.no_taskbar", L"Не снимать панель задач") \
X(L_REC_CUR, "rec.cursor", L"Снимать курсор мыши") \
X(L_REC_FPS, "rec.fps", L"%d кадров/с") \
X(L_REC_ALL, "rec.desktop", L"Весь рабочий стол") \
X(L_REC_BTN, "rec.btn_monitor", L"Монитор с кнопкой") \
X(L_REC_MON, "rec.monitor", L"Монитор %d (%dx%d)") \
X(L_REC_MIC, "rec.mic", L"Микрофон") \
X(L_REC_MICDEF, "rec.mic_default", L"По умолчанию") \
X(L_REC_MICOFF, "rec.mic_off", L"Без звука") \
X(L_REC_DIR, "rec.open_dir", L"Папка с видео") \
X(L_REC_SAVED, "rec.saved", L"Видео сохранено: %s") \
X(L_REC_FAIL, "rec.fail", L"Не удалось начать запись") \
X(L_REC_OV, "rec.ov", L"Оверлей ввода (клавиши, мышь)") \
X(L_REC_OV_OFF, "rec.ov_off", L"Выключен") \
X(L_REC_OV_VID, "rec.ov_video", L"На видео") \
X(L_REC_OV_FILE, "rec.ov_file", L"Отдельным файлом (зелёный фон)") \
X(L_REC_OV_CFG, "rec.ov_cfgall", L"Настроить положение и размер всех…") \
X(L_REC_OV_SHOW, "rec.ov_show", L"Показывать на экране") \
X(L_REC_OV_NOSEL, "rec.ov_nosel", L"Нет включённых оверлеев - выберите режим «На видео» или «Отдельным файлом»") \
X(L_REC_OV_TAG, "rec.ov_tag", L"файл") \
X(L_REC_OV_PWD, "rec.ov_pwd", L"Не показывать клавиши в полях пароля") \
X(L_REC_OV_DIR, "rec.ov_dir", L"Папка overlays") \
X(L_REC_OV_NONE, "rec.ov_none", L"Пресетов нет (папка overlays)") \
X(L_REC_OV_BAD, "rec.ov_bad", L"Не удалось загрузить пресет") \
X(L_REC_OV_HINT, "rec.ov_hint2", L"Тянуть - перенос, углы или колесо - масштаб, Enter или правый клик - готово, Esc - отмена. Все включённые оверлеи показаны сразу") \
X(L_REC_OV_WARN, "rec.ov_warn", L"Внимание: нажатые клавиши видны в видео - не вводите пароли") \
X(L_PK_COPIED, "picker.copied", L"Цвет %s скопирован в буфер") \
X(L_TRAY_TIMER, "tray.timer", L"Секундомер и таймер") \
X(L_TRAY_HISTON, "tray.history_on", L"Своя история буфера") \
X(L_TRAY_AUTO, "tray.autostart", L"Автозапуск") \
X(L_TRAY_PAUSE, "tray.pause", L"Пауза") \
X(L_TRAY_ELEV, "tray.elevate", L"Перезапустить от администратора") \
X(L_TRAY_NONAME, "tray.noname", L"(без названия)") \
X(L_TRAY_HIDDEN, "tray.hidden", L"Спрятанные окна") \
X(L_TRAY_INI, "tray.open_ini", L"Открыть DKeys.ini") \
X(L_TRAY_MEM, "tray.open_memory", L"Открыть папку memory") \
X(L_TRAY_RELOAD, "tray.reload", L"Перечитать настройки") \
X(L_TRAY_EXIT, "tray.exit", L"Выход") \
X(L_TRAY_TIPADM, "tray.tip_admin", L"DKeys (администратор)") \
X(L_TAB_HIST, "popup.tab_history", L"История") \
X(L_TAB_LAUNCH, "popup.tab_launch", L"Запуск") \
X(L_TAB_SCR, "popup.tab_scripts", L"Скрипты") \
X(L_INFO_SCR, "popup.info_scripts", L"T - tmp.py, 1-9 - остальные, Enter - запустить   Tab - режим   Esc - закрыть") \
X(L_EMPTY_SCR, "popup.scripts_empty", L"В папке py нет скриптов (*.py)") \
X(L_TRAY_SCR, "tray.scripts", L"Скрипты…") \
X(L_SEARCH, "popup.search", L"Введите для поиска…") \
X(L_INFO_LAUNCH, "popup.info_launch", L"Enter - запустить   Tab - режим   Esc - закрыть") \
X(L_INFO_BUF, "popup.info_buffer", L"%d симв.   цифра/Enter - вставить   Del - удалить   F2 - правка   Ins - новая   ←→ - буфер") \
X(L_INFO_HIST, "popup.info_history", L"%d симв.   Enter - вставить   Del - удалить   F2 - правка   Ctrl+1..9 - в буфер") \
X(L_EMPTY_NF, "popup.not_found", L"Ничего не найдено") \
X(L_EMPTY_BUF, "popup.buffer_empty", L"Буфер %d пуст. Ctrl+Num%d копирует выделенное сюда, Ins - ввести вручную") \
X(L_EMPTY_HIST, "popup.history_empty", L"История пуста") \
X(L_CANCEL, "dialog.cancel", L"Отмена") \
X(L_ED_NEW, "dialog.buffer_new", L"Буфер %d: новая запись") \
X(L_ED_BUF, "dialog.buffer_entry", L"Буфер %d: запись %d") \
X(L_ED_HIST, "dialog.history_entry", L"Запись истории") \
X(L_T_RUNFAIL, "toast.run_failed", L"Не удалось запустить: %.120s") \
X(L_T_PYFAIL, "toast.python_failed", L"Не удалось запустить Python (%.80s)") \
X(L_T_NOSEL, "toast.no_selection", L"Нет выделенного текста") \
X(L_T_TOPON, "toast.topmost_on", L"Поверх всех: ВКЛ  —  %s") \
X(L_T_TOPOFF, "toast.topmost_off", L"Поверх всех: ВЫКЛ  —  %s") \
X(L_T_TOTRAY, "toast.to_tray", L"Спрятано в трей: %s") \
X(L_T_NOLAYOUT, "toast.need_layouts", L"Для смены раскладки нужны раскладки EN и RU") \
X(L_T_NOLAYTXT, "toast.no_layout_text", L"Нет текста для смены раскладки") \
X(L_T_TOBUF, "toast.to_buffer_len", L"→ буфер %d   (%d симв.)") \
X(L_T_TOBUF2, "toast.to_buffer", L"→ буфер %d") \
X(L_T_NOITEM, "toast.no_item", L"В буфере %d нет записи %d") \
X(L_T_PAUSED, "toast.paused", L"DKeys: пауза") \
X(L_T_WORKING, "toast.working", L"DKeys: работает") \
X(L_T_LISTINI, "toast.launcher_in_ini", L"Список запуска правится в DKeys.ini") \
X(L_T_AUTOFAIL, "toast.autostart_failed", L"Не удалось создать задачу автозапуска") \
X(L_T_RELOADED, "toast.reloaded", L"Настройки перечитаны") \
X(L_CAM_NAME, "cam.default_name", L"Камера") \
X(L_CAM_P0, "cam.prop_device", L"Камера: яркость, контраст…") \
X(L_CAM_P1, "cam.prop_capture", L"Видеопоток: формат (capture pin)…") \
X(L_CAM_P2, "cam.prop_preview", L"Видеопоток: предпросмотр (preview pin)…") \
X(L_CAM_P3, "cam.prop_crossbar", L"Видеовход (crossbar)…") \
X(L_CAM_PTITLE, "cam.prop_title", L"Настройки камеры") \
X(L_CAM_BUSY, "cam.open_failed", L"Не удалось открыть камеру (занята другим приложением?)") \
X(L_CAM_TITLE, "cam.title_fmt", L"Вебкамера - %s") \
X(L_CAM_NONE, "cam.none", L"Камеры не найдены") \
X(L_CAM_MCAM, "cam.menu_camera", L"Камера") \
X(L_CAM_MRES, "cam.menu_resolution", L"Разрешение") \
X(L_CAM_MSET, "cam.menu_settings", L"Настройки") \
X(L_CAM_FULL, "cam.menu_fullscreen", L"Полный экран (F11)") \
X(L_CAM_WND, "cam.window", L"Вебкамера") \
X(L_TM_DONE, "timer.done", L"Таймер: время вышло") \
X(L_TM_NEEDTIME, "timer.need_time", L"Задайте время: меню - Время таймера") \
X(L_TM_INPUT, "timer.input_title", L"Таймер: минуты, м:с или ч:м:с") \
X(L_TM_CANCELED, "timer.shutdown_canceled", L"Выключение отменено") \
X(L_TM_SWPAUSE, "timer.sw_pause", L"Секундомер: пауза") \
X(L_TM_SWSTART, "timer.sw_start", L"Секундомер: старт") \
X(L_TM_SWRESET, "timer.sw_reset", L"Секундомер: сбросить") \
X(L_TM_TPAUSE, "timer.t_pause", L"Таймер: пауза") \
X(L_TM_TSTART, "timer.t_start", L"Таймер: старт") \
X(L_TM_TRESET, "timer.t_reset", L"Таймер: сбросить") \
X(L_TM_ENTER, "timer.enter_time", L"Ввести время…") \
X(L_TM_FAVADD, "timer.fav_add", L"Добавить текущее в избранное") \
X(L_TM_FAVDEL, "timer.fav_remove", L"Убрать из избранного") \
X(L_TM_TIME, "timer.time", L"Время таймера") \
X(L_TM_E0, "timer.end_signal", L"Только сигнал") \
X(L_TM_E1, "timer.end_off", L"Выключить компьютер") \
X(L_TM_E2, "timer.end_reboot", L"Перезагрузить компьютер") \
X(L_TM_END, "timer.end_action", L"По окончании таймера") \
X(L_TM_FG, "timer.color_font", L"Цвет цифр…") \
X(L_TM_BG, "timer.color_bg", L"Цвет фона…") \
X(L_TM_ALPHA, "timer.opacity", L"Непрозрачность фона") \
X(L_TM_SIZE, "timer.size", L"Размер цифр") \
X(L_TM_VIEW, "timer.view", L"Вид") \
X(L_TM_CANCEL, "timer.cancel_shutdown", L"Отменить выключение/перезагрузку") \
X(L_TM_CLOSE, "timer.close", L"Закрыть") \
X(L_D_TT0, "draw.tip_pen", L"Перо") \
X(L_D_TT1, "draw.tip_marker", L"Маркер") \
X(L_D_TT2, "draw.tip_arrow", L"Стрелка") \
X(L_D_TT3, "draw.tip_line", L"Линия") \
X(L_D_TT4, "draw.tip_rect", L"Прямоугольник") \
X(L_D_TT5, "draw.tip_ellipse", L"Овал") \
X(L_D_TT6, "draw.tip_text", L"Текст (Enter - готово, Shift+Enter - новая строка)") \
X(L_D_TT7, "draw.tip_number", L"Нумерованная метка") \
X(L_D_TT8, "draw.tip_blur", L"Скрыть (пикселизация), например пароль") \
X(L_D_TT9, "draw.tip_eraser", L"Ластик (убирает фигуру целиком)") \
X(L_D_TT10, "draw.tip_spot", L"Выделить область: всё остальное затемняется") \
X(L_D_TT11, "draw.tip_crop", L"Область сохранения: в буфер и в файл попадёт только она") \
X(L_D_TT12, "draw.tip_loupe", L"Лупа: увеличивает круг (радиус - до курсора, толщина - степень)") \
X(L_D_TT13, "draw.tip_undo", L"Отменить") \
X(L_D_TT14, "draw.tip_redo", L"Вернуть") \
X(L_D_TT15, "draw.tip_mode", L"Режим: снимок экрана / поверх живого экрана") \
X(L_D_TT16, "draw.tip_monitor", L"Следующий монитор, с Shift - предыдущий (рисунки очищаются)") \
X(L_D_TT17, "draw.tip_copy", L"Копировать в буфер и закрыть") \
X(L_D_TT18, "draw.tip_save", L"Сохранить в PNG и закрыть") \
X(L_D_TT19, "draw.tip_close", L"Закрыть без сохранения") \
X(L_D_TTHALO, "draw.tip_halo", L"Подсветка курсора (для показа экрана)") \
X(L_D_TTCOL, "draw.tip_color", L"Цвет") \
X(L_D_TTWID, "draw.tip_width", L"Толщина") \
X(L_D_STATIC, "draw.mode_static", L"Фото") \
X(L_D_COPIED, "draw.copied", L"Скриншот скопирован в буфер обмена") \
X(L_D_COPYFAIL, "draw.copy_failed", L"Не удалось скопировать скриншот") \
X(L_D_SAVED, "draw.saved", L"Сохранено: %s") \
X(L_D_SAVEFAIL, "draw.save_failed", L"Не удалось сохранить PNG") \
X(L_D_GDIFAIL, "draw.gdi_failed", L"Не удалось запустить GDI+") \
X(L_D_MEMFAIL, "draw.no_memory", L"Не хватает памяти для снимка экрана")
#define LE_(e, k, t) e,
#define LT_(e, k, t) {L##k, t},
enum { LANG_LIST(LE_) L_COUNT };
struct LangEnt { const wchar_t *key, *def; };
static const LangEnt kLang[L_COUNT] = { LANG_LIST(LT_) };
static wchar_t *g_lsTxt[L_COUNT]; static wchar_t g_lang[16] = L"ru";
static const wchar_t *Tr(int id) { return g_lsTxt[id] ? g_lsTxt[id] : kLang[id].def; }

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
; Описание - у каждой секции ниже. Секции модулей ([Draw] [Record] [Overlay] [Timer] [Camera]) правятся и из меню значка.

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
; 1 - после pyrun и запуска из меню скриптов окно остаётся открытым (pause)
PyRunPause=1
; Всплывающие подсказки внизу экрана
Toasts=1
; В пипетке при зажатом Alt мышь движется медленнее в N раз (точное наведение). 0 или 1 - выключено
MouseSlow=4
; Язык интерфейса: файл langs\<язык>.ini (создаётся сам; для перевода скопируйте ru.ini и поменяйте тексты)
Language=ru

[Hotkeys]
; Клавиши = действие
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
;     scripts                   меню скриптов из папки py (*.py): имя - первая строка-комментарий "# Имя", иначе имя файла; T - tmp.py, цифры - остальные, Enter - запустить
;     layout                    исправить раскладку: последний набранный текст, а если его нет - выделенный (строка, если выделения нет);
;                               повторное нажатие сразу после этого возвращает текст обратно
;     run КОМАНДА [@ ПАПКА]     запустить программу/файл/ссылку (ПАПКА - рабочая папка)
;     admin КОМАНДА [@ ПАПКА]   то же от администратора
;     py СКРИПТ [арг] [@ ПАПКА] скрипт из папки py. Выделенный текст: переменные окружения DKEYS_SEL и DKEYS_SEL_FILE
;     pyrun                     выделенный текст выполнить как скрипт Python (py\tmp.py)
;     topmost                   окно "поверх всех" вкл/выкл
;     totray                    спрятать окно (вернуть - из меню значка в трее)
;     pause                     пауза/продолжить хоткеи
;     webcam                    окно предпросмотра веб-камеры (секция [Camera])
;     stopwatch / timer         секундомер / таймер: старт-пауза (секция [Timer])
;     colorpicker / picker      пипетка: клик - цвет пикселя (#RRGGBB) в буфер, стрелки - сдвиг на 1 пиксель, Esc - отмена
;     draw                      рисовалка на экране (секция [Draw])
;     record                    запись экрана в mp4 (секции [Record] и [Overlay])
; Win+цифры буферов перекрывают запуск закреплённых программ на панели задач.
; В меню буфера: Del - удалить, F2 - править, Ins - новая запись, Ctrl+Вверх/Вниз - переставить,
; Влево/Вправо или Win+цифра - другой буфер, Ctrl+цифра - скопировать запись в буфер, Tab - история/запуск.
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
Win+P=scripts
Ctrl+Tilde=layout
Win+C=run calc.exe
Win+O=topmost
Win+NumMul=totray
Win+W=run python.exe wm.py @ D:\woodman
Win+Y=pyrun
Win+K=webcam
Win+S=draw
Win+Shift+C=colorpicker
Win+Shift+R=record
)INI";
static const char *kIni2 = u8R"INI(
[Launcher]
; Меню быстрого запуска (действие launcher): Название=команда (те же команды, что в run / admin)
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

[Draw]
; Рисовалка на экране (действие draw). Закомментированные строки - параметры со значением по умолчанию, уберите ; чтобы изменить.
; Внутри: P перо, M маркер, A стрелка, L линия, R прямоугольник, O овал, T текст (Enter - готово), N номер, B размытие, E ластик,
;   S затемнить всё кроме области, C область сохранения, Z лупа, 1-3 толщина, H подсветка курсора, Tab живой экран / снимок,
;   D и Shift+D - следующий / предыдущий монитор, Ctrl+мышь - двигать, менять размер и поворачивать готовую фигуру,
;   Ctrl+Z / Ctrl+Y - отмена / возврат, Enter или Ctrl+C - в буфер и закрыть, Ctrl+S - PNG в папку shots, Esc - закрыть.
; Mode: static - стоп-кадр экрана | live - поверх живого экрана
;Mode=static
; Color: 0-7 - номер цвета в палитре панели
;Color=0
; Width: 0 тонкая | 1 средняя | 2 толстая
;Width=1
; SaveDir: папка для PNG (пусто - shots рядом с exe)
;SaveDir=D:\Screens

[Record]
; Запись экрана в mp4 (действие record): кнопка-кружок - клик старт/стоп, Ctrl+клик - пауза (на паузе клик - продолжить, Ctrl+клик - стоп),
;   Shift+клик - выбрать область (Shift при рисовании - пропорции экрана; клик без рамки - сбросить область); хоткей при записи = стоп.
; NoTaskbar: 1 не снимать панель задач | 0 снимать весь монитор
;NoTaskbar=1
; Cursor: 1 снимать курсор мыши | 0 нет
;Cursor=1
; Fps: 15 | 30 | 60
;Fps=30
; Screen: 0 монитор с кнопкой | 1-8 монитор по номеру | all весь рабочий стол
;Screen=0
; Dir: папка для видео (пусто - стандартная "Видео" пользователя)
;Dir=D:\Videos
; Mic: пусто - микрофон по умолчанию | - без звука | точное имя устройства (проще выбрать в меню)
;Mic=
; Pos: положение кнопки записи x,y
;Pos=100,100

[Overlay]
; Оверлей ввода при записи (пресеты input-overlay: папка overlays\*\имя.json + имя.png). Строка на пресет: имя=режим,x,y,масштаб
;   режим: 0 выкл | 1 на видео | 2 отдельный mp4 на зелёном фоне (хромакей); x,y - положение в промилле кадра; масштаб в %
;   x,y и масштаб задаются в меню "Настроить положение и размер всех" (все включённые оверлеи сразу)
;имя_пресета=1,700,650,100
; Show: 1 пока на экране кнопка записи, показывать на экране оверлеи "на видео" (в запись не попадают) - видно, что они перекрывают | 0 нет
;Show=0
; HidePasswords: 1 не показывать клавиши в поле пароля | 0 показывать. ВНИМАНИЕ: нажатые клавиши видны в видео - не вводите пароли
;HidePasswords=1

[Timer]
; Секундомер и таймер (действия stopwatch, timer)
; FontColor, BgColor: цвета цифр и фона, RRGGBB
;FontColor=FFFFFF
;BgColor=000000
; BgAlpha: непрозрачность фона 0-100 %
;BgAlpha=60
; Size: высота шрифта 12-300
;Size=48
; EndAction по окончании таймера: 0 сигнал | 1 выключить компьютер | 2 перезагрузить
;EndAction=0
; Favorites: избранные времена в секундах через запятую (до 10)
;Favorites=60,300,600
; Pos: положение окна x,y
;Pos=80,80

[Camera]
; Веб-камера (действие webcam)
; Device: имя камеры | Resolution: ШxВ | Window: положение и размер окна x,y,ширина,высота
;Device=Integrated Camera
;Resolution=1920x1080
;Window=100,100,800,450
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
static void HkName(UINT mods, UINT vk, wchar_t *o) {
    o[0] = 0;
    if (mods & DM_WIN) Cat(o, 48, L"Win+");
    if (mods & DM_CTRL) Cat(o, 48, L"Ctrl+");
    if (mods & DM_ALT) Cat(o, 48, L"Alt+");
    if (mods & DM_SHIFT) Cat(o, 48, L"Shift+");
    wchar_t k[16] = L"";
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { k[0] = (wchar_t)vk; k[1] = 0; }
    else if (vk >= VK_F1 && vk <= VK_F24) wsprintfW(k, L"F%d", vk - VK_F1 + 1);
    else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) wsprintfW(k, L"Num%d", vk - VK_NUMPAD0);
    else for (int i = 0; i < (int)(sizeof kKeys / sizeof kKeys[0]); i++) if (kKeys[i].vk == vk) { lstrcpynW(k, kKeys[i].n, 16); break; }
    Cat(o, 48, k);
}
// Хоткей первого назначения действия act (для подписи в меню).
static BOOL HkFor(int act, wchar_t *o) {
    BOOL f = FALSE; o[0] = 0;
    AcquireSRWLockShared(&g_bindLock);
    for (int i = 0; i < g_nbind; i++) if (g_bind[i].act == act) { HkName(g_bind[i].mods, g_bind[i].key, o); f = TRUE; break; }
    ReleaseSRWLockShared(&g_bindLock);
    return f;
}
// Пункт меню "текст<TAB>хоткей"
static void AddHk(HMENU m, UINT fl, UINT_PTR id, const wchar_t *text, int act) {
    wchar_t b[200], hk[48];
    lstrcpynW(b, text, 150);
    if (act && HkFor(act, hk)) { Cat(b, 200, L"\t"); Cat(b, 200, hk); }
    AppendMenuW(m, fl, id, b);
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
        {L"copy", A_COPY, 1}, {L"paste", A_PASTE, 3}, {L"buffer", A_BUFFER, 4}, {L"slots", A_BUFFER, 4}, {L"layout", A_LAYOUT, 0}, {L"history", A_HIST, 0}, {L"launcher", A_LAUNCH, 0}, {L"scripts", A_SCRIPTS, 0},
        {L"run", A_RUN, 2}, {L"admin", A_ADMIN, 2}, {L"py", A_PY, 2}, {L"pyrun", A_PYRUN, 0}, {L"topmost", A_TOPMOST, 0},
        {L"totray", A_TOTRAY, 0}, {L"pause", A_PAUSE, 0}, {L"webcam", A_WEBCAM, 0}, {L"stopwatch", A_STOPWATCH, 0}, {L"timer", A_TIMER, 0}, {L"draw", A_DRAW, 0}, {L"record", A_RECORD, 0}, {L"colorpicker", A_PICKER, 0}, {L"picker", A_PICKER, 0},
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

static COLORREF HexRgb(const wchar_t *s) {
    DWORD v = 0; while (*s == L'#' || *s == L' ') s++;
    for (;; s++) {
        int d;
        if (*s >= L'0' && *s <= L'9') d = *s - L'0';
        else if (*s >= L'a' && *s <= L'f') d = *s - L'a' + 10;
        else if (*s >= L'A' && *s <= L'F') d = *s - L'A' + 10;
        else break;
        v = v * 16 + d;
    }
    return RGB((v >> 16) & 255, (v >> 8) & 255, v & 255);
}
static void CfgCb(const wchar_t *s, const wchar_t *k, const wchar_t *v) {
    if (!lstrcmpiW(s, L"General")) {
        if (!lstrcmpiW(k, L"Language")) lstrcpynW(g_lang, v, 16);
        else if (!lstrcmpiW(k, L"MouseSlow")) { g_mouseSlow = ToInt(v); if (g_mouseSlow < 0 || g_mouseSlow > 20) g_mouseSlow = 4; }
        else if (!lstrcmpiW(k, L"HistoryEnabled")) g_histOn = ToInt(v) != 0;
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
    } else if (!lstrcmpiW(s, L"Draw")) {
        if (!lstrcmpiW(k, L"Mode")) g_dLive = !lstrcmpiW(v, L"live");
        else if (!lstrcmpiW(k, L"Color")) { g_dCol = ToInt(v); if (g_dCol < 0 || g_dCol > 7) g_dCol = 0; }
        else if (!lstrcmpiW(k, L"Width")) { g_dWid = ToInt(v); if (g_dWid < 0 || g_dWid > 2) g_dWid = 1; }
        else if (!lstrcmpiW(k, L"SaveDir")) lstrcpynW(g_dSaveDir, v, MAX_PATH);
    } else if (!lstrcmpiW(s, L"Record")) {
        if (!lstrcmpiW(k, L"NoTaskbar")) g_rNoTb = ToInt(v) != 0;
        else if (!lstrcmpiW(k, L"Cursor")) g_rCur = ToInt(v) != 0;
        else if (!lstrcmpiW(k, L"AllScreens")) { if (ToInt(v)) g_rScr = -1; }
        else if (!lstrcmpiW(k, L"Screen")) g_rScr = !lstrcmpiW(v, L"all") ? -1 : ToInt(v) > 0 && ToInt(v) <= 8 ? ToInt(v) : 0;
        else if (!lstrcmpiW(k, L"Mic")) lstrcpynW(g_rMic, v, 80);
        else if (!lstrcmpiW(k, L"Dir")) lstrcpynW(g_rDir, v, MAX_PATH);
        else if (!lstrcmpiW(k, L"Fps")) { g_rFps = ToInt(v); if (g_rFps != 15 && g_rFps != 60) g_rFps = 30; }
        else if (!lstrcmpiW(k, L"Pos")) { const wchar_t *q = v; g_rPos[0] = ToInt(q); while (*q && *q != L',') q++; g_rPos[1] = *q ? ToInt(q + 1) : -99999; }
    } else if (!lstrcmpiW(s, L"Overlay")) {
        if (!lstrcmpiW(k, L"HidePasswords")) g_rPwd = ToInt(v) != 0;
        else if (!lstrcmpiW(k, L"Show")) g_rShow = ToInt(v) != 0;
        else { OvCfg *c = OvCfgGet(k, TRUE); if (c) { const wchar_t *q = v; c->mode = ToInt(q) & 3; if (c->mode == 3) c->mode = 1; int *f[3] = {&c->x, &c->y, &c->sc};
            for (int i = 0; i < 3; i++) { while (*q && *q != L',') q++; if (!*q) break; q++; *f[i] = ToInt(q); }
            if (c->sc < 10) c->sc = 10; if (c->sc > 600) c->sc = 600; } }
    } else if (!lstrcmpiW(s, L"Timer")) {
        if (!lstrcmpiW(k, L"FontColor")) g_tmFg = HexRgb(v);
        else if (!lstrcmpiW(k, L"BgColor")) g_tmBg = HexRgb(v);
        else if (!lstrcmpiW(k, L"BgAlpha")) { g_tmAlpha = ToInt(v); if (g_tmAlpha < 0) g_tmAlpha = 0; if (g_tmAlpha > 100) g_tmAlpha = 100; }
        else if (!lstrcmpiW(k, L"Size")) { g_tmSize = ToInt(v); if (g_tmSize < 12) g_tmSize = 12; if (g_tmSize > 300) g_tmSize = 300; }
        else if (!lstrcmpiW(k, L"EndAction")) { g_tmEnd = ToInt(v); if (g_tmEnd < 0 || g_tmEnd > 2) g_tmEnd = 0; }
        else if (!lstrcmpiW(k, L"Favorites")) {
            g_tmNFav = 0; const wchar_t *q = v;
            while (*q && g_tmNFav < 10) { int t = ToInt(q); if (t > 0) g_tmFav[g_tmNFav++] = t; while (*q && *q != L',') q++; if (*q) q++; }
        }
        else if (!lstrcmpiW(k, L"Pos")) { const wchar_t *q = v; g_tmPos[0] = ToInt(q); while (*q && *q != L',') q++; g_tmPos[1] = *q ? ToInt(q + 1) : 80; }
    } else if (!lstrcmpiW(s, L"Camera")) {
        if (!lstrcmpiW(k, L"Device")) lstrcpynW(g_cfgCamDev, v, 80);
        else if (!lstrcmpiW(k, L"Resolution")) {
            g_cfgCamW = ToInt(v); const wchar_t *x = v; while (*x && *x != L'x' && *x != L'X') x++;
            g_cfgCamH = *x ? ToInt(x + 1) : 0;
        } else if (!lstrcmpiW(k, L"Window")) {
            const wchar_t *q = v;
            for (int i = 0; i < 4; i++) { g_cfgWin[i] = ToInt(q); while (*q && *q != L',') q++; if (*q) q++; }
        }
    } else if (!lstrcmpiW(s, L"Launcher")) {
        g_launch = (KV *)Realloc(g_launch, (g_nlaunch + 1) * sizeof(KV));
        g_launch[g_nlaunch].k = Dup(k); g_launch[g_nlaunch].v = Dup(v); g_nlaunch++;
    }
}

static void FreeBinds(Bind *b, int n) {
    for (int i = 0; i < n; i++) { Free(b[i].arg); Free(b[i].dir); }
    Free(b);
}
static void LangCb(const wchar_t *s, const wchar_t *k, const wchar_t *v) {
    if (lstrcmpiW(s, L"Text")) return;
    for (int i = 0; i < L_COUNT; i++) if (!lstrcmpW(k, kLang[i].key)) { Free(g_lsTxt[i]); g_lsTxt[i] = Dup(v); return; }
}
// Читает langs\<Language>.ini; отсутствующие строки (или весь файл) дописывает из встроенных русских.
static void LoadLang(void) {
    for (int i = 0; i < L_COUNT; i++) { Free(g_lsTxt[i]); g_lsTxt[i] = NULL; }
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    lstrcpynW(dir, g_dir, MAX_PATH); Cat(dir, MAX_PATH, L"\\langs");
    lstrcpynW(path, dir, MAX_PATH); Cat(path, MAX_PATH, L"\\"); Cat(path, MAX_PATH, g_lang); Cat(path, MAX_PATH, L".ini");
    DWORD sz = 0; BYTE *raw = ReadAll(path, &sz);
    Buf all = {0};
    if (raw) { wchar_t *t = FromUtf8((char *)raw, sz); Free(raw); BufAdd(&all, t); IniParse(t, LangCb); Free(t); }
    else BufAdd(&all, L"; DKeys: тексты интерфейса. Правьте только то, что после знака =. Перевод: скопируйте файл в <язык>.ini и укажите Language=<язык> в DKeys.ini [General].\r\n[Text]\r\n");
    int miss = 0;
    for (int i = 0; i < L_COUNT; i++) {
        if (g_lsTxt[i]) continue;
        if (!miss && raw && all.n && all.p[all.n - 1] != L'\n') BufAdd(&all, L"\r\n");
        miss++; BufAdd(&all, kLang[i].key); BufAdd(&all, L"="); BufAdd(&all, kLang[i].def); BufAdd(&all, L"\r\n");
    }
    if (miss || !raw) {
        CreateDirectoryW(dir, NULL);
        int n; char *u = ToUtf8(all.p, &n); if (u) { WriteAtomic(path, u, n); Free(u); }
    }
    Free(all.p);
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
    LoadLang();
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
// вкладки: 0..8 - буферы 1..9, 9 - история, 10 - запуск, 11 - скрипты (режим = номер - 8)
#define NTABS 12
#define kTabName(i) Tr((i) == 0 ? L_TAB_HIST : (i) == 1 ? L_TAB_LAUNCH : L_TAB_SCR)

static Row *g_rows; static int g_nrows;
static wchar_t **g_scrNames; static int g_nscr;      // файлы вкладки "Скрипты" (ref строки - индекс сюда)
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
    for (int i = 0; i < g_nscr; i++) Free(g_scrNames[i]);
    Free(g_scrNames); g_scrNames = NULL; g_nscr = 0;
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
// имя скрипта: первая строка-комментарий (не shebang и не "coding"), иначе имя файла
static void ScrName(const wchar_t *t, wchar_t *out, int cap) {
    out[0] = 0;
    for (int ln = 0; ln < 4 && *t; ln++) {
        const wchar_t *e = t; while (*e && *e != L'\n') e++;
        const wchar_t *s = t; while (s < e && (*s == L' ' || *s == L'\t' || *s == L'\r')) s++;
        t = *e ? e + 1 : e;
        if (s >= e) continue;
        if (*s != L'#') return;
        s++;
        if (s < e && *s == L'!') continue;
        while (s < e && (*s == L' ' || *s == L'\t')) s++;
        const wchar_t *x = e; while (x > s && (x[-1] == L'\r' || x[-1] == L' ' || x[-1] == L'\t')) x--;
        int n = (int)(x - s); if (n <= 0) continue;
        if (n >= cap) n = cap - 1;
        memcpy(out, s, n * sizeof(wchar_t)); out[n] = 0;
        if (ContainsI(out, L"coding:") || ContainsI(out, L"coding=")) { out[0] = 0; continue; }
        return;
    }
}
static wchar_t **ScrList(int *cnt) {
    wchar_t pat[MAX_PATH]; lstrcpynW(pat, g_pyDir, MAX_PATH); Cat(pat, MAX_PATH, L"*.py");
    wchar_t **a = NULL; int n = 0;
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || n >= 200) continue;
            a = (wchar_t **)Realloc(a, (n + 1) * sizeof(wchar_t *)); a[n++] = Dup(fd.cFileName);
            if (!lstrcmpiW(fd.cFileName, L"tmp.py") && n > 1) { wchar_t *x = a[n - 1]; memmove(a + 1, a, (n - 1) * sizeof(wchar_t *)); a[0] = x; }      // tmp.py - первым (клавиша T)
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    *cnt = n; return a;
}
static void ScrRow(Row *r, const wchar_t *file, const wchar_t *label) {
    wchar_t path[MAX_PATH]; lstrcpynW(path, g_pyDir, MAX_PATH); Cat(path, MAX_PATH, file);
    DWORD sz = 0; BYTE *raw = ReadAll(path, &sz);
    if (sz > 6000) sz = 6000;
    wchar_t *txt = raw ? FromUtf8((char *)raw, sz) : Dup(L"");
    Free(raw);
    wchar_t nm[100]; ScrName(txt, nm, 100);
    r->label = Dup(label); r->title = Dup(nm[0] ? nm : file); r->text = txt;
}
static void BuildRows(void) {
    FreeRows();
    wchar_t **sf = NULL; int ns = 0;
    if (g_pmode == PM_SCRIPTS) { sf = ScrList(&ns); g_scrNames = sf; g_nscr = ns; }
    int n = g_pmode == PM_BUF ? g_bufN[g_curBuf] : g_pmode == PM_HIST ? g_nhist : g_pmode == PM_SCRIPTS ? ns : g_nlaunch;
    g_rows = (Row *)Alloc(sizeof(Row) * (n ? n : 1)); g_vis = (int *)Alloc(sizeof(int) * (n ? n : 1));
    for (int i = 0; i < n; i++) {
        Row *r = &g_rows[i];
        r->ref = i;
        if (g_pmode == PM_BUF) {
            wchar_t l[2] = {(wchar_t)(i < 9 ? L'1' + i : L'0'), 0};
            r->label = Dup(l); r->text = Dup(g_buf[g_curBuf][i]); r->title = Collapse(r->text);
        } else if (g_pmode == PM_HIST) {
            r->label = Dup(L""); r->text = Dup(g_hist[i]); r->title = Collapse(r->text);
        } else if (g_pmode == PM_SCRIPTS) {
            BOOL tmp = !lstrcmpiW(sf[i], L"tmp.py"); int k = i - (ns && !lstrcmpiW(sf[0], L"tmp.py") ? 1 : 0);
            wchar_t lb[2] = {(wchar_t)(tmp ? L'T' : k < 9 ? L'1' + k : k == 9 ? L'0' : 0), 0};     // T - tmp.py, остальные по цифрам 1-9,0
            ScrRow(r, sf[i], lb);
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
        BOOL cur = i < 9 ? (g_pmode == PM_BUF && g_curBuf == i + 1) : (g_pmode == i - 8);
        wchar_t dg[2] = {(wchar_t)(L'1' + i), 0};
        const wchar_t *tx = i < 9 ? dg : kTabName(i - 9);
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
    else { SetTextColor(dc, P.dim); DrawTextW(dc, Tr(L_SEARCH), -1, &fr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS); }
    // rows
    int y0 = g_hh, lw = g_pmode == PM_BUF || g_pmode == PM_SCRIPTS ? Sc(28) : 0;
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
        if (g_pmode == PM_LAUNCH) lstrcpynW(info, Tr(L_INFO_LAUNCH), 128);
        else if (g_pmode == PM_SCRIPTS) lstrcpynW(info, Tr(L_INFO_SCR), 128);
        else if (g_pmode == PM_BUF) wsprintfW(info, Tr(L_INFO_BUF), tl);
        else wsprintfW(info, Tr(L_INFO_HIST), tl);
        SetTextColor(dc, P.dim);
        DrawTextW(dc, info, -1, &ir, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else {
        SetTextColor(dc, P.dim);
        wchar_t e[96];
        if (g_pmode == PM_BUF) wsprintfW(e, g_nfilt ? Tr(L_EMPTY_NF) : Tr(L_EMPTY_BUF), g_curBuf, g_curBuf);
        else lstrcpynW(e, g_nfilt ? Tr(L_EMPTY_NF) : g_pmode == PM_HIST ? Tr(L_EMPTY_HIST) : g_pmode == PM_SCRIPTS ? Tr(L_EMPTY_SCR) : Tr(L_EMPTY_NF), 96);
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
static BOOL g_edLine;     // однострочный режим: маленькое окно, Enter = OK
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
    g_edEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | (g_edLine ? ES_AUTOHSCROLL : (WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN)),
                               0, 0, 10, 10, g_edDlg, (HMENU)3, g_inst, NULL);
    HWND ok = CreateWindowExW(0, L"BUTTON", g_edLine ? L"OK  (Enter)" : L"OK  (Ctrl+Enter)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, g_edDlg, (HMENU)IDOK, g_inst, NULL);
    HWND cn = CreateWindowExW(0, L"BUTTON", Tr(L_CANCEL), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, g_edDlg, (HMENU)IDCANCEL, g_inst, NULL);
    SendMessageW(g_edEdit, WM_SETFONT, (WPARAM)f, TRUE); SendMessageW(ok, WM_SETFONT, (WPARAM)f, TRUE); SendMessageW(cn, WM_SETFONT, (WPARAM)f, TRUE);
    SendMessageW(g_edEdit, EM_SETLIMITTEXT, 4000000, 0);
    SetWindowTextW(g_edEdit, text);
    int W = MulDiv(g_edLine ? 380 : 640, d, 96), H = MulDiv(g_edLine ? 150 : 420, d, 96);
    SetWindowPos(g_edDlg, HWND_TOPMOST, (mi.rcWork.left + mi.rcWork.right - W) / 2, (mi.rcWork.top + mi.rcWork.bottom - H) / 2, W, H, SWP_SHOWWINDOW);
    ForceFg(g_edDlg); SetFocus(g_edEdit);
    if (g_edLine) SendMessageW(g_edEdit, EM_SETSEL, 0, -1);
    InterlockedExchange(&g_modal, 1);
    MSG msg;
    while (!g_edDone && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_ESCAPE) { g_edDone = 2; break; }
            if (msg.wParam == VK_RETURN && (g_edLine || (GetKeyState(VK_CONTROL) & 0x8000))) { SendMessageW(g_edDlg, WM_COMMAND, IDOK, 0); continue; }
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
        wchar_t m[200]; wsprintfW(m, Tr(L_T_RUNFAIL), file); Toast(m);
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
        wchar_t m[200]; wsprintfW(m, Tr(L_T_PYFAIL), g_python); Toast(m);
    }
}
static void RunPy(const wchar_t *arg, const wchar_t *dir, BOOL hold = FALSE) {   // hold: окно консоли остаётся открытым (pause)
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
    if (hold && g_pyPause) BufAdd(&c, L"cmd.exe /d /c \"");
    BufAdd(&c, L"\""); BufAdd(&c, g_python); BufAdd(&c, L"\" \""); BufAdd(&c, script); BufAdd(&c, L"\"");
    if (*par) { BufAdd(&c, L" "); BufAdd(&c, par); }
    if (hold && g_pyPause) BufAdd(&c, L" & pause\"");
    RunProc(c.p, dir, env);
    Free(c.p); Free(env); Free(sel); Free(s);
}
static void RunPySelection(void) {
    wchar_t *sel = GrabSelection();
    if (!sel || !*sel) { Free(sel); Toast(Tr(L_T_NOSEL)); return; }
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
    wsprintfW(m, on ? Tr(L_T_TOPON) : Tr(L_T_TOPOFF), t);
    Toast(m);
}
static void HideToTray(void) {
    HWND h = TopWin(); if (!h || g_nhid >= 32) return;
    wchar_t t[64], m[160]; GetWindowTextW(h, t, 64);
    ShowWindow(h, SW_HIDE); g_hid[g_nhid++] = h;
    wsprintfW(m, Tr(L_T_TOTRAY), t); Toast(m);
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
    if (!en || !ru) { Toast(Tr(L_T_NOLAYOUT)); return; }
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
    if (!sel || !*sel) { Free(sel); Toast(Tr(L_T_NOLAYTXT)); return; }
    int up = 0, letters = 0;
    for (wchar_t *p = sel; *p; p++) { if (IsCharUpperW(*p)) { up++; letters++; } else if (IsCharLowerW(*p)) letters++; }
    if (letters >= 2 && up * 2 > letters) {   // больше половины букв заглавные (CapsLock): сначала только регистр, раскладку - вторым нажатием
        CharLowerBuffW(sel, lstrlenW(sel));
        int cl = CaretLen(sel);
        PasteText(sel);
        SendKeysN(VK_LEFT, cl, VK_SHIFT);       // снова выделить, чтобы повторное нажатие поменяло раскладку
        InterlockedExchange(&g_lastValid, 0);
        Free(sel);
        return;
    }
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
    if (!t || !*t) { Free(t); Toast(Tr(L_T_NOSEL)); return; }
    int len = lstrlenW(t);
    BufInsert(n, t);
    g_curBuf = n;
    wchar_t m[96]; wsprintfW(m, Tr(L_T_TOBUF), n, len);
    Toast(m);
}
static void PasteBuf(int n, int item) {
    if (item < 1 || item > g_bufN[n]) { wchar_t m[48]; wsprintfW(m, Tr(L_T_NOITEM), n, item); Toast(m); return; }
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
    case A_SCRIPTS: OpenPopup(PM_SCRIPTS, 0); break;
    case A_RUN: RunShell(b->arg, b->dir, FALSE); break;
    case A_ADMIN: RunShell(b->arg, b->dir, TRUE); break;
    case A_PY: RunPy(b->arg, b->dir); break;
    case A_PYRUN: RunPySelection(); break;
    case A_TOPMOST: ToggleTopmost(); break;
    case A_TOTRAY: HideToTray(); break;
    case A_LAYOUT: FixLayout(); break;
    case A_WEBCAM: CamOpen(); break;
    case A_STOPWATCH: TmToggle(1); break;
    case A_TIMER: TmToggle(2); break;
    case A_DRAW: DrawOpen(); break;
    case A_PICKER: PickerOpen(); break;
    case A_RECORD: RecOpen(); break;
    case A_PAUSE: InterlockedExchange(&g_paused, !g_paused); Toast(g_paused ? Tr(L_T_PAUSED) : Tr(L_T_WORKING)); break;
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
    wchar_t *text = Dup(mode == PM_SCRIPTS ? g_scrNames[r->ref] : r->text);
    PopupClose();
    if (mode == PM_LAUNCH) ExecSpec(text);
    else if (mode == PM_SCRIPTS) { wchar_t *q = (wchar_t *)Alloc((lstrlenW(text) + 3) * sizeof(wchar_t)); wsprintfW(q, L"\"%s\"", text); RunPy(q, NULL, TRUE); Free(q); }
    else PasteText(text);
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
    else { if (g_pmode == PM_LAUNCH) Toast(Tr(L_T_LISTINI)); return; }
    PopupReselect(g_sel);
}
static void PopupEdit(BOOL isNew) {
    if (g_pmode == PM_LAUNCH || g_pmode == PM_SCRIPTS || (!isNew && g_nvis <= 0)) return;
    int mode = g_pmode, buf = g_curBuf, ref = isNew ? -1 : g_rows[g_vis[g_sel]].ref;
    wchar_t *cur = Dup(isNew ? L"" : g_rows[g_vis[g_sel]].text);
    PopupClose();
    wchar_t title[64];
    if (mode == PM_BUF) wsprintfW(title, isNew ? Tr(L_ED_NEW) : Tr(L_ED_BUF), buf, ref + 1);
    else lstrcpyW(title, Tr(L_ED_HIST));
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
    if (i < 9) PopupSetMode(PM_BUF, i + 1); else PopupSetMode(i - 8, 0);
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
    case VK_TAB: { int m = (g_pmode + 1) % 4; if (m == PM_HIST && !g_histOn) m = PM_LAUNCH; PopupSetMode(m, 0); return; }
    case VK_DELETE: PopupDelete(); return;
    case VK_F2: PopupEdit(FALSE); return;
    case VK_INSERT: if (buf) PopupEdit(TRUE); return;
    case VK_BACK:
        if (g_nfilt) { g_filt[--g_nfilt] = 0; g_sel = g_top = 0; Refilter(); PopupRedraw(); }
        return;
    }
    if ((mods & DM_WIN) && dig >= 1) { PopupSetMode(PM_BUF, dig); return; }
    if ((mods & DM_CTRL) && dig >= 1 && g_pmode != PM_LAUNCH && g_pmode != PM_SCRIPTS && g_nvis > 0) {
        BufInsert(dig, Dup(g_rows[g_vis[g_sel]].text));
        wchar_t m[64]; wsprintfW(m, Tr(L_T_TOBUF2), dig); Toast(m);
        if (buf) { g_sel = 0; PopupReselect(0); }
        return;
    }
    if (g_pmode == PM_SCRIPTS && !g_nfilt && plain && (dig >= 0 || key == L'T')) {       // T - tmp.py, цифры - остальные по порядку
        wchar_t want = key == L'T' ? L'T' : (wchar_t)(L'0' + dig);
        for (int vi = 0; vi < g_nvis; vi++) if (g_rows[g_vis[vi]].label[0] == want) { PopupChoose(vi); return; }
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
    { UINT sc = k->scanCode & 0xFF, ic = sc;                     // код клавиши как в input-overlay (старый формат: скан-код, E0-клавиши в старших битах)
      if (k->flags & LLKHF_EXTENDED) ic = (sc == 0x48 || sc == 0x50 || sc == 0x4B || sc == 0x4D) ? (0xE000 | sc) : (0x0E00 | sc);
      g_ioKey[ic] = down ? 1 : 0; if (down) g_ioHit[ic] = GetTickCount(); }   // g_ioHit - момент нажатия: короткое нажатие держится на видео минимум 70 мс   // бит0 - зажата, бит1 - нажималась с прошлого кадра (короткие нажатия не теряются)
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
static int g_slowRx, g_slowRy;   // остаток движения мыши в долях при замедлении
static LRESULT CALLBACK LLMouse(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {                                           // состояние кнопок / колеса для оверлея ввода
        MSLLHOOKSTRUCT *mm = (MSLLHOOKSTRUCT *)lp;
        switch (wp) {
        case WM_LBUTTONDOWN: { g_ioMouse[1] = 1; g_ioMHit[1] = GetTickCount(); } break; case WM_LBUTTONUP: g_ioMouse[1] = 0; break;
        case WM_RBUTTONDOWN: { g_ioMouse[2] = 1; g_ioMHit[2] = GetTickCount(); } break; case WM_RBUTTONUP: g_ioMouse[2] = 0; break;
        case WM_MBUTTONDOWN: { g_ioMouse[3] = 1; g_ioMHit[3] = GetTickCount(); } break; case WM_MBUTTONUP: g_ioMouse[3] = 0; break;
        case WM_XBUTTONDOWN: { int bn = HIWORD(mm->mouseData) == 1 ? 4 : 5; g_ioMouse[bn] = 1; g_ioMHit[bn] = GetTickCount(); } break;
        case WM_XBUTTONUP: g_ioMouse[HIWORD(mm->mouseData) == 1 ? 4 : 5] = 0; break;
        case WM_MOUSEWHEEL: g_ioWheelD = (short)HIWORD(mm->mouseData); g_ioWheelT = GetTickCount(); break;
        }
    }
    if (code == HC_ACTION && wp == WM_MOUSEMOVE && g_pickActive && g_mouseSlow > 1) {
        MSLLHOOKSTRUCT *m = (MSLLHOOKSTRUCT *)lp;
        if (!(m->flags & LLMHF_INJECTED) && (g_modBits & 12)) {            // Alt зажат: движение делим на g_mouseSlow
            POINT cur; GetCursorPos(&cur);
            int tx = (m->pt.x - cur.x) + g_slowRx, ty = (m->pt.y - cur.y) + g_slowRy;
            int sx = tx / g_mouseSlow, sy = ty / g_mouseSlow;
            g_slowRx = tx - sx * g_mouseSlow; g_slowRy = ty - sy * g_mouseSlow;
            if (sx || sy) SetCursorPos(cur.x + sx, cur.y + sy);
            return 1;
        }
        g_slowRx = g_slowRy = 0;
    }
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
    lstrcpyW(g_nid.szTip, g_elev ? Tr(L_TRAY_TIPADM) : L"DKeys");
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
        if (RunHidden(cmd) != 0) Toast(Tr(L_T_AUTOFAIL));
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
    RecShowSync();
    Toast(Tr(L_T_RELOADED));
}
/* ---------- веб-камера (DirectShow): предпросмотр в окне ---------- */
struct CamRes { int w, h, idx; LONGLONG iv; };
#define CAMSTYLE (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX)
enum { CAM_DEV = 0x6000, CAM_RES = 0x6100, CAM_FULL = 0x6200, CAM_SET = 0x6300 };
static HWND g_hwndCam;
static HMENU g_camMenu, g_camPopDev, g_camPopRes, g_camPopSet;
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
            if (!g_camName[n][0]) lstrcpyW(g_camName[n], Tr(L_CAM_NAME));
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
    while (GetMenuItemCount(g_camPopSet) > 0) DeleteMenu(g_camPopSet, 0, MF_BYPOSITION);
    const wchar_t *sn[4] = {Tr(L_CAM_P0), Tr(L_CAM_P1), Tr(L_CAM_P2), Tr(L_CAM_P3)};
    for (int k = 0; k < 4; k++) {
        IUnknown *o = CamObj(k);
        AppendMenuW(g_camPopSet, o ? MF_STRING : MF_STRING | MF_GRAYED, CAM_SET + k, sn[k]);
        if (o) o->Release();
    }
    DrawMenuBar(g_hwndCam);
}
static void CamQuerySize(void) {          // реальный размер картинки из графа
    IBasicVideo *bv;
    if (g_gb && SUCCEEDED(g_gb->QueryInterface(IID_IBasicVideo, (void **)&bv))) {
        long a = 0, b = 0;
        if (SUCCEEDED(bv->GetVideoSize(&a, &b)) && a > 0 && b > 0) { g_vidW = a; g_vidH = b; }
        bv->Release();
    }
}
// объекты со страницами свойств: 0 - фильтр камеры, 1 - capture-пин, 2 - preview-пин, 3 - crossbar
static IUnknown *CamObj(int k) {
    IUnknown *o = NULL;
    if (!g_cgb || !g_capf) return NULL;
    if (k == 0) { o = g_capf; o->AddRef(); }
    else if (k == 3) g_cgb->FindInterface(&LOOK_UPSTREAM_ONLY, NULL, g_capf, IID_IUnknown, (void **)&o);
    else g_cgb->FindInterface(k == 1 ? &PIN_CATEGORY_CAPTURE : &PIN_CATEGORY_PREVIEW, &MEDIATYPE_Video, g_capf, IID_IUnknown, (void **)&o);
    ISpecifyPropertyPages *sp;
    if (!o) return NULL;
    CAUUID ca; ca.cElems = 0;
    if (SUCCEEDED(o->QueryInterface(IID_ISpecifyPropertyPages, (void **)&sp))) {     // серым, если страниц свойств нет
        if (SUCCEEDED(sp->GetPages(&ca))) CoTaskMemFree(ca.pElems);
        sp->Release();
    }
    if (!ca.cElems) { o->Release(); o = NULL; }
    return o;
}
static void CamProps(int k) {
    IUnknown *o = CamObj(k); if (!o) return;
    ISpecifyPropertyPages *sp; CAUUID ca;
    if (SUCCEEDED(o->QueryInterface(IID_ISpecifyPropertyPages, (void **)&sp))) {
        if (SUCCEEDED(sp->GetPages(&ca))) {
            if (k == 1 || k == 2) g_mc->Stop();      // формат потока меняется только на остановленном графе
            OleCreatePropertyFrame(g_hwndCam, 0, 0, Tr(L_CAM_PTITLE), 1, &o, ca.cElems, ca.pElems, 0, 0, NULL);
            CoTaskMemFree(ca.pElems);
            if (k == 1 || k == 2) { g_mc->Run(); CamQuerySize(); }
        }
        sp->Release();
    }
    o->Release();
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
        if (ri < 0) {                           // -1: разрешение из ini, иначе самое большое
            ri = 0;
            for (int k = 0; k < g_nres; k++) if (g_res[k].w == g_cfgCamW && g_res[k].h == g_cfgCamH) ri = k;
        }
        if (ri >= g_nres) ri = 0;
        if (g_nres) CamSetFmt(ri);
        g_curRes = ri;
        ok = SUCCEEDED(g_cgb->RenderStream(&PIN_CATEGORY_PREVIEW, &MEDIATYPE_Video, g_capf, NULL, NULL))
          && SUCCEEDED(g_gb->QueryInterface(IID_IVideoWindow, (void **)&g_vwin))
          && SUCCEEDED(g_gb->QueryInterface(IID_IMediaControl, (void **)&g_mc));
    }
    if (!ok) { CamStop(); g_nres = 0; Toast(Tr(L_CAM_BUSY)); return FALSE; }
    g_vidW = g_nres ? g_res[ri].w : 640; g_vidH = g_nres ? g_res[ri].h : 480;
    CamQuerySize();
    g_vwin->put_Owner((OAHWND)g_hwndCam);
    g_vwin->put_MessageDrain((OAHWND)g_hwndCam);
    g_vwin->put_WindowStyle(WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
    CamLayout();
    g_vwin->put_Visible(OATRUE);
    g_mc->Run();
    wchar_t t[120]; wsprintfW(t, Tr(L_CAM_TITLE), g_camName[dev]); SetWindowTextW(g_hwndCam, t);
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
    BOOL saved = center && g_cfgWin[2] > dw;     // при открытии - размер и место из ini
    if (saved) { cw = g_cfgWin[2] - dw; ch = MulDiv(cw, g_vidH, g_vidW); }
    if (cw > mcw) { cw = mcw; ch = MulDiv(cw, g_vidH, g_vidW); }
    if (ch > mch) { ch = mch; cw = MulDiv(ch, g_vidW, g_vidH); }
    int W = cw + dw, H = ch + dh;
    RECT sr = {g_cfgWin[0], g_cfgWin[1], g_cfgWin[0] + W, g_cfgWin[1] + H};
    if (saved && MonitorFromRect(&sr, MONITOR_DEFAULTTONULL))
        SetWindowPos(g_hwndCam, NULL, sr.left, sr.top, W, H, SWP_NOZORDER | SWP_NOACTIVATE);
    else if (center) {
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
static void CamSave(void) {                  // запомнить в ini камеру, разрешение, положение и размер окна
    wchar_t v[64]; RECT r;
    if (g_camFull) r = g_camSaved;
    else { WINDOWPLACEMENT wp; wp.length = sizeof wp; GetWindowPlacement(g_hwndCam, &wp); r = wp.rcNormalPosition; }
    g_cfgWin[0] = r.left; g_cfgWin[1] = r.top; g_cfgWin[2] = r.right - r.left; g_cfgWin[3] = r.bottom - r.top;
    wsprintfW(v, L"%d,%d,%d,%d", g_cfgWin[0], g_cfgWin[1], g_cfgWin[2], g_cfgWin[3]);
    IniSet(L"Camera", L"Window", v);
    if (g_curDev < g_ncam) { lstrcpynW(g_cfgCamDev, g_camName[g_curDev], 80); IniSet(L"Camera", L"Device", g_cfgCamDev); }
    if (g_nres > 0) {
        g_cfgCamW = g_res[g_curRes].w; g_cfgCamH = g_res[g_curRes].h;
        wsprintfW(v, L"%dx%d", g_cfgCamW, g_cfgCamH); IniSet(L"Camera", L"Resolution", v);
    }
}
static LRESULT CALLBACK CamProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id >= CAM_DEV && id < CAM_DEV + 16) { if (CamStart(id - CAM_DEV, 0)) CamSizeWindow(FALSE); CamMenuBuild(); }
        else if (id >= CAM_RES && id < CAM_RES + 64) { if (CamStart(g_curDev, id - CAM_RES)) CamSizeWindow(FALSE); CamMenuBuild(); }
        else if (id >= CAM_SET && id < CAM_SET + 4) { CamProps(id - CAM_SET); CamSizeWindow(FALSE); CamMenuBuild(); }
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
            mm->ptMinTrackSize.x = 480; mm->ptMinTrackSize.y = 160;
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
    case WM_CLOSE: CamSave(); DestroyWindow(h); return 0;
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
    if (!g_ncam) { Toast(Tr(L_CAM_NONE)); return; }
    RegClass(L"DKCam", CamProc, 0);
    g_camMenu = CreateMenu(); g_camPopDev = CreatePopupMenu(); g_camPopRes = CreatePopupMenu(); g_camPopSet = CreatePopupMenu();
    AppendMenuW(g_camMenu, MF_POPUP, (UINT_PTR)g_camPopDev, Tr(L_CAM_MCAM));
    AppendMenuW(g_camMenu, MF_POPUP, (UINT_PTR)g_camPopRes, Tr(L_CAM_MRES));
    AppendMenuW(g_camMenu, MF_POPUP, (UINT_PTR)g_camPopSet, Tr(L_CAM_MSET));
    AppendMenuW(g_camMenu, MF_STRING, CAM_FULL, Tr(L_CAM_FULL));
    g_hwndCam = CreateWindowExW(0, L"DKCam", Tr(L_CAM_WND), CAMSTYLE, CW_USEDEFAULT, CW_USEDEFAULT, 640, 480, NULL, g_camMenu, g_inst, NULL);
    if (!g_hwndCam) { DestroyMenu(g_camMenu); return; }
    int dev = 0;
    for (int i = 0; i < g_ncam; i++) if (!lstrcmpiW(g_camName[i], g_cfgCamDev)) dev = i;
    CamStart(dev, -1);
    CamMenuBuild();
    CamSizeWindow(TRUE);
    ShowWindow(g_hwndCam, SW_SHOW);
    SetForegroundWindow(g_hwndCam);
}

/* ---------- секундомер и таймер: только цифры на экране, окно без рамки ---------- */
#define TM_ID 0x7000
static HWND g_hwndTm;
static int g_tmMode;                                  // 0 - выключено, 1 - секундомер, 2 - таймер
static BOOL g_tmRun, g_shutPend;
static ULONGLONG g_tmStart, g_tmAcc, g_tmTarget;      // мс
static wchar_t g_tmLast[16];

static ULONGLONG TmElapsed(void) { return g_tmAcc + (g_tmRun ? GetTickCount64() - g_tmStart : 0); }
static void TmText(wchar_t *b) {
    ULONGLONG ms = TmElapsed(), t;
    if (g_tmMode == 2) t = ms >= g_tmTarget ? 0 : (g_tmTarget - ms + 999) / 1000; else t = ms / 1000;
    int h = (int)(t / 3600), m = (int)(t / 60 % 60), s = (int)(t % 60);
    if (h) wsprintfW(b, L"%d:%02d:%02d", h, m, s); else wsprintfW(b, L"%02d:%02d", m, s);
}
static void TmSave(void) {
    wchar_t v[32];
    wsprintfW(v, L"%02X%02X%02X", GetRValue(g_tmFg), GetGValue(g_tmFg), GetBValue(g_tmFg)); IniSet(L"Timer", L"FontColor", v);
    wsprintfW(v, L"%02X%02X%02X", GetRValue(g_tmBg), GetGValue(g_tmBg), GetBValue(g_tmBg)); IniSet(L"Timer", L"BgColor", v);
    wsprintfW(v, L"%d", g_tmAlpha); IniSet(L"Timer", L"BgAlpha", v);
    wsprintfW(v, L"%d", g_tmSize); IniSet(L"Timer", L"Size", v);
    wsprintfW(v, L"%d", g_tmEnd); IniSet(L"Timer", L"EndAction", v);
    wsprintfW(v, L"%d,%d", g_tmPos[0], g_tmPos[1]); IniSet(L"Timer", L"Pos", v);
    wchar_t f[128]; int o = 0; f[0] = 0;
    for (int i = 0; i < g_tmNFav; i++) o += wsprintfW(f + o, i ? L",%d" : L"%d", g_tmFav[i]);
    IniSet(L"Timer", L"Favorites", f);
}
static void TmFmt(int sec, wchar_t *b) {
    if (sec >= 3600) wsprintfW(b, L"%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60); else wsprintfW(b, L"%d:%02d", sec / 60, sec % 60);
}
static int ParseTime(const wchar_t *s) {       // секунды: "5" - минуты, "1:30" - м:с, "1:02:03" или "1 2 3" - ч:м:с
    int v[3], n = 0;
    while (*s && n < 3) {
        while (*s && (*s < L'0' || *s > L'9')) s++;
        if (!*s) break;
        v[n] = ToInt(s); if (v[n] > 99999) v[n] = 99999; n++;
        while (*s >= L'0' && *s <= L'9') s++;
    }
    int t = n == 1 ? v[0] * 60 : n == 2 ? v[0] * 60 + v[1] : n == 3 ? v[0] * 3600 + v[1] * 60 + v[2] : 0;
    return t > 359999 ? 359999 : t;
}
// рисуем цифры в слое с попиксельной прозрачностью: фон - цвет с заданной прозрачностью, цифры - непрозрачные
static void TmPaint(void) {
    if (!g_hwndTm) return;
    wchar_t txt[16]; TmText(txt); int n = lstrlenW(txt);
    lstrcpyW(g_tmLast, txt);
    HDC sdc = GetDC(NULL), dc = CreateCompatibleDC(sdc);
    HFONT f = CreateFontW(-g_tmSize, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    HGDIOBJ of = SelectObject(dc, f);
    SIZE sz; GetTextExtentPoint32W(dc, txt, n, &sz);
    int pad = g_tmSize / 4, w = sz.cx + 2 * pad, h = sz.cy + pad;
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    DWORD *px = NULL; HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    if (px) {
        HGDIOBJ ob = SelectObject(dc, bmp);
        memset(px, 0, (size_t)w * h * 4);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
        RECT r = {0, 0, w, h}; DrawTextW(dc, txt, n, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        GdiFlush();
        int bgA = g_tmAlpha * 255 / 100; if (bgA < 1) bgA = 1;        // alpha 0 пропускал бы клики сквозь окно
        int fr = GetRValue(g_tmFg), fg = GetGValue(g_tmFg), fb = GetBValue(g_tmFg), br = GetRValue(g_tmBg), bg = GetGValue(g_tmBg), bb = GetBValue(g_tmBg);
        for (int i = 0; i < w * h; i++) {
            int cov = (px[i] >> 8) & 255, A = bgA + (255 - bgA) * cov / 255;
            int R = (br * (255 - cov) + fr * cov) / 255, G = (bg * (255 - cov) + fg * cov) / 255, B = (bb * (255 - cov) + fb * cov) / 255;
            px[i] = ((DWORD)A << 24) | ((DWORD)(R * A / 255) << 16) | ((DWORD)(G * A / 255) << 8) | (DWORD)(B * A / 255);
        }
        POINT src = {0, 0}, pos; SIZE ws = {w, h}; RECT wr; GetWindowRect(g_hwndTm, &wr); pos.x = wr.left; pos.y = wr.top;
        BLENDFUNCTION bf; bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0; bf.SourceConstantAlpha = 255; bf.AlphaFormat = AC_SRC_ALPHA;
        UpdateLayeredWindow(g_hwndTm, sdc, &pos, &ws, dc, &src, 0, &bf, ULW_ALPHA);
        SelectObject(dc, ob);
    }
    SelectObject(dc, of); DeleteObject(f); if (bmp) DeleteObject(bmp);
    DeleteDC(dc); ReleaseDC(NULL, sdc);
}
static void TmFinish(void) {
    g_tmAcc = g_tmTarget; g_tmRun = FALSE;
    TmPaint();
    MessageBeep(MB_ICONEXCLAMATION);
    Toast(Tr(L_TM_DONE));
    if (g_tmEnd) {
        RunHidden(g_tmEnd == 1 ? L"shutdown.exe /s /t 30 /c \"DKeys: таймер\"" : L"shutdown.exe /r /t 30 /c \"DKeys: таймер\"");
        g_shutPend = TRUE;
    }
}
static LRESULT CALLBACK TmProc(HWND h, UINT m, WPARAM w, LPARAM l);
static void TmMenuPopup(void);
static void TmEnsure(int mode) {
    if (g_tmMode != mode) { g_tmMode = mode; g_tmRun = FALSE; g_tmAcc = 0; }
    if (g_hwndTm) return;
    RegClass(L"DKTimer", TmProc, 0);
    POINT p = {g_tmPos[0], g_tmPos[1]};
    if (!MonitorFromPoint(p, MONITOR_DEFAULTTONULL)) { p.x = 80; p.y = 80; }
    g_hwndTm = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"DKTimer", L"", WS_POPUP, p.x, p.y, 10, 10, NULL, NULL, g_inst, NULL);
    if (!g_hwndTm) { g_tmMode = 0; return; }
    SetTimer(g_hwndTm, 1, 250, NULL);
    TmPaint();
    ShowWindow(g_hwndTm, SW_SHOWNA);
}
static void TmClose(void) { g_tmMode = 0; g_tmRun = FALSE; if (g_hwndTm) DestroyWindow(g_hwndTm); }
static void TmToggle(int mode) {
    TmEnsure(mode); if (!g_hwndTm) return;
    if (mode == 2 && g_tmTarget == 0) { Toast(Tr(L_TM_NEEDTIME)); return; }
    if (g_tmRun) { g_tmAcc = TmElapsed(); g_tmRun = FALSE; }
    else { if (mode == 2 && TmElapsed() >= g_tmTarget) g_tmAcc = 0; g_tmStart = GetTickCount64(); g_tmRun = TRUE; }
    TmPaint();
}
static void TmStartFor(int sec) {
    TmEnsure(2); if (!g_hwndTm) return;
    g_tmTarget = (ULONGLONG)sec * 1000; g_tmAcc = 0; g_tmStart = GetTickCount64(); g_tmRun = TRUE;
    TmPaint();
}
static void TmReset(int mode) { TmEnsure(mode); g_tmAcc = 0; g_tmRun = FALSE; TmPaint(); }
static void TmPick(BOOL font) {
    static COLORREF cust[16];
    CHOOSECOLORW cc; memset(&cc, 0, sizeof cc);
    cc.lStructSize = sizeof cc; cc.hwndOwner = g_hwndTm; cc.rgbResult = font ? g_tmFg : g_tmBg;
    cc.lpCustColors = cust; cc.Flags = CC_RGBINIT | CC_FULLOPEN;
    if (ChooseColorW(&cc)) { if (font) g_tmFg = cc.rgbResult; else g_tmBg = cc.rgbResult; TmSave(); TmPaint(); }
}
static const int kTmSize[5] = {24, 36, 48, 72, 120};
static void TmCommand(int id) {
    int k = id - TM_ID;
    if (k == 1) TmToggle(1);
    else if (k == 2) TmReset(1);
    else if (k == 3) TmToggle(2);
    else if (k == 4) TmReset(2);
    else if (k == 5) {                                   // ввести время и сразу запустить
        wchar_t cur[16] = L"";
        if (g_tmTarget) TmFmt((int)(g_tmTarget / 1000), cur);
        g_edLine = TRUE; wchar_t *s = EditDialog(Tr(L_TM_INPUT), cur); g_edLine = FALSE;
        if (s) { int t = ParseTime(s); Free(s); if (t > 0) TmStartFor(t); }
    }
    else if (k == 6) {                                   // текущее время таймера - в избранное
        int t = (int)(g_tmTarget / 1000), i = 0;
        while (i < g_tmNFav && g_tmFav[i] != t) i++;
        if (t > 0 && i == g_tmNFav && g_tmNFav < 10) { g_tmFav[g_tmNFav++] = t; TmSave(); }
    }
    else if (k >= 50 && k < 50 + g_tmNFav) TmStartFor(g_tmFav[k - 50]);
    else if (k >= 60 && k < 60 + g_tmNFav) {
        memmove(g_tmFav + k - 60, g_tmFav + k - 59, (g_tmNFav - (k - 60) - 1) * sizeof(int)); g_tmNFav--; TmSave();
    }
    else if (k >= 12 && k <= 14) { g_tmEnd = k - 12; TmSave(); }
    else if (k == 15 || k == 16) { TmEnsure(g_tmMode ? g_tmMode : 1); if (g_hwndTm) TmPick(k == 15); }
    else if (k >= 20 && k <= 24) { TmEnsure(g_tmMode ? g_tmMode : 1); g_tmAlpha = (k - 20) * 25; TmSave(); TmPaint(); }
    else if (k >= 30 && k <= 34) { TmEnsure(g_tmMode ? g_tmMode : 1); g_tmSize = kTmSize[k - 30]; TmSave(); TmPaint(); }
    else if (k == 40) TmClose();
    else if (k == 41) { RunHidden(L"shutdown.exe /a"); g_shutPend = FALSE; Toast(Tr(L_TM_CANCELED)); }
}
static HMENU TmMenu(void) {
    HMENU m = CreatePopupMenu(), add = CreatePopupMenu(), en = CreatePopupMenu(), view = CreatePopupMenu(), al = CreatePopupMenu(), sz = CreatePopupMenu();
    AddHk(m, MF_STRING, TM_ID + 1, g_tmMode == 1 && g_tmRun ? Tr(L_TM_SWPAUSE) : Tr(L_TM_SWSTART), A_STOPWATCH);
    AppendMenuW(m, MF_STRING, TM_ID + 2, Tr(L_TM_SWRESET));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AddHk(m, MF_STRING, TM_ID + 3, g_tmMode == 2 && g_tmRun ? Tr(L_TM_TPAUSE) : Tr(L_TM_TSTART), A_TIMER);
    AppendMenuW(m, MF_STRING, TM_ID + 4, Tr(L_TM_TRESET));
    HMENU rm = CreatePopupMenu();
    AppendMenuW(add, MF_STRING, TM_ID + 5, Tr(L_TM_ENTER));
    if (g_tmNFav) AppendMenuW(add, MF_SEPARATOR, 0, NULL);
    for (int i = 0; i < g_tmNFav; i++) {
        wchar_t t[16]; TmFmt(g_tmFav[i], t);
        AppendMenuW(add, MF_STRING, TM_ID + 50 + i, t); AppendMenuW(rm, MF_STRING, TM_ID + 60 + i, t);
    }
    AppendMenuW(add, MF_SEPARATOR, 0, NULL);
    int cur = (int)(g_tmTarget / 1000), ci = 0; while (ci < g_tmNFav && g_tmFav[ci] != cur) ci++;
    AppendMenuW(add, MF_STRING | (cur > 0 && ci == g_tmNFav && g_tmNFav < 10 ? 0 : MF_GRAYED), TM_ID + 6, Tr(L_TM_FAVADD));
    if (g_tmNFav) AppendMenuW(add, MF_POPUP, (UINT_PTR)rm, Tr(L_TM_FAVDEL)); else DestroyMenu(rm);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)add, Tr(L_TM_TIME));
    const wchar_t *en_[3] = {Tr(L_TM_E0), Tr(L_TM_E1), Tr(L_TM_E2)};
    for (int i = 0; i < 3; i++) AppendMenuW(en, MF_STRING | (g_tmEnd == i ? MF_CHECKED : 0), TM_ID + 12 + i, en_[i]);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)en, Tr(L_TM_END));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_STRING, TM_ID + 15, Tr(L_TM_FG));
    AppendMenuW(view, MF_STRING, TM_ID + 16, Tr(L_TM_BG));
    for (int i = 0; i < 5; i++) { wchar_t t[16]; wsprintfW(t, L"%d%%", i * 25); AppendMenuW(al, MF_STRING | (g_tmAlpha == i * 25 ? MF_CHECKED : 0), TM_ID + 20 + i, t); }
    AppendMenuW(view, MF_POPUP, (UINT_PTR)al, Tr(L_TM_ALPHA));
    for (int i = 0; i < 5; i++) { wchar_t t[16]; wsprintfW(t, L"%d", kTmSize[i]); AppendMenuW(sz, MF_STRING | (g_tmSize == kTmSize[i] ? MF_CHECKED : 0), TM_ID + 30 + i, t); }
    AppendMenuW(view, MF_POPUP, (UINT_PTR)sz, Tr(L_TM_SIZE));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)view, Tr(L_TM_VIEW));
    if (g_shutPend) AppendMenuW(m, MF_STRING, TM_ID + 41, Tr(L_TM_CANCEL));
    if (g_tmMode) AppendMenuW(m, MF_STRING, TM_ID + 40, Tr(L_TM_CLOSE));
    return m;
}
static void TmMenuPopup(void) {
    POINT pt; GetCursorPos(&pt);
    HMENU m = TmMenu();
    SetForegroundWindow(g_hwndTm);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwndTm, NULL);
    PostMessageW(g_hwndTm, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (c) TmCommand(c);
}
static LRESULT CALLBACK TmProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCHITTEST: return HTCAPTION;                              // перетаскивание за любое место
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_NCLBUTTONDBLCLK: TmToggle(g_tmMode ? g_tmMode : 1); return 0;
    case WM_NCRBUTTONUP: TmMenuPopup(); return 0;
    case WM_EXITSIZEMOVE: { RECT r; GetWindowRect(h, &r); g_tmPos[0] = r.left; g_tmPos[1] = r.top; TmSave(); return 0; }
    case WM_TIMER: {
        if (g_tmMode == 2 && g_tmRun && TmElapsed() >= g_tmTarget) { TmFinish(); return 0; }
        wchar_t t[16]; TmText(t); if (lstrcmpW(t, g_tmLast)) TmPaint();
        return 0;
    }
    case WM_DESTROY: KillTimer(h, 1); g_hwndTm = NULL; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ---------- рисовалка на экране (GDI+ flat API) ----------
   Один слоистый (UpdateLayeredWindow) оверлей на весь монитор под курсором.
   Статика: фон - снимок экрана. Живой режим: фон прозрачный (alpha=1, чтобы ловить мышь), под ним настоящий экран.
   Фигуры хранятся списком (undo/redo = счётчик), стирание - отдельная фигура S_DEL со ссылкой. */
namespace DrawNs {
using namespace Gdiplus;
using namespace Gdiplus::DllExports;

enum { T_PEN, T_MARK, T_ARROW, T_LINE, T_RECT, T_ELL, T_TEXT, T_NUM, T_BLUR, T_ERASE, T_SPOT, T_CROP, T_LOUPE, NTOOL, S_DEL = 20, S_XF };
// кнопки панели: 0..NTOOL-1 инструменты, дальше действия, цвета, толщины, подсветка мыши
enum { B_UNDO = NTOOL, B_REDO, B_MODE, B_MON, B_COPY, B_SAVE, B_CLOSE, B_COL, B_WID = B_COL + 8, B_HALO = B_WID + 3, NIT };
struct Shp { int t; unsigned col; float w, x1, y1, x2, y2; PointF *pts; int n, cap; wchar_t *txt; int num, ref; BOOL hid; float rc, rs; struct Shp *alt; };   // rc,rs: поворот (cos,sin) прямоугольника/овала; alt: запасная копия для S_XF
struct Surf { HBITMAP hb; DWORD *bits; HDC dc; GpBitmap *bmp; GpGraphics *g; };
struct FR { float x, y, w, h; };
static const unsigned kCol[8] = {0xFFFF3B30, 0xFFFF9500, 0xFFFFD60A, 0xFF34C759, 0xFF0A84FF, 0xFFAF52DE, 0xFFFFFFFF, 0xFF000000};
static const float kWid[3] = {3, 6, 12};
static const GUID kPng = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};

static Surf bg, base, back;
static int W, H, OX, OY, tool, nextNum;
static float sc, tbX, tbY, tbW, tbH, tbDX, tbDY;
static HWND hw;
static ULONG_PTR gdiTok;
static Shp **shp; static int shN, shTot, shCap;
static Shp *act;
static BOOL typing, drag, tbDrag, halo;
static FR ir[NIT];

static float Sqrt(float v) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(v))); }
static float SegD(float px, float py, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy, t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    float qx = ax + t * dx - px, qy = ay + t * dy - py;
    return Sqrt(qx * qx + qy * qy);
}

/* --- поверхности --- */
static BOOL SurfNew(Surf *s) {
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    s->hb = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&s->bits, NULL, 0);
    if (!s->hb) return FALSE;
    s->dc = CreateCompatibleDC(NULL); SelectObject(s->dc, s->hb);
    GdipCreateBitmapFromScan0(W, H, W * 4, PixelFormat32bppPARGB, (BYTE *)s->bits, &s->bmp);
    if (!s->bmp) return FALSE;
    GdipGetImageGraphicsContext((GpImage *)s->bmp, &s->g);
    if (!s->g) return FALSE;
    GdipSetSmoothingMode(s->g, SmoothingModeAntiAlias);
    GdipSetTextRenderingHint(s->g, TextRenderingHintAntiAlias);
    return TRUE;
}
static void SurfFree(Surf *s) {
    if (s->g) GdipDeleteGraphics(s->g);
    if (s->bmp) GdipDisposeImage((GpImage *)s->bmp);
    if (s->dc) DeleteDC(s->dc);
    if (s->hb) DeleteObject(s->hb);
    memset(s, 0, sizeof *s);
}
static void Capture(void) {
    HDC sdc = GetDC(NULL);
    BitBlt(bg.dc, 0, 0, W, H, sdc, OX, OY, SRCCOPY | CAPTUREBLT);
    ReleaseDC(NULL, sdc);
    DWORD *p = bg.bits; for (int i = W * H; i > 0; i--, p++) *p |= 0xFF000000;
}

/* --- примитивы GDI+ --- */
static GpBrush *MkBr(unsigned col) { GpSolidFill *b = NULL; GdipCreateSolidFill(col, &b); return (GpBrush *)b; }
static GpPen *MkPen(unsigned col, float w, int cap) {
    GpPen *p = NULL; GdipCreatePen1(col, w, UnitPixel, &p);
    GdipSetPenStartCap(p, (LineCap)cap); GdipSetPenEndCap(p, (LineCap)cap); GdipSetPenLineJoin(p, LineJoinRound);
    return p;
}
static GpPath *RPath(float x, float y, float w, float h, float r) {
    GpPath *p = NULL; GdipCreatePath(FillModeAlternate, &p); float d = r * 2;
    GdipAddPathArc(p, x, y, d, d, 180, 90); GdipAddPathArc(p, x + w - d, y, d, d, 270, 90);
    GdipAddPathArc(p, x + w - d, y + h - d, d, d, 0, 90); GdipAddPathArc(p, x, y + h - d, d, d, 90, 90);
    GdipClosePathFigure(p); return p;
}
static void FillRR(GpGraphics *g, unsigned col, float x, float y, float w, float h, float r) {
    GpPath *p = RPath(x, y, w, h, r); GpBrush *b = MkBr(col);
    GdipFillPath(g, b, p); GdipDeleteBrush(b); GdipDeletePath(p);
}
// style: 0 обычный, 1 жирный. centered: по центру прямоугольника, иначе от левого верха (x, y)
static void Text(GpGraphics *g, const wchar_t *t, float x, float y, float w, float h, float px, int style, unsigned col, BOOL centered, const wchar_t *fam) {
    GpFontFamily *ff = NULL; GpFont *f = NULL; GpStringFormat *sf = NULL;
    GdipCreateFontFamilyFromName(fam, NULL, &ff);
    if (!ff) return;
    GdipCreateFont(ff, px, style, UnitPixel, &f);
    if (centered) {
        GdipCreateStringFormat(0, 0, &sf);
        GdipSetStringFormatAlign(sf, StringAlignmentCenter); GdipSetStringFormatLineAlign(sf, StringAlignmentCenter);
    }
    RectF r(x, y, w, h); GpBrush *b = MkBr(col);
    if (f) GdipDrawString(g, t, -1, f, &r, sf, b);
    GdipDeleteBrush(b); if (sf) GdipDeleteStringFormat(sf); if (f) GdipDeleteFont(f); GdipDeleteFontFamily(ff);
}
static void Arrow(GpGraphics *g, unsigned col, float w, float x1, float y1, float x2, float y2) {
    float dx = x2 - x1, dy = y2 - y1, len = Sqrt(dx * dx + dy * dy);
    if (len < 2) return;
    float ux = dx / len, uy = dy / len, hl = w * 4 + 10, hw2;
    if (hl > len * 0.9f) hl = len * 0.9f;
    hw2 = hl * 0.45f;
    float bx = x2 - ux * hl, by = y2 - uy * hl;
    GpPen *p = MkPen(col, w, 2); GdipDrawLine(g, p, x1, y1, x2 - ux * hl * 0.8f, y2 - uy * hl * 0.8f); GdipDeletePen(p);
    PointF tri[3]; tri[0].X = x2; tri[0].Y = y2;
    tri[1].X = bx - uy * hw2; tri[1].Y = by + ux * hw2; tri[2].X = bx + uy * hw2; tri[2].Y = by - ux * hw2;
    GpBrush *b = MkBr(col); GdipFillPolygon2(g, b, tri, 3); GdipDeleteBrush(b);
}
static float LensR(Shp *s) { float dx = s->x2 - s->x1, dy = s->y2 - s->y1; return Sqrt(dx * dx + dy * dy); }
static float TxtPx(float w) { return w < 4 ? 20.f : w < 8 ? 30.f : 46.f; }
static float NumR(float w) { return w < 4 ? 13.f : w < 8 ? 18.f : 26.f; }

// Поворот прямоугольника/овала вокруг центра: матрица мира GDI+ из (cos, sin), без тригонометрии CRT.
static BOOL RotOn(GpGraphics *g, Shp *s, float cx, float cy) {
    if ((s->rc == 0 && s->rs == 0) || (s->rc == 1 && s->rs == 0)) return FALSE;
    GpMatrix *m = NULL; GdipCreateMatrix2(s->rc, s->rs, -s->rs, s->rc, cx - s->rc * cx + s->rs * cy, cy - s->rs * cx - s->rc * cy, &m);
    if (!m) return FALSE;
    GdipSetWorldTransform(g, m); GdipDeleteMatrix(m); return TRUE;
}
static void DrawShape(GpGraphics *g, Shp *s) {
    float x = s->x1 < s->x2 ? s->x1 : s->x2, y = s->y1 < s->y2 ? s->y1 : s->y2;
    float w = s->x1 < s->x2 ? s->x2 - s->x1 : s->x1 - s->x2, h = s->y1 < s->y2 ? s->y2 - s->y1 : s->y1 - s->y2;
    switch (s->t) {
    case T_PEN: case T_MARK: {
        float lw = s->t == T_MARK ? s->w * 3.5f : s->w;
        if (s->n == 1) { GpBrush *b = MkBr(s->col); GdipFillEllipse(g, b, s->pts[0].X - lw / 2, s->pts[0].Y - lw / 2, lw, lw); GdipDeleteBrush(b); }
        else if (s->n > 1) { GpPen *p = MkPen(s->col, lw, s->t == T_MARK ? 0 : 2); GdipDrawCurve2(g, p, s->pts, s->n, 0.5f); GdipDeletePen(p); }
        break;
    }
    case T_ARROW: Arrow(g, s->col, s->w, s->x1, s->y1, s->x2, s->y2); break;
    case T_LINE: { GpPen *p = MkPen(s->col, s->w, 2); GdipDrawLine(g, p, s->x1, s->y1, s->x2, s->y2); GdipDeletePen(p); break; }
    case T_RECT: { BOOL r = RotOn(g, s, x + w / 2, y + h / 2); GpPen *p = MkPen(s->col, s->w, 2); GdipDrawRectangle(g, p, x, y, w, h); GdipDeletePen(p); if (r) GdipResetWorldTransform(g); break; }
    case T_ELL: { BOOL r = RotOn(g, s, x + w / 2, y + h / 2); GpPen *p = MkPen(s->col, s->w, 2); GdipDrawEllipse(g, p, x, y, w, h); GdipDeletePen(p); if (r) GdipResetWorldTransform(g); break; }
    case T_TEXT: {
        wchar_t t[520]; lstrcpynW(t, s->txt, 512);
        if (s == act && typing) Cat(t, 520, L"|");
        float px = TxtPx(s->w);
        Text(g, t, s->x1 + 1.5f, s->y1 + 1.5f, 4000, 4000, px, 1, (s->col & 0xFFFFFF) ? 0x99000000 : 0x99FFFFFF, FALSE, L"Segoe UI");
        Text(g, t, s->x1, s->y1, 4000, 4000, px, 1, s->col, FALSE, L"Segoe UI");
        break;
    }
    case T_NUM: {
        float r = NumR(s->w); wchar_t t[12]; wsprintfW(t, L"%d", s->num);
        GpBrush *b = MkBr(s->col); GdipFillEllipse(g, b, s->x1 - r, s->y1 - r, r * 2, r * 2); GdipDeleteBrush(b);
        GpPen *p = MkPen(0xCCFFFFFF, 1.5f, 2); GdipDrawEllipse(g, p, s->x1 - r, s->y1 - r, r * 2, r * 2); GdipDeletePen(p);
        unsigned c = s->col & 0xFFFFFF; BOOL light = c == 0xFFD60A || c == 0xFFFFFF || c == 0x34C759 || c == 0xFF9500;
        Text(g, t, s->x1 - r, s->y1 - r, r * 2, r * 2, r * 1.15f, 1, light ? 0xFF000000 : 0xFFFFFFFF, TRUE, L"Segoe UI");
        break;
    }
    case T_LOUPE: {                                           // только кольцо: содержимое линзы строит Lens()
        float r = LensR(s); if (r < 2) break;
        GpPen *p0 = MkPen(0x66000000, 4, 2); GdipDrawEllipse(g, p0, s->x1 - r, s->y1 - r, r * 2, r * 2); GdipDeletePen(p0);
        GpPen *p1 = MkPen(0xFFFFFFFF, 2, 2); GdipDrawEllipse(g, p1, s->x1 - r, s->y1 - r, r * 2, r * 2); GdipDeletePen(p1);
        break;
    }
    }
}
static int LensZ(Shp *s) { return s->w < 4 ? 2 : s->w < 8 ? 3 : 4; }
// Линза: содержимое круга (центр x1,y1; радиус до x2,y2) увеличено в LensZ раз из снимка bg.
static void Lens(Surf *d, Shp *s) {
    int r = (int)LensR(s), z = LensZ(s), cx = (int)s->x1, cy = (int)s->y1;
    if (r < 2) return;
    for (int y = cy - r; y <= cy + r; y++) {
        if (y < 0 || y >= H) continue;
        for (int x = cx - r; x <= cx + r; x++) {
            if (x < 0 || x >= W) continue;
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy > r * r) continue;
            int sx = cx + dx / z, sy = cy + dy / z;
            d->bits[(size_t)y * W + x] = bg.bits[(size_t)sy * W + sx];
        }
    }
}
// Затемнение всего, кроме видимых T_SPOT. Снимок (alpha=255) умножается на 0.4, прозрачный живой фон получает чёрный alpha 0x99.
static void Dim(Surf *d) {
    int hx0[16], hx1[16], hy0[16], hy1[16], nh = 0;
    for (int i = 0; i < shN && nh < 16; i++) {
        Shp *s = shp[i]; if (s->t != T_SPOT || s->hid) continue;
        int x0 = (int)(s->x1 < s->x2 ? s->x1 : s->x2), x1 = (int)(s->x1 < s->x2 ? s->x2 : s->x1);
        int y0 = (int)(s->y1 < s->y2 ? s->y1 : s->y2), y1 = (int)(s->y1 < s->y2 ? s->y2 : s->y1);
        hx0[nh] = x0; hx1[nh] = x1; hy0[nh] = y0; hy1[nh] = y1; nh++;
    }
    if (!nh) return;
    for (int y = 0; y < H; y++) {
        DWORD *row = d->bits + (size_t)y * W; int a[16], b[16], k = 0;
        for (int j = 0; j < nh; j++) if (y >= hy0[j] && y < hy1[j]) { a[k] = hx0[j]; b[k] = hx1[j]; k++; }
        for (int x = 0; x < W; x++) {
            BOOL in = FALSE; for (int j = 0; j < k; j++) if (x >= a[j] && x < b[j]) { in = TRUE; break; }
            if (in) continue;
            DWORD c = row[x];
            if ((c >> 24) == 0xFF) row[x] = 0xFF000000 | ((((c >> 16) & 255) * 102 >> 8) << 16) | ((((c >> 8) & 255) * 102 >> 8) << 8) | ((c & 255) * 102 >> 8);
            else row[x] = 0x99000000;
        }
    }
}
static void Pixelate(Surf *d, Shp *s) {
    int x0 = (int)(s->x1 < s->x2 ? s->x1 : s->x2), x1 = (int)(s->x1 < s->x2 ? s->x2 : s->x1);
    int y0 = (int)(s->y1 < s->y2 ? s->y1 : s->y2), y1 = (int)(s->y1 < s->y2 ? s->y2 : s->y1);
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > W) x1 = W; if (y1 > H) y1 = H;
    int B = s->w < 4 ? 8 : s->w < 8 ? 14 : 22;
    for (int by = y0; by < y1; by += B) for (int bx = x0; bx < x1; bx += B) {
        int ex = bx + B < x1 ? bx + B : x1, ey = by + B < y1 ? by + B : y1, n = (ex - bx) * (ey - by);
        unsigned r = 0, gg = 0, b = 0;
        for (int yy = by; yy < ey; yy++) for (int xx = bx; xx < ex; xx++) {
            DWORD c = bg.bits[(size_t)yy * W + xx]; r += (c >> 16) & 255; gg += (c >> 8) & 255; b += c & 255;
        }
        DWORD c = 0xFF000000 | ((r / n) << 16) | ((gg / n) << 8) | (b / n);
        for (int yy = by; yy < ey; yy++) for (int xx = bx; xx < ex; xx++) d->bits[(size_t)yy * W + xx] = c;
    }
}
// Собрать слой "фон + готовые фигуры" (без панели и без текущей фигуры).
static void Rebuild(BOOL withBg) {
    for (int i = 0; i < shTot; i++) shp[i]->hid = FALSE;
    nextNum = 1;
    for (int i = 0; i < shN; i++) if (shp[i]->t == S_DEL) shp[shp[i]->ref]->hid = TRUE;
    for (int i = 0; i < shN; i++) if (shp[i]->t == T_NUM && !shp[i]->hid && shp[i]->num >= nextNum) nextNum = shp[i]->num + 1;
    if (!g_dLive || withBg) memcpy(base.bits, bg.bits, (size_t)W * H * 4);
    else { DWORD *p = base.bits; for (int i = W * H; i > 0; i--) *p++ = 0x01000000; }
    for (int i = 0; i < shN; i++) if (shp[i]->t == T_BLUR && !shp[i]->hid) Pixelate(&base, shp[i]);
    for (int i = 0; i < shN; i++) if (shp[i]->t == T_LOUPE && !shp[i]->hid) Lens(&base, shp[i]);
    Dim(&base);
    for (int i = 0; i < shN; i++) if (!shp[i]->hid && shp[i]->t != T_BLUR && shp[i]->t != S_DEL && shp[i]->t != T_SPOT && shp[i]->t != T_CROP && shp[i]->t != S_XF) DrawShape(base.g, shp[i]);
}

/* --- панель --- */
static void SetR(int i, float x, float y, float w, float h) { ir[i].x = x; ir[i].y = y; ir[i].w = w; ir[i].h = h; }
static void TbLayout(void) {
    float B = 36 * sc, gp = 4 * sc, pd = 8 * sc, sp = 10 * sc, x = tbX + pd, y = tbY + pd;
    for (int i = 0; i < B_COL; i++) { if (i == B_UNDO || i == B_MODE) x += sp; SetR(i, x, y, B, B); x += B + gp; }
    tbW = x - gp + pd - tbX;
    float cb = B * 0.8f; x = tbX + pd; y += B + gp;
    for (int i = 0; i < 8; i++) { SetR(B_COL + i, x, y, cb, cb); x += cb + gp; }
    x += sp;
    for (int i = 0; i < 3; i++) { SetR(B_WID + i, x, y, cb * 1.5f, cb); x += cb * 1.5f + gp; }
    x += sp; SetR(B_HALO, x, y, cb * 1.5f, cb);
    tbH = pd * 2 + B + gp + cb;
}
static int HitTb(int x, int y) {
    if (x < tbX || y < tbY || x >= tbX + tbW || y >= tbY + tbH) return -1;
    for (int i = 0; i < NIT; i++) if (x >= ir[i].x && x < ir[i].x + ir[i].w && y >= ir[i].y && y < ir[i].y + ir[i].h) return i;
    return 99;
}
#define PX(a) (r->x + r->w * (a))
#define PY(b) (r->y + r->h * (b))
static void Icon(GpGraphics *g, int i, FR *r, unsigned ic) {
    GpPen *p = MkPen(ic, 2.2f * sc, 2); GpBrush *b = MkBr(ic);
    switch (i) {
    case 0: { PointF q[4]; q[0].X = PX(.22f); q[0].Y = PY(.7f); q[1].X = PX(.4f); q[1].Y = PY(.32f); q[2].X = PX(.58f); q[2].Y = PY(.68f); q[3].X = PX(.78f); q[3].Y = PY(.3f); GdipDrawCurve2(g, p, q, 4, 0.5f); break; }
    case 1: { GpPen *m = MkPen((ic & 0xFF000000) ? 0xB0FFD60A : 0x40FFD60A, r->w * 0.2f, 0); GdipDrawLine(g, m, PX(.24f), PY(.68f), PX(.76f), PY(.34f)); GdipDeletePen(m); break; }
    case 2: Arrow(g, ic, 2.2f * sc, PX(.24f), PY(.76f), PX(.76f), PY(.24f)); break;
    case 3: GdipDrawLine(g, p, PX(.25f), PY(.75f), PX(.75f), PY(.25f)); break;
    case 4: GdipDrawRectangle(g, p, PX(.24f), PY(.3f), r->w * .52f, r->h * .4f); break;
    case 5: GdipDrawEllipse(g, p, PX(.22f), PY(.26f), r->w * .56f, r->h * .48f); break;
    case 6: Text(g, L"T", r->x, r->y, r->w, r->h, r->h * .56f, 1, ic, TRUE, L"Segoe UI"); break;
    case 7: GdipFillEllipse(g, b, PX(.2f), PY(.2f), r->w * .6f, r->h * .6f); Text(g, L"1", r->x, r->y, r->w, r->h, r->h * .38f, 1, 0xFF202124, TRUE, L"Segoe UI"); break;
    case 8: for (int a = 0; a < 3; a++) for (int c = 0; c < 3; c++) {
                GpBrush *q = MkBr(((a + c) & 1) ? (ic & 0x66FFFFFF) : ic);
                GdipFillRectangle(g, q, PX(.2f + c * .2f), PY(.2f + a * .2f), r->w * .18f, r->h * .18f); GdipDeleteBrush(q);
            } break;
    case 9: { PointF q[4]; q[0].X = PX(.26f); q[0].Y = PY(.62f); q[1].X = PX(.5f); q[1].Y = PY(.28f); q[2].X = PX(.78f); q[2].Y = PY(.46f); q[3].X = PX(.54f); q[3].Y = PY(.8f); GdipDrawPolygon(g, p, q, 4); GdipDrawLine(g, p, PX(.38f), PY(.45f), PX(.66f), PY(.63f)); break; }
    case T_SPOT: { GpBrush *q = MkBr(ic & 0x66FFFFFF); GdipFillRectangle(g, q, PX(.2f), PY(.22f), r->w * .6f, r->h * .56f); GdipDeleteBrush(q);
                   GdipDrawRectangle(g, p, PX(.34f), PY(.36f), r->w * .32f, r->h * .28f); break; }
    case T_CROP: { PointF a[3], c[3]; a[0].X = PX(.3f); a[0].Y = PY(.18f); a[1].X = PX(.3f); a[1].Y = PY(.7f); a[2].X = PX(.82f); a[2].Y = PY(.7f);
                   c[0].X = PX(.18f); c[0].Y = PY(.3f); c[1].X = PX(.7f); c[1].Y = PY(.3f); c[2].X = PX(.7f); c[2].Y = PY(.82f);
                   GdipDrawLines(g, p, a, 3); GdipDrawLines(g, p, c, 3); break; }
    case T_LOUPE: GdipDrawEllipse(g, p, PX(.18f), PY(.18f), r->w * .44f, r->h * .44f); GdipDrawLine(g, p, PX(.58f), PY(.58f), PX(.8f), PY(.8f)); break;
    case B_MON: GdipDrawRectangle(g, p, PX(.14f), PY(.26f), r->w * .44f, r->h * .34f); GdipDrawRectangle(g, p, PX(.46f), PY(.42f), r->w * .4f, r->h * .34f); break;
    case B_UNDO: Text(g, L"\x21B6", r->x, r->y, r->w, r->h, r->h * .62f, 0, ic, TRUE, L"Segoe UI Symbol"); break;
    case B_REDO: Text(g, L"\x21B7", r->x, r->y, r->w, r->h, r->h * .62f, 0, ic, TRUE, L"Segoe UI Symbol"); break;
    case B_MODE: Text(g, g_dLive ? L"LIVE" : Tr(L_D_STATIC), r->x, r->y, r->w, r->h, r->h * .3f, 1, ic, TRUE, L"Segoe UI"); break;
    case B_COPY: GdipDrawRectangle(g, p, PX(.26f), PY(.2f), r->w * .34f, r->h * .4f); GdipDrawRectangle(g, p, PX(.4f), PY(.4f), r->w * .34f, r->h * .4f); break;
    case B_SAVE: GdipDrawLine(g, p, PX(.5f), PY(.2f), PX(.5f), PY(.62f)); GdipDrawLine(g, p, PX(.33f), PY(.46f), PX(.5f), PY(.64f)); GdipDrawLine(g, p, PX(.67f), PY(.46f), PX(.5f), PY(.64f));
            GdipDrawLine(g, p, PX(.27f), PY(.78f), PX(.73f), PY(.78f)); break;
    case B_CLOSE: GdipDrawLine(g, p, PX(.3f), PY(.3f), PX(.7f), PY(.7f)); GdipDrawLine(g, p, PX(.7f), PY(.3f), PX(.3f), PY(.7f)); break;
    case B_HALO: { float d = r->h * .62f, cx = r->x + r->w / 2 - d / 2, cy = r->y + r->h / 2 - d / 2;
                   GpBrush *q = MkBr(0x99FFD60A); GdipFillEllipse(g, q, cx, cy, d, d); GdipDeleteBrush(q); GdipDrawEllipse(g, p, cx, cy, d, d); break; }
    }
    GdipDeletePen(p); GdipDeleteBrush(b);
}
static void DrawToolbar(GpGraphics *g) {
    GpPath *pa = RPath(tbX, tbY, tbW, tbH, 14 * sc); GpBrush *b = MkBr(0xF2202124);
    GdipFillPath(g, b, pa); GdipDeleteBrush(b);
    GpPen *pn = MkPen(0x40FFFFFF, 1, 0); GdipDrawPath(g, pn, pa); GdipDeletePen(pn); GdipDeletePath(pa);
    for (int i = 0; i < NIT; i++) {
        FR *r = &ir[i];
        BOOL sel = (i < NTOOL && i == tool) || (i >= B_COL && i < B_WID && i - B_COL == g_dCol) || (i >= B_WID && i < B_HALO && i - B_WID == g_dWid) || (i == B_HALO && halo);
        if (i < B_COL || i == B_HALO) {
            if (sel) FillRR(g, 0xFF0A84FF, r->x, r->y, r->w, r->h, 8 * sc);
            BOOL off = (i == B_UNDO && !shN && !typing) || (i == B_REDO && shN >= shTot);
            Icon(g, i, r, off ? 0x55FFFFFF : 0xFFFFFFFF);
        } else if (i < B_WID) {
            float d = r->w * 0.74f, cx = r->x + r->w / 2 - d / 2, cy = r->y + r->h / 2 - d / 2;
            GpBrush *c = MkBr(kCol[i - B_COL]); GdipFillEllipse(g, c, cx, cy, d, d); GdipDeleteBrush(c);
            GpPen *q = MkPen(sel ? 0xFFFFFFFF : 0x66FFFFFF, sel ? 2.5f * sc : 1, 2); GdipDrawEllipse(g, q, sel ? cx - 3 * sc : cx, sel ? cy - 3 * sc : cy, sel ? d + 6 * sc : d, sel ? d + 6 * sc : d); GdipDeletePen(q);
        } else {
            if (sel) FillRR(g, 0xFF0A84FF, r->x, r->y, r->w, r->h, 8 * sc);
            GpPen *q = MkPen(0xFFFFFFFF, kWid[i - B_WID] * 0.7f + 1, 2); GdipDrawLine(g, q, r->x + r->w * .25f, r->y + r->h / 2, r->x + r->w * .75f, r->y + r->h / 2); GdipDeletePen(q);
        }
    }
}

/* --- подсказка над кнопкой панели --- */
static int hov = -1; static DWORD hovTick; static BOOL tipOn;
static const wchar_t *const kTipKey[NIT] = {L"P", L"M", L"A", L"L", L"R", L"O", L"T", L"N", L"B", L"E", L"S", L"C", L"Z", L"Ctrl+Z", L"Ctrl+Y", L"Tab", L"D / Shift+D",
    L"Enter / Ctrl+C", L"Ctrl+S", L"Esc", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, L"1", L"2", L"3", L"H"};
static float MeasureW(GpGraphics *g, const wchar_t *t, float px) {
    GpFontFamily *ff = NULL; GpFont *f = NULL; float w = 0;
    GdipCreateFontFamilyFromName(L"Segoe UI", NULL, &ff);
    if (!ff) return 0;
    GdipCreateFont(ff, px, 0, UnitPixel, &f);
    if (f) { RectF lay(0, 0, 4000, 200), bb; GdipMeasureString(g, t, -1, f, &lay, NULL, &bb, NULL, NULL); w = bb.Width; GdipDeleteFont(f); }
    GdipDeleteFontFamily(ff); return w;
}
static BOOL TipVisible(void) { return hov >= 0 && !drag && !tbDrag && GetTickCount() - hovTick >= 350; }
static void DrawTip(GpGraphics *g) {
    int i = hov; wchar_t t[220];
    lstrcpynW(t, Tr(i < B_COL ? L_D_TT0 + i : i < B_WID ? L_D_TTCOL : i < B_HALO ? L_D_TTWID : L_D_TTHALO), 170);
    if (kTipKey[i]) { Cat(t, 220, L"   ["); Cat(t, 220, kTipKey[i]); Cat(t, 220, L"]"); }
    float px = 14 * sc, tw = MeasureW(g, t, px), bw = tw + 18 * sc, bh = px * 1.9f;
    FR *r = &ir[i];
    float x = r->x + r->w / 2 - bw / 2, y = r->y + r->h + 8 * sc;
    if (x > W - bw - 4) x = W - bw - 4; if (x < 4) x = 4;
    if (y + bh > H) y = r->y - bh - 8 * sc;
    FillRR(g, 0xF2101114, x, y, bw, bh, 7 * sc);
    Text(g, t, x, y, bw, bh, px, 0, 0xFFFFFFFF, TRUE, L"Segoe UI");
}

// Область сохранения: текущая рисуемая T_CROP или последняя видимая (границы по экрану).
static BOOL CropRect(int *x, int *y, int *w, int *h) {
    Shp *s = NULL;
    if (act && act->t == T_CROP) s = act;
    else for (int i = shN - 1; i >= 0; i--) if (shp[i]->t == T_CROP && !shp[i]->hid) { s = shp[i]; break; }
    if (!s) return FALSE;
    int x0 = (int)(s->x1 < s->x2 ? s->x1 : s->x2), x1 = (int)(s->x1 < s->x2 ? s->x2 : s->x1);
    int y0 = (int)(s->y1 < s->y2 ? s->y1 : s->y2), y1 = (int)(s->y1 < s->y2 ? s->y2 : s->y1);
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > W) x1 = W; if (y1 > H) y1 = H;
    if (x1 - x0 < 4 || y1 - y0 < 4) return FALSE;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0; return TRUE;
}

/* --- выделение с Ctrl: рамка, 8 маркеров размера, маркер поворота --- */
enum { XM_NONE, XM_MOVE, XM_SCALE, XM_ROT };
struct Xf { float cx, cy, hx, hy, rc, rs; BOOL scl, rot; };           // рамка: центр, полуразмеры, поворот
static int xsel = -1, xmode, xh; static BOOL xdrag, xchg; static Shp *xo; static float xmx, xmy; static Xf xf0;
static const int kGX[8] = {-1, 0, 1, 1, 1, 0, -1, -1}, kGY[8] = {-1, -1, -1, 0, 1, 1, 1, 0};
static void FrameOf(Shp *s, Xf *f) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, pad = 0;
    f->rc = 1; f->rs = 0; f->scl = TRUE; f->rot = FALSE;
    switch (s->t) {
    case T_PEN: case T_MARK:
        for (int i = 0; i < s->n; i++) {
            PointF *p = &s->pts[i];
            if (p->X < x0) x0 = p->X; if (p->X > x1) x1 = p->X; if (p->Y < y0) y0 = p->Y; if (p->Y > y1) y1 = p->Y;
        }
        pad = (s->t == T_MARK ? s->w * 1.75f : s->w / 2) + 5 * sc; f->rot = TRUE; break;
    case T_ARROW: case T_LINE:
        x0 = s->x1 < s->x2 ? s->x1 : s->x2; x1 = s->x1 < s->x2 ? s->x2 : s->x1; y0 = s->y1 < s->y2 ? s->y1 : s->y2; y1 = s->y1 < s->y2 ? s->y2 : s->y1;
        pad = s->w * 2 + 5 * sc; f->rot = TRUE; break;
    case T_RECT: case T_ELL: case T_BLUR: case T_SPOT: case T_CROP:
        x0 = s->x1 < s->x2 ? s->x1 : s->x2; x1 = s->x1 < s->x2 ? s->x2 : s->x1; y0 = s->y1 < s->y2 ? s->y1 : s->y2; y1 = s->y1 < s->y2 ? s->y2 : s->y1;
        if (s->t == T_RECT || s->t == T_ELL) { f->rot = TRUE; if (s->rc || s->rs) { f->rc = s->rc; f->rs = s->rs; } }
        break;
    case T_LOUPE: { float r = LensR(s); x0 = s->x1 - r; x1 = s->x1 + r; y0 = s->y1 - r; y1 = s->y1 + r; f->scl = FALSE; break; }
    case T_NUM: { float r = NumR(s->w) + 2; x0 = s->x1 - r; x1 = s->x1 + r; y0 = s->y1 - r; y1 = s->y1 + r; f->scl = FALSE; break; }
    case T_TEXT: {
        float sz = TxtPx(s->w); int lines = 1, cur = 0, mx = 0;
        for (const wchar_t *c = s->txt; *c; c++) { if (*c == L'\n') { lines++; cur = 0; } else if (++cur > mx) mx = cur; }
        x0 = s->x1 - 4; x1 = s->x1 + mx * sz * 0.6f + 4; y0 = s->y1 - 4; y1 = s->y1 + lines * sz * 1.3f + 4; f->scl = FALSE; break;
    }
    }
    x0 -= pad; x1 += pad; y0 -= pad; y1 += pad;
    f->cx = (x0 + x1) / 2; f->cy = (y0 + y1) / 2; f->hx = (x1 - x0) / 2; f->hy = (y1 - y0) / 2;
}
static void XW(const Xf *f, float ox, float oy, float *x, float *y) { *x = f->cx + f->rc * ox - f->rs * oy; *y = f->cy + f->rs * ox + f->rc * oy; }
static void DrawSel(GpGraphics *g, Shp *s) {
    Xf f; FrameOf(s, &f);
    static const float cx4[4] = {-1, 1, 1, -1}, cy4[4] = {-1, -1, 1, 1};
    PointF q[4]; for (int k = 0; k < 4; k++) XW(&f, cx4[k] * f.hx, cy4[k] * f.hy, &q[k].X, &q[k].Y);
    GpPen *pw = MkPen(0xCCFFFFFF, 3.5f * sc, 0), *pb = MkPen(0xFF0A84FF, 1.3f * sc, 0);
    GdipDrawPolygon(g, pw, q, 4); GdipDrawPolygon(g, pb, q, 4);
    GpBrush *bw = MkBr(0xFFFFFFFF), *bb = MkBr(0xFF0A84FF); float hs = 9 * sc;
    if (f.rot) {
        float tx, ty, rx, ry; XW(&f, 0, -f.hy, &tx, &ty); XW(&f, 0, -f.hy - 28 * sc, &rx, &ry);
        GdipDrawLine(g, pw, tx, ty, rx, ry); GdipDrawLine(g, pb, tx, ty, rx, ry);
        GdipFillEllipse(g, bb, rx - hs * .6f, ry - hs * .6f, hs * 1.2f, hs * 1.2f);
        GpPen *p2 = MkPen(0xFFFFFFFF, 1.6f * sc, 0); GdipDrawEllipse(g, p2, rx - hs * .6f, ry - hs * .6f, hs * 1.2f, hs * 1.2f); GdipDeletePen(p2);
    }
    if (f.scl) for (int i = 0; i < 8; i++) {
        float px, py; XW(&f, kGX[i] * f.hx, kGY[i] * f.hy, &px, &py);
        GdipFillRectangle(g, bw, px - hs / 2, py - hs / 2, hs, hs); GdipDrawRectangle(g, pb, px - hs / 2, py - hs / 2, hs, hs);
    }
    GdipDeleteBrush(bw); GdipDeleteBrush(bb); GdipDeletePen(pw); GdipDeletePen(pb);
}

/* --- вывод на экран --- */
static void Present(void) {
    memcpy(back.bits, base.bits, (size_t)W * H * 4);
    if (act) {
        if (act->t == T_BLUR) Pixelate(&back, act);
        else if (act->t == T_LOUPE) Lens(&back, act);
        if (act->t == T_BLUR || act->t == T_SPOT || act->t == T_CROP) {
            GpPen *p = MkPen(0xCCFFFFFF, 1, 0); float x = act->x1 < act->x2 ? act->x1 : act->x2, y = act->y1 < act->y2 ? act->y1 : act->y2;
            GdipDrawRectangle(back.g, p, x, y, act->x1 < act->x2 ? act->x2 - act->x1 : act->x1 - act->x2, act->y1 < act->y2 ? act->y2 - act->y1 : act->y1 - act->y2); GdipDeletePen(p);
        } else DrawShape(back.g, act);
    }
    int kx, ky, kw, kh;
    if (CropRect(&kx, &ky, &kw, &kh)) {                          // область сохранения: снаружи затемнено, рамка
        GpBrush *b = MkBr(0x88000000);
        GdipFillRectangleI(back.g, b, 0, 0, W, ky); GdipFillRectangleI(back.g, b, 0, ky + kh, W, H - ky - kh);
        GdipFillRectangleI(back.g, b, 0, ky, kx, kh); GdipFillRectangleI(back.g, b, kx + kw, ky, W - kx - kw, kh);
        GdipDeleteBrush(b);
        GpPen *p0 = MkPen(0xFF000000, 3, 0), *p1 = MkPen(0xFFFFFFFF, 1, 0);
        GdipDrawRectangleI(back.g, p0, kx, ky, kw, kh); GdipDrawRectangleI(back.g, p1, kx, ky, kw, kh);
        GdipDeletePen(p0); GdipDeletePen(p1);
    }
    if (halo) {                                                  // подсветка курсора (только на экране, в результат не попадает)
        POINT hp; GetCursorPos(&hp); float hx = (float)(hp.x - OX), hy = (float)(hp.y - OY), hr = 26 * sc;
        GpBrush *b = MkBr(0x55FFD60A); GdipFillEllipse(back.g, b, hx - hr, hy - hr, hr * 2, hr * 2); GdipDeleteBrush(b);
        GpPen *p = MkPen(0xCCFFD60A, 2, 2); GdipDrawEllipse(back.g, p, hx - hr, hy - hr, hr * 2, hr * 2); GdipDeletePen(p);
    }
    if (xsel >= 0 && xsel < shN && !shp[xsel]->hid) DrawSel(back.g, shp[xsel]);
    DrawToolbar(back.g);
    if (TipVisible()) DrawTip(back.g);
    POINT pd = {OX, OY}, ps = {0, 0}; SIZE sz = {W, H};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hw, NULL, &pd, &sz, back.dc, &ps, 0, &bf, ULW_ALPHA);
}

/* --- фигуры --- */
static Shp *NewShp(int t) {
    Shp *s = (Shp *)Alloc(sizeof(Shp)); s->t = t; s->col = kCol[g_dCol]; s->w = kWid[g_dWid];
    if (t == T_MARK) s->col = (s->col & 0xFFFFFF) | 0x73000000;
    if (t == T_TEXT) s->txt = (wchar_t *)Alloc(520 * sizeof(wchar_t));
    s->rc = 1; s->rs = 0;
    return s;
}
static void FreeShp(Shp *s) { if (!s) return; FreeShp(s->alt); Free(s->pts); Free(s->txt); Free(s); }
static Shp *CloneShp(Shp *s) {
    Shp *n = (Shp *)Alloc(sizeof(Shp)); memcpy(n, s, sizeof(Shp)); n->alt = NULL; n->pts = NULL; n->txt = NULL; n->cap = 0;
    if (s->n) { n->pts = (PointF *)Alloc(s->n * sizeof(PointF)); memcpy(n->pts, s->pts, s->n * sizeof(PointF)); n->cap = s->n; }
    if (s->txt) { n->txt = (wchar_t *)Alloc(520 * sizeof(wchar_t)); memcpy(n->txt, s->txt, 520 * sizeof(wchar_t)); }
    return n;
}
static void AddShp(Shp *s) {
    for (int i = shN; i < shTot; i++) FreeShp(shp[i]);
    shTot = shN;
    if (shN == shCap) { shCap = shCap ? shCap * 2 : 64; shp = (Shp **)Realloc(shp, shCap * sizeof(Shp *)); }
    shp[shN++] = s; shTot = shN;
}
static void AddPt(Shp *s, int x, int y) {
    if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 64; s->pts = (PointF *)Realloc(s->pts, s->cap * sizeof(PointF)); }
    s->pts[s->n].X = (float)x; s->pts[s->n].Y = (float)y; s->n++;
}
static void CommitText(void) {
    if (!typing) return;
    typing = FALSE;
    if (act && act->txt[0]) AddShp(act); else FreeShp(act);
    act = NULL; Rebuild(FALSE); Present();
}
static BOOL HitShp(Shp *s, float px, float py) {
    float tol = 8 + s->w / 2;
    if ((s->t == T_RECT || s->t == T_ELL) && (s->rc || s->rs)) {          // повёрнутая фигура: точку в её систему координат
        float cx = (s->x1 + s->x2) / 2, cy = (s->y1 + s->y2) / 2, dx = px - cx, dy = py - cy;
        px = cx + s->rc * dx + s->rs * dy; py = cy - s->rs * dx + s->rc * dy;
    }
    float x0 = s->x1 < s->x2 ? s->x1 : s->x2, x1 = s->x1 < s->x2 ? s->x2 : s->x1, y0 = s->y1 < s->y2 ? s->y1 : s->y2, y1 = s->y1 < s->y2 ? s->y2 : s->y1;
    switch (s->t) {
    case T_PEN: case T_MARK: {
        if (s->t == T_MARK) tol = 8 + s->w * 1.75f;
        if (s->n == 1) return SegD(px, py, s->pts[0].X, s->pts[0].Y, s->pts[0].X, s->pts[0].Y) < tol;
        for (int i = 1; i < s->n; i++) if (SegD(px, py, s->pts[i - 1].X, s->pts[i - 1].Y, s->pts[i].X, s->pts[i].Y) < tol) return TRUE;
        return FALSE;
    }
    case T_ARROW: case T_LINE: return SegD(px, py, s->x1, s->y1, s->x2, s->y2) < tol;
    case T_LOUPE: { float r = LensR(s) + 6, dx = px - s->x1, dy = py - s->y1; return dx * dx + dy * dy <= r * r; }
    case T_RECT: case T_ELL: case T_CROP: {
        BOOL out = px >= x0 - tol && px <= x1 + tol && py >= y0 - tol && py <= y1 + tol;
        BOOL in = px > x0 + tol && px < x1 - tol && py > y0 + tol && py < y1 - tol;
        return out && !in;
    }
    case T_BLUR: case T_SPOT: return px >= x0 && px <= x1 && py >= y0 && py <= y1;
    case T_NUM: { float r = NumR(s->w) + 4, dx = px - s->x1, dy = py - s->y1; return dx * dx + dy * dy <= r * r; }
    case T_TEXT: {
        float sz = TxtPx(s->w); int lines = 1, cur = 0, mx = 0;
        for (const wchar_t *c = s->txt; *c; c++) { if (*c == L'\n') { lines++; cur = 0; } else if (++cur > mx) mx = cur; }
        return px >= s->x1 - 4 && px <= s->x1 + mx * sz * 0.6f + 4 && py >= s->y1 - 4 && py <= s->y1 + lines * sz * 1.3f + 4;
    }
    }
    return FALSE;
}
static void EraseAt(int x, int y) {
    for (int i = shN - 1; i >= 0; i--) {
        if (shp[i]->hid || shp[i]->t == S_DEL || !HitShp(shp[i], (float)x, (float)y)) continue;
        Shp *d = (Shp *)Alloc(sizeof(Shp)); d->t = S_DEL; d->ref = i; AddShp(d);
        Rebuild(FALSE); Present(); return;
    }
}

static int TopAt(int x, int y) {
    for (int i = shN - 1; i >= 0; i--)
        if (!shp[i]->hid && shp[i]->t != S_DEL && shp[i]->t != S_XF && HitShp(shp[i], (float)x, (float)y)) return i;
    return -1;
}
/* --- Ctrl: выделение и преобразование готовых фигур --- */
static int XHandle(int x, int y, int *idx) {
    if (xsel < 0 || xsel >= shN) return XM_NONE;
    Xf f; FrameOf(shp[xsel], &f); float hr = 9 * sc, px, py;
    if (f.rot) { XW(&f, 0, -f.hy - 28 * sc, &px, &py); if ((x - px) * (x - px) + (y - py) * (y - py) <= hr * hr) return XM_ROT; }
    if (f.scl) for (int i = 0; i < 8; i++) {
        XW(&f, kGX[i] * f.hx, kGY[i] * f.hy, &px, &py);
        if (x - px <= hr && px - x <= hr && y - py <= hr && py - y <= hr) { *idx = i; return XM_SCALE; }
    }
    return XM_NONE;
}
static BOOL XInside(int x, int y, int i) {
    if (i < 0 || i >= shN) return FALSE;
    Xf f; FrameOf(shp[i], &f); float dx = x - f.cx, dy = y - f.cy, lx = f.rc * dx + f.rs * dy, ly = -f.rs * dx + f.rc * dy, m = 6 * sc;
    return lx >= -f.hx - m && lx <= f.hx + m && ly >= -f.hy - m && ly <= f.hy + m;
}
// Выбор фигуры под курсором, пока зажат Ctrl; без Ctrl выделение снимается.
static void XSync(int x, int y) {
    if (xdrag) return;
    int old = xsel, hi = 0;
    if (!(GetKeyState(VK_CONTROL) & 0x8000) || typing || tbDrag || HitTb(x, y) >= 0) xsel = -1;
    else if (xsel >= 0 && XHandle(x, y, &hi) != XM_NONE) {}
    else {
        int t = TopAt(x, y);
        if (t < 0 && XInside(x, y, xsel)) t = xsel;                 // внутри рамки уже выбранной фигуры
        if (t < 0) for (int i = shN - 1; i >= 0; i--)               // иначе - верхняя фигура, в рамке которой курсор (внутренность пустого прямоугольника)
            if (!shp[i]->hid && shp[i]->t != S_DEL && shp[i]->t != S_XF && XInside(x, y, i)) { t = i; break; }
        if (t >= 0) xsel = t;                                       // пока Ctrl зажат, выбор не слетает - до маркеров можно дотянуться снаружи
    }
    if (xsel != old) Present();
}
static void XSyncCur(void) { POINT p; GetCursorPos(&p); ScreenToClient(hw, &p); XSync(p.x, p.y); }
static void ApplyAff(Shp *c, Shp *o, float a, float b, float cc, float d, float e, float f) {
    c->x1 = a * o->x1 + cc * o->y1 + e; c->y1 = b * o->x1 + d * o->y1 + f;
    c->x2 = a * o->x2 + cc * o->y2 + e; c->y2 = b * o->x2 + d * o->y2 + f;
    for (int i = 0; i < o->n; i++) { c->pts[i].X = a * o->pts[i].X + cc * o->pts[i].Y + e; c->pts[i].Y = b * o->pts[i].X + d * o->pts[i].Y + f; }
}
// Пересчёт фигуры из исходной копии xo по текущему положению мыши (перенос / размер / поворот).
static void XApply(int x, int y) {
    Shp *c = shp[xsel], *o = xo; Xf f = xf0; float mx = (float)x, my = (float)y;
    float a = 1, b = 0, cc = 0, d = 1, e = 0, ff = 0; BOOL aff = TRUE;
    BOOL rl = o->t == T_RECT || o->t == T_ELL || o->t == T_BLUR || o->t == T_SPOT || o->t == T_CROP;
    c->rc = o->rc; c->rs = o->rs;
    if (xmode == XM_MOVE) { e = mx - xmx; ff = my - xmy; }
    else if (xmode == XM_ROT) {
        float ux = xmx - f.cx, uy = xmy - f.cy, vx = mx - f.cx, vy = my - f.cy, lu = Sqrt(ux * ux + uy * uy), lv = Sqrt(vx * vx + vy * vy);
        if (lu < 1 || lv < 1) return;
        ux /= lu; uy /= lu; vx /= lv; vy /= lv;
        float dc = vx * ux + vy * uy, ds = vy * ux - vx * uy;
        if (o->t == T_RECT || o->t == T_ELL) { c->rc = f.rc * dc - f.rs * ds; c->rs = f.rs * dc + f.rc * ds; }
        else { a = dc; b = ds; cc = -ds; d = dc; e = f.cx - dc * f.cx + ds * f.cy; ff = f.cy - ds * f.cx - dc * f.cy; }
    } else {
        float gx = (float)kGX[xh], gy = (float)kGY[xh], dx = mx - f.cx, dy = my - f.cy;
        if (rl) {                                                  // в осях фигуры: противоположная грань неподвижна
            float lx = f.rc * dx + f.rs * dy, ly = -f.rs * dx + f.rc * dy, xa = -f.hx, xb = f.hx, ya = -f.hy, yb = f.hy;
            if (gx) { float ox = -gx * f.hx; xa = ox < lx ? ox : lx; xb = ox < lx ? lx : ox; }
            if (gy) { float oy = -gy * f.hy; ya = oy < ly ? oy : ly; yb = oy < ly ? ly : oy; }
            float ncx = (xa + xb) / 2, ncy = (ya + yb) / 2, nhx = (xb - xa) / 2, nhy = (yb - ya) / 2;
            float wx = f.cx + f.rc * ncx - f.rs * ncy, wy = f.cy + f.rs * ncx + f.rc * ncy;
            c->x1 = wx - nhx; c->x2 = wx + nhx; c->y1 = wy - nhy; c->y2 = wy + nhy; aff = FALSE;
        } else {                                                   // точки: масштаб относительно противоположного маркера
            float ax = f.cx - gx * f.hx, ay = f.cy - gy * f.hy, sx = 1, sy = 1;
            if (gx) { sx = (mx - ax) / (2 * gx * f.hx); if (sx > -0.02f && sx < 0.02f) sx = sx < 0 ? -0.02f : 0.02f; }
            if (gy) { sy = (my - ay) / (2 * gy * f.hy); if (sy > -0.02f && sy < 0.02f) sy = sy < 0 ? -0.02f : 0.02f; }
            a = sx; d = sy; e = ax * (1 - sx); ff = ay * (1 - sy);
        }
    }
    if (aff) ApplyAff(c, o, a, b, cc, d, e, ff);
    if (mx != xmx || my != xmy) xchg = TRUE;
    Rebuild(FALSE); Present();
}
// Распознавание: штрих пера, на конце которого задержали мышь, превращается в линию / прямоугольник / овал.
static void Recognize(Shp *s) {
    int n = s->n; if (n < 8) return;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int i = 0; i < n; i++) {
        float px = s->pts[i].X, py = s->pts[i].Y;
        if (px < x0) x0 = px; if (px > x1) x1 = px; if (py < y0) y0 = py; if (py > y1) y1 = py;
    }
    float w = x1 - x0, h = y1 - y0, ax = s->pts[0].X, ay = s->pts[0].Y, bx = s->pts[n - 1].X, by = s->pts[n - 1].Y;
    float de = Sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay)), big = w > h ? w : h;
    if (de > 25) {
        float dev = 0;
        for (int i = 0; i < n; i++) { float d = SegD(s->pts[i].X, s->pts[i].Y, ax, ay, bx, by); if (d > dev) dev = d; }
        if (dev < 8 + 0.06f * de) {
            s->t = T_LINE; s->x1 = ax; s->y1 = ay; s->x2 = bx; s->y2 = by; Free(s->pts); s->pts = NULL; s->n = s->cap = 0; return;
        }
    }
    if (w < 20 || h < 20 || de > 0.35f * big) return;                 // незамкнутая линия - оставляем пером
    float a = w / 2, b = h / 2, cx = x0 + a, cy = y0 + b, ee = 0, er = 0;
    for (int i = 0; i < n; i++) {
        float px = s->pts[i].X, py = s->pts[i].Y, nx = (px - cx) / a, ny = (py - cy) / b, r = Sqrt(nx * nx + ny * ny);
        ee += r > 1 ? r - 1 : 1 - r;
        float d = px - x0; if (x1 - px < d) d = x1 - px; if (py - y0 < d) d = py - y0; if (y1 - py < d) d = y1 - py;
        er += d;
    }
    ee = ee / n * (a + b) / 2; er = er / n;
    if ((ee < er ? ee : er) > 0.14f * big) return;
    s->t = ee < er ? T_ELL : T_RECT; s->x1 = x0; s->y1 = y0; s->x2 = x1; s->y2 = y1;
    Free(s->pts); s->pts = NULL; s->n = s->cap = 0;
}

/* --- результат --- */
static void Close(void) { if (hw) DestroyWindow(hw); }
static void PrepExport(void) {
    CommitText();
    if (g_dLive) { ShowWindow(hw, SW_HIDE); Sleep(150); Capture(); }     // живой режим: заново снять настоящий экран
    Rebuild(TRUE);
}
static void DoCopy(void) {
    PrepExport();
    int cx = 0, cy = 0, cw = W, ch = H; CropRect(&cx, &cy, &cw, &ch);
    SIZE_T bytes = (SIZE_T)cw * ch * 4;
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + bytes);
    BOOL ok = FALSE;
    if (hg) {
        BYTE *p = (BYTE *)GlobalLock(hg);
        BITMAPINFOHEADER *bh = (BITMAPINFOHEADER *)p; memset(bh, 0, sizeof *bh);
        bh->biSize = sizeof *bh; bh->biWidth = cw; bh->biHeight = ch; bh->biPlanes = 1; bh->biBitCount = 32; bh->biSizeImage = (DWORD)bytes;
        BYTE *d = p + sizeof *bh;
        for (int y = 0; y < ch; y++) memcpy(d + (size_t)y * cw * 4, base.bits + (size_t)(cy + ch - 1 - y) * W + cx, (size_t)cw * 4);
        GlobalUnlock(hg);
        if (OpenClipboard(hw)) { EmptyClipboard(); ok = SetClipboardData(CF_DIB, hg) != NULL; CloseClipboard(); }
        if (!ok) GlobalFree(hg);
    }
    Close();
    Toast(ok ? Tr(L_D_COPIED) : Tr(L_D_COPYFAIL));
}
static void DoSave(void) {
    PrepExport();
    wchar_t dir[MAX_PATH], path[MAX_PATH + 48], nm[48];
    if (g_dSaveDir[0]) lstrcpynW(dir, g_dSaveDir, MAX_PATH); else { lstrcpynW(dir, g_dir, MAX_PATH); Cat(dir, MAX_PATH, L"\\shots"); }
    CreateDirectoryW(dir, NULL);
    SYSTEMTIME st; GetLocalTime(&st);
    wsprintfW(nm, L"Screen_%04d-%02d-%02d_%02d-%02d-%02d.png", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    lstrcpynW(path, dir, MAX_PATH); Cat(path, MAX_PATH + 48, L"\\"); Cat(path, MAX_PATH + 48, nm);
    int cx = 0, cy = 0, cw = W, ch = H; CropRect(&cx, &cy, &cw, &ch);
    Status s;
    if (cw != W || ch != H) {
        GpBitmap *cl = NULL; s = GdipCloneBitmapAreaI(cx, cy, cw, ch, PixelFormat32bppPARGB, base.bmp, &cl);
        if (s == Ok) { s = GdipSaveImageToFile((GpImage *)cl, path, &kPng, NULL); GdipDisposeImage((GpImage *)cl); }
    } else s = GdipSaveImageToFile((GpImage *)base.bmp, path, &kPng, NULL);
    Close();
    wchar_t msg[160]; wsprintfW(msg, Tr(L_D_SAVED), nm);
    Toast(s == Ok ? msg : Tr(L_D_SAVEFAIL));
}
static void XSwap(Shp *r) { Shp *t = shp[r->ref]; shp[r->ref] = r->alt; r->alt = t; }   // S_XF: поменять фигуру в слоте и копию в записи
static void Undo(void) {
    if (typing) { typing = FALSE; FreeShp(act); act = NULL; }
    else if (shN > 0) { shN--; if (shp[shN]->t == S_XF) XSwap(shp[shN]); }
    xsel = -1; Rebuild(FALSE); Present();
}
static void Redo(void) {
    CommitText();
    if (shN < shTot) { if (shp[shN]->t == S_XF) XSwap(shp[shN]); shN++; }
    xsel = -1; Rebuild(FALSE); Present();
}
static BOOL CALLBACK MonEnum(HMONITOR, HDC, LPRECT r, LPARAM l) {
    RECT *a = (RECT *)l; for (int i = 0; i < 8; i++) if (a[i].right == a[i].left) { a[i] = *r; break; }
    return TRUE;
}
// Следующий монитор: снимок заново, рисунки очищаются (координаты старого экрана).
static void NextMonitor(int dir) {
    RECT m[8]; memset(m, 0, sizeof m); EnumDisplayMonitors(NULL, NULL, MonEnum, (LPARAM)m);
    int n = 0, cur = 0; while (n < 8 && m[n].right != m[n].left) { if (m[n].left == OX && m[n].top == OY) cur = n; n++; }
    if (n < 2) return;
    CommitText();
    RECT nr = m[(cur + n + dir) % n];
    ShowWindow(hw, SW_HIDE); Sleep(150);
    FreeShp(act); act = NULL; xsel = -1;
    for (int i = 0; i < shTot; i++) FreeShp(shp[i]);
    shN = shTot = 0;
    SurfFree(&bg); SurfFree(&base); SurfFree(&back);
    OX = nr.left; OY = nr.top; W = nr.right - nr.left; H = nr.bottom - nr.top;
    if (!SurfNew(&bg) || !SurfNew(&base) || !SurfNew(&back)) { Close(); return; }
    Capture();
    SetWindowPos(hw, HWND_TOPMOST, OX, OY, W, H, SWP_NOACTIVATE);
    sc = GetDpiForWindow(hw) / 96.0f;
    tbX = 0; tbY = 0; TbLayout(); tbX = (W - tbW) / 2; tbY = 16 * sc; TbLayout();
    Rebuild(FALSE); Present();
    ShowWindow(hw, SW_SHOW); ForceFg(hw);
}
static void ToggleLive(void) {
    CommitText();
    if (g_dLive) {                                         // живой -> статика: снять экран без оверлея
        ShowWindow(hw, SW_HIDE); Sleep(150); g_dLive = FALSE; Capture();
        ShowWindow(hw, SW_SHOW); ForceFg(hw);
    } else g_dLive = TRUE;
    Rebuild(FALSE); Present();
}
static void TbClick(int hit) {
    CommitText();
    if (hit < NTOOL) tool = hit;
    else if (hit == B_UNDO) { Undo(); return; }
    else if (hit == B_REDO) { Redo(); return; }
    else if (hit == B_MODE) { ToggleLive(); return; }
    else if (hit == B_MON) { NextMonitor((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1); return; }
    else if (hit == B_COPY) { DoCopy(); return; }
    else if (hit == B_SAVE) { DoSave(); return; }
    else if (hit == B_CLOSE) { Close(); return; }
    else if (hit == B_HALO) halo = !halo;
    else if (hit < B_WID) g_dCol = hit - B_COL;
    else g_dWid = hit - B_WID;
    Present();
}

/* --- ввод --- */
static DWORD lastMove;
static void OnDown(int x, int y) {
    int hit = HitTb(x, y); hov = -1;
    if (hit == 99) { tbDrag = TRUE; tbDX = x - tbX; tbDY = y - tbY; SetCapture(hw); return; }
    if (hit >= 0) { TbClick(hit); return; }
    CommitText();
    if (GetKeyState(VK_CONTROL) & 0x8000) {                      // Ctrl: двигать / менять размер / поворачивать готовую фигуру
        XSync(x, y); if (xsel < 0) return;
        int hi = 0; xmode = XHandle(x, y, &hi); xh = hi;
        if (xmode == XM_NONE) { if (!XInside(x, y, xsel) && TopAt(x, y) != xsel) return; xmode = XM_MOVE; }
        xo = CloneShp(shp[xsel]); FrameOf(xo, &xf0); xmx = (float)x; xmy = (float)y; xchg = FALSE; xdrag = TRUE; SetCapture(hw); return;
    }
    if (tool == T_ERASE) { drag = TRUE; SetCapture(hw); EraseAt(x, y); return; }
    if (tool == T_NUM) { Shp *s = NewShp(T_NUM); s->x1 = (float)x; s->y1 = (float)y; s->num = nextNum; AddShp(s); Rebuild(FALSE); Present(); return; }
    Shp *s = NewShp(tool); s->x1 = s->x2 = (float)x; s->y1 = s->y2 = (float)y;
    if (tool == T_PEN || tool == T_MARK) AddPt(s, x, y);
    lastMove = GetTickCount();
    act = s;
    if (tool == T_TEXT) { typing = TRUE; s->y1 = y - TxtPx(s->w) / 2; }
    else { drag = TRUE; SetCapture(hw); }
    Present();
}
static void OnMove(int x, int y) {
    if (tbDrag) {
        tbX = x - tbDX; tbY = y - tbDY;
        if (tbX < 0) tbX = 0; if (tbY < 0) tbY = 0; if (tbX > W - tbW) tbX = W - tbW; if (tbY > H - tbH) tbY = H - tbH;
        TbLayout(); Present(); return;
    }
    if (xdrag) { XApply(x, y); return; }
    if (!drag) {
        XSync(x, y);
        int hit = HitTb(x, y); if (hit < 0 || hit >= NIT) hit = -1;
        if (hit != hov) { hov = hit; hovTick = GetTickCount(); Present(); }
        else if (halo) Present();
        return;
    }
    if (tool == T_ERASE) { EraseAt(x, y); return; }
    if (!act) return;
    BOOL shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (act->t == T_PEN || act->t == T_MARK) {
        if (shift) act->n = 1;                                   // Shift: прямая линия от начала штриха
        PointF *l = &act->pts[act->n - 1]; float dx = x - l->X, dy = y - l->Y;
        if (dx * dx + dy * dy < 4) return;
        AddPt(act, x, y); lastMove = GetTickCount();
    } else {
        float dx = x - act->x1, dy = y - act->y1, ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
        if (shift && (act->t == T_RECT || act->t == T_ELL)) { float m = ax > ay ? ax : ay; dx = dx < 0 ? -m : m; dy = dy < 0 ? -m : m; }   // квадрат / круг
        else if (shift && (act->t == T_LINE || act->t == T_ARROW)) {                                                                   // углы кратны 45°
            if (ay < ax * 0.414f) dy = 0; else if (ax < ay * 0.414f) dx = 0; else { float m = (ax + ay) / 2; dx = dx < 0 ? -m : m; dy = dy < 0 ? -m : m; }
        }
        act->x2 = act->x1 + dx; act->y2 = act->y1 + dy;
    }
    Present();
}
static void OnUp(void) {
    if (tbDrag) { tbDrag = FALSE; ReleaseCapture(); return; }
    if (xdrag) {
        xdrag = FALSE; ReleaseCapture();
        if (xchg) { Shp *r = (Shp *)Alloc(sizeof(Shp)); r->t = S_XF; r->ref = xsel; r->alt = xo; AddShp(r); }   // запись хранит исходную копию
        else FreeShp(xo);
        xo = NULL; Rebuild(FALSE); XSyncCur(); Present(); return;
    }
    if (!drag) return;
    drag = FALSE; ReleaseCapture();
    if (act) {
        float dx = act->x2 - act->x1, dy = act->y2 - act->y1;
        BOOL tiny = (dx < 3 && dx > -3 && dy < 3 && dy > -3) && act->t != T_PEN && act->t != T_MARK;
        if (tiny) FreeShp(act); else AddShp(act);
        act = NULL; Rebuild(FALSE);
    }
    Present();
}
static void OnKey(WPARAM k) {
    BOOL ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (k == VK_ESCAPE) { if (typing) Undo(); else Close(); return; }
    if (k == VK_RETURN) {
        if (typing) { if (shift) { int n = lstrlenW(act->txt); if (n < 510) { act->txt[n] = L'\n'; Present(); } } else CommitText(); }
        else DoCopy();
        return;
    }
    if (ctrl) {
        if (k == 'C') DoCopy(); else if (k == 'S') DoSave();
        else if (k == 'Z') { if (shift) Redo(); else Undo(); }
        else if (k == 'Y') Redo();
        return;
    }
    if (typing) return;
    if (k == 'D') { NextMonitor(shift ? -1 : 1); return; }
    static const char keys[] = "PMALROTNBESCZ";
    for (int i = 0; i < NTOOL; i++) if (k == (WPARAM)keys[i]) { tool = i; Present(); return; }
    if (k == 'H') { halo = !halo; Present(); }
    else if (k >= '1' && k <= '3') { g_dWid = (int)k - '1'; Present(); }
    else if (k == VK_TAB) ToggleLive();
}
static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR: {
        POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
        LPCWSTR cu = IDC_CROSS; int hi = 0;
        if (HitTb(p.x, p.y) >= 0) cu = IDC_ARROW;
        else if (xdrag || xsel >= 0) {
            int m = xdrag ? xmode : XHandle(p.x, p.y, &hi); if (xdrag) hi = xh;
            cu = m == XM_ROT ? IDC_HAND : m == XM_SCALE ? ((hi & 1) ? (hi == 1 || hi == 5 ? IDC_SIZENS : IDC_SIZEWE) : (hi == 0 || hi == 4 ? IDC_SIZENWSE : IDC_SIZENESW)) : IDC_SIZEALL;
        }
        else if (GetKeyState(VK_CONTROL) & 0x8000) cu = IDC_ARROW;
        else if (tool == T_TEXT) cu = IDC_IBEAM;
        SetCursor(LoadCursorW(NULL, cu));
        return TRUE;
    }
    case WM_TIMER: {
        if (!drag && !xdrag && !tbDrag) XSyncCur();
        if (drag && act && act->t == T_PEN && act->n >= 8 && GetTickCount() - lastMove >= 500) { Recognize(act); if (act->t != T_PEN) Present(); }
        BOOL v = TipVisible(); if (v != tipOn) { tipOn = v; Present(); } return 0; }
    case WM_LBUTTONDOWN: OnDown((short)LOWORD(l), (short)HIWORD(l)); return 0;
    case WM_MOUSEMOVE: OnMove((short)LOWORD(l), (short)HIWORD(l)); return 0;
    case WM_LBUTTONUP: OnUp(); return 0;
    case WM_KEYDOWN: if (w == VK_CONTROL) XSyncCur(); else OnKey(w); return 0;
    case WM_KEYUP: if (w == VK_CONTROL) XSyncCur(); return 0;
    case WM_CHAR:
        if (typing && act) {
            int n = lstrlenW(act->txt);
            if (w == 8) { if (n) act->txt[n - 1] = 0; Present(); }
            else if (w >= 32 && n < 510) { act->txt[n] = (wchar_t)w; Present(); }
        }
        return 0;
    case WM_DESTROY: {
        wchar_t t[8];
        IniSet(L"Draw", L"Mode", g_dLive ? L"live" : L"static");
        wsprintfW(t, L"%d", g_dCol); IniSet(L"Draw", L"Color", t);
        wsprintfW(t, L"%d", g_dWid); IniSet(L"Draw", L"Width", t);
        FreeShp(act); act = NULL;
        for (int i = 0; i < shTot; i++) FreeShp(shp[i]);
        Free(shp); shp = NULL; shN = shTot = shCap = 0;
        SurfFree(&bg); SurfFree(&base); SurfFree(&back);
        GdiplusShutdown(gdiTok);
        KillTimer(h, 1); FreeShp(xo); xo = NULL; xsel = -1; typing = drag = tbDrag = xdrag = tipOn = FALSE; hov = -1; hw = NULL;
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}
static void Open(void) {
    if (hw) { DestroyWindow(hw); return; }
    POINT pt; GetCursorPos(&pt);
    MONITORINFO mi; mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    OX = mi.rcMonitor.left; OY = mi.rcMonitor.top; W = mi.rcMonitor.right - OX; H = mi.rcMonitor.bottom - OY;
    GdiplusStartupInput in;
    if (GdiplusStartup(&gdiTok, &in, NULL) != Ok) { Toast(Tr(L_D_GDIFAIL)); return; }
    if (!SurfNew(&bg) || !SurfNew(&base) || !SurfNew(&back)) {
        SurfFree(&bg); SurfFree(&base); SurfFree(&back); GdiplusShutdown(gdiTok); Toast(Tr(L_D_MEMFAIL)); return;
    }
    Capture();
    RegClass(L"DKDraw", Proc, 0);
    hw = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"DKDraw", L"", WS_POPUP, OX, OY, W, H, NULL, NULL, g_inst, NULL);
    if (!hw) { SurfFree(&bg); SurfFree(&base); SurfFree(&back); GdiplusShutdown(gdiTok); return; }
    sc = GetDpiForWindow(hw) / 96.0f;
    tool = T_PEN; tbX = 0; tbY = 0; TbLayout();
    tbX = (W - tbW) / 2; tbY = 16 * sc; TbLayout();
    hov = -1; tipOn = FALSE; SetTimer(hw, 1, 150, NULL);
    Rebuild(FALSE); Present();
    ShowWindow(hw, SW_SHOW); ForceFg(hw); SetFocus(hw);
}
}   // namespace DrawNs
static void DrawOpen(void) { DrawNs::Open(); }

/* ---------- пипетка: снимок виртуального экрана на весь экран + круглая лупа 4x (11x11 пикселей) у курсора ---------- */
namespace PickNs {
enum { SRC = 13, ZOOM = 4, LD = SRC * ZOOM, LABW = 96, LABH = 24 };   // круг 52 px (~50), 13x13 пикселей источника, центр - курсор
static HWND hw; static HDC mdc; static HBITMAP hb, hbOld; static DWORD *bits; static HFONT font;
static int VX, VY, VW, VH; static RECT prev; static BOOL havePrev;

static void Close(void) { if (hw) DestroyWindow(hw); }
static void Cur(POINT *c) {
    GetCursorPos(c); c->x -= VX; c->y -= VY;
    if (c->x < 0) c->x = 0; if (c->y < 0) c->y = 0; if (c->x >= VW) c->x = VW - 1; if (c->y >= VH) c->y = VH - 1;
}
static void Update(void) {
    POINT c; Cur(&c);
    HDC dc = GetDC(hw);
    if (havePrev) BitBlt(dc, prev.left, prev.top, prev.right - prev.left, prev.bottom - prev.top, mdc, prev.left, prev.top, SRCCOPY);
    int tw = LABW, lx = c.x - LD / 2, ly = c.y - LD / 2, bx = c.x - tw / 2;
    int ty = ly + LD + 4;                                   // плашка под кругом, а у нижнего края - над ним
    if (ty + LABH > VH) ty = ly - 4 - LABH;
    if (bx < 0) bx = 0; if (bx + tw > VW) bx = VW - tw;
    HRGN rg = CreateEllipticRgn(lx, ly, lx + LD + 1, ly + LD + 1);
    SelectClipRgn(dc, rg);
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchBlt(dc, lx, ly, LD, LD, mdc, c.x - SRC / 2, c.y - SRC / 2, SRC, SRC, SRCCOPY);
    SelectClipRgn(dc, NULL); DeleteObject(rg);
    HPEN pw = CreatePen(PS_SOLID, 1, RGB(255, 255, 255)), pk = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH)), op = SelectObject(dc, pw);
    Ellipse(dc, lx, ly, lx + LD, ly + LD);                  // тонкая обводка 1 px
    int cx = lx + (SRC / 2) * ZOOM, cy = ly + (SRC / 2) * ZOOM;       // клетка под курсором
    SelectObject(dc, pk); Rectangle(dc, cx - 1, cy - 1, cx + ZOOM + 1, cy + ZOOM + 1);
    SelectObject(dc, pw); Rectangle(dc, cx, cy, cx + ZOOM, cy + ZOOM);
    DWORD v = bits[(size_t)c.y * VW + c.x] & 0xFFFFFF;
    COLORREF col = RGB((v >> 16) & 255, (v >> 8) & 255, v & 255);
    HBRUSH bl = CreateSolidBrush(RGB(32, 33, 36)), bs = CreateSolidBrush(col);
    SelectObject(dc, pk); SelectObject(dc, bl); RoundRect(dc, bx, ty, bx + tw, ty + LABH, 8, 8);
    SelectObject(dc, pw); SelectObject(dc, bs); Rectangle(dc, bx + 6, ty + 5, bx + 20, ty + LABH - 5);
    wchar_t t[16]; wsprintfW(t, L"#%06X", v);
    RECT tr = {bx + 26, ty, bx + tw - 4, ty + LABH};
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
    HGDIOBJ of = SelectObject(dc, font);
    DrawTextW(dc, t, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(dc, of); SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(pw); DeleteObject(pk); DeleteObject(bl); DeleteObject(bs);
    ReleaseDC(hw, dc);
    prev.left = (lx < bx ? lx : bx) - 3; prev.right = (lx + LD > bx + tw ? lx + LD : bx + tw) + 3;
    prev.top = (ty < ly ? ty : ly) - 3; prev.bottom = (ty + LABH > ly + LD ? ty + LABH : ly + LD) + 3; havePrev = TRUE;
}
static void Pick(void) {
    POINT c; Cur(&c);
    DWORD v = bits[(size_t)c.y * VW + c.x] & 0xFFFFFF;
    wchar_t t[16]; wsprintfW(t, L"#%06X", v);
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, sizeof t);
    BOOL ok = FALSE;
    if (hg) {
        memcpy(GlobalLock(hg), t, sizeof t); GlobalUnlock(hg);
        if (OpenClipboard(hw)) { EmptyClipboard(); ok = SetClipboardData(CF_UNICODETEXT, hg) != NULL; CloseClipboard(); }
        if (!ok) GlobalFree(hg);
    }
    Close();
    if (ok) { wchar_t m[80]; wsprintfW(m, Tr(L_PK_COPIED), t); Toast(m); }
}
static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        BitBlt(dc, 0, 0, VW, VH, mdc, 0, 0, SRCCOPY);
        EndPaint(h, &ps); havePrev = FALSE; Update(); return 0;
    }
    case WM_SETCURSOR: SetCursor(NULL); return TRUE;           // курсор скрыт: его место занимает центр лупы
    case WM_MOUSEMOVE: Update(); return 0;
    case WM_LBUTTONDOWN: Pick(); return 0;
    case WM_RBUTTONDOWN: Close(); return 0;
    case WM_KEYDOWN: {
        if (w == VK_ESCAPE) { Close(); return 0; }
        if (w == VK_RETURN) { Pick(); return 0; }
        int dx = w == VK_LEFT ? -1 : w == VK_RIGHT ? 1 : 0, dy = w == VK_UP ? -1 : w == VK_DOWN ? 1 : 0;
        if (dx || dy) { POINT p; GetCursorPos(&p); SetCursorPos(p.x + dx, p.y + dy); Update(); }
        return 0;
    }
    case WM_DESTROY:
        if (mdc) { SelectObject(mdc, hbOld); DeleteDC(mdc); mdc = NULL; }
        if (hb) { DeleteObject(hb); hb = NULL; }
        if (font) { DeleteObject(font); font = NULL; }
        InterlockedExchange(&g_pickActive, 0); hw = NULL; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static void Open(void) {
    if (hw) { DestroyWindow(hw); return; }
    VX = GetSystemMetrics(SM_XVIRTUALSCREEN); VY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    VW = GetSystemMetrics(SM_CXVIRTUALSCREEN); VH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = VW; bi.bmiHeader.biHeight = -VH;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    hb = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (!hb) return;
    mdc = CreateCompatibleDC(NULL); hbOld = (HBITMAP)SelectObject(mdc, hb);
    HDC sdc = GetDC(NULL); BitBlt(mdc, 0, 0, VW, VH, sdc, VX, VY, SRCCOPY | CAPTUREBLT); ReleaseDC(NULL, sdc);
    font = CreateFontW(-MulDiv(10, GetDpiForSystem(), 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    RegClass(L"DKPick", Proc, 0);
    hw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"DKPick", L"", WS_POPUP, VX, VY, VW, VH, NULL, NULL, g_inst, NULL);
    if (!hw) { SelectObject(mdc, hbOld); DeleteDC(mdc); mdc = NULL; DeleteObject(hb); hb = NULL; DeleteObject(font); font = NULL; return; }
    havePrev = FALSE;
    ShowWindow(hw, SW_SHOW); ForceFg(hw); SetFocus(hw);
    InterlockedExchange(&g_pickActive, 1);
}
}   // namespace PickNs
static void PickerOpen(void) { PickNs::Open(); }

/* ---------- запись экрана: монитор -> H.264 mp4 (Media Foundation), кнопка-кружок скрыта из кадра ---------- */
static void OvCfgSave(OvCfg *c) { wchar_t v[48]; wsprintfW(v, L"%d,%d,%d,%d", c->mode, c->x, c->y, c->sc); IniSet(L"Overlay", c->stem, v); }
namespace RecNs {
using namespace Gdiplus;
using namespace Gdiplus::DllExports;
static HWND hw; static HANDLE th, ev; static volatile LONG stopF, okF; static BOOL recording, moved;
static RECT cr; static wchar_t path[MAX_PATH], fname[64]; static int fps; static POINT dn, wp; static BOOL areaOn; static RECT area; static HWND selw; static POINT selA; static RECT selR; static BOOL selHas; static RECT selMon;

extern "C" HRESULT WINAPI SHGetFolderPathW(HWND, int, HANDLE, DWORD, LPWSTR);
static void Dir(wchar_t *d) {
    if (g_rDir[0]) { lstrcpynW(d, g_rDir, MAX_PATH); return; }
    if (SHGetFolderPathW(NULL, 0x000e, NULL, 0, d) != S_OK) { lstrcpynW(d, g_dir, MAX_PATH); Cat(d, MAX_PATH, L"\\videos"); }   // CSIDL_MYVIDEO
}
/* --- оверлей ввода: пресеты input-overlay (json + png-атлас) --- */
static BOOL KeyOn(int c) { return g_ioKey[c] || GetTickCount() - g_ioHit[c] < 70; }
static BOOL MouseOn(int c) { return g_ioMouse[c] || GetTickCount() - g_ioMHit[c] < 70; }
struct OvPre { wchar_t grp[48], stem[64], json[MAX_PATH], png[MAX_PATH]; };
struct OvEl { int t, code, px, py, mx, my, mw, mh; };
struct Ov { OvPre *pr; OvCfg *c; int cw, ch, n; OvEl *el; GpImage *img; };
#define NOVL 6                                                       // сколько оверлеев одновременно
static OvPre pre[24]; static int npre;

static const char *JFind(const char *s, const char *e, const char *key) {      // значение по ключу "key": -> указатель после ':'
    int kl = lstrlenA(key);
    for (const char *p = s; p + kl + 2 < e; p++) {
        if (*p != '"' || p[kl + 1] != '"' || memcmp(p + 1, key, kl)) continue;
        const char *q = p + kl + 2; while (q < e && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
        if (q < e && *q == ':') { q++; while (q < e && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++; return q; }
    }
    return NULL;
}
static int JInt(const char *&q, const char *e) {                              // число (в кавычках тоже), дробная часть отбрасывается
    while (q < e && !((*q >= '0' && *q <= '9') || *q == '-')) q++;
    int neg = 0, v = 0; if (q < e && *q == '-') { neg = 1; q++; }
    while (q < e && *q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0');
    if (q < e && *q == '.') { q++; while (q < e && *q >= '0' && *q <= '9') q++; }
    return neg ? -v : v;
}
static BOOL OvLoad(OvPre *p, OvCfg *c, Ov *o) {
    memset(o, 0, sizeof *o); o->pr = p; o->c = c;
    HANDLE f = CreateFileW(p->json, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    DWORD sz = GetFileSize(f, NULL), rd = 0;
    if (!sz || sz > 4000000) { CloseHandle(f); return FALSE; }
    char *s = (char *)Alloc(sz + 1); ReadFile(f, s, sz, &rd, NULL); CloseHandle(f); s[rd] = 0;
    const char *e = s + rd, *q;
    if ((q = JFind(s, e, "overlay_width"))) o->cw = JInt(q, e);
    if ((q = JFind(s, e, "overlay_height"))) o->ch = JInt(q, e);
    const char *el = JFind(s, e, "elements"); int n = 0;
    if (el) for (const char *x = el; x < e; x++) if (*x == '{') n++;
    if (el && n && o->cw > 0 && o->ch > 0) {
        o->el = (OvEl *)Alloc(n * sizeof(OvEl));
        const char *x = el;
        while (x < e && o->n < n) {
            const char *b = x; while (b < e && *b != '{') b++;
            const char *d = b; while (d < e && *d != '}') d++;
            if (b >= e || d >= e) break;
            x = d + 1;
            OvEl *z = &o->el[o->n];
            const char *v = JFind(b, d, "type"); if (!v) continue;
            z->t = JInt(v, d);
            if (z->t != 0 && z->t != 1 && z->t != 3 && z->t != 4) continue;           // остальные типы (геймпад, движение мыши) не поддерживаются
            if ((v = JFind(b, d, "code"))) z->code = JInt(v, d);
            if (!(v = JFind(b, d, "pos"))) continue;
            z->px = JInt(v, d); z->py = JInt(v, d);
            v = JFind(b, d, "mapping"); if (!v) continue;
            z->mx = JInt(v, d); z->my = JInt(v, d); z->mw = JInt(v, d); z->mh = JInt(v, d);
            if (z->mw > 0 && z->mh > 0) o->n++;
        }
    }
    Free(s);
    if (o->n) {
        GpImage *im = NULL; GdipLoadImageFromFile(p->png, &im);
        if (im) {                                                    // атлас в предумноженный ARGB: рисуется в разы быстрее исходного PNG
            UINT iw = 0, ih = 0; GdipGetImageWidth(im, &iw); GdipGetImageHeight(im, &ih);
            GpBitmap *cl = NULL; GdipCloneBitmapAreaI(0, 0, iw, ih, PixelFormat32bppPARGB, (GpBitmap *)im, &cl);
            if (cl) { GdipDisposeImage(im); o->img = (GpImage *)cl; } else o->img = im;
        }
    }
    if (!o->n || !o->img) { Free(o->el); o->el = NULL; if (o->img) GdipDisposeImage(o->img); o->img = NULL; return FALSE; }
    return TRUE;
}
static void OvFree(Ov *o) { Free(o->el); o->el = NULL; if (o->img) GdipDisposeImage(o->img); o->img = NULL; }
// Отрисовка пресета в точку (ox, oy) с масштабом s. Нажатая версия спрайта лежит в атласе на 2 px ниже обычной.
static void DrawOv(GpGraphics *g, Ov *o, float ox, float oy, float s, int fl) {   // fl: 1 - не показывать клавиши (поле пароля), 2 - показать всё нажатым (настройка)
    DWORD now = GetTickCount();
    GdipSetInterpolationMode(g, s == 1.f ? InterpolationModeNearestNeighbor : InterpolationModeHighQualityBilinear);
    GdipSetPixelOffsetMode(g, PixelOffsetModeHalf);
    for (int i = 0; i < o->n; i++) {
        OvEl *e = &o->el[i]; int sx = e->mx, sy = e->my, sw = e->mw, sh = e->mh, dx = (int)(ox + e->px * s), dy = (int)(oy + e->py * s), dw = (int)(sw * s + 0.5f), dh = (int)(sh * s + 0.5f);
        if (e->t == 1) { if (!(fl & 1) && ((fl & 2) || KeyOn(e->code & 0xFFFF))) sy += sh + 2; }
        else if (e->t == 3) { if ((fl & 2) || (!(fl & 1) && MouseOn(e->code & 7))) sy += sh + 2; }
        else if (e->t == 4) { if ((fl & 2) || (!(fl & 1) && MouseOn(3))) sx += sw + 2; }          // колесо: нажатая средняя кнопка
        if (e->t == 1 && (fl & 1)) continue;
        GdipDrawImageRectRectI(g, o->img, dx, dy, dw, dh, sx, sy, sw, sh, UnitPixel, NULL, NULL, NULL);
        if (e->t == 4 && !(fl & 1) && (fl & 2 || now - g_ioWheelT < 150)) {                               // и стрелка прокрутки вверх / вниз
            int k = (fl & 2) || g_ioWheelD > 0 ? 2 : 3;
            GdipDrawImageRectRectI(g, o->img, dx, dy, dw, dh, e->mx + k * (sw + 2), e->my, sw, sh, UnitPixel, NULL, NULL, NULL);
        }
    }
}
static BOOL PreFits(const wchar_t *png, const wchar_t *stem, int *len) {        // имя png (без .png) - начало имени json
    int l = lstrlenW(png) - 4; if (l <= 0 || l > lstrlenW(stem)) return FALSE;
    if (CompareStringOrdinal(png, l, stem, l, TRUE) != CSTR_EQUAL) return FALSE;
    *len = l; return TRUE;
}
static void ScanDir(const wchar_t *dir, int depth, const wchar_t *grp) {
    wchar_t pat[MAX_PATH]; lstrcpynW(pat, dir, MAX_PATH); Cat(pat, MAX_PATH, L"\\*");
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;
        wchar_t full[MAX_PATH]; lstrcpynW(full, dir, MAX_PATH); Cat(full, MAX_PATH, L"\\"); Cat(full, MAX_PATH, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 2) ScanDir(full, depth + 1, depth == 0 ? fd.cFileName : grp); continue; }
        int l = lstrlenW(fd.cFileName); if (l < 6 || lstrcmpiW(fd.cFileName + l - 5, L".json") || npre >= 24) continue;
        OvPre *p = &pre[npre]; lstrcpynW(p->grp, grp, 48); lstrcpynW(p->stem, fd.cFileName, l - 4 < 64 ? l - 4 : 64); lstrcpynW(p->json, full, MAX_PATH);
        wchar_t pp[MAX_PATH]; lstrcpynW(pp, dir, MAX_PATH); Cat(pp, MAX_PATH, L"\\*.png");
        WIN32_FIND_DATAW f2; HANDLE h2 = FindFirstFileW(pp, &f2); int best = 0;
        if (h2 != INVALID_HANDLE_VALUE) {
            do { int pl; if (PreFits(f2.cFileName, p->stem, &pl) && pl > best) { best = pl; lstrcpynW(p->png, dir, MAX_PATH); Cat(p->png, MAX_PATH, L"\\"); Cat(p->png, MAX_PATH, f2.cFileName); } }
            while (FindNextFileW(h2, &f2));
            FindClose(h2);
        }
        if (best) npre++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static void ScanOv(void) { npre = 0; wchar_t d[MAX_PATH]; lstrcpynW(d, g_dir, MAX_PATH); Cat(d, MAX_PATH, L"\\overlays"); ScanDir(d, 0, L""); }

/* --- поля пароля: UI Automation, пока фокус в таком поле - клавиши в оверлее не показываются --- */
static volatile LONG pwdNow, pwdStop;
static DWORD WINAPI PwdWorker(LPVOID) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IUIAutomation *ua = NULL;
    if (SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation), NULL, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), (void **)&ua)) && ua) {
        while (!pwdStop) {
            IUIAutomationElement *el = NULL; BOOL p = FALSE;
            if (SUCCEEDED(ua->GetFocusedElement(&el)) && el) { el->get_CurrentIsPassword(&p); el->Release(); }
            pwdNow = p ? 1 : 0; Sleep(150);
        }
        ua->Release();
    }
    pwdNow = 0; CoUninitialize(); return 0;
}

static const PROPERTYKEY kFriendly = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};   // PKEY_Device_FriendlyName
static CRITICAL_SECTION cs; static LARGE_INTEGER fq, t0; static volatile LONG paused; static volatile long long pauseAcc, pauseStart;
static IAudioClient *aclient; static IAudioCaptureClient *aclip; static IMFSinkWriter *swr; static int aStream = -1;

static long long ToU(long long ticks) { return ticks / fq.QuadPart * 10000000LL + ticks % fq.QuadPart * 10000000LL / fq.QuadPart; }   // тики QPC -> 100 нс без переполнения
static long long Now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static void WriteBuf(IMFSinkWriter *wr, int stream, const void *data, DWORD bytes, long long ts, long long dur) {
    IMFMediaBuffer *mb = NULL; IMFSample *sm = NULL;
    if (ts < 0) ts = 0;
    if (FAILED(MFCreateMemoryBuffer(bytes, &mb))) return;
    BYTE *p = NULL; if (SUCCEEDED(mb->Lock(&p, NULL, NULL))) { if (data) memcpy(p, data, bytes); else memset(p, 0, bytes); mb->Unlock(); }
    mb->SetCurrentLength(bytes);
    if (SUCCEEDED(MFCreateSample(&sm))) {
        sm->AddBuffer(mb); sm->SetSampleTime(ts); sm->SetSampleDuration(dur);
        EnterCriticalSection(&cs); wr->WriteSample(stream, sm); LeaveCriticalSection(&cs);
        sm->Release();
    }
    mb->Release();
}
// Микрофон (WASAPI, общий режим, 16 бит 48 кГц стерео с автопреобразованием). Имя пустое - по умолчанию, "-" - без звука.
static BOOL AudioInit(void) {
    if (g_rMic[0] == L'-' && !g_rMic[1]) return FALSE;
    BOOL ok = FALSE; IMMDeviceEnumerator *en = NULL; IMMDevice *dev = NULL;
    if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void **)&en))) return FALSE;
    if (g_rMic[0]) {
        IMMDeviceCollection *col = NULL; UINT n = 0;
        if (SUCCEEDED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col))) {
            col->GetCount(&n);
            for (UINT i = 0; i < n && !dev; i++) {
                IMMDevice *d = NULL; IPropertyStore *ps = NULL; PROPVARIANT pv; PropVariantInit(&pv);
                if (SUCCEEDED(col->Item(i, &d)) && SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)) && SUCCEEDED(ps->GetValue(kFriendly, &pv)) && pv.vt == VT_LPWSTR && !lstrcmpW(pv.pwszVal, g_rMic)) { dev = d; d = NULL; }
                PropVariantClear(&pv); if (ps) ps->Release(); if (d) d->Release();
            }
            col->Release();
        }
    }
    if (!dev) en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev);
    if (dev) {
        WAVEFORMATEX wf; memset(&wf, 0, sizeof wf); wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 2; wf.nSamplesPerSec = 48000; wf.wBitsPerSample = 16; wf.nBlockAlign = 4; wf.nAvgBytesPerSec = 192000;
        if (SUCCEEDED(dev->Activate(IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&aclient))
            && SUCCEEDED(aclient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 10000000, 0, &wf, NULL))
            && SUCCEEDED(aclient->GetService(IID_IAudioCaptureClient, (void **)&aclip))) ok = TRUE;
        dev->Release();
    }
    en->Release();
    if (!ok) { if (aclip) { aclip->Release(); aclip = NULL; } if (aclient) { aclient->Release(); aclient = NULL; } }
    return ok;
}
static DWORD WINAPI AudioWorker(LPVOID) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    long long t0u = ToU(t0.QuadPart);
    aclient->Start();
    while (!stopF) {
        Sleep(10);
        UINT32 n = 0;
        while (SUCCEEDED(aclip->GetNextPacketSize(&n)) && n) {
            BYTE *d = NULL; UINT32 fr = 0; DWORD fl = 0; UINT64 dpos = 0, qpc = 0;
            if (FAILED(aclip->GetBuffer(&d, &fr, &fl, &dpos, &qpc))) break;
            if (!paused && fr) WriteBuf(swr, aStream, (fl & AUDCLNT_BUFFERFLAGS_SILENT) ? NULL : d, fr * 4, (long long)qpc - t0u - ToU(pauseAcc), (long long)fr * 10000000LL / 48000);
            aclip->ReleaseBuffer(fr);
        }
    }
    aclient->Stop();
    CoUninitialize(); return 0;
}
// Видеодорожка H.264 в писателе: вход RGB32 (снизу вверх), выход H.264
static BOOL AddVid(IMFSinkWriter *wr, int w, int h, DWORD *si) {
    IMFMediaType *mo = NULL, *mi = NULL; BOOL ok = FALSE;
    if (SUCCEEDED(MFCreateMediaType(&mo)) && SUCCEEDED(MFCreateMediaType(&mi))) {
        unsigned long long br = (unsigned long long)w * h * fps / 10; if (br < 2000000) br = 2000000; if (br > 40000000) br = 40000000;
        mo->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); mo->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264); mo->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)br);
        mi->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); mi->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        IMFMediaType *ts[2] = {mo, mi};
        for (int i = 0; i < 2; i++) {
            ts[i]->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            MFSetAttributeSize(ts[i], MF_MT_FRAME_SIZE, w, h); MFSetAttributeRatio(ts[i], MF_MT_FRAME_RATE, fps, 1); MFSetAttributeRatio(ts[i], MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        }
        ok = SUCCEEDED(wr->AddStream(mo, si)) && SUCCEEDED(wr->SetInputMediaType(*si, mi, NULL));
    }
    if (mo) mo->Release(); if (mi) mi->Release();
    return ok;
}
static BOOL MkDib(HDC ref, int w, int h, HDC *dc, HBITMAP *hb, void **bits) {
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = h;     // снизу вверх - как RGB32 в Media Foundation
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    *bits = NULL; *hb = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, bits, NULL, 0);
    if (!*hb) return FALSE;
    *dc = CreateCompatibleDC(ref); SelectObject(*dc, *hb); return TRUE;
}
static BOOL MkGp(void *bits, int w, int h, PixelFormat fmt, GpBitmap **bmp, GpGraphics **g) {  // GDI+ поверх DIB снизу вверх (отрицательный шаг строки)
    *bmp = NULL; *g = NULL;
    GdipCreateBitmapFromScan0(w, h, -w * 4, fmt,(BYTE *)bits + (size_t)(h - 1) * w * 4, bmp);
    if (!*bmp) return FALSE;
    GdipGetImageGraphicsContext((GpImage *)*bmp, g);
    if (!*g) return FALSE;
    GdipSetInterpolationMode(*g, InterpolationModeHighQualityBicubic); return TRUE;
}
struct OvOut { IMFSinkWriter *wr; HDC dc; HBITMAP hb; void *bits; GpBitmap *bmp; GpGraphics *g; int w, h; DWORD bytes; };

static DWORD WINAPI Worker(LPVOID) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    ULONG_PTR gtok = 0; GdiplusStartupInput gin; BOOL gp = GdiplusStartup(&gtok, &gin, NULL) == Ok;
    BOOL started = FALSE, mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)), audio = FALSE; HANDLE ath = NULL, pth = NULL;
    int w = cr.right - cr.left, h = cr.bottom - cr.top;
    Ov ov[NOVL]; OvOut oo[NOVL]; int nov = 0; memset(ov, 0, sizeof ov); memset(oo, 0, sizeof oo);
    HDC sdc = GetDC(NULL);
    swr = NULL; aStream = -1;
    if (mf) {
        DWORD si = 0;
        audio = AudioInit();
        if (gp) for (int i = 0; i < npre && nov < NOVL; i++) { OvCfg *c = OvCfgGet(pre[i].stem, FALSE); if (c && c->mode && OvLoad(&pre[i], c, &ov[nov])) nov++; }
        if (SUCCEEDED(MFCreateSinkWriterFromURL(path, NULL, NULL, &swr)) && AddVid(swr, w, h, &si)) {
            started = TRUE;
            if (audio) {                                               // звук - вторая дорожка (AAC); не получилось - пишем без звука
                IMFMediaType *ao = NULL, *ai = NULL; DWORD sa = 0;
                if (SUCCEEDED(MFCreateMediaType(&ao)) && SUCCEEDED(MFCreateMediaType(&ai))) {
                    ao->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio); ao->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
                    ao->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16); ao->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000); ao->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
                    ao->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000); ao->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
                    ai->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio); ai->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                    ai->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16); ai->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000); ai->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
                    ai->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4); ai->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000);
                    if (SUCCEEDED(swr->AddStream(ao, &sa)) && SUCCEEDED(swr->SetInputMediaType(sa, ai, NULL))) aStream = (int)sa;
                }
                if (ao) ao->Release(); if (ai) ai->Release();
            }
            started = SUCCEEDED(swr->BeginWriting());
        }
        for (int k = 0; started && k < nov; k++) if (ov[k].c->mode & 2) {   // оверлей отдельным файлом на зелёном фоне (хромакей)
            OvOut *o = &oo[k]; float s = ov[k].c->sc / 100.f;
            o->w = (int)(ov[k].cw * s + 0.5f) & ~1; o->h = (int)(ov[k].ch * s + 0.5f) & ~1; if (o->w < 16) o->w = 16; if (o->h < 16) o->h = 16;
            wchar_t p2[MAX_PATH]; lstrcpynW(p2, path, MAX_PATH); p2[lstrlenW(p2) - 4] = 0; Cat(p2, MAX_PATH, L"_"); Cat(p2, MAX_PATH, ov[k].pr->stem); Cat(p2, MAX_PATH, L".mp4");
            DWORD s2 = 0;
            if (SUCCEEDED(MFCreateSinkWriterFromURL(p2, NULL, NULL, &o->wr)) && AddVid(o->wr, o->w, o->h, &s2) && SUCCEEDED(o->wr->BeginWriting())
                && MkDib(sdc, o->w, o->h, &o->dc, &o->hb, &o->bits) && MkGp(o->bits, o->w, o->h, PixelFormat32bppRGB, &o->bmp, &o->g)) o->bytes = (DWORD)o->w * o->h * 4;
            else { if (o->wr) { o->wr->Release(); o->wr = NULL; } DeleteFileW(p2); }
        }
    }
    okF = started; SetEvent(ev);
    if (started) {
        HDC mdc = NULL; HBITMAP hb = NULL; void *bits = NULL; GpBitmap *fb = NULL; GpGraphics *fg = NULL;
        if (MkDib(sdc, w, h, &mdc, &hb, &bits)) {
            if (nov) MkGp(bits, w, h, PixelFormat32bppRGB, &fb, &fg);
            DWORD bytes = (DWORD)w * h * 4;
            QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0); pauseAcc = 0; paused = 0; pwdStop = 0; pwdNow = 0;
            if (aStream >= 0) ath = CreateThread(NULL, 0, AudioWorker, NULL, 0, NULL);
            if (nov && g_rPwd) pth = CreateThread(NULL, 0, PwdWorker, NULL, 0, NULL);
            long long interval = fq.QuadPart / fps, next = t0.QuadPart, dur = 10000000LL / fps;
            timeBeginPeriod(1);
            while (!stopF) {
                if (paused) { Sleep(20); next = Now(); continue; }
                long long tn = Now();
                BitBlt(mdc, 0, 0, w, h, sdc, cr.left, cr.top, SRCCOPY | CAPTUREBLT);
                if (g_rCur) {
                    CURSORINFO ci; ci.cbSize = sizeof ci; ICONINFO ii;
                    if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && GetIconInfo(ci.hCursor, &ii)) {
                        DrawIconEx(mdc, ci.ptScreenPos.x - (int)ii.xHotspot - cr.left, ci.ptScreenPos.y - (int)ii.yHotspot - cr.top, ci.hCursor, 0, 0, 0, NULL, DI_NORMAL);
                        if (ii.hbmMask) DeleteObject(ii.hbmMask); if (ii.hbmColor) DeleteObject(ii.hbmColor);
                    }
                }
                long long ts = ToU(tn - t0.QuadPart - pauseAcc); int hk = pwdNow && g_rPwd ? 1 : 0;
                GdiFlush();
                if (fg) {
                    for (int k = 0; k < nov; k++) if (ov[k].c->mode & 1) DrawOv(fg, &ov[k], w * ov[k].c->x / 1000.f, h * ov[k].c->y / 1000.f, ov[k].c->sc / 100.f, hk);
                    GdipFlush(fg, FlushIntentionSync);
                }
                WriteBuf(swr, 0, bits, bytes, ts, dur);
                for (int k = 0; k < nov; k++) if (oo[k].wr) {
                    for (DWORD *px = (DWORD *)oo[k].bits, n = (DWORD)oo[k].w * oo[k].h; n; n--) *px++ = 0xFF00FF00;     // чистый зелёный
                    DrawOv(oo[k].g, &ov[k], 0, 0, ov[k].c->sc / 100.f, hk); GdipFlush(oo[k].g, FlushIntentionSync);
                    WriteBuf(oo[k].wr, 0, oo[k].bits, oo[k].bytes, ts, dur);
                }
                next += interval; tn = Now();
                long long wait = (next - tn) * 1000 / fq.QuadPart;
                if (wait > 0) Sleep((DWORD)wait); else next = tn;        // отстали - долг не копим
            }
            timeEndPeriod(1);
            if (ath) { WaitForSingleObject(ath, 5000); CloseHandle(ath); ath = NULL; }
            if (pth) { pwdStop = 1; WaitForSingleObject(pth, 3000); CloseHandle(pth); pth = NULL; }
            if (fg) GdipDeleteGraphics(fg); if (fb) GdipDisposeImage((GpImage *)fb);
            DeleteDC(mdc); DeleteObject(hb);
        }
    }
    for (int k = 0; k < NOVL; k++) {
        if (oo[k].wr) { if (started) oo[k].wr->Finalize(); oo[k].wr->Release(); }
        if (oo[k].g) GdipDeleteGraphics(oo[k].g); if (oo[k].bmp) GdipDisposeImage((GpImage *)oo[k].bmp);
        if (oo[k].dc) DeleteDC(oo[k].dc); if (oo[k].hb) DeleteObject(oo[k].hb);
    }
    for (int k = 0; k < nov; k++) OvFree(&ov[k]);
    if (sdc) ReleaseDC(NULL, sdc);
    if (swr) { if (started) swr->Finalize(); swr->Release(); swr = NULL; }
    if (aclip) { aclip->Release(); aclip = NULL; } if (aclient) { aclient->Release(); aclient = NULL; }
    if (gp) GdiplusShutdown(gtok);
    if (mf) MFShutdown();
    CoUninitialize(); return 0;
}
static void Repaint(void) { if (hw) InvalidateRect(hw, NULL, FALSE); }
static BOOL CALLBACK MonCb(HMONITOR m, HDC, LPRECT, LPARAM l) { HMONITOR *a = (HMONITOR *)l; for (int i = 0; i < 8; i++) if (!a[i]) { a[i] = m; break; } return TRUE; }
static int Mons(HMONITOR *hm) { memset(hm, 0, 8 * sizeof(HMONITOR)); EnumDisplayMonitors(NULL, NULL, MonCb, (LPARAM)hm); int n = 0; while (n < 8 && hm[n]) n++; return n; }
static HWND ow; static BOOL ovBusy(void) { return ow != NULL; }
static void CalcCr(RECT *r) {                                // что будет снято: монитор / рабочая область / все экраны / выбранная область
    POINT pt; GetCursorPos(&pt);
    MONITORINFO mi; mi.cbSize = sizeof mi; GetMonitorInfoW(hw ? MonitorFromWindow(hw, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    *r = g_rNoTb ? mi.rcWork : mi.rcMonitor;                 // рабочая область = без панели задач
    if (g_rScr < 0) { r->left = GetSystemMetrics(SM_XVIRTUALSCREEN); r->top = GetSystemMetrics(SM_YVIRTUALSCREEN); r->right = r->left + GetSystemMetrics(SM_CXVIRTUALSCREEN); r->bottom = r->top + GetSystemMetrics(SM_CYVIRTUALSCREEN); }
    else if (g_rScr > 0) { HMONITOR hm[8]; int n = Mons(hm); if (g_rScr <= n) { MONITORINFO m2; m2.cbSize = sizeof m2; if (GetMonitorInfoW(hm[g_rScr - 1], &m2)) *r = g_rNoTb ? m2.rcWork : m2.rcMonitor; } }
    if (areaOn) *r = area;                                    // выбранная область важнее настроек монитора
}
static void Start(void) {
    if (recording || !hw || ovBusy()) return;
    CalcCr(&cr); ScanOv();
    static BOOL csInit; if (!csInit) { InitializeCriticalSection(&cs); csInit = TRUE; }
    paused = 0;
    cr.right -= (cr.right - cr.left) & 1; cr.bottom -= (cr.bottom - cr.top) & 1;     // H.264: чётные размеры
    wchar_t d[MAX_PATH]; Dir(d); CreateDirectoryW(d, NULL);
    SYSTEMTIME st; GetLocalTime(&st);
    wsprintfW(fname, L"Rec_%04d-%02d-%02d_%02d-%02d-%02d.mp4", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    lstrcpynW(path, d, MAX_PATH); Cat(path, MAX_PATH, L"\\"); Cat(path, MAX_PATH, fname);
    fps = g_rFps; stopF = 0; okF = 0; ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    th = CreateThread(NULL, 0, Worker, NULL, 0, NULL);
    if (th) WaitForSingleObject(ev, 8000);
    CloseHandle(ev); ev = NULL;
    if (!th || !okF) {
        if (th) { stopF = 1; WaitForSingleObject(th, 5000); CloseHandle(th); th = NULL; }
        DeleteFileW(path); Toast(Tr(L_REC_FAIL)); return;
    }
    recording = TRUE; Repaint();
}
static void Pause(void) {
    if (!recording) return;
    if (!paused) { pauseStart = Now(); paused = 1; }
    else { pauseAcc += Now() - pauseStart; paused = 0; }
    Repaint();
}
static void Stop(void) {
    if (!recording) return;
    if (paused) { pauseAcc += Now() - pauseStart; paused = 0; }
    stopF = 1; WaitForSingleObject(th, 20000); CloseHandle(th); th = NULL; recording = FALSE; Repaint();
    wchar_t m[160]; wsprintfW(m, Tr(L_REC_SAVED), fname); Toast(m);
}
/* --- выбор области записи: затемнение монитора, рамка мышью, Shift - пропорции экрана --- */
static LRESULT CALLBACK SelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    int W = selMon.right - selMon.left, H = selMon.bottom - selMon.top;
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR: SetCursor(LoadCursorW(NULL, IDC_CROSS)); return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        HDC md = CreateCompatibleDC(dc); HBITMAP hb = CreateCompatibleBitmap(dc, W, H); HGDIOBJ ob = SelectObject(md, hb);
        RECT all = {0, 0, W, H}; HBRUSH bk = CreateSolidBrush(RGB(0, 0, 0)); FillRect(md, &all, bk); DeleteObject(bk);
        if (selHas) {
            HBRUSH key = CreateSolidBrush(RGB(255, 0, 255)); FillRect(md, &selR, key); DeleteObject(key);     // "дырка" - прозрачный цвет окна
            HPEN pw = CreatePen(PS_INSIDEFRAME, 2, RGB(255, 255, 255)); HGDIOBJ op = SelectObject(md, pw), obr = SelectObject(md, GetStockObject(NULL_BRUSH));
            Rectangle(md, selR.left - 2, selR.top - 2, selR.right + 2, selR.bottom + 2); SelectObject(md, op); SelectObject(md, obr); DeleteObject(pw);
            wchar_t t[32]; wsprintfW(t, L"%d x %d", selR.right - selR.left, selR.bottom - selR.top);
            SetBkMode(md, TRANSPARENT); SetTextColor(md, RGB(255, 255, 255)); TextOutW(md, selR.left, selR.top > 20 ? selR.top - 20 : selR.bottom + 4, t, lstrlenW(t));
        }
        BitBlt(dc, 0, 0, W, H, md, 0, 0, SRCCOPY);
        SelectObject(md, ob); DeleteObject(hb); DeleteDC(md); EndPaint(h, &ps); return 0;
    }
    case WM_LBUTTONDOWN: selA.x = (short)LOWORD(l); selA.y = (short)HIWORD(l); selHas = FALSE; SetCapture(h); return 0;
    case WM_MOUSEMOVE:
        if (GetCapture() == h) {
            int x = (short)LOWORD(l), y = (short)HIWORD(l);
            if (x < 0) x = 0; if (y < 0) y = 0; if (x > W) x = W; if (y > H) y = H;
            int dx = x - selA.x, dy = y - selA.y, ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            if (w & MK_SHIFT) {                                          // пропорционально экрану: ширина/высота как у монитора
                int mw = ax, mh = ay; float asp = (float)W / H;
                if (ax < ay * asp) mw = (int)(ay * asp); else mh = (int)(ax / asp);
                int mxw = dx < 0 ? selA.x : W - selA.x, mxh = dy < 0 ? selA.y : H - selA.y;
                if (mw > mxw) { mw = mxw; mh = (int)(mw / asp); } if (mh > mxh) { mh = mxh; mw = (int)(mh * asp); }
                ax = mw; ay = mh; dx = dx < 0 ? -ax : ax; dy = dy < 0 ? -ay : ay;
            }
            selR.left = dx < 0 ? selA.x + dx : selA.x; selR.right = selR.left + ax; selR.top = dy < 0 ? selA.y + dy : selA.y; selR.bottom = selR.top + ay;
            selHas = TRUE; InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == h) {
            ReleaseCapture();
            if (selHas && selR.right - selR.left >= 32 && selR.bottom - selR.top >= 32) {
                area = selR; OffsetRect(&area, selMon.left, selMon.top); areaOn = TRUE;
            } else areaOn = FALSE;                                       // простой клик без рамки - сбросить область
            DestroyWindow(h);
        }
        return 0;
    case WM_KEYDOWN: case WM_RBUTTONUP: if (m == WM_RBUTTONUP || w == VK_ESCAPE) DestroyWindow(h); return 0;
    case WM_DESTROY: selw = NULL; if (hw) { ShowWindow(hw, SW_SHOWNOACTIVATE); Repaint(); } return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static void SelectArea(void) {
    if (selw || recording || !hw) return;
    MONITORINFO mi; mi.cbSize = sizeof mi; GetMonitorInfoW(MonitorFromWindow(hw, MONITOR_DEFAULTTONEAREST), &mi);
    selMon = mi.rcMonitor; selHas = FALSE;
    RegClass(L"DKRecSel", SelProc, 0);
    selw = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"DKRecSel", L"", WS_POPUP, selMon.left, selMon.top, selMon.right - selMon.left, selMon.bottom - selMon.top, NULL, NULL, g_inst, NULL);
    if (!selw) return;
    SetLayeredWindowAttributes(selw, RGB(255, 0, 255), 130, LWA_COLORKEY | LWA_ALPHA);
    ShowWindow(hw, SW_HIDE); ShowWindow(selw, SW_SHOW); ForceFg(selw); SetFocus(selw);
}
/* --- оверлеи на экране ("Показывать на экране"): не попадают в запись, клики проходят насквозь --- */
struct ShowW { HWND h; Ov o; HDC dc; HBITMAP hb; void *bits; GpBitmap *bmp; GpGraphics *g; int W, H, X, Y; };
static ShowW sw[NOVL]; static int nsw; static ULONG_PTR stok; static BOOL stokOn;
static void ShowFree(void) {
    for (int i = 0; i < nsw; i++) {
        ShowW *s = &sw[i]; HWND h = s->h; s->h = NULL; if (h) DestroyWindow(h);
        if (s->g) GdipDeleteGraphics(s->g); if (s->bmp) GdipDisposeImage((GpImage *)s->bmp);
        if (s->dc) DeleteDC(s->dc); if (s->hb) DeleteObject(s->hb);
        OvFree(&s->o);
    }
    memset(sw, 0, sizeof sw); nsw = 0;
    if (stokOn) { GdiplusShutdown(stok); stokOn = FALSE; }
}
static void ShowPaint(ShowW *s) {
    GdipGraphicsClear(s->g, 0);
    DrawOv(s->g, &s->o, 0, 0, s->o.c->sc / 100.f, 0); GdipFlush(s->g, FlushIntentionSync);
    POINT pt = {s->X, s->Y}, p0 = {0, 0}; SIZE sz = {s->W, s->H}; BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(s->h, NULL, &pt, &sz, s->dc, &p0, 0, &bf, ULW_ALPHA);
}
static LRESULT CALLBACK ShowProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_TIMER) { for (int i = 0; i < nsw; i++) if (sw[i].h == h && sw[i].g) { ShowPaint(&sw[i]); break; } return 0; }
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}
static void ShowSync(void) {                                  // пересоздаёт окна по текущим настройкам (включены только режимом "на видео")
    ShowFree();
    if (!g_rShow || ow || !hw) return;                        // только пока кнопка записи на экране
    ScanOv();
    GdiplusStartupInput in; if (GdiplusStartup(&stok, &in, NULL) != Ok) return;
    stokOn = TRUE;
    RECT r; CalcCr(&r);
    RegClass(L"DKRecShow", ShowProc, 0);
    HDC sd = GetDC(NULL);
    for (int i = 0; i < npre && nsw < NOVL; i++) {
        OvCfg *c = OvCfgGet(pre[i].stem, FALSE); if (!c || c->mode != 1) continue;
        ShowW *s = &sw[nsw]; memset(s, 0, sizeof *s);
        if (!OvLoad(&pre[i], c, &s->o)) continue;
        s->W = (int)(s->o.cw * c->sc / 100.f + 0.5f); s->H = (int)(s->o.ch * c->sc / 100.f + 0.5f); if (s->W < 8) s->W = 8; if (s->H < 8) s->H = 8;
        s->X = r.left + MulDiv(r.right - r.left, c->x, 1000); s->Y = r.top + MulDiv(r.bottom - r.top, c->y, 1000);
        if (!MkDib(sd, s->W, s->H, &s->dc, &s->hb, &s->bits) || !MkGp(s->bits, s->W, s->H, PixelFormat32bppPARGB, &s->bmp, &s->g)) { nsw++; continue; }
        s->h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"DKRecShow", L"", WS_POPUP, s->X, s->Y, s->W, s->H, NULL, NULL, g_inst, NULL);
        nsw++;
        if (!s->h) continue;
        SetWindowDisplayAffinity(s->h, 0x11);                  // WDA_EXCLUDEFROMCAPTURE: экранная копия в видео не попадает
        ShowPaint(s); ShowWindow(s->h, SW_SHOWNOACTIVATE); SetTimer(s->h, 1, 33, NULL);
    }
    ReleaseDC(NULL, sd);
}
/* --- настройка положения и масштаба: все включённые оверлеи сразу, у каждого своё окно; тянуть - перенос, углы / колесо - масштаб --- */
struct OvWin { HWND h; Ov o; OvCfg *c; int x, y; float sc; };
static OvWin ocw[NOVL]; static int ncw; static RECT ocr; static ULONG_PTR otok; static int ocorner; static POINT odn; static RECT orc0;
static void OvPaint(int i) {
    OvWin *z = &ocw[i];
    int W = (int)(z->o.cw * z->sc + 0.5f), H = (int)(z->o.ch * z->sc + 0.5f); if (W < 40) W = 40; if (H < 40) H = 40;
    HDC sd = GetDC(NULL), md = NULL; HBITMAP hb = NULL; void *bits = NULL; GpBitmap *bmp = NULL; GpGraphics *g = NULL;
    if (MkDib(sd, W, H, &md, &hb, &bits) && MkGp(bits, W, H, PixelFormat32bppPARGB, &bmp, &g)) {
        GdipGraphicsClear(g, 0x70101820);                                    // полупрозрачная подложка: за неё можно схватить
        DrawOv(g, &z->o, 0, 0, z->sc, 2);                                    // все клавиши показаны нажатыми - видна вся раскладка
        GpPen *pen = NULL; GpSolidFill *br = NULL, *tb = NULL, *lb = NULL;
        GdipCreatePen1(z->c->mode == 2 ? 0xFF34C759 : 0xFFFFFFFF, 2, UnitPixel, &pen); GdipCreateSolidFill(0xFFFF3B30, &br);
        GdipDrawRectangleI(g, pen, 1, 1, W - 3, H - 3);
        GdipFillRectangleI(g, br, 0, 0, 12, 12); GdipFillRectangleI(g, br, W - 12, 0, 12, 12); GdipFillRectangleI(g, br, 0, H - 12, 12, 12); GdipFillRectangleI(g, br, W - 12, H - 12, 12, 12);
        wchar_t nm[100]; if (z->c->mode == 2) wsprintfW(nm, L"%s [%s]", z->c->stem, Tr(L_REC_OV_TAG)); else lstrcpynW(nm, z->c->stem, 100);     // подпись: чей это оверлей
        GpFontFamily *ff = NULL; GpFont *f = NULL;
        GdipCreateFontFamilyFromName(L"Segoe UI", NULL, &ff);
        if (ff) GdipCreateFont(ff, 14, 1, UnitPixel, &f);
        if (f) {
            GdipCreateSolidFill(0xFFFFFFFF, &tb); GdipCreateSolidFill(0xD0101820, &lb);
            RectF lay(0, 0, 4000, 200), bb; GdipMeasureString(g, nm, -1, f, &lay, NULL, &bb, NULL, NULL);
            GdipFillRectangleI(g, lb, 16, 14, (int)bb.Width + 6, (int)bb.Height + 2);
            RectF tr(19, 15, bb.Width + 4, bb.Height); GdipDrawString(g, nm, -1, f, &tr, NULL, tb);
            GdipDeleteFont(f);
        }
        if (ff) GdipDeleteFontFamily(ff);
        if (tb) GdipDeleteBrush(tb); if (lb) GdipDeleteBrush(lb);
        GdipDeletePen(pen); GdipDeleteBrush(br); GdipFlush(g, FlushIntentionSync);
        POINT pt = {z->x, z->y}, ps0 = {0, 0}; SIZE sz = {W, H}; BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(z->h, sd, &pt, &sz, md, &ps0, 0, &bf, ULW_ALPHA);
    }
    if (g) GdipDeleteGraphics(g); if (bmp) GdipDisposeImage((GpImage *)bmp);
    if (md) DeleteDC(md); if (hb) DeleteObject(hb); ReleaseDC(NULL, sd);
}
static void OvSave(void) {
    int W = ocr.right - ocr.left, H = ocr.bottom - ocr.top; if (W < 1 || H < 1) return;
    for (int i = 0; i < ncw; i++) {
        OvWin *z = &ocw[i];
        z->c->x = MulDiv(z->x - ocr.left, 1000, W); z->c->y = MulDiv(z->y - ocr.top, 1000, H); z->c->sc = (int)(z->sc * 100 + 0.5f);
        OvCfgSave(z->c);
    }
}
static void OvClose(void) {
    for (int i = 0; i < ncw; i++) { HWND h = ocw[i].h; ocw[i].h = NULL; if (h) DestroyWindow(h); OvFree(&ocw[i].o); }
    ncw = 0; ow = NULL; GdiplusShutdown(otok);
    if (hw) { ShowWindow(hw, SW_SHOWNOACTIVATE); Repaint(); }
    ShowSync();
}
static LRESULT CALLBACK OvProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    int i = (int)GetWindowLongPtrW(h, GWLP_USERDATA) - 1;
    if (i < 0 || i >= ncw || ocw[i].h != h) return DefWindowProcW(h, m, w, l);
    OvWin *z = &ocw[i];
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: {
        RECT r; GetClientRect(h, &r); int x = (short)LOWORD(l), y = (short)HIWORD(l), W = r.right, H = r.bottom;
        ocorner = 0;
        if (x < 16 && y < 16) ocorner = 1; else if (x > W - 16 && y < 16) ocorner = 2; else if (x < 16 && y > H - 16) ocorner = 3; else if (x > W - 16 && y > H - 16) ocorner = 4;
        SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);     // выбранный оверлей наверх - видно, что кого перекрывает
        GetCursorPos(&odn); GetWindowRect(h, &orc0); SetCapture(h); return 0;
    }
    case WM_MOUSEMOVE:
        if (GetCapture() == h) {
            POINT p; GetCursorPos(&p);
            if (!ocorner) { z->x = orc0.left + p.x - odn.x; z->y = orc0.top + p.y - odn.y; SetWindowPos(h, NULL, z->x, z->y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE); }
            else {
                BOOL lf = ocorner == 1 || ocorner == 3, tp = ocorner == 1 || ocorner == 2;
                int ax = lf ? orc0.right : orc0.left, ay = tp ? orc0.bottom : orc0.top;
                int dx = p.x > ax ? p.x - ax : ax - p.x, dy = p.y > ay ? p.y - ay : ay - p.y;
                float s1 = (float)dx / z->o.cw, s2 = (float)dy / z->o.ch, s = s1 > s2 ? s1 : s2;
                if (s < 0.2f) s = 0.2f; if (s > 6.f) s = 6.f;
                z->sc = s; int W = (int)(z->o.cw * s + 0.5f), H = (int)(z->o.ch * s + 0.5f);
                z->x = lf ? ax - W : ax; z->y = tp ? ay - H : ay; OvPaint(i);
            }
        }
        return 0;
    case WM_LBUTTONUP: if (GetCapture() == h) ReleaseCapture(); return 0;
    case WM_MOUSEWHEEL: {                                                     // колесо приходит окну в фокусе - берём то, что под курсором
        POINT p; GetCursorPos(&p); HWND uh = WindowFromPoint(p); int j = i;
        for (int k = 0; k < ncw; k++) if (ocw[k].h == uh) j = k;
        OvWin *q = &ocw[j]; q->sc *= (short)HIWORD(w) > 0 ? 1.05f : 1.f / 1.05f; if (q->sc < 0.2f) q->sc = 0.2f; if (q->sc > 6.f) q->sc = 6.f; OvPaint(j); return 0;
    }
    case WM_RBUTTONUP: OvSave(); OvClose(); return 0;
    case WM_KEYDOWN: if (w == VK_RETURN) { OvSave(); OvClose(); } else if (w == VK_ESCAPE) OvClose(); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static void OvConfig(void) {
    if (ow || recording) return;
    ScanOv();
    GdiplusStartupInput in;
    if (GdiplusStartup(&otok, &in, NULL) != Ok) { Toast(Tr(L_D_GDIFAIL)); return; }
    ShowFree(); CalcCr(&ocr); ncw = 0; memset(ocw, 0, sizeof ocw);
    RegClass(L"DKRecOv", OvProc, 0);
    for (int i = 0; i < npre && ncw < NOVL; i++) {
        OvCfg *c = OvCfgGet(pre[i].stem, FALSE); if (!c || !c->mode) continue;
        OvWin *z = &ocw[ncw];
        if (!OvLoad(&pre[i], c, &z->o)) continue;
        z->c = c; z->sc = c->sc / 100.f; z->x = ocr.left + MulDiv(ocr.right - ocr.left, c->x, 1000); z->y = ocr.top + MulDiv(ocr.bottom - ocr.top, c->y, 1000);
        z->h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"DKRecOv", L"", WS_POPUP, z->x, z->y, 100, 100, NULL, NULL, g_inst, NULL);
        if (!z->h) { OvFree(&z->o); continue; }
        SetWindowLongPtrW(z->h, GWLP_USERDATA, ncw + 1); ncw++;
    }
    if (!ncw) { GdiplusShutdown(otok); ShowSync(); Toast(Tr(L_REC_OV_NOSEL)); return; }
    ow = ocw[0].h;
    if (hw) ShowWindow(hw, SW_HIDE);
    for (int i = 0; i < ncw; i++) { OvPaint(i); ShowWindow(ocw[i].h, SW_SHOW); }
    ForceFg(ocw[ncw - 1].h); SetFocus(ocw[ncw - 1].h); Toast(Tr(L_REC_OV_HINT));
}
static void SavePos(void) {
    RECT r; if (!hw || !GetWindowRect(hw, &r)) return;
    wchar_t v[32]; wsprintfW(v, L"%d,%d", r.left, r.top); IniSet(L"Record", L"Pos", v); g_rPos[0] = r.left; g_rPos[1] = r.top;
}
static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT r; GetClientRect(h, &r); int S = r.right;
        HBRUSH bg = CreateSolidBrush(RGB(32, 33, 36)); FillRect(dc, &r, bg); DeleteObject(bg);
        HPEN pw = CreatePen(PS_SOLID, 2, RGB(255, 255, 255)); HBRUSH red = CreateSolidBrush(RGB(255, 59, 48));
        HGDIOBJ op = SelectObject(dc, pw), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, 2, 2, S - 2, S - 2);
        SelectObject(dc, GetStockObject(NULL_PEN)); SelectObject(dc, red);
        if (recording && paused) { RoundRect(dc, S * 32 / 100, S * 30 / 100, S * 45 / 100, S * 70 / 100 + 1, 3, 3); RoundRect(dc, S * 55 / 100, S * 30 / 100, S * 68 / 100 + 1, S * 70 / 100 + 1, 3, 3); }
        else if (recording) RoundRect(dc, S * 31 / 100, S * 31 / 100, S * 69 / 100 + 1, S * 69 / 100 + 1, S / 10, S / 10);
        else Ellipse(dc, S * 27 / 100, S * 27 / 100, S * 73 / 100 + 1, S * 73 / 100 + 1);
        SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pw); DeleteObject(red);
        if (areaOn && !recording) { HPEN pa = CreatePen(PS_SOLID, 1, RGB(255, 255, 255)); HGDIOBJ o1 = SelectObject(dc, pa), o2 = SelectObject(dc, GetStockObject(NULL_BRUSH)); Rectangle(dc, S * 62 / 100, S * 62 / 100, S * 86 / 100, S * 82 / 100); SelectObject(dc, o1); SelectObject(dc, o2); DeleteObject(pa); }
        EndPaint(h, &ps); return 0;
    }
    case WM_LBUTTONDOWN: { GetCursorPos(&dn); RECT r; GetWindowRect(h, &r); wp.x = r.left; wp.y = r.top; moved = FALSE; SetCapture(h); return 0; }
    case WM_MOUSEMOVE:
        if (GetCapture() == h) {
            POINT p; GetCursorPos(&p);
            if (!moved && (p.x - dn.x) * (p.x - dn.x) + (p.y - dn.y) * (p.y - dn.y) > 36) moved = TRUE;
            if (moved) SetWindowPos(h, NULL, wp.x + p.x - dn.x, wp.y + p.y - dn.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == h) {
            ReleaseCapture();
            if (moved) SavePos(); else if (recording) { BOOL c = (w & MK_CONTROL) != 0; if (paused ? c : !c) Stop(); else Pause(); }
            else if (w & MK_SHIFT) SelectArea(); else Start();
        }
        return 0;
    case WM_RBUTTONUP: if (!recording) DestroyWindow(h); return 0;
    case WM_DESTROY: Stop(); SavePos(); hw = NULL; ShowFree(); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static void Open(void) {
    if (recording) { Stop(); return; }
    if (hw) { Start(); return; }
    POINT pt; GetCursorPos(&pt);
    MONITORINFO mi; mi.cbSize = sizeof mi; GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    int S = MulDiv(48, GetDpiForSystem(), 96), x = mi.rcMonitor.right - S - S / 2, y = mi.rcMonitor.top + S / 2;
    POINT sp = {g_rPos[0], g_rPos[1]};
    if (g_rPos[0] != -99999 && MonitorFromPoint(sp, MONITOR_DEFAULTTONULL)) { x = g_rPos[0]; y = g_rPos[1]; }
    RegClass(L"DKRec", Proc, 0);
    hw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"DKRec", L"", WS_POPUP, x, y, S, S, NULL, NULL, g_inst, NULL);
    if (!hw) return;
    SetWindowRgn(hw, CreateEllipticRgn(0, 0, S + 1, S + 1), TRUE);
    SetWindowDisplayAffinity(hw, 0x11);                      // WDA_EXCLUDEFROMCAPTURE: кнопка не попадает в видео
    ShowWindow(hw, SW_SHOWNOACTIVATE);
    ShowSync();
}
static wchar_t micList[16][80];
static HMENU MicMenu(void) {
    HMENU m = CreatePopupMenu(); BOOL off = g_rMic[0] == L'-' && !g_rMic[1], def = !g_rMic[0]; int nm = 0;
    AppendMenuW(m, MF_STRING | (def ? MF_CHECKED : 0), 30, Tr(L_REC_MICDEF));
    AppendMenuW(m, MF_STRING | (off ? MF_CHECKED : 0), 31, Tr(L_REC_MICOFF));
    IMMDeviceEnumerator *en = NULL; IMMDeviceCollection *col = NULL; UINT n = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void **)&en))) {
        if (SUCCEEDED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col))) {
            col->GetCount(&n); if (n) AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            for (UINT i = 0; i < n && nm < 16; i++) {
                IMMDevice *d = NULL; IPropertyStore *ps = NULL; PROPVARIANT pv; PropVariantInit(&pv);
                if (SUCCEEDED(col->Item(i, &d)) && SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)) && SUCCEEDED(ps->GetValue(kFriendly, &pv)) && pv.vt == VT_LPWSTR) {
                    lstrcpynW(micList[nm], pv.pwszVal, 80);
                    wchar_t e[170]; int o = 0; for (int j = 0; micList[nm][j] && o < 160; j++) { if (micList[nm][j] == L'&') e[o++] = L'&'; e[o++] = micList[nm][j]; } e[o] = 0;
                    AppendMenuW(m, MF_STRING | (!lstrcmpW(g_rMic, micList[nm]) ? MF_CHECKED : 0), 32 + nm, e); nm++;
                }
                PropVariantClear(&pv); if (ps) ps->Release(); if (d) d->Release();
            }
            col->Release();
        }
        en->Release();
    }
    return m;
}
static HMENU OvMenu(void) {
    HMENU m = CreatePopupMenu(); ScanOv(); BOOL done[24]; memset(done, 0, sizeof done);
    if (!npre) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, Tr(L_REC_OV_NONE));
    for (int g = 0; g < npre; g++) {
        if (done[g]) continue;
        HMENU gm = pre[g].grp[0] ? CreatePopupMenu() : m;                 // пресеты из подпапки - в своё подменю
        for (int i = g; i < npre; i++) {
            if (done[i] || lstrcmpiW(pre[i].grp, pre[g].grp)) continue;
            done[i] = TRUE;
            OvCfg *c = OvCfgGet(pre[i].stem, FALSE); int md = c ? c->mode : 0; HMENU s = CreatePopupMenu();
            const wchar_t *nm[3] = {Tr(L_REC_OV_OFF), Tr(L_REC_OV_VID), Tr(L_REC_OV_FILE)};
            for (int j = 0; j < 3; j++) AppendMenuW(s, MF_STRING | (md == j ? MF_CHECKED : 0), 300 + i * 8 + j, nm[j]);
            wchar_t e[140]; int o = 0; for (int j = 0; pre[i].stem[j] && o < 130; j++) { if (pre[i].stem[j] == L'&') e[o++] = L'&'; e[o++] = pre[i].stem[j]; } e[o] = 0;
            if (md) { e[o++] = L' '; e[o++] = 0x2713; e[o] = 0; }               // галочка у включённых
            AppendMenuW(gm, MF_POPUP, (UINT_PTR)s, e);
        }
        if (gm != m) {
            wchar_t e[100]; int o = 0; for (int j = 0; pre[g].grp[j] && o < 90; j++) { if (pre[g].grp[j] == L'&') e[o++] = L'&'; e[o++] = pre[g].grp[j]; } e[o] = 0;
            AppendMenuW(m, MF_POPUP, (UINT_PTR)gm, e);
        }
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 63, Tr(L_REC_OV_CFG));
    AppendMenuW(m, MF_STRING | (g_rShow ? MF_CHECKED : 0), 62, Tr(L_REC_OV_SHOW));
    AppendMenuW(m, MF_STRING | (g_rPwd ? MF_CHECKED : 0), 60, Tr(L_REC_OV_PWD));
    AppendMenuW(m, MF_STRING, 61, Tr(L_REC_OV_DIR));
    return m;
}
static HMENU Menu(void) {
    HMENU m = CreatePopupMenu(); wchar_t t[64];
    AppendMenuW(m, MF_STRING | (g_rNoTb ? MF_CHECKED : 0), 20, Tr(L_REC_NOTB));
    AppendMenuW(m, MF_STRING | (g_rCur ? MF_CHECKED : 0), 21, Tr(L_REC_CUR));
    AppendMenuW(m, MF_STRING | (g_rScr == 0 ? MF_CHECKED : 0), 27, Tr(L_REC_BTN));
    { HMONITOR hm[8]; int n = Mons(hm);
      for (int i = 0; i < n; i++) {
          MONITORINFO m2; m2.cbSize = sizeof m2; wchar_t e[80]; if (!GetMonitorInfoW(hm[i], &m2)) continue;
          wsprintfW(e, Tr(L_REC_MON), i + 1, m2.rcMonitor.right - m2.rcMonitor.left, m2.rcMonitor.bottom - m2.rcMonitor.top);
          AppendMenuW(m, MF_STRING | (g_rScr == i + 1 ? MF_CHECKED : 0), 50 + i, e);
      } }
    AppendMenuW(m, MF_STRING | (g_rScr < 0 ? MF_CHECKED : 0), 26, Tr(L_REC_ALL));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)MicMenu(), Tr(L_REC_MIC));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)OvMenu(), Tr(L_REC_OV));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    static const int f[3] = {15, 30, 60};
    for (int i = 0; i < 3; i++) { wsprintfW(t, Tr(L_REC_FPS), f[i]); AppendMenuW(m, MF_STRING | (g_rFps == f[i] ? MF_CHECKED : 0), 22 + i, t); }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 25, Tr(L_REC_DIR));
    return m;
}
static void Cmd(int c) {
    wchar_t v[8];
    if (c == 20) { g_rNoTb = !g_rNoTb; IniSet(L"Record", L"NoTaskbar", g_rNoTb ? L"1" : L"0"); }
    else if (c == 21) { g_rCur = !g_rCur; IniSet(L"Record", L"Cursor", g_rCur ? L"1" : L"0"); }
    else if (c >= 22 && c <= 24) { g_rFps = c == 22 ? 15 : c == 23 ? 30 : 60; wsprintfW(v, L"%d", g_rFps); IniSet(L"Record", L"Fps", v); }
    else if (c == 26 || c == 27 || (c >= 50 && c < 58)) {
        g_rScr = c == 26 ? -1 : c == 27 ? 0 : c - 49; wsprintfW(v, L"%d", g_rScr); IniSet(L"Record", L"Screen", c == 26 ? L"all" : v);
    }
    else if (c >= 30 && c < 48) {
        if (c == 30) g_rMic[0] = 0; else if (c == 31) { g_rMic[0] = L'-'; g_rMic[1] = 0; } else lstrcpynW(g_rMic, micList[c - 32], 80);
        IniSet(L"Record", L"Mic", g_rMic);
    }
    else if (c == 60) { g_rPwd = !g_rPwd; IniSet(L"Overlay", L"HidePasswords", g_rPwd ? L"1" : L"0"); }
    else if (c == 62) { g_rShow = !g_rShow; IniSet(L"Overlay", L"Show", g_rShow ? L"1" : L"0"); ShowSync(); }
    else if (c == 63) OvConfig();
    else if (c == 61) { wchar_t d[MAX_PATH]; lstrcpynW(d, g_dir, MAX_PATH); Cat(d, MAX_PATH, L"\\overlays"); CreateDirectoryW(d, NULL); ShellExecuteW(NULL, L"open", d, NULL, NULL, SW_SHOWNORMAL); }
    else if (c >= 300 && c < 500) {
        int i = (c - 300) / 8, j = (c - 300) % 8;
        if (i < npre && j < 3) { OvCfg *g = OvCfgGet(pre[i].stem, TRUE); if (g) { g->mode = j; OvCfgSave(g); ShowSync(); if (j) Toast(Tr(L_REC_OV_WARN)); } }
    }
    else if (c == 25) { wchar_t d[MAX_PATH]; Dir(d); CreateDirectoryW(d, NULL); ShellExecuteW(NULL, L"open", d, NULL, NULL, SW_SHOWNORMAL); }
}
}   // namespace RecNs
static void RecOpen(void) { RecNs::Open(); }
static void RecShowSync(void) { RecNs::ShowSync(); }

static int g_nav[8], g_navN;                                  // путь к пункту: номера (без разделителей) по уровням
static BOOL FindPath(HMENU m, UINT id, int depth) {            // ищет пункт id в дереве меню
    int n = GetMenuItemCount(m), k = 0;
    for (int i = 0; i < n && depth < 8; i++) {
        MENUITEMINFOW mi; memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi; mi.fMask = MIIM_ID | MIIM_SUBMENU | MIIM_FTYPE;
        if (!GetMenuItemInfoW(m, i, TRUE, &mi)) continue;
        if (mi.fType & MFT_SEPARATOR) continue;
        g_nav[depth] = k++;
        if (mi.hSubMenu) { if (FindPath(mi.hSubMenu, id, depth + 1)) return TRUE; }
        else if (mi.wID == id) { g_navN = depth + 1; return TRUE; }
    }
    return FALSE;
}
static void NavKey(WORD vk) {
    keybd_event((BYTE)vk, 0, KEYEVENTF_EXTENDEDKEY, 0);
    keybd_event((BYTE)vk, 0, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, 0);
}
static void NavRun(void) {                                      // клавишами проходим к пункту, как после ручного открытия
    for (int d = 0; d < g_navN; d++) {
        if (d) NavKey(VK_RIGHT);
        for (int i = 0; i < g_nav[d] + (d == 0); i++) NavKey(VK_DOWN);
    }
}
static BOOL KeepMenu(int c) {                                   // настройки: меню остаётся открытым после выбора
    return (c >= 20 && c < 58 && c != 25) || c == 60 || c == 62 || (c >= 300 && c < 500 && (c - 300) % 8 < 4) || (c >= TM_ID + 12 && c < TM_ID + 40);
}
static void TrayMenuAt(POINT pt, int again);
static void TrayMenu(void) { POINT pt; GetCursorPos(&pt); TrayMenuAt(pt, 0); }
static void TrayMenuAt(POINT pt, int again) {
    HMENU m = CreatePopupMenu(), hm = NULL;
    AddHk(m, MF_STRING, 1, Tr(L_TRAY_BUF), A_BUFFER);
    AddHk(m, g_histOn ? MF_STRING : MF_STRING | MF_GRAYED, 2, Tr(L_TRAY_HIST), A_HIST);
    AddHk(m, MF_STRING, 3, Tr(L_TRAY_LAUNCH), A_LAUNCH);
    AddHk(m, MF_STRING, 8, Tr(L_TRAY_SCR), A_SCRIPTS);
    AddHk(m, MF_STRING, 4, Tr(L_TRAY_CAM), A_WEBCAM);
    AddHk(m, MF_STRING, 5, Tr(L_TRAY_DRAW), A_DRAW);
    AddHk(m, MF_STRING, 6, Tr(L_TRAY_PICK), A_PICKER);
    AddHk(m, MF_STRING, 7, RecNs::recording ? Tr(L_TRAY_RECSTOP) : Tr(L_TRAY_REC), A_RECORD);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)RecNs::Menu(), Tr(L_TRAY_RECSET));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)TmMenu(), Tr(L_TRAY_TIMER));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AddHk(m, MF_STRING | (g_histOn ? MF_CHECKED : 0), 10, Tr(L_TRAY_HISTON), A_HIST);
    AppendMenuW(m, MF_STRING | (AutoGet() ? MF_CHECKED : 0), 11, Tr(L_TRAY_AUTO));
    AddHk(m, MF_STRING | (g_paused ? MF_CHECKED : 0), 15, Tr(L_TRAY_PAUSE), A_PAUSE);
    if (!g_elev) AppendMenuW(m, MF_STRING, 17, Tr(L_TRAY_ELEV));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    for (int i = g_nhid - 1; i >= 0; i--) if (!IsWindow(g_hid[i])) { memmove(g_hid + i, g_hid + i + 1, (g_nhid - i - 1) * sizeof(HWND)); g_nhid--; }
    if (g_nhid) {
        hm = CreatePopupMenu();
        for (int i = 0; i < g_nhid; i++) {
            wchar_t t[64], e[140]; GetWindowTextW(g_hid[i], t, 50);
            int o = 0; for (int j = 0; t[j] && o < 130; j++) { if (t[j] == L'&') e[o++] = L'&'; e[o++] = t[j]; }
            e[o] = 0;
            AppendMenuW(hm, MF_STRING, 200 + i, *e ? e : Tr(L_TRAY_NONAME));
        }
        AppendMenuW(m, MF_POPUP, (UINT_PTR)hm, Tr(L_TRAY_HIDDEN));
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    }
    AppendMenuW(m, MF_STRING, 12, Tr(L_TRAY_INI));
    AppendMenuW(m, MF_STRING, 13, Tr(L_TRAY_MEM));
    AppendMenuW(m, MF_STRING, 14, Tr(L_TRAY_RELOAD));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 16, Tr(L_TRAY_EXIT));
    SetForegroundWindow(g_hwnd);
    g_navN = 0;
    if (again && FindPath(m, again, 0) && g_navN) SetTimer(g_hwnd, TID_NAV, 40, NULL);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, NULL);
    KillTimer(g_hwnd, TID_NAV);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (KeepMenu(c)) {
        if (c >= TM_ID) TmCommand(c); else RecNs::Cmd(c);
        TrayMenuAt(pt, c);
        return;
    }
    switch (c) {
    case 1: PopupOpen(PM_BUF); break;
    case 2: PopupOpen(PM_HIST); break;
    case 3: PopupOpen(PM_LAUNCH); break;
    case 8: PopupOpen(PM_SCRIPTS); break;
    case 4: CamOpen(); break;
    case 5: Sleep(150); DrawOpen(); break;      // пауза: меню успевает исчезнуть с экрана
    case 6: Sleep(150); PickerOpen(); break;
    case 7: Sleep(150); RecOpen(); break;
    case 10: SetHistory(!g_histOn); break;
    case 11: AutoSet(!AutoGet()); break;
    case 15: InterlockedExchange(&g_paused, !g_paused); break;
    case 17: RestartElevated(); break;
    case 12: ShellExecuteW(NULL, NULL, L"notepad.exe", g_ini, NULL, SW_SHOWNORMAL); break;
    case 13: ShellExecuteW(NULL, L"open", g_memDir, NULL, NULL, SW_SHOWNORMAL); break;
    case 14: Reload(); break;
    case 16: DestroyWindow(g_hwnd); break;
    default:
        if ((c >= 20 && c < 70) || (c >= 300 && c < 500)) RecNs::Cmd(c);
        else if (c >= TM_ID && c < TM_ID + 80) TmCommand(c);
        else if (c >= 200 && c < 200 + 32) ShowHidden(c - 200);
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
        else if (w == TID_NAV) { KillTimer(h, TID_NAV); NavRun(); }
        return 0;
    case WM_CLIPBOARDUPDATE: OnClip(); return 0;
    case WM_APP_RENDERED: if (g_psPending) SetTimer(g_hwnd, TID_RESTORE, g_pasteDelay, NULL); return 0;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION: if (w) { for (int i = 0; i < g_nhid; i++) if (IsWindow(g_hid[i])) ShowWindow(g_hid[i], SW_SHOW); } return 0;
    case WM_DESTROY: if (RecNs::hw) DestroyWindow(RecNs::hw); PostQuitMessage(0); return 0;     // запись корректно дописывается
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
    RecShowSync();
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
