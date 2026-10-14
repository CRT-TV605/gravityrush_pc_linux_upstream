// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace Gow3Sampler {

/// GOW3_SAMPLE_FILE set (a path prefix): samples the calling thread's call stack about 1000 times a
/// second and rewrites <prefix>-<name>.txt every 10 s with the most frequent stacks (addresses relative to the
/// executable; scripts/symbolize_samples.py names them). Diagnostics only.
void SampleCurrentThread(const char* name);

} // namespace Gow3Sampler
