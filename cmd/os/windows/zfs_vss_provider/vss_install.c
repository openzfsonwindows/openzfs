/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2025 Jorgen Lundman <lundman@lundman.net>
 */

/*
 * vss_install.c - Installer for the ZFS VSS Provider EXE.
 *
 * The ZFS VSS provider is an out-of-process COM server (LocalServer32 EXE).
 * VSS activates software providers (Type=2) via CLSCTX_LOCAL_SERVER, so
 * the provider must be registered as LocalServer32, not InprocServer32.
 * When VSS needs the provider, COM launches the EXE automatically.
 *
 * Registry layout written by /install:
 *
 *   HKLM\SOFTWARE\Classes\CLSID\{89300202-3CAE-4584-B38F-E72F2B3A2FBF}
 *     (Default)      = "OpenZFS VSS Provider"
 *     LocalServer32\(Default) = "<path>\zfs_vss_provider.exe"
 *
 *   HKLM\SYSTEM\CurrentControlSet\Services\VSS\Providers\
 *       {89300202-3CAE-4584-B38F-E72F2B3A2FBF}
 *     (Default)  = "OpenZFS VSS Provider"
 *     Type       = REG_DWORD 2   (VSS_PROV_SOFTWARE)
 *     Version    = "1.0"
 *     VersionId  = "{00000001-0000-0000-0001-000000000001}"
 *     CLSID\(Default) = "{89300202-3CAE-4584-B38F-E72F2B3A2FBF}"
 *
 * Usage (run as Administrator):
 *   zfs_vss_install.exe /install   <exe_path>
 *   zfs_vss_install.exe /uninstall
 */

#define	_CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>

#include "vss_provider.h"

#define	PROVIDER_VERSION_ID	L"{00000001-0000-0000-0001-000000000001}"
#define	VSS_PROV_SOFTWARE_TYPE	2

/* ------------------------------------------------------------------ */
/* Service name                                                         */
/* ------------------------------------------------------------------ */

#define	ZFS_VSS_SERVICE_NAME	L"ZfsVssProvider"
#define	ZFS_VSS_SERVICE_DISPLAY	L"OpenZFS VSS Provider"
#define	ZFS_VSS_SERVICE_DESC	L"Provides Volume Shadow Copy snapshots " \
	L"for ZFS datasets via the OpenZFS VSS provider."

/* ------------------------------------------------------------------ */
/* AppID registration                                                   */
/*                                                                      */
/* VSS uses CLSCTX_LOCAL_SERVER with AppID LocalService.  When COM      */
/* sees LocalService in the AppID, it starts the named service rather   */
/* than launching a bare EXE, so no LaunchPermission DACL is needed.   */
/* ------------------------------------------------------------------ */

static int
reg_appid(void)
{
	wchar_t key[256];
	HKEY hk;

	_snwprintf(key, 256,
	    L"SOFTWARE\\Classes\\AppID\\%s", ZFS_VSS_PROVIDER_GUID_STR);

	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, NULL,
	    REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hk, NULL)
	    != ERROR_SUCCESS) {
		fwprintf(stderr, L"Cannot create AppID key\n");
		return (-1);
	}

	RegSetValueExW(hk, NULL, 0, REG_SZ,
	    (const BYTE *)ZFS_VSS_PROVIDER_NAME,
	    (DWORD)((wcslen(ZFS_VSS_PROVIDER_NAME) + 1) *
	    sizeof (wchar_t)));

	/* LocalService tells COM to start our service instead of a bare EXE */
	RegSetValueExW(hk, L"LocalService", 0, REG_SZ,
	    (const BYTE *)ZFS_VSS_SERVICE_NAME,
	    (DWORD)((wcslen(ZFS_VSS_SERVICE_NAME) + 1) *
	    sizeof (wchar_t)));

	RegCloseKey(hk);
	return (0);
}

/* ------------------------------------------------------------------ */
/* Windows service registration                                         */
/* ------------------------------------------------------------------ */

