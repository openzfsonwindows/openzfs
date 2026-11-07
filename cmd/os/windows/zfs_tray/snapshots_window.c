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
 * Copyright (c) 2026 Jorgen Lundman <lundman@lundman.net>.
 */

// cmd/os/windows/zfs_tray/snapshots_window.c
//
// "Snapshots": pick a dataset, see its snapshots, take a new one or destroy
// some. Started as "zfs_tray.exe --snapshots [dataset]". All the work is done
// by the service (OP_LIST_DATASETS, OP_LIST_SNAPSHOTS, OP_SNAPSHOT_CREATE,
// OP_SNAPSHOT_DESTROY).
#define	_CRT_SECURE_NO_WARNINGS
#define	UNICODE
#define	_UNICODE
#define	_WIN32_WINNT 0x0600
#define	_WIN32_IE    0x0600
#include <windows.h>
#include <commctrl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define	JSMN_STATIC
#include "jsmn.h"
#include "jsmn_utils.h"

#include "pipe_rpc.h"
#include "rpc_client.h"
#include "snapshots_window.h"
#include "pass_prompt.h"
#include "dlg_util.h"

#pragma comment(lib, "comctl32.lib")

#ifndef ARRAYSIZE
#define	ARRAYSIZE(a) (sizeof (a)/sizeof ((a)[0]))
#endif

#define	IDC_SDATASET	3301
#define	IDC_SLIST	3302
#define	IDC_SNAME	3303
#define	IDC_SRECURSIVE	3304
#define	IDC_SCREATE	3305
#define	IDC_SDESTROY	3306
#define	IDC_SREFRESH	3307
#define	IDC_SINFO	3308
#define	IDC_SNEWLBL	3309
#define	IDC_SDSLBL	3310
#define	IDC_SROLLBACK	3311
#define	IDC_SMOUNT	3312
#define	IDC_SUNMOUNT	3313
#define	IDC_SDSINFO	3314

// Mirror of the service's flags, see ops_snapshots.h
#define	ZSNAP_RECURSIVE	0x01
#define	ZROLL_DESTROY_NEWER	0x01
#define	ZROLL_DESTROY_CLONES	0x02
#define	ZUMNT_FORCE	0x01

typedef struct {
	wchar_t name[256];
	wchar_t mountpoint[260];
	int depth;
	BOOL volume;
	BOOL mounted;
	BOOL encrypted;
	BOOL keyloaded;
	uint64_t used;
	uint64_t avail;
	HTREEITEM item;
} DsEnt;

typedef struct {
	wchar_t name[256];
	uint64_t creation;
	uint64_t used;
	uint64_t refer;
	uint64_t holds;
} SnapEnt;

typedef struct {
	HWND hWnd;
	HWND hList;
	HWND hTree;
	HFONT hHeaderFont;
	zrpc_t rpc;
	DsEnt *ds;
	int nds;
	SnapEnt *snaps;
	int nsnaps;
	wchar_t cur[256];	// dataset whose snapshots are shown
	BOOL building;		// filling the tree, ignore its notifications
} SCtx;

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
	char b[1024] = { 0 };

	jsmn_copy_string(json, v, b, sizeof (b));
	out[0] = L'\0';
	MultiByteToWideChar(CP_UTF8, 0, b, -1, out, cch);
}

static jsmntok_t *
ParseTokens(const char *json, int json_len, int *r)
{
	jsmn_parser p;
	jsmntok_t *tok;
	int ntok = json_len / 2 + 16;

	tok = (jsmntok_t *)HeapAlloc(GetProcessHeap(), 0,
	    ntok * sizeof (jsmntok_t));
	if (!tok)
		return (NULL);
	jsmn_init(&p);
	*r = jsmn_parse(&p, json, json_len, tok, ntok);
	if (*r < 1 || tok[0].type != JSMN_OBJECT) {
		HeapFree(GetProcessHeap(), 0, tok);
		return (NULL);
	}
	return (tok);
}

// TRUE if the service said "ok":true; *err gets its message otherwise.
static BOOL
ReplyOk(const char *json, int json_len, wchar_t *err, int errcch)
{
	jsmntok_t *tok;
	int r, i;
	BOOL ok = FALSE;

	tok = ParseTokens(json, json_len, &r);
	if (tok == NULL)
		return (FALSE);
	i = 1;
	while (i + 1 < r && tok[i].start < tok[0].end) {
		if (jsmn_eq(json, &tok[i], "ok"))
			ok = PrimTrue(json, &tok[i + 1]);
		else if (jsmn_eq(json, &tok[i], "err"))
			TokWide(json, &tok[i + 1], err, errcch);
		i = SkipValue(tok, i + 1, r);
	}
	HeapFree(GetProcessHeap(), 0, tok);
	return (ok);
}

// TRUE if the reply has the top level boolean "key" set.
static BOOL
ReplyBool(const char *json, int json_len, const char *key)
{
	jsmntok_t *tok;
	int r, i;
	BOOL val = FALSE;

	tok = ParseTokens(json, json_len, &r);
	if (tok == NULL)
		return (FALSE);
	i = 1;
	while (i + 1 < r && tok[i].start < tok[0].end) {
		if (jsmn_eq(json, &tok[i], key))
			val = PrimTrue(json, &tok[i + 1]);
		i = SkipValue(tok, i + 1, r);
	}
	HeapFree(GetProcessHeap(), 0, tok);
	return (val);
}

// One "key":value pair of a dataset object.
static void
DsField(const char *json, const jsmntok_t *k, const jsmntok_t *v, DsEnt *d)
{
	if (jsmn_eq(json, k, "name")) {
		const wchar_t *s;

		TokWide(json, v, d->name, ARRAYSIZE(d->name));
		for (s = d->name; *s; s++)
			if (*s == L'/')
				d->depth++;
	} else if (jsmn_eq(json, k, "mountpoint")) {
		TokWide(json, v, d->mountpoint, ARRAYSIZE(d->mountpoint));
	} else if (jsmn_eq(json, k, "volume")) {
		d->volume = PrimTrue(json, v);
	} else if (jsmn_eq(json, k, "mounted")) {
		d->mounted = PrimTrue(json, v);
	} else if (jsmn_eq(json, k, "encrypted")) {
		d->encrypted = PrimTrue(json, v);
	} else if (jsmn_eq(json, k, "keyloaded")) {
		d->keyloaded = PrimTrue(json, v);
	} else if (jsmn_eq(json, k, "used")) {
		d->used = TokU64(json, v);
	} else if (jsmn_eq(json, k, "avail")) {
		d->avail = TokU64(json, v);
	}
}

