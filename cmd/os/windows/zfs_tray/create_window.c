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

// cmd/os/windows/zfs_tray/create_window.c
#define	_CRT_SECURE_NO_WARNINGS
#define	UNICODE
#define	_UNICODE
#define	_WIN32_WINNT 0x0600
#define	_WIN32_IE    0x0600
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>
#include <winioctl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define	JSMN_STATIC
#include "jsmn.h"
#include "jsmn_utils.h"

#include "pipe_rpc.h"
#include "rpc_client.h"
#include "create_window.h"
#include "resource.h"
#include "dlg_util.h"
#include "pass_prompt.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")

#ifndef ARRAYSIZE
#define	ARRAYSIZE(a) (sizeof (a)/sizeof ((a)[0]))
#endif

// ---- WM_APP messages
#define	WM_APP_DISKS_DONE (WM_APP + 200)
#define	WM_APP_CREATE_DONE (WM_APP + 201)

// ---- data types
typedef struct {
	// UTF-8, "PhysicalDriveN", "HarddiskNPartitionM" or the path of a file
	char path[MAX_PATH * 3];
	int part; // 0 == whole disk; else a partition nested under its disk
	BOOL is_file; // a file to use as a vdev
	wchar_t sizeW[32];
	wchar_t modelW[128];
	BOOL is_boot;
	BOOL has_pool;
} DiskEntry;

typedef struct {
	DiskEntry *items;
	int count;
} DiskList;

typedef struct {
	HWND hWnd;
	zrpc_t *rpc;
	HANDLE hScanThread;
	HFONT hHeaderFont;
	DiskList *disks;
} CreateCtx;

// Formats a byte count roughly like `zpool list`/Explorer ("2.00 TB").
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

// Parse: { "disks":[ {"path","size","model","is_boot","has_pool"}, ... ] }
static DiskList *
ParseDisksJSON(const char *json, int json_len)
{
	jsmn_parser p;
	jsmn_init(&p);
	jsmntok_t tok[4096];
	int r = jsmn_parse(&p, json, json_len, tok, (int)ARRAYSIZE(tok));
	if (r < 0)
		return (NULL);

	int arr = -1;
	for (int i = 1; i < r - 1; ++i) {
		if (tok[i].type == JSMN_STRING &&
		    tok[i + 1].type == JSMN_ARRAY &&
		    jsmn_eq(json, &tok[i], "disks")) {
			arr = i + 1;
			break;
		}
	}
	if (arr < 0)
		return (NULL);

	int elems = tok[arr].size;
	DiskList *dl = (DiskList *)HeapAlloc(GetProcessHeap(),
	    HEAP_ZERO_MEMORY, sizeof (*dl));
	if (!dl)
		return (NULL);
	dl->items = (DiskEntry *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
	    (elems > 0 ? elems : 1) * sizeof (DiskEntry));
	if (!dl->items) {
		HeapFree(GetProcessHeap(), 0, dl);
		return (NULL);
	}
	dl->count = 0;

	int idx = arr + 1;
	for (int e = 0; e < elems && idx < r; ++e) {
		if (tok[idx].type != JSMN_OBJECT) {
			int end = tok[idx].end;
			idx++;
			while (idx < r && tok[idx].start < end) idx++;
			continue;
		}
		int obj = idx++, end = tok[obj].end;
		DiskEntry *d = &dl->items[dl->count];
		ZeroMemory(d, sizeof (*d));
		uint64_t size = 0;

		while (idx + 1 < r && tok[idx].start < end) {
			jsmntok_t *k = &tok[idx], *v = &tok[idx + 1];
			if (k->type == JSMN_STRING) {
				if (jsmn_eq(json, k, "path")) {
					jsmn_copy_string(json, v, d->path,
					    sizeof (d->path));
				} else if (jsmn_eq(json, k, "size")) {
					char sbuf[32] = { 0 };
					jsmn_copy_string(json, v, sbuf,
					    sizeof (sbuf));
					size = _strtoui64(sbuf, NULL, 10);
				} else if (jsmn_eq(json, k, "part")) {
					char pbuf[16] = { 0 };
					jsmn_copy_string(json, v, pbuf,
					    sizeof (pbuf));
					d->part = atoi(pbuf);
				} else if (jsmn_eq(json, k, "model")) {
					char mbuf[128] = { 0 };
					jsmn_copy_string(json, v, mbuf,
					    sizeof (mbuf));
					MultiByteToWideChar(CP_UTF8, 0, mbuf,
					    -1, d->modelW,
					    (int)ARRAYSIZE(d->modelW));
				} else if (jsmn_eq(json, k, "is_boot")) {
					d->is_boot = (v->type ==
					    JSMN_PRIMITIVE &&
					    json[v->start] == 't');
				} else if (jsmn_eq(json, k, "has_pool")) {
					d->has_pool = (v->type ==
					    JSMN_PRIMITIVE &&
					    json[v->start] == 't');
				}
			}
			int vend = v->end;
			idx += 2;
			while (idx < r && tok[idx].start < vend) idx++;
		}

		FormatBytesW(size, d->sizeW, ARRAYSIZE(d->sizeW));
		if (d->path[0])
			dl->count++;
	}
	return (dl);
}

