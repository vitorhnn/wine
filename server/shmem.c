/*
 * Server-side dynamic shared memory management
 *
 * Copyright (C) 2018 Derek Lesho
 *
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "wine/port.h"

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#ifdef HAVE_SYS_MMAN_H
# include <sys/mman.h>
#endif

#include "wine/list.h"

#include "shmem.h"

#define SUBHEAP_SIZE 0x7ffff /* 524 kilobytes*/

/* malloc implementation based of glibc's malloc */

typedef struct tagChunkHeader
{
    size_t previous_chunk_size;
    unsigned int magic;
	size_t user_data_size;
    union
    {
        struct list entry; /* entry in subheap's list of free chunks */
        void* user_data;   /* user data for a chunk that is in-use*/
    };
} ChunkHeader;

/* should be called "base header size" */
const size_t InUseChunkHeaderSize = offsetof(ChunkHeader, user_data);
/* should be called "free-extended" header size */
const size_t FreeChunkHeaderSize = sizeof(ChunkHeader);

const unsigned int InUseMagic = (unsigned int) 0x8fa0abaa;
const unsigned int FreeMagic = (unsigned int) 0xe3339aaa;

const unsigned int SubHeapMagic = (unsigned int) 0xe821e31d;

typedef struct tagSubHeap
{
    unsigned int magic;
    struct list entry; /* entry in heap's list of subheaps */
    struct list chunks; /* list of free chunks (their headers) */
    size_t remaining_space; /* space left before running out*/
    size_t new_chunk_offset;
} SubHeap;

typedef struct tagHeap
{
    struct list subheaps; /* list of subheaps, includes chunk headers + the used data */
} Heap;

void make_new_subheap(Heap* heap)
{
    SubHeap* new_sub = mmap(NULL, SUBHEAP_SIZE, PROT_READ | PROT_WRITE, 
	                          MAP_ANONYMOUS | MAP_SHARED, -1, 0);
    
    list_add_tail(&heap->subheaps, &new_sub->entry);
    list_init(&new_sub->chunks);
    new_sub->remaining_space = SUBHEAP_SIZE - sizeof(SubHeap); /* remaining space excluding freed arenas*/
    new_sub->new_chunk_offset = 0;
    new_sub->magic = SubHeapMagic;
}

static Heap global_heap = {LIST_INIT(global_heap.subheaps)};

void* shmalloc(size_t size)
{
    size_t full_size;
    SubHeap* cur_subheap;
    ChunkHeader* free_chunk_header;
    struct list* prev_entry;

    if(size > (SUBHEAP_SIZE - sizeof(SubHeap) - InUseChunkHeaderSize ))
    {
        fprintf(stderr, "size too large");
        return NULL;
    }
    
    if(size < sizeof(struct list))
    {
        size = sizeof(struct list);
    }
      
    full_size = size + InUseChunkHeaderSize;
    
    /* find a subheap with enough space */
    /* TODO: this is NOT efficient, since we are looping through chunks twice in shmalloc, instead make this loop encapsulate the whole function and continue when an invalid subheap is detected*/
    cur_subheap = NULL;
    LIST_FOR_EACH_ENTRY(cur_subheap, &global_heap.subheaps, SubHeap, entry)
    {
        size_t max_free_space;
        ChunkHeader* cur_free_header;

        /* max amount of free space in an existing chunk */
        max_free_space = 0;
        LIST_FOR_EACH_ENTRY(cur_free_header, &cur_subheap->chunks, ChunkHeader, entry)
        {
            if(cur_free_header->user_data_size > max_free_space) max_free_space = cur_free_header->user_data_size;
        }

        if( ((cur_subheap->remaining_space - sizeof(size_t)) > full_size) || max_free_space > size) break;
        cur_subheap = NULL;
    }
    
    if(!cur_subheap || list_empty(&global_heap.subheaps) )
    {
        make_new_subheap(&global_heap);
        cur_subheap = LIST_ENTRY(list_tail(&global_heap.subheaps), SubHeap, entry);
    }
    
    /* Can we use existing space or do we need to allocate a new chunk? */
    
    free_chunk_header = NULL;
    LIST_FOR_EACH_ENTRY(free_chunk_header, &cur_subheap->chunks, ChunkHeader, entry)
    {
        if(free_chunk_header->user_data_size >= size) break;
        free_chunk_header = NULL;
    }
    
    if(!free_chunk_header || list_empty(&cur_subheap->chunks) )
    {
        ChunkHeader* new_chunk;
        ChunkHeader* next_chunk;

        /* make new chunk */
        assert(cur_subheap->magic == SubHeapMagic);
        new_chunk = (void*)( (unsigned char*)cur_subheap + sizeof(SubHeap) + cur_subheap->new_chunk_offset );

        new_chunk->user_data_size = size;
        
        cur_subheap->new_chunk_offset += new_chunk->user_data_size + InUseChunkHeaderSize;
        cur_subheap->remaining_space -= (new_chunk->user_data_size + InUseChunkHeaderSize);

        new_chunk->magic = InUseMagic;
        /* Write to prev_size of potential next chunk*/
        next_chunk = (void*)( (unsigned char*)new_chunk + new_chunk->user_data_size + InUseChunkHeaderSize );
        next_chunk->previous_chunk_size = new_chunk->user_data_size;
        
        return &(new_chunk->user_data);
    }
    
    prev_entry = free_chunk_header->entry.prev;
    list_remove(&free_chunk_header->entry);
    
    free_chunk_header->magic = InUseMagic;
    
    /* do we need to split off a tail free chunk? */
    if(free_chunk_header->user_data_size > (size + FreeChunkHeaderSize))
    {
        ChunkHeader* next_chunk;
        ChunkHeader* tail_chunk;
        
        tail_chunk = (void*)( (unsigned char*)free_chunk_header + full_size );
        
        tail_chunk->previous_chunk_size = size;
        tail_chunk->user_data_size = free_chunk_header->user_data_size - (size + InUseChunkHeaderSize);
        tail_chunk->magic = FreeMagic;
        
        list_add_after(prev_entry, &tail_chunk->entry);

        next_chunk = (void*)( (unsigned char*)tail_chunk + tail_chunk->user_data_size + InUseChunkHeaderSize );
        next_chunk->previous_chunk_size = tail_chunk->user_data_size;
        
        free_chunk_header->user_data_size = size;
    }
    
    return &free_chunk_header->user_data;
}

