/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * uds-esp32-setup: sets up an ESP32-S3 board for Game Boy Virtual Console trades with a 3DS, with no Python, ESP-IDF or mGBA. It
 * flashes a release image of Azahar's esp32-uds-bridge firmware (uds-esp32-flash.c) and stores the 3DS UDS key from the user's
 * aes_keys.txt on the board (the same request as uds-esp32-probe --store-key; only the one 16-byte key leaves the PC, and it is never
 * shown or logged). Either step can be done on its own. Flashing writes the whole image, which erases the key store, so with both
 * ticked the key is stored after the flash.
 *
 * Windows only (Win32, no other dependencies). Build target: uds-esp32-setup.
 */
#include <mgba/internal/gb/sio/uds-esp32-flash.h>
#include <mgba/internal/gb/sio/uds-esp32.h>
#include <mgba/internal/gb/sio/uds-keyfile.h>

#include "../../gba/sio/esp32/esp32-serial.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dbt.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#define TITLE "UDS ESP32 Setup"
#define SETTINGS_KEY "Software\\mGBA-LDN\\uds-esp32-setup"
#define MAX_PORTS 8

enum {
	ID_FW_CHECK = 100,
	ID_FW_EDIT,
	ID_FW_BROWSE,
	ID_FW_INFO,
	ID_KEY_CHECK,
	ID_KEY_EDIT,
	ID_KEY_BROWSE,
	ID_PORT_LABEL,
	ID_PORT_COMBO,
	ID_PORT_REFRESH,
	ID_START,
	ID_PROGRESS,
	ID_LOG,
};

enum {
	WM_APP_LOG = WM_APP + 1,
	WM_APP_PROGRESS,
	WM_APP_DONE,
};

static HWND sWindow;
static HFONT sFont;
static int sDpi = 96;
static bool sBusy;
static struct Esp32SerialPortInfo sPorts[MAX_PORTS];
static size_t sPortCount;

struct Job {
	bool flash;
	bool key;
	char firmware[MAX_PATH];
	char keys[MAX_PATH];
	char port[32];
	bool usbJtag;
};

static int _px(int value) {
	return MulDiv(value, sDpi, 96);
}

static HWND _item(int id) {
	return GetDlgItem(sWindow, id);
}

// Settings: the two paths, so that updating a board later is one click. The key itself is never stored here.
static void _loadSetting(const char* name, char* out, DWORD capacity) {
	DWORD size = capacity;
	if (RegGetValueA(HKEY_CURRENT_USER, SETTINGS_KEY, name, RRF_RT_REG_SZ, NULL, out, &size) != ERROR_SUCCESS) {
		out[0] = 0;
	}
}

static void _saveSetting(const char* name, const char* value) {
	RegSetKeyValueA(HKEY_CURRENT_USER, SETTINGS_KEY, name, REG_SZ, value, (DWORD) strlen(value) + 1);
}

// Worker → window ----------------------------------------------------------------------------------------------------------------

static void _log(const char* format, ...) {
	char text[512];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	PostMessageA(sWindow, WM_APP_LOG, 0, (LPARAM) _strdup(text));
}

static void _flashLog(void* context, const char* text) {
	(void) context;
	_log("%s", text);
}

static void _flashProgress(void* context, size_t done, size_t total) {
	(void) context;
	PostMessageA(sWindow, WM_APP_PROGRESS, (WPARAM) (total ? done * 1000 / total : 0), 0);
}

static void _finish(bool ok, const char* summary) {
	PostMessageA(sWindow, WM_APP_DONE, ok, (LPARAM) _strdup(summary));
}

static uint8_t* _readFile(const char* path, size_t* size) {
	FILE* file = fopen(path, "rb");
	if (!file) {
		return NULL;
	}
	uint8_t* data = NULL;
	if (fseek(file, 0, SEEK_END) == 0) {
		long length = ftell(file);
		if (length > 0 && length <= (64 << 20) && fseek(file, 0, SEEK_SET) == 0) {
			data = malloc((size_t) length);
			if (data && fread(data, 1, (size_t) length, file) != (size_t) length) {
				free(data);
				data = NULL;
			}
			*size = (size_t) length;
		}
	}
	fclose(file);
	return data;
}

