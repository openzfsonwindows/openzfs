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

#define	UNICODE
#define	_UNICODE

#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define	JSMN_STATIC
#include "jsmn.h"

#include "update_check.h"

#pragma comment(lib, "winhttp.lib")

#ifndef IMAGE_FILE_MACHINE_ARM64
#define	IMAGE_FILE_MACHINE_ARM64	0xAA64
#endif

#define	UC_TIMER_FIRST	0x7a01
#define	UC_TIMER_DAILY	0x7a02
#define	UC_FIRST_MS	(30 * 1000)
#define	UC_DAILY_MS	(24 * 60 * 60 * 1000)

#define	UC_VER_MAX	48
#define	UC_URL_MAX	512
#define	UC_BODY_MAX	(8 * 1024 * 1024)

#define	UC_REG_KEY	L"Software\\OpenZFS\\zfs_tray"
#define	UC_REG_CHECK	L"CheckForUpdates"
#define	UC_REG_PRE	L"IncludePrereleases"
#define	UC_REG_NOTIFIED	L"NotifiedUpdateVersion"

#define	UC_API_HOST	L"api.github.com"
#define	UC_API_PATH	\
	L"/repos/openzfsonwindows/openzfs/releases?per_page=30"

/*
 * <maj>.<min>.<pat>[rc<N>][<letter>], and nothing else. That is what
 * excludes the memleak, ZVOL..., ".99-...-g<hash>" and "-dirty" installers.
 */
typedef struct {
	unsigned maj;
	unsigned min;
	unsigned pat;
	unsigned rc;	/* UINT_MAX: no "rc", a final release */
	char suffix;	/* 0: none */
} uc_ver_t;

typedef struct {
	HWND hwnd;
	LONG gen;
	BOOL prerelease;
} uc_job_t;

typedef struct {
	LONG gen;
	BOOL ok;	/* FALSE: keep whatever is currently shown */
	BOOL have;	/* a newer installer than the driver exists */
	wchar_t ver[UC_VER_MAX];
	wchar_t installed[UC_VER_MAX];
	wchar_t url[UC_URL_MAX];
} uc_result_t;

static update_notify_fn g_notify;
static BOOL g_enabled = TRUE;
static BOOL g_prerelease = FALSE;
static volatile LONG g_gen = 0;

/* What is currently shown (UI thread only) */
static BOOL g_avail = FALSE;
static wchar_t g_avail_ver[UC_VER_MAX];
static wchar_t g_avail_url[UC_URL_MAX];

/* The version we last showed a notification for */
static wchar_t g_notified[UC_VER_MAX];

/* ---- version parsing and ordering ---- */

static BOOL
uc_parse_number(const char *s, size_t n, size_t *pos, unsigned *out)
{
	size_t i = *pos;
	unsigned v = 0;
	int digits = 0;

	while (i < n && s[i] >= '0' && s[i] <= '9') {
		if (++digits > 9)
			return (FALSE);
		v = v * 10 + (unsigned)(s[i] - '0');
		i++;
	}
	if (digits == 0)
		return (FALSE);
	*pos = i;
	*out = v;
	return (TRUE);
}

static BOOL
uc_parse_ver(const char *s, size_t n, uc_ver_t *v)
{
	size_t i = 0;

	memset(v, 0, sizeof (*v));
	v->rc = UINT_MAX;

	if (!uc_parse_number(s, n, &i, &v->maj))
		return (FALSE);
	if (i >= n || s[i++] != '.')
		return (FALSE);
	if (!uc_parse_number(s, n, &i, &v->min))
		return (FALSE);
	if (i >= n || s[i++] != '.')
		return (FALSE);
	if (!uc_parse_number(s, n, &i, &v->pat))
		return (FALSE);

	if (i + 2 < n && s[i] == 'r' && s[i + 1] == 'c' &&
	    s[i + 2] >= '0' && s[i + 2] <= '9') {
		i += 2;
		if (!uc_parse_number(s, n, &i, &v->rc))
			return (FALSE);
		if (v->rc == UINT_MAX)
			return (FALSE);
	}

	if (i < n && s[i] >= 'a' && s[i] <= 'z')
		v->suffix = s[i++];

	return (i == n);
}

/*
 * Numbers first, then rcN numerically (no rc sorts after its rcs), then the
 * letter (no letter sorts before one):
 * 2.4.1rc16 < 2.4.1rc16b < 2.4.1rc17 < 2.4.1rc17b < 2.4.1
 */
