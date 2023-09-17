#pragma once
#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef null
  #define null NULL
#endif

enum{IS_FREE, IS_FREE_POINTER, IS_FREE_FINAL, IS_ALLOCATED};


#define ALLOCATOR_USE_DEFAULT_PAGE_SIZE 0
#define ALLOCATOR_DEFAULT_PAGE_SIZE sysconf(_SC_PAGE_SIZE)

#define ALLOCATOR_INTERNAL_PAGE_ALLOCATOR(size) malloc(size)
#define ALLOCATOR_INTERNAL_PAGE_DEALLOCATOR(ptr,size) free(ptr)

//Magic NO touch
#define ALLOCATOR_ALIGN8(x) (x % 8)? (((x >> 3) + 1) << 3) : x

typedef uint8_t byte;

typedef struct allocator{
  void* page;
  uint64_t page_size;
  struct allocator* next;
} allocator;

#define allocator_auto_cleanup __attribute__((cleanup(allocator_dealloc)))


typedef struct allocator_header{
  uint64_t size;
  uint64_t status;
} allocator_header;



typedef struct allocator_allocation{
  allocator_header* head;
  byte* ptr;
} allocator_allocation;


static allocator_allocation allocation_map_internal(void* ptr){
  return (allocator_allocation){
    .head = ((allocator_header*)(ptr)),
    .ptr  = &((uint8_t*)(ptr))[sizeof(allocator_header)]
  };
}

//YOU PASS YOUR POINTER NOT THE ALLOCATION HANDLE
static allocator_allocation allocation_map(void* ptr){
  return (allocator_allocation){
    .head = ((allocator_header*)(ptr) - 1),
    .ptr  = ((uint8_t*)(ptr))
  };
}

#define ALLOCATOR_DEFINE_PRINT_FUCNTION
#ifdef ALLOCATOR_DEFINE_PRINT_FUCNTION
#include <stdio.h>
static void allocation_print(allocator_allocation* b){
  
  printf("size: %lu",b->head->size);
  switch (b->head->status){
    case IS_FREE: printf(",status:\tIS_FREE\t"); break;
    case IS_FREE_POINTER: printf(",status:\tIS_FREE_POINTER %p\t",(uint64_t*)*(uint64_t*)b->ptr); break;
    case IS_FREE_FINAL: printf(",status:\tIS_FREE_FINAL\t"); break;
    case IS_ALLOCATED: printf(",status:\tIS_ALLOCATED\t"); break;
  }
  printf(", data: %p\n", b->ptr);
}
#endif

#define ALLOCATION_HEADER_SIZE sizeof(allocator_header)

//splits a block in half the old block is the left side
static int8_t allocation_split(allocator_allocation old_alloc,uint64_t rsize,allocator_allocation* left_side,allocator_allocation* right_side){
  uint64_t new_block_size = old_alloc.head->size - rsize;

  if(new_block_size < 8){return -1;}

  old_alloc.head->size = rsize;
  uint8_t* new_alloc_start = old_alloc.ptr + rsize;
  allocator_allocation new_alloc = allocation_map_internal(new_alloc_start);

  *new_alloc.head = (allocator_header){new_block_size - ALLOCATION_HEADER_SIZE, old_alloc.head->status};

  if(old_alloc.head->status == IS_FREE_POINTER){
    *(uint64_t*)new_alloc.ptr = (uint64_t)*(uint64_t*)old_alloc.ptr; 
  }

  if(left_side){*left_side = old_alloc;}
  if(right_side){*right_side = new_alloc;}

  return 0;
}


static allocator allocator_init(uint64_t page_size){
  allocator a;
  a.page_size = (page_size == ALLOCATOR_USE_DEFAULT_PAGE_SIZE)? ALLOCATOR_DEFAULT_PAGE_SIZE : page_size;
  a.page = ALLOCATOR_INTERNAL_PAGE_ALLOCATOR(a.page_size);
  a.next = null;

  *((allocator_header*)(a.page)) = (allocator_header){ a.page_size - ALLOCATION_HEADER_SIZE, IS_FREE};
  return a;
}

