// TinyMFLoopPlayer.cpp - Decode (MP3/WAV) with Media Foundation, loop with XAudio2, hotkeys + toast
#define UNICODE
#define _UNICODE
#include <cmath> // std::round
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmreg.h>
#include <shellapi.h> // CommandLineToArgvW
#include <string>
#include <vector>
#include <windows.h>
#include <xaudio2.h>

using namespace std;

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "xaudio2.lib")

template <typename T>
static void SafeRelease(T **pp) {
    if (pp && *pp) {
        (*pp)->Release();
        *pp = nullptr;
    }
}

static void ShowHrBox(const wchar_t *what, HRESULT hr) {
    wchar_t buf[512];
    wsprintfW(buf, L"%s\nHRESULT=0x%08X", what, (unsigned)hr);
    MessageBoxW(NULL, buf, L"TinyMFLoopPlayer", MB_OK | MB_ICONERROR);
}

static std::wstring ExeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(NULL, p, MAX_PATH);
    wchar_t *b = wcsrchr(p, L'\\');
    if (b)
        *b = 0;
    return p;
}

/* ---------------- Toast（右下角提示）实现开始 ---------------- */

struct ToastData {
    std::wstring text;
    BYTE alpha = 0;         // 当前透明度
    BYTE targetAlpha = 230; // 峰值透明度（0-255）
    int phase = 0;          // 0:淡入 1:停留 2:淡出
    int holdMs = 1200;      // 停留时间
    HFONT hFont = nullptr;
    COLORREF bg = RGB(0, 0, 0);       // 背景色（黑色）
    COLORREF fg = RGB(255, 255, 255); // 前景色（白色）
    int paddingX = 16;
    int paddingY = 10;
    int corner = 16; // 圆角半径
};

static const wchar_t *kToastClass = L"TinyMFToastWndClass";

