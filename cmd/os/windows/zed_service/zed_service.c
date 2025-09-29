
// zed_service.c — shared service/foreground runner
#define	_CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <sddl.h>
#include <stdio.h>
#include <stdint.h>

#include <libzfs.h>

#include "ops_status.h" // zed_status_json_build, etc.
// #include "rpc_dispatch.h" // your pipe dispatch function prototypes
#include "pipe_rpc.h"

// ---- service name
static const wchar_t kServiceName[] = L"OpenZFS_tray";

// ---- globals
static SERVICE_STATUS_HANDLE g_ScmHandle = NULL;
static SERVICE_STATUS g_SvcStatus = { 0 };
static HANDLE g_StopEvent = NULL;   // signaled to stop accept loop

static DWORD ClientWorker(HANDLE client);

// ---- debug print
#undef dprintf
static void
dprintf(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(buf, sizeof (buf), _TRUNCATE, fmt, ap);
	va_end(ap);
	OutputDebugStringA(buf);
}

// ---- SCM status helper
static void
ReportSvcStatus(DWORD state, DWORD win32Exit, DWORD waitHint)
{
	if (!g_ScmHandle)
		return;
	static DWORD checkPoint = 1;
	g_SvcStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	g_SvcStatus.dwCurrentState = state;
	g_SvcStatus.dwWin32ExitCode = win32Exit;
	g_SvcStatus.dwWaitHint = waitHint;
	g_SvcStatus.dwControlsAccepted = (state == SERVICE_START_PENDING) ? 0 :
	    SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
	g_SvcStatus.dwCheckPoint =
	    ((state == SERVICE_RUNNING) || (state == SERVICE_STOPPED)) ?
	    0 : checkPoint++;
	SetServiceStatus(g_ScmHandle, &g_SvcStatus);
}

// ---- SCM control handler
static DWORD WINAPI
SvcCtrlHandler(DWORD ctrl, DWORD, LPVOID, LPVOID)
{
	if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
		ReportSvcStatus(SERVICE_STOP_PENDING, NO_ERROR, 3000);
		if (g_StopEvent) SetEvent(g_StopEvent);
		return (NO_ERROR);
	}
	return (ERROR_CALL_NOT_IMPLEMENTED);
}

static int
get_names_cb(zpool_handle_t *zhp, void *cookie)
{
	(void) cookie;
	const char *name = zpool_get_name(zhp);
	if (name) printf("%s\n", name);
	zpool_close(zhp);
	return (0);
}

// Console runner for debugging: build JSON and print to stdout
static int
run_console(int argc, wchar_t **argv)
{
	// default: status-json
	int list_names = 0;
	for (int i = 1; i < argc; ++i) {
		if (wcscmp(argv[i], L"--list-names") == 0) list_names = 1;
		if (wcscmp(argv[i], L"--status-json") == 0) list_names = 0;
	}

	if (list_names) {
		// minimal libzfs iteration to print names only
		libzfs_handle_t *g = libzfs_init();
		if (!g) {
			fputs("libzfs_init failed\n", stderr);
			return (2);
		}
		zpool_iter(g, (zpool_iter_f *)get_names_cb, NULL);
		libzfs_fini(g);
		return (0);
	} else {
		size_t jlen = 0;
		char *json = zed_status_json_build(&jlen);
		if (!json) {
			fputs("zed_status_json_build failed\n", stderr);
			return (3);
		}
		fwrite(json, 1, jlen, stdout);
		fputc('\n', stdout);
		HeapFree(GetProcessHeap(), 0, json);
		return (0);
	}
}

// ---- security attrs for the pipe: Users R/W, Admins & System full
static BOOL
MakePipeSA(SECURITY_ATTRIBUTES *sa, PSECURITY_DESCRIPTOR *psdOut)
{
	static const wchar_t *sddl =
	    L"D:P"
	    L"(A;;GA;;;SY)"	// System: full
	    L"(A;;GA;;;BA)"	// Administrators: full
	    L"(A;;GRGW;;;BU)";	// Users: read/write
	PSECURITY_DESCRIPTOR sd = NULL;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl,
	    SDDL_REVISION_1, &sd, NULL))
		return (FALSE);
	sa->nLength = sizeof (*sa);
	sa->bInheritHandle = FALSE;
	sa->lpSecurityDescriptor = sd;
	*psdOut = sd;
	return (TRUE);
}

