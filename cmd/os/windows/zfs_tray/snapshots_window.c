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
#define	IDC_SNEWDS	3315
#define	IDC_SDELDS	3316
#define	IDC_SCLONE	3317
#define	IDC_SPROMOTE	3318
#define	IDC_SRENDS	3319
#define	IDM_SRENSNAP	3330
#define	IDM_SDIFF	3331
#define	IDC_STAB	3320
#define	IDC_PLIST	3321
#define	IDC_PEDIT	3322
#define	IDC_PINHERIT	3323
#define	IDC_PINFO	3324
#define	ZSETPROP_INHERIT	0x01

// Mirror of the service's flags, see ops_snapshots.h
#define	ZSNAP_RECURSIVE	0x01
#define	ZROLL_DESTROY_NEWER	0x01
#define	ZROLL_DESTROY_CLONES	0x02
#define	ZUMNT_FORCE	0x01
#define	ZDSD_RECURSIVE	0x01
#define	ZREN_RECURSIVE	0x01

typedef struct {
	wchar_t name[256];
	wchar_t mountpoint[260];
	wchar_t origin[300];
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
	wchar_t name[64];
	wchar_t value[512];
	wchar_t source[24];	// local, inherited, received, ...
	wchar_t from[256];	// where an inherited value comes from
	wchar_t hint[512];	// ZFS's own list of the values it takes
	BOOL readonly;
	BOOL inheritable;
} PropEnt;

typedef struct {
	HWND hWnd;
	HWND hList;
	HWND hTab;
	HWND hPropList;
	HWND hTree;
	HFONT hHeaderFont;
	zrpc_t rpc;
	DsEnt *ds;
	int nds;
	SnapEnt *snaps;
	int nsnaps;
	PropEnt *props;
	int nprops;
	int page;		// 0 snapshots, 1 properties
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
	} else if (jsmn_eq(json, k, "origin")) {
		TokWide(json, v, d->origin, ARRAYSIZE(d->origin));
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

// "\\" back to "\" in the values the service sent, in place.
static void
UnescapeW(wchar_t *s)
{
	wchar_t *w = s;

	for (; *s; s++) {
		*w++ = *s;
		if (s[0] == L'\\' && s[1] == L'\\')
			s++;
	}
	*w = L'\0';
}

// One "key":value pair of a property object.
static void
PropField(const char *json, const jsmntok_t *k, const jsmntok_t *v,
    PropEnt *e)
{
	if (jsmn_eq(json, k, "name")) {
		TokWide(json, v, e->name, ARRAYSIZE(e->name));
	} else if (jsmn_eq(json, k, "value")) {
		TokWide(json, v, e->value, ARRAYSIZE(e->value));
		UnescapeW(e->value);
	} else if (jsmn_eq(json, k, "source")) {
		TokWide(json, v, e->source, ARRAYSIZE(e->source));
	} else if (jsmn_eq(json, k, "from")) {
		TokWide(json, v, e->from, ARRAYSIZE(e->from));
	} else if (jsmn_eq(json, k, "hint")) {
		TokWide(json, v, e->hint, ARRAYSIZE(e->hint));
	} else if (jsmn_eq(json, k, "readonly")) {
		e->readonly = PrimTrue(json, v);
	} else if (jsmn_eq(json, k, "inheritable")) {
		e->inheritable = PrimTrue(json, v);
	}
}

static BOOL
ParseProps(SCtx *c, const char *json, int json_len)
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
		} else if (jsmn_eq(json, k, "props") &&
		    v->type == JSMN_ARRAY) {
			int n = v->size, e, j = i + 2;

			c->props = (PropEnt *)HeapAlloc(GetProcessHeap(),
			    HEAP_ZERO_MEMORY,
			    (n > 0 ? n : 1) * sizeof (PropEnt));
			if (!c->props)
				break;
			for (e = 0; e < n && j < r; e++) {
				int oend = tok[j].end, q = j + 1;

				while (q + 1 < r && tok[q].start < oend) {
					PropField(json, &tok[q], &tok[q + 1],
					    &c->props[c->nprops]);
					q = SkipValue(tok, q + 1, r);
				}
				c->nprops++;
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
	EnableWindow(GetDlgItem(c->hWnd, IDC_SCLONE),
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

// -------------------- create / destroy datasets --------------------

static HWND MakeChild(HWND parent, const wchar_t *cls, const wchar_t *text,
    DWORD style, int id);

// A property with a short list of values to pick from. The first choice is
// "leave it to be inherited"; the others are the value itself.
typedef struct {
	const wchar_t *label;
	const wchar_t *key;
	const wchar_t *const *choices;	// NULL terminated
	const wchar_t *const *values;	// what is sent, NULL: the choice
	HWND h;
} PromptCombo;

typedef struct {
	const wchar_t *title;
	const wchar_t *prompt;
	const wchar_t *hint;	// NULL: just the one box, no properties
	wchar_t *text;		// in: first contents, out: what was entered
	int textcch;
	wchar_t *props;		// out: the picks, then what was typed
	int propscch;
	PromptCombo *combos;
	int ncombos;
	HWND hText;
	HWND hProps;
	BOOL ok;
	BOOL done;
} PromptCtx;

static LRESULT CALLBACK
PromptWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	PromptCtx *p = (PromptCtx *)GetWindowLongPtrW(hWnd, GWLP_USERDATA);

	switch (msg) {
	case WM_NCCREATE: {
		CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
		SetWindowLongPtrW(hWnd, GWLP_USERDATA,
		    (LONG_PTR)cs->lpCreateParams);
		break;
	}
	case WM_COMMAND:
		if (p == NULL)
			break;
		if (LOWORD(wParam) == IDOK) {
			GetWindowTextW(p->hText, p->text, p->textcch);
			if (p->hProps != NULL) {
				wchar_t typed[1024], line[160];
				int i;

				GetWindowTextW(p->hProps, typed,
				    ARRAYSIZE(typed));
				p->props[0] = L'\0';
				for (i = 0; i < p->ncombos; i++) {
					int sel = (int)SendMessageW(
					    p->combos[i].h, CB_GETCURSEL, 0,
					    0);

					const PromptCombo *pc = &p->combos[i];

					if (sel <= 0)
						continue;
					_snwprintf_s(line, ARRAYSIZE(line),
					    _TRUNCATE, L"%s=%s\n", pc->key,
					    pc->values != NULL ?
					    pc->values[sel] :
					    pc->choices[sel]);
					wcsncat_s(p->props, p->propscch, line,
					    _TRUNCATE);
				}
				wcsncat_s(p->props, p->propscch, typed,
				    _TRUNCATE);
			}
			p->ok = TRUE;
			DestroyWindow(hWnd);
		} else if (LOWORD(wParam) == IDCANCEL) {
			DestroyWindow(hWnd);
		}
		return (0);
	case WM_CLOSE:
		DestroyWindow(hWnd);
		return (0);
	case WM_DESTROY:
		// Not PostQuitMessage(): the window under it keeps running.
		if (p != NULL)
			p->done = TRUE;
		return (0);
	}
	return (DefWindowProcW(hWnd, msg, wParam, lParam));
}

// A small modal box: one line of text, and optionally a properties box.
// FALSE if cancelled.
static BOOL
PromptText(HWND owner, const wchar_t *title, const wchar_t *prompt,
    const wchar_t *hint, wchar_t *text, int textcch, wchar_t *props,
    int propscch, PromptCombo *combos, int ncombos,
    const wchar_t *const *text_choices)
{
	static BOOL registered = FALSE;
	HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE);
	HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
	PromptCtx p;
	RECT rc, orc;
	MSG m;
	HWND dlg, h;
	int w = 460, hgt, y = 12, bw = 90, bh = 26, i;
	DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;

	if (!registered) {
		WNDCLASSW wc = { 0 };

		wc.lpfnWndProc = PromptWndProc;
		wc.hInstance = hInst;
		wc.lpszClassName = L"ZfsPromptWnd";
		wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
		wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
		RegisterClassW(&wc);
		registered = TRUE;
	}

	ZeroMemory(&p, sizeof (p));
	p.title = title;
	p.prompt = prompt;
	p.hint = hint;
	p.text = text;
	p.textcch = textcch;
	p.props = props;
	p.propscch = propscch;
	p.combos = combos;
	p.ncombos = ncombos;

	hgt = 12 + 36 + 8 + 24 + 12 + bh + 12;
	if (ncombos > 0)
		hgt += ((ncombos + 1) / 2) * 30 + 6;
	if (hint != NULL)
		hgt += 24 + 96 + 4;
	rc.left = 0;
	rc.top = 0;
	rc.right = w;
	rc.bottom = hgt;
	AdjustWindowRect(&rc, style, FALSE);
	GetWindowRect(owner, &orc);
	dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ZfsPromptWnd", title,
	    style, orc.left + ((orc.right - orc.left) - (rc.right - rc.left)) /
	    2, orc.top + 60, rc.right - rc.left, rc.bottom - rc.top, owner,
	    NULL, hInst, &p);
	if (dlg == NULL)
		return (FALSE);

	h = MakeChild(dlg, L"STATIC", prompt, SS_LEFT, 0);
	SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
	MoveWindow(h, 12, y, w - 24, 36, TRUE);
	y += 36 + 8;
	if (text_choices != NULL) {
		// Something to pick from, or type over.
		int k;

		p.hText = MakeChild(dlg, L"COMBOBOX", L"", CBS_DROPDOWN |
		    CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, 0);
		for (k = 0; text_choices[k] != NULL; k++)
			SendMessageW(p.hText, CB_ADDSTRING, 0,
			    (LPARAM)text_choices[k]);
		SetWindowTextW(p.hText, text);
		SendMessageW(p.hText, WM_SETFONT, (WPARAM)font, TRUE);
		MoveWindow(p.hText, 12, y, w - 24, 200, TRUE);
	} else {
		p.hText = MakeChild(dlg, L"EDIT", text, ES_AUTOHSCROLL |
		    WS_BORDER | WS_TABSTOP, 0);
		SendMessageW(p.hText, WM_SETFONT, (WPARAM)font, TRUE);
		MoveWindow(p.hText, 12, y, w - 24, 24, TRUE);
	}
	y += 24 + 12;
	for (i = 0; i < ncombos; i++) {
		// Two to a row: label and drop-down, label and drop-down.
		int x = 12 + (i % 2) * 228, k;

		h = MakeChild(dlg, L"STATIC", combos[i].label, SS_LEFT, 0);
		SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
		MoveWindow(h, x, y + 4, 96, 20, TRUE);
		combos[i].h = MakeChild(dlg, L"COMBOBOX", L"",
		    CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0);
		SendMessageW(combos[i].h, WM_SETFONT, (WPARAM)font, TRUE);
		MoveWindow(combos[i].h, x + 98, y, 122, 200, TRUE);
		for (k = 0; combos[i].choices[k] != NULL; k++)
			SendMessageW(combos[i].h, CB_ADDSTRING, 0,
			    (LPARAM)combos[i].choices[k]);
		SendMessageW(combos[i].h, CB_SETCURSEL, 0, 0);
		if (i % 2 == 1 || i == ncombos - 1)
			y += 30;
	}
	if (ncombos > 0)
		y += 6;
	if (hint != NULL) {
		h = MakeChild(dlg, L"STATIC", hint, SS_LEFT, 0);
		SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
		MoveWindow(h, 12, y, w - 24, 24, TRUE);
		y += 24 + 4;
		p.hProps = MakeChild(dlg, L"EDIT", props, ES_MULTILINE |
		    ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_BORDER |
		    WS_TABSTOP, 0);
		SendMessageW(p.hProps, WM_SETFONT, (WPARAM)font, TRUE);
		MoveWindow(p.hProps, 12, y, w - 24, 96, TRUE);
		y += 96 + 12;
	}
	h = MakeChild(dlg, L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP,
	    IDOK);
	SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
	MoveWindow(h, w - 12 - 2 * bw - 8, y, bw, bh, TRUE);
	h = MakeChild(dlg, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP,
	    IDCANCEL);
	SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
	MoveWindow(h, w - 12 - bw, y, bw, bh, TRUE);

	ApplyThemeFollowSystem(dlg);
	EnableWindow(owner, FALSE);
	ShowWindow(dlg, SW_SHOWNORMAL);
	SetFocus(p.hText);
	if (text_choices != NULL)
		SendMessageW(p.hText, CB_SETEDITSEL, 0, MAKELPARAM(0, -1));
	else
		SendMessageW(p.hText, EM_SETSEL, 0, -1);
	while (!p.done && GetMessageW(&m, NULL, 0, 0) > 0) {
		if (!IsDialogMessageW(dlg, &m)) {
			TranslateMessage(&m);
			DispatchMessageW(&m);
		}
	}
	EnableWindow(owner, TRUE);
	SetForegroundWindow(owner);
	return (p.ok);
}

