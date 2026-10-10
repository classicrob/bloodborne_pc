// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../gpu/shim/macos_metalfx.h"

int main() {
    // Invalid handles distinguish successful signature validation from the unsigned failure.
    BbMetalFX::TemporalScaler scaler(VK_NULL_HANDLE, nullptr, VK_NULL_HANDLE,
                                     VK_NULL_HANDLE, {2560, 1440, 3840, 2160});
    return 1;
}