static int
uc_ver_cmp(const uc_ver_t *a, const uc_ver_t *b)
{
	if (a->maj != b->maj)
		return (a->maj < b->maj ? -1 : 1);
	if (a->min != b->min)
		return (a->min < b->min ? -1 : 1);
	if (a->pat != b->pat)
		return (a->pat < b->pat ? -1 : 1);
	if (a->rc != b->rc)
		return (a->rc < b->rc ? -1 : 1);
	if (a->suffix != b->suffix)
		return (a->suffix < b->suffix ? -1 : 1);
	return (0);
}

/* ---- installed driver version ---- */

static BOOL
uc_driver_running(void)
{
	SC_HANDLE scm, svc;
	SERVICE_STATUS_PROCESS ssp;
	DWORD need = 0;
	BOOL running = TRUE;	/* can't tell: fall back to the registry */

	scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
	if (scm == NULL)
		return (TRUE);
	svc = OpenServiceW(scm, L"OpenZFS", SERVICE_QUERY_STATUS);
	if (svc != NULL) {
		if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
		    (LPBYTE)&ssp, sizeof (ssp), &need))
			running = (ssp.dwCurrentState == SERVICE_RUNNING);
		CloseServiceHandle(svc);
	}
	CloseServiceHandle(scm);
	return (running);
}

/*
 * For testing: a dev build reports "zfswin-2.4.1rc17-920-g...-dirty", which
 * is not a release version, so the check would never show anything. Setting
 * HKCU\Software\OpenZFS\zfs_tray\InstalledVersionOverride (REG_SZ,
 * e.g. "2.4.1rc1") makes the check use that as the installed version.
 */
static BOOL
uc_override_version(char *buf, size_t sz)
{
	HKEY key;
	DWORD type = 0, len = (DWORD)(sz - 1);
	LSTATUS st;

	if (RegOpenKeyExW(HKEY_CURRENT_USER, UC_REG_KEY, 0, KEY_READ,
	    &key) != ERROR_SUCCESS)
		return (FALSE);
	st = RegQueryValueExA(key, "InstalledVersionOverride", NULL, &type,
	    (LPBYTE)buf, &len);
	RegCloseKey(key);
	if (st != ERROR_SUCCESS || type != REG_SZ || len == 0)
		return (FALSE);
	buf[len < sz ? len : sz - 1] = 0;
	return (TRUE);
}

/*
 * The version the loaded driver reports, same place "zfs version" reads it
 * from: the zfs_version value on its service key, e.g. "zfswin-2.4.4rc5".
 */
static BOOL
uc_installed_version(uc_ver_t *v, char *str, size_t str_sz)
{
	HKEY key;
	char buf[128];
	DWORD type = 0, len = sizeof (buf) - 1;
	LSTATUS st;
	const char *s;

	if (uc_override_version(buf, sizeof (buf)))
		goto have;

	if (!uc_driver_running())
		return (FALSE);

	st = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
	    L"SYSTEM\\CurrentControlSet\\Services\\OpenZFS", 0,
	    KEY_READ | KEY_WOW64_64KEY, &key);
	if (st != ERROR_SUCCESS)
		return (FALSE);
	st = RegQueryValueExA(key, "zfs_version", NULL, &type, (LPBYTE)buf,
	    &len);
	RegCloseKey(key);
	if (st != ERROR_SUCCESS || type != REG_SZ)
		return (FALSE);
	buf[len < sizeof (buf) ? len : sizeof (buf) - 1] = 0;

have:
	s = buf;
	if (strncmp(s, "zfswin-", 7) == 0)
		s += 7;
	else if (strncmp(s, "zfs-", 4) == 0)
		s += 4;

	if (!uc_parse_ver(s, strlen(s), v) || strlen(s) >= str_sz)
		return (FALSE);
	strcpy(str, s);
	return (TRUE);
}

/* ---- picking the newest installer out of the releases JSON ---- */

static int
uc_skip(const jsmntok_t *tok, int n, int i)
{
	int end = tok[i].end;

	i++;
	while (i < n && tok[i].start < end)
		i++;
	return (i);
}