// The value of the last "key=value" line for key in the properties text, or
// "" if there is none.
static void
PropValue(const wchar_t *props, const wchar_t *key, wchar_t *out, int cch)
{
	size_t klen = wcslen(key);
	const wchar_t *line = props;

	out[0] = L'\0';
	while (*line != L'\0') {
		const wchar_t *end = line + wcscspn(line, L"\r\n");

		if ((size_t)(end - line) > klen &&
		    _wcsnicmp(line, key, klen) == 0 && line[klen] == L'=') {
			int n = (int)(end - line - klen - 1);

			if (n >= cch)
				n = cch - 1;
			memcpy(out, line + klen + 1, n * sizeof (wchar_t));
			out[n] = L'\0';
		}
		line = end;
		while (*line == L'\r' || *line == L'\n')
			line++;
	}
}

// Ask for a new passphrase twice (a typo here loses the data for good), as
// UTF-8 in out. FALSE if cancelled, they differ, or it is too short.
static BOOL
AskNewPassphrase(SCtx *c, const wchar_t *ds, char *out, size_t outsz)
{
	wchar_t again[700];
	uint8_t *p1 = NULL, *p2 = NULL;
	uint32_t l1 = 0, l2 = 0;
	BOOL same;

	if (!PromptPassphrase(c->hWnd, ds, &p1, &l1))
		return (FALSE);
	_snwprintf_s(again, ARRAYSIZE(again), _TRUNCATE, L"%s (again)", ds);
	if (!PromptPassphrase(c->hWnd, again, &p2, &l2)) {
		SecureZeroMemory(p1, l1);
		HeapFree(GetProcessHeap(), 0, p1);
		return (FALSE);
	}
	same = (l1 == l2 && memcmp(p1, p2, l1) == 0);
	SecureZeroMemory(p2, l2);
	HeapFree(GetProcessHeap(), 0, p2);
	if (!same) {
		ErrorBox(c->hWnd, L"The two passphrases are not the same.");
	} else if (l1 < 8) {
		// ZFS refuses anything shorter
		ErrorBox(c->hWnd, L"A passphrase needs at least 8 "
		    L"characters.");
		same = FALSE;
	} else if (l1 + 1 > outsz) {
		ErrorBox(c->hWnd, L"The passphrase is too long.");
		same = FALSE;
	} else {
		memcpy(out, p1, l1 + 1);	// with its NUL
	}
	SecureZeroMemory(p1, l1);
	HeapFree(GetProcessHeap(), 0, p1);
	return (same);
}

// The drive letters nobody is using, for the driveletter picker: the text
// shown ("G:") and the value to send ("G"), after the fixed choices.
typedef struct {
	wchar_t shown[26][8];
	wchar_t value[26][4];
	const wchar_t *choices[32];
	const wchar_t *values[32];
} LetterList;

static void
FillLetters(LetterList *ll)
{
	DWORD used = GetLogicalDrives();
	int n = 0, i;

	ZeroMemory(ll, sizeof (*ll));
	ll->choices[n] = L"(inherit)";
	ll->values[n++] = L"";
	ll->choices[n] = L"off";
	ll->values[n++] = L"off";
	ll->choices[n] = L"on (next free)";
	ll->values[n++] = L"on";
	// Not A: and B:, and not the ones already in use.
	for (i = 'D' - 'A'; i < 26; i++) {
		if (used & (1UL << i))
			continue;
		_snwprintf_s(ll->shown[i], ARRAYSIZE(ll->shown[i]), _TRUNCATE,
		    L"%c:", (wchar_t)(L'A' + i));
		_snwprintf_s(ll->value[i], ARRAYSIZE(ll->value[i]), _TRUNCATE,
		    L"%c", (wchar_t)(L'A' + i));
		ll->choices[n] = ll->shown[i];
		ll->values[n++] = ll->value[i];
	}
	ll->choices[n] = NULL;
	ll->values[n] = NULL;
}