// ---- accept loop shared by service/foreground
static DWORD
RunPipeServerLoop(void)
{
	const wchar_t *pipeName = L"\\\\.\\pipe\\openzfs_zed";
	SECURITY_ATTRIBUTES sa;
	PSECURITY_DESCRIPTOR sd = NULL;
	if (!MakePipeSA(&sa, &sd))
		return (ERROR_ACCESS_DENIED);

	DWORD err = NO_ERROR;

	for (;;) {
		if (WaitForSingleObject(g_StopEvent, 0) == WAIT_OBJECT_0)
			break;

		HANDLE hPipe = CreateNamedPipeW(
		    pipeName,
		    // first instance ok; if you want multi, drop FIRST
		    PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
		    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
		    1, // max instances; bump if you want concurrency
		    64 * 1024, 64 * 1024, // out/in buffer
		    5000, // default timeout
		    &sa);
		if (hPipe == INVALID_HANDLE_VALUE) {
			err = GetLastError();
			dprintf("CreateNamedPipe failed: %lu\n", err);
			break;
		}

		dprintf("Waiting for client...\n");
		BOOL ok = ConnectNamedPipe(hPipe, NULL) ? TRUE :
		    (GetLastError() == ERROR_PIPE_CONNECTED);
		if (!ok) {
			err = GetLastError();
			CloseHandle(hPipe);
			if (err == ERROR_OPERATION_ABORTED)
				break;
			dprintf("ConnectNamedPipe failed: %lu\n", err);
			continue;
		}

		dprintf("Client connected\n");
		ClientWorker(hPipe);
		FlushFileBuffers(hPipe);
		DisconnectNamedPipe(hPipe);
		CloseHandle(hPipe);
		dprintf("Client disconnected\n");
	}

	if (sd)
		LocalFree(sd);
	return (err);
}

static BOOL
signal_handler(DWORD sig)
{
	if (sig == CTRL_C_EVENT || sig == CTRL_BREAK_EVENT ||
	    sig == CTRL_CLOSE_EVENT || sig == CTRL_LOGOFF_EVENT ||
	    sig == CTRL_SHUTDOWN_EVENT) {
		if (g_StopEvent) SetEvent(g_StopEvent);
		return (TRUE);
	}
	return (FALSE);
}

// ---- shared main (service or foreground)
static DWORD
ServiceMain_impl(BOOL is_service)
{
	DWORD rc = NO_ERROR;

	// stop event always exists
	g_StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!g_StopEvent)
		return (GetLastError());

	if (is_service) {
		ReportSvcStatus(SERVICE_START_PENDING, NO_ERROR, 3000);
	} else {
		// Console: Ctrl+C triggers stop event
		// SetConsoleCtrlHandler(signal_handler, TRUE);
	}

	if (is_service)
		ReportSvcStatus(SERVICE_RUNNING, NO_ERROR, 0);

	rc = RunPipeServerLoop();

	// shutdown
	if (g_StopEvent) {
		CloseHandle(g_StopEvent);
		g_StopEvent = NULL;
	}

	if (is_service)
		ReportSvcStatus(SERVICE_STOPPED, rc, 0);
	return (rc);
}

// ---- SCM entry point
static void WINAPI
ServiceMain(DWORD, LPWSTR *)
{
	g_ScmHandle = RegisterServiceCtrlHandlerExW(kServiceName,
	    SvcCtrlHandler, NULL);
	if (!g_ScmHandle)
		return;
	ZeroMemory(&g_SvcStatus, sizeof (g_SvcStatus));
	ReportSvcStatus(SERVICE_START_PENDING, NO_ERROR, 3000);
	(void) ServiceMain_impl(TRUE);
}

// ---- foreground runner (no SCM)
int
ServiceMainImpl_Foreground(void)
{
	dprintf("Running foreground server (Ctrl+C to stop)\n");
	return ((int)ServiceMain_impl(FALSE));
}

// ---- console helpers you already have (optional)
static int run_console(int argc, wchar_t **argv);

