// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 MorpheusUK. All Rights Reserved.

#include "mrss/engine.hpp"
#include "mrss/graphics.hpp"
#include "mrss/version.hpp"
#include <Darkmodelib.h>
#include <algorithm>
#include <atomic>
#include <commctrl.h>
#include <commdlg.h>
#include <fstream>
#include <mutex>
#include <objbase.h>
#include <powrprof.h>
#include <random>
#include <set>
#include <shellapi.h>
#include <thread>
#include <windows.h>

using namespace mrss;
namespace
{
HINSTANCE instance;
constexpr UINT completed = WM_APP + 10, imageReady = WM_APP + 11, feedProgress = WM_APP + 12;
constexpr Transition randomTransitions[] = {Transition::fade,   Transition::slide, Transition::crossFade,
                                            Transition::flip,   Transition::cube,  Transition::collage,
                                            Transition::panZoom};
constexpr int randomControls[] = {44, 45, 46, 47, 75, 76, 77};
// Presentation order is independent of persisted enum values.
constexpr Transition transitionChoices[] = {Transition::none,      Transition::fade,    Transition::slide,
                                            Transition::crossFade, Transition::flip,    Transition::random,
                                            Transition::cube,      Transition::collage, Transition::panZoom};
int scale(HWND h, int value)
{
    return MulDiv(value, static_cast<int>(GetDpiForWindow(h)), 96);
}
HWND control(HWND h, int id)
{
    return GetDlgItem(h, id);
}
std::wstring value(HWND h, int id)
{
    auto c = control(h, id);
    const int n = GetWindowTextLengthW(c);
    std::wstring s(static_cast<std::size_t>(n) + 1, L'\0');
    GetWindowTextW(c, s.data(), n + 1);
    s.resize(n);
    return s;
}
void set(HWND h, int id, std::wstring_view s)
{
    SetWindowTextW(control(h, id), std::wstring(s).c_str());
}
void check(HWND h, int id, bool v)
{
    SendDlgItemMessageW(h, id, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0);
}
bool checked(HWND h, int id)
{
    return SendDlgItemMessageW(h, id, BM_GETCHECK, 0, 0) == BST_CHECKED;
}
void select(HWND h, int id, int v)
{
    SendDlgItemMessageW(h, id, CB_SETCURSEL, v, 0);
}
int selected(HWND h, int id)
{
    return static_cast<int>(SendDlgItemMessageW(h, id, CB_GETCURSEL, 0, 0));
}
unsigned number(HWND h, int id, unsigned min, unsigned max)
{
    auto s = value(h, id);
    std::size_t used = 0;
    unsigned long v = 0;
    try
    {
        v = std::stoul(s, &used);
    }
    catch (...)
    {
        throw std::runtime_error("Enter a valid number");
    }
    if (used != s.size() || v < min || v > max)
        throw std::runtime_error("Number must be between " + std::to_string(min) + " and " +
                                 std::to_string(max));
    return static_cast<unsigned>(v);
}
std::vector<std::wstring> lines(std::wstring_view s)
{
    std::vector<std::wstring> r;
    std::size_t start = 0;
    while (start < s.size())
    {
        auto end = s.find_first_of(L"\r\n", start);
        if (end == s.npos)
            end = s.size();
        if (end > start)
            r.emplace_back(s.substr(start, end - start));
        start = end + 1;
    }
    return r;
}
std::wstring join(const std::vector<std::wstring> &list)
{
    std::wstring r;
    for (const auto &s : list)
    {
        if (!r.empty())
            r += L"\r\n";
        r += s;
    }
    return r;
}
void error(HWND h, const std::exception &e)
{
    dmlib::darkMessageBoxW(h, wide(e.what()).c_str(), L"mRSS Screen Saver", MB_OK | MB_ICONERROR);
}
bool ask(HWND h, const wchar_t *s)
{
    return dmlib::darkMessageBoxW(h, s, L"mRSS Screen Saver", MB_YESNO | MB_ICONQUESTION) == IDYES;
}
void showAbout(HWND h)
{
    dmlib::darkMessageBoxW(
        h,
        L"mRSS Screen Saver\nVersion " MRSS_APP_VERSION_W L"\n\n\u00A9 2026 MorpheusUK. All Rights Reserved."
        L"\n\nLicensed under the GNU General Public License, version 3.0 (GPL-3.0-only)."
        L"\nThis software comes with no warranty. See LICENSE.txt in the installation folder."
        L"\n\nIncludes darkmodelib, PCRE2 and nlohmann/json under their respective licenses.",
        L"About mRSS Screen Saver", MB_OK | MB_ICONINFORMATION);
}
void theme(HWND h, unsigned choice)
{
    HIGHCONTRASTW hc{sizeof(hc)};
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0);
    dmlib::setDarkModeConfigEx((hc.dwFlags & HCF_HIGHCONTRASTON) ? 3
                               : choice == 0                     ? (dmlib::isDarkModeReg() ? 1 : 0)
                               : choice == 2                     ? 1
                                                                 : 0);
    dmlib::setDefaultColors(true);
    dmlib::setDarkWndNotifySafe(h);
    dmlib::setWindowEraseBgSubclass(h);
    InvalidateRect(h, nullptr, TRUE);
}
struct Layout
{
    struct Entry
    {
        HWND h;
        int x, y, w, hgt;
        bool wide = false, bottom = false;
    };
    std::vector<Entry> entries;
    HFONT font = nullptr;
    UINT fontDpi = 0;
    int baseW = 0, baseH = 0;
    ~Layout()
    {
        if (font)
            DeleteObject(font);
    }
    HWND add(HWND parent, int id, const wchar_t *cls, const wchar_t *label, DWORD style, int x, int y, int w,
             int h, bool expand = false, bool bottom = false)
    {
        if (!font)
        {
            font = CreateFontW(-scale(parent, 14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                               L"Segoe UI");
            fontDpi = GetDpiForWindow(parent);
        }
        const bool edit = wcscmp(cls, L"EDIT") == 0;
        HWND c =
            CreateWindowExW(edit ? WS_EX_CLIENTEDGE : 0, cls, label, WS_CHILD | WS_VISIBLE | style,
                            scale(parent, x), scale(parent, y), scale(parent, w), scale(parent, h), parent,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        entries.push_back({c, x, y, w, h, expand, bottom});
        return c;
    }
    void label(HWND h, const wchar_t *text, int x, int y, int w)
    {
        add(h, 0, L"STATIC", text, SS_LEFT, x, y, w, 22);
    }
    void button(HWND h, int id, const wchar_t *text, int x, int y, int w = 96, bool bottom = false)
    {
        add(h, id, L"BUTTON", text, WS_TABSTOP | BS_PUSHBUTTON, x, y, w, 28, false, bottom);
    }
    void toggle(HWND h, int id, const wchar_t *text, int x, int y, int w = 250)
    {
        add(h, id, L"BUTTON", text, WS_TABSTOP | BS_AUTOCHECKBOX, x, y, w, 24);
    }
    void edit(HWND h, int id, int x, int y, int w, int height = 25, bool multi = false, bool expand = false)
    {
        add(h, id, L"EDIT", L"",
            WS_TABSTOP | ES_AUTOHSCROLL | (multi ? (ES_MULTILINE | ES_WANTRETURN | WS_VSCROLL) : 0), x, y, w,
            height, expand);
        SendDlgItemMessageW(h, id, EM_SETLIMITTEXT, multi ? 8192 : 16384, 0);
    }
    void combo(HWND h, int id, int x, int y, int w, std::initializer_list<const wchar_t *> choices)
    {
        add(h, id, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, x, y, w, 220);
        for (const auto *s : choices)
            SendDlgItemMessageW(h, id, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        select(h, id, 0);
    }
    void resize(HWND h)
    {
        if (fontDpi != GetDpiForWindow(h))
        {
            const auto old = font;
            font = CreateFontW(-scale(h, 14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                               L"Segoe UI");
            fontDpi = GetDpiForWindow(h);
            for (const auto &entry : entries)
                SendMessageW(entry.h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            if (old)
                DeleteObject(old);
        }
        RECT r{};
        GetClientRect(h, &r);
        const int dx = r.right - scale(h, baseW), dy = r.bottom - scale(h, baseH);
        for (const auto &e : entries)
            MoveWindow(e.h, scale(h, e.x), scale(h, e.y) + (e.bottom ? dy : 0),
                       (std::max)(20, scale(h, e.w) + (e.wide ? dx : 0)), scale(h, e.hgt), TRUE);
    }
};
INT_PTR modal(HWND owner, const wchar_t *title, int w, int h, DLGPROC proc, LPARAM context,
              bool resize = false)
{
    struct Template
    {
        DLGTEMPLATE dialog;
        WORD menu = 0, cls = 0, text = 0;
    } t{};
    t.dialog.style =
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | (resize ? WS_THICKFRAME | WS_MAXIMIZEBOX : 0);
    t.dialog.dwExtendedStyle = WS_EX_CONTROLPARENT;
    t.dialog.cx = static_cast<short>(w / 2);
    t.dialog.cy = static_cast<short>(h / 2);
    // Dialog text/size are set by each WM_INITDIALOG after receiving its state.
    (void)title;
    return DialogBoxIndirectParamW(instance, &t.dialog, owner, proc, context);
}
void sizeDialog(HWND h, const wchar_t *title, int w, int height)
{
    SetWindowTextW(h, title);
    SendMessageW(h, WM_SETICON, ICON_BIG,
                 reinterpret_cast<LPARAM>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                                     GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                                                     LR_SHARED)));
    SendMessageW(h, WM_SETICON, ICON_SMALL,
                 reinterpret_cast<LPARAM>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                                     GetSystemMetrics(SM_CXSMICON),
                                                     GetSystemMetrics(SM_CYSMICON), LR_SHARED)));
    RECT r{0, 0, scale(h, w), scale(h, height)};
    AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongPtrW(h, GWL_STYLE)), FALSE,
                             static_cast<DWORD>(GetWindowLongPtrW(h, GWL_EXSTYLE)), GetDpiForWindow(h));
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    SetWindowPos(h, nullptr, (work.right - r.right + r.left) / 2, (work.bottom - r.bottom + r.top) / 2,
                 r.right - r.left, r.bottom - r.top, SWP_NOZORDER);
}
std::optional<std::filesystem::path> fileDialog(HWND owner, bool save, const wchar_t *filter,
                                                const wchar_t *ext)
{
    wchar_t path[32768]{};
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = owner;
    ofn.lpstrFile = path;
    ofn.nMaxFile = 32768;
    ofn.lpstrFilter = filter;
    ofn.lpstrDefExt = ext;
    ofn.Flags =
        OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))
        return std::filesystem::path(path);
    return {};
}
void dpi(HWND h, LPARAM l)
{
    const auto *r = reinterpret_cast<RECT *>(l);
    SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

struct FilterDialog
{
    Layout ui;
    Filter filter;
    unsigned appearance = 0;
};
void filterEnable(HWND h)
{
    const auto mode = static_cast<FilterMode>(selected(h, 10));
    const bool active = mode != FilterMode::none;
    for (const int id : {11, 13, 14, 15, 16, 17, 18, 19, 20})
        EnableWindow(control(h, id), active);
    EnableWindow(control(h, 12), mode == FilterMode::regex);
}
INT_PTR CALLBACK filterProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto *s = reinterpret_cast<FilterDialog *>(GetWindowLongPtrW(h, DWLP_USER));
    try
    {
        if (m == WM_INITDIALOG)
        {
            s = reinterpret_cast<FilterDialog *>(l);
            SetWindowLongPtrW(h, DWLP_USER, l);
            sizeDialog(h, L"Advanced metadata filters", 660, 470);
            auto &u = s->ui;
            u.label(h, L"Filter mode", 18, 18, 120);
            u.combo(h, 10, 150, 16, 220, {L"None", L"Boolean (AND, OR, NOT)", L"Regular expression"});
            u.label(h, L"Expression / include regex", 18, 58, 400);
            u.edit(h, 11, 18, 82, 620, 65, true);
            u.label(h, L"Exclude regex (regex mode only)", 18, 156, 400);
            u.edit(h, 12, 18, 180, 620, 48, true);
            u.toggle(h, 13, L"Case sensitive", 18, 240);
            u.toggle(h, 14, L"Title", 18, 272, 90);
            u.toggle(h, 15, L"Description", 115, 272, 140);
            u.toggle(h, 16, L"URL", 270, 272, 90);
            u.toggle(h, 17, L"Author", 370, 272, 90);
            u.toggle(h, 18, L"Keywords", 470, 272, 130);
            u.label(h, L"Boolean precedence: NOT, then AND, then OR. Use quotes for phrases.", 18, 307, 620);
            u.label(h, L"Test metadata", 18, 336, 150);
            u.edit(h, 19, 150, 332, 488);
            u.button(h, 20, L"Test", 18, 374);
            u.label(h, L"", 125, 379, 510);
            u.button(h, IDOK, L"OK", 440, 423);
            u.button(h, IDCANCEL, L"Cancel", 542, 423);
            select(h, 10, static_cast<int>(s->filter.mode));
            set(h, 11, s->filter.expression);
            set(h, 12, s->filter.exclude);
            check(h, 13, s->filter.caseSensitive);
            for (int i = 0; i < 5; ++i)
                check(h, 14 + i, (s->filter.fields & (1u << i)) != 0);
            filterEnable(h);
            theme(h, s->appearance);
            return TRUE;
        }
        if (!s)
            return FALSE;
        if (m == WM_COMMAND)
        {
            const auto id = LOWORD(w);
            if (id == 10 && HIWORD(w) == CBN_SELCHANGE)
            {
                filterEnable(h);
                return TRUE;
            }
            if (id == IDCANCEL)
            {
                EndDialog(h, 0);
                return TRUE;
            }
            if (id == IDOK || id == 20)
            {
                Filter f;
                f.mode = static_cast<FilterMode>(selected(h, 10));
                f.expression = value(h, 11);
                f.exclude = value(h, 12);
                f.caseSensitive = checked(h, 13);
                f.fields = 0;
                for (int i = 0; i < 5; ++i)
                    if (checked(h, 14 + i))
                        f.fields |= 1u << i;
                if (!f.fields)
                {
                    if (f.mode != FilterMode::none)
                        throw std::runtime_error("Select at least one metadata field");
                    f.fields = 3;
                }
                validateFilter(f);
                if (id == 20)
                    dmlib::darkMessageBoxW(h, matchesFilter(f, value(h, 19)) ? L"Matches" : L"Does not match",
                                           L"Filter test", MB_OK);
                else
                {
                    s->filter = std::move(f);
                    EndDialog(h, 1);
                }
                return TRUE;
            }
        }
    }
    catch (const std::exception &e)
    {
        error(h, e);
    }
    return FALSE;
}
struct MetadataDialog
{
    Layout ui;
    MetadataOptions options;
    unsigned appearance = 0;
};
int CALLBACK collectFont(const LOGFONTW *font, const TEXTMETRICW *, DWORD, LPARAM data)
{
    if (font->lfFaceName[0] != L'@')
        reinterpret_cast<std::set<std::wstring> *>(data)->insert(font->lfFaceName);
    return 1;
}
void enableMetadata(HWND h)
{
    for (const int id : {12, 13, 14, 15, 16, 17})
        EnableWindow(control(h, id), checked(h, 10) || checked(h, 11));
}
INT_PTR CALLBACK metadataProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto *s = reinterpret_cast<MetadataDialog *>(GetWindowLongPtrW(h, DWLP_USER));
    try
    {
        if (m == WM_INITDIALOG)
        {
            s = reinterpret_cast<MetadataDialog *>(l);
            SetWindowLongPtrW(h, DWLP_USER, l);
            sizeDialog(h, L"Image metadata", 510, 310);
            auto &u = s->ui;
            u.toggle(h, 10, L"Show image title", 18, 18, 220);
            u.toggle(h, 11, L"Show author / credit", 250, 18, 240);
            u.label(h, L"Display for", 18, 58, 110);
            u.edit(h, 12, 130, 54, 70);
            u.label(h, L"seconds (capped to image duration)", 210, 58, 290);
            u.label(h, L"Font", 18, 100, 110);
            u.add(h, 13, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL, 130, 96,
                  360, 220);
            SendDlgItemMessageW(h, 13, CB_LIMITTEXT, 128, 0);
            std::set<std::wstring> fonts;
            LOGFONTW font{};
            font.lfCharSet = DEFAULT_CHARSET;
            const auto dc = GetDC(h);
            EnumFontFamiliesExW(dc, &font, collectFont, reinterpret_cast<LPARAM>(&fonts), 0);
            ReleaseDC(h, dc);
            for (const auto &name : fonts)
                SendDlgItemMessageW(h, 13, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
            u.label(h, L"Size", 18, 142, 110);
            u.edit(h, 14, 130, 138, 70);
            u.label(h, L"points (8–96)", 210, 142, 115);
            u.toggle(h, 16, L"Bold", 330, 138, 80);
            u.toggle(h, 17, L"Italic", 420, 138, 80);
            u.label(h, L"Position", 18, 184, 110);
            u.combo(h, 15, 130, 180, 360, {L"Top centre", L"Bottom centre"});
            u.label(h, L"White text on a translucent black panel. Missing fields are omitted.", 18, 222, 480);
            u.button(h, IDOK, L"OK", 284, 264);
            u.button(h, IDCANCEL, L"Cancel", 390, 264);
            check(h, 10, s->options.title);
            check(h, 11, s->options.author);
            set(h, 12, std::to_wstring(s->options.seconds));
            set(h, 13, s->options.font);
            set(h, 14, std::to_wstring(s->options.points));
            select(h, 15, s->options.top ? 0 : 1);
            check(h, 16, s->options.bold);
            check(h, 17, s->options.italic);
            enableMetadata(h);
            theme(h, s->appearance);
            return TRUE;
        }
        if (s && m == WM_COMMAND)
        {
            const auto id = LOWORD(w);
            if (id == 10 || id == 11)
            {
                enableMetadata(h);
                return TRUE;
            }
            if (id == IDCANCEL)
            {
                EndDialog(h, 0);
                return TRUE;
            }
            if (id == IDOK)
            {
                auto options = s->options;
                options.title = checked(h, 10);
                options.author = checked(h, 11);
                options.seconds = number(h, 12, 1, 86400);
                options.points = number(h, 14, 8, 96);
                options.font = value(h, 13);
                if (options.font.empty() || options.font.size() > 128)
                    throw std::runtime_error("Choose a font family");
                options.top = selected(h, 15) == 0;
                options.bold = checked(h, 16);
                options.italic = checked(h, 17);
                s->options = std::move(options);
                EndDialog(h, 1);
                return TRUE;
            }
        }
    }
    catch (const std::exception &e)
    {
        error(h, e);
    }
    return FALSE;
}
struct FeedDialog
{
    Layout ui;
    FeedConfig feed;
    unsigned appearance = 0;
    AgeLimit globalAge;
    AgeLimit localDraft;
    MetadataOptions globalMetadata, metadataDraft;
};
void fillAge(HWND h, const AgeLimit &age)
{
    check(h, 13, age.entireFeed);
    set(h, 14, std::to_wstring(age.value));
    select(h, 15, static_cast<int>(age.unit));
    check(h, 16, age.includeUndated);
}
AgeLimit readAge(HWND h, AgeLimit age)
{
    age.entireFeed = checked(h, 13);
    if (!age.entireFeed)
        age.value = number(h, 14, 1, 1200);
    age.unit = static_cast<AgeUnit>(selected(h, 15));
    age.includeUndated = checked(h, 16);
    return age;
}
void feedValidation(HWND h)
{
    const auto valid = validateUrl(value(h, 10), checked(h, 19));
    set(h, 24, valid.url ? L"✓ URL syntax valid" : L"⚠ " + valid.error);
}
void feedEnable(HWND h)
{
    const bool local = checked(h, 25), limited = local && !checked(h, 13);
    EnableWindow(control(h, 13), local);
    EnableWindow(control(h, 14), limited);
    EnableWindow(control(h, 15), limited);
    EnableWindow(control(h, 16), limited);
    EnableWindow(control(h, 18), !checked(h, 17));
    EnableWindow(control(h, 27), checked(h, 26));
}
INT_PTR CALLBACK feedProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto *s = reinterpret_cast<FeedDialog *>(GetWindowLongPtrW(h, DWLP_USER));
    try
    {
        if (m == WM_INITDIALOG)
        {
            s = reinterpret_cast<FeedDialog *>(l);
            SetWindowLongPtrW(h, DWLP_USER, l);
            sizeDialog(h, L"Feed settings", 680, 578);
            auto &u = s->ui;
            u.label(h, L"Feed &URL", 18, 16, 100);
            u.add(h, 24, L"STATIC", L"", SS_LEFT, 122, 16, 535, 22);
            u.edit(h, 10, 18, 42, 640);
            u.label(h, L"&Title (updated from the feed when available)", 18, 75, 620);
            u.edit(h, 11, 18, 99, 640);
            u.toggle(h, 12, L"Enable this feed", 18, 137);
            u.toggle(h, 25, L"Override global maximum image age", 18, 171, 350);
            u.toggle(h, 13, L"Entire feed (ignore age)", 390, 171, 270);
            u.label(h, L"Maximum image age", 18, 207, 170);
            u.edit(h, 14, 190, 204, 90);
            u.combo(h, 15, 292, 204, 160, {L"Minutes", L"Hours", L"Days", L"Weeks", L"Months"});
            u.toggle(h, 16, L"Include items without a publication date", 18, 240, 500);
            u.toggle(h, 17, L"Use global image display time", 18, 273, 350);
            u.edit(h, 18, 380, 273, 90);
            u.label(h, L"seconds", 480, 277, 100);
            u.toggle(h, 19, L"Allow HTTP for this feed and its images", 18, 310, 480);
            u.toggle(h, 20, L"Allow private/LAN destinations (trusted feeds only)", 18, 342, 620);
            u.label(h, L"Skip image URLs containing (one literal substring per line)", 18, 377, 620);
            u.edit(h, 21, 18, 401, 640, 58, true);
            u.toggle(h, 26, L"Override global metadata", 18, 474, 300);
            u.button(h, 27, L"Metadata…", 340, 472, 160);
            u.button(h, 22, L"Advanced filters…", 18, 522, 160);
            u.button(h, IDOK, L"OK", 460, 522);
            u.button(h, IDCANCEL, L"Cancel", 562, 522);
            set(h, 10, s->feed.url);
            set(h, 11, s->feed.title);
            const auto &p = s->feed.policy;
            check(h, 12, p.enabled);
            s->localDraft = p.overrideAge ? p.age : s->globalAge;
            check(h, 25, p.overrideAge);
            fillAge(h, p.overrideAge ? s->localDraft : s->globalAge);
            check(h, 17, !p.displaySeconds);
            set(h, 18, std::to_wstring(p.displaySeconds.value_or(10)));
            check(h, 19, p.allowHttp);
            check(h, 20, p.allowPrivateNetwork);
            set(h, 21, join(p.excludedUrlText));
            check(h, 26, s->feed.metadata.has_value());
            s->metadataDraft = s->feed.metadata.value_or(s->globalMetadata);
            feedEnable(h);
            feedValidation(h);
            theme(h, s->appearance);
            return TRUE;
        }
        if (!s)
            return FALSE;
        if (m == WM_COMMAND)
        {
            const auto id = LOWORD(w);
            if (id == 25)
            {
                if (!checked(h, 25))
                {
                    try
                    {
                        s->localDraft = readAge(h, s->localDraft);
                    }
                    catch (const std::exception &)
                    {
                    }
                    fillAge(h, s->globalAge);
                }
                else
                    fillAge(h, s->localDraft);
                feedEnable(h);
                return TRUE;
            }
            if ((id == 10 && HIWORD(w) == EN_CHANGE) || id == 19)
            {
                feedValidation(h);
                if (id == 19)
                    return TRUE;
            }
            if (id == 13 || id == 17 || id == 26)
            {
                feedEnable(h);
                return TRUE;
            }
            if (id == IDCANCEL)
            {
                EndDialog(h, 0);
                return TRUE;
            }
            if (id == 22)
            {
                FilterDialog f;
                f.appearance = s->appearance;
                f.filter = s->feed.filter;
                if (modal(h, L"", 660, 470, filterProc, reinterpret_cast<LPARAM>(&f)) == 1)
                    s->feed.filter = f.filter;
                return TRUE;
            }
            if (id == 27 && checked(h, 26))
            {
                MetadataDialog dialog;
                dialog.appearance = s->appearance;
                dialog.options = s->metadataDraft;
                if (modal(h, L"", 510, 310, metadataProc, reinterpret_cast<LPARAM>(&dialog)) == 1)
                    s->metadataDraft = std::move(dialog.options);
                theme(h, s->appearance);
                return TRUE;
            }
            if (id == IDOK)
            {
                auto f = s->feed;
                f.url = value(h, 10);
                f.title = value(h, 11);
                auto &p = f.policy;
                p.enabled = checked(h, 12);
                p.allowHttp = checked(h, 19);
                p.allowPrivateNetwork = checked(h, 20);
                const auto valid = validateUrl(f.url, p.allowHttp);
                if (!valid.url)
                    throw std::runtime_error(utf8(valid.error));
                p.overrideAge = checked(h, 25);
                p.age = p.overrideAge ? readAge(h, s->localDraft) : s->localDraft;
                if (checked(h, 17))
                    p.displaySeconds.reset();
                else
                    p.displaySeconds = number(h, 18, 1, 86400);
                p.excludedUrlText = lines(value(h, 21));
                f.metadata = checked(h, 26) ? std::optional<MetadataOptions>(s->metadataDraft) : std::nullopt;
                validateFilter(f.filter);
                s->feed = std::move(f);
                EndDialog(h, 1);
                return TRUE;
            }
        }
    }
    catch (const std::exception &e)
    {
        error(h, e);
    }
    return FALSE;
}