static void
DoNewDataset(SCtx *c)
{
	static const wchar_t *const compress[] = { L"(inherit)", L"off",
	    L"lz4", L"zstd", L"gzip", NULL };
	// These two can only be chosen now, never changed later.
	static const wchar_t *const casesens[] = { L"(inherit)", L"sensitive",
	    L"insensitive", L"mixed", NULL };
	static const wchar_t *const normal[] = { L"(inherit)", L"none",
	    L"formC", L"formD", L"formKC", L"formKD", NULL };
	static const wchar_t *const recsize[] = { L"(inherit)", L"16K", L"64K",
	    L"128K", L"1M", NULL };
	static const wchar_t *const atime[] = { L"(inherit)", L"on", L"off",
	    NULL };
	// Anything but inherit and off needs a passphrase, "on" is the
	// default cipher.
	static const wchar_t *const encrypt[] = { L"(inherit)", L"off", L"on",
	    L"aes-256-ccm", L"aes-256-gcm", NULL };
	LetterList letters;
	PromptCombo combos[] = {
		{ L"Compression:", L"compression", compress, NULL, NULL },
		{ L"Record size:", L"recordsize", recsize, NULL, NULL },
		{ L"Case sensitivity:", L"casesensitivity", casesens, NULL,
		    NULL },
		{ L"Normalization:", L"normalization", normal, NULL, NULL },
		{ L"Access time:", L"atime", atime, NULL, NULL },
		{ L"Drive letter:", L"driveletter", NULL, NULL, NULL },
		{ L"Encryption:", L"encryption", encrypt, NULL, NULL },
	};
	wchar_t cipher[64];
	char pass[1100] = "";
	const DsEnt *d = CurDs(c);
	wchar_t parent[256], name[256] = L"", props[1024] = L"";
	wchar_t full[600], prompt[400], err[256];
	char body[1024 + 2048 + 1100];
	size_t n1, n2, n3;
	uint8_t *out = NULL;
	uint32_t outlen = 0;
	const wchar_t *s;
	BOOL ok;

	if (d == NULL || d->volume)
		return;
	lstrcpynW(parent, d->name, ARRAYSIZE(parent));
	FillLetters(&letters);
	combos[5].choices = letters.choices;
	combos[5].values = letters.values;
	_snwprintf_s(prompt, ARRAYSIZE(prompt), _TRUNCATE,
	    L"New dataset under %s. Name:", parent);
	if (!PromptText(c->hWnd, L"New dataset", prompt,
	    L"Anything else, one per line, like recordsize=1M:",
	    name, ARRAYSIZE(name), props, ARRAYSIZE(props), combos,
	    (int)ARRAYSIZE(combos), NULL))
		return;
	if (name[0] == L'\0') {
		ErrorBox(c->hWnd, L"Enter a name for the dataset.");
		return;
	}
	for (s = name; *s; s++) {
		if (*s == L'@' || *s == L'/' || *s == L'\\' || *s == L' ') {
			ErrorBox(c->hWnd, L"The name cannot contain spaces, "
			    L"slashes or '@'.");
			return;
		}
	}
	_snwprintf_s(full, ARRAYSIZE(full), _TRUNCATE, L"%s/%s", parent, name);
	if (!ToUtf8(full, body, 1024)) {
		ErrorBox(c->hWnd, L"The name is too long.");
		return;
	}
	n1 = strlen(body) + 1;
	if (!ToUtf8(props, body + n1, (int)(sizeof (body) - n1))) {
		ErrorBox(c->hWnd, L"The properties are too long.");
		return;
	}
	n2 = strlen(body + n1) + 1;
	// A new encryption root needs a passphrase of its own; "off", and
	// inheriting an encrypted parent's key, do not.
	PropValue(props, L"encryption", cipher, ARRAYSIZE(cipher));
	if (cipher[0] != L'\0' && lstrcmpiW(cipher, L"off") != 0 &&
	    !AskNewPassphrase(c, full, pass, sizeof (pass)))
		return;
	n3 = strlen(pass) + 1;
	if (n1 + n2 + n3 > sizeof (body)) {
		SecureZeroMemory(pass, sizeof (pass));
		ErrorBox(c->hWnd, L"Too much to send.");
		return;
	}
	memcpy(body + n1 + n2, pass, n3);
	SecureZeroMemory(pass, sizeof (pass));

	ok = Call(c, OP_DATASET_CREATE, body, (uint32_t)(n1 + n2 + n3), &out,
	    &outlen, err, ARRAYSIZE(err));
	SecureZeroMemory(body, sizeof (body));
	if (!ok) {
		ErrorBox(c->hWnd, err);
		return;
	}
	if (!ReplyOk((const char *)out, (int)outlen, err, ARRAYSIZE(err))) {
		if (err[0] == L'\0')
			lstrcpynW(err, L"The dataset was not created.",
			    ARRAYSIZE(err));
		HeapFree(GetProcessHeap(), 0, out);
		ErrorBox(c->hWnd, err);
		return;
	}
	HeapFree(GetProcessHeap(), 0, out);
	(void) LoadDatasets(c, full, TRUE);
}

// One OP_DATASET_DESTROY. 0 done, 1 the service said no (err says why, and
// *needs_recursive tells if that was all that was wrong), 2 no reply.
static int
DestroyCall(SCtx *c, uint32_t flags, const wchar_t *ds, wchar_t *err,
    int errcch, BOOL *needs_recursive)
{
	char name[1024];
	uint8_t body[sizeof (uint32_t) + sizeof (name)];
	uint8_t *out = NULL;
	uint32_t outlen = 0;
	size_t n;

	*needs_recursive = FALSE;
	err[0] = L'\0';
	if (!ToUtf8(ds, name, sizeof (name))) {
		lstrcpynW(err, L"Invalid name.", errcch);
		return (2);
	}
	n = strlen(name) + 1;
	memcpy(body, &flags, sizeof (flags));
	memcpy(body + sizeof (flags), name, n);
	if (!Call(c, OP_DATASET_DESTROY, body, (uint32_t)(sizeof (flags) + n),
	    &out, &outlen, err, errcch))
		return (2);
	if (!ReplyOk((const char *)out, (int)outlen, err, errcch)) {
		*needs_recursive = ReplyBool((const char *)out, (int)outlen,
		    "needs_recursive");
		if (err[0] == L'\0')
			lstrcpynW(err, L"The dataset was not destroyed.",
			    errcch);
		HeapFree(GetProcessHeap(), 0, out);
		return (1);
	}
	HeapFree(GetProcessHeap(), 0, out);
	return (0);
}

