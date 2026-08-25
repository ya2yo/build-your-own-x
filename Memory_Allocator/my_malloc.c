#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

void debug_log(const char *msg) { write(STDOUT_FILENO, msg, strlen(msg)); }

struct free_area {
  uint8_t marker;
  struct free_area *prev;
  bool in_use;
  uint32_t length;
  struct free_area *next;
};
struct stats {
  int magic_bytes;
  bool my_simple_lock;
  uint32_t amount_of_blocks;
  uint32_t amount_of_pages;
};
typedef struct free_area area;
typedef struct stats my_stats;

const int MAGICAL_BYTES = 0x55;
const int BLOCK_MARKER = 0xDD;
const int FIRST_BLOCK_OFFSET = sizeof(area);
const int PAGE_SIZE = 4096;

char *heap_start = NULL;

int *add_used_block(ssize_t size);
my_stats *get_malloc_header() {
  assert(heap_start != NULL);
  my_stats *malloc_header = (my_stats *)heap_start;
  assert(malloc_header->magic_bytes == MAGICAL_BYTES);
  return malloc_header;
}
area *find_first_block() {
  return (area *)((char *)heap_start + sizeof(my_stats));
}
area *find_last_block();
void reduce_heap_size_if_possible();

static void check_heap_invariants(void) {
  if (heap_start == NULL) {
    return;
  }

  my_stats *header = get_malloc_header();
  area *block = (area *)((char *)heap_start + sizeof(my_stats));
  area *previous = NULL;
  uint32_t count = 0;

  while (block != NULL) {
    assert(block->marker == BLOCK_MARKER);
    assert(block->prev == previous);
    assert(block->length > 0);
    if (block->next != NULL) {
      assert(block->next > block);
      assert(block->next->prev == block);
    }
    previous = block;
    block = block->next;
    count++;
  }

  assert(previous != NULL);
  assert(!previous->in_use);
  assert(count == header->amount_of_blocks);
}

static area *find_block_by_payload(const void *ptr) {
  if (ptr == NULL || heap_start == NULL) {
    return NULL;
  }

  area *block = (area *)((char *)heap_start + sizeof(my_stats));
  while (block != NULL) {
    if ((char *)block + sizeof(area) == (const char *)ptr) {
      return block;
    }
    block = block->next;
  }
  return NULL;
}

int *an_malloc(ssize_t size) {
  if (size <= 0) {
    return NULL;
  }
  if (heap_start == NULL) {
    heap_start = sbrk(0);
    if (heap_start == (void *)-1 || sbrk(PAGE_SIZE) == (void *)-1) {
      heap_start = NULL;
      return NULL;
    }
  }
  char *heap_end = sbrk(0);
  if (heap_end == (void *)-1) {
    return NULL;
  }
  long int length = heap_end - heap_start;
  // Fisrt, check if the magical bytes are at the begining of the heap
  if ((*heap_start) != MAGICAL_BYTES) {
    // first execution of malloc
    *(heap_start) = MAGICAL_BYTES;
    my_stats *malloc_header = (my_stats *)heap_start;
    malloc_header->amount_of_blocks = 1;
    malloc_header->amount_of_pages = 1;
    area *first_block = (area *)((char *)heap_start + sizeof(my_stats));
    first_block->marker = BLOCK_MARKER;
    first_block->in_use = false;
    first_block->length = length - sizeof(my_stats) - sizeof(area);
    first_block->next = NULL;
    first_block->prev = NULL;
  }
  int *result = add_used_block(size);
  if (result != NULL) {
    check_heap_invariants();
  }
  return result;
}

