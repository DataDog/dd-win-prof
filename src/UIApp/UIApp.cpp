// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

// UIApp.cpp : A minimal Win32 UI test application used to exercise the
// dd-win-prof UI hang detector. It renders a live clock and offers three
// push-like buttons that deliberately freeze the UI thread for a configurable
// duration:
//   - Sleep   : blocks the UI thread with Sleep() (blocked, non-runnable hang)
//   - Wait    : blocks the UI thread on WaitForSingleObject() (blocked hang)
//   - CPU hog : busy-spins the UI thread (runnable, CPU-bound hang)
//
// The profiler is started on window creation and the main window is registered
// for hang monitoring; profiling is stopped when the window is destroyed.
//
// For CI, an automation mode (--auto-hang sleep|wait|cpu|all, --hang-duration-ms,
// --hang-cycles) drives the same hangs from a worker thread that posts a private
// WM_APP_HANG message to the UI thread, then closes the window when done. The
// process exit code is non-zero if the profiler or hang monitoring failed.

#include <Windows.h>
#include <shellapi.h>

#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <string>

#include "../dd-win-prof/dd-win-prof.h"

namespace {

constexpr int ID_EDIT_INTERVAL = 1001;
constexpr int ID_BTN_SLEEP = 1002;
constexpr int ID_BTN_WAIT = 1003;
constexpr int ID_BTN_CPU = 1004;

constexpr UINT_PTR ID_CLOCK_TIMER = 1;
constexpr UINT CLOCK_INTERVAL_MS = 100;

constexpr UINT_PTR ID_SLEEP_TIMER = 2;
constexpr UINT_PTR ID_WAIT_TIMER = 3;
constexpr UINT_PTR ID_CPU_TIMER = 4;

// Default freeze duration when the interval text box is empty or invalid.
constexpr UINT DEFAULT_INTERVAL_MS = 1000;

// The kind of hang to reproduce; also used as the wParam of WM_APP_HANG.
enum class HangKind { Sleep, Wait, Cpu, All };

// Private message posted by the automation worker thread to the UI thread to
// request a hang. wParam is a HangKind (Sleep/Wait/Cpu); the hang runs on the
// UI thread inside the handler, which is exactly what the detector must catch.
constexpr UINT WM_APP_HANG = WM_APP + 1;

// Time the automation worker waits, on top of the hang duration, before posting
// the next hang. It lets the UI thread pump long enough for the watchdog to
// observe the probe again and emit the UIHang=false (recovered) sample. Kept as
// an internal constant so the command line stays limited to the three options.
constexpr UINT kRecoveryGapMs = 500;

// Time the automation worker waits before posting the very first hang. It lets
// the initial window paint finish and the watchdog establish a responsive
// baseline, so the first real hang is detected inside its blocking call rather
// than merging with startup jank.
constexpr UINT kWarmupMs = 1000;

HWND g_editInterval = nullptr;
HWND g_btnSleep = nullptr;
HWND g_btnWait = nullptr;
HWND g_btnCpu = nullptr;

HINSTANCE g_hInstance = nullptr;

// Service information passed to the profiler, populated from the command line
// (--name / --env). Empty means "not provided" (the profiler then falls back
// to environment variables).
std::string g_serviceName;
std::string g_serviceEnv;

// Directory where .pprof files are written, populated from the command line
// (--pprofdir). Empty means "not provided" (the profiler then falls back to
// environment variables).
std::string g_pprofDir;

// Whether to symbolize call stacks, populated from the command line
// (--symbolize). Defaults to false (obfuscated call stacks).
bool g_symbolize = false;

// Automation settings, populated from the command line (--auto-hang,
// --hang-duration-ms, --hang-cycles). When g_autoEnabled is true, a worker
// thread drives the hangs headlessly and the app exits when done.
bool g_autoEnabled = false;
HangKind g_autoKind = HangKind::All;
UINT g_autoDurationMs = 1500;
UINT g_autoCycles = 2;

// Worker thread handle (owned; joined in WM_DESTROY) and the process exit code
// reported through PostQuitMessage so the CI harness can gate on it.
HANDLE g_autoThread = nullptr;
bool g_profilerStarted = false;
int g_exitCode = 0;

HFONT g_clockFont = nullptr;
int g_clockTextHeight = 0;

// A manual-reset event that is never signaled, so WaitForSingleObject on it
// always blocks until the timeout elapses (WAIT_TIMEOUT).
HANDLE g_waitEvent = nullptr;

const wchar_t* const kWindowClass = L"DDWinProfUiAppMainWindow";
const wchar_t* const kWindowTitle = L"dd-win-prof UI hangs demo";

// Busy-spins on the calling (UI) thread for the given duration, keeping one CPU
// core at 100%. Unlike Sleep/WaitForSingleObject, the thread stays runnable, so
// this reproduces a CPU-bound hang (rising thread CPU time) rather than a
// blocked one.
void BusySpin(UINT milliseconds) {
  LARGE_INTEGER freq{};
  LARGE_INTEGER start{};
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&start);

