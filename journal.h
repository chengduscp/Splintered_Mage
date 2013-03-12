#ifndef JOURNAL_STRUCT_HEADER
#define JOURNAL_STRUCT_HEADER

#include "ospfs.h"

// Journal types
#define JOURNAL_EMPTY  0
#define JOURNAL_WRITE  1
#define JOURNAL_FREE   2
#define JOURNAL_SYMLNK 3
#define JOURNAL_HRDLNK 4

// Journal sizes
#define JOURNAL_HEADER_SIZE        1
#define JOURNAL_BLOCKNO_LIST_SIZE  1
#define JOURNAL_INDIR_BLOCKS       2
#define JOURNAL_MAX_BLOCKS        256

// Journal offsets
#define JOURNAL_HEADER_POS         0
#define JOURNAL_BLOCKNO_LIST_POS   1
#define JOURNAL_INDIR_BLOCK_POS    2
#define JOURNAL_INDIR2_BLOCK_POS   3
#define JOURNAL_DATA_BLOCKS_POS    4

// Journal resize types
// No allocation/freeing of indirect or indirect2 blocks
#define JOURNAL_RESIZE_NORMAL     0 
// Allocation/freeing of indirect or indirect2 blocks
#define JOURNAL_RESIZE_INDIRECT   1
#define JOURNAL_RESIZE_INDIRECT2  2
// This is the RESIZE_INDIRECT and RESIZE_INDIRECT2 or'd together
#define JOURNAL_RESIZE_BOTH       3

// Structure of journal
/*
          Block Type            Block Number
+----------------------------+
|            Header          |       0
+----------------------------+
|        Block Numbers       |       1
+----------------------------+
|      Saved indir2 Block    |       2
+----------------------------+
|      Saved indir Block     |       3
+----------------------------+
|         Data Block         |       4
+----------------------------+
|         Data Block         |       5 
+----------------------------+
|            ...             |      ...
+----------------------------+
|         Data Block         |      258
+----------------------------+
|         Data Block         |      259
+----------------------------+
*/

// Header information (block 0 of journal)
typedef struct journal_header_struct {
	// Journal Flags
	uint32_t execute_type;
	uint32_t completed;

	// For changing inodes
	uint32_t inode_num;
	ospfs_inode_t inode;

	// To see how many blocks to write
	uint32_t n_blocks_affected;

	// Info for doubly indirect block writing
	uint32_t indirect2_blockno;
	uint32_t indirect_blockno;

	// For block freeing and adding
	uint32_t file_resize_type;
} journal_header_t;

// Useful struct (going to change)
typedef struct file_index_struct {
	uint32_t blk_size; // Size in blocks of the file
	int indir2_idx, indir_idx, dir_idx; // Indecies of the file
} file_index_t;

// To simplify file resize requests -- this is what we need to know!!
typedef struct resize_request_struct {
	// file index for knowing where we are in the file
	file_index_t index;

	// Knowing whether we are freeing the indirect(2) block or not
	uint32_t resize_type;

	// Indirect block (if needed)
	uint32_t indirect_blockno; // in case we are in indirect2 range
	uint32_t indirect_block[OSPFS_NINDIRECT];

	// Doubly indirect block (if needed)
	uint32_t indirect2_blockno; // in case we change indir2 pointer
	uint32_t indirect2_block[OSPFS_NINDIRECT];

	// List of affected blocknos
	uint32_t n; // n blocks affected
	uint32_t blocknos[JOURNAL_MAX_BLOCKS];
} resize_request;

// For list of blocks affected (block 1 of journal)
typedef struct journal_blocknos_affected {
	uint32_t blocknos[JOURNAL_MAX_BLOCKS];
} journal_blocknos_t;

// For indirect block, indirect2 block, and data blocks, use this
typedef struct journal_block_struct {
	char buf[OSPFS_BLKSIZE];
} journal_block_t;

#endif