bool an_free(void *ptr) {
  if (ptr == NULL || heap_start == NULL) {
    return false;
  }

  my_stats *malloc_header = get_malloc_header();
  while (malloc_header->my_simple_lock) {
    sleep(1);
  }
  malloc_header->my_simple_lock = true;

  area *block = find_block_by_payload(ptr);
  if (block == NULL || !block->in_use) {
    malloc_header->my_simple_lock = false;
    return false;
  }

  block->in_use = false;
  memset(ptr, 0, block->length);

  /* Merge with the next free block. */
  if (block->next != NULL && !block->next->in_use) {
    area *next = block->next;
    block->next = next->next;
    block->length += sizeof(area) + next->length;
    if (block->next != NULL) {
      block->next->prev = block;
    }
    assert(malloc_header->amount_of_blocks > 1);
    malloc_header->amount_of_blocks--;
  }

  /* Merge with the previous free block.  The previous block is the merged
   * block; never skip over it or dereference its predecessor blindly. */
  if (block->prev != NULL && !block->prev->in_use) {
    area *previous = block->prev;
    previous->length += sizeof(area) + block->length;
    previous->next = block->next;
    if (previous->next != NULL) {
      previous->next->prev = previous;
    }
    block = previous;
    assert(malloc_header->amount_of_blocks > 1);
    malloc_header->amount_of_blocks--;
  }

  reduce_heap_size_if_possible();
  malloc_header->my_simple_lock = false;
  check_heap_invariants();
  return true;
}

int *add_used_block(ssize_t size) {
  assert(size > 0);
  my_stats *malloc_header = get_malloc_header();
  while (malloc_header->my_simple_lock) {
    sleep(1);
  };
  malloc_header->my_simple_lock = true;
  // find smallest space in the free blocks and add there
  area *block = (area *)((char *)heap_start + sizeof(my_stats));
  area *smallest_block = NULL;
  // best fit
  while (block != NULL) {
    assert(block->marker == BLOCK_MARKER);
    if ((ssize_t)(block->length + sizeof(area)) >= size &&
        block->in_use == false) {
      smallest_block = block;
      break;
    }
    block = block->next;
  }
  // no big enough blocks.
  if (smallest_block == NULL) {
    area *last = find_last_block();
    assert(!last->in_use);
    while (last->length < (uint32_t)size) {
      if (sbrk(PAGE_SIZE) == (void *)-1) {
        malloc_header->my_simple_lock = false;
        return NULL;
      }
      last->length += PAGE_SIZE;
      malloc_header->amount_of_pages++;
    }
    smallest_block = last;
  }

  size_t available = smallest_block->length;
  size_t requested = (size_t)size;
  if (requested > SIZE_MAX - sizeof(area) - 1) {
    malloc_header->my_simple_lock = false;
    return NULL;
  }
  if (available < requested + sizeof(area) + 1) {
    if (sbrk(PAGE_SIZE) == (void *)-1) {
      malloc_header->my_simple_lock = false;
      return NULL;
    }
    smallest_block->length += PAGE_SIZE;
    available += PAGE_SIZE;
    malloc_header->amount_of_pages++;
  }
  if (available >= requested + sizeof(area) + 1) {
    area *new_block = (area *)((char *)smallest_block + sizeof(area) + requested);
    new_block->marker = BLOCK_MARKER;
    new_block->in_use = false;
    new_block->prev = smallest_block;
    new_block->next = smallest_block->next;
    new_block->length = available - requested - sizeof(area);
    if (new_block->next != NULL) {
      new_block->next->prev = new_block;
    }
    smallest_block->next = new_block;
    smallest_block->length = requested;
    malloc_header->amount_of_blocks++;
  }

  smallest_block->in_use = true;
  malloc_header->my_simple_lock = false;
  return (int *)((char *)smallest_block + sizeof(area));
}

area *find_last_block() {
  my_stats *malloc_header = get_malloc_header();
  area *block = (area *)((char *)malloc_header + sizeof(my_stats));
  while (block->next) {
    block = block->next;
  }
  return block;
}

area *find_previous_used_block(area *ptr) {
  area *mov_ptr = ptr == NULL ? NULL : ptr->prev;
  while (mov_ptr != NULL) {
    if (mov_ptr->in_use) {
      return mov_ptr;
    }
    mov_ptr = mov_ptr->prev;
  }
  return NULL;
}

