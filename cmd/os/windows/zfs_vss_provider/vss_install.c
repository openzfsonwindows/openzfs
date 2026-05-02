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

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <wow64apiset.h>

#include "vss_provider.h"

#define PROVIDER_VERSION_ID L"{00000001-0000-0000-0001-000000000001}"
#define VSS_PROV_SOFTWARE_TYPE 2

/* ------------------------------------------------------------------ */
/* COM CLSID registration (InprocServer32)                             */
/* ------------------------------------------------------------------ */
static int
reg_com_inproc(const wchar_t *dll_path)
{
    wchar_t key[512];
    HKEY hk = NULL, hk_inproc = NULL, hk_appid = NULL;
    LSTATUS status;

    /* 1. Register CLSID */
    _snwprintf(key, 512, L"Software\\Classes\\CLSID\\%s", ZFS_VSS_PROVIDER_GUID_STR);
    status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, NULL,
        REG_OPTION_NON_VOLATILE, KEY_WRITE | KEY_WOW64_64KEY,
        NULL, &hk, NULL); 

    if (status != ERROR_SUCCESS) return (-1);

    /* Set (Default) Provider Name */
    RegSetValueExW(hk, NULL, 0, REG_SZ, (const BYTE *)ZFS_VSS_PROVIDER_NAME,
        (DWORD)((wcslen(ZFS_VSS_PROVIDER_NAME) + 1) * sizeof(wchar_t))); 

    /* Link to AppID */
    RegSetValueExW(hk, L"AppID", 0, REG_SZ, (const BYTE *)ZFS_VSS_PROVIDER_GUID_STR,
        (DWORD)((wcslen(ZFS_VSS_PROVIDER_GUID_STR) + 1) * sizeof(wchar_t)));

    /* 2. RESTORED: InprocServer32 and DLLPath */
    status = RegCreateKeyExW(hk, L"InprocServer32", 0, NULL,
        REG_OPTION_NON_VOLATILE, KEY_WRITE | KEY_WOW64_64KEY,
        NULL, &hk_inproc, NULL);

    if (status == ERROR_SUCCESS) {
        /* Set the DLL path as the (Default) value */
        RegSetValueExW(hk_inproc, NULL, 0, REG_SZ, (const BYTE *)dll_path,
            (DWORD)((wcslen(dll_path) + 1) * sizeof(wchar_t))); 
        
        RegSetValueExW(hk_inproc, L"ThreadingModel", 0, REG_SZ, (const BYTE *)L"Both", 
            (DWORD)(5 * sizeof(wchar_t))); 
        RegCloseKey(hk_inproc);
    }
    RegCloseKey(hk);

    /* 3. Register AppID Key */
    _snwprintf(key, 512, L"Software\\Classes\\AppID\\%s", ZFS_VSS_PROVIDER_GUID_STR);
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, NULL,
        REG_OPTION_NON_VOLATILE, KEY_WRITE | KEY_WOW64_64KEY,
        NULL, &hk_appid, NULL) == ERROR_SUCCESS) {
        
        RegSetValueExW(hk_appid, NULL, 0, REG_SZ, (const BYTE *)ZFS_VSS_PROVIDER_NAME,
            (DWORD)((wcslen(ZFS_VSS_PROVIDER_NAME) + 1) * sizeof(wchar_t)));
        
        /* Empty DllSurrogate is required */
        RegSetValueExW(hk_appid, L"DllSurrogate", 0, REG_SZ, (const BYTE *)L"", 2);

	/* Inside reg_com_inproc - The Security Fix */
	_snwprintf(key, 512, L"Software\\Classes\\AppID\\%s", ZFS_VSS_PROVIDER_GUID_STR);
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, NULL,
	    REG_OPTION_NON_VOLATILE, KEY_WRITE | KEY_WOW64_64KEY,
	    NULL, &hk_appid, NULL) == ERROR_SUCCESS) {

	    /* Set name */
	    RegSetValueExW(hk_appid, NULL, 0, REG_SZ, (const BYTE *)ZFS_VSS_PROVIDER_NAME,
		(DWORD)((wcslen(ZFS_VSS_PROVIDER_NAME) + 1) * sizeof(wchar_t)));

	    /* Required for DLL hosting */
	    RegSetValueExW(hk_appid, L"DllSurrogate", 0, REG_SZ, (const BYTE *)L"", 2);

	    /* NEW: Grant permissions to avoid 0x80070005 */
	    const wchar_t *runas = L"Interactive User";
	    RegSetValueExW(hk_appid, L"RunAs", 0, REG_SZ, (const BYTE *)runas,
		(DWORD)((wcslen(runas) + 1) * sizeof(wchar_t)));

	    DWORD auth_level = 1; // RPC_C_AUTHN_LEVEL_NONE
	    RegSetValueExW(hk_appid, L"AuthenticationLevel", 0, REG_DWORD, (const BYTE *)&auth_level, sizeof(DWORD));
	}

	RegCloseKey(hk_appid);
    }

    return (status == ERROR_SUCCESS ? 0 : -1);
}

