/*
 * Copyright (c) 2023, Kirill Garbar, Red Hat, Inc. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include "precompiled.hpp"
#include "gc/msweep/msweepArguments.hpp"
#include "gc/msweep/msweepHeap.hpp"
#include "gc/shared/gcArguments.hpp"
#include "gc/shared/tlab_globals.hpp"
#include "logging/log.hpp"
#include "runtime/globals.hpp"
#include "runtime/globals_extension.hpp"

size_t MSweepArguments::conservative_max_heap_alignment() {
    return UseLargePages ? os::large_page_size() : os::vm_page_size();
}

void MSweepArguments::initialize() {
    GCArguments::initialize();

    assert(UseMSweepGC, "Sanity");

    // Forcefully exit when OOME is detected. Nothing we can do at that point.
    if (FLAG_IS_DEFAULT(ExitOnOutOfMemoryError)) {
        FLAG_SET_DEFAULT(ExitOnOutOfMemoryError, true);
    }

    if (MSweepMaxTLABSize < MinTLABSize) {
        log_warning(gc)("MSweepMaxTLABSize < MinTLABSize, adjusting it to " SIZE_FORMAT, MinTLABSize);
        MSweepMaxTLABSize = MinTLABSize;
    }

#ifdef COMPILER2
    // Enable loop strip mining: there are still non-GC safepoints, no need to make it worse
  if (FLAG_IS_DEFAULT(UseCountedLoopSafepoints)) {
    FLAG_SET_DEFAULT(UseCountedLoopSafepoints, true);
    if (FLAG_IS_DEFAULT(LoopStripMiningIter)) {
      FLAG_SET_DEFAULT(LoopStripMiningIter, 1000);
    }
  }
#endif
}

void MSweepArguments::initialize_alignments() {
    size_t page_size = UseLargePages ? os::large_page_size() : os::vm_page_size();
    size_t align = MAX2((size_t)os::vm_allocation_granularity(), page_size);
    SpaceAlignment = align;
    HeapAlignment  = align;
}

CollectedHeap* MSweepArguments::create_heap() {
    return new MSweepHeap();
}