void reduce_heap_size_if_possible() {
  area *last_block = find_last_block();
  assert(last_block != NULL);
  assert(!last_block->in_use); /* allocator invariant */

  area *last_used = find_previous_used_block(last_block);
  char *heap_end = (char *)sbrk(0);
  char *new_end;

  if (last_used == NULL) {
    /* No live allocation: retain the initial page. */
    new_end = heap_start + PAGE_SIZE;
  } else {
    new_end = (char *)last_used + sizeof(area) + last_used->length;
  }

  while (heap_end - new_end > PAGE_SIZE) {
    if (sbrk(-PAGE_SIZE) == (void *)-1) {
      break;
    }
    heap_end -= PAGE_SIZE;
    get_malloc_header()->amount_of_pages--;
  }

  /* The existing trailing free header remains valid if it is still inside the
   * heap. Recompute its capacity rather than creating a stale second header. */
  if (last_used == NULL) {
    last_block->length = (uint32_t)(heap_end - (char *)last_block - sizeof(area));
  } else {
    assert((char *)last_block >= new_end);
    last_block->length = (uint32_t)(heap_end - (char *)last_block - sizeof(area));
    assert(last_block->length > 0);
  }
}

void test_basic_malloc() {
  char *ptr = (char *)an_malloc(1);
  area *first_block = (void *)ptr - sizeof(area);
  assert(first_block->marker == BLOCK_MARKER);
  *ptr = 'C';
  assert(*ptr == 'C');
}

void test_bigger_than_available_malloc() {
  uint16_t *ptr = (uint16_t *)an_malloc(5000);
  area *first_block = (void *)ptr - sizeof(area);
  for (uint16_t i = 0; i <= 2499; i = i + 1) {
    *(ptr + i) = i;
  }
  assert(first_block->marker == BLOCK_MARKER);
  assert(*ptr == 0);
  assert(*(ptr + 2) == 2);
  assert(*(ptr + 2499) == 2499);
  // little endian valid only
  assert(*((uint8_t *)ptr + 4999) == (2499 >> 8));
  assert(*((uint8_t *)ptr + 4998) == (2499 & 0xFF));
}

void test_free() {
  uint8_t *first = (uint8_t *)an_malloc(2048);
  area *first_block = (void *)first - sizeof(area);
  assert(first_block->next != NULL);
  assert(first_block->length == 2048);
  area *second_block = first_block->next;
  assert(second_block->marker == BLOCK_MARKER);
  assert(second_block->in_use == false);
  assert(second_block->next == NULL);
  assert(second_block->length == PAGE_SIZE - sizeof(my_stats) -
                                     (2 * sizeof(area)) - first_block->length);
  an_free(first);
  assert(first_block->marker == BLOCK_MARKER);
  assert(first_block->next == NULL);
  assert(first_block->length == PAGE_SIZE - sizeof(my_stats) - sizeof(area));
}

