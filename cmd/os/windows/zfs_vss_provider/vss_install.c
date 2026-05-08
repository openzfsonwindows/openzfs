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

#define	_CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <sddl.h>
#include <stdio.h>
#include <wow64apiset.h>

#include "vss_provider.h"

#define	PROVIDER_VERSION_ID	L"{00000001-0000-0000-0001-000000000001}"
#define	VSS_PROV_SOFTWARE_TYPE	2

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
	_snwprintf(key, 512, L"Software\\Classes\\CLSID\\%s",
	    ZFS_VSS_PROVIDER_GUID_STR);
	status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, NULL,
	    REG_OPTION_NON_VOLATILE, KEY_WRITE | KEY_WOW64_64KEY,
	    NULL, &hk, NULL);
	if (status != ERROR_SUCCESS)
		return (-1);

	RegSetValueExW(hk, NULL, 0, REG_SZ,
	    (const BYTE *)ZFS_VSS_PROVIDER_NAME,
	    (DWORD)((wcslen(ZFS_VSS_PROVIDER_NAME) + 1) * sizeof (wchar_t)));
	RegSetValueExW(hk, L"AppID", 0, REG_SZ,
	    (const BYTE *)ZFS_VSS_PROVIDER_GUID_STR,
	    (DWORD)((wcslen(ZFS_VSS_PROVIDER_GUID_STR) + 1) * sizeof (wchar_t)));

	/* 2. InprocServer32 subkey */
	status = RegCreateKeyExW(hk, L"InprocServer32", 0, NULL,
	    REG_OPTION_NON_VOLATILE, KEY_WRITE | KEY_WOW64_64KEY,
	    NULL, &hk_inproc, NULL);
	if (status == ERROR_SUCCESS) {
		RegSetValueExW(hk_inproc, NULL, 0, REG_SZ,
		    (const BYTE *)dll_path,
		    (DWORD)((wcslen(dll_path) + 1) * sizeof (wchar_t)));
		RegSetValueExW(hk_inproc, L"ThreadingModel", 0, REG_SZ,
		    (const BYTE *)L"Both", (DWORD)(5 * sizeof (wchar_t)));
		RegCloseKey(hk_inproc);
	}
	RegCloseKey(hk);

	/*
	 * 3. AppID key.
	 * DllSurrogate="" is required: vssvc.exe activates the provider via
	 * out-of-process COM, so without a surrogate entry the CLSID is
	 * seen as unregistered (0x80040154).
	 *
	 * RunAs="NT AUTHORITY\\NetworkService": without an explicit RunAs,
	 * RPCSS cannot impersonate the caller (NETWORK SERVICE) at the
	 * level required to launch a new process, giving 0x80070005.  For
	 * built-in service accounts RPCSS/SCM launch the surrogate directly
	 * without a stored password.  The surrogate then runs as NETWORK
	 * SERVICE (S-1-5-20), which the kernel maps to uid 0.
	 *
	 * The explicit LaunchPermission and AccessPermission ACLs grant
	 * SYSTEM, NETWORK SERVICE, and Administrators the COM launch and
	 * activation rights so vssvc.exe can reach the surrogate.
	 */
	_snwprintf(key, 512, L"Software\\Classes\\AppID\\%s",
	    ZFS_VSS_PROVIDER_GUID_STR);
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, NULL,
	    REG_OPTION_NON_VOLATILE, KEY_WRITE | KEY_WOW64_64KEY,
	    NULL, &hk_appid, NULL) == ERROR_SUCCESS) {
		RegSetValueExW(hk_appid, NULL, 0, REG_SZ,
		    (const BYTE *)ZFS_VSS_PROVIDER_NAME,
		    (DWORD)((wcslen(ZFS_VSS_PROVIDER_NAME) + 1) *
		    sizeof (wchar_t)));
		RegSetValueExW(hk_appid, L"DllSurrogate", 0, REG_SZ,
		    (const BYTE *)L"", 2);
		RegSetValueExW(hk_appid, L"RunAs", 0, REG_SZ,
		    (const BYTE *)L"NT AUTHORITY\\NetworkService",
		    (DWORD)((wcslen(L"NT AUTHORITY\\NetworkService") + 1) *
		    sizeof (wchar_t)));

		/*
		 * COM permission bits (WinNT.h):
		 *   0x01  COM_RIGHTS_EXECUTE
		 *   0x02  COM_RIGHTS_EXECUTE_LOCAL
		 *   0x04  COM_RIGHTS_EXECUTE_REMOTE
		 *   0x08  COM_RIGHTS_ACTIVATE_LOCAL
		 *   0x10  COM_RIGHTS_ACTIVATE_REMOTE
		 * 0x1f = all five bits = full local+remote access.
		 * SY=SYSTEM  NS=NetworkService  BA=BuiltinAdministrators
		 */
		PSECURITY_DESCRIPTOR psd = NULL;
		ULONG sd_size = 0;
		if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
		    L"O:BAG:BAD:(A;;0x1f;;;SY)(A;;0x1f;;;NS)(A;;0x1f;;;BA)",
		    SDDL_REVISION_1, &psd, &sd_size)) {
			RegSetValueExW(hk_appid, L"LaunchPermission", 0,
			    REG_BINARY, (const BYTE *)psd, sd_size);
			RegSetValueExW(hk_appid, L"AccessPermission", 0,
			    REG_BINARY, (const BYTE *)psd, sd_size);
			LocalFree(psd);
		}

		RegCloseKey(hk_appid);
	}

	return (0);
}