// After a flash the board restarts, and its USB port may come back a moment later: keep trying to open it.
static bool _openBoard(struct UDSEsp32* esp, const char* port, unsigned timeoutMs) {
	static const struct UDSEsp32Handlers handlers = {0};
	ULONGLONG start = GetTickCount64();
	do {
		if (udsEsp32Open(esp, port, &handlers)) {
			return true;
		}
		Sleep(500);
	} while (GetTickCount64() - start < timeoutMs);
	return false;
}

static DWORD WINAPI _work(LPVOID param) {
	struct Job* job = param;
	uint8_t key[16];
	uint8_t* image = NULL;
	size_t size = 0;
	bool ok = false;
	char error[400] = "";
	const char* summary = "";

	// Everything that can be checked is checked before the board is touched.
	if (job->key) {
		enum UDSKeyStatus status = udsKeyFileLoad(job->keys, key);
		if (!udsKeyStatusOk(status)) {
			_log("Keys: %s", udsKeyStatusText(status));
			summary = "The key file has no usable 3DS UDS key (slot 0x2D). Nothing was changed on the board.";
			goto done;
		}
		_log("Keys: %s", udsKeyStatusText(status));
	}
	if (job->flash) {
		image = _readFile(job->firmware, &size);
		if (!image) {
			_log("Firmware: cannot read %s", job->firmware);
			summary = "The firmware file cannot be read. Nothing was changed on the board.";
			goto done;
		}
		struct UDSFlashImageInfo info;
		if (!udsFlashCheckImage(image, size, &info, error, sizeof(error))) {
			_log("Firmware: %s", error);
			summary = "That firmware file cannot be flashed. Nothing was changed on the board.";
			goto done;
		}
		_log("Firmware: %s %s, built %s %s (%u KB)", info.project[0] ? info.project : "(unnamed)", info.version, info.date, info.time,
		     (unsigned) ((size + 1023) / 1024));

		struct UDSFlashPort* port = udsFlashOpenPort(job->port);
		if (!port) {
			_log("Cannot open %s. Close mGBA, Azahar or any serial monitor that is using the board, then try again.", job->port);
			summary = "The board's port could not be opened. Nothing was changed on the board.";
			goto done;
		}
		struct UDSFlashHandlers handlers = {NULL, _flashLog, _flashProgress};
		struct UDSFlashOptions options = {job->usbJtag};
		bool flashed = udsFlashWriteImage(port, &handlers, &options, image, size, error, sizeof(error));
		udsFlashClosePort(port);
		if (!flashed) {
			_log("Flashing failed: %s", error);
			summary = "Flashing failed (see the log).";
			goto done;
		}
		_log("Waiting for the new firmware to start");
		Sleep(1500);
	}

	// The firmware: say hello, then the key.
	{
		struct UDSEsp32 esp;
		_log("Talking to the firmware on %s", job->port);
		if (!_openBoard(&esp, job->port, 10000) && !_openBoard(&esp, NULL, 3000)) {
			_log("The board's port did not come back. Unplug the board and plug it in again, then store the key on its own.");
			summary = job->flash ? "Flashed, but the board could not be reached afterwards (see the log)." : "The board's port could not be opened.";
			goto done;
		}
		bool ready = udsEsp32WaitReady(&esp, 15000);
		if (!ready) {
			// Still in download mode? Restart it into the firmware once more and ask again.
			udsEsp32Close(&esp);
			struct UDSFlashPort* port = udsFlashOpenPort(job->port);
			if (port) {
				_log("No answer yet: restarting the board into its firmware");
				struct UDSFlashOptions options = {job->usbJtag};
				udsFlashRestart(port, &options);
				udsFlashClosePort(port);
				Sleep(1500);
				ready = _openBoard(&esp, job->port, 10000) && udsEsp32WaitReady(&esp, 15000);
			}
		}
		if (!ready) {
			udsEsp32Close(&esp);
			_log("The firmware did not answer. A board running another firmware (the GB-Link Switch LDN firmware, for one) does not speak "
			     "this protocol: flash esp32-uds-bridge first.");
			summary = "The board's firmware did not answer (see the log).";
			goto done;
		}
		_log("Firmware %u.%u is running (protocol %u)", esp.info.major, esp.info.minor, esp.info.proto);
		if (job->key) {
			if (!udsEsp32HasGbWrapper(&esp)) {
				udsEsp32Close(&esp);
				_log("This firmware keeps no keys: version 1.4 or later is needed");
				summary = "The board's firmware is too old to keep the key. Flash 1.4 or later.";
				goto done;
			}
			bool stored = udsEsp32SetKey(&esp, UDS_ESP32_KEY_SLOT_DATA, key, 3000);
			int present = stored ? udsEsp32KeyStatus(&esp, 1000) : -1;
			udsEsp32Close(&esp);
			if (!stored || present != 1) {
				_log("The board did not store the key");
				summary = "The key was not stored (see the log).";
				goto done;
			}
			_log("The 3DS UDS key is stored on the board");
		} else {
			udsEsp32Close(&esp);
		}
	}
	ok = true;
	summary = job->flash && job->key ? "Done: the firmware is flashed and the key is stored. The board is ready."
	        : job->flash             ? "Done: the firmware is flashed. Store the key before trading (flashing erased it)."
	                                 : "Done: the key is stored on the board.";

done:
	memset(key, 0, sizeof(key));
	free(image);
	free(job);
	_finish(ok, summary);
	return 0;
}