static void
FreeDiskList(DiskList *dl)
{
	if (!dl)
		return;
	if (dl->items)
		HeapFree(GetProcessHeap(), 0, dl->items);
	HeapFree(GetProcessHeap(), 0, dl);
}

static DWORD WINAPI
DiskScanThread(LPVOID param)
{
	CreateCtx *ctx = (CreateCtx *)param;

	uint8_t *out = NULL;
	uint32_t st = 0, outlen = 0;

	if (zrpc_call(ctx->rpc, OP_LIST_DISKS, NULL, 0, &st, &out, &outlen) &&
	    st == 0 && out) {
		DiskList *dl = ParseDisksJSON((const char *)out, (int)outlen);
		HeapFree(GetProcessHeap(), 0, out);
		PostMessageW(ctx->hWnd, WM_APP_DISKS_DONE, 0, (LPARAM)dl);
	} else {
		if (out) HeapFree(GetProcessHeap(), 0, out);
		PostMessageW(ctx->hWnd, WM_APP_DISKS_DONE, 0, (LPARAM)NULL);
	}
	return (0);
}

static void
AddDiskListColumns(HWND hList)
{
	LVCOLUMNW col = { 0 };
	col.mask = LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
	col.pszText = L"Disk / Partition";
	col.cx = 190;
	col.iSubItem = 0;
	ListView_InsertColumn(hList, 0, &col);
	col.pszText = L"Size";
	col.cx = 80;
	col.iSubItem = 1;
	ListView_InsertColumn(hList, 1, &col);
	col.pszText = L"Model";
	col.cx = 170;
	col.iSubItem = 2;
	ListView_InsertColumn(hList, 2, &col);
	col.pszText = L"Flags";
	col.cx = 80;
	col.iSubItem = 3;
	ListView_InsertColumn(hList, 3, &col);
}

static int
InsertDiskRow(HWND hList, const DiskEntry *d, int row, int data)
{
	wchar_t pathW[MAX_PATH * 2];
	// Partitions arrive right after their disk; indent them with a
	// tree glyph (LVITEM.iIndent needs an image list to show).
	int pre = 0;
	if (d->part > 0)
		pre = _snwprintf_s(pathW, ARRAYSIZE(pathW), _TRUNCATE,
		    L"   \x2514\x2500 ");
	MultiByteToWideChar(CP_UTF8, 0, d->path, -1, pathW + pre,
	    (int)ARRAYSIZE(pathW) - pre);

	LVITEMW it = { 0 };
	it.mask = LVIF_TEXT | LVIF_PARAM;
	it.iItem = row;
	it.pszText = pathW;
	it.lParam = (LPARAM)data; // index into DiskList
	int idx = ListView_InsertItem(hList, &it);

	ListView_SetItemText(hList, idx, 1, (LPWSTR)d->sizeW);
	ListView_SetItemText(hList, idx, 2, (LPWSTR)d->modelW);

	wchar_t flags[64] = L"";
	if (d->is_boot && d->has_pool)
		lstrcpynW(flags, L"BOOT, HAS POOL", ARRAYSIZE(flags));
	else if (d->is_boot)
		lstrcpynW(flags, L"BOOT DISK", ARRAYSIZE(flags));
	else if (d->has_pool)
		lstrcpynW(flags, L"HAS POOL", ARRAYSIZE(flags));
	else if (d->is_file)
		lstrcpynW(flags, L"FILE", ARRAYSIZE(flags));
	ListView_SetItemText(hList, idx, 3, flags);
	return (idx);
}

static void
PopulateDiskList(HWND hList, const DiskList *dl)
{
	ListView_DeleteAllItems(hList);
	if (!dl || dl->count == 0)
		return;

	for (int i = 0; i < dl->count; i++)
		InsertDiskRow(hList, &dl->items[i], i, i);
}

