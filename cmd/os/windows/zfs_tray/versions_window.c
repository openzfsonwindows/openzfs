// SPDX-License-Identifier: CDDL-1.0
/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * https://opensource.org/license/CDDL-1.0.
 */

/*
 * Copyright (c) 2025 Jorgen Lundman <lundman@lundman.net>.
 */

// cmd/os/windows/zfs_tray/versions_window.c
//
// "ZFS versions": lists the snapshots that hold an older version of a file
// or directory, so it can be opened, copied or restored without mounting
// snapshots by hand. Started as "zfs_tray.exe --versions <path>".
#define	_CRT_SECURE_NO_WARNINGS
#define	UNICODE
#define	_UNICODE
#define	_WIN32_WINNT 0x0600
#define	_WIN32_IE    0x0600
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define	JSMN_STATIC
#include "jsmn.h"
#include "jsmn_utils.h"

#include "pipe_rpc.h"
#include "rpc_client.h"
#include "versions_window.h"
#include "dlg_util.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")

#ifndef ARRAYSIZE
#define	ARRAYSIZE(a) (sizeof (a)/sizeof ((a)[0]))
#endif

#define	IDC_VLIST	3001
#define	IDC_VOPEN	3002
#define	IDC_VCOPY	3003
#define	IDC_VRESTORE	3004
#define	IDC_VCHANGED	3005
#define	IDC_VINFO	3006
#define	IDC_VDELETED	3007

typedef struct {
	wchar_t snap[128];
	wchar_t path[MAX_PATH * 2]; // version path via <mount>\.zfs\snapshot
	uint64_t creation;
	uint64_t size;
	uint64_t mtime;
	BOOL same_as_live;
	BOOL changed;
} Version;

typedef struct {
	HWND hWnd;
	HWND hList;
	HFONT hHeaderFont;
	wchar_t path[MAX_PATH * 2]; // the file/dir the user asked about
	BOOL is_dir;
	uint64_t live_size;
	uint64_t live_mtime;
	Version *vers;
	int nvers;
} VCtx;

// -------------------- formatting --------------------

static void
FormatBytesW(uint64_t bytes, wchar_t *out, size_t outcch)
{
	static const wchar_t *units[] = { L"B", L"KB", L"MB", L"GB", L"TB",
	    L"PB" };
	double v = (double)bytes;
	int u = 0;
	while (v >= 1024.0 && u < (int)ARRAYSIZE(units) - 1) {
		v /= 1024.0;
		u++;
	}
	if (u == 0)
		_snwprintf_s(out, outcch, _TRUNCATE, L"%.0f %s", v, units[u]);
	else
		_snwprintf_s(out, outcch, _TRUNCATE, L"%.2f %s", v, units[u]);
}

// Unix seconds -> local "date time"
static void
FormatTimeW(uint64_t secs, wchar_t *out, size_t outcch)
{
	ULARGE_INTEGER u;
	FILETIME ft;
	SYSTEMTIME st, lt;
	wchar_t d[64], t[64];

	u.QuadPart = (secs + 11644473600ULL) * 10000000ULL;
	ft.dwLowDateTime = u.LowPart;
	ft.dwHighDateTime = u.HighPart;
	if (!FileTimeToSystemTime(&ft, &st) ||
	    !SystemTimeToTzSpecificLocalTime(NULL, &st, &lt)) {
		out[0] = L'\0';
		return;
	}
	GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &lt, NULL, d,
	    ARRAYSIZE(d));
	GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &lt, NULL, t,
	    ARRAYSIZE(t));
	_snwprintf_s(out, outcch, _TRUNCATE, L"%s %s", d, t);
}

static uint64_t
NowUnix(void)
{
	FILETIME ft;
	ULARGE_INTEGER u;

	GetSystemTimeAsFileTime(&ft);
	u.LowPart = ft.dwLowDateTime;
	u.HighPart = ft.dwHighDateTime;
	return (u.QuadPart / 10000000ULL - 11644473600ULL);
}

// "3 days ago"
static void
FormatAgeW(uint64_t then, wchar_t *out, size_t outcch)
{
	uint64_t now = NowUnix();
	uint64_t d = (then > now) ? 0 : now - then;
	uint64_t n;
	const wchar_t *unit;

	if (d < 60) {
		_snwprintf_s(out, outcch, _TRUNCATE, L"just now");
		return;
	} else if (d < 3600) {
		n = d / 60;
		unit = L"minute";
	} else if (d < 86400) {
		n = d / 3600;
		unit = L"hour";
	} else if (d < 86400ULL * 30) {
		n = d / 86400;
		unit = L"day";
	} else if (d < 86400ULL * 365) {
		n = d / (86400ULL * 30);
		unit = L"month";
	} else {
		n = d / (86400ULL * 365);
		unit = L"year";
	}
	_snwprintf_s(out, outcch, _TRUNCATE, L"%llu %s%s ago",
	    (unsigned long long)n, unit, n == 1 ? L"" : L"s");
}

// -------------------- JSON --------------------

static int
SkipValue(const jsmntok_t *tok, int idx, int r)
{
	int end = tok[idx].end;

	idx++;
	while (idx < r && tok[idx].start < end)
		idx++;
	return (idx);
}

static BOOL
PrimTrue(const char *json, const jsmntok_t *v)
{
	return (v->type == JSMN_PRIMITIVE && json[v->start] == 't');
}

static uint64_t
TokU64(const char *json, const jsmntok_t *v)
{
	char b[32] = { 0 };

	jsmn_copy_string(json, v, b, sizeof (b));
	return (_strtoui64(b, NULL, 10));
}

static void
TokWide(const char *json, const jsmntok_t *v, wchar_t *out, int cch)
{
	char b[MAX_PATH * 4] = { 0 };

	jsmn_copy_string(json, v, b, sizeof (b));
	MultiByteToWideChar(CP_UTF8, 0, b, -1, out, cch);
}

// The "path" values hold JSON-escaped backslashes; undo "\\" -> "\".
static void
Unescape(wchar_t *s)
{
	wchar_t *w = s;

	for (; *s; s++) {
		*w++ = *s;
		if (s[0] == L'\\' && s[1] == L'\\')
			s++;
	}
	*w = L'\0';
}

