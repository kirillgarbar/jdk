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

#include "gc/msweep/msweepHeap.hpp"
#include "gc/msweep/msweepMemoryPool.hpp"
#include "gc/msweep/msweepInitLogger.hpp"
#include "precompiled.hpp"
#include "memory/universe.hpp"
#include "memory/allocation.hpp"
#include "memory/allocation.inline.hpp"
#include "gc/shared/gcArguments.hpp"
#include "gc/shared/locationPrinter.inline.hpp"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/metaspaceUtils.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "runtime/atomic.hpp"
#include "runtime/globals.hpp"
#include "runtime/mutexLocker.hpp"
#include "gc/shared/markBitMap.hpp"
#include "compiler/oopMap.hpp"
#include "gc/shared/weakProcessor.hpp"
#include "gc/shared/strongRootsScope.hpp"
#include "classfile/classLoaderDataGraph.hpp"
#include "classfile/stringTable.hpp"
#include "classfile/systemDictionary.hpp"
#include "code/codeCache.hpp"
#include "gc/shared/strongRootsScope.hpp"
#include "gc/shared/preservedMarks.inline.hpp"
#include "gc/shared/weakProcessor.hpp"
#include "memory/iterator.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "runtime/objectMonitor.inline.hpp"
#include "runtime/thread.hpp"
#include "runtime/threads.hpp"
#include "runtime/vmOperations.hpp"
#include "runtime/vmThread.hpp"
#include "utilities/stack.inline.hpp"
#include "services/management.hpp"

MSweepHeap* MSweepHeap::heap() {
    return named_heap<MSweepHeap>(CollectedHeap::MSweep);
}

jint MSweepHeap::initialize() {
    size_t align = HeapAlignment;
    size_t init_byte_size = align_up(InitialHeapSize, align);
    size_t max_byte_size  = align_up(MaxHeapSize, align);

    // Initialize backing storage(maximum size)
    ReservedHeapSpace heap_rs = Universe::reserve_heap(max_byte_size, align);
    _virtual_space.initialize(heap_rs, max_byte_size);

    MemRegion committed_region((HeapWord*)_virtual_space.low(),          (HeapWord*)_virtual_space.high());

    initialize_reserved_region(heap_rs);

    // Mark bitmap reserve and initialization(No large page)
    size_t heap_size = heap_rs.size();
    size_t bitmap_size = MarkBitMap::compute_size(heap_size);
    
    ReservedSpace bitmap_space(bitmap_size);

    _bitmap_region = MemRegion((HeapWord*) (bitmap_space.base()),
			bitmap_space.size() / HeapWordSize);

    _mark_bitmap.initialize(committed_region, _bitmap_region);

    // Mark bitmap reserve and initialization(No large page)
    ReservedSpace fc_bitmap_space(bitmap_size);

    _fc_bitmap_region = MemRegion((HeapWord*) (fc_bitmap_space.base()),
			fc_bitmap_space.size() / HeapWordSize);

    _free_chunk_bitmap.initialize(committed_region, _fc_bitmap_region);

    if (!os::commit_memory((char*)_fc_bitmap_region.start(), _fc_bitmap_region.byte_size(), false)) {
			log_warning(gc)("Could not commit native memory for marking bitmap");
		}

    //Initialize space
    _free_list_space = new MSweepFreeListSpace(&_free_chunk_bitmap);
    _free_list_space->initialize(committed_region, /* clear_space = */ true, /* mangle_space = */ true);

    _max_tlab_size = MIN2(CollectedHeap::max_tlab_size(), align_object_size(MSweepMaxTLABSize / HeapWordSize));

    // Install barrier set
    BarrierSet::set_barrier_set(new MSweepBarrierSet());

    // Print out the configuration
    MSweepInitLogger::print();

    return JNI_OK;
}

void MSweepHeap::initialize_serviceability() {
    _pool = new MSweepMemoryPool(this);
    _memory_manager.add_pool(_pool);
}

GrowableArray<GCMemoryManager*> MSweepHeap::memory_managers() {
    GrowableArray<GCMemoryManager*> memory_managers(1);
    memory_managers.append(&_memory_manager);
    return memory_managers;
}

GrowableArray<MemoryPool*> MSweepHeap::memory_pools() {
    GrowableArray<MemoryPool*> memory_pools(1);
    memory_pools.append(_pool);
    return memory_pools;
}