// The window ---------------------------------------------------------------------------------------------------------------------

static void _appendLog(const char* text) {
	HWND log = _item(ID_LOG);
	int length = GetWindowTextLengthA(log);
	SendMessageA(log, EM_SETSEL, length, length);
	SendMessageA(log, EM_REPLACESEL, FALSE, (LPARAM) text);
	SendMessageA(log, EM_REPLACESEL, FALSE, (LPARAM) "\r\n");
}

static void _refreshPorts(void) {
	HWND combo = _item(ID_PORT_COMBO);
	char selected[32] = "";
	int current = (int) SendMessageA(combo, CB_GETCURSEL, 0, 0);
	if (current >= 0 && (size_t) current < sPortCount) {
		snprintf(selected, sizeof(selected), "%s", sPorts[current].name);
	}
	SendMessageA(combo, CB_RESETCONTENT, 0, 0);
	sPortCount = Esp32SerialListEspressif(sPorts, MAX_PORTS);
	int pick = 0;
	size_t i;
	for (i = 0; i < sPortCount; ++i) {
		char text[96];
		snprintf(text, sizeof(text), "%s  (%s)", sPorts[i].name, sPorts[i].description[0] ? sPorts[i].description : "USB serial");
		SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM) text);
		if (!strcmp(sPorts[i].name, selected)) {
			pick = (int) i;
		}
	}
	if (!sPortCount) {
		SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM) "No board found: plug it in by its native USB port");
	}
	SendMessageA(combo, CB_SETCURSEL, pick, 0);
}

static void _showFirmwareInfo(void) {
	char path[MAX_PATH];
	GetWindowTextA(_item(ID_FW_EDIT), path, sizeof(path));
	char text[200] = "";
	if (path[0]) {
		size_t size = 0;
		uint8_t* image = _readFile(path, &size);
		struct UDSFlashImageInfo info;
		char why[300];
		if (!image) {
			snprintf(text, sizeof(text), "The file cannot be read");
		} else if (!udsFlashCheckImage(image, size, &info, why, sizeof(why))) {
			snprintf(text, sizeof(text), "%s", why);
		} else {
			snprintf(text, sizeof(text), "%s, built %s %s", info.project[0] ? info.project : "firmware", info.date, info.time);
		}
		free(image);
	}
	SetWindowTextA(_item(ID_FW_INFO), text);
}