struct Playback;
struct Canvas
{
    Playback *owner = nullptr;
    HWND hwnd = nullptr;
    std::unique_ptr<Renderer> renderer;
    ~Canvas()
    {
        if (hwnd && IsWindow(hwnd))
            DestroyWindow(hwnd);
    }
};
void showPlayback(HWND owner, const Settings &settings, bool screen = false, HWND embed = nullptr,
                  bool liveSettings = false);
struct Playback
{
    Layout ui;
    Settings settings;
    Catalog catalog;
    HWND dialog = nullptr;
    bool screen = false, embedded = false, playing = true, loading = false, powerOff = false,
         liveSettings = false;
    std::filesystem::file_time_type settingsTime{};
    std::uint64_t generation = 0;
    std::vector<std::unique_ptr<Canvas>> canvases;
    std::jthread worker;
    std::mutex mutex;
    std::optional<Catalog> incomingCatalog;
    std::optional<Pixels> incomingPixels;
    std::wstring failure;
    std::size_t index = 0;
    Shuffle shuffle{std::random_device{}()};
    std::vector<std::size_t> balanced;
    std::size_t balancedCursor = 0;
    std::size_t randomNext(bool reset = false)
    {
        if (!settings.balanceFeeds)
        {
            if (reset)
                shuffle.reset(catalog.items.size());
            return shuffle.next().value_or(0);
        }
        if (reset || balancedCursor >= balanced.size())
        {
            std::vector<std::size_t> ids;
            for (const auto &item : catalog.items)
                ids.push_back(item.feedIndex);
            balanced = balancedOrder(ids, std::random_device{}());
            balancedCursor = 0;
            if (!reset && balanced.size() > 1 && balanced.front() == index)
                std::swap(balanced[0], balanced[1]);
        }
        return balanced.empty() ? 0 : balanced[balancedCursor++];
    }
    std::uint64_t cycle = 0;
    Transition effect = Transition::none;
    std::chrono::steady_clock::time_point transitionStart{}, visibleStart{};
    bool animating = false;
    float motionProgress = 0;
    std::chrono::steady_clock::time_point motionTick{};
    std::optional<Item> displayedItem;
    std::wstring caption;
    float metadataElapsed = 0;
    std::chrono::steady_clock::time_point metadataTick{};
    bool captionShown = false;
    POINT initial{};
    std::chrono::steady_clock::time_point launched = std::chrono::steady_clock::now();
    HPOWERNOTIFY power = nullptr;
    void updateControls(bool readingCatalog = false)
    {
        const auto count = catalog.items.size();
        for (const int id : {101, 102, 103, 104, 105})
            EnableWindow(control(dialog, id), !readingCatalog && count > 1);
        EnableWindow(control(dialog, 107), !readingCatalog && count > 0);
    }
    ~Playback()
    {
        worker.request_stop();
        if (worker.joinable())
            worker.join();
        if (power)
            UnregisterPowerSettingNotification(power);
        for (auto &c : canvases)
            if (IsWindow(c->hwnd))
                DestroyWindow(c->hwnd);
    }
    void stop()
    {
        ++generation;
        worker.request_stop();
        if (worker.joinable())
            worker.join();
        loading = false;
        std::lock_guard guard(mutex);
        incomingCatalog.reset();
        incomingPixels.reset();
        failure.clear();
    }
    void loadCatalog(bool refresh)
    {
        stop();
        loading = true;
        updateControls(true);
        const auto stamp = generation;
        set(dialog, 106, L"Reading feeds…");
        worker = std::jthread([this, refresh, stamp](std::stop_token token) {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            try
            {
                auto c = buildCatalog(settings, refresh, token);
                if (!token.stop_requested())
                {
                    std::lock_guard guard(mutex);
                    incomingCatalog = std::move(c);
                    PostMessageW(dialog, completed, static_cast<WPARAM>(stamp), 0);
                }
            }
            catch (const std::exception &e)
            {
                std::lock_guard guard(mutex);
                failure = wide(e.what());
                PostMessageW(dialog, completed, static_cast<WPARAM>(stamp), 0);
            }
            CoUninitialize();
        });
    }
    void loadImage()
    {
        if (catalog.items.empty())
            return;
        stop();
        loading = true;
        set(dialog, 106, L"Loading image…");
        const auto item = catalog.items[index];
        const auto imageIndex = index;
        const auto stamp = generation;
        worker = std::jthread([this, item, imageIndex, stamp](std::stop_token token) {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            try
            {
                auto path = imageFile(item, settings.defaults, token, embedded);
                auto pixels = decodeImage(path);
                if (!token.stop_requested())
                {
                    {
                        std::lock_guard guard(mutex);
                        incomingPixels = std::move(pixels);
                    }
                    PostMessageW(dialog, imageReady, static_cast<WPARAM>(stamp), 0);
                }
                // Prefetch compressed files only; decoding stays bounded to the
                // current and previous frames. No preview-wide eager downloads.
                if (!embedded)
                    for (unsigned n = 1; n <= settings.defaults.prefetchImages && !token.stop_requested();
                         ++n)
                    {
                        const auto next = (imageIndex + n) % catalog.items.size();
                        if (next == imageIndex)
                            break;
                        try
                        {
                            (void)imageFile(catalog.items[next], settings.defaults, token);
                        }
                        catch (...)
                        {
                        }
                    }
                trimCache(settings.defaults.cacheBytes);
            }
            catch (const std::exception &e)
            {
                if (!token.stop_requested())
                {
                    std::lock_guard guard(mutex);
                    failure = wide(e.what());
                    PostMessageW(dialog, imageReady, static_cast<WPARAM>(stamp), 0);
                }
            }
            CoUninitialize();
        });
    }
    void navigate(int direction)
    {
        if (catalog.items.empty())
            return;
        playing = false;
        set(dialog, 103, L"Play");
        if (direction == -2)
            index = 0;
        else if (direction == 2)
            index = catalog.items.size() - 1;
        else if (direction < 0)
            index = (index + catalog.items.size() - 1) % catalog.items.size();
        else
            index = (index + 1) % catalog.items.size();
        loadImage();
    }
    bool metadataVisible() const
    {
        return displayedItem && !animating && !caption.empty() &&
               metadataElapsed < effectiveMetadataSeconds(settings, displayedItem->feed);
    }
    void draw(Renderer &renderer, float progress)
    {
        captionShown = metadataVisible();
        const auto &options =
            displayedItem ? effectiveMetadata(settings, displayedItem->feed) : settings.metadata;
        renderer.draw(progress, effect, settings, motionProgress,
                      captionShown ? std::wstring_view(caption) : std::wstring_view{}, &options);
    }
    void render(float progress)
    {
        for (auto &c : canvases)
            if (c->renderer)
                draw(*c->renderer, progress);
    }
};
LRESULT CALLBACK canvasProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto *c = reinterpret_cast<Canvas *>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE)
    {
        c = static_cast<Canvas *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        c->hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(c));
    }
    try
    {
        if (c)
        {
            auto &s = *c->owner;
            if (m == WM_ERASEBKGND)
                return 1;
            if (m == WM_SIZE && c->renderer)
            {
                c->renderer->resize();
                s.render(1);
                return 0;
            }
            if (m == WM_PAINT)
            {
                PAINTSTRUCT p{};
                BeginPaint(h, &p);
                struct EndPaintScope
                {
                    HWND window;
                    PAINTSTRUCT *paint;
                    ~EndPaintScope()
                    {
                        EndPaint(window, paint);
                    }
                } end{h, &p};
                if (c->renderer)
                    s.draw(*c->renderer, s.animating ? 0.f : 1.f);
                else
                    FillRect(p.hdc, &p.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
                return 0;
            }
            if (m == WM_DESTROY && s.embedded)
            {
                PostQuitMessage(0);
                return 0;
            }
            if (m == WM_GETDLGCODE)
                return DLGC_WANTARROWS | DLGC_WANTCHARS;
            if (m == WM_LBUTTONDOWN && !s.screen)
            {
                SetFocus(h);
                return 0;
            }
            if (m == WM_TIMER && w == 8 && s.embedded)
            {
                RECT r{};
                GetClientRect(GetParent(h), &r);
                MoveWindow(h, 0, 0, r.right, r.bottom, TRUE);
                return 0;
            }
            if (s.screen && (m == WM_KEYDOWN || m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN ||
                             m == WM_MBUTTONDOWN || m == WM_MOUSEWHEEL))
            {
                PostMessageW(s.dialog, WM_CLOSE, 0, 0);
                return 0;
            }
            if (s.screen && m == WM_MOUSEMOVE &&
                std::chrono::steady_clock::now() - s.launched > std::chrono::milliseconds(500))
            {
                POINT point{};
                GetCursorPos(&point);
                if (abs(point.x - s.initial.x) > 4 || abs(point.y - s.initial.y) > 4)
                    PostMessageW(s.dialog, WM_CLOSE, 0, 0);
            }
            if (m == WM_SETCURSOR && s.screen)
            {
                SetCursor(nullptr);
                return TRUE;
            }
            if (m == WM_KEYDOWN && !s.screen)
            {
                if (w == VK_SPACE)
                    SendMessageW(s.dialog, WM_COMMAND, 103, 0);
                else if (w == VK_RIGHT)
                    SendMessageW(s.dialog, WM_COMMAND, 104, 0);
                else if (w == VK_LEFT)
                    SendMessageW(s.dialog, WM_COMMAND, 102, 0);
                else if (w == VK_HOME)
                    SendMessageW(s.dialog, WM_COMMAND, 101, 0);
                else if (w == VK_END)
                    SendMessageW(s.dialog, WM_COMMAND, 105, 0);
                else if (w == VK_ESCAPE)
                    PostMessageW(s.dialog, WM_CLOSE, 0, 0);
                return 0;
            }
        }
    }
    catch (const std::exception &e)
    {
        if (c)
        {
            set(c->owner->dialog, 106, wide(e.what()));
            c->renderer.reset();
        }
    }
    return DefWindowProcW(h, m, w, l);
}
void canvas(Playback &s, HWND parent, const RECT &r, bool popup)
{
    auto c = std::make_unique<Canvas>();
    c->owner = &s;
    c->hwnd = CreateWindowExW(popup ? WS_EX_TOPMOST : 0, L"mRSSCanvas", L"mRSS Screen Saver",
                              (popup ? WS_POPUP : WS_CHILD | WS_TABSTOP) | WS_VISIBLE, r.left, r.top,
                              r.right - r.left, r.bottom - r.top, parent, nullptr, instance, c.get());
    if (!c->hwnd)
        throw std::runtime_error("Cannot create slideshow window");
    c->renderer = std::make_unique<Renderer>(c->hwnd);
    if (s.embedded)
        SetTimer(c->hwnd, 8, 250, nullptr);
    s.canvases.push_back(std::move(c));
}
INT_PTR CALLBACK playbackProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto *s = reinterpret_cast<Playback *>(GetWindowLongPtrW(h, DWLP_USER));
    try
    {
        if (m == WM_INITDIALOG)
        {
            s = reinterpret_cast<Playback *>(l);
            s->dialog = h;
            SetWindowLongPtrW(h, DWLP_USER, l);
            GetCursorPos(&s->initial);
            if (s->screen)
            {
                SetWindowLongPtrW(h, GWL_STYLE, WS_POPUP);
                SetWindowLongPtrW(h, GWL_EXSTYLE, WS_EX_TOPMOST);
                SetWindowPos(h, HWND_TOPMOST, 0, 0, 1, 1, SWP_FRAMECHANGED);
                EnumDisplayMonitors(
                    nullptr, nullptr,
                    [](HMONITOR, HDC, LPRECT r, LPARAM p) -> BOOL {
                        auto &state = *reinterpret_cast<Playback *>(p);
                        try
                        {
                            canvas(state, state.dialog, *r, true);
                        }
                        catch (...)
                        {
                            return FALSE;
                        }
                        return TRUE;
                    },
                    reinterpret_cast<LPARAM>(s));
                if (!s->canvases.empty())
                    SetForegroundWindow(s->canvases[0]->hwnd);
            }
            else
            {
                sizeDialog(h, L"Feed preview", 980, 680);
                auto &u = s->ui;
                u.baseW = 980;
                u.baseH = 680;
                u.button(h, 101, L"Start", 18, 596, 90, true);
                u.button(h, 102, L"Previous", 116, 596, 90, true);
                u.button(h, 103, L"Pause", 214, 596, 90, true);
                u.button(h, 104, L"Next", 312, 596, 90, true);
                u.button(h, 105, L"End", 410, 596, 90, true);
                u.button(h, 107, L"Full screen", 510, 596, 115, true);
                u.button(h, IDCANCEL, L"Close", 860, 596, 96, true);
                u.add(h, 106, L"STATIC", L"", SS_LEFT, 18, 637, 940, 36, true, true);
                RECT r{0, 0, scale(h, 980), scale(h, 580)};
                canvas(*s, h, r, false);
                theme(h, s->settings.theme);
            }
            s->power =
                RegisterPowerSettingNotification(h, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);
            SetTimer(h, 1, 100, nullptr);
            s->loadCatalog(false);
            return TRUE;
        }
        if (!s)
            return FALSE;
        if (m == completed)
        {
            if (w != s->generation)
                return TRUE;
            s->loading = false;
            std::optional<Catalog> catalog;
            std::wstring failure;
            {
                std::lock_guard guard(s->mutex);
                catalog = std::move(s->incomingCatalog);
                s->incomingCatalog.reset();
                failure = std::move(s->failure);
                s->failure.clear();
            }
            if (catalog)
            {
                s->catalog = std::move(*catalog);
                s->index = s->settings.random ? s->randomNext(true) : 0;
                s->updateControls();
                if (s->catalog.items.empty())
                {
                    std::wstring message =
                        L"No eligible images. Check age, ratings, filters and feed status.";
                    for (const auto &f : s->catalog.status)
                        message += L" " + f.message;
                    set(h, 106, message);
                    s->visibleStart = std::chrono::steady_clock::now();
                }
                else
                    s->loadImage();
            }
            else
            {
                set(h, 106, failure);
                s->updateControls();
            }
            return TRUE;
        }
        if (m == imageReady)
        {
            if (w != s->generation)
                return TRUE;
            s->loading = false;
            std::optional<Pixels> pixels;
            std::wstring failure;
            {
                std::lock_guard guard(s->mutex);
                pixels = std::move(s->incomingPixels);
                s->incomingPixels.reset();
                failure = std::move(s->failure);
                s->failure.clear();
            }
            if (pixels)
            {
                for (auto &c : s->canvases)
                    if (c->renderer)
                        c->renderer->setImage(*pixels);
                s->effect = s->settings.defaults.transition;
                BOOL animations = TRUE;
                SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
                if (!animations || s->catalog.items.size() < 2)
                    s->effect = Transition::none;
                if (s->effect == Transition::random)
                {
                    std::vector<Transition> effects;
                    for (unsigned i = 0; i < std::size(randomTransitions); ++i)
                        if (s->settings.randomEffects & (1u << i))
                            effects.push_back(randomTransitions[i]);
                    s->effect =
                        effects.empty() ? Transition::none : effects[std::random_device{}() % effects.size()];
                }
                s->transitionStart = std::chrono::steady_clock::now();
                s->motionTick = s->transitionStart;
                s->motionProgress = 0;
                s->animating = s->effect != Transition::none && s->settings.defaults.transitionSeconds > 0;
                s->visibleStart =
                    s->transitionStart +
                    std::chrono::seconds(s->animating ? s->settings.defaults.transitionSeconds : 0);
                s->displayedItem = s->catalog.items[s->index];
                s->caption = metadataText(s->displayedItem->image,
                                          effectiveMetadata(s->settings, s->displayedItem->feed));
                s->metadataElapsed = 0;
                s->metadataTick = s->visibleStart;
                s->render(s->animating ? 0.f : 1.f);
                if (s->animating || (s->effect == Transition::panZoom && s->playing))
                    SetTimer(h, 2, 33, nullptr);
                const auto &item = s->catalog.items[s->index];
                set(h, 106,
                    std::to_wstring(s->index + 1) + L" / " + std::to_wstring(s->catalog.items.size()) +
                        L" • " + item.image.title + L" • " + item.feed.title);
            }
            else
            {
                set(h, 106, L"Image skipped: " + failure);
                s->visibleStart = std::chrono::steady_clock::now() -
                                  std::chrono::seconds(s->settings.defaults.displaySeconds) +
                                  std::chrono::seconds(2);
            }
            return TRUE;
        }
        if (m == WM_TIMER)
        {
            if (s->powerOff)
                return TRUE;
            const auto now = std::chrono::steady_clock::now();
            if (w == 1 && s->displayedItem)
            {
                if (s->playing && !s->animating && !s->loading && now > s->metadataTick)
                    s->metadataElapsed += std::chrono::duration<float>(now - s->metadataTick).count();
                if (now > s->metadataTick)
                    s->metadataTick = now;
                if (s->metadataVisible() != s->captionShown)
                    s->render(1);
            }
            if (w == 1 && s->liveSettings)
            {
                std::error_code ec;
                const auto time = std::filesystem::last_write_time(dataDirectory() / L"settings.json", ec);
                if (!ec && time != s->settingsTime)
                {
                    s->stop();
                    s->settingsTime = time;
                    std::wstring recovery;
                    s->settings = loadSettings(recovery);
                    s->catalog = {};
                    for (auto &c : s->canvases)
                        if (c->renderer)
                            c->renderer->clear();
                    s->animating = false;
                    KillTimer(h, 2);
                    s->render(1);
                    s->loadCatalog(false);
                    return TRUE;
                }
            }
            if (w == 2)
            {
                if (s->animating)
                {
                    const auto seconds = std::chrono::duration<float>(now - s->transitionStart).count();
                    const float p = seconds / static_cast<float>(s->settings.defaults.transitionSeconds);
                    s->render(p);
                    if (p >= 1)
                    {
                        s->animating = false;
                        if (s->effect != Transition::panZoom || !s->playing)
                            KillTimer(h, 2);
                    }
                }
                else if (s->effect == Transition::panZoom && s->playing && !s->loading &&
                         !s->catalog.items.empty())
                {
                    const auto elapsed = std::chrono::duration<float>(now - s->motionTick).count();
                    const auto duration =
                        effectiveDisplaySeconds(s->settings.defaults, s->catalog.items[s->index].feed.policy);
                    s->motionProgress = (std::min)(1.f, s->motionProgress + elapsed / duration);
                    s->render(1);
                }
                s->motionTick = now;
                return TRUE;
            }
            if (s->playing && !s->loading && !s->animating)
            {
                if (s->catalog.items.empty())
                {
                    if (now - s->visibleStart > std::chrono::minutes(5))
                        s->loadCatalog(false);
                }
                else
                {
                    const auto duration =
                        effectiveDisplaySeconds(s->settings.defaults, s->catalog.items[s->index].feed.policy);
                    if (now - s->visibleStart > std::chrono::seconds(duration))
                    {
                        if (s->catalog.items.size() > 1)
                        {
                            s->index = s->settings.random ? s->randomNext()
                                                          : (s->index + 1) % s->catalog.items.size();
                            s->loadImage();
                        }
                        else
                            s->visibleStart = now;
                    }
                }
                if (!s->loading && now - s->launched > std::chrono::hours(s->settings.defaults.refreshHours))
                {
                    s->launched = now;
                    s->loadCatalog(false);
                }
            }
            return TRUE;
        }
        if (m == WM_POWERBROADCAST)
        {
            if (w == PBT_POWERSETTINGCHANGE)
            {
                const auto *p = reinterpret_cast<POWERBROADCAST_SETTING *>(l);
                if (p->PowerSetting == GUID_CONSOLE_DISPLAY_STATE && p->DataLength >= 1)
                {
                    s->powerOff = p->Data[0] == 0;
                    if (s->powerOff)
                        s->stop();
                    else
                    {
                        s->visibleStart = std::chrono::steady_clock::now();
                        if (!s->catalog.items.empty())
                            s->loadImage();
                    }
                }
            }
            return TRUE;
        }
        if (m == WM_SIZE && !s->screen)
        {
            s->ui.resize(h);
            RECT r{};
            GetClientRect(h, &r);
            if (!s->canvases.empty())
                MoveWindow(s->canvases[0]->hwnd, 0, 0, r.right, (std::max)(1L, r.bottom - scale(h, 100)),
                           TRUE);
            return TRUE;
        }
        if (m == WM_DPICHANGED)
        {
            dpi(h, l);
            return TRUE;
        }
        if (m == WM_DISPLAYCHANGE)
        {
            if (s->screen)
            {
                s->stop();
                s->canvases.clear();
                EnumDisplayMonitors(
                    nullptr, nullptr,
                    [](HMONITOR, HDC, LPRECT r, LPARAM p) -> BOOL {
                        auto &state = *reinterpret_cast<Playback *>(p);
                        try
                        {
                            canvas(state, state.dialog, *r, true);
                        }
                        catch (...)
                        {
                            return FALSE;
                        }
                        return TRUE;
                    },
                    reinterpret_cast<LPARAM>(s));
                GetCursorPos(&s->initial);
                if (!s->catalog.items.empty())
                    s->loadImage();
                return TRUE;
            }
            for (auto &c : s->canvases)
                if (c->renderer)
                    c->renderer->resize();
            s->render(1);
            return TRUE;
        }
        if (m == WM_KEYDOWN)
        {
            if (s->screen || w == VK_ESCAPE)
            {
                PostMessageW(h, WM_CLOSE, 0, 0);
                return TRUE;
            }
            if (w == VK_SPACE)
                SendMessageW(h, WM_COMMAND, 103, 0);
            if (w == VK_RIGHT)
                SendMessageW(h, WM_COMMAND, 104, 0);
            if (w == VK_LEFT)
                SendMessageW(h, WM_COMMAND, 102, 0);
            return TRUE;
        }
        if (m == WM_COMMAND)
        {
            const auto id = LOWORD(w);
            if (auto c = control(h, id); c && !IsWindowEnabled(c))
                return TRUE;
            if (id == IDCANCEL)
            {
                PostMessageW(h, WM_CLOSE, 0, 0);
                return TRUE;
            }
            if (id == 101)
                s->navigate(-2);
            if (id == 102)
                s->navigate(-1);
            if (id == 104)
                s->navigate(1);
            if (id == 105)
                s->navigate(2);
            if (id == 103)
            {
                s->playing = !s->playing;
                s->visibleStart = std::chrono::steady_clock::now();
                s->motionTick = s->visibleStart;
                s->metadataTick = s->visibleStart;
                if (s->effect == Transition::panZoom && s->playing)
                    SetTimer(h, 2, 33, nullptr);
                else if (!s->animating)
                    KillTimer(h, 2);
                set(h, 103, s->playing ? L"Pause" : L"Play");
            }
            if (id == 107)
            {
                s->stop();
                showPlayback(h, s->settings, true);
                s->visibleStart = std::chrono::steady_clock::now();
            }
            return TRUE;
        }
        if (m == WM_CLOSE)
        {
            s->stop();
            KillTimer(h, 1);
            KillTimer(h, 2);
            EndDialog(h, 0);
            return TRUE;
        }
    }
    catch (const std::exception &e)
    {
        if (s)
        {
            s->loading = false;
            set(h, 106, wide(e.what()));
            if (s->screen)
                PostMessageW(h, WM_CLOSE, 0, 0);
        }
        else
            error(h, e);
    }
    return FALSE;
}
void showPlayback(HWND owner, const Settings &settings, bool screen, HWND embed, bool liveSettings)
{
    Playback s;
    s.settings = settings;
    s.screen = screen;
    s.embedded = embed != nullptr;
    s.liveSettings = liveSettings;
    std::error_code ec;
    s.settingsTime = std::filesystem::last_write_time(dataDirectory() / L"settings.json", ec);
    if (embed)
    {
        s.dialog =
            CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 0, 1, 1, embed, nullptr, instance, nullptr);
        RECT r{};
        GetClientRect(embed, &r);
        canvas(s, embed, r, false);
        // Windows' tiny embedded preview never accesses the network.
        const auto c = buildCatalog(settings, false, {}, {}, true);
        for (const auto &item : c.items)
            try
            {
                auto p = decodeImage(imageFile(item, settings.defaults, {}, true));
                s.canvases[0]->renderer->setImage(p);
                s.render(1);
                break;
            }
            catch (...)
            {
            }
        MSG msg{};
        while (IsWindow(embed) && GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            if (msg.message == WM_QUIT)
                break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (!IsWindow(s.canvases[0]->hwnd))
                break;
        }
        DestroyWindow(s.dialog);
        return;
    }
    modal(owner, L"", 980, 680, playbackProc, reinterpret_cast<LPARAM>(&s), !screen);
}