static void
InitTopologyCombo(HWND hDlg)
{
	HWND h = GetDlgItem(hDlg, IDC_CMB_TOPOLOGY);
	ComboBox_AddString(h, L"Stripe");
	ComboBox_AddString(h, L"Mirror");
	ComboBox_AddString(h, L"raidz1");
	ComboBox_AddString(h, L"raidz2");
	ComboBox_AddString(h, L"raidz3");
	ComboBox_AddString(h, L"draid1");
	ComboBox_AddString(h, L"draid2");
	ComboBox_AddString(h, L"draid3");
	ComboBox_AddString(h, L"AnyMirror");
	ComboBox_AddString(h, L"AnyMirror2");
	ComboBox_AddString(h, L"AnyMirror3");
	ComboBox_SetCurSel(h, 0);
}

// Populates the Compatibility combo with the special "off"/"legacy" values
// (see zpool_load_compat(), lib/libzfs/libzfs_pool.c) plus any feature-set
// files found in a "compatibility.d" directory next to this executable -
// the same location the Windows build of libzfs itself searches as a
// fallback (zpool_load_compat()'s "edirfd" lookup), so files placed there
// for the CLI show up here too without a separate install step.
static void
PopulateCompatCombo(HWND h)
{
	ComboBox_AddString(h, L"off (all features)");
	ComboBox_AddString(h, L"legacy (v28, no features)");
	ComboBox_SetCurSel(h, 0);

	wchar_t dir[MAX_PATH];
	DWORD len = GetModuleFileNameW(NULL, dir, ARRAYSIZE(dir));
	if (len == 0 || len >= ARRAYSIZE(dir))
		return;
	wchar_t *slash = wcsrchr(dir, L'\\');
	if (!slash)
		return;
	*slash = L'\0';
	if (wcscat_s(dir, ARRAYSIZE(dir), L"\\compatibility.d\\*") != 0)
		return;

	WIN32_FIND_DATAW fd;
	HANDLE fh = FindFirstFileW(dir, &fd);
	if (fh == INVALID_HANDLE_VALUE)
		return;
	do {
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		ComboBox_AddString(h, fd.cFileName);
	} while (FindNextFileW(fh, &fd));
	FindClose(fh);
}

static void
InitPropCombos(HWND hDlg)
{
	HWND h = GetDlgItem(hDlg, IDC_CMB_CASESENS);
	ComboBox_AddString(h, L"sensitive");
	ComboBox_AddString(h, L"insensitive");
	ComboBox_AddString(h, L"mixed");
	ComboBox_SetCurSel(h, 0);

	h = GetDlgItem(hDlg, IDC_CMB_ASHIFT);
	ComboBox_AddString(h, L"9");
	ComboBox_AddString(h, L"12");
	ComboBox_AddString(h, L"13");
	ComboBox_SetCurSel(h, 1); // 12

	h = GetDlgItem(hDlg, IDC_CMB_COMPRESS);
	ComboBox_AddString(h, L"off");
	ComboBox_AddString(h, L"lz4");
	ComboBox_AddString(h, L"zstd");
	ComboBox_AddString(h, L"gzip");
	ComboBox_SetCurSel(h, 1); // lz4

	h = GetDlgItem(hDlg, IDC_CMB_COMPAT);
	PopulateCompatCombo(h);
}

// Red text for rows flagged boot/has_pool, so they read as "be careful"
// without hard-blocking selection (force is implied by checking them).
static LRESULT
HandleDiskListCustomDraw(HWND hDlg, NMLVCUSTOMDRAW *cd, const DiskList *dl)
{
	if (cd->nmcd.dwDrawStage == CDDS_PREPAINT)
		return (CDRF_NOTIFYITEMDRAW);
	if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
		int i = (int)cd->nmcd.dwItemSpec;
		if (dl && i >= 0 && i < dl->count &&
		    (dl->items[i].is_boot || dl->items[i].has_pool)) {
			cd->clrText = RGB(0xC0, 0x00, 0x00);
		}
		return (CDRF_DODEFAULT);
	}
	return (CDRF_DODEFAULT);
}

static void
UpdateCreateButtonState(HWND hDlg)
{
	HWND hList = GetDlgItem(hDlg, IDC_DISKLIST);
	int n = ListView_GetItemCount(hList);
	BOOL anyChecked = FALSE;
	for (int i = 0; i < n; i++) {
		if (ListView_GetCheckState(hList, i)) {
			anyChecked = TRUE;
			break;
		}
	}

	wchar_t nameW[128] = L"";
	GetDlgItemTextW(hDlg, IDC_ED_POOLNAME, nameW, ARRAYSIZE(nameW));

	BOOL confirmed = IsDlgButtonChecked(hDlg, IDC_CHK_CONFIRM_ERASE) ==
	    BST_CHECKED;

	EnableWindow(GetDlgItem(hDlg, IDC_BTN_CREATE),
	    anyChecked && nameW[0] && confirmed);
}