static void _updateEnabled(void) {
	bool flash = IsDlgButtonChecked(sWindow, ID_FW_CHECK) == BST_CHECKED;
	bool key = IsDlgButtonChecked(sWindow, ID_KEY_CHECK) == BST_CHECKED;
	EnableWindow(_item(ID_FW_CHECK), !sBusy);
	EnableWindow(_item(ID_KEY_CHECK), !sBusy);
	EnableWindow(_item(ID_FW_EDIT), !sBusy && flash);
	EnableWindow(_item(ID_FW_BROWSE), !sBusy && flash);
	EnableWindow(_item(ID_KEY_EDIT), !sBusy && key);
	EnableWindow(_item(ID_KEY_BROWSE), !sBusy && key);
	EnableWindow(_item(ID_PORT_COMBO), !sBusy);
	EnableWindow(_item(ID_PORT_REFRESH), !sBusy);
	EnableWindow(_item(ID_START), !sBusy && (flash || key));
	SetWindowTextA(_item(ID_START), sBusy ? "Working..." : "Start");
}

static void _browse(int editId, const char* filter, const char* title) {
	char path[MAX_PATH];
	GetWindowTextA(_item(editId), path, sizeof(path));
	OPENFILENAMEA ofn;
	memset(&ofn, 0, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = sWindow;
	ofn.lpstrFilter = filter;
	ofn.lpstrFile = path;
	ofn.nMaxFile = sizeof(path);
	ofn.lpstrTitle = title;
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
	if (GetOpenFileNameA(&ofn)) {
		SetWindowTextA(_item(editId), path);
		if (editId == ID_FW_EDIT) {
			_showFirmwareInfo();
		}
	}
}

static void _start(void) {
	struct Job* job = calloc(1, sizeof(*job));
	job->flash = IsDlgButtonChecked(sWindow, ID_FW_CHECK) == BST_CHECKED;
	job->key = IsDlgButtonChecked(sWindow, ID_KEY_CHECK) == BST_CHECKED;
	GetWindowTextA(_item(ID_FW_EDIT), job->firmware, sizeof(job->firmware));
	GetWindowTextA(_item(ID_KEY_EDIT), job->keys, sizeof(job->keys));
	int current = (int) SendMessageA(_item(ID_PORT_COMBO), CB_GETCURSEL, 0, 0);
	const char* problem = NULL;
	if (current < 0 || (size_t) current >= sPortCount) {
		problem = "No board is selected. Plug the board in by its native USB port and press Refresh.";
	} else if (job->flash && !job->firmware[0]) {
		problem = "Choose the firmware file (esp32-uds-bridge-fw<version>-esp32s3.bin).";
	} else if (job->key && !job->keys[0]) {
		problem = "Choose your aes_keys.txt.";
	}
	if (problem) {
		free(job);
		MessageBoxA(sWindow, problem, TITLE, MB_OK | MB_ICONINFORMATION);
		return;
	}
	snprintf(job->port, sizeof(job->port), "%s", sPorts[current].name);
	job->usbJtag = !strcmp(sPorts[current].description, "Espressif USB");
	if (job->flash) {
		_saveSetting("firmware", job->firmware);
	}
	if (job->key) {
		_saveSetting("keys", job->keys);
	}
	SetWindowTextA(_item(ID_LOG), "");
	SendMessageA(_item(ID_PROGRESS), PBM_SETPOS, 0, 0);
	sBusy = true;
	_updateEnabled();
	HANDLE thread = CreateThread(NULL, 0, _work, job, 0, NULL);
	if (thread) {
		CloseHandle(thread);
	} else {
		free(job);
		sBusy = false;
		_updateEnabled();
	}
}

static HWND _control(const char* cls, const char* text, DWORD style, int id, int x, int y, int w, int h) {
	HWND hwnd = CreateWindowExA(!strcmp(cls, "EDIT") && !(style & ES_READONLY) ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style,
	                            _px(x), _px(y), _px(w), _px(h), sWindow, (HMENU) (INT_PTR) id, GetModuleHandleA(NULL), NULL);
	SendMessageA(hwnd, WM_SETFONT, (WPARAM) sFont, TRUE);
	return hwnd;
}

// The newest esp32-uds-bridge release image next to this program, as a release folder holds them.
static void _defaultFirmware(char* out, size_t capacity) {
	char dir[MAX_PATH];
	DWORD length = GetModuleFileNameA(NULL, dir, sizeof(dir));
	char* slash = length ? strrchr(dir, '\\') : NULL;
	out[0] = 0;
	if (!slash) {
		return;
	}
	slash[1] = 0;
	char pattern[MAX_PATH + 40];
	snprintf(pattern, sizeof(pattern), "%sesp32-uds-bridge-fw*-esp32s3.bin", dir);
	WIN32_FIND_DATAA found;
	HANDLE find = FindFirstFileA(pattern, &found);
	if (find == INVALID_HANDLE_VALUE) {
		return;
	}
	FILETIME newest = {0};
	do {
		if (CompareFileTime(&found.ftLastWriteTime, &newest) > 0) {
			newest = found.ftLastWriteTime;
			snprintf(out, capacity, "%s%s", dir, found.cFileName);
		}
	} while (FindNextFileA(find, &found));
	FindClose(find);
}

// Where Azahar (or Citra) keeps aes_keys.txt, if it is there.
static void _defaultKeys(char* out, size_t capacity) {
	static const char* const kApps[] = {"Azahar", "Citra"};
	char appData[MAX_PATH];
	out[0] = 0;
	if (!GetEnvironmentVariableA("APPDATA", appData, sizeof(appData))) {
		return;
	}
	size_t i;
	for (i = 0; i < sizeof(kApps) / sizeof(kApps[0]); ++i) {
		snprintf(out, capacity, "%s\\%s\\sysdata\\aes_keys.txt", appData, kApps[i]);
		if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) {
			return;
		}
	}
	out[0] = 0;
}