static BOOL
uc_tok_is(const char *js, const jsmntok_t *t, const char *s)
{
	size_t l = strlen(s);

	return (t->type == JSMN_STRING && (size_t)(t->end - t->start) == l &&
	    memcmp(js + t->start, s, l) == 0);
}

static BOOL
uc_tok_true(const char *js, const jsmntok_t *t)
{
	return (t->type == JSMN_PRIMITIVE && js[t->start] == 't');
}

/*
 * <prefix><ver>.exe, with <ver> a version and nothing else.
 */
static BOOL
uc_asset_match(const char *name, size_t nlen, const char *prefix,
    uc_ver_t *v, char *verstr, size_t verstr_sz)
{
	const char *suffix = ".exe";
	size_t pl = strlen(prefix), sl = strlen(suffix), vl;

	if (nlen <= pl + sl || memcmp(name, prefix, pl) != 0 ||
	    memcmp(name + nlen - sl, suffix, sl) != 0)
		return (FALSE);
	vl = nlen - pl - sl;
	if (vl >= verstr_sz || !uc_parse_ver(name + pl, vl, v))
		return (FALSE);
	memcpy(verstr, name + pl, vl);
	verstr[vl] = 0;
	return (TRUE);
}

/*
 * An asset name is only interesting as OpenZFSOnWindows-<ver>.exe or
 * OpenZFSOnWindows-debug-<ver>.exe (ARM64: OpenZFSOnWindows-ARM64-<ver>.exe
 * and OpenZFSOnWindows-ARM64-debug-<ver>.exe). Only the release name can be
 * confused with the other: "debug-2.4.4" is not a version, so it is not.
 */
static BOOL
uc_asset_version(const char *name, size_t nlen, BOOL arm64, uc_ver_t *v,
    char *verstr, size_t verstr_sz, BOOL *debug)
{
	*debug = FALSE;
	if (uc_asset_match(name, nlen, arm64 ? "OpenZFSOnWindows-ARM64-" :
	    "OpenZFSOnWindows-", v, verstr, verstr_sz))
		return (TRUE);
	*debug = TRUE;
	return (uc_asset_match(name, nlen, arm64 ?
	    "OpenZFSOnWindows-ARM64-debug-" : "OpenZFSOnWindows-debug-", v,
	    verstr, verstr_sz));
}

typedef struct {
	BOOL found;
	BOOL debug;	/* the best so far is a -debug- installer */
	uc_ver_t ver;
	char ver_str[UC_VER_MAX];
	char url[UC_URL_MAX];
} uc_best_t;

/* One asset object: if it is an installer for us and newer, remember it. */
static void
uc_consider_asset(const char *js, const jsmntok_t *tok, int n, int as,
    const jsmntok_t *url, BOOL arm64, uc_best_t *b)
{
	int k = as + 1;
	size_t ul = (size_t)(url->end - url->start);

	if (tok[as].type != JSMN_OBJECT || ul >= sizeof (b->url))
		return;

	/* Direct keys only; the uploader object has a "name"-ish of its own */
	while (k + 1 < n && tok[k].start < tok[as].end) {
		int v = k + 1;
		uc_ver_t ver;
		char vs[UC_VER_MAX];
		BOOL debug;

		/* Newer wins; the same version prefers the release flavour */
		if (uc_tok_is(js, &tok[k], "name") &&
		    tok[v].type == JSMN_STRING &&
		    uc_asset_version(js + tok[v].start,
		    (size_t)(tok[v].end - tok[v].start), arm64, &ver, vs,
		    sizeof (vs), &debug) &&
		    (!b->found || uc_ver_cmp(&ver, &b->ver) > 0 ||
		    (uc_ver_cmp(&ver, &b->ver) == 0 && b->debug && !debug))) {
			b->debug = debug;
			b->ver = ver;
			strcpy(b->ver_str, vs);
			memcpy(b->url, js + url->start, ul);
			b->url[ul] = 0;
			b->found = TRUE;
		}
		k = uc_skip(tok, n, v);
	}
}

/*
 * Every asset of every non-draft (and, unless asked, non-prerelease) release
 * is considered: the newest installer may be newer than its release's tag.
 * Returns FALSE if the document is not a releases array at all.
 */