  if (freq.QuadPart == 0) {
    return;
  }

  const long long target =
      start.QuadPart + static_cast<long long>(milliseconds) * freq.QuadPart / 1000;

  volatile double sink = 0.0;
  LARGE_INTEGER now{};
  do {
    // Do a little work so the loop is never optimized into a pure spin the
    // compiler could reason away.
    sink += 1.0;
    QueryPerformanceCounter(&now);
  } while (now.QuadPart < target);

  (void)sink;
}

// Reads the freeze duration (milliseconds) from the interval text box, falling
// back to DEFAULT_INTERVAL_MS when the value is empty, zero or invalid.
UINT GetIntervalMs(HWND hwnd) {
  BOOL valid = FALSE;
  UINT milliseconds = GetDlgItemInt(hwnd, ID_EDIT_INTERVAL, &valid, FALSE);
  if (!valid || milliseconds == 0) {
    milliseconds = DEFAULT_INTERVAL_MS;
  }
  return milliseconds;
}

void CreateControls(HWND hwnd) {
  HINSTANCE instance =
      reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));

  g_editInterval = CreateWindowExW(
      WS_EX_CLIENTEDGE,
      L"EDIT",
      L"3000",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_RIGHT | ES_NUMBER,
      0,
      0,
      0,
      0,
      hwnd,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_EDIT_INTERVAL)),
      instance,
      nullptr
  );

  g_btnSleep = CreateWindowExW(
      0,
      L"BUTTON",
      L"Sleep",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BS_PUSHLIKE,
      0,
      0,
      0,
      0,
      hwnd,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_SLEEP)),
      instance,
      nullptr
  );

  g_btnWait = CreateWindowExW(
      0,
      L"BUTTON",
      L"Wait",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BS_PUSHLIKE,
      0,
      0,
      0,
      0,
      hwnd,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_WAIT)),
      instance,
      nullptr
  );

  g_btnCpu = CreateWindowExW(
      0,
      L"BUTTON",
      L"CPU hog",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BS_PUSHLIKE,
      0,
      0,
      0,
      0,
      hwnd,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_CPU)),
      instance,
      nullptr
  );

  HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  for (HWND control : {g_editInterval, g_btnSleep, g_btnWait, g_btnCpu}) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  }
}

void LayoutControls(HWND hwnd) {
  RECT rc{};
  GetClientRect(hwnd, &rc);

  const int margin = 10;
  const int spacing = 8;
  const int rowHeight = 24;
  const int buttonWidth = 80;
  const int clientWidth = rc.right - rc.left;

  const int fullWidth = clientWidth - 2 * margin;

  const int bottomY = rc.bottom - margin - rowHeight;
  const int editWidth = fullWidth - 3 * (buttonWidth + spacing);
  int x = margin;
  MoveWindow(
      g_editInterval, x, bottomY, (editWidth > 0 ? editWidth : 0), rowHeight, TRUE
  );
  x += (editWidth > 0 ? editWidth : 0) + spacing;
  MoveWindow(g_btnSleep, x, bottomY, buttonWidth, rowHeight, TRUE);
  x += buttonWidth + spacing;
  MoveWindow(g_btnWait, x, bottomY, buttonWidth, rowHeight, TRUE);
  x += buttonWidth + spacing;
  MoveWindow(g_btnCpu, x, bottomY, buttonWidth, rowHeight, TRUE);
}