// Returns TRUE on a well-formed reply; *err gets the service's message.
static BOOL
ParseVersions(VCtx *c, const char *json, int json_len, wchar_t *err,
    int errcch)
{
	jsmn_parser p;
	jsmntok_t *tok;
	int r, i, ntok = json_len / 2 + 16;
	BOOL ok = FALSE, got_ok = FALSE;

	tok = (jsmntok_t *)HeapAlloc(GetProcessHeap(), 0,
	    ntok * sizeof (jsmntok_t));
	if (!tok)
		return (FALSE);
	jsmn_init(&p);
	r = jsmn_parse(&p, json, json_len, tok, ntok);
	if (r < 1 || tok[0].type != JSMN_OBJECT) {
		HeapFree(GetProcessHeap(), 0, tok);
		return (FALSE);
	}

	i = 1;
	while (i + 1 < r && tok[i].start < tok[0].end) {
		const jsmntok_t *k = &tok[i], *v = &tok[i + 1];

		if (jsmn_eq(json, k, "ok")) {
			got_ok = TRUE;
			ok = PrimTrue(json, v);
		} else if (jsmn_eq(json, k, "err")) {
			TokWide(json, v, err, errcch);
		} else if (jsmn_eq(json, k, "is_dir")) {
			c->is_dir = PrimTrue(json, v);
		} else if (jsmn_eq(json, k, "size")) {
			c->live_size = TokU64(json, v);
		} else if (jsmn_eq(json, k, "mtime")) {
			c->live_mtime = TokU64(json, v);
		} else if (jsmn_eq(json, k, "versions") &&
		    v->type == JSMN_ARRAY) {
			int n = v->size, e, j = i + 2;

			c->vers = (Version *)HeapAlloc(GetProcessHeap(),
			    HEAP_ZERO_MEMORY,
			    (n > 0 ? n : 1) * sizeof (Version));
			if (!c->vers)
				break;
			for (e = 0; e < n && j < r; e++) {
				int oend = tok[j].end, q = j + 1;
				Version *ver = &c->vers[c->nvers];

				while (q + 1 < r && tok[q].start < oend) {
					const jsmntok_t *kk = &tok[q];
					const jsmntok_t *vv = &tok[q + 1];

					if (jsmn_eq(json, kk, "snap")) {
						TokWide(json, vv, ver->snap,
						    ARRAYSIZE(ver->snap));
					} else if (jsmn_eq(json, kk, "path")) {
						TokWide(json, vv, ver->path,
						    ARRAYSIZE(ver->path));
						Unescape(ver->path);
					} else if (jsmn_eq(json, kk,
					    "creation")) {
						ver->creation =
						    TokU64(json, vv);
					} else if (jsmn_eq(json, kk, "size")) {
						ver->size = TokU64(json, vv);
					} else if (jsmn_eq(json, kk, "mtime")) {
						ver->mtime = TokU64(json, vv);
					} else if (jsmn_eq(json, kk,
					    "same_as_live")) {
						ver->same_as_live =
						    PrimTrue(json, vv);
					} else if (jsmn_eq(json, kk,
					    "changed")) {
						ver->changed =
						    PrimTrue(json, vv);
					}
					q = SkipValue(tok, q + 1, r);
				}
				c->nvers++;
				j = q;
			}
			i = j;
			continue;
		}
		i = SkipValue(tok, i + 1, r);
	}

	HeapFree(GetProcessHeap(), 0, tok);
	return (got_ok && ok);
}

// -------------------- list handling --------------------

static void
AddColumns(HWND hList)
{
	static const struct { const wchar_t *name; int cx; } cols[] = {
		{ L"Snapshot", 170 }, { L"Modified", 140 }, { L"Age", 100 },
		{ L"Size", 80 }, { L"Note", 110 },
	};
	LVCOLUMNW col = { 0 };
	int i;

	col.mask = LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
	for (i = 0; i < (int)ARRAYSIZE(cols); i++) {
		col.pszText = (LPWSTR)cols[i].name;
		col.cx = cols[i].cx;
		col.iSubItem = i;
		ListView_InsertColumn(hList, i, &col);
	}
}

static void
Populate(VCtx *c)
{
	BOOL only_changed =
	    (SendDlgItemMessageW(c->hWnd, IDC_VCHANGED, BM_GETCHECK, 0, 0) ==
	    BST_CHECKED);
	wchar_t buf[96];
	int i, row = 0;

	ListView_DeleteAllItems(c->hList);

	// Newest first, with the current file as the first row.
	{
		LVITEMW it = { 0 };
		it.mask = LVIF_TEXT | LVIF_PARAM;
		it.iItem = row;
		it.pszText = L"(current)";
		it.lParam = -1;
		ListView_InsertItem(c->hList, &it);
		FormatTimeW(c->live_mtime, buf, ARRAYSIZE(buf));
		ListView_SetItemText(c->hList, row, 1, buf);
		FormatAgeW(c->live_mtime, buf, ARRAYSIZE(buf));
		ListView_SetItemText(c->hList, row, 2, buf);
		if (c->is_dir)
			buf[0] = L'\0';
		else
			FormatBytesW(c->live_size, buf, ARRAYSIZE(buf));
		ListView_SetItemText(c->hList, row, 3, buf);
		row++;
	}

	for (i = c->nvers - 1; i >= 0; i--) {
		const Version *v = &c->vers[i];
		LVITEMW it = { 0 };

		if (only_changed && !v->changed)
			continue;
		it.mask = LVIF_TEXT | LVIF_PARAM;
		it.iItem = row;
		it.pszText = (LPWSTR)v->snap;
		it.lParam = i;
		ListView_InsertItem(c->hList, &it);
		FormatTimeW(v->mtime, buf, ARRAYSIZE(buf));
		ListView_SetItemText(c->hList, row, 1, buf);
		FormatAgeW(v->mtime, buf, ARRAYSIZE(buf));
		ListView_SetItemText(c->hList, row, 2, buf);
		if (c->is_dir)
			buf[0] = L'\0';
		else
			FormatBytesW(v->size, buf, ARRAYSIZE(buf));
		ListView_SetItemText(c->hList, row, 3, buf);
		ListView_SetItemText(c->hList, row, 4,
		    v->same_as_live ? L"same as current" : L"");
		row++;
	}
}

// Version selected in the list, or NULL (none / the "(current)" row).
static const Version *
Selected(VCtx *c)
{
	int sel = ListView_GetNextItem(c->hList, -1, LVNI_SELECTED);
	LVITEMW it = { 0 };

	if (sel < 0)
		return (NULL);
	it.mask = LVIF_PARAM;
	it.iItem = sel;
	if (!ListView_GetItem(c->hList, &it) || it.lParam < 0)
		return (NULL);
	return (&c->vers[it.lParam]);
}

