#define JOURNAL_EMPTY  0
#define JOURNAL_WRITE  1
#define JOURNAL_FREE   2
#define JOURNAL_SYMLNK 3
#define JOURNAL_HRDLNK 4

#define JOURNAL_MAX_BLOCKS 256

typedef struct block_struct {
	char buf[OSPFS_BLKSIZE];
} block;

typedef struct journal_stuct {
	// Journal Flags
	uint32_t execute_type;
	uint32_t completed;

	// For changing 2 inodes
	uint32_t inode_num;
	ospfs_inode_t inode;
	uint32_t inode2_num;
	ospfs_inode_t inode2;

	// Header info for block writing
	uint32_t n_blocks_affected;
	// uint32_t n_blocks_freed

	// For block writing
	uint32_t blocknos_affected[JOURNAL_MAX_BLOCKS];
	// TODO: Is this the right way to format this...
	// uint32_t blocknos_freed[JOURNAL_MAX_BLOCKS];

	// TODO: Make sure this is aligned at the start of a block
	// Actual blocks
	uint32_t block[JOURNAL_MAX_BLOCKS];
} journal_t;

// TODO: Figure out EXACTLY how many blocks we need for this
// Guestimate: 1 block for the flags, n_blocks*, and inodes.
//   2 blocks - one for each blocknos_* list
//   256 blocks - one for each block for the journal



