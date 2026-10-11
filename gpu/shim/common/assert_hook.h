// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: a diagnostic run once before a GPU assertion stops the port (assert.cpp), e.g. the
// command parser's report of the packet it was decoding. The hook must not assert; a second
// assertion while it runs stops without it.
#pragma once

void SetAssertStopHook(void (*hook)());