static void
UpdateButtons(VCtx *c)
{
	const Version *v = Selected(c);

	EnableWindow(GetDlgItem(c->hWnd, IDC_VOPEN), v != NULL);
	EnableWindow(GetDlgItem(c->hWnd, IDC_VCOPY), v != NULL);
	// Restoring over a live directory would merge, not restore.
	EnableWindow(GetDlgItem(c->hWnd, IDC_VRESTORE),
	    v != NULL && !c->is_dir);
}

// "name (snap).ext" for copies, so they never clobber the original.
static void
CopyName(const VCtx *c, const Version *v, wchar_t *out, size_t cch)
{
	const wchar_t *base = wcsrchr(c->path, L'\\');
	wchar_t name[MAX_PATH];
	wchar_t *dot;

	base = base ? base + 1 : c->path;
	lstrcpynW(name, base, ARRAYSIZE(name));
	dot = wcsrchr(name, L'.');
	if (!c->is_dir && dot != NULL && dot != name) {
		wchar_t ext[MAX_PATH];
		lstrcpynW(ext, dot, ARRAYSIZE(ext));
		*dot = L'\0';
		_snwprintf_s(out, cch, _TRUNCATE, L"%s (%s)%s", name,
		    v->snap, ext);
	} else {
		_snwprintf_s(out, cch, _TRUNCATE, L"%s (%s)", name, v->snap);
	}
}

static void
DoOpen(VCtx *c, const Version *v)
{
	// A real open: this is what makes the snapshot auto-mount.
	HINSTANCE h = ShellExecuteW(c->hWnd, L"open", v->path, NULL, NULL,
	    SW_SHOWNORMAL);
	if ((INT_PTR)h <= 32)
		MessageBoxW(c->hWnd, L"Could not open this version.",
		    L"ZFS versions", MB_ICONERROR);
}

static void
DoCopy(VCtx *c, const Version *v)
{
	wchar_t suggested[MAX_PATH * 2];
	wchar_t dest[MAX_PATH * 2] = L"";
	BOOL ok;

	CopyName(c, v, suggested, ARRAYSIZE(suggested));

	if (c->is_dir) {
		BROWSEINFOW bi = { 0 };
		PIDLIST_ABSOLUTE pidl;
		wchar_t folder[MAX_PATH];
		SHFILEOPSTRUCTW fo = { 0 };
		wchar_t from[MAX_PATH * 2 + 2] = { 0 };
		wchar_t to[MAX_PATH * 2 + 2] = { 0 };

		bi.hwndOwner = c->hWnd;
		bi.lpszTitle = L"Copy this version of the folder into:";
		bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
		pidl = SHBrowseForFolderW(&bi);
		if (pidl == NULL)
			return;
		ok = SHGetPathFromIDListW(pidl, folder);
		CoTaskMemFree(pidl);
		if (!ok)
			return;
		_snwprintf_s(to, ARRAYSIZE(to) - 1, _TRUNCATE, L"%s\\%s",
		    folder, suggested);
		lstrcpynW(from, v->path, ARRAYSIZE(from) - 1);
		// Copy to a new name so nothing is merged into the original.
		fo.hwnd = c->hWnd;
		fo.wFunc = FO_COPY;
		fo.pFrom = from;
		fo.pTo = to;
		fo.fFlags = FOF_NOCONFIRMMKDIR;
		if (SHFileOperationW(&fo) != 0 || fo.fAnyOperationsAborted)
			return;
		MessageBoxW(c->hWnd, L"Copied.", L"ZFS versions",
		    MB_ICONINFORMATION);
		return;
	}

	{
		OPENFILENAMEW ofn = { 0 };

		lstrcpynW(dest, suggested, ARRAYSIZE(dest));
		ofn.lStructSize = sizeof (ofn);
		ofn.hwndOwner = c->hWnd;
		ofn.lpstrFile = dest;
		ofn.nMaxFile = ARRAYSIZE(dest);
		ofn.lpstrTitle = L"Copy this version to";
		ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
		if (!GetSaveFileNameW(&ofn))
			return;
		if (!CopyFileW(v->path, dest, FALSE)) {
			wchar_t m[128];
			_snwprintf_s(m, ARRAYSIZE(m), _TRUNCATE,
			    L"Copy failed (error %lu).",
			    (unsigned long)GetLastError());
			MessageBoxW(c->hWnd, m, L"ZFS versions", MB_ICONERROR);
			return;
		}
		MessageBoxW(c->hWnd, L"Copied.", L"ZFS versions",
		    MB_ICONINFORMATION);
	}
}

