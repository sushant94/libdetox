#ifndef SMALLOC_PAGES
    #define SMALLOC_PAGES 4
#endif

#include <assert.h>
#include <unistd.h>
#include "fbt_mem_alloc.h"
#include "fbt_private_datatypes.h"
#include "fbt_debug.h"
#include "fbt_rbtree.h"
#include "fbt_mem_protection.h"

/**
 * allocate small amount of memory.
 * This function allocates a small amount of memory (less than a page) that is
 * only going to be freed when the thread finishes or the binary translator
 * exits.
 * This function can waste up to as much memory as is going to be allocated. If
 * you need a page or more, use fbt_lalloc().
 * If you need to be able to free/recycle memory, you have to implement this
 * functionality yourself. Have a look at trampoline_allocate() for an example.
 * @param tld thread local data of the current thread
 * @param bytes how many bytes to allocate
 * @return the address of the allocated memory or null on error
 */
void *fbt_smalloc(struct thread_local_data *tld, int bytes)
{
    assert(bytes > 0);

    struct mem_alloc_data *mem_alloc = &(tld->mem_alloc);
    void *retval = 0;

    bytes = (bytes+3)&(~3); /* align to 4 bytes */
    
    if (bytes <= mem_alloc->free_bytes) {
        // we still have enough mapped memory that is free
        retval = mem_alloc->mem_ptr;
        mem_alloc->mem_ptr += bytes;
        mem_alloc->free_bytes -= bytes;
    } else {
        // we actually need to allocate more memory
        int pagesize = sysconf(_SC_PAGESIZE);
        int numpages = SMALLOC_PAGES;
        int alloc_size = SMALLOC_PAGES * pagesize;

        if ((bytes + sizeof(struct alloc_chunk)) > alloc_size) {
            /*
             * our preferred allocation size is not sufficient
             * --> calculate a suitable allocation size
             */
            numpages = (bytes + sizeof(struct alloc_chunk)) / pagesize + 1;
            alloc_size = numpages * pagesize;
        }
        // allocate memory
        tld->next_map -= numpages * pagesize;
        void *mem = mmap(tld->next_map,
                          numpages * pagesize,
                          PROT_READ|PROT_WRITE,
                          MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (MAP_FAILED == mem) {
            llprint("BT failed to allocate memory\n");
            perror("failure in fbt_smalloc()");
        }

        struct alloc_chunk *chunk = 0;
        if (mem_alloc->free_bytes >= sizeof(struct alloc_chunk)) {
            /*
             * we still have space in the old chunk of allocated memory to
             * use for the alloc_chunk struct
             */
            chunk = (struct alloc_chunk*) mem_alloc->mem_ptr;
            retval = mem;
            mem_alloc->free_bytes = alloc_size - bytes;
        } else {
            /*
             * alloc_chunk struct needs to be placed in the newly allocated
             * memory
             */
            chunk = (struct alloc_chunk*) mem;
            retval = mem + sizeof(struct alloc_chunk);
            mem_alloc->free_bytes = alloc_size - bytes
                                    - sizeof(struct alloc_chunk);
        }
        mem_alloc->mem_ptr = retval + bytes;
        chunk->node.addr_begin = mem;
        chunk->node.addr_end = mem + alloc_size;
        chunk->flags = INFO_RFLAG;

        // put alloc_chunk structure at start of linked list
        chunk->next = mem_alloc->chunks;
        mem_alloc->chunks = chunk;

#ifdef SECU_MPROTECT_IDS
//         // insert chunk into the lockdown list
//         chunk->next_lockdown = mem_alloc->lockdown_list;
//         mem_alloc->lockdown_list = chunk;
#endif /* SECU_MPROTECT_IDS */

        // insert alloc_chunk into red-black tree of internal data structures
        mem_alloc->chunks_tree = rb_insert(mem_alloc->chunks_tree,
                                            (struct rb_node*) chunk);
    }
    return retval;
}

/**
 * allocate small amount of persistent memory.
 * This function allocates a small amount of memory (less than a page) that is
 * NOT going to be freed when the thread finishes, only when the binary
 * translator exits.
 * This function can waste up to as much memory as is going to be allocated.
 * If you need a page or more, use allocpages().
 * @param tld thread local data of the current thread
 * @param bytes how many bytes to allocate
 * @return the address of the allocated memory or null on error
 */
void *fbt_smalloc_pers(struct thread_local_data *tld, int bytes)
{
    assert(bytes > 0);

    struct mem_alloc_data *mem_alloc = &(tld->mem_alloc);
    void *retval = 0;

    if (bytes <= mem_alloc->free_pers_bytes) {
        // we still have enough mapped memory that is free
        retval = mem_alloc->pers_mem_ptr;
        mem_alloc->pers_mem_ptr += bytes;
        mem_alloc->free_pers_bytes -= bytes;
    } else {
        // we actually need to allocate more memory
        int pagesize = sysconf(_SC_PAGESIZE);
        int numpages = SMALLOC_PAGES;
        int alloc_size = SMALLOC_PAGES * pagesize;

        if (bytes > alloc_size) {
            /*
             * our preferred allocation size is not sufficient
             * --> calculate a suitable allocation size
             */
            numpages = (bytes / pagesize) + 1;
            alloc_size = numpages * pagesize;
        }
        // allocate memory
        tld->next_map -= alloc_size;
        retval = mmap(tld->next_map,
                          alloc_size,
                          PROT_READ|PROT_WRITE,
                          MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (MAP_FAILED == retval) {
            llprint("BT failed to allocate memory\n");
            perror("failure in fbt_smalloc_pers()");
        }

        // insert it into the tree of internal data structures
        struct alloc_chunk *chunk = fbt_smalloc(tld, sizeof(struct alloc_chunk));
        chunk->node.addr_begin = retval;
        chunk->node.addr_end = retval + alloc_size;
        chunk->flags = INFO_RFLAG;
        mem_alloc->chunks_tree = rb_insert(mem_alloc->chunks_tree,
                                            (struct rb_node*) chunk);

        mem_alloc->pers_mem_ptr = retval + bytes;
        mem_alloc->free_pers_bytes = alloc_size - bytes;
    }
    return retval;
}

/**
 * allocate a number of pages in memory.
 * This function allocates the specified number of pages in memory. The
 * allocated memory is inserted into a list of memory to be freed when the
 * thread is finished.
 * If you want to free the memory yourself, use the allocpages() and
 * freepages() macros instead of this function.
 * @param tld thread local data of the current thread
 * @param pages how many pages to allocate
 * @return the address of the allocated, page aligned memory
 */
void *fbt_lalloc(struct thread_local_data *tld, int pages)
{
    assert(pages > 0);
    int alloc_size = pages * sysconf(_SC_PAGESIZE);

    struct alloc_chunk *chunk = fbt_smalloc(tld, sizeof(struct alloc_chunk));

//     void *retval = mmap((void*) rand_r(&(tld->rand_state)),
//                          pages * sysconf(_SC_PAGESIZE),
//                          PROT_READ|PROT_WRITE,
//                          MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    tld->next_map -= alloc_size;
    void *retval = mmap(tld->next_map,
                        alloc_size,
                        PROT_READ|PROT_WRITE,
                        MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (MAP_FAILED == retval) {
            llprint("BT failed to allocate memory\n");
            perror("failure in fbt_lalloc()");
        }

    if ((void*) -1 == retval) {
        /*
         * something went wrong, and we certainly don't want to put -1 into
         * the list of allocated memory, and later call munmap on -1
         */
        return retval;
    }
    chunk->node.addr_begin = retval;
    chunk->node.addr_end = retval + alloc_size;
    chunk->flags = INFO_RFLAG;

    // put alloc_chunk structure at start of linked list
    chunk->next = tld->mem_alloc.chunks;
    tld->mem_alloc.chunks = chunk;

    // insert alloc_chunk structure into red-black tree
    tld->mem_alloc.chunks_tree = rb_insert(tld->mem_alloc.chunks_tree,
                                            (struct rb_node*) chunk);
//     INFO_LLPRINTF("lalloc allocated memory at %p\n", retval);
    return retval;
}

/**
 * free all non-persistent memory.
 * Walks through the list of alloc_chunks and frees the associated memory.
 * @param tld thread local data of the current thread
 */
void fbt_free_all(struct thread_local_data *tld)
{
    if (NULL == tld) {
        return;
    }
    int kbytes_freed = 0;
    struct mem_alloc_data *mem_alloc = &(tld->mem_alloc);

    // iterate through linked list
    while (mem_alloc->chunks != NULL) {
        /*
         * We have to iterate to the next list element before removing the
         * memory that is tracked with a particular alloc_chunk struct because
         * in some cases, this struct located in that memory.
         */
        void *addr = mem_alloc->chunks->node.addr_begin;
        long length = (long) mem_alloc->chunks->node.addr_end - (long) addr;
        mem_alloc->chunks = mem_alloc->chunks->next;
        munmap(addr, length);
        kbytes_freed += length >> 10;
    }
    INFO_LLPRINTF("KiB freed on thread termination: %d\n", kbytes_freed);
}