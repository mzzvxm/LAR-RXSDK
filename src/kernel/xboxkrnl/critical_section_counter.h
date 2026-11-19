/**
 * @file        kernel/xboxkrnl/critical_section_counter.h
 * @brief       Debug counter of the critical section enters that spin
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

namespace rex::kernel::xboxkrnl {

/**
 * Logs the totals of kernel.count_critical_section_spins and clears them: the enters of a critical
 * section with a spin count above 0, and how many of them took the section in the spin loop. The
 * title state calls it when the title ends.
 *
 * @param enabled The setting's value. With it off nothing is logged unless a count is not 0.
 */
void LogCriticalSectionSpinCounts(bool enabled);

}  // namespace rex::kernel::xboxkrnl
