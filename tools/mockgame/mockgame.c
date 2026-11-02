// A game that does nothing but show where the mouse is, for measuring mouse
// to pixel latency along the whole path: client, gmstream, gamescope,
// Xwayland, Wine, the game, DXVK, gamescope, gmstream, client. It runs under
// Proton and behaves as Warcraft III does: exclusive full screen at 60 Hz
// with vsync, a visible cursor, ClipCursor and GetCursorPos every frame.
//
// The strip along the top flips between black and white on each frame that
// sees the cursor somewhere new (apple/MouseProbe.swift watches it); the
// square follows the cursor, to judge by hand. Esc quits. Each flip is logged
// to /tmp/mockgame.log with the wall clock in microseconds, and each frame's
// read, Present call and Present return as "frame read call return".
//
//   x86_64-w64-mingw32-gcc -O2 -mwindows mockgame.c -ld3d9 -o mockgame.exe

#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
	if (m == WM_DESTROY || (m == WM_KEYDOWN && wp == VK_ESCAPE)) {
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(w, m, wp, lp);
}

static int64_t wall_us(void) {
	FILETIME t;
	GetSystemTimePreciseAsFileTime(&t);
	return (int64_t)(((uint64_t)t.dwHighDateTime << 32 | t.dwLowDateTime) / 10) - 11644473600000000LL;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
	int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
	WNDCLASSW c = {.lpfnWndProc = wndproc, .hInstance = inst, .lpszClassName = L"mockgame",
	               .hCursor = LoadCursor(NULL, IDC_ARROW)};
	RegisterClassW(&c);
	HWND win = CreateWindowExW(0, L"mockgame", L"mockgame", WS_POPUP | WS_VISIBLE, 0, 0, w, h,
	                           NULL, NULL, inst, NULL);
	IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
	D3DPRESENT_PARAMETERS pp = {.BackBufferWidth = w, .BackBufferHeight = h,
	                            .BackBufferFormat = D3DFMT_X8R8G8B8, .BackBufferCount = 1,
	                            .SwapEffect = D3DSWAPEFFECT_DISCARD, .hDeviceWindow = win,
	                            .FullScreen_RefreshRateInHz = 60,
	                            .PresentationInterval = D3DPRESENT_INTERVAL_ONE};
	IDirect3DDevice9 *dev;
	if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, win,
	                                   D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
		return 1;
	FILE *log = fopen("Z:\\tmp\\mockgame.log", "w");
	POINT last = {-1, -1};
	int white = 0, frame = 0;
	for (;;) {
		MSG m;
		while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
			if (m.message == WM_QUIT) return 0;
			TranslateMessage(&m);
			DispatchMessageW(&m);
		}
		RECT clip = {2, 2, w - 2, h - 2};
		ClipCursor(&clip);
		POINT p;
		GetCursorPos(&p);
		int64_t read = wall_us();
		frame++;
		if (p.x != last.x || p.y != last.y) {
			white = !white;
			last = p;
			if (log) {
				fprintf(log, "seen %lld %ld %ld %d\n", (long long)wall_us(), p.x, p.y, frame);
				fflush(log);
			}
		}
		D3DRECT strip = {0, 0, w, h / 8};
		D3DRECT square = {p.x - 20, p.y - 20, p.x + 20, p.y + 20};
		IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(40, 40, 60), 0, 0);
		IDirect3DDevice9_Clear(dev, 1, &strip, D3DCLEAR_TARGET,
		                       white ? D3DCOLOR_XRGB(255, 255, 255) : D3DCOLOR_XRGB(0, 0, 0), 0, 0);
		IDirect3DDevice9_Clear(dev, 1, &square, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 160, 0), 0, 0);
		IDirect3DDevice9_BeginScene(dev);
		IDirect3DDevice9_EndScene(dev);
		int64_t call = wall_us();
		IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
		if (log) fprintf(log, "frame %lld %lld %lld\n", (long long)read, (long long)call, (long long)wall_us());
	}
}