// ---- wmain: SCM vs --fg vs one-shot probes
int
wmain(int argc, wchar_t **argv)
{
	for (int i = 1; i < argc; ++i) {
		if (wcscmp(argv[i], L"--fg") == 0) {
			return (ServiceMainImpl_Foreground());
		}
		if (wcscmp(argv[i], L"--status-json") == 0 ||
		    wcscmp(argv[i], L"--list-names") == 0) {
			return (run_console(argc, argv));
		}
	}

	SERVICE_TABLE_ENTRYW table[] = {
	    { (LPWSTR)kServiceName, ServiceMain },
	    { NULL, NULL }
	};
	if (!StartServiceCtrlDispatcherW(table)) {
		if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
			// Not launched by SCM -> default to foreground server
			// (nicer than exiting)
			return (ServiceMainImpl_Foreground());
		}
		fprintf(stderr, "StartServiceCtrlDispatcherW failed: %lu\n",
		    GetLastError());
		return (1);
	}
	return (0);
}

static DWORD
ReadAll(HANDLE h, void *buf, DWORD need)
{
	DWORD got = 0, total = 0;
	BYTE *p = (BYTE *)buf;
	while (total < need) {
		if (!ReadFile(h, p + total, need - total, &got, NULL))
			return (GetLastError());
		if (got == 0)
			return (ERROR_BROKEN_PIPE);
		total += got;
	}
	return (0);
}


static DWORD
WriteAll(HANDLE h, const void *buf, DWORD len)
{
	DWORD put = 0, total = 0;
	const BYTE *p = (const BYTE *)buf;
	while (total < len) {
		if (!WriteFile(h, p + total, len - total, &put, NULL))
			return (GetLastError());
		total += put;
	}
	return (0);
}

static DWORD
ClientWorker(HANDLE client)
{
	req_hdr_t rh;
	DWORD err;
	err = ReadAll(client, &rh, sizeof (rh));
	if (err)
		return (err);
	if (rh.len > (16*1024*1024))
		return (ERROR_INVALID_DATA); // sanity

	BYTE *payload = NULL;
	if (rh.len) {
		payload = (BYTE*)HeapAlloc(GetProcessHeap(), 0, rh.len);
		if (!payload)
			return (ERROR_OUTOFMEMORY);
		err = ReadAll(client, payload, rh.len);
		if (err) {
			HeapFree(GetProcessHeap(), 0, payload);
			return (err);
		}
	}

	// Minimal dispatch: only GET_STATUS and SUBSCRIBE_EVENTS
	// are implemented here
	switch (rh.op) {
	case OP_GET_STATUS:
		{
			if (rh.len < sizeof (op_get_status_by_guid_req_t)) {
				rsp_hdr_t rsp = { ERROR_GEN_FAILURE, 0 };
				WriteAll(client, &rsp, sizeof (rsp));
				break;
			}
			const op_get_status_by_guid_req_t *req =
			    (const void *)payload;
			size_t jlen = 0;
			char *json =
			    zed_status_json_build_by_guid(req->guid,
			    (zfs_status_verbosity_t)req->verbosity, &jlen);
			if (!json) {
				rsp_hdr_t rsp = { ERROR_GEN_FAILURE, 0 };
				WriteAll(client, &rsp, sizeof (rsp));
			} else {
				rsp_hdr_t rsp = { 0, (uint32_t)jlen };
				WriteAll(client, &rsp, sizeof (rsp));
				WriteAll(client, json, (DWORD)jlen);
				HeapFree(GetProcessHeap(), 0, json);
			}
			err = 0;
		}
		break;
	case  OP_LIST_POOLS:
		{
			dprintf("OP_LIST_POOLS\n");
			size_t jlen = 0;
			char *json = zed_list_json_build(&jlen);
			if (!json) {
				rsp_hdr_t rsp = { ERROR_GEN_FAILURE, 0 };
				WriteAll(client, &rsp, sizeof (rsp));
			} else {
				rsp_hdr_t rsp = { 0, (uint32_t)jlen };
				WriteAll(client, &rsp, sizeof (rsp));
				WriteAll(client, json, (DWORD)jlen);
				HeapFree(GetProcessHeap(), 0, json);
			}
			return (0);
		}
		break;

	case OP_SUBSCRIBE_EVENTS:
		{
			err = 0;
		}
		break;
	default:
		{
			rsp_hdr_t rsp = { ERROR_CALL_NOT_IMPLEMENTED, 0 };
			WriteAll(client, &rsp, sizeof (rsp));
			err = 0;
		}
		break;
	}

	if (payload)
		HeapFree(GetProcessHeap(), 0, payload);
	return (err);
}
