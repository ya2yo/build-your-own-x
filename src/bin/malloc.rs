//! A safe, Rust-style memory allocator simulation.
//!
//! Unlike the C example, this implementation does not manipulate the process
//! break and does not expose raw pointers. `Vec<u8>` owns the arena, block
//! offsets describe its layout, and `Allocation` is a checked handle.
//!
//! The allocation policy is still the same as the original example:
//! best-fit reuse, block splitting, coalescing on free, and page-sized growth.

use std::fmt;

const DEFAULT_PAGE_SIZE: usize = 4096;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Allocation {
    block_id: usize,
    generation: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum BlockState {
    Free,
    Used,
    Retired,
}

#[derive(Debug)]
struct Block {
    offset: usize,
    size: usize,
    generation: u64,
    state: BlockState,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Stats {
    pub arena_size: usize,
    pub pages: usize,
    pub total_blocks: usize,
    pub used_blocks: usize,
    pub free_blocks: usize,
    pub used_bytes: usize,
    pub free_bytes: usize,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AllocatorError {
    InvalidPageSize,
    ZeroSize,
    OutOfMemory,
    InvalidAllocation,
    AlreadyFreed,
}

impl fmt::Display for AllocatorError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let message = match self {
            Self::InvalidPageSize => "page size must be greater than zero",
            Self::ZeroSize => "allocation size must be greater than zero",
            Self::OutOfMemory => "allocator cannot grow the arena",
            Self::InvalidAllocation => "allocation handle is invalid",
            Self::AlreadyFreed => "allocation has already been freed",
        };
        f.write_str(message)
    }
}

impl std::error::Error for AllocatorError {}

/// Owns a byte arena and manages non-overlapping regions inside it.
#[derive(Debug)]
pub struct Allocator {
    arena: Vec<u8>,
    page_size: usize,
    next_generation: u64,
    blocks: Vec<Block>,
}

impl Allocator {
    /// Creates an allocator with one initially available page.
    pub fn new(page_size: usize) -> Result<Self, AllocatorError> {
        if page_size == 0 {
            return Err(AllocatorError::InvalidPageSize);
        }
        Ok(Self {
            arena: vec![0; page_size],
            page_size,
            next_generation: 1,
            blocks: vec![Block {
                offset: 0,
                size: page_size,
                generation: 0,
                state: BlockState::Free,
            }],
        })
    }

    /// Creates an allocator using the example's 4096-byte page size.
    pub fn with_default_page_size() -> Self {
        Self::new(DEFAULT_PAGE_SIZE).expect("the default page size is valid")
    }

    /// Allocates a region using best-fit selection.
    pub fn allocate(&mut self, size: usize) -> Result<Allocation, AllocatorError> {
        if size == 0 {
            return Err(AllocatorError::ZeroSize);
        }

        let block_id = self
            .find_best_fit(size)
            .or_else(|| self.append_free_space(size).ok())
            .ok_or(AllocatorError::OutOfMemory)?;

        let generation = self.next_generation;
        self.next_generation = self.next_generation.wrapping_add(1).max(1);

        let old_size = self.blocks[block_id].size;
        if old_size > size {
            let remainder = old_size - size;
            self.blocks[block_id].size = size;
            self.blocks[block_id].state = BlockState::Used;
            self.blocks.push(Block {
                offset: self.blocks[block_id].offset + size,
                size: remainder,
                generation: 0,
                state: BlockState::Free,
            });
        } else {
            self.blocks[block_id].state = BlockState::Used;
        }
        self.blocks[block_id].generation = generation;

        Ok(Allocation {
            block_id,
            generation,
        })
    }

    /// Releases an allocation and merges adjacent free regions.
    pub fn deallocate(&mut self, allocation: Allocation) -> Result<(), AllocatorError> {
        let block = self
            .blocks
            .get_mut(allocation.block_id)
            .ok_or(AllocatorError::InvalidAllocation)?;
        if block.generation != allocation.generation || block.state == BlockState::Retired {
            return Err(AllocatorError::InvalidAllocation);
        }
        if block.state == BlockState::Free {
            return Err(AllocatorError::AlreadyFreed);
        }

        self.arena[block.offset..block.offset + block.size].fill(0);
        block.state = BlockState::Free;
        block.generation = 0;
        self.coalesce_free_blocks();
        Ok(())
    }

    /// Returns the bytes belonging to a live allocation.
    pub fn get(&self, allocation: Allocation) -> Result<&[u8], AllocatorError> {
        let block = self.checked_block(allocation)?;
        Ok(&self.arena[block.offset..block.offset + block.size])
    }

    /// Returns mutable bytes belonging to a live allocation.
    pub fn get_mut(&mut self, allocation: Allocation) -> Result<&mut [u8], AllocatorError> {
        let (offset, size) = {
            let block = self.checked_block(allocation)?;
            (block.offset, block.size)
        };
        Ok(&mut self.arena[offset..offset + size])
    }

    pub fn stats(&self) -> Stats {
        let mut stats = Stats {
            arena_size: self.arena.len(),
            pages: self.arena.len() / self.page_size,
            total_blocks: 0,
            used_blocks: 0,
            free_blocks: 0,
            used_bytes: 0,
            free_bytes: 0,
        };
        for block in self
            .blocks
            .iter()
            .filter(|block| block.state != BlockState::Retired)
        {
            stats.total_blocks += 1;
            match block.state {
                BlockState::Used => {
                    stats.used_blocks += 1;
                    stats.used_bytes += block.size;
                }
                BlockState::Free => {
                    stats.free_blocks += 1;
                    stats.free_bytes += block.size;
                }
                BlockState::Retired => unreachable!(),
            }
        }
        stats
    }

    fn checked_block(&self, allocation: Allocation) -> Result<&Block, AllocatorError> {
        let block = self
            .blocks
            .get(allocation.block_id)
            .ok_or(AllocatorError::InvalidAllocation)?;
        if block.state != BlockState::Used || block.generation != allocation.generation {
            return Err(AllocatorError::InvalidAllocation);
        }
        Ok(block)
    }

    fn find_best_fit(&self, size: usize) -> Option<usize> {
        self.blocks
            .iter()
            .enumerate()
            .filter(|(_, block)| block.state == BlockState::Free && block.size >= size)
            .min_by_key(|(_, block)| block.size)
            .map(|(id, _)| id)
    }

    fn append_free_space(&mut self, size: usize) -> Result<usize, AllocatorError> {
        let pages = size.div_ceil(self.page_size);
        let additional = pages
            .checked_mul(self.page_size)
            .ok_or(AllocatorError::OutOfMemory)?;
        let offset = self.arena.len();
        self.arena
            .try_reserve_exact(additional)
            .map_err(|_| AllocatorError::OutOfMemory)?;
        self.arena.resize(offset + additional, 0);
        self.blocks.push(Block {
            offset,
            size: additional,
            generation: 0,
            state: BlockState::Free,
        });
        Ok(self.blocks.len() - 1)
    }

    fn coalesce_free_blocks(&mut self) {
        loop {
            let mut merged = false;
            'outer: for left in 0..self.blocks.len() {
                if self.blocks[left].state != BlockState::Free {
                    continue;
                }
                for right in 0..self.blocks.len() {
                    if left == right || self.blocks[right].state != BlockState::Free {
                        continue;
                    }
                    let adjacent = self.blocks[left].offset + self.blocks[left].size
                        == self.blocks[right].offset;
                    if adjacent {
                        self.blocks[left].size += self.blocks[right].size;
                        self.blocks[right].state = BlockState::Retired;
                        merged = true;
                        break 'outer;
                    }
                }
            }
            if !merged {
                break;
            }
        }
    }
}