static void
DoRestore(VCtx *c, const Version *v)
{
	wchar_t msg[MAX_PATH * 3];

	_snwprintf_s(msg, ARRAYSIZE(msg), _TRUNCATE,
	    L"Replace the current file with the version from \"%s\"?\n\n%s\n\n"
	    L"The current contents are overwritten. To keep both, use "
	    L"\"Copy to...\" instead.", v->snap, c->path);
	if (MessageBoxW(c->hWnd, msg, L"Restore version",
	    MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
		return;
	if (!CopyFileW(v->path, c->path, FALSE)) {
		wchar_t m[128];
		_snwprintf_s(m, ARRAYSIZE(m), _TRUNCATE,
		    L"Restore failed (error %lu).",
		    (unsigned long)GetLastError());
		MessageBoxW(c->hWnd, m, L"ZFS versions", MB_ICONERROR);
		return;
	}
	MessageBoxW(c->hWnd, L"Restored.", L"ZFS versions",
	    MB_ICONINFORMATION);
}

// -------------------- window --------------------

static void
Layout(VCtx *c)
{
	RECT rc;
	int w, h, bh = 28, pad = 10, top = 64;

	GetClientRect(c->hWnd, &rc);
	w = rc.right;
	h = rc.bottom;
	MoveWindow(GetDlgItem(c->hWnd, IDC_VINFO), pad, 36, w - 2 * pad, 20,
	    TRUE);
	MoveWindow(c->hList, pad, top, w - 2 * pad, h - top - bh - 2 * pad,
	    TRUE);
	MoveWindow(GetDlgItem(c->hWnd, IDC_VCHANGED), pad, h - bh - pad / 2 -
	    2, 190, bh, TRUE);
	MoveWindow(GetDlgItem(c->hWnd, IDC_VDELETED), 2 * pad + 190,
	    h - bh - pad, 130, bh, TRUE);
	MoveWindow(GetDlgItem(c->hWnd, IDC_VRESTORE), w - pad - 100,
	    h - bh - pad, 100, bh, TRUE);
	MoveWindow(GetDlgItem(c->hWnd, IDC_VCOPY), w - 2 * (pad + 100),
	    h - bh - pad, 100, bh, TRUE);
	MoveWindow(GetDlgItem(c->hWnd, IDC_VOPEN), w - 3 * (pad + 100),
	    h - bh - pad, 100, bh, TRUE);
}

// Open the deleted-items window as a separate zfs_tray process.
static void
LaunchDeleted(const wchar_t *dir)
{
	wchar_t exe[MAX_PATH];
	wchar_t cmd[MAX_PATH * 3];
	STARTUPINFOW si = { sizeof (si) };
	PROCESS_INFORMATION pi;
	size_t n;

	if (GetModuleFileNameW(NULL, exe, ARRAYSIZE(exe)) == 0)
		return;
	// A trailing \ would escape the closing quote.
	_snwprintf_s(cmd, ARRAYSIZE(cmd), _TRUNCATE,
	    L"\"%s\" --deleted \"%s\"", exe, dir);
	n = wcslen(cmd);
	if (n > 2 && cmd[n - 2] == L'\\' && cmd[n - 1] == L'"' &&
	    n + 1 < ARRAYSIZE(cmd)) {
		cmd[n - 1] = L'.';
		cmd[n] = L'"';
		cmd[n + 1] = L'\0';
	}
	if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si,
	    &pi)) {
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	}
}

static LRESULT CALLBACK
VersionsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	VCtx *c = (VCtx *)GetWindowLongPtrW(hWnd, GWLP_USERDATA);

	switch (msg) {
	case WM_NCCREATE: {
		CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
		SetWindowLongPtrW(hWnd, GWLP_USERDATA,
		    (LONG_PTR)cs->lpCreateParams);
		break;
	}
	case WM_SIZE:
		if (c != NULL)
			Layout(c);
		return (0);
	case WM_GETMINMAXINFO: {
		MINMAXINFO *mm = (MINMAXINFO *)lParam;
		mm->ptMinTrackSize.x = 560;
		mm->ptMinTrackSize.y = 300;
		return (0);
	}
	case WM_NOTIFY: {
		NMHDR *nh = (NMHDR *)lParam;
		if (c != NULL && nh->idFrom == IDC_VLIST) {
			if (nh->code == LVN_ITEMCHANGED) {
				UpdateButtons(c);
			} else if (nh->code == NM_DBLCLK) {
				const Version *v = Selected(c);
				if (v != NULL)
					DoOpen(c, v);
			}
		}
		return (0);
	}
	case WM_COMMAND: {
		const Version *v;

		if (c == NULL)
			break;
		switch (LOWORD(wParam)) {
		case IDC_VCHANGED:
			Populate(c);
			UpdateButtons(c);
			break;
		case IDC_VOPEN:
			if ((v = Selected(c)) != NULL)
				DoOpen(c, v);
			break;
		case IDC_VCOPY:
			if ((v = Selected(c)) != NULL)
				DoCopy(c, v);
			break;
		case IDC_VRESTORE:
			if ((v = Selected(c)) != NULL)
				DoRestore(c, v);
			break;
		case IDC_VDELETED:
			LaunchDeleted(c->path);
			break;
		case IDCANCEL:
			DestroyWindow(hWnd);
			break;
		}
		return (0);
	}
	case WM_CLOSE:
		DestroyWindow(hWnd);
		return (0);
	case WM_DESTROY:
		PostQuitMessage(0);
		return (0);
	}
	return (DefWindowProcW(hWnd, msg, wParam, lParam));
}

static HWND
MakeChild(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style,
    int id)
{
	return (CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
	    0, 0, 10, 10, parent, (HMENU)(INT_PTR)id,
	    (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE), NULL));
}

static void
ErrorBox(const wchar_t *msg)
{
	MessageBoxW(NULL, msg, L"ZFS versions", MB_ICONERROR);
}