static void
DoDestroyDataset(SCtx *c)
{
	const DsEnt *d = CurDs(c);
	wchar_t name[256], typed[256] = L"", prompt[500], err[256], msg[600];
	wchar_t parent[256];
	wchar_t *slash;
	BOOL needs_recursive;
	int rc;

	// A pool is destroyed from the tray menu, not from here.
	if (d == NULL || d->depth == 0)
		return;
	lstrcpynW(name, d->name, ARRAYSIZE(name));
	_snwprintf_s(prompt, ARRAYSIZE(prompt), _TRUNCATE,
	    L"This destroys %s and everything stored in it. Type its name "
	    L"to confirm:", name);
	if (!PromptText(c->hWnd, L"Destroy dataset", prompt, NULL, typed,
	    ARRAYSIZE(typed), NULL, 0, NULL, 0, NULL))
		return;
	if (lstrcmpW(typed, name) != 0) {
		ErrorBox(c->hWnd, L"That is not the name of the dataset, "
		    L"nothing was destroyed.");
		return;
	}

	rc = DestroyCall(c, 0, name, err, ARRAYSIZE(err), &needs_recursive);
	if (rc == 1 && needs_recursive) {
		_snwprintf_s(msg, ARRAYSIZE(msg), _TRUNCATE,
		    L"%s: %s.\n\nDestroy all of them too? This cannot be "
		    L"undone.", name, err);
		if (MessageBoxW(c->hWnd, msg, L"Destroy dataset",
		    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
			return;
		rc = DestroyCall(c, ZDSD_RECURSIVE, name, err,
		    ARRAYSIZE(err), &needs_recursive);
	}
	if (rc != 0)
		ErrorBox(c->hWnd, err);

	// Show its parent, or where we were if it is still there.
	lstrcpynW(parent, name, ARRAYSIZE(parent));
	slash = wcsrchr(parent, L'/');
	if (rc == 0 && slash != NULL)
		*slash = L'\0';
	(void) LoadDatasets(c, rc == 0 ? parent : name, TRUE);
}

// -------------------- clone / promote --------------------

// Clone the selected snapshot as a new dataset, anywhere in the pool.
static void
DoClone(SCtx *c)
{
	int sel = ListView_GetNextItem(c->hList, -1, LVNI_SELECTED);
	LVITEMW it = { 0 };
	const SnapEnt *s;
	wchar_t target[256], prompt[512], err[256] = L"";
	char body[sizeof (uint32_t) + 3 * 1024];
	char ds[1024], snap[512], tgt[1024];
	uint8_t *out = NULL;
	uint32_t outlen = 0, flags = 0;
	size_t n1, n2, n3, off;
	const wchar_t *p;
	int slashes = 0;

	if (sel < 0)
		return;
	it.mask = LVIF_PARAM;
	it.iItem = sel;
	if (!ListView_GetItem(c->hList, &it) || it.lParam < 0 ||
	    it.lParam >= c->nsnaps)
		return;
	s = &c->snaps[it.lParam];

	_snwprintf_s(target, ARRAYSIZE(target), _TRUNCATE, L"%s-clone", c->cur);
	_snwprintf_s(prompt, ARRAYSIZE(prompt), _TRUNCATE,
	    L"Clone %s@%s as a new dataset. Its full name, in the same "
	    L"pool:", c->cur, s->name);
	if (!PromptText(c->hWnd, L"Clone snapshot", prompt, NULL, target,
	    ARRAYSIZE(target), NULL, 0, NULL, 0, NULL))
		return;
	for (p = target; *p; p++) {
		if (*p == L'/')
			slashes++;
		if (*p == L'@' || *p == L'\\' || *p == L' ') {
			ErrorBox(c->hWnd, L"The name cannot contain spaces, "
			    L"backslashes or '@'.");
			return;
		}
	}
	if (slashes == 0 || target[0] == L'/') {
		ErrorBox(c->hWnd, L"Give the full name, like pool/name.");
		return;
	}

	if (!ToUtf8(c->cur, ds, sizeof (ds)) ||
	    !ToUtf8(s->name, snap, sizeof (snap)) ||
	    !ToUtf8(target, tgt, sizeof (tgt))) {
		ErrorBox(c->hWnd, L"Invalid name.");
		return;
	}
	n1 = strlen(ds) + 1;
	n2 = strlen(snap) + 1;
	n3 = strlen(tgt) + 1;
	memcpy(body, &flags, sizeof (flags));
	off = sizeof (flags);
	memcpy(body + off, ds, n1);
	off += n1;
	memcpy(body + off, snap, n2);
	off += n2;
	memcpy(body + off, tgt, n3);
	off += n3;

	if (!Call(c, OP_SNAPSHOT_CLONE, body, (uint32_t)off, &out, &outlen,
	    err, ARRAYSIZE(err))) {
		ErrorBox(c->hWnd, err);
		return;
	}
	if (!ReplyOk((const char *)out, (int)outlen, err, ARRAYSIZE(err))) {
		if (err[0] == L'\0')
			lstrcpynW(err, L"The clone was not created.",
			    ARRAYSIZE(err));
		HeapFree(GetProcessHeap(), 0, out);
		ErrorBox(c->hWnd, err);
		return;
	}
	HeapFree(GetProcessHeap(), 0, out);
	(void) LoadDatasets(c, target, TRUE);
}

// Make the selected clone the original: it takes over the snapshots, and the
// dataset it was cloned from becomes the clone.
static void
DoPromote(SCtx *c)
{
	const DsEnt *d = CurDs(c);
	wchar_t msg[1024], err[256] = L"";
	char name[1024];
	uint8_t *out = NULL;
	uint32_t outlen = 0;

	if (d == NULL || d->origin[0] == L'\0')
		return;
	_snwprintf_s(msg, ARRAYSIZE(msg), _TRUNCATE,
	    L"Promote %s?\n\nIt was cloned from %s. After this, %s owns "
	    L"the snapshots up to that one, and the dataset it was cloned "
	    L"from becomes a clone of it. Nothing is lost, but the "
	    L"original cannot be destroyed without %s.",
	    d->name, d->origin, d->name, d->name);
	if (MessageBoxW(c->hWnd, msg, L"Promote", MB_YESNO |
	    MB_ICONQUESTION) != IDYES)
		return;
	if (!ToUtf8(d->name, name, sizeof (name))) {
		ErrorBox(c->hWnd, L"Invalid name.");
		return;
	}
	if (!Call(c, OP_DATASET_PROMOTE, name, (uint32_t)strlen(name) + 1,
	    &out, &outlen, err, ARRAYSIZE(err))) {
		ErrorBox(c->hWnd, err);
		return;
	}
	if (!ReplyOk((const char *)out, (int)outlen, err, ARRAYSIZE(err))) {
		if (err[0] == L'\0')
			lstrcpynW(err, L"The promote failed.", ARRAYSIZE(err));
		HeapFree(GetProcessHeap(), 0, out);
		ErrorBox(c->hWnd, err);
		return;
	}
	HeapFree(GetProcessHeap(), 0, out);
	(void) LoadDatasets(c, c->cur, TRUE);
}

// -------------------- rename --------------------

// One OP_DATASET_RENAME, of a dataset or of a snapshot ("ds@name").
static BOOL
RenameCall(SCtx *c, uint32_t flags, const wchar_t *from, const wchar_t *to,
    wchar_t *err, int errcch)
{
	char a[1024], b[1024];
	char body[sizeof (uint32_t) + sizeof (a) + sizeof (b)];
	uint8_t *out = NULL;
	uint32_t outlen = 0;
	size_t n1, n2;

	err[0] = L'\0';
	if (!ToUtf8(from, a, sizeof (a)) || !ToUtf8(to, b, sizeof (b))) {
		lstrcpynW(err, L"Invalid name.", errcch);
		return (FALSE);
	}
	n1 = strlen(a) + 1;
	n2 = strlen(b) + 1;
	memcpy(body, &flags, sizeof (flags));
	memcpy(body + sizeof (flags), a, n1);
	memcpy(body + sizeof (flags) + n1, b, n2);
	if (!Call(c, OP_DATASET_RENAME, body,
	    (uint32_t)(sizeof (flags) + n1 + n2), &out, &outlen, err, errcch))
		return (FALSE);
	if (!ReplyOk((const char *)out, (int)outlen, err, errcch)) {
		if (err[0] == L'\0')
			lstrcpynW(err, L"The rename failed.", errcch);
		HeapFree(GetProcessHeap(), 0, out);
		return (FALSE);
	}
	HeapFree(GetProcessHeap(), 0, out);
	return (TRUE);
}

// The selected dataset gets a new name, which may also move it to another
// parent in the same pool. Its children go with it.
static void
DoRenameDataset(SCtx *c)
{
	const DsEnt *d = CurDs(c);
	wchar_t name[256], prompt[512], err[256];
	const wchar_t *p;
	int slashes = 0;

	if (d == NULL || d->depth == 0)
		return;
	lstrcpynW(name, d->name, ARRAYSIZE(name));
	_snwprintf_s(prompt, ARRAYSIZE(prompt), _TRUNCATE,
	    L"New name for %s. A full name, so it can also be moved under "
	    L"another dataset in the pool:", d->name);
	if (!PromptText(c->hWnd, L"Rename dataset", prompt, NULL, name,
	    ARRAYSIZE(name), NULL, 0, NULL, 0, NULL))
		return;
	if (lstrcmpW(name, d->name) == 0)
		return;
	for (p = name; *p; p++) {
		if (*p == L'/')
			slashes++;
		if (*p == L'@' || *p == L'\\' || *p == L' ') {
			ErrorBox(c->hWnd, L"The name cannot contain spaces, "
			    L"backslashes or '@'.");
			return;
		}
	}
	if (slashes == 0 || name[0] == L'/') {
		ErrorBox(c->hWnd, L"Give the full name, like pool/name.");
		return;
	}
	if (!RenameCall(c, 0, d->name, name, err, ARRAYSIZE(err))) {
		ErrorBox(c->hWnd, err);
		(void) LoadDatasets(c, c->cur, TRUE);
		return;
	}
	(void) LoadDatasets(c, name, TRUE);
}

// The selected snapshot gets a new name, in the same dataset.
static void
DoRenameSnapshot(SCtx *c)
{
	int sel = ListView_GetNextItem(c->hList, -1, LVNI_SELECTED);
	LVITEMW it = { 0 };
	const SnapEnt *s;
	wchar_t name[256], prompt[512], from[600], to[600], err[256];
	size_t len = lstrlenW(c->cur);
	uint32_t flags = 0;
	BOOL children = FALSE;
	const wchar_t *p;
	int i;

	if (sel < 0)
		return;
	it.mask = LVIF_PARAM;
	it.iItem = sel;
	if (!ListView_GetItem(c->hList, &it) || it.lParam < 0 ||
	    it.lParam >= c->nsnaps)
		return;
	s = &c->snaps[it.lParam];

	lstrcpynW(name, s->name, ARRAYSIZE(name));
	_snwprintf_s(prompt, ARRAYSIZE(prompt), _TRUNCATE,
	    L"New name for the snapshot %s@%s:", c->cur, s->name);
	if (!PromptText(c->hWnd, L"Rename snapshot", prompt, NULL, name,
	    ARRAYSIZE(name), NULL, 0, NULL, 0, NULL))
		return;
	if (lstrcmpW(name, s->name) == 0)
		return;
	for (p = name; *p; p++) {
		if (*p == L'@' || *p == L'/' || *p == L'\\' || *p == L' ') {
			ErrorBox(c->hWnd, L"The name cannot contain spaces, "
			    L"slashes or '@'.");
			return;
		}
	}
	if (name[0] == L'\0')
		return;

	// A recursive snapshot left one by this name in every child dataset.
	for (i = 0; i < c->nds; i++)
		if (wcsncmp(c->ds[i].name, c->cur, len) == 0 &&
		    c->ds[i].name[len] == L'/')
			children = TRUE;
	if (children && MessageBoxW(c->hWnd,
	    L"Rename the snapshots of the same name in the child datasets "
	    L"too?", L"Rename snapshot", MB_YESNO | MB_ICONQUESTION) == IDYES)
		flags |= ZREN_RECURSIVE;

	_snwprintf_s(from, ARRAYSIZE(from), _TRUNCATE, L"%s@%s", c->cur,
	    s->name);
	_snwprintf_s(to, ARRAYSIZE(to), _TRUNCATE, L"%s@%s", c->cur, name);
	if (!RenameCall(c, flags, from, to, err, ARRAYSIZE(err)))
		ErrorBox(c->hWnd, err);
	LoadSnapshots(c);
}

// Right-click on a snapshot: what can be done to it.
static void
SnapMenu(SCtx *c)
{
	HMENU m = CreatePopupMenu();
	UINT n = ListView_GetSelectedCount(c->hList);
	UINT one = n == 1 ? MF_ENABLED : MF_GRAYED;
	POINT pt;
	int cmd;

	if (m == NULL)
		return;
	GetCursorPos(&pt);
	AppendMenuW(m, MF_STRING | (n == 1 || n == 2 ? MF_ENABLED :
	    MF_GRAYED), IDM_SDIFF, n == 2 ? L"Compare these two..." :
	    L"Changes since this one...");
	AppendMenuW(m, MF_SEPARATOR, 0, NULL);
	AppendMenuW(m, MF_STRING | one, IDC_SCLONE, L"Clone...");
	AppendMenuW(m, MF_STRING | one, IDM_SRENSNAP, L"Rename...\tF2");
	AppendMenuW(m, MF_STRING | one, IDC_SROLLBACK, L"Rollback...");
	AppendMenuW(m, MF_STRING | (n > 0 ? MF_ENABLED : MF_GRAYED),
	    IDC_SDESTROY, L"Destroy...");
	cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0,
	    c->hWnd, NULL);
	DestroyMenu(m);
	if (cmd != 0)
		SendMessageW(c->hWnd, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
}

// -------------------- changes (zfs diff) --------------------

typedef struct {
	wchar_t chg;
	wchar_t cls;
	wchar_t path[520];
	wchar_t to[520];
} DiffEnt;

typedef struct {
	const wchar_t *summary;
	const DiffEnt *ents;
	int n;
	HWND list;
	HWND lbl;
	HWND btn;
	BOOL done;
} DiffWin;

static LRESULT CALLBACK
DiffWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	DiffWin *p = (DiffWin *)GetWindowLongPtrW(hWnd, GWLP_USERDATA);

	switch (msg) {
	case WM_NCCREATE:
		SetWindowLongPtrW(hWnd, GWLP_USERDATA,
		    (LONG_PTR)((CREATESTRUCTW *)lParam)->lpCreateParams);
		break;
	case WM_SIZE:
		if (p != NULL && p->list != NULL) {
			RECT rc;

			GetClientRect(hWnd, &rc);
			MoveWindow(p->lbl, 10, 8, rc.right - 20, 20, TRUE);
			MoveWindow(p->list, 10, 34, rc.right - 20,
			    rc.bottom - 34 - 46, TRUE);
			MoveWindow(p->btn, rc.right - 100, rc.bottom - 36,
			    90, 26, TRUE);
		}
		return (0);
	case WM_COMMAND:
		if (LOWORD(wParam) == IDCANCEL) {
			DestroyWindow(hWnd);
			return (0);
		}
		break;
	case WM_DESTROY:
		if (p != NULL)
			p->done = TRUE;
		return (0);
	}
	return (DefWindowProcW(hWnd, msg, wParam, lParam));
}

static const wchar_t *
DiffChangeName(wchar_t chg)
{
	switch (chg) {
	case L'+':
		return (L"Added");
	case L'-':
		return (L"Removed");
	case L'M':
		return (L"Modified");
	case L'R':
		return (L"Renamed");
	}
	return (L"Changed");
}

static const wchar_t *
DiffClassName(wchar_t cls)
{
	switch (cls) {
	case L'/':
		return (L"Folder");
	case L'F':
		return (L"File");
	case L'@':
		return (L"Link");
	}
	return (L"Other");
}

// A list of what changed, in a window of its own that the owner waits for.
static void
ShowDiffWindow(HWND owner, const wchar_t *title, const wchar_t *summary,
    const DiffEnt *ents, int n)
{
	static BOOL registered = FALSE;
	HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE);
	HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
	DiffWin p;
	RECT orc;
	MSG m;
	HWND dlg;
	LVCOLUMNW col = { 0 };
	int i, w = 780, h = 520;

	if (!registered) {
		WNDCLASSW wc = { 0 };

		wc.lpfnWndProc = DiffWndProc;
		wc.hInstance = hInst;
		wc.lpszClassName = L"ZfsDiffWnd";
		wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
		wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
		RegisterClassW(&wc);
		registered = TRUE;
	}

	ZeroMemory(&p, sizeof (p));
	p.summary = summary;
	p.ents = ents;
	p.n = n;
	GetWindowRect(owner, &orc);
	dlg = CreateWindowExW(0, L"ZfsDiffWnd", title,
	    WS_OVERLAPPEDWINDOW | WS_VISIBLE,
	    orc.left + ((orc.right - orc.left) - w) / 2, orc.top + 40, w, h,
	    owner, NULL, hInst, &p);
	if (dlg == NULL)
		return;

	p.lbl = MakeChild(dlg, L"STATIC", summary, SS_LEFT, 0);
	SendMessageW(p.lbl, WM_SETFONT, (WPARAM)font, TRUE);
	p.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
	    WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LVS_REPORT |
	    LVS_SHOWSELALWAYS, 0, 0, 10, 10, dlg, NULL, hInst, NULL);
	SendMessageW(p.list, WM_SETFONT, (WPARAM)font, TRUE);
	ListView_SetExtendedListViewStyle(p.list, LVS_EX_FULLROWSELECT);
	col.mask = LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
	col.pszText = L"Change";
	col.cx = 80;
	col.iSubItem = 0;
	ListView_InsertColumn(p.list, 0, &col);
	col.pszText = L"Type";
	col.cx = 70;
	col.iSubItem = 1;
	ListView_InsertColumn(p.list, 1, &col);
	col.pszText = L"Path";
	col.cx = 560;
	col.iSubItem = 2;
	ListView_InsertColumn(p.list, 2, &col);

	for (i = 0; i < n; i++) {
		LVITEMW it = { 0 };
		wchar_t path[1100];

		it.mask = LVIF_TEXT;
		it.iItem = i;
		it.pszText = (LPWSTR)DiffChangeName(ents[i].chg);
		ListView_InsertItem(p.list, &it);
		ListView_SetItemText(p.list, i, 1,
		    (LPWSTR)DiffClassName(ents[i].cls));
		if (ents[i].to[0] != L'\0')
			_snwprintf_s(path, ARRAYSIZE(path), _TRUNCATE,
			    L"%s  ->  %s", ents[i].path, ents[i].to);
		else
			lstrcpynW(path, ents[i].path, ARRAYSIZE(path));
		ListView_SetItemText(p.list, i, 2, path);
	}

	p.btn = MakeChild(dlg, L"BUTTON", L"Close", BS_DEFPUSHBUTTON |
	    WS_TABSTOP, IDCANCEL);
	SendMessageW(p.btn, WM_SETFONT, (WPARAM)font, TRUE);
	SendMessageW(dlg, WM_SIZE, 0, 0);
	{
		RECT rc;

		GetClientRect(dlg, &rc);
		SendMessageW(dlg, WM_SIZE, SIZE_RESTORED,
		    MAKELPARAM(rc.right, rc.bottom));
	}

	EnableWindow(owner, FALSE);
	while (!p.done && GetMessageW(&m, NULL, 0, 0) > 0) {
		if (!IsDialogMessageW(dlg, &m)) {
			TranslateMessage(&m);
			DispatchMessageW(&m);
		}
	}
	EnableWindow(owner, TRUE);
	SetForegroundWindow(owner);
}

