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

/*
 * "A new version is available" check for the tray.
 *
 * Looks at the GitHub releases of openzfsonwindows/openzfs, picks the newest
 * installer asset for this machine, and compares it with the version of the
 * loaded OpenZFS driver. Nothing is ever downloaded or installed; the user is
 * pointed at the release page.
 */

#pragma once

#include <windows.h>

/* Posted to the tray window when a check finishes; lParam is the result. */
#define	WM_UPDATE_CHECK_DONE	(WM_USER + 20)

/*
 * Shows a notification, returns TRUE only if it was actually handed to the
 * shell. The version is only marked as notified when this returns TRUE.
 */
typedef BOOL (*update_notify_fn)(const wchar_t *title, const wchar_t *text);

/* Load settings, arm the 30 second first-check and the daily timers. */
void update_check_init(HWND hwnd, update_notify_fn notify);

/* WM_TIMER dispatch; returns TRUE if the timer id was ours. */
BOOL update_check_timer(HWND hwnd, UINT_PTR id);

/* WM_UPDATE_CHECK_DONE handler. */
void update_check_done(HWND hwnd, LPARAM lParam);

BOOL update_check_enabled(void);
BOOL update_check_prereleases(void);
void update_check_set_enabled(HWND hwnd, BOOL on);
void update_check_set_prereleases(HWND hwnd, BOOL on);

/* TRUE and the menu text if a newer version is currently to be shown. */
BOOL update_check_menu_text(wchar_t *buf, size_t cch);

/* Open the release page of the available version in the browser. */
void update_check_open_page(void);
