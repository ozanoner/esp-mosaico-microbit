// SPDX-License-Identifier: Apache-2.0
#pragma once

// Internal-RAM sampler.
//
// Snapshots lie on this board: the panel's QSPI path allocates a temporary
// internal DMA bounce per transaction, so a reading taken between stages misses
// the peak that actually kills a flush. A 5 ms task keeps the running minimum,
// and every stage line reports it before resetting the window.

#ifdef __cplusplus
extern "C" {
#endif

/** Start the sampler task. Idempotent. */
void mb_mem_probe_start(void);

/** Log one `[MEM <stage>]` line with free/largest internal RAM and the lowest
 * free value seen since the previous stage line, then reset that window.
 * stage may be NULL. */
void mb_mem_probe_stage(const char *stage);

#ifdef __cplusplus
}
#endif