static BOOL
uc_pick_best(const char *js, int len, BOOL prerelease, BOOL arm64,
    uc_best_t *b)
{
	jsmn_parser p;
	jsmntok_t *tok;
	int n, i, e;

	memset(b, 0, sizeof (*b));

	jsmn_init(&p);
	n = jsmn_parse(&p, js, (size_t)len, NULL, 0);
	if (n < 1)
		return (FALSE);
	tok = (jsmntok_t *)malloc(sizeof (jsmntok_t) * (size_t)n);
	if (tok == NULL)
		return (FALSE);
	jsmn_init(&p);
	if (jsmn_parse(&p, js, (size_t)len, tok, (unsigned int)n) != n ||
	    tok[0].type != JSMN_ARRAY) {
		free(tok);
		return (FALSE);
	}

	i = 1;
	for (e = 0; e < tok[0].size && i < n; e++) {
		int rel = i;
		int k, assets = -1, url = -1;
		BOOL draft = FALSE, pre = FALSE;

		i = uc_skip(tok, n, rel);
		if (tok[rel].type != JSMN_OBJECT)
			continue;

		/* Direct keys of this release only; nested objects skipped */
		k = rel + 1;
		while (k + 1 < n && tok[k].start < tok[rel].end) {
			int val = k + 1;

			if (uc_tok_is(js, &tok[k], "draft"))
				draft = uc_tok_true(js, &tok[val]);
			else if (uc_tok_is(js, &tok[k], "prerelease"))
				pre = uc_tok_true(js, &tok[val]);
			else if (uc_tok_is(js, &tok[k], "html_url") &&
			    tok[val].type == JSMN_STRING)
				url = val;
			else if (uc_tok_is(js, &tok[k], "assets") &&
			    tok[val].type == JSMN_ARRAY)
				assets = val;
			k = uc_skip(tok, n, val);
		}

		if (draft || (pre && !prerelease) || url < 0 || assets < 0)
			continue;

		k = assets + 1;
		while (k < n && tok[k].start < tok[assets].end) {
			uc_consider_asset(js, tok, n, k, &tok[url], arm64, b);
			k = uc_skip(tok, n, k);
		}
	}

	free(tok);
	return (TRUE);
}

/* ---- HTTP ---- */