static int
reg_service(const wchar_t *exe_path)
{
	SC_HANDLE hscm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
	if (!hscm) {
		fwprintf(stderr, L"OpenSCManager failed: %lu\n",
		    GetLastError());
		return (-1);
	}

	/* Delete any existing service first */
	SC_HANDLE hsvc = OpenServiceW(hscm, ZFS_VSS_SERVICE_NAME,
	    DELETE);
	if (hsvc) {
		DeleteService(hsvc);
		CloseServiceHandle(hsvc);
	}

	hsvc = CreateServiceW(hscm,
	    ZFS_VSS_SERVICE_NAME,		/* service name */
	    ZFS_VSS_SERVICE_DISPLAY,		/* display name */
	    SERVICE_ALL_ACCESS,
	    SERVICE_WIN32_OWN_PROCESS,		/* type */
	    SERVICE_DEMAND_START,		/* start type */
	    SERVICE_ERROR_NORMAL,
	    exe_path,				/* binary path */
	    NULL, NULL, L"RPCSS\0",		/* depends on RPCSS */
	    NULL, NULL);			/* LocalSystem account */

	if (!hsvc) {
		fwprintf(stderr, L"CreateService failed: %lu\n",
		    GetLastError());
		CloseServiceHandle(hscm);
		return (-1);
	}

	/* Set description */
	SERVICE_DESCRIPTIONW sd;
	sd.lpDescription = (LPWSTR)ZFS_VSS_SERVICE_DESC;
	ChangeServiceConfig2W(hsvc, SERVICE_CONFIG_DESCRIPTION, &sd);

	CloseServiceHandle(hsvc);
	CloseServiceHandle(hscm);
	return (0);
}

static int
unreg_service(void)
{
	SC_HANDLE hscm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
	if (!hscm)
		return (0);

	SC_HANDLE hsvc = OpenServiceW(hscm, ZFS_VSS_SERVICE_NAME,
	    SERVICE_STOP | DELETE);
	if (hsvc) {
		SERVICE_STATUS ss;
		ControlService(hsvc, SERVICE_CONTROL_STOP, &ss);
		DeleteService(hsvc);
		CloseServiceHandle(hsvc);
	}

	CloseServiceHandle(hscm);
	return (0);
}

/* ------------------------------------------------------------------ */
/* COM CLSID registration                                               */
/* ------------------------------------------------------------------ */

static int
reg_com_local_server(const wchar_t *exe_path)
{
	wchar_t key[256];
	HKEY hk, hk_srv;

	/* CLSID key */
	_snwprintf(key, 256,
	    L"SOFTWARE\\Classes\\CLSID\\%s", ZFS_VSS_PROVIDER_GUID_STR);
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, NULL,
	    REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hk, NULL)
	    != ERROR_SUCCESS) {
		fwprintf(stderr, L"Cannot create CLSID key\n");
		return (-1);
	}
	RegSetValueExW(hk, NULL, 0, REG_SZ,
	    (const BYTE *)ZFS_VSS_PROVIDER_NAME,
	    (DWORD)((wcslen(ZFS_VSS_PROVIDER_NAME) + 1) *
	    sizeof (wchar_t)));

	/* AppID value - links this CLSID to the AppID LaunchPermission */
	RegSetValueExW(hk, L"AppID", 0, REG_SZ,
	    (const BYTE *)ZFS_VSS_PROVIDER_GUID_STR,
	    (DWORD)((wcslen(ZFS_VSS_PROVIDER_GUID_STR) + 1) *
	    sizeof (wchar_t)));

	/* LocalServer32 subkey */
	if (RegCreateKeyExW(hk, L"LocalServer32", 0, NULL,
	    REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hk_srv, NULL)
	    != ERROR_SUCCESS) {
		fwprintf(stderr, L"Cannot create LocalServer32 key\n");
		RegCloseKey(hk);
		return (-1);
	}
	RegSetValueExW(hk_srv, NULL, 0, REG_SZ,
	    (const BYTE *)exe_path,
	    (DWORD)((wcslen(exe_path) + 1) * sizeof (wchar_t)));
	RegCloseKey(hk_srv);
	RegCloseKey(hk);
	return (0);
}

/* ------------------------------------------------------------------ */
/* VSS provider registration                                            */
/* ------------------------------------------------------------------ */

static int
reg_vss_provider(void)
{
	wchar_t key[256];
	HKEY hk, hk_clsid;
	DWORD type_val = VSS_PROV_SOFTWARE_TYPE;

	_snwprintf(key, 256,
	    L"SYSTEM\\CurrentControlSet\\Services\\VSS\\Providers\\%s",
	    ZFS_VSS_PROVIDER_GUID_STR);

	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, NULL,
	    REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hk, NULL)
	    != ERROR_SUCCESS) {
		fwprintf(stderr, L"Cannot create VSS provider key\n");
		return (-1);
	}

	RegSetValueExW(hk, NULL, 0, REG_SZ,
	    (const BYTE *)ZFS_VSS_PROVIDER_NAME,
	    (DWORD)((wcslen(ZFS_VSS_PROVIDER_NAME) + 1) *
	    sizeof (wchar_t)));
	RegSetValueExW(hk, L"Type", 0, REG_DWORD,
	    (const BYTE *)&type_val, sizeof (type_val));
	RegSetValueExW(hk, L"Version", 0, REG_SZ,
	    (const BYTE *)ZFS_VSS_PROVIDER_VERSION,
	    (DWORD)((wcslen(ZFS_VSS_PROVIDER_VERSION) + 1) *
	    sizeof (wchar_t)));
	RegSetValueExW(hk, L"VersionId", 0, REG_SZ,
	    (const BYTE *)PROVIDER_VERSION_ID,
	    (DWORD)((wcslen(PROVIDER_VERSION_ID) + 1) * sizeof (wchar_t)));

	/* CLSID must be a subkey whose default value is the CLSID string */
	if (RegCreateKeyExW(hk, L"CLSID", 0, NULL,
	    REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hk_clsid, NULL)
	    == ERROR_SUCCESS) {
		RegSetValueExW(hk_clsid, NULL, 0, REG_SZ,
		    (const BYTE *)ZFS_VSS_PROVIDER_GUID_STR,
		    (DWORD)((wcslen(ZFS_VSS_PROVIDER_GUID_STR) + 1) *
		    sizeof (wchar_t)));
		RegCloseKey(hk_clsid);
	}

	RegCloseKey(hk);
	return (0);
}