static DWORD
ComposeCreatePoolBody(uint8_t *buf, DWORD cap, uint32_t flags,
    uint32_t topology, const char **diskPathsUtf8, uint32_t ndisks,
    const char *poolnameUtf8, const char *propsBlobUtf8,
    const char *passUtf8)
{
	op_create_pool_req_t *rq = (op_create_pool_req_t *)buf;
	if (cap < sizeof (*rq))
		return (0);
	rq->flags = flags;
	rq->topology = topology;
	rq->ndisks = ndisks;

	DWORD off = sizeof (*rq);
	for (uint32_t i = 0; i < ndisks; i++) {
		size_t n = strlen(diskPathsUtf8[i]) + 1;
		if (off + n > cap)
			return (0);
		memcpy(buf + off, diskPathsUtf8[i], n);
		off += (DWORD)n;
	}

	size_t n = strlen(poolnameUtf8) + 1;
	if (off + n > cap)
		return (0);
	memcpy(buf + off, poolnameUtf8, n);
	off += (DWORD)n;

	n = strlen(propsBlobUtf8) + 1;
	if (off + n > cap)
		return (0);
	memcpy(buf + off, propsBlobUtf8, n);
	off += (DWORD)n;

	n = strlen(passUtf8) + 1;
	if (off + n > cap)
		return (0);
	memcpy(buf + off, passUtf8, n);
	off += (DWORD)n;

	return (off);
}

static int
ParseCreateResult(const char *json, int len, BOOL *ok_out,
    wchar_t *nameOrErrW, size_t nameOrErrCch)
{
	jsmn_parser p;
	jsmntok_t tok[64];
	jsmn_init(&p);
	int n = jsmn_parse(&p, json, len, tok, _countof(tok));
	if (n < 1 || tok[0].type != JSMN_OBJECT)
		return (0);

	BOOL ok = FALSE;
	char buf[512] = { 0 };
	for (int i = 1; i < n; i++) {
		if (tok[i].type != JSMN_STRING)
			continue;
		if (jsmn_eq(json, &tok[i], "ok") && i + 1 < n) {
			ok = (json[tok[i + 1].start] == 't');
			i++;
		} else if (jsmn_eq(json, &tok[i], "err") && i + 1 < n &&
		    tok[i + 1].type == JSMN_STRING) {
			jsmn_copy_string(json, &tok[i + 1], buf, sizeof (buf));
			i++;
		} else if (jsmn_eq(json, &tok[i], "name") && i + 1 < n &&
		    tok[i + 1].type == JSMN_STRING && !buf[0]) {
			jsmn_copy_string(json, &tok[i + 1], buf, sizeof (buf));
			i++;
		}
	}
	*ok_out = ok;
	MultiByteToWideChar(CP_UTF8, 0, buf, -1, nameOrErrW,
	    (int)nameOrErrCch);
	return (1);
}

// ---- files as vdevs

typedef struct {
	uint64_t bytes;
} NewFileCtx;

static INT_PTR CALLBACK
NewFileDlgProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam)
{
	NewFileCtx *nf = (NewFileCtx *)GetWindowLongPtrW(h, GWLP_USERDATA);

	switch (msg) {
	case WM_INITDIALOG: {
		HWND c = GetDlgItem(h, IDC_CMB_FUNIT);
		SetWindowLongPtrW(h, GWLP_USERDATA, lParam);
		ComboBox_AddString(c, L"MB");
		ComboBox_AddString(c, L"GB");
		ComboBox_AddString(c, L"TB");
		ComboBox_SetCurSel(c, 1);
		SetDlgItemTextW(h, IDC_ED_FSIZE, L"1");
		ApplyThemeFollowSystem(h);
		return (TRUE);
	}
	case WM_COMMAND:
		if (LOWORD(wParam) == IDCANCEL) {
			EndDialog(h, IDCANCEL);
			return (TRUE);
		}
		if (LOWORD(wParam) == IDOK) {
			wchar_t buf[32] = L"";
			static const uint64_t unit[] = { 1ULL << 20, 1ULL << 30,
				1ULL << 40 };
			HWND cmb = GetDlgItem(h, IDC_CMB_FUNIT);
			int sel = ComboBox_GetCurSel(cmb);
			GetDlgItemTextW(h, IDC_ED_FSIZE, buf, ARRAYSIZE(buf));
			double v = _wtof(buf);
			if (sel < 0 || sel > 2)
				sel = 1;
			double sz = v * (double)unit[sel];
			if (v <= 0 || sz > (double)(1ULL << 50) ||
			    (uint64_t)sz < (64ULL << 20)) {
				MessageBoxW(h,
				    L"The size must be at least 64 MB.",
				    L"OpenZFS", MB_OK | MB_ICONINFORMATION);
				return (TRUE);
			}
			nf->bytes = (uint64_t)sz;
			EndDialog(h, IDOK);
			return (TRUE);
		}
		break;
	}
	return (FALSE);
}