static BOOL
ParseDatasets(SCtx *c, const char *json, int json_len)
{
	jsmntok_t *tok;
	int r, i;
	BOOL ok = FALSE;

	tok = ParseTokens(json, json_len, &r);
	if (tok == NULL)
		return (FALSE);

	i = 1;
	while (i + 1 < r && tok[i].start < tok[0].end) {
		const jsmntok_t *k = &tok[i], *v = &tok[i + 1];

		if (jsmn_eq(json, k, "ok")) {
			ok = PrimTrue(json, v);
		} else if (jsmn_eq(json, k, "datasets") &&
		    v->type == JSMN_ARRAY) {
			int n = v->size, e, j = i + 2;

			c->ds = (DsEnt *)HeapAlloc(GetProcessHeap(),
			    HEAP_ZERO_MEMORY,
			    (n > 0 ? n : 1) * sizeof (DsEnt));
			if (!c->ds)
				break;
			for (e = 0; e < n && j < r; e++) {
				int oend = tok[j].end, q = j + 1;
				DsEnt *d = &c->ds[c->nds];

				while (q + 1 < r && tok[q].start < oend) {
					DsField(json, &tok[q], &tok[q + 1], d);
					q = SkipValue(tok, q + 1, r);
				}
				c->nds++;
				j = q;
			}
			i = j;
			continue;
		}
		i = SkipValue(tok, i + 1, r);
	}
	HeapFree(GetProcessHeap(), 0, tok);
	return (ok);
}

static BOOL
ParseSnaps(SCtx *c, const char *json, int json_len)
{
	jsmntok_t *tok;
	int r, i;
	BOOL ok = FALSE;

	tok = ParseTokens(json, json_len, &r);
	if (tok == NULL)
		return (FALSE);

	i = 1;
	while (i + 1 < r && tok[i].start < tok[0].end) {
		const jsmntok_t *k = &tok[i], *v = &tok[i + 1];

		if (jsmn_eq(json, k, "ok")) {
			ok = PrimTrue(json, v);
		} else if (jsmn_eq(json, k, "snapshots") &&
		    v->type == JSMN_ARRAY) {
			int n = v->size, e, j = i + 2;

			c->snaps = (SnapEnt *)HeapAlloc(GetProcessHeap(),
			    HEAP_ZERO_MEMORY,
			    (n > 0 ? n : 1) * sizeof (SnapEnt));
			if (!c->snaps)
				break;
			for (e = 0; e < n && j < r; e++) {
				int oend = tok[j].end, q = j + 1;
				SnapEnt *s = &c->snaps[c->nsnaps];

				while (q + 1 < r && tok[q].start < oend) {
					const jsmntok_t *kk = &tok[q];
					const jsmntok_t *vv = &tok[q + 1];

					if (jsmn_eq(json, kk, "name"))
						TokWide(json, vv, s->name,
						    ARRAYSIZE(s->name));
					else if (jsmn_eq(json, kk, "creation"))
						s->creation = TokU64(json, vv);
					else if (jsmn_eq(json, kk, "used"))
						s->used = TokU64(json, vv);
					else if (jsmn_eq(json, kk, "refer"))
						s->refer = TokU64(json, vv);
					else if (jsmn_eq(json, kk, "holds"))
						s->holds = TokU64(json, vv);
					q = SkipValue(tok, q + 1, r);
				}
				c->nsnaps++;
				j = q;
			}
			i = j;
			continue;
		}
		i = SkipValue(tok, i + 1, r);
	}
	HeapFree(GetProcessHeap(), 0, tok);
	return (ok);
}

// -------------------- service calls --------------------

static void
SetInfo(SCtx *c, const wchar_t *text)
{
	SetDlgItemTextW(c->hWnd, IDC_SINFO, text);
}

static void
ErrorBox(HWND parent, const wchar_t *msg)
{
	MessageBoxW(parent, msg, L"ZFS datasets", MB_ICONERROR);
}

// One call. TRUE with *out (HeapFree it) on a reply; FALSE with err set.
static BOOL
Call(SCtx *c, uint32_t op, const void *in, uint32_t inlen, uint8_t **out,
    uint32_t *outlen, wchar_t *err, int errcch)
{
	uint32_t st = 0;

	*out = NULL;
	*outlen = 0;
	SetCursor(LoadCursorW(NULL, IDC_WAIT));
	if (!zrpc_call(&c->rpc, op, in, inlen, &st, out, outlen)) {
		lstrcpynW(err, L"Could not reach the OpenZFS service.",
		    errcch);
		return (FALSE);
	}
	SetCursor(LoadCursorW(NULL, IDC_ARROW));
	if (st != 0 || *out == NULL) {
		if (st == ERROR_ACCESS_DENIED)
			lstrcpynW(err, L"Administrator rights are needed.",
			    errcch);
		else
			_snwprintf_s(err, errcch, _TRUNCATE,
			    L"The OpenZFS service could not do that (%lu).",
			    (unsigned long)st);
		if (*out != NULL) {
			HeapFree(GetProcessHeap(), 0, *out);
			*out = NULL;
		}
		return (FALSE);
	}
	return (TRUE);
}

// A UTF-8 copy of a wide string, in buf; FALSE if it does not fit.
static BOOL
ToUtf8(const wchar_t *w, char *buf, int cb)
{
	return (WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, cb, NULL, NULL) >
	    0);
}

// -------------------- window state --------------------

static void
FreeSnaps(SCtx *c)
{
	if (c->snaps)
		HeapFree(GetProcessHeap(), 0, c->snaps);
	c->snaps = NULL;
	c->nsnaps = 0;
}