/* ------------------------------------------------------------------ */
/* Uninstall                                                            */
/* ------------------------------------------------------------------ */

static int
do_uninstall(void)
{
	wchar_t key[256];

	/* Stop and remove the Windows service */
	unreg_service();

	/* Remove LocalServer32 (and legacy InprocServer32) first */
	_snwprintf(key, 256,
	    L"SOFTWARE\\Classes\\CLSID\\%s\\LocalServer32",
	    ZFS_VSS_PROVIDER_GUID_STR);
	RegDeleteKeyW(HKEY_LOCAL_MACHINE, key);

	_snwprintf(key, 256,
	    L"SOFTWARE\\Classes\\CLSID\\%s\\InprocServer32",
	    ZFS_VSS_PROVIDER_GUID_STR);
	RegDeleteKeyW(HKEY_LOCAL_MACHINE, key);

	_snwprintf(key, 256,
	    L"SOFTWARE\\Classes\\CLSID\\%s",
	    ZFS_VSS_PROVIDER_GUID_STR);
	RegDeleteKeyW(HKEY_LOCAL_MACHINE, key);

	/* AppID key */
	_snwprintf(key, 256,
	    L"SOFTWARE\\Classes\\AppID\\%s",
	    ZFS_VSS_PROVIDER_GUID_STR);
	RegDeleteKeyW(HKEY_LOCAL_MACHINE, key);

	/* VSS provider CLSID subkey first, then parent */
	_snwprintf(key, 256,
	    L"SYSTEM\\CurrentControlSet\\Services\\VSS\\Providers\\%s\\CLSID",
	    ZFS_VSS_PROVIDER_GUID_STR);
	RegDeleteKeyW(HKEY_LOCAL_MACHINE, key);

	_snwprintf(key, 256,
	    L"SYSTEM\\CurrentControlSet\\Services\\VSS\\Providers\\%s",
	    ZFS_VSS_PROVIDER_GUID_STR);
	RegDeleteKeyW(HKEY_LOCAL_MACHINE, key);

	fwprintf(stdout, L"ZFS VSS Provider uninstalled.\n");
	return (0);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                          */
/* ------------------------------------------------------------------ */

int
wmain(int argc, wchar_t *argv[])
{
	if (argc >= 2 && _wcsicmp(argv[1], L"/uninstall") == 0)
		return (do_uninstall());

	if (argc >= 2 && _wcsicmp(argv[1], L"/install") == 0) {
		wchar_t exe_path[MAX_PATH];

		if (argc >= 3) {
			/* Explicit path provided */
			wcsncpy(exe_path, argv[2], MAX_PATH);
		} else {
			/*
			 * Default: same directory as this installer EXE,
			 * named zfs_vss_provider.exe.
			 */
			GetModuleFileNameW(NULL, exe_path, MAX_PATH);
			wchar_t *slash = wcsrchr(exe_path, L'\\');
			if (slash)
				wcscpy(slash + 1, L"zfs_vss_provider.exe");
		}

		fwprintf(stdout, L"Installing ZFS VSS Provider EXE: %s\n",
		    exe_path);

		if (reg_service(exe_path) != 0)
			return (1);
		if (reg_appid() != 0)
			return (1);
		if (reg_com_local_server(exe_path) != 0)
			return (1);
		if (reg_vss_provider() != 0)
			return (1);

		fwprintf(stdout,
		    L"ZFS VSS Provider installed successfully.\n");
		fwprintf(stdout,
		    L"Restart the VSS service to pick up the new provider:\n"
		    L"  net stop vss && net start vss\n");
		return (0);
	}

	fwprintf(stderr,
	    L"Usage: zfs_vss_install.exe /install [exe_path] | /uninstall\n");
	return (1);
}