int
RunVersionsWindow(HINSTANCE hInst, const wchar_t *path)
{
	VCtx ctx;
	zrpc_t rpc;
	char utf8[MAX_PATH * 6];
	uint8_t *out = NULL;
	uint32_t st = 0, outlen = 0;
	wchar_t err[256] = L"";
	WNDCLASSW wc = { 0 };
	HFONT font;
	MSG m;
	INITCOMMONCONTROLSEX icc = { sizeof (icc), ICC_LISTVIEW_CLASSES };
	BOOL got;
	wchar_t info[MAX_PATH * 2 + 32];

	ZeroMemory(&ctx, sizeof (ctx));
	lstrcpynW(ctx.path, path, ARRAYSIZE(ctx.path));
	InitCommonControlsEx(&icc);
	CoInitialize(NULL);

	if (WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof (utf8),
	    NULL, NULL) <= 0) {
		ErrorBox(L"Invalid path.");
		return (1);
	}

	zrpc_init(&rpc, L"\\\\.\\pipe\\openzfs_zed", 30000);
	SetCursor(LoadCursorW(NULL, IDC_WAIT));
	got = zrpc_call(&rpc, OP_FILE_VERSIONS, utf8,
	    (uint32_t)strlen(utf8) + 1, &st, &out, &outlen);
	if (!got) {
		ErrorBox(L"Could not reach the OpenZFS service.");
		return (1);
	}
	if (st != 0 || out == NULL) {
		_snwprintf_s(err, ARRAYSIZE(err), _TRUNCATE,
		    st == ERROR_ACCESS_DENIED ?
		    L"Administrator rights are needed to list versions." :
		    L"The OpenZFS service could not list versions (%lu).",
		    (unsigned long)st);
		ErrorBox(err);
		return (1);
	}
	if (!ParseVersions(&ctx, (const char *)out, (int)outlen, err,
	    ARRAYSIZE(err))) {
		if (err[0] == L'\0')
			lstrcpynW(err, L"Unexpected reply from the service.",
			    ARRAYSIZE(err));
		HeapFree(GetProcessHeap(), 0, out);
		ErrorBox(err);
		return (1);
	}
	HeapFree(GetProcessHeap(), 0, out);

	wc.lpfnWndProc = VersionsWndProc;
	wc.hInstance = hInst;
	wc.lpszClassName = L"ZfsVersionsWnd";
	wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
	RegisterClassW(&wc);

	ctx.hWnd = CreateWindowExW(0, wc.lpszClassName,
	    L"ZFS versions", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
	    CW_USEDEFAULT, 720, 420, NULL, NULL, hInst, &ctx);
	if (ctx.hWnd == NULL)
		return (1);

	font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
	ctx.hHeaderFont = CreateHeaderFont(ctx.hWnd);

	{
		HWND h = MakeChild(ctx.hWnd, L"STATIC", L"Previous versions",
		    SS_LEFT, 0);
		SendMessageW(h, WM_SETFONT, (WPARAM)ctx.hHeaderFont, TRUE);
		MoveWindow(h, 10, 8, 400, 26, TRUE);
	}
	_snwprintf_s(info, ARRAYSIZE(info), _TRUNCATE, L"%s", ctx.path);
	SendMessageW(MakeChild(ctx.hWnd, L"STATIC", info,
	    SS_LEFT | SS_PATHELLIPSIS, IDC_VINFO), WM_SETFONT, (WPARAM)font,
	    TRUE);

	ctx.hList = MakeChild(ctx.hWnd, WC_LISTVIEWW, L"",
	    LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER |
	    WS_TABSTOP, IDC_VLIST);
	ListView_SetExtendedListViewStyle(ctx.hList,
	    LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
	SendMessageW(ctx.hList, WM_SETFONT, (WPARAM)font, TRUE);
	AddColumns(ctx.hList);

	SendMessageW(MakeChild(ctx.hWnd, L"BUTTON", L"Only show changes",
	    BS_AUTOCHECKBOX | WS_TABSTOP, IDC_VCHANGED), WM_SETFONT,
	    (WPARAM)font, TRUE);
	SendDlgItemMessageW(ctx.hWnd, IDC_VCHANGED, BM_SETCHECK, BST_CHECKED,
	    0);
	if (ctx.is_dir) {
		SendMessageW(MakeChild(ctx.hWnd, L"BUTTON",
		    L"Deleted items...", BS_PUSHBUTTON | WS_TABSTOP,
		    IDC_VDELETED), WM_SETFONT, (WPARAM)font, TRUE);
	}
	SendMessageW(MakeChild(ctx.hWnd, L"BUTTON", L"Open",
	    BS_PUSHBUTTON | WS_TABSTOP, IDC_VOPEN), WM_SETFONT, (WPARAM)font,
	    TRUE);
	SendMessageW(MakeChild(ctx.hWnd, L"BUTTON", L"Copy to...",
	    BS_PUSHBUTTON | WS_TABSTOP, IDC_VCOPY), WM_SETFONT, (WPARAM)font,
	    TRUE);
	SendMessageW(MakeChild(ctx.hWnd, L"BUTTON", L"Restore",
	    BS_PUSHBUTTON | WS_TABSTOP, IDC_VRESTORE), WM_SETFONT,
	    (WPARAM)font, TRUE);

	ApplyThemeFollowSystem(ctx.hWnd);
	Layout(&ctx);
	Populate(&ctx);
	UpdateButtons(&ctx);
	ShowWindow(ctx.hWnd, SW_SHOWNORMAL);
	SetForegroundWindow(ctx.hWnd);

	while (GetMessageW(&m, NULL, 0, 0) > 0) {
		if (!IsDialogMessageW(ctx.hWnd, &m)) {
			TranslateMessage(&m);
			DispatchMessageW(&m);
		}
	}

	if (ctx.hHeaderFont)
		DeleteObject(ctx.hHeaderFont);
	if (ctx.vers)
		HeapFree(GetProcessHeap(), 0, ctx.vers);
	return (0);
}

/* -------------------- deleted items window -------------------- */

#define	IDC_DLIST	3101
#define	IDC_DOPEN	3102
#define	IDC_DCOPY	3103
#define	IDC_DRESTORE	3104
#define	IDC_DINFO	3105

typedef struct {
	wchar_t name[MAX_PATH];
	wchar_t snap[128];
	wchar_t path[MAX_PATH * 2];	// the version, inside a snapshot
	BOOL is_dir;
	uint64_t size;
	uint64_t mtime;
} DelRow;

typedef struct {
	HWND hWnd;
	HWND hList;
	HFONT hHeaderFont;
	wchar_t dir[MAX_PATH * 2];	// the directory that was asked about
	DelRow *rows;
	int nrows;
	int caprows;
	BOOL truncated;
	uint64_t snapshots;
} DCtx;

static BOOL
AddRow(DCtx *c, const DelRow *r)
{
	if (c->nrows == c->caprows) {
		int ncap = c->caprows ? c->caprows * 2 : 64;
		DelRow *t;

		if (c->rows == NULL)
			t = (DelRow *)HeapAlloc(GetProcessHeap(),
			    HEAP_ZERO_MEMORY, ncap * sizeof (DelRow));
		else
			t = (DelRow *)HeapReAlloc(GetProcessHeap(),
			    HEAP_ZERO_MEMORY, c->rows,
			    ncap * sizeof (DelRow));
		if (t == NULL)
			return (FALSE);
		c->rows = t;
		c->caprows = ncap;
	}
	c->rows[c->nrows++] = *r;
	return (TRUE);
}

