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

#ifndef SHARE_GC_MSWEEP_MSWEEPHEAP_H
#define SHARE_GC_MSWEEP_MSWEEPHEAP_H

#include "gc/shared/collectedHeap.hpp"
#include "gc/shared/softRefPolicy.hpp"
#include "services/memoryManager.hpp"
#include "gc/shared/space.hpp"
#include "memory/virtualspace.hpp"
#include "gc/msweep/msweepBarrierSet.hpp"
#include "gc/shared/markBitMap.inline.hpp"
#include "gc/msweep/msweepFreeListSpace.hpp"

class MSweepHeap: public CollectedHeap {
    friend class VMStructs;
private:
    SoftRefPolicy _soft_ref_policy;
    GCMemoryManager _memory_manager;
    MemoryPool* _pool;
    MSweepFreeListSpace* _free_list_space;
    VirtualSpace _virtual_space;
    size_t _max_tlab_size;

    //Stats
    size_t _step_counter_update;
    size_t _step_heap_print;
    int64_t _decay_time_ns;
    volatile size_t _last_counter_update;
    volatile size_t _last_heap_print;

    //Mark bitmap
    MarkBitMap _mark_bitmap;
    MemRegion _bitmap_region;

    //FreeChunkBitmap
    MarkBitMap _free_chunk_bitmap;
    MemRegion _fc_bitmap_region;

    void prologue();
    void mark();
    void sweep();
    void epilogue();

public:
    static MSweepHeap* heap();

    MSweepHeap() :
            _memory_manager("MSweep Heap", ""),
            _free_list_space(NULL) {};

    virtual Name kind() const {
        return CollectedHeap::MSweep;
    }

    virtual const char* name() const {
        return "MSweep";
    }

    virtual SoftRefPolicy* soft_ref_policy() {
        return &_soft_ref_policy;
    }

    virtual jint initialize();
    virtual void post_initialize() {
        CollectedHeap::post_initialize();
    };
    virtual void initialize_serviceability();

    virtual GrowableArray<GCMemoryManager*> memory_managers();
    virtual GrowableArray<MemoryPool*> memory_pools();

    virtual size_t max_capacity() const { return _virtual_space.reserved_size();  }
    virtual size_t capacity()     const { return _virtual_space.committed_size(); }
    virtual size_t used()         const { return _free_list_space->used(); }

    virtual bool is_in(const void* p) const {
        return _free_list_space->is_in(p);
    }

    virtual bool requires_barriers(stackChunkOop obj) const { return false; }

    virtual bool is_maximal_no_gc() const {
        return used() >= capacity();
    }

    // Basic allocation method
    HeapWord* allocate_work(size_t size, bool verbose = true);
    HeapWord* allocate_or_collect_work(size_t size, bool verbose = true);
    // Raw memory allocation facilities
    virtual HeapWord* mem_allocate(size_t size, bool* gc_overhead_limit_was_exceeded);
    // Create a new tlab.
    // To allow more flexible TLAB allocations min_size specifies
    // the minimum size needed, while requested_size is the requested
    // size based on ergonomics. The actually allocated size will be
    // returned in actual_size
    virtual HeapWord* allocate_new_tlab(size_t min_size,
                                        size_t requested_size,
                                        size_t* actual_size);

    // TLAB allocation
    virtual size_t tlab_capacity(Thread* thr)         const { return capacity();     }
    virtual size_t tlab_used(Thread* thr)             const { return used();         }
    virtual size_t max_tlab_size()                    const { return _max_tlab_size; }
    virtual size_t unsafe_max_tlab_alloc(Thread* thr) const;
    
    virtual void vmentry_collect(GCCause::Cause cause);
    virtual void entry_collect(GCCause::Cause cause);
    virtual void collect(GCCause::Cause cause);
    virtual void do_full_collection(bool clear_all_soft_refs);

    virtual void do_roots(OopClosure* cl, bool everything);

    // Heap walking support
    virtual void object_iterate(ObjectClosure* cl) { _free_list_space->object_iterate(cl); };

    // Object pinning support: every object is implicitly pinned
    virtual bool supports_object_pinning() const           { return true; }
    virtual oop pin_object(JavaThread* thread, oop obj)    { return obj; }
    virtual void unpin_object(JavaThread* thread, oop obj) { }

    // No support for block parsing.
    HeapWord* block_start(const void* addr) const { return NULL;  }
    bool block_is_obj(const HeapWord* addr) const { return false; }
    
    // No GC threads
    virtual void gc_threads_do(ThreadClosure* tc) const {}

    // No nmethod handling
    virtual void register_nmethod(nmethod* nm) {}
    virtual void unregister_nmethod(nmethod* nm) {}
    virtual void verify_nmethod(nmethod* nm) {}

    // No heap verification
    virtual void prepare_for_verify() {}
    virtual void verify(VerifyOption option) {}

    MemRegion reserved_region() const { return _reserved; }
    bool is_in_reserved(const void* addr) const { return _reserved.contains(addr); }
    
    virtual void print_on(outputStream* st) const {}
    virtual void print_tracing_info() const {}
    virtual bool print_location(outputStream* st, void* addr) const { return true; }
};


#endif //SHARE_GC_MSWEEP_MSWEEPHEAP_H
