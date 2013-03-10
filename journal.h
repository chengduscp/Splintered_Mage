#ifndef JOURNAL_STRUCT_HEADER
#define JOURNAL_STRUCT_HEADER

#include "stdint.h"
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

typedef struct journal_header_struct {
	// Journal Flags
	uint32_t execute_type;
	uint32_t completed;

	// For changing inodes
	uint32_t inode_num;
	ospfs_inode_t inode;

	// To see how many blocks to write
	uint32_t n_blocks_affected;

	// Info for indirect block writing
	uint32_t indir_blockno;

	// Info for direct block writing
	uint32_t dir_blocknos_affected[OSPFS_NDIRECT];
} journal_header_t;

typedef struct blocknos_affected_struct {
	uint32_t a[JOURNAL_MAX_BLOCKS];
} blocknos_affected_t;

#endif