// One "key": value of a change.
static void
DiffKey(const char *json, const jsmntok_t *k, const jsmntok_t *v, DiffEnt *d,
    wchar_t *chg, wchar_t *cls)
{
	if (jsmn_eq(json, k, "chg"))
		TokWide(json, v, chg, 8);
	else if (jsmn_eq(json, k, "cls"))
		TokWide(json, v, cls, 8);
	else if (jsmn_eq(json, k, "path"))
		TokWide(json, v, d->path, ARRAYSIZE(d->path));
	else if (jsmn_eq(json, k, "to"))
		TokWide(json, v, d->to, ARRAYSIZE(d->to));
}

// The changes in OP_SNAPSHOT_DIFF's reply. Free the result with free().
static DiffEnt *
ParseDiff(const char *json, int json_len, int *count)
{
	jsmntok_t *tok;
	DiffEnt *ents = NULL;
	int r, i, n = 0;

	*count = 0;
	tok = ParseTokens(json, json_len, &r);
	if (tok == NULL)
		return (NULL);
	i = 1;
	while (i + 1 < r && tok[i].start < tok[0].end) {
		const jsmntok_t *v = &tok[i + 1];

		if (v->type == JSMN_ARRAY &&
		    jsmn_eq(json, &tok[i], "changes")) {
			int cnt = v->size, e, j = i + 2;

			ents = calloc(cnt > 0 ? cnt : 1, sizeof (DiffEnt));
			for (e = 0; ents != NULL && e < cnt && j < r; e++) {
				int oend = tok[j].end, q = j + 1;
				wchar_t chg[8] = L"", cls[8] = L"";
				DiffEnt *d = &ents[n];

				while (q + 1 < r && tok[q].start < oend) {
					DiffKey(json, &tok[q], &tok[q + 1], d,
					    chg, cls);
					q = SkipValue(tok, q + 1, r);
				}
				d->chg = chg[0];
				d->cls = cls[0];
				n++;
				j = q;
			}
			break;
		}
		i = SkipValue(tok, i + 1, r);
	}
	HeapFree(GetProcessHeap(), 0, tok);
	*count = n;
	return (ents);
}