void shfree(void* p)
{
    ChunkHeader* prev_chunk;
    ChunkHeader* next_chunk;

    ChunkHeader* freeing_header = (void*)( (unsigned char*)p - InUseChunkHeaderSize);
    
    assert(freeing_header->magic == InUseMagic);
    
    prev_chunk = NULL;
    next_chunk = NULL;
    
    /* need to check for free blocks before and behind*/
    if(freeing_header->previous_chunk_size)
    {
        prev_chunk = (void*)( (unsigned char*)freeing_header - (freeing_header->previous_chunk_size + InUseChunkHeaderSize) );
        
        if(prev_chunk->magic != FreeMagic)
            prev_chunk = NULL;
    }
    
    next_chunk = (void*)( (unsigned char*)freeing_header + InUseChunkHeaderSize + freeing_header->user_data_size );
    if(next_chunk->magic != FreeMagic)
        next_chunk = NULL;
    
    /* Append next chunk to current chunk */
    if(next_chunk)
    {
        size_t append_size;
        ChunkHeader* next_next_chunk;

        /* We delete the next free chunk for sure */
        list_remove(&next_chunk->entry);
        
        append_size = InUseChunkHeaderSize + next_chunk->user_data_size;
        
        /* Update next in use chunk's previous size in case needed */
        next_next_chunk = (void*)( (unsigned char*)next_chunk + append_size );
        next_next_chunk->previous_chunk_size += append_size;
        
        /* Pass it down to enlarge the chunk we are freeing */
        freeing_header->user_data_size += append_size;
    }
    
    /* append our chunk to the previous chunk if it exists */
    if(prev_chunk)
    {
        ChunkHeader* next_next_chunk;
        size_t append_size;

        append_size = InUseChunkHeaderSize + freeing_header->user_data_size;
        
        prev_chunk->user_data_size += append_size;
        
        next_next_chunk = (void*)( (unsigned char*)freeing_header + append_size );
        
        next_next_chunk->previous_chunk_size += append_size;
    }else{
        ChunkHeader* cur_chunk = freeing_header;
        SubHeap* cur_subheap;
        struct list* cur_chunk_entry;

        /* In this case, we need to add ourselves as an entry*/

        freeing_header->magic = FreeMagic;
        
        /* Check Before*/
        for(size_t prev_size = cur_chunk->previous_chunk_size; prev_size > 0; prev_size = cur_chunk->previous_chunk_size)
        {
            cur_chunk = (void*)( (unsigned char*)cur_chunk - (prev_size + InUseChunkHeaderSize) );
            
            if(cur_chunk->magic == InUseMagic) continue;
            
            list_add_after(&cur_chunk->entry, &freeing_header->entry);
            
            return;
        }
        
        /* No other free chunks before hand */
        
        /* now cur_chunk is the base chunk, right after the subheap header */
        cur_subheap = (void*)( (unsigned char*) cur_chunk - sizeof(SubHeap) );
        
        assert(cur_subheap->magic == SubHeapMagic);
        
        if(list_empty(&cur_subheap->chunks))
        {
            list_add_tail(&cur_subheap->chunks, &freeing_header->entry);
            return;
        }
        
        LIST_FOR_EACH(cur_chunk_entry, &cur_subheap->chunks)
        {
            if(cur_chunk_entry < &freeing_header->entry)
            {
                list_add_after(cur_chunk_entry, &freeing_header->entry);
                return;
            }
        }
    }
}