//Main allocation method used in any other allocation method
HeapWord* MSweepHeap::allocate_work(size_t size, bool verbose) {
    assert(is_object_aligned(size), "Allocation size should be aligned: " SIZE_FORMAT, size);

    HeapWord* res = NULL;
    // Try to allocate, assume space is available
    res = _free_list_space->allocate(size);

    assert(is_object_aligned(res), "Object should be aligned: " PTR_FORMAT, p2i(res));
    return res;
}

HeapWord* MSweepHeap::allocate_new_tlab(size_t min_size,
                                         size_t requested_size,
                                         size_t* actual_size) {
    Thread* thread = Thread::current();

    bool fits = true;
    size_t size = requested_size;
    int64_t time = 0;

    // Always honor boundaries
    size = clamp(size, min_size, _max_tlab_size);

    // Always honor alignment
    size = align_up(size, MinObjAlignment);

    // Check that adjustments did not break local and global invariants
    assert(is_object_aligned(size),
           "Size honors object alignment: " SIZE_FORMAT, size);
    assert(min_size <= size,
           "Size honors min size: "  SIZE_FORMAT " <= " SIZE_FORMAT, min_size, size);
    assert(size <= _max_tlab_size,
           "Size honors max size: "  SIZE_FORMAT " <= " SIZE_FORMAT, size, _max_tlab_size);
    assert(size <= CollectedHeap::max_tlab_size(),
           "Size honors global max size: "  SIZE_FORMAT " <= " SIZE_FORMAT, size, CollectedHeap::max_tlab_size());

    // All prepared, let's do it!
    HeapWord* res = allocate_or_collect_work(size);

    if (res != NULL) {
        // Allocation successful
        *actual_size = size;
    }

    return res;
}

HeapWord* MSweepHeap::mem_allocate(size_t size, bool *gc_overhead_limit_was_exceeded) {
    *gc_overhead_limit_was_exceeded = false;
    return allocate_or_collect_work(size);
}

size_t MSweepHeap::unsafe_max_tlab_alloc(Thread* thr) const {
    // Return max allocatable TLAB size, and let allocation path figure out
    // the actual allocation size. Note: result should be in bytes.
    return _max_tlab_size * HeapWordSize;
}

//GC
//
//
//
//

class VM_MSweepGC: public VM_Operation {
	private:
		const GCCause::Cause _cause;
		MSweepHeap* const _heap;
	public:
		VM_MSweepGC(GCCause::Cause cause) :
		    VM_Operation(), _cause(cause), _heap(MSweepHeap::heap()) {}

		VM_Operation::VMOp_Type type() const { return VMOp_MSweepGC; }

		const char* name() const { return "MSweepGC Collection"; }

	virtual bool doit_prologue() {
		Heap_lock->lock();
		return true;
	}

	virtual void doit() {
		_heap->entry_collect(_cause);
	}

	virtual void doit_epilogue() {
		Heap_lock->unlock();
	}
};

void MSweepHeap::vmentry_collect(GCCause::Cause cause) {
    VM_MSweepGC vmop(cause);
    VMThread::execute(&vmop);
}

void MSweepHeap::entry_collect(GCCause::Cause cause) {
    prologue();
    mark();
    sweep();
    epilogue();
}

class PrintHeapClosure: public ObjectClosure {
    public:
        virtual void do_object(oop obj) {
            if (obj->size() > 100) log_info(gc)("Object, %li", obj->size());
        }
};

HeapWord* MSweepHeap::allocate_or_collect_work(size_t size, bool verbose) {
	HeapWord* res = allocate_work(size, verbose);
	if (res == NULL) {
		vmentry_collect(GCCause::_allocation_failure);
		res = allocate_work(size);
	}
	return res;
}

typedef Stack<oop, mtGC> MSweepMarkStack;

void MSweepHeap::do_roots(OopClosure* cl, bool everything) {
	// Need to tell runtime we are about to walk the roots with 1 thread
	StrongRootsScope scope(0);

	// Need to adapt oop closure for some special root types.
	CLDToOopClosure clds(cl, ClassLoaderData::_claim_none);
	MarkingCodeBlobClosure blobs(cl, CodeBlobToOopClosure::FixRelocations, false);

	// Walk all these different parts of runtime roots. Some roots require
	// holding the lock when walking them.
	{
		MutexLocker lock(CodeCache_lock, Mutex::_no_safepoint_check_flag);
		CodeCache::blobs_do(&blobs);
	}
	ClassLoaderDataGraph::roots_cld_do(&clds, NULL);
    OopStorageSet::strong_oops_do(cl);
	Threads::oops_do(cl, &blobs);
}