// What changed since the selected snapshot, or between the two selected.
static void
DoDiff(SCtx *c)
{
	int cnt = ListView_GetSelectedCount(c->hList);
	int sel = -1, idx[2], k = 0;
	const SnapEnt *from, *to = NULL;
	wchar_t err[256] = L"", title[600], summary[700];
	char ds[1024], a[512], b[512];
	char body[sizeof (uint32_t) + sizeof (ds) + sizeof (a) + sizeof (b)];
	uint8_t *out = NULL;
	uint32_t outlen = 0, flags = 0;
	size_t n1, n2, n3, off;
	DiffEnt *ents;
	BOOL ok, truncated;
	int n, i, added = 0, removed = 0, modified = 0, renamed = 0;
	HCURSOR old;

	if (cnt < 1 || cnt > 2)
		return;
	while (k < cnt && (sel = ListView_GetNextItem(c->hList, sel,
	    LVNI_SELECTED)) >= 0) {
		LVITEMW it = { 0 };

		it.mask = LVIF_PARAM;
		it.iItem = sel;
		if (!ListView_GetItem(c->hList, &it) || it.lParam < 0 ||
		    it.lParam >= c->nsnaps)
			return;
		idx[k++] = (int)it.lParam;
	}
	if (k != cnt)
		return;
	// The list is oldest first: the older one is where it starts.
	if (cnt == 2 && idx[0] > idx[1]) {
		int t = idx[0];

		idx[0] = idx[1];
		idx[1] = t;
	}
	from = &c->snaps[idx[0]];
	if (cnt == 2)
		to = &c->snaps[idx[1]];

	if (!ToUtf8(c->cur, ds, sizeof (ds)) ||
	    !ToUtf8(from->name, a, sizeof (a)) ||
	    (to != NULL && !ToUtf8(to->name, b, sizeof (b)))) {
		ErrorBox(c->hWnd, L"Invalid name.");
		return;
	}
	if (to == NULL)
		b[0] = '\0';
	n1 = strlen(ds) + 1;
	n2 = strlen(a) + 1;
	n3 = strlen(b) + 1;
	memcpy(body, &flags, sizeof (flags));
	off = sizeof (flags);
	memcpy(body + off, ds, n1);
	off += n1;
	memcpy(body + off, a, n2);
	off += n2;
	memcpy(body + off, b, n3);
	off += n3;

	old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
	ok = Call(c, OP_SNAPSHOT_DIFF, body, (uint32_t)off, &out, &outlen, err,
	    ARRAYSIZE(err));
	SetCursor(old);
	if (!ok) {
		ErrorBox(c->hWnd, err);
		return;
	}
	if (!ReplyOk((const char *)out, (int)outlen, err, ARRAYSIZE(err))) {
		if (err[0] == L'\0')
			lstrcpynW(err, L"Could not list the changes.",
			    ARRAYSIZE(err));
		HeapFree(GetProcessHeap(), 0, out);
		ErrorBox(c->hWnd, err);
		return;
	}
	truncated = ReplyBool((const char *)out, (int)outlen, "truncated");
	ents = ParseDiff((const char *)out, (int)outlen, &n);
	HeapFree(GetProcessHeap(), 0, out);

	if (n == 0) {
		MessageBoxW(c->hWnd, L"Nothing has changed.", L"Changes",
		    MB_OK | MB_ICONINFORMATION);
		free(ents);
		return;
	}
	for (i = 0; i < n; i++) {
		switch (ents[i].chg) {
		case L'+':
			added++;
			break;
		case L'-':
			removed++;
			break;
		case L'M':
			modified++;
			break;
		case L'R':
			renamed++;
			break;
		}
	}
	if (to != NULL)
		_snwprintf_s(title, ARRAYSIZE(title), _TRUNCATE,
		    L"Changes from %s@%s to %s@%s", c->cur, from->name, c->cur,
		    to->name);
	else
		_snwprintf_s(title, ARRAYSIZE(title), _TRUNCATE,
		    L"Changes in %s since %s", c->cur, from->name);
	_snwprintf_s(summary, ARRAYSIZE(summary), _TRUNCATE,
	    L"%d added, %d removed, %d modified, %d renamed%s", added, removed,
	    modified, renamed, truncated ? L" (only the first are listed)" :
	    L"");
	ShowDiffWindow(c->hWnd, title, summary, ents, n);
	free(ents);
}

// -------------------- window --------------------

static void
Layout(SCtx *c)
{
	RECT rc;
	int w, h, bh = 26, pad = 10, top = 40, lw = 270;
	int row1, row2, x0, rw, x, listtop, pw;

	GetClientRect(c->hWnd, &rc);
	w = rc.right;
	h = rc.bottom;
	row2 = h - bh - pad;
	row1 = row2 - bh - pad;
	x0 = pad + lw + pad;
	rw = w - x0 - pad;
	listtop = top + 72;

	// Left: the datasets, with new/destroy and mount/unmount under them.
	MoveWindow(c->hTree, pad, top, lw, row1 - top - pad, TRUE);
	MoveWindow(GetDlgItem(c->hWnd, IDC_SNEWDS), pad, row1, 90, bh, TRUE);
	MoveWindow(GetDlgItem(c->hWnd, IDC_SDELDS), pad + 98, row1, 90, bh,
	    TRUE);
	MoveWindow(GetDlgItem(c->hWnd, IDC_SRENDS), pad + 196, row1, 84, bh,
	    TRUE);
	MoveWindow(GetDlgItem(c->hWnd, IDC_SMOUNT), pad, row2, 90, bh, TRUE);
	MoveWindow(GetDlgItem(c->hWnd, IDC_SUNMOUNT), pad + 98, row2, 90, bh,
	    TRUE);
	MoveWindow(GetDlgItem(c->hWnd, IDC_SPROMOTE), pad + 196, row2, 84, bh,
	    TRUE);

	// Right: what is known about the selected dataset, its snapshots.
	MoveWindow(GetDlgItem(c->hWnd, IDC_SDSINFO), x0, top, rw, 36, TRUE);
	MoveWindow(c->hTab, x0, top + 40, rw, 28, TRUE);
	MoveWindow(c->hList, x0, listtop, rw, row1 - listtop - pad, TRUE);
	// The properties page takes the place of the snapshots page.
	MoveWindow(c->hPropList, x0, listtop, rw, row2 - listtop - pad, TRUE);
	MoveWindow(GetDlgItem(c->hWnd, IDC_PEDIT), x0, row2, 90, bh, TRUE);
	MoveWindow(GetDlgItem(c->hWnd, IDC_PINHERIT), x0 + 98, row2, 90, bh,
	    TRUE);
	// Up to the Refresh button, which both pages share.
	pw = rw - 196 - 286 - pad;
	if (pw < 20)
		pw = 20;
	MoveWindow(GetDlgItem(c->hWnd, IDC_PINFO), x0 + 196, row2 + 4, pw, 20,
	    TRUE);

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
	    rw - 384 - pad, 20, TRUE);
	MoveWindow(GetDlgItem(c->hWnd, IDC_SCLONE), w - pad - 384, row2, 90,
	    bh, TRUE);
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
	// Children go under filesystems; a pool is destroyed from the menu.
	EnableWindow(GetDlgItem(c->hWnd, IDC_SNEWDS),
	    d != NULL && !d->volume);
	EnableWindow(GetDlgItem(c->hWnd, IDC_SDELDS),
	    d != NULL && d->depth > 0);
	EnableWindow(GetDlgItem(c->hWnd, IDC_SRENDS),
	    d != NULL && d->depth > 0);
	// Only a clone has something to promote.
	EnableWindow(GetDlgItem(c->hWnd, IDC_SPROMOTE),
	    d != NULL && d->origin[0] != L'\0');
}

