// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace BbReplay {
/// True only inside the production replay iteration loop, after capture loading, initial reset
/// validation and exact image round trip succeeded. Read-only: no environment flag enables it.
/// This identifies a diagnostic repeated workload, never pixel correctness or live gameplay.
bool MeasurementActive();
}