void complex_set_of_malloc_and_free_calls() {
  uint8_t *first =
      (uint8_t *)an_malloc(2048); // will leave another 2048 on the first page
  area *first_block = find_first_block();
  assert(first_block->length == 2048);
  area *second_block = first_block->next;
  assert(second_block->length ==
         PAGE_SIZE - sizeof(my_stats) - 2 * sizeof(area) - first_block->length);
  assert(second_block->next == NULL);
  assert(second_block->prev == first_block);
  uint8_t *second =
      (uint8_t *)an_malloc(10000); // will need around two more pages
  assert(second_block->length == 10000);
  assert(second_block->next != NULL);
  area *third_block = second_block->next;
  assert(third_block->length == 3 * PAGE_SIZE - sizeof(my_stats) -
                                    3 * sizeof(area) - first_block->length -
                                    second_block->length);
  my_stats *malloc_header = get_malloc_header();
  assert(malloc_header->amount_of_pages == 3);
  assert(malloc_header->amount_of_blocks == 3);
  an_free(second);
  assert(malloc_header->amount_of_pages == 1);
  assert(malloc_header->amount_of_blocks == 2);
  int heap_size = (int)((char *)sbrk(0) - heap_start);
  assert(heap_size == PAGE_SIZE);
  // The second block is whatever is left from the first page
  assert(second_block->in_use == false);
  assert(first_block->length == 2048);
  assert(second_block->length ==
         PAGE_SIZE - sizeof(my_stats) - 2 * sizeof(area) - first_block->length);
  assert(second_block->next == NULL);
  // test block unification, add three blocks, free the left, free the right,
  // and then free the middle
  uint8_t *third = (uint8_t *)an_malloc(1000);
  assert(malloc_header->amount_of_pages == 1);
  assert(malloc_header->amount_of_blocks == 3);
  // A third, empty block has been created
  area *third_block_new = second_block->next;
  assert(third_block_new->marker == BLOCK_MARKER);
  assert(third_block_new->in_use == false);
  // The second block, which before was longer, now is used for the call. The
  // third block is the one that is empty
  assert(second_block->length == 1000);
  assert(third_block_new->length == PAGE_SIZE - sizeof(my_stats) -
                                        3 * sizeof(area) - first_block->length -
                                        second_block->length);
  assert(third_block_new->next == NULL);
  uint8_t *fourth = (uint8_t *)an_malloc(5000);
  // third block has been used for the fourth malloc call
  assert(third_block_new->length == 5000);
  assert(third_block_new->next != NULL);
  assert(third_block_new->in_use == true);
  assert(third_block_new->prev == second_block);
  assert(malloc_header->amount_of_pages ==
         3); // the 5000 needed a second page, and then another page was needed
             // to create a third block
  assert(malloc_header->amount_of_blocks == 4);
  uint8_t *fifth = (uint8_t *)an_malloc(1000);
  // a new block has been created
  area *fourth_block = third_block_new->next;
  assert(fourth_block->marker == BLOCK_MARKER);
  assert(third_block_new->length == 5000);
  assert(fourth_block->length == 1000);
  assert(fourth_block->in_use == true);
  assert(fourth_block->next != NULL);
  area *fifth_block = fourth_block->next;
  assert(fifth_block->marker == BLOCK_MARKER);
  assert(fifth_block->in_use == false);
  assert(fifth_block->next == NULL);
  assert(malloc_header->amount_of_pages == 3);
  assert(malloc_header->amount_of_blocks == 5); // fifth malloc made a new block
  uint8_t *sixth = (uint8_t *)an_malloc(
      1000); // just as buffer between the end and the fifth block
  assert(fifth_block->in_use == true);
  assert(fifth_block->length == 1000);
  assert(fifth_block->next != NULL);
  assert(fifth_block->prev == fourth_block);
  area *sixth_block = fifth_block->next;
  assert(sixth_block->marker == BLOCK_MARKER);
  assert(sixth_block->in_use == false);
  assert(sixth_block->next == NULL);
  assert(malloc_header->amount_of_pages == 3);
  assert(malloc_header->amount_of_blocks == 6);
  an_free(third);
  assert(second_block->in_use == false);
  assert(second_block->length == 1000); // should be unchanged
  assert(malloc_header->amount_of_pages == 3);
  assert(malloc_header->amount_of_blocks == 6); // because we have a free block
  an_free(fifth);
  assert(fourth_block->in_use == false);
  assert(malloc_header->amount_of_pages == 3);
  assert(malloc_header->amount_of_blocks == 6);
  an_free(fourth);
  assert(third_block_new->in_use == false);
  assert(malloc_header->amount_of_pages ==
         3); // that is normal, as block six is still there
  assert(malloc_header->amount_of_blocks == 4); // three blocks have become one
}

void call_test(void (*test_func)(), const char *msg) {
  pid_t pid = fork();
  if (pid == 0) {
    test_func();
    exit(0);
  } else {
    int status;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
      printf("%s crashed with signal %d\n", msg, WTERMSIG(status));
    } else {
      printf("%s passed\n", msg);
    }
  }
}

int main() {
  call_test(test_basic_malloc, "Basic Malloc");
  call_test(test_bigger_than_available_malloc, "Request more memory Malloc");
  call_test(test_free, "Basic Free");
  call_test(complex_set_of_malloc_and_free_calls, "Complex");
  debug_log("DONE");
}