static void _create(void) {
	_control("BUTTON", "Flash this firmware:", BS_AUTOCHECKBOX | WS_TABSTOP, ID_FW_CHECK, 12, 12, 400, 20);
	_control("EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, ID_FW_EDIT, 30, 34, 450, 24);
	_control("BUTTON", "Browse...", BS_PUSHBUTTON | WS_TABSTOP, ID_FW_BROWSE, 488, 33, 100, 26);
	_control("STATIC", "", SS_LEFT | SS_ENDELLIPSIS, ID_FW_INFO, 32, 62, 556, 18);
	_control("BUTTON", "Store the 3DS key from:", BS_AUTOCHECKBOX | WS_TABSTOP, ID_KEY_CHECK, 12, 90, 400, 20);
	_control("EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, ID_KEY_EDIT, 30, 112, 450, 24);
	_control("BUTTON", "Browse...", BS_PUSHBUTTON | WS_TABSTOP, ID_KEY_BROWSE, 488, 111, 100, 26);
	_control("STATIC", "Board:", SS_LEFT, ID_PORT_LABEL, 12, 152, 60, 20);
	_control("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_PORT_COMBO, 72, 148, 408, 200);
	_control("BUTTON", "Refresh", BS_PUSHBUTTON | WS_TABSTOP, ID_PORT_REFRESH, 488, 147, 100, 26);
	_control("BUTTON", "Start", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_START, 12, 188, 140, 30);
	_control(PROGRESS_CLASSA, "", 0, ID_PROGRESS, 164, 194, 424, 18);
	SendMessageA(_item(ID_PROGRESS), PBM_SETRANGE32, 0, 1000);
	_control("EDIT", "", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER, ID_LOG, 12, 228, 576, 190);

	char path[MAX_PATH];
	_defaultFirmware(path, sizeof(path));
	if (!path[0]) {
		_loadSetting("firmware", path, sizeof(path));
	}
	SetWindowTextA(_item(ID_FW_EDIT), path);
	_loadSetting("keys", path, sizeof(path));
	if (!path[0]) {
		_defaultKeys(path, sizeof(path));
	}
	SetWindowTextA(_item(ID_KEY_EDIT), path);
	CheckDlgButton(sWindow, ID_FW_CHECK, BST_CHECKED);
	CheckDlgButton(sWindow, ID_KEY_CHECK, BST_CHECKED);
	_showFirmwareInfo();
	_refreshPorts();
	_updateEnabled();
}

static LRESULT CALLBACK _windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
	switch (message) {
	case WM_CREATE:
		sWindow = hwnd;
		_create();
		return 0;
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case ID_FW_CHECK:
		case ID_KEY_CHECK:
			_updateEnabled();
			break;
		case ID_FW_BROWSE:
			_browse(ID_FW_EDIT, "Firmware image (*.bin)\0*.bin\0All files\0*.*\0", "Choose the esp32-uds-bridge firmware");
			break;
		case ID_KEY_BROWSE:
			_browse(ID_KEY_EDIT, "Key files (*.txt)\0*.txt\0All files\0*.*\0", "Choose your aes_keys.txt");
			break;
		case ID_FW_EDIT:
			if (HIWORD(wParam) == EN_KILLFOCUS) {
				_showFirmwareInfo();
			}
			break;
		case ID_PORT_REFRESH:
			_refreshPorts();
			break;
		case ID_START:
			_start();
			break;
		}
		return 0;
	case WM_DEVICECHANGE:
		if (wParam == DBT_DEVNODES_CHANGED && !sBusy) {
			_refreshPorts();
		}
		return TRUE;
	case WM_APP_LOG:
		_appendLog((const char*) lParam);
		free((void*) lParam);
		return 0;
	case WM_APP_PROGRESS:
		SendMessageA(_item(ID_PROGRESS), PBM_SETPOS, wParam, 0);
		return 0;
	case WM_APP_DONE:
		sBusy = false;
		_updateEnabled();
		_appendLog((const char*) lParam);
		MessageBoxA(hwnd, (const char*) lParam, TITLE, MB_OK | (wParam ? MB_ICONINFORMATION : MB_ICONWARNING));
		free((void*) lParam);
		_refreshPorts();
		return 0;
	case WM_CLOSE:
		if (sBusy && MessageBoxA(hwnd, "The board is being set up. Closing now can leave it without working firmware. Close anyway?", TITLE,
		                         MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
			return 0;
		}
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcA(hwnd, message, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR commandLine, int show) {
	(void) previous;
	(void) commandLine;
	SetProcessDPIAware();
	HDC screen = GetDC(NULL);
	sDpi = GetDeviceCaps(screen, LOGPIXELSX);
	ReleaseDC(NULL, screen);
	INITCOMMONCONTROLSEX common = {sizeof(common), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
	InitCommonControlsEx(&common);
	NONCLIENTMETRICSA metrics;
	memset(&metrics, 0, sizeof(metrics));
	metrics.cbSize = sizeof(metrics);
	SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
	sFont = CreateFontIndirectA(&metrics.lfMessageFont);

	WNDCLASSA wc;
	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = _windowProc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
	wc.hbrBackground = (HBRUSH) (COLOR_BTNFACE + 1);
	wc.lpszClassName = "UdsEsp32Setup";
	RegisterClassA(&wc);

	DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
	RECT rect = {0, 0, _px(600), _px(430)};
	AdjustWindowRect(&rect, style, FALSE);
	HWND hwnd = CreateWindowExA(0, wc.lpszClassName, TITLE, style, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
	                            NULL, NULL, instance, NULL);
	if (!hwnd) {
		return 1;
	}
	ShowWindow(hwnd, show);
	UpdateWindow(hwnd);
	MSG msg;
	while (GetMessageA(&msg, NULL, 0, 0) > 0) {
		if (!IsDialogMessageA(hwnd, &msg)) {
			TranslateMessage(&msg);
			DispatchMessageA(&msg);
		}
	}
	DeleteObject(sFont);
	return 0;
}