// Parse { ok, truncated, snapshots, items:[ {name,is_dir,versions:[..]} ] }
static BOOL
ParseDeleted(DCtx *c, const char *json, int json_len, wchar_t *err,
    int errcch)
{
	jsmn_parser p;
	jsmntok_t *tok;
	int r, i, ntok = json_len / 2 + 16;
	BOOL ok = FALSE, got_ok = FALSE;

	tok = (jsmntok_t *)HeapAlloc(GetProcessHeap(), 0,
	    ntok * sizeof (jsmntok_t));
	if (!tok)
		return (FALSE);
	jsmn_init(&p);
	r = jsmn_parse(&p, json, json_len, tok, ntok);
	if (r < 1 || tok[0].type != JSMN_OBJECT) {
		HeapFree(GetProcessHeap(), 0, tok);
		return (FALSE);
	}

	i = 1;
	while (i + 1 < r && tok[i].start < tok[0].end) {
		const jsmntok_t *k = &tok[i], *v = &tok[i + 1];

		if (jsmn_eq(json, k, "ok")) {
			got_ok = TRUE;
			ok = PrimTrue(json, v);
		} else if (jsmn_eq(json, k, "err")) {
			TokWide(json, v, err, errcch);
		} else if (jsmn_eq(json, k, "truncated")) {
			c->truncated = PrimTrue(json, v);
		} else if (jsmn_eq(json, k, "snapshots")) {
			c->snapshots = TokU64(json, v);
		} else if (jsmn_eq(json, k, "items") && v->type == JSMN_ARRAY) {
			int n = v->size, e, j = i + 2;

			for (e = 0; e < n && j < r; e++) {
				int oend = tok[j].end, q = j + 1;
				DelRow base;
				int vstart = -1;

				ZeroMemory(&base, sizeof (base));
				while (q + 1 < r && tok[q].start < oend) {
					const jsmntok_t *kk = &tok[q];
					const jsmntok_t *vv = &tok[q + 1];

					if (jsmn_eq(json, kk, "name")) {
						TokWide(json, vv, base.name,
						    ARRAYSIZE(base.name));
					} else if (jsmn_eq(json, kk,
					    "is_dir")) {
						base.is_dir =
						    PrimTrue(json, vv);
					} else if (jsmn_eq(json, kk,
					    "versions") &&
					    vv->type == JSMN_ARRAY) {
						vstart = q + 1;
					}
					q = SkipValue(tok, q + 1, r);
				}

				// Now the versions of this item.
				if (vstart >= 0) {
					int nv = tok[vstart].size, x;
					int y = vstart + 1;

					for (x = 0; x < nv && y < r; x++) {
						int vend = tok[y].end;
						int z = y + 1;
						DelRow row = base;

						while (z + 1 < r &&
						    tok[z].start < vend) {
							const jsmntok_t *a =
							    &tok[z];
							const jsmntok_t *b =
							    &tok[z + 1];

							if (jsmn_eq(json, a,
							    "snap")) {
								TokWide(json, b,
								    row.snap,
								    ARRAYSIZE(
								    row.snap));
							} else if (jsmn_eq(
							    json, a, "path")) {
								TokWide(json, b,
								    row.path,
								    ARRAYSIZE(
								    row.path));
								Unescape(
								    row.path);
							} else if (jsmn_eq(
							    json, a, "size")) {
								row.size =
								    TokU64(json,
								    b);
							} else if (jsmn_eq(
							    json, a, "mtime")) {
								row.mtime =
								    TokU64(json,
								    b);
							}
							z = SkipValue(tok,
							    z + 1, r);
						}
						(void) AddRow(c, &row);
						y = z;
					}
				}
				j = q;
			}
			i = j;
			continue;
		}
		i = SkipValue(tok, i + 1, r);
	}

	HeapFree(GetProcessHeap(), 0, tok);
	return (got_ok && ok);
}

static const DelRow *
DelSelected(DCtx *c)
{
	int sel = ListView_GetNextItem(c->hList, -1, LVNI_SELECTED);
	LVITEMW it = { 0 };

	if (sel < 0)
		return (NULL);
	it.mask = LVIF_PARAM;
	it.iItem = sel;
	if (!ListView_GetItem(c->hList, &it))
		return (NULL);
	return (&c->rows[it.lParam]);
}

static void
DelButtons(DCtx *c)
{
	BOOL have = (DelSelected(c) != NULL);

	EnableWindow(GetDlgItem(c->hWnd, IDC_DOPEN), have);
	EnableWindow(GetDlgItem(c->hWnd, IDC_DCOPY), have);
	EnableWindow(GetDlgItem(c->hWnd, IDC_DRESTORE), have);
}

static void
DelPopulate(DCtx *c)
{
	wchar_t buf[96];
	int i;

	ListView_DeleteAllItems(c->hList);
	for (i = 0; i < c->nrows; i++) {
		const DelRow *r = &c->rows[i];
		LVITEMW it = { 0 };

		it.mask = LVIF_TEXT | LVIF_PARAM;
		it.iItem = i;
		it.pszText = (LPWSTR)r->name;
		it.lParam = i;
		ListView_InsertItem(c->hList, &it);
		ListView_SetItemText(c->hList, i, 1, (LPWSTR)r->snap);
		FormatTimeW(r->mtime, buf, ARRAYSIZE(buf));
		ListView_SetItemText(c->hList, i, 2, buf);
		FormatAgeW(r->mtime, buf, ARRAYSIZE(buf));
		ListView_SetItemText(c->hList, i, 3, buf);
		if (r->is_dir)
			lstrcpynW(buf, L"folder", ARRAYSIZE(buf));
		else
			FormatBytesW(r->size, buf, ARRAYSIZE(buf));
		ListView_SetItemText(c->hList, i, 4, buf);
	}
}

static void
DelOpen(DCtx *c, const DelRow *r)
{
	HINSTANCE h = ShellExecuteW(c->hWnd, L"open", r->path, NULL, NULL,
	    SW_SHOWNORMAL);

	if ((INT_PTR)h <= 32)
		MessageBoxW(c->hWnd, L"Could not open this item.",
		    L"ZFS deleted items", MB_ICONERROR);
}

// Copy src to dest (a file, or a folder tree); TRUE on success.
static BOOL
DelCopy(HWND hWnd, const DelRow *r, const wchar_t *dest, BOOL overwrite)
{
	if (!r->is_dir) {
		if (CopyFileW(r->path, dest, !overwrite))
			return (TRUE);
		{
			wchar_t m[MAX_PATH * 2];
			DWORD gle = GetLastError();

			if (gle == ERROR_FILE_EXISTS)
				_snwprintf_s(m, ARRAYSIZE(m), _TRUNCATE,
				    L"\"%s\" already exists here. Use "
				    L"\"Copy to...\" to choose another name.",
				    r->name);
			else
				_snwprintf_s(m, ARRAYSIZE(m), _TRUNCATE,
				    L"Copy failed (error %lu).",
				    (unsigned long)gle);
			MessageBoxW(hWnd, m, L"ZFS deleted items",
			    MB_ICONWARNING);
		}
		return (FALSE);
	}

	{
		SHFILEOPSTRUCTW fo = { 0 };
		wchar_t from[MAX_PATH * 2 + 2] = { 0 };
		wchar_t to[MAX_PATH * 2 + 2] = { 0 };

		lstrcpynW(from, r->path, ARRAYSIZE(from) - 1);
		lstrcpynW(to, dest, ARRAYSIZE(to) - 1);
		fo.hwnd = hWnd;
		fo.wFunc = FO_COPY;
		fo.pFrom = from;
		fo.pTo = to;
		fo.fFlags = FOF_NOCONFIRMMKDIR;
		return (SHFileOperationW(&fo) == 0 &&
		    !fo.fAnyOperationsAborted);
	}
}

