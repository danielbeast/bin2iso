// Polished Win32 GUI front-end for bin2iso, modelled on the WinBin2Iso layout:
// a Source field, a Target field, browse buttons, a Convert/Cancel button pair,
// a progress bar and a status line.
//
// Threading model: the conversion runs on a detached worker thread. The worker
// NEVER touches the UI directly (no MessageBox / EnableWindow / SetWindowText).
// It only PostMessages back to the main window:
//   * WM_APP_PROGRESS carries percent (wParam) + MB-done*10 (lParam) for the
//     status line and progress bar.
//   * WM_APP_DONE carries a heap-allocated DoneMsg the UI thread takes ownership
//     of, deletes, and uses to restore the UI and show the result dialog.
// Cancellation is cooperative: the Cancel button sets g_cancel; the ProgressFn
// returns !g_cancel so the engine aborts and deletes the partial ISO itself.
//
// Native theming is driven by an embedded Common-Controls v6 manifest (see
// app.manifest / app.rc), not a linker #pragma, so it stays self-contained and
// does not fight the Visual Studio generator's manifest tooling.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

// <windows.h> must precede the other Win32 headers below (they rely on its
// types and macros). The project's .clang-format sets SortIncludes: Never, so
// this order is preserved as written.
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <atomic>
#include <string>
#include <thread>

#include "converter.h"
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

namespace
{

    using bin2iso::convert;
    using bin2iso::ConvertOptions;
    using bin2iso::ConvertResult;

    enum
    {
        IDC_SRC_EDIT = 1001,
        IDC_SRC_BROWSE,
        IDC_TGT_EDIT,
        IDC_TGT_BROWSE,
        IDC_CONVERT,
        IDC_CANCEL,
        IDC_ABOUT,
        IDC_PROGRESS,
        IDC_STATUS,
    };

// Custom messages the worker posts to the UI thread.
#define WM_APP_PROGRESS (WM_APP + 1)
#define WM_APP_DONE (WM_APP + 2)

    // Result handed from the worker to the UI thread via WM_APP_DONE. The UI thread
    // owns and deletes it.
    struct DoneMsg
    {
        bool ok = false;
        bool cancelled = false;
        std::wstring message; // ready-to-show dialog text
        std::wstring isoPath; // for the "open containing folder" offer (success only)
        UINT icon = MB_ICONINFORMATION;
    };

    HWND g_srcEdit = nullptr;
    HWND g_tgtEdit = nullptr;
    HWND g_progress = nullptr;
    HWND g_status = nullptr;
    HWND g_convert = nullptr;
    HWND g_cancel = nullptr;
    HFONT g_uiFont = nullptr; // Segoe UI, sized to the window's DPI

    volatile bool g_running = false;
    std::atomic<bool> g_cancel_req{false};