// Create (or empty and resize) a sparse file of the given size
static BOOL
CreateSparseFile(const wchar_t *path, uint64_t bytes)
{
	HANDLE f = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
	    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return (FALSE);

	// Not every file system can do sparse; the file is still usable
	DWORD ret = 0;
	(void) DeviceIoControl(f, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &ret,
	    NULL);

	LARGE_INTEGER li;
	li.QuadPart = (LONGLONG)bytes;
	BOOL ok = SetFilePointerEx(f, li, NULL, FILE_BEGIN) && SetEndOfFile(f);
	CloseHandle(f);
	if (!ok)
		DeleteFileW(path);
	return (ok);
}

// Add a file to the list as a ticked entry, or tick it if it is there.
static void
AddFileEntry(HWND hDlg, CreateCtx *ctx, const wchar_t *pathW, uint64_t bytes)
{
	HWND hList = GetDlgItem(hDlg, IDC_DISKLIST);
	DiskEntry de = { 0 };
	DiskList *dl = ctx->disks;
	int n = ListView_GetItemCount(hList);

	if (WideCharToMultiByte(CP_UTF8, 0, pathW, -1, de.path,
	    sizeof (de.path), NULL, NULL) == 0)
		return;

	if (dl != NULL) {
		for (int i = 0; i < dl->count; i++) {
			if (!dl->items[i].is_file ||
			    _stricmp(dl->items[i].path, de.path) != 0)
				continue;
			for (int r = 0; r < n; r++) {
				LVITEMW it = { 0 };
				it.mask = LVIF_PARAM;
				it.iItem = r;
				ListView_GetItem(hList, &it);
				if ((int)it.lParam == i) {
					ListView_SetCheckState(hList, r, TRUE);
					ListView_EnsureVisible(hList, r, FALSE);
				}
			}
			return;
		}
	} else {
		dl = (DiskList *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
		    sizeof (*dl));
		if (dl == NULL)
			return;
		ctx->disks = dl;
	}

	DiskEntry *items;
	if (dl->items == NULL)
		items = (DiskEntry *)HeapAlloc(GetProcessHeap(),
		    HEAP_ZERO_MEMORY, sizeof (DiskEntry));
	else
		items = (DiskEntry *)HeapReAlloc(GetProcessHeap(),
		    HEAP_ZERO_MEMORY, dl->items,
		    (dl->count + 1) * sizeof (DiskEntry));
	if (items == NULL)
		return;
	dl->items = items;

	de.is_file = TRUE;
	FormatBytesW(bytes, de.sizeW, ARRAYSIZE(de.sizeW));
	lstrcpynW(de.modelW, L"File", ARRAYSIZE(de.modelW));
	dl->items[dl->count] = de;
	int row = InsertDiskRow(hList, &dl->items[dl->count], n, dl->count);
	dl->count++;
	ListView_SetCheckState(hList, row, TRUE);
	ListView_EnsureVisible(hList, row, FALSE);
}