static BOOL
uc_http_get(char **body, int *len)
{
	HINTERNET s = NULL, c = NULL, r = NULL;
	BOOL ok = FALSE;
	DWORD status = 0, sz = sizeof (status);
	char *buf = NULL;
	size_t used = 0, cap = 0;

	s = WinHttpOpen(L"zfs_tray", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
	    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (s == NULL)
		s = WinHttpOpen(L"zfs_tray", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (s == NULL)
		goto out;
	WinHttpSetTimeouts(s, 10000, 10000, 15000, 20000);

	c = WinHttpConnect(s, UC_API_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
	if (c == NULL)
		goto out;
	r = WinHttpOpenRequest(c, L"GET", UC_API_PATH, NULL,
	    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
	    WINHTTP_FLAG_SECURE);
	if (r == NULL)
		goto out;

	if (!WinHttpSendRequest(r,
	    L"Accept: application/vnd.github+json\r\n", (DWORD)-1L,
	    WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
	    !WinHttpReceiveResponse(r, NULL))
		goto out;

	if (!WinHttpQueryHeaders(r,
	    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
	    WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
	    WINHTTP_NO_HEADER_INDEX) || status != 200)
		goto out;

	for (;;) {
		DWORD avail = 0, got = 0;

		if (!WinHttpQueryDataAvailable(r, &avail))
			goto out;
		if (avail == 0)
			break;
		if (used + avail + 1 > UC_BODY_MAX)
			goto out;
		if (used + avail + 1 > cap) {
			size_t ncap = (cap ? cap * 2 : 65536);
			char *nb;

			while (ncap < used + avail + 1)
				ncap *= 2;
			nb = (char *)realloc(buf, ncap);
			if (nb == NULL)
				goto out;
			buf = nb;
			cap = ncap;
		}
		if (!WinHttpReadData(r, buf + used, avail, &got))
			goto out;
		if (got == 0)
			break;
		used += got;
	}
	if (used == 0)
		goto out;
	buf[used] = 0;
	*body = buf;
	*len = (int)used;
	buf = NULL;
	ok = TRUE;

out:
	free(buf);
	if (r != NULL)
		WinHttpCloseHandle(r);
	if (c != NULL)
		WinHttpCloseHandle(c);
	if (s != NULL)
		WinHttpCloseHandle(s);
	return (ok);
}

/* ---- worker ---- */

static BOOL
uc_machine_is_arm64(void)
{
	typedef BOOL(WINAPI *pfn_iswow64process2)(HANDLE, USHORT *,
	    USHORT *);
	HMODULE k = GetModuleHandleW(L"kernel32.dll");
	pfn_iswow64process2 f = NULL;
	SYSTEM_INFO si;

	if (k != NULL)
		f = (pfn_iswow64process2)(void *)GetProcAddress(k,
		    "IsWow64Process2");
	if (f != NULL) {
		USHORT proc = 0, native = 0;

		if (f(GetCurrentProcess(), &proc, &native))
			return (native == IMAGE_FILE_MACHINE_ARM64);
	}
	GetNativeSystemInfo(&si);
	return (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64);
}

static DWORD WINAPI
uc_thread(LPVOID arg)
{
	uc_job_t *job = (uc_job_t *)arg;
	uc_result_t *res;
	uc_ver_t installed;
	char installed_str[UC_VER_MAX];
	uc_best_t best;
	char *body = NULL;
	int blen = 0;

	res = (uc_result_t *)calloc(1, sizeof (*res));
	if (res == NULL) {
		free(job);
		return (0);
	}
	res->gen = job->gen;

	if (!uc_installed_version(&installed, installed_str,
	    sizeof (installed_str))) {
		/* No driver, or no version we can read: show nothing */
		res->ok = TRUE;
	} else if (uc_http_get(&body, &blen) &&
	    uc_pick_best(body, blen, job->prerelease, uc_machine_is_arm64(),
	    &best)) {
		res->ok = TRUE;
		if (best.found && uc_ver_cmp(&best.ver, &installed) > 0) {
			res->have = TRUE;
			MultiByteToWideChar(CP_UTF8, 0, installed_str, -1,
			    res->installed, UC_VER_MAX);
			MultiByteToWideChar(CP_UTF8, 0, best.ver_str, -1,
			    res->ver, UC_VER_MAX);
			MultiByteToWideChar(CP_UTF8, 0, best.url, -1,
			    res->url, UC_URL_MAX);
		}
	}
	free(body);

	if (!PostMessageW(job->hwnd, WM_UPDATE_CHECK_DONE, 0, (LPARAM)res))
		free(res);
	free(job);
	return (0);
}

static void
uc_start_check(HWND hwnd)
{
	uc_job_t *job;
	HANDLE h;

	if (!g_enabled)
		return;
	job = (uc_job_t *)calloc(1, sizeof (*job));
	if (job == NULL)
		return;
	job->hwnd = hwnd;
	job->gen = g_gen;
	job->prerelease = g_prerelease;
	h = CreateThread(NULL, 0, uc_thread, job, 0, NULL);
	if (h == NULL) {
		free(job);
		return;
	}
	CloseHandle(h);
}

/* ---- settings ---- */

static void
uc_reg_set_dword(const wchar_t *name, DWORD v)
{
	HKEY key;

	if (RegCreateKeyExW(HKEY_CURRENT_USER, UC_REG_KEY, 0, NULL, 0,
	    KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
		return;
	RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE *)&v, sizeof (v));
	RegCloseKey(key);
}

static void
uc_reg_set_string(const wchar_t *name, const wchar_t *s)
{
	HKEY key;

	if (RegCreateKeyExW(HKEY_CURRENT_USER, UC_REG_KEY, 0, NULL, 0,
	    KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
		return;
	RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)s,
	    (DWORD)((wcslen(s) + 1) * sizeof (wchar_t)));
	RegCloseKey(key);
}

static void
uc_load_settings(void)
{
	HKEY key;
	DWORD v, len, type;

	g_enabled = TRUE;
	g_prerelease = FALSE;
	g_notified[0] = 0;

	if (RegOpenKeyExW(HKEY_CURRENT_USER, UC_REG_KEY, 0, KEY_READ,
	    &key) != ERROR_SUCCESS)
		return;

	len = sizeof (v);
	if (RegQueryValueExW(key, UC_REG_CHECK, NULL, &type, (LPBYTE)&v,
	    &len) == ERROR_SUCCESS && type == REG_DWORD)
		g_enabled = (v != 0);
	len = sizeof (v);
	if (RegQueryValueExW(key, UC_REG_PRE, NULL, &type, (LPBYTE)&v,
	    &len) == ERROR_SUCCESS && type == REG_DWORD)
		g_prerelease = (v != 0);

	len = sizeof (g_notified) - sizeof (wchar_t);
	if (RegQueryValueExW(key, UC_REG_NOTIFIED, NULL, &type,
	    (LPBYTE)g_notified, &len) != ERROR_SUCCESS || type != REG_SZ)
		g_notified[0] = 0;
	g_notified[UC_VER_MAX - 1] = 0;

	RegCloseKey(key);
}

/* ---- public ---- */

void
update_check_init(HWND hwnd, update_notify_fn notify)
{
	g_notify = notify;
	uc_load_settings();

	/* Its own one-shot, so it is not held up by the daily timer */
	SetTimer(hwnd, UC_TIMER_FIRST, UC_FIRST_MS, NULL);
	SetTimer(hwnd, UC_TIMER_DAILY, UC_DAILY_MS, NULL);
}

BOOL
update_check_timer(HWND hwnd, UINT_PTR id)
{
	if (id == UC_TIMER_FIRST) {
		KillTimer(hwnd, UC_TIMER_FIRST);
		uc_start_check(hwnd);
		return (TRUE);
	}
	if (id == UC_TIMER_DAILY) {
		uc_start_check(hwnd);
		return (TRUE);
	}
	return (FALSE);
}

void
update_check_done(HWND hwnd, LPARAM lParam)
{
	uc_result_t *res = (uc_result_t *)lParam;

	(void) hwnd;
	if (res == NULL)
		return;

	/* Unticked, or the settings changed since: a newer check is coming */
	if (!g_enabled || res->gen != g_gen) {
		free(res);
		return;
	}

	/* Failure: keep what is shown and try again at the next check */
	if (!res->ok) {
		free(res);
		return;
	}

	if (!res->have) {
		g_avail = FALSE;
		free(res);
		return;
	}

	g_avail = TRUE;
	wcscpy_s(g_avail_ver, UC_VER_MAX, res->ver);
	wcscpy_s(g_avail_url, UC_URL_MAX, res->url);

	/*
	 * Once per version, and only counted once it was really shown, so a
	 * refused notification is retried at the next check.
	 */
	if (wcscmp(g_notified, res->ver) != 0 && g_notify != NULL) {
		wchar_t text[200];

		_snwprintf_s(text, _countof(text), _TRUNCATE,
		    L"OpenZFS %s is available. You have %s.",
		    res->ver, res->installed);
		if (g_notify(L"OpenZFS update available", text)) {
			wcscpy_s(g_notified, UC_VER_MAX, res->ver);
			uc_reg_set_string(UC_REG_NOTIFIED, g_notified);
		}
	}
	free(res);
}

BOOL
update_check_enabled(void)
{
	return (g_enabled);
}

BOOL
update_check_prereleases(void)
{
	return (g_prerelease);
}

void
update_check_set_enabled(HWND hwnd, BOOL on)
{
	g_enabled = on ? TRUE : FALSE;
	uc_reg_set_dword(UC_REG_CHECK, g_enabled ? 1 : 0);

	/* Anything in flight is for the old setting */
	InterlockedIncrement(&g_gen);

	if (!g_enabled)
		g_avail = FALSE;	/* hide the item right away */
	else
		uc_start_check(hwnd);
}

void
update_check_set_prereleases(HWND hwnd, BOOL on)
{
	g_prerelease = on ? TRUE : FALSE;
	uc_reg_set_dword(UC_REG_PRE, g_prerelease ? 1 : 0);

	InterlockedIncrement(&g_gen);
	uc_start_check(hwnd);
}

BOOL
update_check_menu_text(wchar_t *buf, size_t cch)
{
	if (!g_enabled || !g_avail)
		return (FALSE);
	_snwprintf_s(buf, cch, _TRUNCATE, L"OpenZFS %s Is Available…",
	    g_avail_ver);
	return (TRUE);
}

void
update_check_open_page(void)
{
	static const wchar_t prefix[] =
	    L"https://github.com/openzfsonwindows/openzfs/";

	if (!g_avail)
		return;
	/* The URL came off the network: only ever open our own project */
	if (_wcsnicmp(g_avail_url, prefix, _countof(prefix) - 1) != 0)
		return;
	ShellExecuteW(NULL, L"open", g_avail_url, NULL, NULL, SW_SHOWNORMAL);
}