static LRESULT CALLBACK ToastWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ToastData *td = reinterpret_cast<ToastData *>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
        td = reinterpret_cast<ToastData *>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)td);
        return TRUE;
    }
    case WM_CREATE: {
        // 圆角
        RECT rc;
        GetClientRect(hWnd, &rc);
        HRGN rgn = CreateRoundRectRgn(0, 0, rc.right - rc.left, rc.bottom - rc.top, td->corner, td->corner);
        SetWindowRgn(hWnd, rgn, FALSE); // 系统接管 rgn，无需 DeleteObject
        // 初始完全透明
        SetLayeredWindowAttributes(hWnd, 0, 0, LWA_ALPHA);
        // 启动淡入
        SetTimer(hWnd, 1, 15, NULL);
        return 0;
    }
    case WM_TIMER: {
        if (!td)
            break;
        if (wParam == 1) { // 淡入
            if (td->alpha < td->targetAlpha) {
                td->alpha = (BYTE)min<int>(td->alpha + 25, td->targetAlpha);
                SetLayeredWindowAttributes(hWnd, 0, td->alpha, LWA_ALPHA);
            } else {
                KillTimer(hWnd, 1);
                // 停留
                SetTimer(hWnd, 2, td->holdMs, NULL);
            }
        } else if (wParam == 2) { // 停留结束，开始淡出
            KillTimer(hWnd, 2);
            SetTimer(hWnd, 3, 15, NULL);
        } else if (wParam == 3) { // 淡出
            if (td->alpha > 0) {
                td->alpha = (BYTE)max<int>(td->alpha - 25, 0);
                SetLayeredWindowAttributes(hWnd, 0, td->alpha, LWA_ALPHA);
            } else {
                KillTimer(hWnd, 3);
                DestroyWindow(hWnd);
            }
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        HBRUSH br = CreateSolidBrush(td ? td->bg : RGB(0, 0, 0));
        FillRect(hdc, &rc, br);
        DeleteObject(br);

        HFONT oldF = nullptr;
        if (td && td->hFont)
            oldF = (HFONT)SelectObject(hdc, td->hFont);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, td ? td->fg : RGB(255, 255, 255));

        RECT trc = rc;
        trc.left += td ? td->paddingX : 12;
        trc.right -= td ? td->paddingX : 12;
        trc.top += td ? td->paddingY : 8;
        trc.bottom -= td ? td->paddingY : 8;

        DrawTextW(hdc, td ? td->text.c_str() : L"", -1, &trc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        if (oldF)
            SelectObject(hdc, oldF);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_DESTROY: {
        if (td) {
            if (td->hFont)
                DeleteObject(td->hFont);
            delete td;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        }
        return 0;
    }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static ATOM EnsureToastClass() {
    static ATOM a = 0;
    if (a)
        return a;
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = ToastWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kToastClass;
    wc.style = CS_DROPSHADOW; // 轻微阴影（可选）
    a = RegisterClassExW(&wc);
    return a;
}

static void ShowToast(const std::wstring &msg, int holdMs = 1200, int targetAlpha = 230) {
    if (!EnsureToastClass())
        return;

    // 计算文本尺寸
    HFONT font = CreateFontW(
        -28, // 字号，负数表示像素高度
        0, 0, 0,
        FW_NORMAL, // 普通粗细（FW_BOLD 改粗体）
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, // 抗锯齿渲染
        DEFAULT_PITCH | FF_DONTCARE,
        L"01TogeGothic-ExtraLight-2" // 这里换成你想要的字体名
    );

    HDC hdc = GetDC(NULL);
    HFONT oldF = (HFONT)SelectObject(hdc, font);
    RECT rc = {0, 0, 1, 1};
    DrawTextW(hdc, msg.c_str(), -1, &rc, DT_SINGLELINE | DT_CALCRECT);
    SelectObject(hdc, oldF);
    ReleaseDC(NULL, hdc);

    // padding
    int padX = 16, padY = 10;
    int w = (rc.right - rc.left) + padX * 2;
    int h = (rc.bottom - rc.top) + padY * 2;

    // 放在工作区右下角
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.right - w - 24;
    int y = wa.bottom - h - 24;

    // 准备数据
    ToastData *td = new ToastData();
    td->text = msg;
    td->targetAlpha = (BYTE)max<int>(0, min<int>(targetAlpha, 255));
    td->holdMs = holdMs;
    td->paddingX = padX;
    td->paddingY = padY;
    td->hFont = font; // 用系统默认 GUI 字体

    HWND hWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        kToastClass, L"", WS_POPUP,
        x, y, w, h, NULL, NULL, GetModuleHandleW(NULL), td);

    if (!hWnd) {
        delete td;
        return;
    }

    ShowWindow(hWnd, SW_SHOWNOACTIVATE);
    SetWindowPos(hWnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

/* ---------------- Toast 实现结束 ---------------- */

static HRESULT ReadPcmAll(const wchar_t *path, WAVEFORMATEX **ppWfx, UINT32 *pcbWfx, std::vector<BYTE> &pcm) {
    *ppWfx = nullptr;
    *pcbWfx = 0;
    pcm.clear();
    IMFSourceReader *rdr = nullptr;
    IMFMediaType *outType = nullptr;
    IMFMediaType *curType = nullptr;
    HRESULT hr = S_OK;

    hr = MFCreateSourceReaderFromURL(path, nullptr, &rdr);
    if (FAILED(hr))
        goto done;

    // 要求输出为 PCM（试 48k/44.1k，双声道，16bit）
    {
        IMFMediaType *req = nullptr;
        hr = MFCreateMediaType(&req);
        if (FAILED(hr))
            goto done;
        hr = req->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (FAILED(hr)) {
            req->Release();
            goto done;
        }
        hr = req->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        if (FAILED(hr)) {
            req->Release();
            goto done;
        }
        req->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        req->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        // 优先 48000，失败退 44100
        req->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        hr = rdr->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, req);
        if (FAILED(hr)) {
            req->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
            hr = rdr->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, req);
        }
        req->Release();
        if (FAILED(hr))
            goto done;
    }

    hr = rdr->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &curType);
    if (FAILED(hr))
        goto done;

    // 拿到 WAVEFORMATEX
    hr = MFCreateWaveFormatExFromMFMediaType(curType, ppWfx, pcbWfx);
    if (FAILED(hr))
        goto done;

    // 读完整个音频为连续 PCM
    for (;;) {
        DWORD flags = 0;
        IMFSample *smp = nullptr;
        hr = rdr->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &smp);
        if (FAILED(hr))
            goto done;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            break;
        }
        if (smp) {
            IMFMediaBuffer *buf = nullptr;
            hr = smp->ConvertToContiguousBuffer(&buf);
            if (SUCCEEDED(hr)) {
                BYTE *p = nullptr;
                DWORD maxLen = 0, curLen = 0;
                if (SUCCEEDED(buf->Lock(&p, &maxLen, &curLen))) {
                    pcm.insert(pcm.end(), p, p + curLen);
                    buf->Unlock();
                }
            }
            SafeRelease(&buf);
            SafeRelease(&smp);
        }
    }

    // 对齐到样本边界
    if (!pcm.empty() && *ppWfx) {
        size_t ba = (*ppWfx)->nBlockAlign;
        size_t r = pcm.size() % ba;
        if (r)
            pcm.resize(pcm.size() - r);
    }

