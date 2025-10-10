// cmd/os/windows/zfs_tray/import_window.c
#define	_CRT_SECURE_NO_WARNINGS
#define	UNICODE
#define	_UNICODE
#define	_WIN32_WINNT 0x0600
#define	_WIN32_IE    0x0600
#include <windows.h>
#include <commctrl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define	JSMN_STATIC
#include "jsmn.h"

#include "pipe_rpc.h"
#include "rpc_client.h"
#include "import_window.h"

#pragma comment(lib, "comctl32.lib")

#ifndef ARRAYSIZE
#define	ARRAYSIZE(a) (sizeof (a)/sizeof ((a)[0]))
#endif

// ---- WM_APP messages
#define	WM_APP_SCAN_DONE (WM_APP + 100)
#define	WM_APP_IMPORT_DONE (WM_APP + 101)

// ---- controls
enum {
    IDC_LBL_STATUS = 1001,
    IDC_LIST = 1002,
    IDC_CHK_FORCE = 1003,
    IDC_CHK_RO = 1004,
    IDC_CHK_NOMNT = 1005,
    IDC_ED_ALTROOT = 1006,
    IDC_BTN_IMPORT = 1007,
    IDC_BTN_CLOSE = 1008,
};

// ---- data types
typedef struct {
    wchar_t  name[64];
    uint64_t guid;
    wchar_t  state[32];
} Candidate;

typedef struct {
    Candidate *items;
    int count;
} CandidateList;

typedef struct {
    HWND   hWnd;
    zrpc_t *rpc;
    HANDLE hScanThread;
} ImportCtx;

// Parse: { "candidates":[ {"name":"...", "guid":"...", "state":"..."} ... ] }
static CandidateList *
ParseScanJSON(const char *json, int json_len)
{
	jsmn_parser p;
	jsmn_init(&p);
	jsmntok_t tok[2048];
	int r = jsmn_parse(&p, json, json_len, tok, (int)ARRAYSIZE(tok));
	if (r < 0)
		return (NULL);

	// find candidates array
	int arr = -1;
	for (int i = 1; i < r-1; ++i) {
		if (tok[i].type == JSMN_STRING && tok[i+1].type == JSMN_ARRAY) {
			int klen = tok[i].end - tok[i].start;
			if (klen == 10 &&
			    memcmp(json + tok[i].start, "candidates",
			    10) == 0) {
				arr = i + 1;
				break;
			}
		}
	}
	if (arr < 0)
		return (NULL);

	int elems = tok[arr].size;
	CandidateList *cl = (CandidateList *)HeapAlloc(GetProcessHeap(),
	    HEAP_ZERO_MEMORY, sizeof (*cl));
	if (!cl)
		return (NULL);
	cl->items = (Candidate *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
	    elems * sizeof (Candidate));
	if (!cl->items) {
		HeapFree(GetProcessHeap(), 0, cl);
		return (NULL);
	}
	cl->count = 0;

	int idx = arr + 1;
	for (int e = 0; e < elems && idx < r; ++e) {
		if (tok[idx].type != JSMN_OBJECT) {
			int end = tok[idx].end;
			idx++;
			while (idx < r && tok[idx].start < end) idx++;
			continue;
		}
		int obj = idx++, end = tok[obj].end;
		int name_v = -1, guid_v = -1, state_v = -1;

		while (idx + 1 < r && tok[idx].start < end) {
			jsmntok_t *k = &tok[idx], *v = &tok[idx+1];
			if (k->type == JSMN_STRING) {
				int klen = k->end - k->start;
				const char *ks = json + k->start;
				if (klen == 4 && memcmp(ks,
				    "name", 4) == 0)
					name_v = idx + 1;
				else if (klen == 4 && memcmp(ks,
				    "guid", 4) == 0)
					guid_v = idx + 1;
				else if (klen == 5 && memcmp(ks,
				    "state", 5) == 0)
					state_v = idx + 1;
			}
			int vend = v->end;
			idx += 2;
			while (idx < r && tok[idx].start < vend) idx++;
		}

		Candidate *c = &cl->items[cl->count];
		if (name_v > 0 && tok[name_v].type == JSMN_STRING) {
			int len = tok[name_v].end - tok[name_v].start;
			MultiByteToWideChar(CP_UTF8, 0,
			    json+tok[name_v].start, len, c->name,
			    (int)ARRAYSIZE(c->name)-1);
		}
		if (guid_v > 0 && (tok[guid_v].type == JSMN_STRING ||
		    tok[guid_v].type == JSMN_PRIMITIVE)) {
			int len = tok[guid_v].end - tok[guid_v].start;
			c->guid = parse_u64_from_utf8(json + tok[guid_v].start,
			    len);
		}
		if (state_v > 0 && tok[state_v].type == JSMN_STRING) {
			int len = tok[state_v].end - tok[state_v].start;
			MultiByteToWideChar(CP_UTF8, 0,
			    json + tok[state_v].start, len, c->state,
			    (int)ARRAYSIZE(c->state)-1);
		}
		if (c->name[0] || c->guid) cl->count++;
	}
	return (cl);
}