// Builds a bold font 3x the size of the default GUI font for the clock display.
void CreateClockFont(HWND hwnd) {
  LOGFONTW lf{};
  HFONT baseFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  GetObjectW(baseFont, sizeof(lf), &lf);

  lf.lfHeight = lf.lfHeight * 3;
  lf.lfWidth = 0;
  lf.lfWeight = FW_BOLD;

  g_clockFont = CreateFontIndirectW(&lf);

  HDC dc = GetDC(hwnd);
  HFONT previous = static_cast<HFONT>(SelectObject(dc, g_clockFont));
  TEXTMETRICW tm{};
  GetTextMetricsW(dc, &tm);
  g_clockTextHeight = tm.tmHeight;
  SelectObject(dc, previous);
  ReleaseDC(hwnd, dc);
}

// Rectangle near the top of the client area where the clock is drawn.
RECT GetClockRect(HWND hwnd) {
  RECT client{};
  GetClientRect(hwnd, &client);

  const int margin = 10;

  RECT r{};
  r.left = client.left + margin;
  r.top = client.top + margin;
  r.right = client.right - margin;
  r.bottom = r.top + (g_clockTextHeight > 0 ? g_clockTextHeight : 48) + 4;
  return r;
}

// Automation worker thread. Runs off the UI thread so it keeps its own clock
// even while the UI thread is frozen in a hang: it posts WM_APP_HANG, waits for
// the hang plus a recovery gap, and repeats for the requested cycles/kinds.
// The hang itself happens on the UI thread inside the WM_APP_HANG handler.
DWORD WINAPI AutomationProc(LPVOID param) {
  HWND hwnd = static_cast<HWND>(param);

  // Let the window finish its initial paint and the watchdog see a responsive
  // baseline before the first hang.
  ::Sleep(kWarmupMs);

  const HangKind sequence[] = {HangKind::Sleep, HangKind::Wait, HangKind::Cpu};

  auto runKind = [&](HangKind kind) {
    for (UINT i = 0; i < g_autoCycles; ++i) {
      ::PostMessageW(hwnd, WM_APP_HANG, static_cast<WPARAM>(kind), 0);
      ::Sleep(g_autoDurationMs + kRecoveryGapMs);
    }
  };

  if (g_autoKind == HangKind::All) {
    for (HangKind kind : sequence) {
      runKind(kind);
    }
  } else {
    runKind(g_autoKind);
  }

  // Ask the UI thread to shut down cleanly (WM_DESTROY -> StopProfiler).
  ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
  return 0;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_CREATE: {
      CreateClockFont(hwnd);
      CreateControls(hwnd);
      g_waitEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      SetTimer(hwnd, ID_CLOCK_TIMER, CLOCK_INTERVAL_MS, nullptr);

      // Start the profiler and register this window for UI hang monitoring.
      // Service name/environment come from the command line (--name/--env);
      // when omitted, the profiler falls back to environment variables.
      ProfilerConfig config{};
      config.size = sizeof(config);
      config.serviceName =
          g_serviceName.empty() ? "dd-win-prof-uiapp" : g_serviceName.c_str();
      config.serviceEnvironment = g_serviceEnv.empty() ? nullptr : g_serviceEnv.c_str();
      config.pprofOutputDirectory = g_pprofDir.empty() ? nullptr : g_pprofDir.c_str();
      config.symbolizeCallstacks = g_symbolize;
      bool monitoring = false;
      if (SetupProfiler(&config)) {
        if (StartProfiler()) {
          g_profilerStarted = true;
          monitoring = MonitorWindowHangs(hwnd);
        }
      }

      // In automation mode, drive the hangs from a worker thread. If the
      // profiler or hang monitoring failed to start, there is nothing to
      // measure, so fail fast with a non-zero exit code.
      if (g_autoEnabled) {
        if (!monitoring || g_waitEvent == nullptr) {
          g_exitCode = 10;
          ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
        } else {
          // Feed the requested duration through the same textbox the manual
          // buttons read, so GetIntervalMs() stays the single source of truth.
          SetDlgItemInt(hwnd, ID_EDIT_INTERVAL, g_autoDurationMs, FALSE);
          g_autoThread =
              ::CreateThread(nullptr, 0, AutomationProc, hwnd, 0, nullptr);
          if (g_autoThread == nullptr) {
            g_exitCode = 11;
            ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
          }
        }
      }
      return 0;
    }

    case WM_TIMER:
      if (wParam == ID_CLOCK_TIMER) {
        RECT r = GetClockRect(hwnd);
        InvalidateRect(hwnd, &r, TRUE);
      } else if (wParam == ID_SLEEP_TIMER) {
        const UINT milliseconds = GetIntervalMs(hwnd);

        // Stop the timer so the frozen (Sleep) period does not overlap the
        // non-frozen waiting period, then restart it once Sleep returns.
        KillTimer(hwnd, ID_SLEEP_TIMER);
        Sleep(milliseconds);
        if (SendMessageW(g_btnSleep, BM_GETCHECK, 0, 0) == BST_CHECKED) {
          SetTimer(hwnd, ID_SLEEP_TIMER, milliseconds, nullptr);
        }
      } else if (wParam == ID_WAIT_TIMER) {
        const UINT milliseconds = GetIntervalMs(hwnd);

        // Stop the timer so the frozen (Wait) period does not overlap the
        // non-frozen waiting period, then restart it once the wait times out.
        KillTimer(hwnd, ID_WAIT_TIMER);
        WaitForSingleObject(g_waitEvent, milliseconds);
        if (SendMessageW(g_btnWait, BM_GETCHECK, 0, 0) == BST_CHECKED) {
          SetTimer(hwnd, ID_WAIT_TIMER, milliseconds, nullptr);
        }
      } else if (wParam == ID_CPU_TIMER) {
        const UINT milliseconds = GetIntervalMs(hwnd);

        // Stop the timer so the frozen (busy-spin) period does not overlap
        // the non-frozen waiting period, then restart it once the spin ends.
        KillTimer(hwnd, ID_CPU_TIMER);
        BusySpin(milliseconds);
        if (SendMessageW(g_btnCpu, BM_GETCHECK, 0, 0) == BST_CHECKED) {
          SetTimer(hwnd, ID_CPU_TIMER, milliseconds, nullptr);
        }
      }
      return 0;

    case WM_APP_HANG: {
      // Freeze the UI thread here, on purpose, using the same primitives the
      // buttons use. Because we are inside DispatchMessage, GetMessage is not
      // called for the duration, so the probe message is not dequeued and the
      // watchdog detects the hang.
      const UINT milliseconds = GetIntervalMs(hwnd);
      switch (static_cast<HangKind>(wParam)) {
        case HangKind::Sleep:
          Sleep(milliseconds);
          break;
        case HangKind::Wait:
          WaitForSingleObject(g_waitEvent, milliseconds);
          break;
        case HangKind::Cpu:
          BusySpin(milliseconds);
          break;
        default:
          break;
      }
      return 0;
    }

    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);

      SYSTEMTIME st{};
      GetLocalTime(&st);

      wchar_t buffer[64];
      wsprintfW(
          buffer,
          L"%02d:%02d:%02d.%01d",
          st.wHour,
          st.wMinute,
          st.wSecond,
          st.wMilliseconds / 100
      );

      HFONT font = g_clockFont
                       ? g_clockFont
                       : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
      HFONT previous = static_cast<HFONT>(SelectObject(hdc, font));
      const int oldBkMode = SetBkMode(hdc, TRANSPARENT);
      const COLORREF oldColor = SetTextColor(hdc, RGB(0, 0, 255));

      RECT r = GetClockRect(hwnd);
      DrawTextW(hdc, buffer, -1, &r, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOCLIP);

      SetTextColor(hdc, oldColor);
      SetBkMode(hdc, oldBkMode);
      SelectObject(hdc, previous);

      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_SIZE:
      LayoutControls(hwnd);
      return 0;

    case WM_COMMAND: {
      const int controlId = LOWORD(wParam);

      switch (controlId) {
        case ID_BTN_SLEEP:
          // BS_AUTOCHECKBOX toggles the state before this notification arrives,
          // so a checked (pushed-in) button means "start", unchecked means "stop".
          if (SendMessageW(g_btnSleep, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            SetTimer(hwnd, ID_SLEEP_TIMER, GetIntervalMs(hwnd), nullptr);
          } else {
            KillTimer(hwnd, ID_SLEEP_TIMER);
          }
          return 0;

        case ID_BTN_WAIT:
          if (SendMessageW(g_btnWait, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            SetTimer(hwnd, ID_WAIT_TIMER, GetIntervalMs(hwnd), nullptr);
          } else {
            KillTimer(hwnd, ID_WAIT_TIMER);
          }
          return 0;

        case ID_BTN_CPU:
          if (SendMessageW(g_btnCpu, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            SetTimer(hwnd, ID_CPU_TIMER, GetIntervalMs(hwnd), nullptr);
          } else {
            KillTimer(hwnd, ID_CPU_TIMER);
          }
          return 0;

        default:
          break;
      }
      break;
    }

    case WM_DESTROY:
      KillTimer(hwnd, ID_CLOCK_TIMER);
      KillTimer(hwnd, ID_SLEEP_TIMER);
      KillTimer(hwnd, ID_WAIT_TIMER);
      KillTimer(hwnd, ID_CPU_TIMER);

      // The worker posts WM_CLOSE as its last action, so by the time we get
      // here it has already finished; join and release it before stopping.
      if (g_autoThread != nullptr) {
        ::WaitForSingleObject(g_autoThread, INFINITE);
        ::CloseHandle(g_autoThread);
        g_autoThread = nullptr;
      }
      if (g_clockFont) {
        DeleteObject(g_clockFont);
        g_clockFont = nullptr;
      }
      if (g_waitEvent) {
        CloseHandle(g_waitEvent);
        g_waitEvent = nullptr;
      }
      if (g_profilerStarted) {
        StopProfiler();
      }
      PostQuitMessage(g_exitCode);
      return 0;

    default:
      break;
  }

  return DefWindowProcW(hwnd, message, wParam, lParam);
}

// Converts a wide (UTF-16) string to a UTF-8 narrow string.
std::string ToUtf8(const wchar_t* wide) {
  if (!wide || !*wide) {
    return std::string();
  }

  const int needed =
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 0) {
    return std::string();
  }

  std::string result(static_cast<size_t>(needed - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), needed, nullptr, nullptr);
  return result;
}

