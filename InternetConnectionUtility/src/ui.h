// Internet Connection Utility
// Copyright (c) 2026-2027 Charles McDonald. BSD 3-Clause License.
// Part of Zanphar's WinSuite; see LICENSE.md in the suite root.

#pragma once
// winsock2 must precede windows.h in every translation unit.
#include <winsock2.h>
#include <windows.h>

// Entry point for the Internet Connection Utility UI.
int runApp(HINSTANCE inst, int nCmdShow);