static void
FreeCandidateList(CandidateList *cl)
{
	if (!cl)
		return;
	if (cl->items)
		HeapFree(GetProcessHeap(), 0, cl->items);
	HeapFree(GetProcessHeap(), 0, cl);
}

// ---- worker: scan thread
static DWORD WINAPI
ScanThread(LPVOID param)
{
	ImportCtx *ctx = (ImportCtx*)param;

	op_import_scan_req_t rq = {0};
	uint8_t *out = NULL;
	uint32_t st = 0, outlen = 0;

	if (zrpc_call(ctx->rpc, OP_IMPORT_SCAN, &rq, sizeof (rq), &st,
	    &out, &outlen) && st == 0 && out) {
		CandidateList *cl = ParseScanJSON((const char *)out,
		    (int)outlen);
		HeapFree(GetProcessHeap(), 0, out);
		PostMessageW(ctx->hWnd, WM_APP_SCAN_DONE, 0, (LPARAM)cl);
	} else {
		PostMessageW(ctx->hWnd, WM_APP_SCAN_DONE, 0, (LPARAM)NULL);
	}
	return (0);
}

// ---- UI helpers
static void
SetStatus(HWND hWnd, LPCWSTR s)
{
	SetDlgItemTextW(hWnd, IDC_LBL_STATUS, s);
}

static void
AddListColumns(HWND hList)
{
	LVCOLUMNW col = {0};
	col.mask = LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
	col.pszText = L"Name";
	col.cx = 180;
	col.iSubItem = 0;
	ListView_InsertColumn(hList, 0, &col);
	col.pszText = L"GUID";
	col.cx = 180;
	col.iSubItem = 1;
	ListView_InsertColumn(hList, 1, &col);
	col.pszText = L"State";
	col.cx = 100;
	col.iSubItem = 2;
	ListView_InsertColumn(hList, 2, &col);
}

static void
PopulateList(HWND hList, const CandidateList *cl)
{
	ListView_DeleteAllItems(hList);
	if (!cl || cl->count == 0)
		return;

	for (int i = 0; i < cl->count; i++) {
		const Candidate *c = &cl->items[i];
		LVITEMW it = {0};
		it.mask = LVIF_TEXT | LVIF_PARAM;
		it.iItem = i;
		it.pszText = (LPWSTR)c->name;
		it.lParam = (LPARAM)c->guid; // stash guid
		int idx = ListView_InsertItem(hList, &it);

		wchar_t gbuf[32];
		_snwprintf_s(gbuf, ARRAYSIZE(gbuf), _TRUNCATE, L"%llu",
		    (unsigned long long)c->guid);
		ListView_SetItemText(hList, idx, 1, gbuf);
		ListView_SetItemText(hList, idx, 2,
		    (LPWSTR)(c->state[0] ? c->state : L""));
	}
}

static uint64_t
GetSelectedGuid(HWND hList)
{
	int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
	if (sel < 0)
		return (0);
	LVITEMW it = {0};
	it.mask = LVIF_PARAM;
	it.iItem = sel;
	if (ListView_GetItem(hList, &it))
		return ((uint64_t)it.lParam);
	return (0);
}

static DWORD
ComposeImportOneBody(uint8_t *buf, DWORD cap,
    uint32_t flags, uint64_t guid,
    const wchar_t *newNameW, const wchar_t *altrootW)
{
	op_import_one_req_t *rq = (op_import_one_req_t *)buf;
	if (cap < sizeof (*rq))
		return (0);
	rq->flags = flags;
	rq->guid  = guid;

	DWORD off = sizeof (*rq);
	// new_name (optional)
	if (newNameW && newNameW[0]) {
		int n = WideCharToMultiByte(CP_UTF8, 0, newNameW, -1,
		    (char *)buf + off, (int)(cap - off), NULL, NULL);
		if (n <= 0)
			return (0);
		off += (DWORD)n;
	} else {
		((char *)buf)[off++] = '\0';
	}
	// altroot (optional)
	if (altrootW && altrootW[0]) {
		int n = WideCharToMultiByte(CP_UTF8, 0, altrootW, -1,
		    (char *)buf + off, (int)(cap - off), NULL, NULL);
		if (n <= 0)
			return (0);
		off += (DWORD)n;
	} else {
		((char *)buf)[off++] = '\0';
	}
	return (off);
}

