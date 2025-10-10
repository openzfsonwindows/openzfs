#pragma once
#include <windows.h>
#include "rpc_client.h" // zrpc_t

// Create a modeless Import window owned by hParent.
// The window will manage its own lifetime.
HWND CreateImportWindow(HWND hParent, zrpc_t *rpc);