    // DPI of the window, resolved at runtime. GetDpiForWindow exists on Win10 1607+;
    // we load it dynamically so the binary still links and runs on older systems,
    // where it falls back to the device-context DPI (system scaling).
    UINT dpiFor(HWND hwnd)
    {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        static auto fn =
            reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
        if (fn)
        {
            UINT d = fn(hwnd);
            if (d)
            {
                return d;
            }
        }
        HDC dc = GetDC(hwnd);
        UINT d = dc ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX)) : 96;
        if (dc)
        {
            ReleaseDC(hwnd, dc);
        }
        return d ? d : 96;
    }

    // Scales a 96-DPI design coordinate to the window's actual DPI.
    int scaled(int px96, UINT dpi)
    {
        return MulDiv(px96, static_cast<int>(dpi), 96);
    }

    // Builds the native UI face (Segoe UI 9pt) at the given DPI. The stock
    // DEFAULT_GUI_FONT is the legacy bitmap face; Segoe UI is what every modern
    // Windows app uses and it hints cleanly when scaled.
    HFONT makeUiFont(UINT dpi)
    {
        LOGFONTW lf{};
        lf.lfHeight = -MulDiv(9, static_cast<int>(dpi), 72);
        lf.lfWeight = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        HFONT f = CreateFontIndirectW(&lf);
        return f ? f : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    }

    std::wstring toW(const std::string& s)
    {
        if (s.empty())
        {
            return L"";
        }
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(n ? n - 1 : 0, L'\0');
        if (n)
        {
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
        }
        return w;
    }

    std::string toU8(const std::wstring& w)
    {
        if (w.empty())
        {
            return "";
        }
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(n ? n - 1 : 0, '\0');
        if (n)
        {
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
        }
        return s;
    }

    std::wstring getText(HWND h)
    {
        int len = GetWindowTextLengthW(h);
        std::wstring s(len, L'\0');
        GetWindowTextW(h, &s[0], len + 1);
        return s;
    }

    bool fileExists(const std::wstring& path)
    {
        if (path.empty())
        {
            return false;
        }
        DWORD a = GetFileAttributesW(path.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }

    std::wstring browseOpen(HWND owner)
    {
        wchar_t file[MAX_PATH] = L"";
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = owner;
        ofn.lpstrFilter = L"CD images (*.cue;*.bin)\0*.cue;*.bin\0CUE sheets (*.cue)\0*.cue\0All files (*.*)\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn))
        {
            return file;
        }
        return L"";
    }

    std::wstring browseSave(HWND owner, const std::wstring& suggested)
    {
        wchar_t file[MAX_PATH] = L"";
        if (!suggested.empty())
        {
            wcsncpy_s(file, suggested.c_str(), _TRUNCATE);
        }
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = owner;
        ofn.lpstrFilter = L"ISO images (*.iso)\0*.iso\0All files (*.*)\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrDefExt = L"iso";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        if (GetSaveFileNameW(&ofn))
        {
            return file;
        }
        return L"";
    }

    std::wstring deriveTarget(const std::wstring& cue)
    {
        size_t dot = cue.find_last_of(L'.');
        size_t slash = cue.find_last_of(L"/\\");
        if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
        {
            return cue.substr(0, dot) + L".iso";
        }
        return cue + L".iso";
    }

    // Updates the Convert button's enabled state: only clickable with a non-empty
    // Source and no conversion in progress.
    void updateConvertEnabled()
    {
        bool enable = !g_running && GetWindowTextLengthW(g_srcEdit) > 0;
        EnableWindow(g_convert, enable ? TRUE : FALSE);
    }

    // Worker thread. Runs the conversion and reports back only via PostMessage.
    void runConversion(HWND hwnd, std::wstring cue, std::wstring iso)
    {
        ConvertOptions opts;
        opts.cuePath = toU8(cue);
        opts.isoPath = toU8(iso);

        ConvertResult r = convert(opts,
                                  [hwnd](uint64_t done, uint64_t total) -> bool
                                  {
                                      if (total != 0)
                                      {
                                          int pct = static_cast<int>((done * 100) / total);
                                          // MB done, kept to one decimal by multiplying by 10.
                                          WPARAM wp = static_cast<WPARAM>(pct);
                                          LPARAM lp = static_cast<LPARAM>((done * 10) / (1024 * 1024));
                                          PostMessageW(hwnd, WM_APP_PROGRESS, wp, lp);
                                      }
                                      return !g_cancel_req.load();
                                  });

        auto* dm = new DoneMsg();
        dm->ok = r.ok;
        dm->isoPath = iso;
        if (r.ok)
        {
            dm->message = L"Done. Wrote " + std::to_wstring(r.sectorsWritten) + L" sectors (" +
                          std::to_wstring(r.bytesWritten / (1024 * 1024)) + L" MB).";
            if (!r.warnings.empty())
            {
                dm->message += L"\n\n" + toW(r.warnings);
                dm->icon = MB_ICONWARNING;
            }
            else
            {
                dm->icon = MB_ICONINFORMATION;
            }
        }
        else
        {
            // The engine reports cancellation through error text; flag it so the UI
            // shows "Cancelled" rather than an error dialog.
            std::string lower = r.error;
            dm->cancelled = g_cancel_req.load() || lower.find("cancel") != std::string::npos;
            dm->message = toW(r.error);
            dm->icon = MB_ICONERROR;
        }
        PostMessageW(hwnd, WM_APP_DONE, 0, reinterpret_cast<LPARAM>(dm));
    }

    void startConversion(HWND hwnd, const std::wstring& cue, const std::wstring& iso)
    {
        g_cancel_req.store(false);
        g_running = true;
        SendMessageW(g_progress, PBM_SETPOS, 0, 0);
        SetWindowTextW(g_status, L"Converting\u2026");
        EnableWindow(g_convert, FALSE);
        ShowWindow(g_cancel, SW_SHOW);
        EnableWindow(g_cancel, TRUE);
        std::thread(runConversion, hwnd, cue, iso).detach();
    }

    void onConvert(HWND hwnd)
    {
        if (g_running)
        {
            return;
        }
        std::wstring cue = getText(g_srcEdit);
        std::wstring iso = getText(g_tgtEdit);
        if (cue.empty())
        {
            MessageBoxW(hwnd, L"Please select a source .cue or .bin file.", L"WinBin2Iso", MB_OK | MB_ICONWARNING);
            return;
        }
        if (iso.empty())
        {
            iso = deriveTarget(cue);
            SetWindowTextW(g_tgtEdit, iso.c_str());
        }
        // Guard the direct Convert path (the save dialog has its own
        // OFN_OVERWRITEPROMPT, but a typed/derived target bypasses it).
        if (fileExists(iso))
        {
            std::wstring prompt = L"\"" + iso + L"\" already exists.\n\nOverwrite it?";
            if (MessageBoxW(hwnd, prompt.c_str(), L"WinBin2Iso", MB_YESNO | MB_ICONWARNING) != IDYES)
            {
                return;
            }
        }
        startConversion(hwnd, cue, iso);
    }

    void onDone(HWND hwnd, DoneMsg* dm)
    {
        g_running = false;
        EnableWindow(g_cancel, FALSE);
        ShowWindow(g_cancel, SW_HIDE);
        updateConvertEnabled();

        if (dm->ok)
        {
            SendMessageW(g_progress, PBM_SETPOS, 100, 0);
            SetWindowTextW(g_status, L"Done");
            // One dialog, not two: the result and the "open folder?" offer are a
            // single Yes/No prompt so a successful conversion interrupts once.
            std::wstring prompt = dm->message + L"\n\nOpen the containing folder?";
            if (MessageBoxW(hwnd, prompt.c_str(), L"WinBin2Iso", MB_YESNO | dm->icon) == IDYES)
            {
                std::wstring arg = L"/select,\"" + dm->isoPath + L"\"";
                ShellExecuteW(hwnd, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
            }
        }
        else if (dm->cancelled)
        {
            SendMessageW(g_progress, PBM_SETPOS, 0, 0);
            SetWindowTextW(g_status, L"Cancelled");
        }
        else
        {
            SendMessageW(g_progress, PBM_SETPOS, 0, 0);
            SetWindowTextW(g_status, L"Failed");
            MessageBoxW(hwnd, dm->message.c_str(), L"WinBin2Iso", MB_OK | MB_ICONERROR);
        }
    }

    void showAbout(HWND hwnd)
    {
        std::wstring text = L"WinBin2Iso (C++)  v" + toW(APP_VERSION_STRING) +
                            L"\n\nConverts BIN/CUE disc images to ISO.\n"
                            L"A Win32 front-end over the bin2iso engine.";
        MessageBoxW(hwnd, text.c_str(), L"About WinBin2Iso", MB_OK | MB_ICONINFORMATION);
    }

    void onDropFiles(HDROP drop)
    {
        wchar_t path[MAX_PATH] = L"";
        if (DragQueryFileW(drop, 0, path, MAX_PATH))
        {
            SetWindowTextW(g_srcEdit, path);
            if (GetWindowTextLengthW(g_tgtEdit) == 0)
            {
                SetWindowTextW(g_tgtEdit, deriveTarget(path).c_str());
            }
            updateConvertEnabled();
        }
        DragFinish(drop);
    }

    HWND makeLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h, HFONT font)
    {
        HWND lbl = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, parent, nullptr,
                                   (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE), nullptr);
        SendMessageW(lbl, WM_SETFONT, (WPARAM)font, TRUE);
        return lbl;
    }

    void setFont(HWND h, HFONT font)
    {
        SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_CREATE:
        {
            HINSTANCE hInst = ((LPCREATESTRUCTW)lParam)->hInstance;
            UINT dpi = dpiFor(hwnd);
            g_uiFont = makeUiFont(dpi);
            HFONT font = g_uiFont;

            // Window icon (title bar + taskbar).
            HICON icon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
            if (icon)
            {
                SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
                SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
            }

            // Layout grid. All coordinates are authored at 96 DPI and scaled to
            // the window's DPI via S(). A 16px margin frames the window; the two
            // fields share a column with their labels in a fixed gutter and a
            // browse button closing each row. A wider gap sets the action group
            // apart from the inputs, then the progress bar and status line
            // follow with deliberate vertical rhythm.
            auto S = [dpi](int v)
            {
                return scaled(v, dpi);
            };
            const int margin = S(16);
            const int gutter = S(10);
            const int labelW = S(52);
            const int rowH = S(26);
            const int btnW = S(32);
            const int contentW = S(520) - 2 * margin; // width inside the margins
            const int fieldX = margin + labelW + gutter;
            const int fieldW = margin + contentW - fieldX - gutter - btnW;
            const int browseX = margin + contentW - btnW;

            int y = margin;

            // Row 1: Source. The label is nudged down to sit level with the edit
            // control's text baseline.
            makeLabel(hwnd, L"Source", margin, y + S(5), labelW, S(18), font);
            g_srcEdit =
                CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                fieldX, y, fieldW, rowH, hwnd, (HMENU)IDC_SRC_EDIT, hInst, nullptr);
            setFont(g_srcEdit, font);
            HWND srcBrowse =
                CreateWindowExW(0, L"BUTTON", L"\u2026", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, browseX, y,
                                btnW, rowH, hwnd, (HMENU)IDC_SRC_BROWSE, hInst, nullptr);
            setFont(srcBrowse, font);

            y += rowH + gutter;

            // Row 2: Target.
            makeLabel(hwnd, L"Target", margin, y + S(5), labelW, S(18), font);
            g_tgtEdit =
                CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                fieldX, y, fieldW, rowH, hwnd, (HMENU)IDC_TGT_EDIT, hInst, nullptr);
            setFont(g_tgtEdit, font);
            HWND tgtBrowse =
                CreateWindowExW(0, L"BUTTON", L"\u2026", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, browseX, y,
                                btnW, rowH, hwnd, (HMENU)IDC_TGT_BROWSE, hInst, nullptr);
            setFont(tgtBrowse, font);

            // Wider gap separates the action group from the inputs above.
            y += rowH + S(20);

            // Command row. Convert is the primary action and leads; Cancel takes
            // its exact place while a conversion runs; About sits to the right,
            // de-emphasized. The window's close box / Esc handle "exit", so no
            // redundant Exit button clutters the row.
            const int cmdBtnW = S(104);
            const int cmdBtnH = S(30);
            g_convert = CreateWindowExW(0, L"BUTTON", L"Convert", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                        margin, y, cmdBtnW, cmdBtnH, hwnd, (HMENU)IDC_CONVERT, hInst, nullptr);
            setFont(g_convert, font);
            EnableWindow(g_convert, FALSE); // enabled once a Source is set

            g_cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, margin, y,
                                       cmdBtnW, cmdBtnH, hwnd, (HMENU)IDC_CANCEL, hInst, nullptr);
            setFont(g_cancel, font); // hidden until converting; overlays Convert

            const int aboutW = S(84);
            HWND about =
                CreateWindowExW(0, L"BUTTON", L"About", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                margin + contentW - aboutW, y, aboutW, cmdBtnH, hwnd, (HMENU)IDC_ABOUT, hInst, nullptr);
            setFont(about, font);

            y += cmdBtnH + S(18);

            g_progress = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_SMOOTH, margin, y,
                                         contentW, S(18), hwnd, (HMENU)IDC_PROGRESS, hInst, nullptr);
            SendMessageW(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

            y += S(18) + gutter;

            g_status = CreateWindowExW(0, L"STATIC", L"Ready",
                                       WS_CHILD | WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS | SS_ENDELLIPSIS, margin, y,
                                       contentW, S(20), hwnd, (HMENU)IDC_STATUS, hInst, nullptr);
            setFont(g_status, font);

            DragAcceptFiles(hwnd, TRUE);
            return 0;
        }
        case WM_APP_PROGRESS:
        {
            int pct = static_cast<int>(wParam);
            double mb = static_cast<double>(lParam) / 10.0;
            SendMessageW(g_progress, PBM_SETPOS, pct, 0);
            wchar_t buf[128];
            swprintf_s(buf, L"Converting\u2026 %d%% (%.1f MB)", pct, mb);
            SetWindowTextW(g_status, buf);
            return 0;
        }
        case WM_APP_DONE:
        {
            DoneMsg* dm = reinterpret_cast<DoneMsg*>(lParam);
            if (dm)
            {
                onDone(hwnd, dm);
                delete dm;
            }
            return 0;
        }
        case WM_DROPFILES:
            onDropFiles(reinterpret_cast<HDROP>(wParam));
            return 0;
        case WM_COMMAND:
        {
            switch (LOWORD(wParam))
            {
            case IDC_SRC_EDIT:
                if (HIWORD(wParam) == EN_CHANGE)
                {
                    updateConvertEnabled();
                }
                return 0;
            case IDC_SRC_BROWSE:
            {
                std::wstring f = browseOpen(hwnd);
                if (!f.empty())
                {
                    SetWindowTextW(g_srcEdit, f.c_str());
                    if (GetWindowTextLengthW(g_tgtEdit) == 0)
                    {
                        SetWindowTextW(g_tgtEdit, deriveTarget(f).c_str());
                    }
                }
                return 0;
            }
            case IDC_TGT_BROWSE:
            {
                std::wstring f = browseSave(hwnd, deriveTarget(getText(g_srcEdit)));
                if (!f.empty())
                {
                    SetWindowTextW(g_tgtEdit, f.c_str());
                }
                return 0;
            }
            case IDC_CONVERT:
                onConvert(hwnd);
                return 0;
            case IDC_CANCEL:
                g_cancel_req.store(true);
                EnableWindow(g_cancel, FALSE);
                SetWindowTextW(g_status, L"Cancelling\u2026");
                return 0;
            case IDC_ABOUT:
                showAbout(hwnd);
                return 0;
            }
            return 0;
        }
        case WM_CLOSE:
            if (g_running)
            {
                if (MessageBoxW(hwnd, L"A conversion is running. Quit anyway?", L"WinBin2Iso",
                                MB_YESNO | MB_ICONQUESTION) != IDYES)
                {
                    return 0;
                }
                g_cancel_req.store(true);
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (g_uiFont)
            {
                DeleteObject(g_uiFont);
                g_uiFont = nullptr;
            }
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow)
{
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    const wchar_t* cls = L"Bin2IsoMainWindow";
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = cls;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    RegisterClassW(&wc);

    const DWORD style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX;

    // Client size matches the DPI-scaled layout (authored at 96 DPI: 520x210).
    // Size the window for the system DPI so a high-DPI primary monitor gets a
    // frame that fits the scaled controls. AdjustWindowRectExForDpi and
    // GetDpiForSystem are Win10 1607+; we load them dynamically and fall back to
    // the 96-DPI AdjustWindowRect on older systems.
    UINT sysDpi = 96;
    using GetDpiForSystemFn = UINT(WINAPI*)();
    if (auto gdfs =
            reinterpret_cast<GetDpiForSystemFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem")))
    {
        if (UINT d = gdfs())
        {
            sysDpi = d;
        }
    }
    RECT r{0, 0, MulDiv(520, static_cast<int>(sysDpi), 96), MulDiv(210, static_cast<int>(sysDpi), 96)};
    using AdjustFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    if (auto adj =
            reinterpret_cast<AdjustFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi")))
    {
        adj(&r, style, FALSE, 0, sysDpi);
    }
    else
    {
        AdjustWindowRect(&r, style, FALSE);
    }
    int w = r.right - r.left;
    int h = r.bottom - r.top;

    // Center on the primary monitor at startup.
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    if (x < 0)
    {
        x = CW_USEDEFAULT;
    }
    if (y < 0)
    {
        y = CW_USEDEFAULT;
    }

    HWND hwnd = CreateWindowExW(0, cls, L"WinBin2Iso  -  BIN/CUE to ISO", style, x, y, w, h, nullptr, nullptr,
                                hInstance, nullptr);
    if (!hwnd)
    {
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        // Esc closes the window; keep IsDialogMessage for tab/Enter navigation.
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE)
        {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            continue;
        }
        if (!IsDialogMessageW(hwnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}