fn main() -> Result<(), AllocatorError> {
    let mut allocator = Allocator::with_default_page_size();
    let first = allocator.allocate(128)?;
    allocator.get_mut(first)?[0] = b'R';
    assert_eq!(allocator.get(first)?[0], b'R');
    allocator.deallocate(first)?;

    let large = allocator.allocate(10_000)?;
    allocator.get_mut(large)?.fill(0xabu8);
    println!("allocator checks passed: {:?}", allocator.stats());
    allocator.deallocate(large)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    // Equivalent to the C test_basic_malloc test.
    #[test]
    fn test_basic_malloc() {
        let mut allocator = Allocator::new(DEFAULT_PAGE_SIZE).unwrap();
        let allocation = allocator.allocate(1).unwrap();

        assert_eq!(allocator.get(allocation).unwrap().len(), 1);
        allocator.get_mut(allocation).unwrap()[0] = b'C';
        assert_eq!(allocator.get(allocation).unwrap()[0], b'C');
    }

    // Equivalent to test_bigger_than_available_malloc. The C test writes 2500
    // little-endian u16 values, which occupy exactly 5000 bytes.
    #[test]
    fn test_bigger_than_available_malloc() {
        let mut allocator = Allocator::new(DEFAULT_PAGE_SIZE).unwrap();
        let allocation = allocator.allocate(5000).unwrap();
        let data = allocator.get_mut(allocation).unwrap();

        for (index, chunk) in data.chunks_exact_mut(2).enumerate() {
            let value = index as u16;
            chunk.copy_from_slice(&value.to_le_bytes());
        }

        let data = allocator.get(allocation).unwrap();
        assert_eq!(data.len(), 5000);
        assert_eq!(u16::from_le_bytes([data[0], data[1]]), 0);
        assert_eq!(u16::from_le_bytes([data[4], data[5]]), 2);
        assert_eq!(u16::from_le_bytes([data[4998], data[4999]]), 2499);
        assert_eq!(allocator.stats().arena_size, 3 * DEFAULT_PAGE_SIZE);
    }

    // Equivalent to test_free. Rust does not need an in-memory block header,
    // so the assertions describe the same layout in payload bytes.
    #[test]
    fn test_free() {
        let mut allocator = Allocator::new(DEFAULT_PAGE_SIZE).unwrap();
        let allocation = allocator.allocate(2048).unwrap();
        assert_eq!(allocator.stats().used_bytes, 2048);
        assert_eq!(allocator.stats().free_bytes, 2048);

        allocator.deallocate(allocation).unwrap();
        let stats = allocator.stats();
        assert_eq!(stats.used_blocks, 0);
        assert_eq!(stats.free_blocks, 1);
        assert_eq!(stats.free_bytes, DEFAULT_PAGE_SIZE);
        assert_eq!(
            allocator.get(allocation),
            Err(AllocatorError::InvalidAllocation)
        );
    }

    // Equivalent to complex_set_of_malloc_and_free_calls. The exact C block
    // counts differ because this Rust allocator stores metadata in `Vec<Block>`
    // rather than inside the managed byte arena.
    #[test]
    fn complex_set_of_malloc_and_free_calls() {
        let mut allocator = Allocator::new(DEFAULT_PAGE_SIZE).unwrap();

        let first = allocator.allocate(2048).unwrap();
        assert_eq!(allocator.stats().free_bytes, 2048);

        let second = allocator.allocate(10_000).unwrap();
        assert_eq!(allocator.stats().pages, 4);
        assert_eq!(allocator.stats().used_blocks, 2);

        allocator.deallocate(second).unwrap();
        assert_eq!(allocator.stats().used_blocks, 1);
        assert_eq!(allocator.stats().free_bytes, 14_336);

        let third = allocator.allocate(1000).unwrap();
        let fourth = allocator.allocate(5000).unwrap();
        let fifth = allocator.allocate(1000).unwrap();
        assert_eq!(allocator.stats().used_blocks, 4);
        assert_eq!(allocator.stats().used_bytes, 9048);

        allocator.deallocate(third).unwrap();
        assert_eq!(allocator.stats().used_blocks, 3);

        allocator.deallocate(fifth).unwrap();
        assert_eq!(allocator.stats().used_blocks, 2);

        allocator.deallocate(fourth).unwrap();
        assert_eq!(allocator.stats().used_blocks, 1);
        assert_eq!(allocator.stats().free_bytes, 14_336);

        allocator.deallocate(first).unwrap();
        let stats = allocator.stats();
        assert_eq!(stats.used_blocks, 0);
        assert_eq!(stats.free_blocks, 1);
        assert_eq!(stats.free_bytes, 4 * DEFAULT_PAGE_SIZE);
    }
}