//if it returns null there is no space left
static void* allocator_allocate_write_internal(uint64_t* data_size, allocator_allocation* allocation, allocator_allocation* origin) {
  allocator_allocation left_side = *allocation;
  allocator_allocation right_side;
  if (allocation->head->size > *data_size) {
    allocation_split(*allocation, *data_size, &left_side, &right_side);
    if (origin->head->status == IS_FREE_POINTER)
      *(uint64_t *)origin->ptr = (uint64_t)(uint64_t *)right_side.ptr;
  }
  // allocate
  left_side.head->status = IS_ALLOCATED;
  return left_side.ptr;
}
static void *allocator_alloc_internal(allocator *a, uint64_t data_size) {
  byte* block = (byte*)a->page;
  allocator_allocation allocation = allocation_map_internal(block);
  allocator_allocation origin = allocation;
  const byte* const end = block + a->page_size;
  allocator_allocation jump;
  
  while(block + allocation.head->size < end){
    if((allocation.head->status == IS_FREE || allocation.head->status == IS_FREE_FINAL) && allocation.head->size >= data_size){
      return allocator_allocate_write_internal(&data_size, &allocation, &origin);
    }
    else if(allocation.head->status == IS_FREE_POINTER){

      if(allocation.head->size >= data_size){
        return allocator_allocate_write_internal(&data_size, &allocation, &origin);
      }

      jump = allocation_map((uint64_t*)*(uint64_t*)allocation.ptr);
      if(jump.head->status == IS_ALLOCATED){
        allocation.head->status = IS_FREE;
      }
      else if(jump.head->size >= data_size){
        origin = allocation;
        block = jump.ptr - sizeof(allocator_header);
        goto SKIP;
      }
    }
    
    block = allocation.ptr + allocation.head->size;
    SKIP: allocation = allocation_map_internal(block);
  }
  return null;
}

static void* allocator_allocation_recursion_internal(allocator* a,uint64_t data_size){
  //try to do an allocation
  void* p = allocator_alloc_internal(a, data_size);
  if(p != null) return p;

  //failed to allocate in this allocation now we create a new page
  a->next = (allocator*)malloc(sizeof(allocator));
  uint64_t new_page_size = a->page_size;
  
  while (new_page_size < data_size) {new_page_size *= 2;} //Magic NO touch
  new_page_size = ALLOCATOR_ALIGN8(new_page_size);
  *a->next = allocator_init(new_page_size);

  return allocator_allocation_recursion_internal(a->next,data_size);
}

//DO NOT FREE THIS POINTER WITH FREE USE THE ALLOCATOR_FREE
static void* allocator_malloc(allocator* a,uint64_t data_size){
  data_size += sizeof(allocator_header);
  data_size = ALLOCATOR_ALIGN8(data_size);
  return allocator_allocation_recursion_internal(a,data_size);
}

static void allocator_free(void* ptr){
  allocator_header* head = (allocator_header*)((byte*)ptr - sizeof(allocator_header)); 
  head->status = IS_FREE;
}

static void allocator_defragment_internal(allocator* a){
  byte* page = (byte*)a->page;
  allocator_allocation allocation = allocation_map_internal(page);
  allocator_allocation prev_allocation = allocation;
  const uint64_t page_size = a->page_size;
  byte *end = page + page_size;

  while (allocation.ptr + allocation.head->size <= end){
    if(allocation.head->status == IS_FREE){
      if(prev_allocation.head->status == IS_FREE || prev_allocation.head->status == IS_FREE_POINTER){
        *(uint64_t*)prev_allocation.ptr = (uint64_t)(uint64_t*)allocation.ptr;
        prev_allocation.head->status = IS_FREE_POINTER;
      }

      //find neighbouring free blocks merge them and cache the current allocation
      allocator_allocation next = allocation_map_internal(allocation.ptr + allocation.head->size);
      uint64_t expansion_size = 0;
      while (next.head->status == IS_FREE && next.ptr + next.head->size <= end) {
        expansion_size += next.head->size + sizeof(allocator_header);
        next = allocation_map_internal(next.ptr + next.head->size);
      }
      //change the size
      allocation.head->size += expansion_size;
      prev_allocation = allocation;
    }
      
    page = allocation.ptr + allocation.head->size;
    allocation = allocation_map_internal(page);
  }
  if(prev_allocation.head->status == IS_FREE || prev_allocation.head->status == IS_FREE_POINTER) prev_allocation.head->status = IS_FREE_FINAL; 
}