/*
 * Create HKLM\SOFTWARE\OpenZFS\VSS\Snapshots and grant NETWORK SERVICE
 * write access so the provider (running as NT AUTHORITY\NetworkService)
 * can store and retrieve per-snapshot metadata at runtime.
 */
static int
reg_vss_snapshot_store(void)
{
	PSECURITY_DESCRIPTOR psd = NULL;
	ULONG sd_size = 0;
	SECURITY_ATTRIBUTES sa;
	HKEY hk = NULL;
	LSTATUS r;

	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
	    L"D:(A;OICI;KA;;;SY)(A;OICI;KA;;;NS)(A;OICI;KA;;;BA)",
	    SDDL_REVISION_1, &psd, &sd_size))
		return (-1);

	ZeroMemory(&sa, sizeof (sa));
	sa.nLength = sizeof (sa);
	sa.lpSecurityDescriptor = psd;
	sa.bInheritHandle = FALSE;

	r = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
	    L"SOFTWARE\\OpenZFS\\VSS\\Snapshots",
	    0, NULL, REG_OPTION_NON_VOLATILE,
	    KEY_WRITE | KEY_WOW64_64KEY, &sa, &hk, NULL);
	LocalFree(psd);

	if (r != ERROR_SUCCESS)
		return (-1);

	/* Re-apply the DACL to the key in case it already existed. */
	if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
	    L"D:(A;OICI;KA;;;SY)(A;OICI;KA;;;NS)(A;OICI;KA;;;BA)",
	    SDDL_REVISION_1, &psd, &sd_size)) {
		RegSetKeySecurity(hk, DACL_SECURITY_INFORMATION, psd);
		LocalFree(psd);
	}

	RegCloseKey(hk);
	return (0);
}