static void
AddColumns(HWND hList)
{
	static const struct { const wchar_t *name; int cx; } cols[] = {
		{ L"Snapshot", 200 }, { L"Created", 140 }, { L"Age", 100 },
		{ L"Used", 80 }, { L"Referenced", 90 }, { L"Holds", 50 },
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
UpdateButtons(SCtx *c)
{
	EnableWindow(GetDlgItem(c->hWnd, IDC_SDESTROY),
	    ListView_GetSelectedCount(c->hList) > 0);
	// Rolling back goes to exactly one snapshot.
	EnableWindow(GetDlgItem(c->hWnd, IDC_SROLLBACK),
	    ListView_GetSelectedCount(c->hList) == 1);
	EnableWindow(GetDlgItem(c->hWnd, IDC_SCREATE), c->cur[0] != L'\0');
}

// Newest first.
static void
Populate(SCtx *c)
{
	wchar_t buf[96];
	int i, row = 0;

	ListView_DeleteAllItems(c->hList);
	for (i = c->nsnaps - 1; i >= 0; i--) {
		const SnapEnt *s = &c->snaps[i];
		LVITEMW it = { 0 };

		it.mask = LVIF_TEXT | LVIF_PARAM;
		it.iItem = row;
		it.pszText = (LPWSTR)s->name;
		it.lParam = i;
		ListView_InsertItem(c->hList, &it);
		FormatTimeW(s->creation, buf, ARRAYSIZE(buf));
		ListView_SetItemText(c->hList, row, 1, buf);
		FormatAgeW(s->creation, buf, ARRAYSIZE(buf));
		ListView_SetItemText(c->hList, row, 2, buf);
		FormatBytesW(s->used, buf, ARRAYSIZE(buf));
		ListView_SetItemText(c->hList, row, 3, buf);
		FormatBytesW(s->refer, buf, ARRAYSIZE(buf));
		ListView_SetItemText(c->hList, row, 4, buf);
		if (s->holds != 0) {
			_snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE,
			    L"%llu", (unsigned long long)s->holds);
			ListView_SetItemText(c->hList, row, 5, buf);
		}
		row++;
	}
	UpdateButtons(c);
}

// Ask the service for the snapshots of c->cur.
static void
LoadSnapshots(SCtx *c)
{
	char utf8[1024];
	uint8_t *out = NULL;
	uint32_t outlen = 0;
	wchar_t err[256] = L"";
	wchar_t info[320];

	FreeSnaps(c);
	if (c->cur[0] == L'\0') {
		Populate(c);
		SetInfo(c, L"");
		return;
	}
	if (!ToUtf8(c->cur, utf8, sizeof (utf8))) {
		Populate(c);
		return;
	}
	if (!Call(c, OP_LIST_SNAPSHOTS, utf8, (uint32_t)strlen(utf8) + 1,
	    &out, &outlen, err, ARRAYSIZE(err))) {
		Populate(c);
		SetInfo(c, err);
		return;
	}
	if (!ParseSnaps(c, (const char *)out, (int)outlen)) {
		ReplyOk((const char *)out, (int)outlen, err, ARRAYSIZE(err));
		if (err[0] == L'\0')
			lstrcpynW(err, L"Unexpected reply from the service.",
			    ARRAYSIZE(err));
		FreeSnaps(c);
		HeapFree(GetProcessHeap(), 0, out);
		Populate(c);
		SetInfo(c, err);
		return;
	}
	HeapFree(GetProcessHeap(), 0, out);
	Populate(c);
	_snwprintf_s(info, ARRAYSIZE(info), _TRUNCATE, L"%d snapshot%s",
	    c->nsnaps, c->nsnaps == 1 ? L"" : L"s");
	SetInfo(c, info);
}

// "manual-20261107-153012"
static void
DefaultName(wchar_t *out, size_t cch)
{
	SYSTEMTIME lt;

	GetLocalTime(&lt);
	_snwprintf_s(out, cch, _TRUNCATE, L"manual-%04u%02u%02u-%02u%02u%02u",
	    lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond);
}

static void
DoCreate(SCtx *c)
{
	wchar_t name[256], err[256] = L"";
	wchar_t *p;
	char ds[1024], snap[512];
	uint8_t body[sizeof (uint32_t) + sizeof (ds) + sizeof (snap)];
	uint8_t *out = NULL;
	uint32_t outlen = 0, flags = 0;
	size_t n1, n2;

	GetDlgItemTextW(c->hWnd, IDC_SNAME, name, ARRAYSIZE(name));
	if (name[0] == L'\0') {
		ErrorBox(c->hWnd, L"Enter a name for the snapshot.");
		return;
	}
	if (wcschr(name, L'@') != NULL) {
		ErrorBox(c->hWnd, L"The name cannot contain '@'.");
		return;
	}
	for (p = name; *p; p++) {
		if (*p == L' ' || *p == L'/' || *p == L'\\') {
			ErrorBox(c->hWnd, L"The name cannot contain spaces "
			    L"or slashes.");
			return;
		}
	}
	if (!ToUtf8(c->cur, ds, sizeof (ds)) ||
	    !ToUtf8(name, snap, sizeof (snap))) {
		ErrorBox(c->hWnd, L"Invalid name.");
		return;
	}
	if (SendDlgItemMessageW(c->hWnd, IDC_SRECURSIVE, BM_GETCHECK, 0, 0) ==
	    BST_CHECKED)
		flags |= ZSNAP_RECURSIVE;

	n1 = strlen(ds) + 1;
	n2 = strlen(snap) + 1;
	memcpy(body, &flags, sizeof (flags));
	memcpy(body + sizeof (flags), ds, n1);
	memcpy(body + sizeof (flags) + n1, snap, n2);

	if (!Call(c, OP_SNAPSHOT_CREATE, body,
	    (uint32_t)(sizeof (flags) + n1 + n2), &out, &outlen, err,
	    ARRAYSIZE(err))) {
		ErrorBox(c->hWnd, err);
		return;
	}
	if (!ReplyOk((const char *)out, (int)outlen, err, ARRAYSIZE(err))) {
		if (err[0] == L'\0')
			lstrcpynW(err, L"The snapshot was not created.",
			    ARRAYSIZE(err));
		HeapFree(GetProcessHeap(), 0, out);
		ErrorBox(c->hWnd, err);
		return;
	}
	HeapFree(GetProcessHeap(), 0, out);

	DefaultName(name, ARRAYSIZE(name));
	SetDlgItemTextW(c->hWnd, IDC_SNAME, name);
	LoadSnapshots(c);
}

static void
DoDestroy(SCtx *c)
{
	int sel, count = 0;
	wchar_t msg[1024], err[256] = L"";
	wchar_t list[512] = L"";
	char ds[1024];
	uint8_t *body = NULL;
	size_t cap, used;
	uint32_t flags = 0, ucount;
	uint8_t *out = NULL;
	uint32_t outlen = 0;
	size_t dslen;

	if (!ToUtf8(c->cur, ds, sizeof (ds)))
		return;
	dslen = strlen(ds) + 1;

	// Worst case UTF-8 size of every selected name, plus the header.
	cap = 2 * sizeof (uint32_t) + dslen;
	sel = -1;
	while ((sel = ListView_GetNextItem(c->hList, sel,
	    LVNI_SELECTED)) >= 0)
		cap += 4 * 256;
	body = (uint8_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cap);
	if (body == NULL)
		return;
	used = 2 * sizeof (uint32_t);
	memcpy(body + used, ds, dslen);
	used += dslen;

	sel = -1;
	while ((sel = ListView_GetNextItem(c->hList, sel,
	    LVNI_SELECTED)) >= 0) {
		LVITEMW it = { 0 };
		int len;

		it.mask = LVIF_PARAM;
		it.iItem = sel;
		if (!ListView_GetItem(c->hList, &it) || it.lParam < 0 ||
		    it.lParam >= c->nsnaps)
			continue;
		len = WideCharToMultiByte(CP_UTF8, 0,
		    c->snaps[it.lParam].name, -1, (char *)body + used,
		    (int)(cap - used), NULL, NULL);
		if (len <= 0)
			continue;
		used += len;
		// The first few, for the question
		if (count < 5) {
			wcsncat_s(list, ARRAYSIZE(list),
			    c->snaps[it.lParam].name, _TRUNCATE);
			wcsncat_s(list, ARRAYSIZE(list), L"\n", _TRUNCATE);
		}
		count++;
	}
	if (count == 0) {
		HeapFree(GetProcessHeap(), 0, body);
		return;
	}
	if (count > 5)
		wcsncat_s(list, ARRAYSIZE(list), L"...\n", _TRUNCATE);

	_snwprintf_s(msg, ARRAYSIZE(msg), _TRUNCATE,
	    L"Destroy %d snapshot%s of %s?\n\n%s\nThis cannot be undone.",
	    count, count == 1 ? L"" : L"s", c->cur, list);
	if (MessageBoxW(c->hWnd, msg, L"Destroy snapshots",
	    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
		HeapFree(GetProcessHeap(), 0, body);
		return;
	}

	ucount = (uint32_t)count;
	memcpy(body, &flags, sizeof (flags));
	memcpy(body + sizeof (flags), &ucount, sizeof (ucount));
	if (!Call(c, OP_SNAPSHOT_DESTROY, body, (uint32_t)used, &out, &outlen,
	    err, ARRAYSIZE(err))) {
		HeapFree(GetProcessHeap(), 0, body);
		ErrorBox(c->hWnd, err);
		return;
	}
	HeapFree(GetProcessHeap(), 0, body);
	if (!ReplyOk((const char *)out, (int)outlen, err, ARRAYSIZE(err))) {
		if (err[0] == L'\0')
			lstrcpynW(err, L"The snapshots were not destroyed.",
			    ARRAYSIZE(err));
		HeapFree(GetProcessHeap(), 0, out);
		ErrorBox(c->hWnd, err);
		LoadSnapshots(c);
		return;
	}
	HeapFree(GetProcessHeap(), 0, out);
	LoadSnapshots(c);
}

static void
DoRollback(SCtx *c)
{
	int sel = ListView_GetNextItem(c->hList, -1, LVNI_SELECTED);
	LVITEMW it = { 0 };
	const SnapEnt *s;
	wchar_t msg[1024], err[256] = L"";
	char ds[1024], snap[512];
	uint8_t body[sizeof (uint32_t) + sizeof (ds) + sizeof (snap)];
	uint8_t *out = NULL;
	uint32_t outlen = 0, flags = 0;
	size_t n1, n2;
	int newer;
	BOOL again = TRUE;

	if (sel < 0)
		return;
	it.mask = LVIF_PARAM;
	it.iItem = sel;
	if (!ListView_GetItem(c->hList, &it) || it.lParam < 0 ||
	    it.lParam >= c->nsnaps)
		return;
	s = &c->snaps[it.lParam];
	// The list is oldest first, so these are the ones after it.
	newer = c->nsnaps - 1 - (int)it.lParam;

	if (newer == 0) {
		_snwprintf_s(msg, ARRAYSIZE(msg), _TRUNCATE,
		    L"Roll back %s to %s?\n\nEverything changed since this "
		    L"snapshot was taken will be lost.", c->cur, s->name);
	} else {
		_snwprintf_s(msg, ARRAYSIZE(msg), _TRUNCATE,
		    L"Roll back %s to %s?\n\nThis also DESTROYS the %d newer "
		    L"snapshot%s, and everything changed since %s will be "
		    L"lost.\n\nTo get single files back without losing "
		    L"anything, cancel and use ZFS versions instead.",
		    c->cur, s->name, newer, newer == 1 ? L"" : L"s",
		    s->name);
		flags |= ZROLL_DESTROY_NEWER;
	}
	if (MessageBoxW(c->hWnd, msg, L"Roll back",
	    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
		return;

	if (!ToUtf8(c->cur, ds, sizeof (ds)) ||
	    !ToUtf8(s->name, snap, sizeof (snap))) {
		ErrorBox(c->hWnd, L"Invalid name.");
		return;
	}
	n1 = strlen(ds) + 1;
	n2 = strlen(snap) + 1;
	memcpy(body + sizeof (flags), ds, n1);
	memcpy(body + sizeof (flags) + n1, snap, n2);

	while (again) {
		again = FALSE;
		memcpy(body, &flags, sizeof (flags));
		if (!Call(c, OP_SNAPSHOT_ROLLBACK, body,
		    (uint32_t)(sizeof (flags) + n1 + n2), &out, &outlen, err,
		    ARRAYSIZE(err))) {
			ErrorBox(c->hWnd, err);
			return;
		}
		if (ReplyOk((const char *)out, (int)outlen, err,
		    ARRAYSIZE(err))) {
			HeapFree(GetProcessHeap(), 0, out);
			LoadSnapshots(c);
			_snwprintf_s(msg, ARRAYSIZE(msg), _TRUNCATE,
			    L"Rolled back to %s", s->name);
			SetInfo(c, msg);
			return;
		}
		if (!(flags & ZROLL_DESTROY_CLONES) &&
		    ReplyBool((const char *)out, (int)outlen,
		    "clones_block")) {
			// Other datasets are cloned from the snapshots that
			// would go; they would have to go as well.
			_snwprintf_s(msg, ARRAYSIZE(msg), _TRUNCATE,
			    L"%s.\n\nRolling back needs those clones to be "
			    L"destroyed too. Destroy them and roll back?",
			    err);
			HeapFree(GetProcessHeap(), 0, out);
			if (MessageBoxW(c->hWnd, msg, L"Roll back",
			    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) ==
			    IDYES) {
				flags |= ZROLL_DESTROY_CLONES;
				err[0] = L'\0';
				again = TRUE;
				continue;
			}
			return;
		}
		if (err[0] == L'\0')
			lstrcpynW(err, L"The rollback failed.", ARRAYSIZE(err));
		HeapFree(GetProcessHeap(), 0, out);
		ErrorBox(c->hWnd, err);
		LoadSnapshots(c);
		return;
	}
}

// -------------------- mount / unmount --------------------

static const DsEnt *
CurDs(const SCtx *c)
{
	int i;

	for (i = 0; i < c->nds; i++)
		if (lstrcmpiW(c->ds[i].name, c->cur) == 0)
			return (&c->ds[i]);
	return (NULL);
}

static BOOL LoadDatasets(SCtx *c, const wchar_t *want, BOOL load_snaps);

// The names in {"locked":[{"name":..}, ..]} from OP_MOUNT_PREFLIGHT.
static int
ParseLocked(const char *json, int json_len, wchar_t names[][256], int max)
{
	jsmntok_t *tok;
	int r, i, n = 0;

	tok = ParseTokens(json, json_len, &r);
	if (tok == NULL)
		return (0);
	i = 1;
	while (i + 1 < r && tok[i].start < tok[0].end) {
		const jsmntok_t *v = &tok[i + 1];

		if (jsmn_eq(json, &tok[i], "locked") && v->type == JSMN_ARRAY) {
			int cnt = v->size, e, j = i + 2;

			for (e = 0; e < cnt && j < r; e++) {
				int oend = tok[j].end, q = j + 1;

				while (q + 1 < r && tok[q].start < oend) {
					if (n < max &&
					    jsmn_eq(json, &tok[q], "name"))
						TokWide(json, &tok[q + 1],
						    names[n++], 256);
					q = SkipValue(tok, q + 1, r);
				}
				j = q;
			}
			i = j;
			continue;
		}
		i = SkipValue(tok, i + 1, r);
	}
	HeapFree(GetProcessHeap(), 0, tok);
	return (n);
}

// Ask for the passphrase of every locked encryption root the dataset needs.
// FALSE if the user gave up or a key was refused.
static BOOL
UnlockDataset(SCtx *c, const wchar_t *ds)
{
	op_mount_preflight_req_t rq;
	wchar_t names[8][256], err[256] = L"";
	uint8_t *out = NULL;
	uint32_t outlen = 0;
	int n, i;

	ZeroMemory(&rq, sizeof (rq));
	if (!ToUtf8(ds, rq.dataset, sizeof (rq.dataset)))
		return (FALSE);
	if (!Call(c, OP_MOUNT_PREFLIGHT, &rq, sizeof (rq), &out, &outlen, err,
	    ARRAYSIZE(err))) {
		ErrorBox(c->hWnd, err);
		return (FALSE);
	}
	n = ParseLocked((const char *)out, (int)outlen, names, 8);
	HeapFree(GetProcessHeap(), 0, out);

	for (i = 0; i < n; i++) {
		op_load_key_one_req_t *pkt;
		uint8_t *pass = NULL, *rep = NULL;
		uint32_t passlen = 0, replen = 0;
		size_t sz;
		BOOL ok;

		if (!PromptPassphrase(c->hWnd, names[i], &pass, &passlen))
			return (FALSE);
		sz = sizeof (*pkt) + passlen + 1;
		pkt = (op_load_key_one_req_t *)HeapAlloc(GetProcessHeap(),
		    HEAP_ZERO_MEMORY, sz);
		if (pkt == NULL) {
			SecureZeroMemory(pass, passlen);
			HeapFree(GetProcessHeap(), 0, pass);
			return (FALSE);
		}
		ToUtf8(names[i], pkt->dataset, sizeof (pkt->dataset));
		pkt->passlen = passlen;
		// The passphrase comes with its NUL, as in the tray's mount.
		memcpy((uint8_t *)pkt + sizeof (*pkt), pass, passlen + 1);

		err[0] = L'\0';
		ok = Call(c, OP_LOAD_KEY_ONE, pkt, (uint32_t)sz, &rep, &replen,
		    err, ARRAYSIZE(err));
		if (ok) {
			ok = ReplyOk((const char *)rep, (int)replen, err,
			    ARRAYSIZE(err));
			HeapFree(GetProcessHeap(), 0, rep);
		}
		SecureZeroMemory(pkt, sz);
		HeapFree(GetProcessHeap(), 0, pkt);
		SecureZeroMemory(pass, passlen);
		HeapFree(GetProcessHeap(), 0, pass);
		if (!ok) {
			if (err[0] == L'\0')
				lstrcpynW(err, L"The passphrase was refused.",
				    ARRAYSIZE(err));
			ErrorBox(c->hWnd, err);
			return (FALSE);
		}
	}
	return (TRUE);
}

// Mount or unmount one dataset. 0 done, 1 the service said no (err has its
// reason), 2 the service could not be asked (err says why).
static int
DatasetOp(SCtx *c, uint32_t op, uint32_t flags, const wchar_t *ds,
    wchar_t *err, int errcch)
{
	char name[1024];
	uint8_t body[sizeof (uint32_t) + sizeof (name)];
	uint8_t *out = NULL;
	uint32_t outlen = 0;
	size_t n;

	err[0] = L'\0';
	if (!ToUtf8(ds, name, sizeof (name))) {
		lstrcpynW(err, L"Invalid name.", errcch);
		return (2);
	}
	n = strlen(name) + 1;
	memcpy(body, &flags, sizeof (flags));
	memcpy(body + sizeof (flags), name, n);
	if (!Call(c, op, body, (uint32_t)(sizeof (flags) + n), &out, &outlen,
	    err, errcch))
		return (2);
	if (!ReplyOk((const char *)out, (int)outlen, err, errcch)) {
		if (err[0] == L'\0')
			lstrcpynW(err, L"The service refused.", errcch);
		HeapFree(GetProcessHeap(), 0, out);
		return (1);
	}
	HeapFree(GetProcessHeap(), 0, out);
	return (0);
}

static void
DoMount(SCtx *c)
{
	const DsEnt *d = CurDs(c);
	wchar_t name[256], err[256];

	if (d == NULL || d->volume || d->mounted)
		return;
	lstrcpynW(name, d->name, ARRAYSIZE(name));
	if (d->encrypted && !d->keyloaded && !UnlockDataset(c, name))
		return;
	if (DatasetOp(c, OP_DATASET_MOUNT, 0, name, err, ARRAYSIZE(err)) !=
	    0)
		ErrorBox(c->hWnd, err);
	(void) LoadDatasets(c, name, TRUE);
}

static void
DoUnmount(SCtx *c)
{
	const DsEnt *d = CurDs(c);
	wchar_t name[256], err[256], msg[600];
	int rc;

	if (d == NULL || d->volume || !d->mounted)
		return;
	lstrcpynW(name, d->name, ARRAYSIZE(name));
	rc = DatasetOp(c, OP_DATASET_UNMOUNT, 0, name, err, ARRAYSIZE(err));
	if (rc == 1) {
		// Most likely something has files open on it.
		_snwprintf_s(msg, ARRAYSIZE(msg), _TRUNCATE,
		    L"%s\n\nForce the unmount? Programs using %s will lose "
		    L"their open files.", err, name);
		if (MessageBoxW(c->hWnd, msg, L"Unmount",
		    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES)
			rc = DatasetOp(c, OP_DATASET_UNMOUNT, ZUMNT_FORCE, name,
			    err, ARRAYSIZE(err));
	}
	if (rc != 0)
		ErrorBox(c->hWnd, err);
	(void) LoadDatasets(c, name, TRUE);
}

// -------------------- window --------------------

static void
Layout(SCtx *c)
{
	RECT rc;
	int w, h, bh = 26, pad = 10, top = 40, lw = 270;
	int row1, row2, x0, rw, x, listtop;

	GetClientRect(c->hWnd, &rc);
	w = rc.right;
	h = rc.bottom;
	row2 = h - bh - pad;
	row1 = row2 - bh - pad;
	x0 = pad + lw + pad;
	rw = w - x0 - pad;
	listtop = top + 40;

	// Left: the datasets, with mount and unmount under them.
	MoveWindow(c->hTree, pad, top, lw, row2 - top - pad, TRUE);
	MoveWindow(GetDlgItem(c->hWnd, IDC_SMOUNT), pad, row2, 90, bh, TRUE);
	MoveWindow(GetDlgItem(c->hWnd, IDC_SUNMOUNT), pad + 98, row2, 90, bh,
	    TRUE);

	// Right: what is known about the selected dataset, its snapshots.
	MoveWindow(GetDlgItem(c->hWnd, IDC_SDSINFO), x0, top, rw, 36, TRUE);
	MoveWindow(c->hList, x0, listtop, rw, row1 - listtop - pad, TRUE);

	MoveWindow(GetDlgItem(c->hWnd, IDC_SNEWLBL), x0, row1 + 5, 90, 20,
	    TRUE);
	x = x0 + 94;
	MoveWindow(GetDlgItem(c->hWnd, IDC_SCREATE), w - pad - 100, row1, 100,
	    bh, TRUE);
	MoveWindow(GetDlgItem(c->hWnd, IDC_SRECURSIVE), w - pad - 100 - 8 -
	    150, row1 + 2, 150, 22, TRUE);
	MoveWindow(GetDlgItem(c->hWnd, IDC_SNAME), x, row1,
	    w - pad - 100 - 8 - 150 - 8 - x, bh - 2, TRUE);

	MoveWindow(GetDlgItem(c->hWnd, IDC_SINFO), x0, row2 + 4,
	    rw - 286 - pad, 20, TRUE);
	MoveWindow(GetDlgItem(c->hWnd, IDC_SREFRESH), w - pad - 286, row2, 90,
	    bh, TRUE);
	MoveWindow(GetDlgItem(c->hWnd, IDC_SROLLBACK), w - pad - 188, row2, 90,
	    bh, TRUE);
	MoveWindow(GetDlgItem(c->hWnd, IDC_SDESTROY), w - pad - 90, row2, 90,
	    bh, TRUE);
}

// The dataset line above the snapshots, and the mount buttons.
static void
ShowDatasetInfo(SCtx *c)
{
	const DsEnt *d = NULL;
	wchar_t buf[600], used[64], avail[64];
	int i;

	for (i = 0; i < c->nds; i++)
		if (lstrcmpiW(c->ds[i].name, c->cur) == 0)
			d = &c->ds[i];

	if (d == NULL) {
		SetDlgItemTextW(c->hWnd, IDC_SDSINFO, L"");
	} else {
		FormatBytesW(d->used, used, ARRAYSIZE(used));
		FormatBytesW(d->avail, avail, ARRAYSIZE(avail));
		if (d->volume)
			_snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE,
			    L"%s\nVolume, %s used", d->name, used);
		else if (d->mounted)
			_snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE,
			    L"%s\nMounted at %s, %s used, %s available",
			    d->name, d->mountpoint, used, avail);
		else
			_snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE,
			    L"%s\nNot mounted%s, %s used, %s available",
			    d->name, (d->encrypted && !d->keyloaded) ?
			    L" (locked)" : L"", used, avail);
		SetDlgItemTextW(c->hWnd, IDC_SDSINFO, buf);
	}
	EnableWindow(GetDlgItem(c->hWnd, IDC_SMOUNT),
	    d != NULL && !d->volume && !d->mounted);
	EnableWindow(GetDlgItem(c->hWnd, IDC_SUNMOUNT),
	    d != NULL && !d->volume && d->mounted);
}

// Select the dataset at index i and show its snapshots.
static void
SelectDataset(SCtx *c, int i)
{
	if (i < 0 || i >= c->nds) {
		c->cur[0] = L'\0';
	} else {
		lstrcpynW(c->cur, c->ds[i].name, ARRAYSIZE(c->cur));
	}
	ShowDatasetInfo(c);
	LoadSnapshots(c);
}


static LRESULT CALLBACK
SnapshotsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	SCtx *c = (SCtx *)GetWindowLongPtrW(hWnd, GWLP_USERDATA);

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
		mm->ptMinTrackSize.x = 760;
		mm->ptMinTrackSize.y = 320;
		return (0);
	}
	case WM_NOTIFY: {
		NMHDR *nh = (NMHDR *)lParam;

		if (c != NULL && nh->idFrom == IDC_SLIST &&
		    nh->code == LVN_ITEMCHANGED) {
			UpdateButtons(c);
		} else if (c != NULL && !c->building &&
		    nh->idFrom == IDC_SDATASET &&
		    nh->code == TVN_SELCHANGEDW) {
			NMTREEVIEWW *nt = (NMTREEVIEWW *)lParam;

			SelectDataset(c, (int)nt->itemNew.lParam);
		}
		return (0);
	}
	case WM_COMMAND: {
		if (c == NULL)
			break;
		switch (LOWORD(wParam)) {
		case IDC_SMOUNT:
			DoMount(c);
			break;
		case IDC_SUNMOUNT:
			DoUnmount(c);
			break;
		case IDC_SCREATE:
			DoCreate(c);
			break;
		case IDOK:
			// Enter in the name box takes the snapshot
			if (GetFocus() == GetDlgItem(hWnd, IDC_SNAME))
				DoCreate(c);
			break;
		case IDC_SDESTROY:
			DoDestroy(c);
			break;
		case IDC_SROLLBACK:
			DoRollback(c);
			break;
		case IDC_SREFRESH:
			(void) LoadDatasets(c, c->cur, TRUE);
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

// "name" below its parent, plus what is worth knowing about it.
static void
TreeLabel(const DsEnt *d, wchar_t *out, size_t cch)
{
	const wchar_t *base = wcsrchr(d->name, L'/');

	base = (base != NULL) ? base + 1 : d->name;
	if (d->volume)
		_snwprintf_s(out, cch, _TRUNCATE, L"%s (volume)", base);
	else if (!d->mounted)
		_snwprintf_s(out, cch, _TRUNCATE, L"%s (%s)", base,
		    (d->encrypted && !d->keyloaded) ? L"locked" :
		    L"not mounted");
	else
		_snwprintf_s(out, cch, _TRUNCATE, L"%s", base);
}

// Fetch the dataset list and fill the tree, selecting "want" if it is still
// there. The snapshots are listed again if load_snaps, or if "want" is gone.
// FALSE (with a message shown) if the service could not be used.
static BOOL
LoadDatasets(SCtx *c, const wchar_t *want, BOOL load_snaps)
{
	uint8_t *out = NULL;
	uint32_t outlen = 0;
	wchar_t err[256] = L"";
	wchar_t keep[256];
	HTREEITEM stack[16] = { 0 };
	int i, pick = 0;
	BOOL found = FALSE;

	// "want" may be c->cur, which SelectDataset() overwrites.
	lstrcpynW(keep, want != NULL ? want : L"", ARRAYSIZE(keep));
	want = keep;
	c->building = TRUE;
	TreeView_DeleteAllItems(c->hTree);
	if (c->ds)
		HeapFree(GetProcessHeap(), 0, c->ds);
	c->ds = NULL;
	c->nds = 0;

	if (!Call(c, OP_LIST_DATASETS, NULL, 0, &out, &outlen, err,
	    ARRAYSIZE(err))) {
		c->building = FALSE;
		ErrorBox(NULL, err);
		return (FALSE);
	}
	if (!ParseDatasets(c, (const char *)out, (int)outlen)) {
		ReplyOk((const char *)out, (int)outlen, err, ARRAYSIZE(err));
		if (err[0] == L'\0')
			lstrcpynW(err, L"Unexpected reply from the service.",
			    ARRAYSIZE(err));
		HeapFree(GetProcessHeap(), 0, out);
		c->building = FALSE;
		ErrorBox(NULL, err);
		return (FALSE);
	}
	HeapFree(GetProcessHeap(), 0, out);

	// Parents come before their children, so the last item seen at the
	// depth above is the parent.
	for (i = 0; i < c->nds; i++) {
		DsEnt *d = &c->ds[i];
		TVINSERTSTRUCTW tvi = { 0 };
		wchar_t label[300];
		int depth = (d->depth < 15) ? d->depth : 15;

		TreeLabel(d, label, ARRAYSIZE(label));
		tvi.hParent = (depth > 0 && stack[depth - 1] != NULL) ?
		    stack[depth - 1] : TVI_ROOT;
		tvi.hInsertAfter = TVI_LAST;
		tvi.item.mask = TVIF_TEXT | TVIF_PARAM;
		tvi.item.pszText = label;
		tvi.item.lParam = i;
		d->item = TreeView_InsertItem(c->hTree, &tvi);
		stack[depth] = d->item;
		if (tvi.hParent != TVI_ROOT)
			TreeView_Expand(c->hTree, tvi.hParent, TVE_EXPAND);
		if (want[0] != L'\0' && lstrcmpiW(want, d->name) == 0) {
			pick = i;
			found = TRUE;
		}
	}
	if (c->nds > 0)
		TreeView_SelectItem(c->hTree, c->ds[pick].item);
	c->building = FALSE;

	if (c->nds == 0) {
		c->cur[0] = L'\0';
		ShowDatasetInfo(c);
		SetInfo(c, L"There are no datasets.");
	} else if (load_snaps || !found) {
		SelectDataset(c, pick);
	} else {
		ShowDatasetInfo(c);
	}
	return (TRUE);
}

int
RunSnapshotsWindow(HINSTANCE hInst, const wchar_t *dataset)
{
	SCtx ctx;
	WNDCLASSW wc = { 0 };
	HFONT font;
	MSG m;
	INITCOMMONCONTROLSEX icc = { sizeof (icc),
	    ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES };
	wchar_t name[256];

	ZeroMemory(&ctx, sizeof (ctx));
	InitCommonControlsEx(&icc);
	CoInitialize(NULL);
	zrpc_init(&ctx.rpc, L"\\\\.\\pipe\\openzfs_zed", 60000);

	wc.lpfnWndProc = SnapshotsWndProc;
	wc.hInstance = hInst;
	wc.lpszClassName = L"ZfsSnapshotsWnd";
	wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
	RegisterClassW(&wc);

	ctx.hWnd = CreateWindowExW(0, wc.lpszClassName, L"ZFS datasets",
	    WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 900, 500, NULL,
	    NULL, hInst, &ctx);
	if (ctx.hWnd == NULL)
		return (1);

	font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
	ctx.hHeaderFont = CreateHeaderFont(ctx.hWnd);

	{
		HWND h = MakeChild(ctx.hWnd, L"STATIC", L"Datasets", SS_LEFT,
		    0);
		SendMessageW(h, WM_SETFONT, (WPARAM)ctx.hHeaderFont, TRUE);
		MoveWindow(h, 10, 8, 400, 26, TRUE);
	}
	ctx.hTree = MakeChild(ctx.hWnd, WC_TREEVIEWW, L"",
	    TVS_HASLINES | TVS_HASBUTTONS | TVS_LINESATROOT |
	    TVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, IDC_SDATASET);
	SendMessageW(ctx.hTree, WM_SETFONT, (WPARAM)font, TRUE);
	SetWindowTheme(ctx.hTree, L"Explorer", NULL);
	SendMessageW(MakeChild(ctx.hWnd, L"STATIC", L"", SS_LEFT,
	    IDC_SDSINFO), WM_SETFONT, (WPARAM)font, TRUE);
	SendMessageW(MakeChild(ctx.hWnd, L"BUTTON", L"Mount",
	    BS_PUSHBUTTON | WS_TABSTOP, IDC_SMOUNT), WM_SETFONT,
	    (WPARAM)font, TRUE);
	SendMessageW(MakeChild(ctx.hWnd, L"BUTTON", L"Unmount",
	    BS_PUSHBUTTON | WS_TABSTOP, IDC_SUNMOUNT), WM_SETFONT,
	    (WPARAM)font, TRUE);

	ctx.hList = MakeChild(ctx.hWnd, WC_LISTVIEWW, L"",
	    LVS_REPORT | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP,
	    IDC_SLIST);
	ListView_SetExtendedListViewStyle(ctx.hList,
	    LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
	SendMessageW(ctx.hList, WM_SETFONT, (WPARAM)font, TRUE);
	AddColumns(ctx.hList);

	SendMessageW(MakeChild(ctx.hWnd, L"STATIC", L"New snapshot:", SS_LEFT,
	    IDC_SNEWLBL), WM_SETFONT, (WPARAM)font, TRUE);
	DefaultName(name, ARRAYSIZE(name));
	SendMessageW(MakeChild(ctx.hWnd, L"EDIT", name,
	    ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, IDC_SNAME), WM_SETFONT,
	    (WPARAM)font, TRUE);
	SendMessageW(MakeChild(ctx.hWnd, L"BUTTON", L"Include child datasets",
	    BS_AUTOCHECKBOX | WS_TABSTOP, IDC_SRECURSIVE), WM_SETFONT,
	    (WPARAM)font, TRUE);
	SendMessageW(MakeChild(ctx.hWnd, L"BUTTON", L"Create",
	    BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_SCREATE), WM_SETFONT,
	    (WPARAM)font, TRUE);
	SendMessageW(MakeChild(ctx.hWnd, L"STATIC", L"",
	    SS_LEFT | SS_PATHELLIPSIS, IDC_SINFO), WM_SETFONT, (WPARAM)font,
	    TRUE);
	SendMessageW(MakeChild(ctx.hWnd, L"BUTTON", L"Refresh",
	    BS_PUSHBUTTON | WS_TABSTOP, IDC_SREFRESH), WM_SETFONT,
	    (WPARAM)font, TRUE);
	SendMessageW(MakeChild(ctx.hWnd, L"BUTTON", L"Rollback...",
	    BS_PUSHBUTTON | WS_TABSTOP, IDC_SROLLBACK), WM_SETFONT,
	    (WPARAM)font, TRUE);
	SendMessageW(MakeChild(ctx.hWnd, L"BUTTON", L"Destroy...",
	    BS_PUSHBUTTON | WS_TABSTOP, IDC_SDESTROY), WM_SETFONT,
	    (WPARAM)font, TRUE);

	ApplyThemeFollowSystem(ctx.hWnd);
	Layout(&ctx);
	if (!LoadDatasets(&ctx, dataset, TRUE)) {
		DestroyWindow(ctx.hWnd);
		return (1);
	}
	ShowWindow(ctx.hWnd, SW_SHOWNORMAL);
	SetForegroundWindow(ctx.hWnd);

	while (GetMessageW(&m, NULL, 0, 0) > 0) {
		if (!IsDialogMessageW(ctx.hWnd, &m)) {
			TranslateMessage(&m);
			DispatchMessageW(&m);
		}
	}

	zrpc_close(&ctx.rpc);
	if (ctx.hHeaderFont)
		DeleteObject(ctx.hHeaderFont);
	FreeSnaps(&ctx);
	if (ctx.ds)
		HeapFree(GetProcessHeap(), 0, ctx.ds);
	return (0);
}