static void allocator_defragment(allocator* a){
  allocator* it = a;
  while (it != null) {
    allocator_defragment_internal(it);
    it = it->next;
  }
}

static void allocator_print_allocation(allocator* a){
  byte* ptr = (byte*)a->page;
  allocator_allocation allocation = allocation_map_internal(ptr);

  const uint64_t page_size = a->page_size;
  byte* end = ptr + page_size - 1;

  while(ptr + allocation.head->size < end){
    putchar('\t');allocation_print(&allocation);
    
    ptr = allocation.ptr + allocation.head->size;
    allocation = allocation_map_internal(ptr);
  }

}

static void allocator_print_allocations(allocator *a){
  allocator* it = a;
  uint64_t page_index = 0;
  while (it != null) {
    printf("Page index:%li\n",page_index);
    allocator_print_allocation(it);
    it = it->next;
    page_index++;
  }
}

static void allocator_dealloc(allocator* a){
  allocator* it = a;
  if(it->page) free(it->page);
  //gotta start from a->next because a might be allocated on the stack
  it = a->next;
  while(it != null){
    allocator* tmp = it;
    if(it->page) free(it->page);
    it = it->next;
    if(tmp) free(tmp);
  }
}




//used for smaller allocation you can not free individual allocations from this
typedef struct arena_allocator{
  allocator* alloc;
  uint64_t capacity;
  uint64_t size;
  void* ptr;
}arena_allocator;


static inline uint64_t arena_allocator_remaining_capacity(arena_allocator* aa){
  return aa->capacity - aa->size;
}

static void arena_allocator_print_info(arena_allocator* aa){
  printf("Allocator: %p\tCapacity: %lu\tSize: %lu\tRemaining capacity: %lu\tPointer: %p\n",aa->alloc,aa->capacity,aa->size,arena_allocator_remaining_capacity(aa),aa->ptr);
}

#define ALLOCATOR_ARENA_DEBUG_FUNC
#ifdef ALLOCATOR_ARENA_DEBUG_FUNC
  #define ALLOCATOR_ARENA_DEBUG_ENABLE(x) x
  #define ALLOCATOR_ARENA_DEBUG_DISABLE(x)
#else
  #define ALLOCATOR_ARENA_DEBUG_ENABLE(x)
  #define ALLOCATOR_ARENA_DEBUG_DISABLE(x) x
#endif



//if the a is null then an allocator will be create for you
static inline arena_allocator arena_allocator_init(uint64_t capacity,allocator* a){
  allocator* alloc = a;
  if(alloc == null){
    alloc = (allocator*)malloc(sizeof(allocator));
    *alloc = allocator_init(capacity+capacity/4); //Magic NO touch
  }

  if(alloc->page_size < capacity) {
    ALLOCATOR_ARENA_DEBUG_DISABLE(__builtin_unreachable());
    ALLOCATOR_ARENA_DEBUG_ENABLE(
      printf("%s() arena needs more capacity(%lu bytes) allocator can provide(%lu bytes)\n", __FUNCTION__, capacity, alloc->page_size);
      exit(EXIT_FAILURE);
    ); 
    
  }

  void* ptr = allocator_malloc(alloc, capacity);

  return (arena_allocator){alloc,capacity,0,ptr};
}

static void* arena_allocator_malloc(arena_allocator* aa,uint64_t size){
  if(aa->size + size > aa->capacity) return null; //full

  uint8_t* rptr = ((uint8_t*)aa->ptr) + aa->size;
  aa->size += size;

  return rptr;
}

inline void arena_allocator_reset(arena_allocator* aa){
  aa->size = 0;
}


static inline allocator* arena_allocator_free(arena_allocator* aa){
  allocator_free(aa->ptr);
  return aa->alloc;
}