// ---- Import window proc
static LRESULT CALLBACK
ImportWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	ImportCtx *ctx = (ImportCtx *)GetWindowLongPtrW(hWnd, GWLP_USERDATA);

	switch (msg) {
	case WM_CREATE: {
		INITCOMMONCONTROLSEX icc = { sizeof (icc),
		    ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
		InitCommonControlsEx(&icc);

		ctx = (ImportCtx *)((CREATESTRUCTW *)lParam)->lpCreateParams;
		ctx->hWnd = hWnd;
		SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)ctx);

		// layout(
		CreateWindow(TEXT("STATIC"),
		    TEXT("Scanning for importable pools…"),
		    WS_CHILD | WS_VISIBLE,
		    12, 10, 360, 18, hWnd, (HMENU)IDC_LBL_STATUS, NULL, NULL);

		HWND hList = CreateWindow(WC_LISTVIEW, TEXT(""),
		    WS_CHILD|WS_VISIBLE|LVS_REPORT|LVS_SINGLESEL|WS_BORDER,
		    12, 32, 460, 200, hWnd, (HMENU)IDC_LIST, NULL, NULL);
		ListView_SetExtendedListViewStyle(hList,
		    LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
		AddListColumns(hList);

		CreateWindow(TEXT("BUTTON"), TEXT("Force"),
		    WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
		    12, 238, 70, 20, hWnd, (HMENU)IDC_CHK_FORCE, NULL, NULL);
		CreateWindow(TEXT("BUTTON"), TEXT("Read-only"),
		    WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
		    90, 238, 90, 20, hWnd, (HMENU)IDC_CHK_RO, NULL, NULL);
		CreateWindow(TEXT("BUTTON"), TEXT("No-mount"),
		    WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
		    190, 238, 90, 20, hWnd, (HMENU)IDC_CHK_NOMNT, NULL, NULL);

		CreateWindow(TEXT("STATIC"), TEXT("Altroot:"),
		    WS_CHILD|WS_VISIBLE,
		    12, 264, 60, 18, hWnd, 0, NULL, NULL);
		CreateWindow(TEXT("EDIT"), TEXT(""),
		    WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL,
		    72, 262, 220, 22, hWnd, (HMENU)IDC_ED_ALTROOT, NULL, NULL);

		CreateWindow(TEXT("BUTTON"), TEXT("Import"),
		    WS_CHILD|WS_VISIBLE|WS_DISABLED,
		    308, 258, 80, 26, hWnd, (HMENU)IDC_BTN_IMPORT, NULL, NULL);
		CreateWindow(TEXT("BUTTON"), TEXT("Close"),
		    WS_CHILD|WS_VISIBLE,
		    392, 258, 80, 26, hWnd, (HMENU)IDC_BTN_CLOSE, NULL, NULL);

		// kick off scan
		ctx->hScanThread = CreateThread(NULL, 0, ScanThread, ctx, 0,
		    NULL);
		return (0);
	}

	case WM_NOTIFY: {
		LPNMHDR nh = (LPNMHDR)lParam;
		if (nh->idFrom == IDC_LIST && (nh->code == LVN_ITEMCHANGED)) {
			NMLISTVIEW *lv = (NMLISTVIEW*)lParam;
			if ((lv->uChanged & LVIF_STATE) &&
			    (lv->uNewState & LVIS_SELECTED)) {
				EnableWindow(GetDlgItem(hWnd, IDC_BTN_IMPORT),
				    TRUE);
			}
		}
		return (0);
	}

	case WM_COMMAND: {
		switch (LOWORD(wParam)) {
		case IDC_BTN_CLOSE:
			DestroyWindow(hWnd);
			return (0);

		case IDC_BTN_IMPORT:
			{
			HWND hList = GetDlgItem(hWnd, IDC_LIST);
			uint64_t guid = GetSelectedGuid(hList);
			if (!guid) {
				MessageBoxW(hWnd,
				    L"Select a pool to import.", L"OpenZFS",
				    MB_OK|MB_ICONINFORMATION);
				return (0);
			}

			uint32_t flags = 0;
			if (SendMessageW(GetDlgItem(hWnd, IDC_CHK_FORCE),
			    BM_GETCHECK, 0, 0) == BST_CHECKED)
				flags |= ZIMP_FORCE;
			if (SendMessageW(GetDlgItem(hWnd, IDC_CHK_RO),
			    BM_GETCHECK, 0, 0) == BST_CHECKED)
				flags |= ZIMP_READONLY;
			if (SendMessageW(GetDlgItem(hWnd, IDC_CHK_NOMNT),
			    BM_GETCHECK, 0, 0) == BST_CHECKED)
				flags |= ZIMP_NOMOUNT;

			wchar_t altrootW[260];
			GetDlgItemTextW(hWnd, IDC_ED_ALTROOT, altrootW,
			    ARRAYSIZE(altrootW));

			uint8_t body[1024];
			DWORD blen = ComposeImportOneBody(body, sizeof (body),
			    flags, guid, /* new_name */ NULL, altrootW);
			if (!blen) {
				MessageBoxW(hWnd, L"Invalid parameters.",
				    L"OpenZFS", MB_OK|MB_ICONERROR);
				return (0);
			}

			// Disable Import while working
			EnableWindow(GetDlgItem(hWnd, IDC_BTN_IMPORT), FALSE);
			SetStatus(hWnd, L"Importing…");

			uint8_t *out = NULL;
			uint32_t st = 0, outlen = 0;
			BOOL ok = zrpc_call(ctx->rpc, OP_IMPORT_ONE, body, blen,
			    &st, &out, &outlen) && st == 0 && out;
			wchar_t *msgW = NULL;
			if (ok) {
				msgW = (wchar_t *)HeapAlloc(GetProcessHeap(), 0,
				    (outlen + 1) * 2);
				int wn = MultiByteToWideChar(CP_UTF8, 0,
				    (const char *)out, (int)outlen, msgW,
				    outlen);
				if (wn >= 0) {
					msgW[wn] = 0;
				} else {
					wcscpy_s(msgW, 16, L"OK");
				}
				HeapFree(GetProcessHeap(), 0, out);
				PostMessageW(hWnd, WM_APP_IMPORT_DONE, TRUE,
				    (LPARAM)msgW);
			} else {

				if (out) HeapFree(GetProcessHeap(), 0, out);
				const wchar_t *fallback =
				    L"Import failed or access denied.";
				size_t bytes = (wcslen(fallback) + 1) *
				    sizeof (wchar_t);
				msgW = (wchar_t *)HeapAlloc(GetProcessHeap(), 0,
				    bytes);
				if (msgW)
					wcscpy_s(msgW, bytes / sizeof (wchar_t),
					    fallback);
				PostMessageW(hWnd, WM_APP_IMPORT_DONE, FALSE,
				    (LPARAM)msgW);
			}
			return (0);
			}
		}
		return (0); // COMMAND
	}

	case WM_APP_SCAN_DONE: {
		CandidateList *cl = (CandidateList*)lParam;
		HWND hList = GetDlgItem(hWnd, IDC_LIST);
		if (!cl || cl->count == 0) {
			SetStatus(hWnd, L"No importable pools found.");
		} else {
			SetStatus(hWnd, L"Select a pool and click Import.");
			PopulateList(hList, cl);
		}
		FreeCandidateList(cl);
		return (0);
		}

	case WM_APP_IMPORT_DONE: {
		BOOL ok = (BOOL)wParam;
		wchar_t *msg = (wchar_t *)lParam;
		MessageBoxW(hWnd, msg ? msg : (ok ? L"Import completed." :
		    L"Import failed."), L"OpenZFS",
		    MB_OK | (ok?MB_ICONINFORMATION:MB_ICONERROR));
		if (msg) HeapFree(GetProcessHeap(), 0, msg);
		SetStatus(hWnd, L"Ready.");
		EnableWindow(GetDlgItem(hWnd, IDC_BTN_IMPORT), TRUE);
		return (0);
		}

	case WM_CLOSE:
		DestroyWindow(hWnd);
		return (0);

	case WM_DESTROY:
		if (ctx) {
			if (ctx->hScanThread) {
				CloseHandle(ctx->hScanThread);
				ctx->hScanThread = NULL;
			}
			HeapFree(GetProcessHeap(), 0, ctx);
			SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
		}
		return (0);
	}
	return (DefWindowProcW(hWnd, msg, wParam, lParam));
}

// ---- public entry
HWND
CreateImportWindow(HWND hParent, zrpc_t *rpc)
{
	static const wchar_t *kClass = L"OpenZFS_ImportWnd";
	static ATOM s_atom = 0;
	if (!s_atom) {
		WNDCLASSEXW wc = { sizeof (wc) };
		wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
		wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
		wc.hIcon   = LoadIconW(NULL, IDI_INFORMATION);
		wc.hInstance = GetModuleHandleW(NULL);
		wc.lpfnWndProc = ImportWndProc;
		wc.lpszClassName = kClass;
		s_atom = RegisterClassExW(&wc);
	}
	ImportCtx *ctx = (ImportCtx*)HeapAlloc(GetProcessHeap(),
	    HEAP_ZERO_MEMORY, sizeof (*ctx));
	if (!ctx)
		return (NULL);
	ctx->rpc = rpc;

	HWND hWnd = CreateWindowExW(WS_EX_TOOLWINDOW, kClass,
	    L"OpenZFS — Import",
	    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
	    CW_USEDEFAULT, CW_USEDEFAULT, 500, 330,
	    hParent, NULL, GetModuleHandleW(NULL), ctx);
	if (hWnd) ShowWindow(hWnd, SW_SHOW);
	return (hWnd);
}
