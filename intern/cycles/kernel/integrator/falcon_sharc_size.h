/* SPDX-FileCopyrightText: 2026 Falcon experiment
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Size of the Falcon photon / SHARC hash table, in ONE place.
 *
 * This used to be written out as `1 << 26` in five separate spots -- the kernel
 * constant here, three allocations in scene/integrator.cpp, and the host mirror
 * in integrator/path_trace.cpp. Changing a subset does not fail loudly: raising
 * only the kernel's copy left the host allocating the old size and the
 * measurement came back *unchanged*, which was read as "table size does not
 * matter" and sent a whole investigation down the wrong path (2026-08-10).
 * Lowering a subset is the same bug wearing a costume -- it crashes with
 * "Illegal address in CUDA queue" instead.
 *
 * Header only, no namespace, no device qualifiers: it is included from both
 * kernel and host translation units. */

#pragma once

/* Power of two. 26 = 64M cells * 16 bytes = 1 GB on the device.
 * Occupancy matters: the table has one slot per cell and no collision
 * resolution, so two world cells that hash together share a slot, their flux is
 * summed, and whoever wrote its tag last reads the mixture as its own. The
 * brightening that follows is a pure function of how full the table is
 * (tools/cache_stats.py computes it from a baked cache). */
#define FALCON_SHARC_CELL_COUNT_LOG2 26
#define FALCON_SHARC_CELL_COUNT (1u << FALCON_SHARC_CELL_COUNT_LOG2)
#define FALCON_SHARC_CELL_MASK (FALCON_SHARC_CELL_COUNT - 1u)

/* Floats per cell: accumulated R, G, B, and the collision tag. */
#define FALCON_SHARC_CELL_STRIDE 4