static void LoadProps(SCtx *c);

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
	if (c->page == 1)
		LoadProps(c);
}

// -------------------- properties --------------------

static void
FreeProps(SCtx *c)
{
	if (c->props)
		HeapFree(GetProcessHeap(), 0, c->props);
	c->props = NULL;
	c->nprops = 0;
}

// The selected property, or NULL.
static const PropEnt *
SelectedProp(SCtx *c)
{
	int sel = ListView_GetNextItem(c->hPropList, -1, LVNI_SELECTED);
	LVITEMW it = { 0 };

	if (sel < 0)
		return (NULL);
	it.mask = LVIF_PARAM;
	it.iItem = sel;
	if (!ListView_GetItem(c->hPropList, &it) || it.lParam < 0 ||
	    it.lParam >= c->nprops)
		return (NULL);
	return (&c->props[it.lParam]);
}

static void
UpdatePropButtons(SCtx *c)
{
	const PropEnt *e = SelectedProp(c);
	BOOL set = (e != NULL && !e->readonly);

	EnableWindow(GetDlgItem(c->hWnd, IDC_PEDIT), set);
	EnableWindow(GetDlgItem(c->hWnd, IDC_PINHERIT), set &&
	    e->inheritable && (lstrcmpW(e->source, L"local") == 0 ||
	    lstrcmpW(e->source, L"received") == 0));
}

static void
PopulateProps(SCtx *c)
{
	int i;

	ListView_DeleteAllItems(c->hPropList);
	for (i = 0; i < c->nprops; i++) {
		const PropEnt *e = &c->props[i];
		LVITEMW it = { 0 };
		wchar_t src[300];

		it.mask = LVIF_TEXT | LVIF_PARAM;
		it.iItem = i;
		it.pszText = (LPWSTR)e->name;
		it.lParam = i;
		ListView_InsertItem(c->hPropList, &it);
		ListView_SetItemText(c->hPropList, i, 1, (LPWSTR)e->value);
		if (lstrcmpW(e->source, L"inherited") == 0)
			_snwprintf_s(src, ARRAYSIZE(src), _TRUNCATE,
			    L"inherited from %s", e->from);
		else if (lstrcmpW(e->source, L"none") == 0)
			src[0] = L'\0';
		else
			lstrcpynW(src, e->source, ARRAYSIZE(src));
		ListView_SetItemText(c->hPropList, i, 2, src);
	}
	UpdatePropButtons(c);
}

// Ask for the properties of c->cur.
static void
LoadProps(SCtx *c)
{
	char utf8[1024];
	uint8_t *out = NULL;
	uint32_t outlen = 0;
	wchar_t err[256] = L"";

	FreeProps(c);
	if (c->cur[0] == L'\0' || !ToUtf8(c->cur, utf8, sizeof (utf8))) {
		PopulateProps(c);
		return;
	}
	if (!Call(c, OP_DATASET_PROPS, utf8, (uint32_t)strlen(utf8) + 1, &out,
	    &outlen, err, ARRAYSIZE(err))) {
		PopulateProps(c);
		SetDlgItemTextW(c->hWnd, IDC_PINFO, err);
		return;
	}
	if (!ParseProps(c, (const char *)out, (int)outlen)) {
		ReplyOk((const char *)out, (int)outlen, err, ARRAYSIZE(err));
		if (err[0] == L'\0')
			lstrcpynW(err, L"Unexpected reply from the service.",
			    ARRAYSIZE(err));
		FreeProps(c);
		HeapFree(GetProcessHeap(), 0, out);
		PopulateProps(c);
		SetDlgItemTextW(c->hWnd, IDC_PINFO, err);
		return;
	}
	HeapFree(GetProcessHeap(), 0, out);
	PopulateProps(c);
	SetDlgItemTextW(c->hWnd, IDC_PINFO, L"");
}

// The plain words in ZFS's hint ("on | off | lz4 | gzip-[1-9] | <size>"),
// the patterns left out, as suggestions for the value.
#define	MAX_CHOICES	32
typedef struct {
	wchar_t text[MAX_CHOICES][64];
	const wchar_t *list[MAX_CHOICES + 1];
} ChoiceList;

static void
HintChoices(const wchar_t *hint, ChoiceList *cl)
{
	const wchar_t *p = hint;
	int n = 0;

	ZeroMemory(cl, sizeof (*cl));
	while (*p != L'\0' && n < MAX_CHOICES) {
		const wchar_t *end = wcschr(p, L'|');
		int len, i;
		BOOL plain = TRUE;

		if (end == NULL)
			end = p + wcslen(p);
		while (p < end && *p == L' ')
			p++;
		len = (int)(end - p);
		while (len > 0 && p[len - 1] == L' ')
			len--;
		for (i = 0; i < len; i++) {
			wchar_t ch = p[i];

			if (!((ch >= L'a' && ch <= L'z') ||
			    (ch >= L'A' && ch <= L'Z') ||
			    (ch >= L'0' && ch <= L'9') || ch == L'-' ||
			    ch == L'_' || ch == L'.'))
				plain = FALSE;
		}
		if (plain && len > 0 && len < 64) {
			memcpy(cl->text[n], p, len * sizeof (wchar_t));
			cl->list[n] = cl->text[n];
			n++;
		}
		p = (*end == L'|') ? end + 1 : end;
	}
	cl->list[n] = NULL;
}

// One OP_DATASET_SETPROP. 0 done, otherwise err says why.
static int
SetPropCall(SCtx *c, uint32_t flags, const wchar_t *ds, const wchar_t *name,
    const wchar_t *value, wchar_t *err, int errcch)
{
	char body[sizeof (uint32_t) + 1024 + 128 + 2048];
	char *w = body + sizeof (uint32_t);
	uint8_t *out = NULL;
	uint32_t outlen = 0;
	size_t n;

	err[0] = L'\0';
	memcpy(body, &flags, sizeof (flags));
	if (!ToUtf8(ds, w, 1024)) {
		lstrcpynW(err, L"Invalid name.", errcch);
		return (2);
	}
	w += strlen(w) + 1;
	if (!ToUtf8(name, w, 128)) {
		lstrcpynW(err, L"Invalid property.", errcch);
		return (2);
	}
	w += strlen(w) + 1;
	if (!ToUtf8(value, w, 2048)) {
		lstrcpynW(err, L"The value is too long.", errcch);
		return (2);
	}
	w += strlen(w) + 1;
	n = (size_t)(w - body);
	if (!Call(c, OP_DATASET_SETPROP, body, (uint32_t)n, &out, &outlen,
	    err, errcch))
		return (2);
	if (!ReplyOk((const char *)out, (int)outlen, err, errcch)) {
		if (err[0] == L'\0')
			lstrcpynW(err, L"The property was not changed.",
			    errcch);
		HeapFree(GetProcessHeap(), 0, out);
		return (1);
	}
	HeapFree(GetProcessHeap(), 0, out);
	return (0);
}

// A change can move the dataset (mountpoint, drive letter) or change what
// the tree and the info line show, so look at everything again.
static void
AfterPropChange(SCtx *c)
{
	wchar_t keep[256];

	lstrcpynW(keep, c->cur, ARRAYSIZE(keep));
	(void) LoadDatasets(c, keep, TRUE);
}

