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

#pragma once
#include <windows.h>

// Shows the snapshots window and runs its message loop until it is closed.
// Used for "zfs_tray.exe --snapshots [dataset]"; "dataset" (a pool or any
// dataset name, may be NULL or empty) is the one initially selected. Returns
// an exit code.
int RunSnapshotsWindow(HINSTANCE hInst, const wchar_t *dataset);