static int
reg_vss_provider(void)
{
    wchar_t key[512];
    HKEY hk, hk_clsid, hk_access;
    DWORD type_val = VSS_PROV_SOFTWARE_TYPE;
    DWORD allow = 1;

    /* 1. Standard Provider Entry */
    _snwprintf(key, 512, L"SYSTEM\\CurrentControlSet\\Services\\VSS\\Providers\\%s", ZFS_VSS_PROVIDER_GUID_STR);
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, NULL, REG_OPTION_NON_VOLATILE,
	KEY_WRITE | KEY_WOW64_64KEY, NULL, &hk, NULL) != ERROR_SUCCESS) return (-1);

    RegSetValueExW(hk, NULL, 0, REG_SZ, (const BYTE *)ZFS_VSS_PROVIDER_NAME,
	(DWORD)((wcslen(ZFS_VSS_PROVIDER_NAME) + 1) * sizeof(wchar_t)));

    /* RESTORED: These must exist for vssadmin to function */
    RegSetValueExW(hk, L"Version", 0, REG_SZ, (const BYTE *)ZFS_VSS_PROVIDER_VERSION,
	(DWORD)((wcslen(ZFS_VSS_PROVIDER_VERSION) + 1) * sizeof(wchar_t)));
    RegSetValueExW(hk, L"VersionId", 0, REG_SZ, (const BYTE *)PROVIDER_VERSION_ID,
	(DWORD)((wcslen(PROVIDER_VERSION_ID) + 1) * sizeof(wchar_t)));

    RegSetValueExW(hk, L"Type", 0, REG_DWORD, (const BYTE *)&type_val, sizeof(type_val));

    if (RegCreateKeyExW(hk, L"CLSID", 0, NULL, REG_OPTION_NON_VOLATILE,
	KEY_WRITE | KEY_WOW64_64KEY, NULL, &hk_clsid, NULL) == ERROR_SUCCESS) {
	RegSetValueExW(hk_clsid, NULL, 0, REG_SZ, (const BYTE *)ZFS_VSS_PROVIDER_GUID_STR,
	    (DWORD)((wcslen(ZFS_VSS_PROVIDER_GUID_STR) + 1) * sizeof(wchar_t)));
	RegCloseKey(hk_clsid);
    }
    RegCloseKey(hk);

    /* 2. Access Control Fix */
    /* If the "NT AUTHORITY\SYSTEM" string is causing issues, try the SID: "S-1-5-18" */
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\VSS\\VssAccessControl",
	0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hk_access, NULL) == ERROR_SUCCESS) {

	/* Use a very clean approach for the name */
	const wchar_t *system_account = L"NT AUTHORITY\\SYSTEM";
	DWORD allow_val = 1;

	RegSetValueExW(hk_access, system_account, 0, REG_DWORD,
	    (const BYTE *)&allow_val, sizeof(DWORD));

	RegCloseKey(hk_access);
    }

    return (0);
}

/* ------------------------------------------------------------------ */
/* Uninstall                                                            */
/* ------------------------------------------------------------------ */
static int
do_uninstall(void)
{
    wchar_t key[256];

    _snwprintf(key, 256,
        L"SOFTWARE\\Classes\\CLSID\\%s\\InprocServer32",
        ZFS_VSS_PROVIDER_GUID_STR);
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, key, KEY_WOW64_64KEY, 0);

    _snwprintf(key, 256,
        L"SOFTWARE\\Classes\\CLSID\\%s\\LocalServer32",
        ZFS_VSS_PROVIDER_GUID_STR);
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, key, KEY_WOW64_64KEY, 0);

    _snwprintf(key, 256,
        L"SOFTWARE\\Classes\\CLSID\\%s",
        ZFS_VSS_PROVIDER_GUID_STR);
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, key, KEY_WOW64_64KEY, 0);

    _snwprintf(key, 256,
        L"SOFTWARE\\Classes\\AppID\\%s",
        ZFS_VSS_PROVIDER_GUID_STR);
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, key, KEY_WOW64_64KEY, 0);

    _snwprintf(key, 256,
        L"SYSTEM\\CurrentControlSet\\Services\\VSS\\Providers\\%s\\CLSID",
        ZFS_VSS_PROVIDER_GUID_STR);
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, key, KEY_WOW64_64KEY, 0);

    _snwprintf(key, 256,
        L"SYSTEM\\CurrentControlSet\\Services\\VSS\\Providers\\%s",
        ZFS_VSS_PROVIDER_GUID_STR);
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, key, KEY_WOW64_64KEY, 0);

    fwprintf(stdout, L"ZFS VSS Provider uninstalled.\n");
    return (0);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                          */
/* ------------------------------------------------------------------ */
int
wmain(int argc, wchar_t *argv[])
{
    PVOID old_value = NULL;
    BOOL disabled_redirection = FALSE;

    if (argc >= 2 && _wcsicmp(argv[1], L"/uninstall") == 0)
        return (do_uninstall());

    if (argc >= 2 && _wcsicmp(argv[1], L"/install") == 0) {
        wchar_t dll_path[MAX_PATH];

        if (argc >= 3) {
            wcsncpy(dll_path, argv[2], MAX_PATH);
        } else {
            GetModuleFileNameW(NULL, dll_path, MAX_PATH);
            wchar_t *slash = wcsrchr(dll_path, L'\\');
            if (slash)
                wcscpy(slash + 1, L"zfs_vss_provider.dll");
        }

        fwprintf(stdout, L"Installing ZFS VSS Provider DLL: %s\n", dll_path);

        /* Safely attempt to disable redirection (will fail gracefully if already 64-bit native) */
        disabled_redirection = Wow64DisableWow64FsRedirection(&old_value);

        if (reg_com_inproc(dll_path) != 0) {
            if (disabled_redirection) Wow64RevertWow64FsRedirection(old_value);
            return (1);
        }
        
        if (reg_vss_provider() != 0) {
            if (disabled_redirection) Wow64RevertWow64FsRedirection(old_value);
            return (1);
        }

        if (disabled_redirection) {
            Wow64RevertWow64FsRedirection(old_value);
        }

        fwprintf(stdout, L"ZFS VSS Provider installed successfully.\n");
        fwprintf(stdout, L"Restart the VSS service to pick up the new provider:\n"
                         L"  net stop vss && net start vss\n");
        return (0);
    }

    fwprintf(stderr, L"Usage: zfs_vss_install.exe /install [dll_path] | /uninstall\n");
    return (1);
}