static int
reg_vss_provider(void)
{
	wchar_t key[512];
	HKEY hk, hk_clsid, hk_access;
	DWORD type_val = VSS_PROV_SOFTWARE_TYPE;
	DWORD allow_val = 1;

	/* 1. Standard Provider Entry */
	_snwprintf(key, 512,
	    L"SYSTEM\\CurrentControlSet\\Services\\VSS\\Providers\\%s",
	    ZFS_VSS_PROVIDER_GUID_STR);
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, NULL,
	    REG_OPTION_NON_VOLATILE, KEY_WRITE | KEY_WOW64_64KEY,
	    NULL, &hk, NULL) != ERROR_SUCCESS)
		return (-1);

	RegSetValueExW(hk, NULL, 0, REG_SZ,
	    (const BYTE *)ZFS_VSS_PROVIDER_NAME,
	    (DWORD)((wcslen(ZFS_VSS_PROVIDER_NAME) + 1) * sizeof (wchar_t)));
	RegSetValueExW(hk, L"Version", 0, REG_SZ,
	    (const BYTE *)ZFS_VSS_PROVIDER_VERSION,
	    (DWORD)((wcslen(ZFS_VSS_PROVIDER_VERSION) + 1) * sizeof (wchar_t)));
	RegSetValueExW(hk, L"VersionId", 0, REG_SZ,
	    (const BYTE *)PROVIDER_VERSION_ID,
	    (DWORD)((wcslen(PROVIDER_VERSION_ID) + 1) * sizeof (wchar_t)));
	RegSetValueExW(hk, L"Type", 0, REG_DWORD,
	    (const BYTE *)&type_val, sizeof (type_val));

	DWORD supported_contexts = 0xFFFFFFFF; // Allow all contexts, or 0x1D specifically for ClientAccessible
	RegSetValueExW(hk, L"SupportedContexts", 0, REG_DWORD,
	    (const BYTE *)&supported_contexts, sizeof(DWORD));

	if (RegCreateKeyExW(hk, L"CLSID", 0, NULL, REG_OPTION_NON_VOLATILE,
	    KEY_WRITE | KEY_WOW64_64KEY, NULL, &hk_clsid, NULL) ==
	    ERROR_SUCCESS) {
		RegSetValueExW(hk_clsid, NULL, 0, REG_SZ,
		    (const BYTE *)ZFS_VSS_PROVIDER_GUID_STR,
		    (DWORD)((wcslen(ZFS_VSS_PROVIDER_GUID_STR) + 1) *
		    sizeof (wchar_t)));
		RegCloseKey(hk_clsid);
	}
	RegCloseKey(hk);

#if 0
	/* 2. VssAccessControl — allow SYSTEM to use this provider */
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE,
	    L"SYSTEM\\CurrentControlSet\\Services\\VSS\\VssAccessControl",
	    0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL,
	    &hk_access, NULL) == ERROR_SUCCESS) {
		RegSetValueExW(hk_access, L"NT AUTHORITY\\SYSTEM", 0,
		    REG_DWORD, (const BYTE *)&allow_val, sizeof (DWORD));
		RegCloseKey(hk_access);
	}
#endif

	return (0);
}

/* ------------------------------------------------------------------ */
/* Uninstall                                                            */
/* ------------------------------------------------------------------ */
static int
do_uninstall(void)
{
	wchar_t key[256];

	_snwprintf(key, 256, L"SOFTWARE\\Classes\\CLSID\\%s\\InprocServer32",
	    ZFS_VSS_PROVIDER_GUID_STR);
	RegDeleteKeyExW(HKEY_LOCAL_MACHINE, key, KEY_WOW64_64KEY, 0);

	_snwprintf(key, 256, L"SOFTWARE\\Classes\\CLSID\\%s\\LocalServer32",
	    ZFS_VSS_PROVIDER_GUID_STR);
	RegDeleteKeyExW(HKEY_LOCAL_MACHINE, key, KEY_WOW64_64KEY, 0);

	_snwprintf(key, 256, L"SOFTWARE\\Classes\\CLSID\\%s",
	    ZFS_VSS_PROVIDER_GUID_STR);
	RegDeleteKeyExW(HKEY_LOCAL_MACHINE, key, KEY_WOW64_64KEY, 0);

	_snwprintf(key, 256, L"SOFTWARE\\Classes\\AppID\\%s",
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

		fwprintf(stdout, L"Installing ZFS VSS Provider DLL: %s\n",
		    dll_path);

		disabled_redirection =
		    Wow64DisableWow64FsRedirection(&old_value);

		if (reg_com_inproc(dll_path) != 0) {
			if (disabled_redirection)
				Wow64RevertWow64FsRedirection(old_value);
			return (1);
		}

		if (reg_vss_provider() != 0) {
			if (disabled_redirection)
				Wow64RevertWow64FsRedirection(old_value);
			return (1);
		}

		if (reg_vss_snapshot_store() != 0) {
			if (disabled_redirection)
				Wow64RevertWow64FsRedirection(old_value);
			return (1);
		}

		if (disabled_redirection)
			Wow64RevertWow64FsRedirection(old_value);

		fwprintf(stdout, L"ZFS VSS Provider installed successfully.\n");
		fwprintf(stdout,
		    L"Restart the VSS service to pick up the new provider:\n"
		    L"  net stop vss && net start vss\n");
		return (0);
	}

	fwprintf(stderr,
	    L"Usage: zfs_vss_install.exe /install [dll_path] | /uninstall\n");
	return (1);
}