static void
DoEditProp(SCtx *c)
{
	const PropEnt *e = SelectedProp(c);
	ChoiceList cl;
	wchar_t name[64], value[512], hint[512], prompt[400], err[256];

	if (e == NULL || e->readonly)
		return;
	lstrcpynW(name, e->name, ARRAYSIZE(name));
	lstrcpynW(value, e->value, ARRAYSIZE(value));
	lstrcpynW(hint, e->hint, ARRAYSIZE(hint));
	HintChoices(hint, &cl);
	if (hint[0] != L'\0')
		_snwprintf_s(prompt, ARRAYSIZE(prompt), _TRUNCATE,
		    L"%s of %s\n(%s)", name, c->cur, hint);
	else
		_snwprintf_s(prompt, ARRAYSIZE(prompt), _TRUNCATE,
		    L"%s of %s", name, c->cur);
	if (!PromptText(c->hWnd, L"Change property", prompt, NULL, value,
	    ARRAYSIZE(value), NULL, 0, NULL, 0, cl.list))
		return;
	if (lstrcmpW(value, e->value) == 0)
		return;
	if (SetPropCall(c, 0, c->cur, name, value, err, ARRAYSIZE(err)) != 0)
		ErrorBox(c->hWnd, err);
	AfterPropChange(c);
}

static void
DoInheritProp(SCtx *c)
{
	const PropEnt *e = SelectedProp(c);
	wchar_t name[64], err[256];

	if (e == NULL || e->readonly || !e->inheritable)
		return;
	lstrcpynW(name, e->name, ARRAYSIZE(name));
	if (SetPropCall(c, ZSETPROP_INHERIT, c->cur, name, L"", err,
	    ARRAYSIZE(err)) != 0)
		ErrorBox(c->hWnd, err);
	AfterPropChange(c);
}

// Show one of the pages under the tab strip.
static void
ShowPage(SCtx *c, int page)
{
	static const int snapids[] = { IDC_SNEWLBL, IDC_SNAME,
	    IDC_SRECURSIVE, IDC_SCREATE, IDC_SINFO, IDC_SCLONE,
	    IDC_SROLLBACK, IDC_SDESTROY };
	static const int propids[] = { IDC_PEDIT, IDC_PINHERIT, IDC_PINFO };
	size_t i;

	c->page = page;
	ShowWindow(c->hList, page == 0 ? SW_SHOW : SW_HIDE);
	for (i = 0; i < ARRAYSIZE(snapids); i++)
		ShowWindow(GetDlgItem(c->hWnd, snapids[i]),
		    page == 0 ? SW_SHOW : SW_HIDE);
	ShowWindow(c->hPropList, page == 1 ? SW_SHOW : SW_HIDE);
	for (i = 0; i < ARRAYSIZE(propids); i++)
		ShowWindow(GetDlgItem(c->hWnd, propids[i]),
		    page == 1 ? SW_SHOW : SW_HIDE);
	if (page == 1)
		LoadProps(c);
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
		} else if (c != NULL && nh->idFrom == IDC_SLIST &&
		    nh->code == NM_RCLICK) {
		SnapMenu(c);
		} else if (c != NULL && nh->idFrom == IDC_SLIST &&
		    nh->code == NM_DBLCLK) {
			DoDiff(c);
		} else if (c != NULL && nh->idFrom == IDC_SLIST &&
		    nh->code == LVN_KEYDOWN &&
		    ((NMLVKEYDOWN *)lParam)->wVKey == VK_F2) {
		DoRenameSnapshot(c);
		} else if (c != NULL && nh->idFrom == IDC_PLIST &&
		    nh->code == LVN_ITEMCHANGED) {
			UpdatePropButtons(c);
		} else if (c != NULL && nh->idFrom == IDC_PLIST &&
		    nh->code == NM_DBLCLK) {
			DoEditProp(c);
		} else if (c != NULL && nh->idFrom == IDC_STAB &&
		    nh->code == TCN_SELCHANGE) {
			ShowPage(c, TabCtrl_GetCurSel(c->hTab));
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
		case IDC_PEDIT:
			DoEditProp(c);
			break;
		case IDC_PINHERIT:
			DoInheritProp(c);
			break;
		case IDC_SNEWDS:
			DoNewDataset(c);
			break;
		case IDC_SDELDS:
			DoDestroyDataset(c);
			break;
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
		case IDC_SCLONE:
			DoClone(c);
			break;
		case IDC_SRENDS:
			DoRenameDataset(c);
			break;
		case IDM_SRENSNAP:
			DoRenameSnapshot(c);
			break;
		case IDM_SDIFF:
			DoDiff(c);
			break;
		case IDC_SPROMOTE:
			DoPromote(c);
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
	    ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES |
	    ICC_TAB_CLASSES };
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
	SendMessageW(MakeChild(ctx.hWnd, L"BUTTON", L"New...",
	    BS_PUSHBUTTON | WS_TABSTOP, IDC_SNEWDS), WM_SETFONT,
	    (WPARAM)font, TRUE);
	SendMessageW(MakeChild(ctx.hWnd, L"BUTTON", L"Destroy...",
	    BS_PUSHBUTTON | WS_TABSTOP, IDC_SDELDS), WM_SETFONT,
	    (WPARAM)font, TRUE);
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

	{
		TCITEMW ti = { 0 };

		ctx.hTab = MakeChild(ctx.hWnd, WC_TABCONTROLW, L"",
		    WS_TABSTOP, IDC_STAB);
		SendMessageW(ctx.hTab, WM_SETFONT, (WPARAM)font, TRUE);
		ti.mask = TCIF_TEXT;
		ti.pszText = L"Snapshots";
		TabCtrl_InsertItem(ctx.hTab, 0, &ti);
		ti.pszText = L"Properties";
		TabCtrl_InsertItem(ctx.hTab, 1, &ti);
	}
	ctx.hPropList = MakeChild(ctx.hWnd, WC_LISTVIEWW, L"",
	    LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER |
	    WS_TABSTOP, IDC_PLIST);
	ListView_SetExtendedListViewStyle(ctx.hPropList,
	    LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
	SendMessageW(ctx.hPropList, WM_SETFONT, (WPARAM)font, TRUE);
	{
		static const struct { const wchar_t *name; int cx; } pc[] = {
			{ L"Property", 150 }, { L"Value", 200 },
			{ L"Source", 190 },
		};
		LVCOLUMNW col = { 0 };
		int k;

		col.mask = LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
		for (k = 0; k < (int)ARRAYSIZE(pc); k++) {
			col.pszText = (LPWSTR)pc[k].name;
			col.cx = pc[k].cx;
			col.iSubItem = k;
			ListView_InsertColumn(ctx.hPropList, k, &col);
		}
	}
	SendMessageW(MakeChild(ctx.hWnd, L"BUTTON", L"Edit...",
	    BS_PUSHBUTTON | WS_TABSTOP, IDC_PEDIT), WM_SETFONT,
	    (WPARAM)font, TRUE);
	SendMessageW(MakeChild(ctx.hWnd, L"BUTTON", L"Inherit",
	    BS_PUSHBUTTON | WS_TABSTOP, IDC_PINHERIT), WM_SETFONT,
	    (WPARAM)font, TRUE);
	SendMessageW(MakeChild(ctx.hWnd, L"STATIC", L"",
	    SS_LEFT | SS_PATHELLIPSIS, IDC_PINFO), WM_SETFONT,
	    (WPARAM)font, TRUE);

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
	SendMessageW(MakeChild(ctx.hWnd, L"BUTTON", L"Rename...",
	    BS_PUSHBUTTON | WS_TABSTOP, IDC_SRENDS), WM_SETFONT,
	    (WPARAM)font, TRUE);
	SendMessageW(MakeChild(ctx.hWnd, L"BUTTON", L"Clone...",
	    BS_PUSHBUTTON | WS_TABSTOP, IDC_SCLONE), WM_SETFONT,
	    (WPARAM)font, TRUE);
	SendMessageW(MakeChild(ctx.hWnd, L"BUTTON", L"Promote...",
	    BS_PUSHBUTTON | WS_TABSTOP, IDC_SPROMOTE), WM_SETFONT,
	    (WPARAM)font, TRUE);
	SendMessageW(MakeChild(ctx.hWnd, L"BUTTON", L"Rollback...",
	    BS_PUSHBUTTON | WS_TABSTOP, IDC_SROLLBACK), WM_SETFONT,
	    (WPARAM)font, TRUE);
	SendMessageW(MakeChild(ctx.hWnd, L"BUTTON", L"Destroy...",
	    BS_PUSHBUTTON | WS_TABSTOP, IDC_SDESTROY), WM_SETFONT,
	    (WPARAM)font, TRUE);

	ApplyThemeFollowSystem(ctx.hWnd);
	Layout(&ctx);
	ShowPage(&ctx, 0);
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
	FreeProps(&ctx);
	if (ctx.ds)
		HeapFree(GetProcessHeap(), 0, ctx.ds);
	return (0);
}