done:
    SafeRelease(&outType);
    SafeRelease(&curType);
    SafeRelease(&rdr);
    return hr;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);

    // 解析路径：默认 exe 同目录 bgm.mp3，也可拖拽文件到 exe 上
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring file = (argc >= 2) ? argv[1] : (ExeDir() + L"\\bgm.mp3");
    if (argv)
        LocalFree(argv);
    SetCurrentDirectoryW(ExeDir().c_str());

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        ShowHrBox(L"CoInitializeEx 失败", hr);
        return 1;
    }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        ShowHrBox(L"MFStartup 失败", hr);
        return 1;
    }

    WAVEFORMATEX *wfx = nullptr;
    UINT32 cbwfx = 0;
    std::vector<BYTE> pcm;
    hr = ReadPcmAll(file.c_str(), &wfx, &cbwfx, pcm);
    if (FAILED(hr) || pcm.empty()) {
        ShowHrBox(L"解码音频失败（路径/格式/解码器）", hr);
        return 1;
    }

    IXAudio2 *xa = nullptr;
    hr = XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        ShowHrBox(L"XAudio2Create 失败", hr);
        return 1;
    }

    IXAudio2MasteringVoice *master = nullptr;
    if (FAILED(xa->CreateMasteringVoice(&master))) {
        ShowHrBox(L"CreateMasteringVoice 失败", E_FAIL);
        return 1;
    }

    IXAudio2SourceVoice *src = nullptr;
    if (FAILED(xa->CreateSourceVoice(&src, wfx))) {
        ShowHrBox(L"CreateSourceVoice 失败", E_FAIL);
        return 1;
    }

    XAUDIO2_BUFFER buf = {};
    buf.pAudioData = pcm.data();
    buf.AudioBytes = (UINT32)pcm.size();
    buf.LoopCount = XAUDIO2_LOOP_INFINITE; // 无限循环

    float currentVolume = 0.2f; // 默认音量
    src->SetVolume(currentVolume);

    if (FAILED(src->SubmitSourceBuffer(&buf))) {
        ShowHrBox(L"SubmitSourceBuffer 失败", E_FAIL);
        return 1;
    }
    if (FAILED(src->Start(0))) {
        ShowHrBox(L"Start 播放失败", E_FAIL);
        return 1;
    }

    // 注册全局热键
    if (!RegisterHotKey(NULL, 1, MOD_CONTROL | MOD_SHIFT, 'Z')) {
        MessageBoxW(NULL, L"注册播放/暂停热键失败（Ctrl+Shift+Z）", L"TinyMFLoopPlayer", MB_OK | MB_ICONERROR);
    }
    if (!RegisterHotKey(NULL, 2, MOD_CONTROL | MOD_SHIFT, VK_UP)) {
        MessageBoxW(NULL, L"注册音量加热键失败（Ctrl+Alt+↑）", L"TinyMFLoopPlayer", MB_OK | MB_ICONERROR);
    }
    if (!RegisterHotKey(NULL, 3, MOD_CONTROL | MOD_SHIFT, VK_DOWN)) {
        MessageBoxW(NULL, L"注册音量减热键失败（Ctrl+Alt+↓）", L"TinyMFLoopPlayer", MB_OK | MB_ICONERROR);
    }

    bool isPlaying = true; // 初始状态：正在播放

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_HOTKEY) {
            if (msg.wParam == 1) { // 播放/暂停（带淡入淡出）
                auto FadeOut = [&](IXAudio2SourceVoice *v, float seconds = 1.0f) {
                    const int steps = 20; // 20 步
                    float vol;
                    v->GetVolume(&vol);
                    float step = (steps > 0 ? vol / steps : vol);
                    for (int i = 0; i < steps; i++) {
                        vol -= step;
                        if (vol < 0.0f)
                            vol = 0.0f;
                        v->SetVolume(vol);
                        Sleep((DWORD)(seconds * 1000 / steps));
                    }
                };
                auto FadeIn = [&](IXAudio2SourceVoice *v, float seconds = 1.0f) {
                    const int steps = 20;
                    float vol = 0.0f;
                    v->SetVolume(vol);
                    v->Start(0);
                    float step = (steps > 0 ? currentVolume / steps : currentVolume);
                    for (int i = 0; i < steps; i++) {
                        vol += step;
                        if (vol > currentVolume)
                            vol = currentVolume;
                        v->SetVolume(vol);
                        Sleep((DWORD)(seconds * 1000 / steps));
                    }
                };

                if (isPlaying) {
                    FadeOut(src, 0.9f); // 0.9 秒淡出
                    src->Stop(0);
                    isPlaying = false;
                    ShowToast(L"已暂停", 900);
                } else {
                    FadeIn(src, 0.9f); // 0.9 秒淡入至 currentVolume
                    isPlaying = true;
                    ShowToast(L"继续播放", 900);
                }
            } else if (msg.wParam == 2) { // 音量加
                currentVolume += 0.05f;
                if (currentVolume > 1.0f)
                    currentVolume = 1.0f;
                src->SetVolume(currentVolume);
                int pct = (int)std::round(currentVolume * 100.0f);
                ShowToast(L"音量 " + std::to_wstring(pct) + L"%");
            } else if (msg.wParam == 3) { // 音量减
                currentVolume -= 0.05f;
                if (currentVolume < 0.0f)
                    currentVolume = 0.0f;
                src->SetVolume(currentVolume);
                int pct = (int)std::round(currentVolume * 100.0f);
                ShowToast(L"音量 " + std::to_wstring(pct) + L"%");
            }
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}