class ScanOopClosure: public BasicOopIterateClosure {
    private:
        MSweepMarkStack* const _stack;
        MarkBitMap* const _bitmap;

        template<class T>
		void do_oop_work(T* p) {
			  // p is the pointer to memory location where oop is, load the value
			  // from it, unpack the compressed reference, if needed:
			T o = RawAccess<>::oop_load(p);
			if (!CompressedOops::is_null(o)) {
				oop obj = CompressedOops::decode_not_null(o);

				// Object is discovered. See if it is marked already. If not,
				// mark and push it on mark stack for further traversal. Non-atomic
				// check and set would do, as this closure is called by single thread.
				if (!_bitmap->is_marked(obj)) {
                    if (obj->size() > 100 ) log_info(gc)("Marking obj %li", obj->size());
					_bitmap->mark(obj);
					_stack->push(obj);
				}
			}
		}

    public:
        ScanOopClosure(MSweepMarkStack* stack, MarkBitMap* bitmap) :
			_stack(stack), _bitmap(bitmap) {
		}

        virtual void do_oop(oop* p) {
			do_oop_work(p);
		}
		virtual void do_oop(narrowOop* p) {
			do_oop_work(p);
		}
};

class SweepClosure: public ObjectClosure {
    private:
        MarkBitMap* const _live_bitmap;
        MSweepFreeList* const _free_list;

    public:
        SweepClosure(MarkBitMap* live, MSweepFreeList* free_list) :
			_live_bitmap(live), _free_list(free_list) {
		}
        
        virtual void do_object(oop obj) {
            if (!_live_bitmap->is_marked(obj)) {
                if (obj->size() > 100) log_info(gc)("Sweeping obj %li", obj->size());
                MSweepNode* node = new MSweepNode(cast_from_oop<HeapWord*>(obj), MSweepFreeList::adjust_chunk_size(obj->size()));
                
                _free_list->append(node);
            }
        }
};

void MSweepHeap::prologue() {
    //Commiting memory for bitmap
    if (!os::commit_memory((char*)_bitmap_region.start(), _bitmap_region.byte_size(), false)) { return; }

    //Retire all TLABS
    ensure_parsability(true);
}

void MSweepHeap::mark() {
    // Marking stack and the closure that does most of the work. The closure
    // would scan the outgoing references, mark them, and push newly-marked
    // objects to stack for further processing.
    MSweepMarkStack stack;
    ScanOopClosure cl(&stack, &_mark_bitmap);

    //Not all roots
    do_roots(&cl, false);

    // Scan the rest of the heap until we run out of objects. Termination is
    // guaranteed, because all reachable objects would be marked eventually.
    while (!stack.is_empty()) {
        oop obj = stack.pop();
        obj->oop_iterate(&cl);
    }
}

void MSweepHeap::sweep() {
    SweepClosure cl = SweepClosure(&_mark_bitmap, _free_list_space->free_list());
    _free_list_space->object_iterate(&cl);
}

void MSweepHeap::epilogue() {
    if (!os::uncommit_memory((char*)_bitmap_region.start(), _bitmap_region.byte_size())) {
			log_warning(gc)("Could not uncommit native memory for marking bitmap");
		}
}

void MSweepHeap::collect(GCCause::Cause cause) {
    switch (cause) {
        case GCCause::_metadata_GC_threshold:
        case GCCause::_metadata_GC_clear_soft_refs:
            // Receiving these causes means the VM itself entered the safepoint for metadata collection.
            // While MSweep does not do GC, it has to perform sizing adjustments, otherwise we would
            // re-enter the safepoint again very soon.

            assert(SafepointSynchronize::is_at_safepoint(), "Expected at safepoint");
            log_info(gc)("GC request for \"%s\" is handled", GCCause::to_string(cause));
            MetaspaceGC::compute_new_size();
            break;
        default:
            log_info(gc)("GC request for \"%s\" is ignored", GCCause::to_string(cause));
    }
}

void MSweepHeap::do_full_collection(bool clear_all_soft_refs) {
    collect(gc_cause());
}