static void
DelRestore(DCtx *c, const DelRow *r)
{
	wchar_t dest[MAX_PATH * 3];
	DWORD attr;

	_snwprintf_s(dest, ARRAYSIZE(dest), _TRUNCATE, L"%s\\%s", c->dir,
	    r->name);
	attr = GetFileAttributesW(dest);
	if (attr != INVALID_FILE_ATTRIBUTES) {
		wchar_t m[MAX_PATH * 2];
		_snwprintf_s(m, ARRAYSIZE(m), _TRUNCATE,
		    L"\"%s\" exists again in this folder. Use \"Copy to...\" "
		    L"to restore it under another name.", r->name);
		MessageBoxW(c->hWnd, m, L"ZFS deleted items", MB_ICONWARNING);
		return;
	}
	if (DelCopy(c->hWnd, r, dest, FALSE))
		MessageBoxW(c->hWnd, L"Restored.", L"ZFS deleted items",
		    MB_ICONINFORMATION);
}

static void
DelCopyTo(DCtx *c, const DelRow *r)
{
	wchar_t dest[MAX_PATH * 2] = L"";
	OPENFILENAMEW ofn = { 0 };

	if (r->is_dir) {
		BROWSEINFOW bi = { 0 };
		PIDLIST_ABSOLUTE pidl;
		wchar_t folder[MAX_PATH];
		wchar_t to[MAX_PATH * 3];

		bi.hwndOwner = c->hWnd;
		bi.lpszTitle = L"Copy this folder into:";
		bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
		pidl = SHBrowseForFolderW(&bi);
		if (pidl == NULL)
			return;
		if (!SHGetPathFromIDListW(pidl, folder)) {
			CoTaskMemFree(pidl);
			return;
		}
		CoTaskMemFree(pidl);
		_snwprintf_s(to, ARRAYSIZE(to), _TRUNCATE, L"%s\\%s", folder,
		    r->name);
		if (DelCopy(c->hWnd, r, to, TRUE))
			MessageBoxW(c->hWnd, L"Copied.", L"ZFS deleted items",
			    MB_ICONINFORMATION);
		return;
	}

	lstrcpynW(dest, r->name, ARRAYSIZE(dest));
	ofn.lStructSize = sizeof (ofn);
	ofn.hwndOwner = c->hWnd;
	ofn.lpstrFile = dest;
	ofn.nMaxFile = ARRAYSIZE(dest);
	ofn.lpstrTitle = L"Copy this version to";
	ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
	if (!GetSaveFileNameW(&ofn))
		return;
	if (DelCopy(c->hWnd, r, dest, TRUE))
		MessageBoxW(c->hWnd, L"Copied.", L"ZFS deleted items",
		    MB_ICONINFORMATION);
}

static void
DelLayout(DCtx *c)
{
	RECT rc;
	int w, h, bh = 28, pad = 10, top = 64;

	GetClientRect(c->hWnd, &rc);
	w = rc.right;
	h = rc.bottom;
	MoveWindow(GetDlgItem(c->hWnd, IDC_DINFO), pad, 36, w - 2 * pad, 20,
	    TRUE);
	MoveWindow(c->hList, pad, top, w - 2 * pad, h - top - bh - 2 * pad,
	    TRUE);
	MoveWindow(GetDlgItem(c->hWnd, IDC_DRESTORE), w - pad - 100,
	    h - bh - pad, 100, bh, TRUE);
	MoveWindow(GetDlgItem(c->hWnd, IDC_DCOPY), w - 2 * (pad + 100),
	    h - bh - pad, 100, bh, TRUE);
	MoveWindow(GetDlgItem(c->hWnd, IDC_DOPEN), w - 3 * (pad + 100),
	    h - bh - pad, 100, bh, TRUE);
}

static LRESULT CALLBACK
DeletedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	DCtx *c = (DCtx *)GetWindowLongPtrW(hWnd, GWLP_USERDATA);

	switch (msg) {
	case WM_NCCREATE: {
		CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
		SetWindowLongPtrW(hWnd, GWLP_USERDATA,
		    (LONG_PTR)cs->lpCreateParams);
		break;
	}
	case WM_SIZE:
		if (c != NULL)
			DelLayout(c);
		return (0);
	case WM_GETMINMAXINFO: {
		MINMAXINFO *mm = (MINMAXINFO *)lParam;
		mm->ptMinTrackSize.x = 560;
		mm->ptMinTrackSize.y = 300;
		return (0);
	}
	case WM_NOTIFY: {
		NMHDR *nh = (NMHDR *)lParam;
		if (c != NULL && nh->idFrom == IDC_DLIST) {
			if (nh->code == LVN_ITEMCHANGED) {
				DelButtons(c);
			} else if (nh->code == NM_DBLCLK) {
				const DelRow *r = DelSelected(c);
				if (r != NULL)
					DelOpen(c, r);
			}
		}
		return (0);
	}
	case WM_COMMAND: {
		const DelRow *r;

		if (c == NULL)
			break;
		r = DelSelected(c);
		switch (LOWORD(wParam)) {
		case IDC_DOPEN:
			if (r != NULL)
				DelOpen(c, r);
			break;
		case IDC_DCOPY:
			if (r != NULL)
				DelCopyTo(c, r);
			break;
		case IDC_DRESTORE:
			if (r != NULL)
				DelRestore(c, r);
			break;
		case IDCANCEL:
			DestroyWindow(hWnd);
			break;
		}
		return (0);
	}
	case WM_CLOSE:
		DestroyWindow(hWnd);
		return (0);
	case WM_DESTROY:
		PostQuitMessage(0);
		return (0);
	}
	return (DefWindowProcW(hWnd, msg, wParam, lParam));
}