// Maps the --auto-hang value to a HangKind. Returns false on an unknown value.
bool ParseHangKind(const wchar_t* value, HangKind& kind) {
  if (_wcsicmp(value, L"sleep") == 0) {
    kind = HangKind::Sleep;
  } else if (_wcsicmp(value, L"wait") == 0) {
    kind = HangKind::Wait;
  } else if (_wcsicmp(value, L"cpu") == 0) {
    kind = HangKind::Cpu;
  } else if (_wcsicmp(value, L"all") == 0) {
    kind = HangKind::All;
  } else {
    return false;
  }
  return true;
}

// Parses a strictly positive unsigned integer. Returns false on non-numeric or
// zero/negative input so the caller can reject malformed automation options.
bool ParsePositiveUInt(const wchar_t* value, UINT& result) {
  wchar_t* end = nullptr;
  const long parsed = wcstol(value, &end, 10);
  if (end == value || *end != L'\0' || parsed <= 0) {
    return false;
  }
  result = static_cast<UINT>(parsed);
  return true;
}

// Parses the process command line for the service information flags:
//   --name <service>       Service name        (config.serviceName)
//   --env  <environment>   Service environment (config.serviceEnvironment)
//   --pprofdir <folder>    Output directory for .pprof files
//                          (config.pprofOutputDirectory)
//   --symbolize            Enable call stack symbolization
//                          (config.symbolizeCallstacks); no value
// Automation flags (headless CI mode):
//   --auto-hang <kind>     sleep | wait | cpu | all; enables automation
//   --hang-duration-ms <n> Freeze duration per hang (positive integer)
//   --hang-cycles <n>      Number of hangs per kind (positive integer)
// Unknown arguments are ignored. Values are stored into the g_* globals.
// Returns false if an automation option is present but malformed.
bool ParseCommandLine() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) {
    return true;
  }

  bool ok = true;
  for (int i = 1; i < argc; ++i) {
    const bool hasValue = (i + 1 < argc);
    if (_wcsicmp(argv[i], L"--symbolize") == 0) {
      g_symbolize = true;
    } else if (_wcsicmp(argv[i], L"--name") == 0 && hasValue) {
      g_serviceName = ToUtf8(argv[++i]);
    } else if (_wcsicmp(argv[i], L"--env") == 0 && hasValue) {
      g_serviceEnv = ToUtf8(argv[++i]);
    } else if (_wcsicmp(argv[i], L"--pprofdir") == 0 && hasValue) {
      g_pprofDir = ToUtf8(argv[++i]);
    } else if (_wcsicmp(argv[i], L"--auto-hang") == 0) {
      if (!hasValue || !ParseHangKind(argv[++i], g_autoKind)) {
        ok = false;
      } else {
        g_autoEnabled = true;
      }
    } else if (_wcsicmp(argv[i], L"--hang-duration-ms") == 0) {
      if (!hasValue || !ParsePositiveUInt(argv[++i], g_autoDurationMs)) {
        ok = false;
      }
    } else if (_wcsicmp(argv[i], L"--hang-cycles") == 0) {
      if (!hasValue || !ParsePositiveUInt(argv[++i], g_autoCycles)) {
        ok = false;
      }
    }
  }

  LocalFree(argv);
  return ok;
}

}  // namespace