// "Add file..." picks an existing file, "New file..." makes a sparse one.
static void
DoAddFile(HWND hDlg, CreateCtx *ctx, BOOL create)
{
	wchar_t path[MAX_PATH] = L"";
	OPENFILENAMEW ofn = { 0 };
	uint64_t bytes = 0;

	ofn.lStructSize = sizeof (ofn);
	ofn.hwndOwner = hDlg;
	ofn.lpstrFilter = L"All files\0*.*\0\0";
	ofn.lpstrFile = path;
	ofn.nMaxFile = ARRAYSIZE(path);
	ofn.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

	if (create) {
		NewFileCtx nf = { 0 };

		ofn.lpstrTitle = L"Create a file for the pool";
		ofn.Flags |= OFN_OVERWRITEPROMPT;
		if (!GetSaveFileNameW(&ofn))
			return;
		if (DialogBoxParamW(GetModuleHandleW(NULL),
		    MAKEINTRESOURCEW(IDD_NEWFILE), hDlg, NewFileDlgProc,
		    (LPARAM)&nf) != IDOK)
			return;
		if (!CreateSparseFile(path, nf.bytes)) {
			MessageBoxW(hDlg, L"Could not create the file.",
			    L"OpenZFS", MB_OK | MB_ICONERROR);
			return;
		}
		bytes = nf.bytes;
	} else {
		LARGE_INTEGER sz = { 0 };

		ofn.lpstrTitle = L"Select a file to use for the pool";
		ofn.Flags |= OFN_FILEMUSTEXIST;
		if (!GetOpenFileNameW(&ofn))
			return;
		HANDLE f = CreateFileW(path, FILE_READ_ATTRIBUTES,
		    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (f != INVALID_HANDLE_VALUE) {
			GetFileSizeEx(f, &sz);
			CloseHandle(f);
		}
		bytes = (uint64_t)sz.QuadPart;
	}

	AddFileEntry(hDlg, ctx, path, bytes);
}

static INT_PTR CALLBACK
CreatePoolDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	CreateCtx *ctx = (CreateCtx *)GetWindowLongPtrW(hDlg, GWLP_USERDATA);

	switch (msg) {
	case WM_INITDIALOG: {
		SetWindowLongPtrW(hDlg, GWLP_USERDATA, (LONG_PTR)lParam);
		ctx = (CreateCtx *)lParam;
		ctx->hWnd = hDlg;

		INITCOMMONCONTROLSEX icc = { sizeof (icc),
		    ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
		InitCommonControlsEx(&icc);

		SetDlgIcon(hDlg, IDC_ICON_HDR, IDI_APP, 32, 32);
		ctx->hHeaderFont = CreateHeaderFont(hDlg);
		if (ctx->hHeaderFont)
			SendDlgItemMessageW(hDlg, IDC_TITLE, WM_SETFONT,
			    (WPARAM)ctx->hHeaderFont, TRUE);
		ApplyThemeFollowSystem(hDlg);
		PositionNearCursor(hDlg);

		HWND hList = GetDlgItem(hDlg, IDC_DISKLIST);
		AddDiskListColumns(hList);
		ListView_SetExtendedListViewStyle(hList,
		    LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);

		InitTopologyCombo(hDlg);
		InitPropCombos(hDlg);

		SetDlgItemTextW(hDlg, IDC_STATUS_CREATE,
		    L"Scanning for disks...");
		EnableWindow(GetDlgItem(hDlg, IDC_BTN_CREATE), FALSE);
		EnableWindow(GetDlgItem(hDlg, IDC_BTN_ADDFILE), FALSE);
		EnableWindow(GetDlgItem(hDlg, IDC_BTN_NEWFILE), FALSE);

		ctx->hScanThread = CreateThread(NULL, 0, DiskScanThread, ctx,
		    0, NULL);
		return (TRUE);
	}

	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDC_BTN_CANCEL_CREATE:
			DestroyWindow(hDlg); // modeless
			return (TRUE);

		case IDC_BTN_ADDFILE:
			DoAddFile(hDlg, ctx, FALSE);
			return (TRUE);

		case IDC_BTN_NEWFILE:
			DoAddFile(hDlg, ctx, TRUE);
			return (TRUE);

		case IDC_ED_POOLNAME:
			if (HIWORD(wParam) == EN_CHANGE)
				UpdateCreateButtonState(hDlg);
			return (TRUE);

		case IDC_CHK_CONFIRM_ERASE:
			UpdateCreateButtonState(hDlg);
			return (TRUE);

		case IDC_BTN_CREATE: {
			HWND hList = GetDlgItem(hDlg, IDC_DISKLIST);
			DiskList *dl = ctx->disks;

			uint32_t ndisks = 0;
			int n = ListView_GetItemCount(hList);
			for (int i = 0; i < n; i++)
				if (ListView_GetCheckState(hList, i))
					ndisks++;

			if (ndisks == 0 || !dl) {
				MessageBoxW(hDlg, L"Select at least one disk.",
				    L"OpenZFS", MB_OK | MB_ICONINFORMATION);
				return (TRUE);
			}

			char **diskPaths = (char **)HeapAlloc(GetProcessHeap(),
			    0, ndisks * sizeof (char *));
			if (!diskPaths)
				return (TRUE);

			BOOL anyFlagged = FALSE;
			uint32_t j = 0;
			for (int i = 0; i < n && j < ndisks; i++) {
				if (!ListView_GetCheckState(hList, i))
					continue;
				LVITEMW it = { 0 };
				it.mask = LVIF_PARAM;
				it.iItem = i;
				ListView_GetItem(hList, &it);
				int di = (int)it.lParam;
				if (di >= 0 && di < dl->count) {
					diskPaths[j] = dl->items[di].path;
					if (dl->items[di].is_boot ||
					    dl->items[di].has_pool)
						anyFlagged = TRUE;
				} else {
					diskPaths[j] = "";
				}
				j++;
			}

			if (anyFlagged) {
				if (MessageBoxW(hDlg,
				    L"One or more selected disks or partitions "
				    "is the boot volume or already has a ZFS "
				    "pool on it.\n\n"
				    "Continuing will ERASE it. Proceed?",
				    L"OpenZFS", MB_YESNO | MB_ICONWARNING |
				    MB_DEFBUTTON2) != IDYES) {
					HeapFree(GetProcessHeap(), 0,
					    diskPaths);
					return (TRUE);
				}
			}

			wchar_t nameW[128] = L"";
			GetDlgItemTextW(hDlg, IDC_ED_POOLNAME, nameW,
			    ARRAYSIZE(nameW));
			char nameUtf8[192] = { 0 };
			WideCharToMultiByte(CP_UTF8, 0, nameW, -1, nameUtf8,
			    sizeof (nameUtf8), NULL, NULL);

			int topology = ComboBox_GetCurSel(
			    GetDlgItem(hDlg, IDC_CMB_TOPOLOGY));
			if (topology < 0) topology = 0;

			wchar_t caseW[16], ashiftW[8], compW[8];
			GetDlgItemTextW(hDlg, IDC_CMB_CASESENS, caseW,
			    ARRAYSIZE(caseW));
			GetDlgItemTextW(hDlg, IDC_CMB_ASHIFT, ashiftW,
			    ARRAYSIZE(ashiftW));
			GetDlgItemTextW(hDlg, IDC_CMB_COMPRESS, compW,
			    ARRAYSIZE(compW));

			// Indices 0/1 are this combo's own decorated display
			// text ("off (all features)" / "legacy (v28, no
			// features)"), not valid property values - translate
			// back to the literal "off"/"legacy" that
			// zpool_load_compat() expects. Any other entry is a
			// bare filename read straight out of compatibility.d,
			// used as-is.
			wchar_t compatW[64];
			int compatSel = ComboBox_GetCurSel(
			    GetDlgItem(hDlg, IDC_CMB_COMPAT));
			if (compatSel == 1) {
				lstrcpynW(compatW, L"legacy",
				    ARRAYSIZE(compatW));
			} else if (compatSel > 1) {
				GetDlgItemTextW(hDlg, IDC_CMB_COMPAT, compatW,
				    ARRAYSIZE(compatW));
			} else {
				lstrcpynW(compatW, L"off", ARRAYSIZE(compatW));
			}

			wchar_t extraW[4096] = L"";
			GetDlgItemTextW(hDlg, IDC_ED_PROPS, extraW,
			    ARRAYSIZE(extraW));

			wchar_t blobW[4400];
			_snwprintf_s(blobW, ARRAYSIZE(blobW), _TRUNCATE,
			    L"casesensitivity=%s\r\ncompression=%s\r\n"
			    L"ashift=%s\r\ncompatibility=%s\r\n%s\r\n",
			    caseW, compW, ashiftW, compatW, extraW);
			char blobUtf8[8192];
			WideCharToMultiByte(CP_UTF8, 0, blobW, -1, blobUtf8,
			    sizeof (blobUtf8), NULL, NULL);

			uint8_t *pass = NULL;
			uint32_t passlen = 0;
			BOOL wantEncryption = IsDlgButtonChecked(hDlg,
			    IDC_CHK_ENCRYPTION) == BST_CHECKED;
			if (wantEncryption) {
				if (!PromptPassphrase(hDlg, nameW, &pass,
				    &passlen)) {
					HeapFree(GetProcessHeap(), 0,
					    diskPaths);
					return (TRUE);
				}
			}

			uint32_t flags = anyFlagged ? ZCREATE_FORCE : 0;

			uint8_t body[8192 + 512];
			DWORD blen = ComposeCreatePoolBody(body, sizeof (body),
			    flags, (uint32_t)topology,
			    (const char **)diskPaths, ndisks, nameUtf8,
			    blobUtf8, pass ? (const char *)pass : "");

			if (pass) {
				SecureZeroMemory(pass, passlen);
				HeapFree(GetProcessHeap(), 0, pass);
			}
			HeapFree(GetProcessHeap(), 0, diskPaths);

			if (!blen) {
				MessageBoxW(hDlg, L"Invalid parameters.",
				    L"OpenZFS", MB_OK | MB_ICONERROR);
				return (TRUE);
			}

			EnableWindow(GetDlgItem(hDlg, IDC_BTN_CREATE), FALSE);
			SetDlgItemTextW(hDlg, IDC_STATUS_CREATE,
			    L"Creating pool...");

			uint8_t *out = NULL;
			uint32_t st = 0, outlen = 0;
			BOOL callok = zrpc_call(ctx->rpc, OP_CREATE_POOL, body,
			    blen, &st, &out, &outlen) && st == 0 && out;

			BOOL ok = FALSE;
			wchar_t resultW[512] = L"";
			if (callok) {
				ParseCreateResult((const char *)out,
				    (int)outlen, &ok, resultW,
				    ARRAYSIZE(resultW));
				HeapFree(GetProcessHeap(), 0, out);
			} else {
				if (out) HeapFree(GetProcessHeap(), 0, out);
				lstrcpynW(resultW, L"Service unavailable.",
				    ARRAYSIZE(resultW));
			}

			wchar_t *dup = _wcsdup(resultW);
			PostMessageW(hDlg, WM_APP_CREATE_DONE, ok,
			    (LPARAM)dup);
			return (TRUE);
		}
		}
		return (FALSE);

	case WM_NOTIFY: {
		LPNMHDR nh = (LPNMHDR)lParam;
		if (nh->idFrom == IDC_DISKLIST) {
			if (nh->code == LVN_ITEMCHANGED) {
				NMLISTVIEW *lv = (NMLISTVIEW *)lParam;
				if (lv->uChanged & LVIF_STATE)
					UpdateCreateButtonState(hDlg);
			} else if (nh->code == NM_CUSTOMDRAW) {
				LRESULT r = HandleDiskListCustomDraw(hDlg,
				    (NMLVCUSTOMDRAW *)lParam, ctx->disks);
				SetWindowLongPtrW(hDlg, DWLP_MSGRESULT, r);
				return (TRUE);
			}
		}
		return (FALSE);
	}

	case WM_APP_DISKS_DONE: {
		DiskList *dl = (DiskList *)lParam;
		HWND hList = GetDlgItem(hDlg, IDC_DISKLIST);

		if (ctx->disks) FreeDiskList(ctx->disks);
		ctx->disks = dl;
		EnableWindow(GetDlgItem(hDlg, IDC_BTN_ADDFILE), TRUE);
		EnableWindow(GetDlgItem(hDlg, IDC_BTN_NEWFILE), TRUE);

		if (!dl || dl->count == 0) {
			SetDlgItemTextW(hDlg, IDC_STATUS_CREATE,
			    L"No candidate disks found.");
		} else {
			SetDlgItemTextW(hDlg, IDC_STATUS_CREATE,
			    L"Select disk(s) or partition(s), set options, "
			    L"and click Create.");
			PopulateDiskList(hList, dl);
		}
		return (TRUE);
	}

	case WM_APP_CREATE_DONE: {
		BOOL ok = (BOOL)wParam;
		wchar_t *msg = (wchar_t *)lParam;
		wchar_t full[600];
		_snwprintf_s(full, ARRAYSIZE(full), _TRUNCATE,
		    ok ? L"Pool '%s' created." : L"Create failed: %s",
		    msg ? msg : L"");
		MessageBoxW(hDlg, full, L"OpenZFS",
		    MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
		if (msg) free(msg);

		SetDlgItemTextW(hDlg, IDC_STATUS_CREATE, L"Ready.");
		if (ok) {
			DestroyWindow(hDlg);
		} else {
			UpdateCreateButtonState(hDlg);
		}
		return (TRUE);
	}

	case WM_DESTROY:
		if (ctx) {
			if (ctx->hScanThread) {
				CloseHandle(ctx->hScanThread);
				ctx->hScanThread = NULL;
			}
			if (ctx->disks) {
				FreeDiskList(ctx->disks);
				ctx->disks = NULL;
			}
			if (ctx->hHeaderFont)
				DeleteObject(ctx->hHeaderFont);
			HeapFree(GetProcessHeap(), 0, ctx);
			SetWindowLongPtrW(hDlg, GWLP_USERDATA, 0);
		}
		return (TRUE);
	}
	return (FALSE);
}

HWND
CreateCreatePoolWindow(HWND hParent, zrpc_t *rpc)
{
	CreateCtx *ctx = (CreateCtx *)HeapAlloc(GetProcessHeap(),
	    HEAP_ZERO_MEMORY, sizeof (*ctx));
	if (!ctx)
		return (NULL);
	ctx->rpc = rpc;

	HWND hDlg = CreateDialogParamW(GetModuleHandleW(NULL),
	    MAKEINTRESOURCEW(IDD_CREATE_POOL), hParent, CreatePoolDlgProc,
	    (LPARAM)ctx);
	if (hDlg) ShowWindow(hDlg, SW_SHOW);
	return (hDlg);
}