int
RunDeletedWindow(HINSTANCE hInst, const wchar_t *path)
{
	static const struct { const wchar_t *name; int cx; } cols[] = {
		{ L"Name", 200 }, { L"Last seen in", 120 },
		{ L"Modified", 130 }, { L"Age", 90 }, { L"Size", 80 },
	};
	DCtx ctx;
	zrpc_t rpc;
	char u8[MAX_PATH * 6];
	uint8_t *out = NULL;
	uint32_t st = 0, outlen = 0;
	wchar_t err[256] = L"";
	wchar_t info[320];
	WNDCLASSW wc = { 0 };
	HFONT font;
	MSG m;
	INITCOMMONCONTROLSEX icc = { sizeof (icc), ICC_LISTVIEW_CLASSES };
	size_t n;
	int i;

	ZeroMemory(&ctx, sizeof (ctx));
	lstrcpynW(ctx.dir, path, ARRAYSIZE(ctx.dir));
	// Strip a trailing '\' (but keep "E:\" meaning the volume root form).
	n = wcslen(ctx.dir);
	while (n > 3 && ctx.dir[n - 1] == L'\\')
		ctx.dir[--n] = L'\0';
	if (n == 3 && ctx.dir[1] == L':' && ctx.dir[2] == L'\\')
		ctx.dir[--n] = L'\0';

	InitCommonControlsEx(&icc);
	CoInitialize(NULL);

	if (WideCharToMultiByte(CP_UTF8, 0, path, -1, u8, sizeof (u8), NULL,
	    NULL) <= 0) {
		ErrorBox(L"Invalid path.");
		return (1);
	}

	zrpc_init(&rpc, L"\\\\.\\pipe\\openzfs_zed", 300000);
	SetCursor(LoadCursorW(NULL, IDC_WAIT));
	if (!zrpc_call(&rpc, OP_DELETED_ITEMS, u8, (uint32_t)strlen(u8) + 1,
	    &st, &out, &outlen)) {
		ErrorBox(L"Could not reach the OpenZFS service.");
		return (1);
	}
	if (st != 0 || out == NULL) {
		_snwprintf_s(err, ARRAYSIZE(err), _TRUNCATE,
		    st == ERROR_ACCESS_DENIED ?
		    L"Administrator rights are needed to list deleted items." :
		    L"The OpenZFS service could not list deleted items (%lu).",
		    (unsigned long)st);
		ErrorBox(err);
		return (1);
	}
	if (!ParseDeleted(&ctx, (const char *)out, (int)outlen, err,
	    ARRAYSIZE(err))) {
		if (err[0] == L'\0')
			lstrcpynW(err, L"Unexpected reply from the service.",
			    ARRAYSIZE(err));
		HeapFree(GetProcessHeap(), 0, out);
		ErrorBox(err);
		return (1);
	}
	HeapFree(GetProcessHeap(), 0, out);

	wc.lpfnWndProc = DeletedWndProc;
	wc.hInstance = hInst;
	wc.lpszClassName = L"ZfsDeletedWnd";
	wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
	RegisterClassW(&wc);

	ctx.hWnd = CreateWindowExW(0, wc.lpszClassName, L"ZFS deleted items",
	    WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 760, 420, NULL,
	    NULL, hInst, &ctx);
	if (ctx.hWnd == NULL)
		return (1);

	font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
	ctx.hHeaderFont = CreateHeaderFont(ctx.hWnd);

	{
		HWND h = CreateWindowExW(0, L"STATIC", L"Deleted items",
		    WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 8, 400, 26, ctx.hWnd,
		    NULL, hInst, NULL);
		SendMessageW(h, WM_SETFONT, (WPARAM)ctx.hHeaderFont, TRUE);
	}
	if (ctx.nrows == 0)
		_snwprintf_s(info, ARRAYSIZE(info), _TRUNCATE,
		    L"%s: nothing was deleted from this folder in the %llu "
		    L"snapshots searched.", ctx.dir,
		    (unsigned long long)ctx.snapshots);
	else
		_snwprintf_s(info, ARRAYSIZE(info), _TRUNCATE,
		    L"%s: in %llu snapshots%s", ctx.dir,
		    (unsigned long long)ctx.snapshots,
		    ctx.truncated ? L" (only the newest were searched)" : L"");
	SendMessageW(MakeChild(ctx.hWnd, L"STATIC", info,
	    SS_LEFT | SS_PATHELLIPSIS, IDC_DINFO), WM_SETFONT, (WPARAM)font,
	    TRUE);

	ctx.hList = MakeChild(ctx.hWnd, WC_LISTVIEWW, L"",
	    LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER |
	    WS_TABSTOP, IDC_DLIST);
	ListView_SetExtendedListViewStyle(ctx.hList,
	    LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
	SendMessageW(ctx.hList, WM_SETFONT, (WPARAM)font, TRUE);
	for (i = 0; i < (int)ARRAYSIZE(cols); i++) {
		LVCOLUMNW col = { 0 };
		col.mask = LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
		col.pszText = (LPWSTR)cols[i].name;
		col.cx = cols[i].cx;
		col.iSubItem = i;
		ListView_InsertColumn(ctx.hList, i, &col);
	}

	SendMessageW(MakeChild(ctx.hWnd, L"BUTTON", L"Open",
	    BS_PUSHBUTTON | WS_TABSTOP, IDC_DOPEN), WM_SETFONT, (WPARAM)font,
	    TRUE);
	SendMessageW(MakeChild(ctx.hWnd, L"BUTTON", L"Copy to...",
	    BS_PUSHBUTTON | WS_TABSTOP, IDC_DCOPY), WM_SETFONT, (WPARAM)font,
	    TRUE);
	SendMessageW(MakeChild(ctx.hWnd, L"BUTTON", L"Restore",
	    BS_PUSHBUTTON | WS_TABSTOP, IDC_DRESTORE), WM_SETFONT,
	    (WPARAM)font, TRUE);

	ApplyThemeFollowSystem(ctx.hWnd);
	DelLayout(&ctx);
	DelPopulate(&ctx);
	DelButtons(&ctx);
	ShowWindow(ctx.hWnd, SW_SHOWNORMAL);
	SetForegroundWindow(ctx.hWnd);

	while (GetMessageW(&m, NULL, 0, 0) > 0) {
		if (!IsDialogMessageW(ctx.hWnd, &m)) {
			TranslateMessage(&m);
			DispatchMessageW(&m);
		}
	}

	if (ctx.hHeaderFont)
		DeleteObject(ctx.hHeaderFont);
	if (ctx.rows)
		HeapFree(GetProcessHeap(), 0, ctx.rows);
	return (0);
}