struct Main
{
    Layout ui;
    Settings settings;
    std::jthread worker;
    std::mutex mutex;
    std::optional<Catalog> incoming;
    std::vector<std::pair<std::size_t, FeedStatus>> pendingStatuses;
    HWND dialog = nullptr;
    HIMAGELIST statusIcons = nullptr;
    std::chrono::steady_clock::time_point lastRefresh{};
    ~Main()
    {
        worker.request_stop();
        if (worker.joinable())
            worker.join();
        if (statusIcons)
            ImageList_Destroy(statusIcons);
    }
    void stop()
    {
        worker.request_stop();
        if (worker.joinable())
            worker.join();
        std::lock_guard guard(mutex);
        incoming.reset();
        pendingStatuses.clear();
    }
    int row()
    {
        return ListView_GetNextItem(control(dialog, 10), -1, LVNI_SELECTED);
    }
    void updateControls()
    {
        const int count = ListView_GetItemCount(control(dialog, 10)), selectedRow = row();
        const bool hasSelection =
            selectedRow >= 0 && selectedRow < count && selectedRow < static_cast<int>(settings.feeds.size());
        for (const int id : {12, 13, 16})
            EnableWindow(control(dialog, id), hasSelection);
        EnableWindow(control(dialog, 14), hasSelection && selectedRow > 0);
        EnableWindow(control(dialog, 15), hasSelection && selectedRow + 1 < count);
        EnableWindow(control(dialog, 19), count > 0);
        bool enabled = false;
        for (int i = 0; i < count; ++i)
            if (ListView_GetCheckState(control(dialog, 10), i))
                enabled = true;
        EnableWindow(control(dialog, 17), enabled);
        const bool limited = !checked(dialog, 71);
        for (const int id : {72, 73, 74})
            EnableWindow(control(dialog, id), limited);
        const bool randomPlayback = selected(dialog, 35) == 1;
        EnableWindow(control(dialog, 40), randomPlayback);
        const auto transition = transitionChoices[selected(dialog, 34)];
        EnableWindow(control(dialog, 31), transition != Transition::none);
        for (const int id : randomControls)
            EnableWindow(control(dialog, id), transition == Transition::random);
    }
    void rows(int selection = -1)
    {
        HWND list = control(dialog, 10);
        ListView_DeleteAllItems(list);
        for (std::size_t i = 0; i < settings.feeds.size(); ++i)
        {
            const auto &f = settings.feeds[i];
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_IMAGE;
            item.iImage = validateUrl(f.url, f.policy.allowHttp).url ? 0 : 2;
            item.iItem = static_cast<int>(i);
            item.pszText =
                const_cast<wchar_t *>(f.title.empty() ? L"(title not retrieved)" : f.title.c_str());
            ListView_InsertItem(list, &item);
            ListView_SetItemText(list, item.iItem, 1, const_cast<wchar_t *>(f.url.c_str()));
            const auto status = validateUrl(f.url, f.policy.allowHttp);
            auto text = status.url ? L"Not checked" : status.error.c_str();
            ListView_SetItemText(list, item.iItem, 2, const_cast<wchar_t *>(L"—"));
            ListView_SetItemText(list, item.iItem, 3, const_cast<wchar_t *>(text));
            ListView_SetCheckState(list, item.iItem, f.policy.enabled);
        }
        if (selection >= 0)
            ListView_SetItemState(list, selection, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
        updateControls();
    }
    void fill()
    {
        const auto &d = settings.defaults;
        set(dialog, 30, std::to_wstring(d.displaySeconds));
        set(dialog, 31, std::to_wstring(d.transitionSeconds));
        set(dialog, 32, std::to_wstring(d.refreshHours));
        set(dialog, 33, std::to_wstring(d.cacheBytes / (1024 * 1024)));
        select(dialog, 34,
               static_cast<int>(
                   std::find(std::begin(transitionChoices), std::end(transitionChoices), d.transition) -
                   std::begin(transitionChoices)));
        select(dialog, 35, settings.random ? 1 : 0);
        select(dialog, 36, static_cast<int>(settings.theme));
        select(dialog, 37, settings.fill ? 1 : 0);
        check(dialog, 38, d.allowMature);
        check(dialog, 39, d.excludeUnrated);
        check(dialog, 40, settings.balanceFeeds);
        set(dialog, 41, join(d.excludedUrlText));
        set(dialog, 42, std::to_wstring(settings.hdrNits));
        set(dialog, 43, std::to_wstring(d.simultaneousDownloads));
        check(dialog, 71, d.age.entireFeed);
        set(dialog, 72, std::to_wstring(d.age.value));
        select(dialog, 73, static_cast<int>(d.age.unit));
        check(dialog, 74, d.age.includeUndated);
        for (unsigned i = 0; i < std::size(randomControls); ++i)
            check(dialog, randomControls[i], (settings.randomEffects & (1u << i)) != 0);
        rows();
        theme(dialog, settings.theme);
    }
    void read()
    {
        auto s = settings;
        auto &d = s.defaults;
        d.displaySeconds = number(dialog, 30, 1, 86400);
        if (transitionChoices[selected(dialog, 34)] != Transition::none)
            d.transitionSeconds = number(dialog, 31, 0, 30);
        d.refreshHours = number(dialog, 32, 1, 8760);
        d.cacheBytes = static_cast<std::uint64_t>(number(dialog, 33, 10, 4096)) * 1024 * 1024;
        d.transition = transitionChoices[selected(dialog, 34)];
        s.random = selected(dialog, 35) == 1;
        s.theme = static_cast<unsigned>(selected(dialog, 36));
        s.fill = selected(dialog, 37) == 1;
        d.allowMature = checked(dialog, 38);
        d.excludeUnrated = checked(dialog, 39);
        s.balanceFeeds = checked(dialog, 40);
        d.excludedUrlText = lines(value(dialog, 41));
        s.hdrNits = number(dialog, 42, 80, 1000);
        d.simultaneousDownloads = number(dialog, 43, 1, 4);
        d.age.entireFeed = checked(dialog, 71);
        if (!d.age.entireFeed)
            d.age.value = number(dialog, 72, 1, 1200);
        d.age.unit = static_cast<AgeUnit>(selected(dialog, 73));
        d.age.includeUndated = checked(dialog, 74);
        s.randomEffects = 0;
        for (unsigned i = 0; i < std::size(randomControls); ++i)
            if (checked(dialog, randomControls[i]))
                s.randomEffects |= 1u << i;
        if (!s.randomEffects)
        {
            if (d.transition == Transition::random)
                throw std::runtime_error("Choose at least one random transition effect");
            s.randomEffects = 127;
        }
        for (std::size_t i = 0; i < s.feeds.size(); ++i)
            s.feeds[i].policy.enabled = ListView_GetCheckState(control(dialog, 10), static_cast<int>(i)) != 0;
        settings = std::move(s);
    }
    void refresh(bool force = true)
    {
        if (force)
            read();
        stop();
        lastRefresh = std::chrono::steady_clock::now();
        const auto snapshot = settings;
        for (std::size_t i = 0; i < snapshot.feeds.size(); ++i)
            ListView_SetItemText(
                control(dialog, 10), static_cast<int>(i), 3,
                const_cast<wchar_t *>(snapshot.feeds[i].policy.enabled ? L"Checking…" : L"Disabled"));
        set(dialog, 60, L"Checking feeds…");
        worker = std::jthread([this, snapshot, force](std::stop_token token) {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            auto c = buildCatalog(snapshot, force, token,
                                  [this, token](std::size_t index, const FeedStatus &status) {
                                      if (!token.stop_requested())
                                      {
                                          std::lock_guard guard(mutex);
                                          pendingStatuses.emplace_back(index, status);
                                          PostMessageW(dialog, feedProgress, 0, 0);
                                      }
                                  });
            if (!token.stop_requested())
            {
                std::lock_guard guard(mutex);
                incoming = std::move(c);
                PostMessageW(dialog, completed, 0, 0);
            }
            CoUninitialize();
        });
    }
};
INT_PTR CALLBACK mainProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto *s = reinterpret_cast<Main *>(GetWindowLongPtrW(h, DWLP_USER));
    try
    {
        if (m == WM_INITDIALOG)
        {
            s = reinterpret_cast<Main *>(l);
            s->dialog = h;
            SetWindowLongPtrW(h, DWLP_USER, l);
            sizeDialog(h, L"mRSS Screen Saver — Settings", 1080, 752);
            auto &u = s->ui;
            u.baseW = 1080;
            u.baseH = 752;
            u.label(h, L"Image feeds", 18, 15, 250);
            auto list = u.add(h, 10, WC_LISTVIEWW, L"",
                              WS_TABSTOP | WS_HSCROLL | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 18,
                              43, 1040, 200, true);
            ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                                                        LVS_EX_CHECKBOXES | LVS_EX_LABELTIP);
            s->statusIcons = ImageList_Create(scale(h, 16), scale(h, 16), ILC_COLOR32 | ILC_MASK, 3, 0);
            for (const auto icon : {IDI_INFORMATION, IDI_WARNING, IDI_ERROR})
                ImageList_AddIcon(s->statusIcons, LoadIconW(nullptr, icon));
            ListView_SetImageList(list, s->statusIcons, LVSIL_SMALL);
            const wchar_t *cols[] = {L"Feed title", L"URL", L"Eligible", L"Status / last success / cached"};
            // Keep useful column widths rather than squeezing all content into
            // the viewport. The report view scrolls horizontally when needed.
            const int widths[] = {220, 140, 80, 580};
            for (int i = 0; i < 4; ++i)
            {
                LVCOLUMNW c{};
                c.mask = LVCF_TEXT | LVCF_WIDTH;
                c.pszText = const_cast<wchar_t *>(cols[i]);
                c.cx = scale(h, widths[i]);
                ListView_InsertColumn(list, i, &c);
            }
            u.button(h, 11, L"Add…", 18, 295, 80);
            u.button(h, 12, L"Edit…", 106, 295, 80);
            u.button(h, 13, L"Remove", 194, 295, 80);
            u.button(h, 14, L"Up", 282, 295, 64);
            u.button(h, 15, L"Down", 354, 295, 64);
            u.button(h, 16, L"Preview…", 426, 295, 100);
            u.button(h, 17, L"Refresh / Retry", 534, 295, 135);
            u.button(h, 18, L"Import OPML…", 677, 295, 155);
            u.button(h, 19, L"Export OPML…", 840, 295, 155);
            u.label(h,
                    L"Global settings (feeds may override display time and maximum age; mature content is "
                    L"global only)",
                    18, 341, 870);
            u.button(h, 57, L"Metadata…", 900, 337, 140);
            u.label(h, L"Image seconds", 18, 376, 115);
            u.edit(h, 30, 140, 372, 70);
            u.label(h, L"Transition seconds", 230, 376, 145);
            u.edit(h, 31, 378, 372, 70);
            u.label(h, L"Refresh hours", 470, 376, 120);
            u.edit(h, 32, 592, 372, 70);
            u.label(h, L"Cache MiB", 684, 376, 95);
            u.edit(h, 33, 785, 372, 80);
            u.label(h, L"Transition", 18, 416, 110);
            u.combo(h, 34, 140, 412, 180,
                    {L"None", L"Fade", L"Slide", L"Cross-fade", L"Flip", L"Random", L"Rotating cube",
                     L"Collage", L"Pan & zoom"});
            u.label(h, L"Playback", 344, 416, 90);
            u.combo(h, 35, 438, 412, 150, {L"Sequential", L"Random"});
            u.label(h, L"Appearance", 610, 416, 100);
            u.combo(h, 36, 715, 412, 150, {L"Follow Windows", L"Light", L"Dark"});
            u.label(h, L"Image layout", 18, 456, 110);
            u.combo(h, 37, 140, 452, 180, {L"Fit", L"Fill / crop"});
            u.toggle(h, 38, L"Allow mature images", 344, 452, 220);
            u.toggle(h, 39, L"Exclude unrated / unknown", 590, 452, 300);
            u.toggle(h, 40, L"Balance feeds in random playback", 18, 492, 310);
            u.label(h, L"HDR limit (nits)", 344, 496, 140);
            u.edit(h, 42, 490, 492, 80);
            u.label(h, L"Max downloads", 590, 496, 130);
            u.edit(h, 43, 725, 492, 60);
            u.label(h, L"Random effects", 18, 531, 130);
            u.toggle(h, 44, L"Fade", 150, 527, 70);
            u.toggle(h, 45, L"Slide", 225, 527, 70);
            u.toggle(h, 46, L"Cross-fade", 300, 527, 110);
            u.toggle(h, 47, L"Flip", 415, 527, 60);
            u.toggle(h, 75, L"Cube", 480, 527, 80);
            u.toggle(h, 76, L"Collage", 565, 527, 100);
            u.toggle(h, 77, L"Pan && zoom", 670, 527, 140);
            u.button(h, 48, L"Background…", 900, 525, 140);
            u.label(h,
                    L"Skip image URLs containing (one literal substring per line; feed rules are additive)",
                    18, 565, 1000);
            u.edit(h, 41, 18, 589, 1040, 55, true, true);
            u.button(h, 50, L"Clear cache", 18, 660, 110);
            u.button(h, 51, L"Restore defaults", 136, 660, 135);
            u.button(h, 52, L"Backup…", 279, 660, 100);
            u.button(h, 53, L"Restore…", 387, 660, 100);
            u.button(h, 54, L"Codec status", 495, 660, 115);
            u.button(h, 56, L"About…", 618, 660, 100);
            u.button(h, IDOK, L"OK", 738, 660, 96);
            u.button(h, 55, L"Apply", 842, 660, 96);
            u.button(h, IDCANCEL, L"Cancel", 946, 660, 110);
            u.add(h, 60, L"STATIC", L"HTTPS by default • Public feeds only • Refresh only while running",
                  SS_LEFT, 18, 707, 1040, 36, true, true);
            // Make room for the global age row without increasing the window's
            // minimum height: the feed list gives up one row of vertical space.
            for (auto &entry : u.entries)
                if (entry.y >= 295 && entry.y < 565)
                    entry.y -= 40;
            u.label(h, L"Maximum image age", 18, 531, 160);
            u.edit(h, 72, 180, 527, 80);
            u.combo(h, 73, 270, 527, 130, {L"Minutes", L"Hours", L"Days", L"Weeks", L"Months"});
            u.toggle(h, 71, L"Entire feed", 420, 527, 200);
            u.toggle(h, 74, L"Include undated items", 650, 527, 320);
            HWND previous = control(h, 48);
            for (const int id : {72, 73, 71, 74})
            {
                const auto current = control(h, id);
                SetWindowPos(current, previous, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                previous = current;
            }
            u.resize(h);
            s->fill();
            // Schedule the initial cache-aware check explicitly, independent of
            // the steady-clock epoch or Windows uptime.
            s->refresh(false);
            SetTimer(h, 1, 1000, nullptr);
            return TRUE;
        }
        if (!s)
            return FALSE;
        if (m == WM_TIMER && w == 1)
        {
            if (std::chrono::steady_clock::now() - s->lastRefresh >=
                std::chrono::hours(s->settings.defaults.refreshHours))
                s->refresh(false);
            return TRUE;
        }
        if (m == WM_SIZE)
        {
            s->ui.resize(h);
            return TRUE;
        }
        if (m == WM_GETMINMAXINFO)
        {
            auto *p = reinterpret_cast<MINMAXINFO *>(l);
            p->ptMinTrackSize = {scale(h, 1100), scale(h, 800)};
            return TRUE;
        }
        if (m == WM_DPICHANGED)
        {
            dpi(h, l);
            s->ui.resize(h);
            return TRUE;
        }
        if (m == WM_SETTINGCHANGE)
        {
            theme(h, s->settings.theme);
            return TRUE;
        }
        if (m == WM_NOTIFY)
        {
            const auto *n = reinterpret_cast<NMHDR *>(l);
            if (n->idFrom == 10 &&
                (n->code == LVN_ITEMCHANGED || n->code == LVN_DELETEALLITEMS || n->code == LVN_INSERTITEM))
                s->updateControls();
            if (n->idFrom == 10 && n->code == NM_DBLCLK)
                SendMessageW(h, WM_COMMAND, 12, 0);
        }
        if (m == feedProgress)
        {
            std::vector<std::pair<std::size_t, FeedStatus>> statuses;
            {
                std::lock_guard guard(s->mutex);
                statuses.swap(s->pendingStatuses);
            }
            for (const auto &[index, status] : statuses)
                if (index < s->settings.feeds.size())
                {
                    const auto count =
                        std::to_wstring(status.eligible) + L"/" + std::to_wstring(status.total);
                    ListView_SetItemText(control(h, 10), static_cast<int>(index), 2,
                                         const_cast<wchar_t *>(count.c_str()));
                    ListView_SetItemText(control(h, 10), static_cast<int>(index), 3,
                                         const_cast<wchar_t *>(status.message.c_str()));
                    if (!status.title.empty())
                    {
                        s->settings.feeds[index].title = status.title;
                        ListView_SetItemText(control(h, 10), static_cast<int>(index), 0,
                                             const_cast<wchar_t *>(status.title.c_str()));
                    }
                }
            return TRUE;
        }
        if (m == completed)
        {
            std::optional<Catalog> c;
            {
                std::lock_guard guard(s->mutex);
                c = std::move(s->incoming);
                s->incoming.reset();
            }
            if (c)
            {
                for (std::size_t i = 0; i < c->status.size() && i < s->settings.feeds.size(); ++i)
                {
                    const auto &f = c->status[i];
                    LVITEMW icon{};
                    icon.mask = LVIF_IMAGE;
                    icon.iItem = static_cast<int>(i);
                    icon.iImage = f.message.starts_with(L"Available") ? 0 : 1;
                    ListView_SetItem(control(h, 10), &icon);
                    if (!f.title.empty())
                    {
                        s->settings.feeds[i].title = f.title;
                        ListView_SetItemText(control(h, 10), static_cast<int>(i), 0,
                                             const_cast<wchar_t *>(f.title.c_str()));
                    }
                    const auto count = std::to_wstring(f.eligible) + L"/" + std::to_wstring(f.total);
                    ListView_SetItemText(control(h, 10), static_cast<int>(i), 2,
                                         const_cast<wchar_t *>(count.c_str()));
                    ListView_SetItemText(control(h, 10), static_cast<int>(i), 3,
                                         const_cast<wchar_t *>(f.message.c_str()));
                }
                set(h, 60,
                    L"Feed check complete. Apply to save retrieved titles. Select a feed and Preview to "
                    L"inspect eligible images.");
            }
            return TRUE;
        }
        if (m == WM_COMMAND)
        {
            const auto id = LOWORD(w);
            if (auto c = control(h, id); c && !IsWindowEnabled(c))
                return TRUE;
            if ((id == 71 && HIWORD(w) == BN_CLICKED) ||
                ((id == 34 || id == 35) && HIWORD(w) == CBN_SELCHANGE))
            {
                s->updateControls();
                return TRUE;
            }
            if (id == IDCANCEL)
            {
                s->stop();
                EndDialog(h, 0);
                return TRUE;
            }
            if (id == 11 || id == 12)
            {
                s->read();
                s->stop();
                const int row = s->row();
                if (id == 12 && row < 0)
                    return TRUE;
                FeedDialog f;
                f.appearance = s->settings.theme;
                f.globalAge = s->settings.defaults.age;
                f.globalMetadata = s->settings.metadata;
                if (id == 12)
                    f.feed = s->settings.feeds[static_cast<std::size_t>(row)];
                if (modal(h, L"", 680, 578, feedProc, reinterpret_cast<LPARAM>(&f)) == 1)
                {
                    if (std::any_of(
                            s->settings.feeds.begin(), s->settings.feeds.end(), [&](const auto &existing) {
                                return &existing != (id == 12
                                                         ? &s->settings.feeds[static_cast<std::size_t>(row)]
                                                         : nullptr) &&
                                       existing.url == f.feed.url;
                            }))
                        throw std::runtime_error("This feed URL is already in the list");
                    if (id == 12)
                        s->settings.feeds[static_cast<std::size_t>(row)] = std::move(f.feed);
                    else
                        s->settings.feeds.push_back(std::move(f.feed));
                    s->rows(id == 12 ? row : static_cast<int>(s->settings.feeds.size() - 1));
                    s->refresh();
                }
                theme(h, s->settings.theme);
                return TRUE;
            }
            if (id == 13)
            {
                s->read();
                s->stop();
                const int row = s->row();
                if (row >= 0 && ask(h, L"Remove the selected feed? Apply saves this change."))
                {
                    s->settings.feeds.erase(s->settings.feeds.begin() + row);
                    s->rows();
                }
                return TRUE;
            }
            if (id == 14 || id == 15)
            {
                s->read();
                s->stop();
                const int row = s->row(), other = row + (id == 14 ? -1 : 1);
                if (row >= 0 && other >= 0 && other < static_cast<int>(s->settings.feeds.size()))
                {
                    std::swap(s->settings.feeds[static_cast<std::size_t>(row)],
                              s->settings.feeds[static_cast<std::size_t>(other)]);
                    s->rows(other);
                }
                return TRUE;
            }
            if (id == 16)
            {
                s->read();
                const int row = s->row();
                if (row >= 0)
                {
                    s->stop();
                    auto preview = s->settings;
                    preview.feeds = {s->settings.feeds[static_cast<std::size_t>(row)]};
                    preview.random = false;
                    showPlayback(h, preview);
                    theme(h, s->settings.theme);
                }
                return TRUE;
            }
            if (id == 17)
            {
                s->refresh();
                return TRUE;
            }
            if (id == 18)
            {
                s->read();
                if (auto p = fileDialog(h, false, L"OPML feeds\0*.opml;*.xml\0All files\0*.*\0", L"opml"))
                {
                    const auto entries = parseOpml(readFile(*p, 8 * 1024 * 1024));
                    const auto prompt = L"Found " + std::to_wstring(entries.size()) +
                                        L" unique feed URLs.\n\nYes: merge with current list\nNo: replace "
                                        L"current list\nCancel: leave unchanged";
                    const int choice = dmlib::darkMessageBoxW(h, prompt.c_str(), L"Import preview",
                                                              MB_YESNOCANCEL | MB_ICONQUESTION);
                    if (choice == IDCANCEL)
                        return TRUE;
                    s->stop();
                    auto imported = s->settings.feeds;
                    if (choice == IDNO)
                        imported.clear();
                    for (const auto &f : entries)
                        if (std::none_of(imported.begin(), imported.end(),
                                         [&](const auto &a) { return a.url == f.url; }))
                        {
                            imported.push_back(decodeOutline(f));
                        }
                    if (imported.size() > 1000)
                        throw std::runtime_error("Feed limit exceeded (maximum 1000 feeds)");
                    s->settings.feeds = std::move(imported);
                    s->rows();
                    s->refresh();
                }
                return TRUE;
            }
            if (id == 19)
            {
                s->read();
                if (!ask(h, L"Exported feed URLs may contain private access tokens. Continue?"))
                    return TRUE;
                if (auto p = fileDialog(h, true, L"OPML feeds\0*.opml\0", L"opml"))
                {
                    std::vector<Outline> entries;
                    const auto encoded = encodeSettings(s->settings);
                    for (std::size_t i = 0; i < s->settings.feeds.size(); ++i)
                    {
                        const auto &f = s->settings.feeds[i];
                        entries.push_back({f.title, f.url, encoded.at("feeds").at(i).dump()});
                    }
                    writeText(*p, exportOpml(entries));
                }
                return TRUE;
            }
            if (id == 50)
            {
                s->stop();
                if (ask(h, L"Delete cached feed data and images? Settings will be kept."))
                {
                    clearCache();
                    set(h, 60, L"Cache cleared.");
                }
                return TRUE;
            }
            if (id == 51)
            {
                if (ask(h, L"Restore global defaults while keeping the feed list and its per-feed settings?"))
                {
                    s->read();
                    auto feeds = s->settings.feeds;
                    s->settings = Settings{};
                    s->settings.feeds = std::move(feeds);
                    s->fill();
                }
                return TRUE;
            }
            if (id == 52)
            {
                s->read();
                if (!ask(h,
                         L"Settings backups include full feed URLs and may contain access tokens. Continue?"))
                    return TRUE;
                if (auto p = fileDialog(h, true, L"Settings backup\0*.json\0", L"json"))
                    writeText(*p, encodeSettings(s->settings).dump(2));
                return TRUE;
            }
            if (id == 53)
            {
                if (auto p = fileDialog(h, false, L"Settings backup\0*.json\0", L"json"))
                {
                    const auto b = readFile(*p, 4 * 1024 * 1024);
                    auto restored = decodeSettings(
                        parseJson(std::string_view(reinterpret_cast<const char *>(b.data()), b.size())));
                    if (ask(h, L"Replace current unsaved settings with this backup?"))
                    {
                        s->stop();
                        s->settings = std::move(restored);
                        s->fill();
                    }
                }
                return TRUE;
            }
            if (id == 54)
            {
                dmlib::darkMessageBoxW(h, codecStatus().c_str(), L"Installed Microsoft image decoders",
                                       MB_OK);
                return TRUE;
            }
            if (id == 57)
            {
                s->read();
                MetadataDialog dialog;
                dialog.appearance = s->settings.theme;
                dialog.options = s->settings.metadata;
                if (modal(h, L"", 510, 310, metadataProc, reinterpret_cast<LPARAM>(&dialog)) == 1)
                    s->settings.metadata = std::move(dialog.options);
                theme(h, s->settings.theme);
                return TRUE;
            }
            if (id == 56)
            {
                showAbout(h);
                return TRUE;
            }
            if (id == 48)
            {
                static COLORREF custom[16]{};
                CHOOSECOLORW c{sizeof(c)};
                c.hwndOwner = h;
                c.rgbResult = RGB(s->settings.background >> 16 & 255, s->settings.background >> 8 & 255,
                                  s->settings.background & 255);
                c.lpCustColors = custom;
                c.Flags = CC_FULLOPEN | CC_RGBINIT;
                if (dmlib::darkChooseColorW(&c))
                    s->settings.background = (GetRValue(c.rgbResult) << 16) | (GetGValue(c.rgbResult) << 8) |
                                             GetBValue(c.rgbResult);
                return TRUE;
            }
            if (id == IDOK || id == 55)
            {
                s->read();
                s->stop();
                saveSettings(s->settings);
                pruneRemovedFeeds(s->settings);
                trimCache(s->settings.defaults.cacheBytes);
                theme(h, s->settings.theme);
                set(h, 60, L"Settings saved.");
                if (id == IDOK)
                    EndDialog(h, 1);
                return TRUE;
            }
            if (id == 36 && HIWORD(w) == CBN_SELCHANGE)
            {
                theme(h, static_cast<unsigned>(selected(h, 36)));
                return TRUE;
            }
        }
        if (m == WM_CLOSE)
        {
            s->stop();
            EndDialog(h, 0);
            return TRUE;
        }
    }
    catch (const std::exception &e)
    {
        error(h, e);
    }
    return FALSE;
}
void selectScreenSaver(bool selectIt)
{
    wchar_t path[32768]{};
    GetModuleFileNameW(nullptr, path, 32768);
    HKEY desktop = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0, nullptr, 0,
                        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &desktop, nullptr) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot open screensaver preferences");
    wchar_t current[32768]{};
    DWORD size = sizeof(current);
    RegGetValueW(desktop, nullptr, L"SCRNSAVE.EXE", RRF_RT_REG_SZ, nullptr, current, &size);
    HKEY own = nullptr;
    RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\mRSSScreenSaver", 0, nullptr, 0,
                    KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &own, nullptr);
    if (selectIt)
    {
        if (_wcsicmp(current, path) != 0 && own)
        {
            RegSetValueExW(own, L"PreviousScreenSaver", 0, REG_SZ, reinterpret_cast<const BYTE *>(current),
                           static_cast<DWORD>((wcslen(current) + 1) * 2));
            BOOL active = FALSE;
            SystemParametersInfoW(SPI_GETSCREENSAVEACTIVE, 0, &active, 0);
            const DWORD previousActive = active ? 1 : 0;
            RegSetValueExW(own, L"PreviousActive", 0, REG_DWORD,
                           reinterpret_cast<const BYTE *>(&previousActive), sizeof(previousActive));
        }
        RegSetValueExW(desktop, L"SCRNSAVE.EXE", 0, REG_SZ, reinterpret_cast<const BYTE *>(path),
                       static_cast<DWORD>((wcslen(path) + 1) * 2));
        SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE, TRUE, nullptr, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    }
    else if (_wcsicmp(current, path) == 0)
    {
        wchar_t previous[32768]{};
        DWORD length = sizeof(previous);
        if (own)
            RegGetValueW(own, nullptr, L"PreviousScreenSaver", RRF_RT_REG_SZ, nullptr, previous, &length);
        if (*previous)
            RegSetValueExW(desktop, L"SCRNSAVE.EXE", 0, REG_SZ, reinterpret_cast<const BYTE *>(previous),
                           static_cast<DWORD>((wcslen(previous) + 1) * 2));
        else
            RegDeleteValueW(desktop, L"SCRNSAVE.EXE");
        DWORD active = 1, bytes = sizeof(active);
        if (own && RegGetValueW(own, nullptr, L"PreviousActive", RRF_RT_REG_DWORD, nullptr, &active,
                                &bytes) == ERROR_SUCCESS)
            SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE, active ? TRUE : FALSE, nullptr,
                                  SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    }
    if (own)
        RegCloseKey(own);
    RegCloseKey(desktop);
}
} // namespace
int WINAPI wWinMain(HINSTANCE app, HINSTANCE, LPWSTR, int)
{
    instance = app;
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com))
        return 1;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);
    dmlib::initDarkMode();
    dmlib::setDarkModeConfigEx(dmlib::isDarkModeReg() ? 1 : 0);
    dmlib::setDefaultColors(true);
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.lpszClassName = L"mRSSCanvas";
    wc.lpfnWndProc = canvasProc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassW(&wc);
    int result = 0;
    HANDLE singleton = nullptr;
    try
    {
        int argc = 0;
        auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::wstring argument = argc > 1 ? argv[1] : L"/c";
        std::transform(argument.begin(), argument.end(), argument.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        for (int i = 1; i < argc; ++i)
            if (std::wstring_view(argv[i]).starts_with(L"/data:"))
                setDataDirectory(std::filesystem::path(std::wstring(argv[i] + 6)));
        std::wstring handleText;
        if (argument.size() > 3 && argument[2] == L':')
            handleText = argument.substr(3);
        else if (argc > 2)
            handleText = argv[2];
        LocalFree(argv);
        if (argument == L"/select")
            selectScreenSaver(true);
        else if (argument == L"/unselect")
            selectScreenSaver(false);
        else
        {
            std::wstring recovery;
            Settings settings;
            try
            {
                settings = loadSettings(recovery);
            }
            catch (const std::exception &e)
            {
                if (argument == L"/s" || argument.starts_with(L"/p"))
                    return 0;
                recovery = wide(e.what());
            }
            if (argument == L"/s")
            {
                singleton = CreateMutexW(nullptr, FALSE, L"Local\\mRSSScreenSaver.Fullscreen.v1");
                if (singleton && GetLastError() != ERROR_ALREADY_EXISTS)
                    showPlayback(nullptr, settings, true, nullptr, true);
            }
            else if (argument.starts_with(L"/p"))
            {
                if (!handleText.empty())
                {
                    const auto n = std::stoull(handleText);
                    HWND parent = reinterpret_cast<HWND>(static_cast<UINT_PTR>(n));
                    if (IsWindow(parent))
                        showPlayback(nullptr, settings, false, parent);
                }
            }
            else
            {
                if (!recovery.empty())
                    dmlib::darkMessageBoxW(nullptr, recovery.c_str(), L"Settings recovery",
                                           MB_OK | MB_ICONWARNING);
                Main main;
                main.settings = std::move(settings);
                modal(nullptr, L"", 1080, 752, mainProc, reinterpret_cast<LPARAM>(&main), true);
            }
        }
    }
    catch (const std::exception &e)
    {
        error(nullptr, e);
        result = 1;
    }
    if (singleton)
        CloseHandle(singleton);
    CoUninitialize();
    return result;
}
