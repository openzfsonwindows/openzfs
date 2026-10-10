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

#pragma once
#include <windows.h>

// Shows the versions window for "path" and runs its message loop until it
// is closed. Used for "zfs_tray.exe --versions <path>"; returns an exit code.
int RunVersionsWindow(HINSTANCE hInst, const wchar_t *path);

// "zfs_tray.exe --deleted <dir>": names that exist only in snapshots of a
// directory, with open / copy / restore.
int RunDeletedWindow(HINSTANCE hInst, const wchar_t *path);