int APIENTRY wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE /*hPrevInstance*/,
    _In_ LPWSTR /*lpCmdLine*/,
    _In_ int nCmdShow
) {
  g_hInstance = hInstance;

  // Name the main (UI) thread so it is easy to identify in the profiler,
  // which reads thread names via GetThreadDescription.
  SetThreadDescription(GetCurrentThread(), L"UIApp_Main");

  if (!ParseCommandLine()) {
    return 2;  // malformed automation option
  }

  WNDCLASSEXW wcex{};
  wcex.cbSize = sizeof(WNDCLASSEXW);
  wcex.style = CS_HREDRAW | CS_VREDRAW;
  wcex.lpfnWndProc = WndProc;
  wcex.hInstance = hInstance;
  wcex.hIcon = nullptr;
  wcex.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wcex.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wcex.lpszClassName = kWindowClass;
  wcex.hIconSm = nullptr;

  if (!RegisterClassExW(&wcex)) {
    return 0;
  }

  HWND hwnd = CreateWindowExW(
      0,
      kWindowClass,
      kWindowTitle,
      WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      580,
      240,
      nullptr,
      nullptr,
      hInstance,
      nullptr
  );

  if (!hwnd) {
    return 0;
  }

  ShowWindow(hwnd, nCmdShow);
  UpdateWindow(hwnd);

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    if (!IsDialogMessageW(hwnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }

  return static_cast<int>(msg.wParam);
}
