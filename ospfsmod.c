#include <linux/autoconf.h>
#include <linux/version.h>
#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#include <linux/module.h>
#include <linux/moduleparam.h>
#include "ospfs.h"
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <asm/uaccess.h>
#include <linux/kernel.h>
#include <linux/sched.h>

// For the journal
#include "journal.h"

// Some useful macros...
#ifndef MIN
#define MIN(x, y) ((x < y) ? x : y)
#endif
#ifndef MAX
#define MAX(x, y) ((x > y) ? x : y)
#endif


/****************************************************************************
 * ospfsmod
 *
 *   This is the OSPFS module!  It contains both library code for your use,
 *   and exercises where you must add code.
 *
 ****************************************************************************/

/* Define eprintk() to be a version of printk(), which prints messages to
 * the console.
 * (If working on a real Linux machine, change KERN_NOTICE to KERN_ALERT or
 * KERN_EMERG so that you are sure to see the messages.  By default, the
 * kernel does not print all messages to the console.  Levels like KERN_ALERT
 * and KERN_EMERG will make sure that you will see messages.) */
#define eprintk(format, ...) printk(KERN_NOTICE format, ## __VA_ARGS__)

// The actual disk data is just an array of raw memory.
// The initial array is defined in fsimg.c, based on your 'base' directory.
extern uint8_t ospfs_data[];
extern uint32_t ospfs_length;

// A pointer to the superblock; see ospfs.h for details on the struct.
static ospfs_super_t * const ospfs_super =
	(ospfs_super_t *) &ospfs_data[OSPFS_BLKSIZE];

static int change_size(ospfs_inode_t *oi, uint32_t inod_num, uint32_t want_size);
static ospfs_direntry_t *find_direntry(ospfs_inode_t *dir_oi, const char *name, int namelen);


/*****************************************************************************
 * FILE SYSTEM OPERATIONS STRUCTURES
 *
 *   Linux filesystems are based around three interrelated structures.
 *
 *   These are:
 *
 *   1. THE LINUX SUPERBLOCK.  This structure represents the whole file system.
 *      Example members include the root directory and the number of blocks
 *      on the disk.
 *   2. LINUX INODES.  Each file and directory in the file system corresponds
 *      to an inode.  Inode operations include "mkdir" and "create" (add to
 *      directory).
 *   3. LINUX FILES.  Corresponds to an open file or directory.  Operations
 *      include "read", "write", and "readdir".
 *
 *   When Linux wants to perform some file system operation,
 *   it calls a function pointer provided by the file system type.
 *   (Thus, Linux file systems are object oriented!)
 *
 *   These function pointers are grouped into structures called "operations"
 *   structures.
 *
 *   The initial portion of the file declares all the operations structures we
 *   need to support ospfsmod: one for the superblock, several for different
 *   kinds of inodes and files.  There are separate inode_operations and
 *   file_operations structures for OSPFS directories and for regular OSPFS
 *   files.  The structures are actually defined near the bottom of this file.
 */

// Basic file system type structure
// (links into Linux's list of file systems it supports)
static struct file_system_type ospfs_fs_type;
// Inode and file operations for regular files
static struct inode_operations ospfs_reg_inode_ops;
static struct file_operations ospfs_reg_file_ops;
// Inode and file operations for directories
static struct inode_operations ospfs_dir_inode_ops;
static struct file_operations ospfs_dir_file_ops;
// Inode operations for symbolic links
static struct inode_operations ospfs_symlink_inode_ops;
// Other required operations
static struct dentry_operations ospfs_dentry_ops;
static struct super_operations ospfs_superblock_ops;



/*****************************************************************************
 * BITVECTOR OPERATIONS
 *
 *   OSPFS uses a free bitmap to keep track of free blocks.
 *   These bitvector operations, which set, clear, and test individual bits
 *   in a bitmap, may be useful.
 */

// bitvector_set -- Set 'i'th bit of 'vector' to 1.
static inline void
bitvector_set(void *vector, int i)
{
	((uint32_t *) vector) [i / 32] |= (1 << (i % 32));
}

// bitvector_clear -- Set 'i'th bit of 'vector' to 0.
static inline void
bitvector_clear(void *vector, int i)
{
	((uint32_t *) vector) [i / 32] &= ~(1 << (i % 32));
}

// bitvector_test -- Return the value of the 'i'th bit of 'vector'.
static inline int
bitvector_test(const void *vector, int i)
{
	return (((const uint32_t *) vector) [i / 32] & (1 << (i % 32))) != 0;
}



/*****************************************************************************
 * OSPFS HELPER FUNCTIONS
 */

// ospfs_size2nblocks(size)
//	Returns the number of blocks required to hold 'size' bytes of data.
//
//   Input:   size -- file size
//   Returns: a number of blocks

uint32_t
ospfs_size2nblocks(uint32_t size)
{
	return (size + OSPFS_BLKSIZE - 1) / OSPFS_BLKSIZE;
}


// ospfs_block(blockno)
//	Use this function to load a block's contents from "disk".
//
//   Input:   blockno -- block number
//   Returns: a pointer to that block's data

static void *
ospfs_block(uint32_t blockno)
{
	return &ospfs_data[blockno * OSPFS_BLKSIZE];
}


// ospfs_inode(ino)
//	Use this function to load a 'ospfs_inode' structure from "disk".
//
//   Input:   ino -- inode number
//   Returns: a pointer to the corresponding ospfs_inode structure

static inline ospfs_inode_t *
ospfs_inode(ino_t ino)
{
	ospfs_inode_t *oi;
	if (ino >= ospfs_super->os_ninodes)
		return 0;
	oi = ospfs_block(ospfs_super->os_firstinob);
	return &oi[ino];
}


// ospfs_inode_blockno(oi, offset)
//	Use this function to look up the blocks that are part of a file's
//	contents.
//
//   Inputs:  oi     -- pointer to a OSPFS inode
//	      offset -- byte offset into that inode
//   Returns: the block number of the block that contains the 'offset'th byte
//	      of the file

static inline uint32_t
ospfs_inode_blockno(ospfs_inode_t *oi, uint32_t offset)
{
	uint32_t blockno = offset / OSPFS_BLKSIZE;
	if (offset >= oi->oi_size || oi->oi_ftype == OSPFS_FTYPE_SYMLINK)
		return 0;
	else if (blockno >= OSPFS_NDIRECT + OSPFS_NINDIRECT) {
		uint32_t blockoff = blockno - (OSPFS_NDIRECT + OSPFS_NINDIRECT);
		uint32_t *indirect2_block = ospfs_block(oi->oi_indirect2);
		uint32_t *indirect_block = ospfs_block(indirect2_block[blockoff / OSPFS_NINDIRECT]);
		return indirect_block[blockoff % OSPFS_NINDIRECT];
	} else if (blockno >= OSPFS_NDIRECT) {
		uint32_t *indirect_block = ospfs_block(oi->oi_indirect);
		return indirect_block[blockno - OSPFS_NDIRECT];
	} else {
		return oi->oi_direct[blockno];
	}
}


// ospfs_inode_data(oi, offset)
//	Use this function to load part of inode's data from "disk",
//	where 'offset' is relative to the first byte of inode data.
//
//   Inputs:  oi     -- pointer to a OSPFS inode
//	      offset -- byte offset into 'oi's data contents
//   Returns: a pointer to the 'offset'th byte of 'oi's data contents
//
//	Be careful the returned pointer is only valid within a single block.
//	This function is a simple combination of 'ospfs_inode_blockno'
//	and 'ospfs_block'.

static inline void *
ospfs_inode_data(ospfs_inode_t *oi, uint32_t offset)
{
	uint32_t blockno = ospfs_inode_blockno(oi, offset);
	return (uint8_t *) ospfs_block(blockno) + (offset % OSPFS_BLKSIZE);
}


/*****************************************************************************
 * LOW-LEVEL FILE SYSTEM FUNCTIONS
 * There are no exercises in this section, and you don't need to understand
 * the code.
 */

// ospfs_mk_linux_inode(sb, ino)
//	Linux's in-memory 'struct inode' structure represents disk
//	objects (files and directories).  Many file systems have their own
//	notion of inodes on disk, and for such file systems, Linux's
//	'struct inode's are like a cache of on-disk inodes.
//
//	This function takes an inode number for the OSPFS and constructs
//	and returns the corresponding Linux 'struct inode'.
//
//   Inputs:  sb  -- the relevant Linux super_block structure (one per mount)
//	      ino -- OSPFS inode number
//   Returns: 'struct inode'

static struct inode *
ospfs_mk_linux_inode(struct super_block *sb, ino_t ino)
{
	ospfs_inode_t *oi = ospfs_inode(ino);
	struct inode *inode;

	if (!oi)
		return 0;
	if (!(inode = new_inode(sb)))
		return 0;

	inode->i_ino = ino;
	// Make it look like everything was created by root.
	inode->i_uid = inode->i_gid = 0;
	inode->i_size = oi->oi_size;

	if (oi->oi_ftype == OSPFS_FTYPE_REG) {
		// Make an inode for a regular file.
		inode->i_mode = oi->oi_mode | S_IFREG;
		inode->i_op = &ospfs_reg_inode_ops;
		inode->i_fop = &ospfs_reg_file_ops;
		inode->i_nlink = oi->oi_nlink;

	} else if (oi->oi_ftype == OSPFS_FTYPE_DIR) {
		// Make an inode for a directory.
		inode->i_mode = oi->oi_mode | S_IFDIR;
		inode->i_op = &ospfs_dir_inode_ops;
		inode->i_fop = &ospfs_dir_file_ops;
		inode->i_nlink = oi->oi_nlink + 1 /* dot-dot */;

	} else if (oi->oi_ftype == OSPFS_FTYPE_SYMLINK) {
		// Make an inode for a symbolic link.
		inode->i_mode = S_IRUSR | S_IRGRP | S_IROTH
			| S_IWUSR | S_IWGRP | S_IWOTH
			| S_IXUSR | S_IXGRP | S_IXOTH | S_IFLNK;
		inode->i_op = &ospfs_symlink_inode_ops;
		inode->i_nlink = oi->oi_nlink;

	} else
		panic("OSPFS: unknown inode type!");

	// Access and modification times are now.
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	return inode;
}


// ospfs_fill_super, ospfs_get_sb
//	These functions are called by Linux when the user mounts a version of
//	the OSPFS onto some directory.  They help construct a Linux
//	'struct super_block' for that file system.

static int
ospfs_fill_super(struct super_block *sb, void *data, int flags)
{
	struct inode *root_inode;

	sb->s_blocksize = OSPFS_BLKSIZE;
	sb->s_blocksize_bits = OSPFS_BLKSIZE_BITS;
	sb->s_magic = OSPFS_MAGIC;
	sb->s_op = &ospfs_superblock_ops;

	if (!(root_inode = ospfs_mk_linux_inode(sb, OSPFS_ROOT_INO))
	    || !(sb->s_root = d_alloc_root(root_inode))) {
		iput(root_inode);
		sb->s_dev = 0;
		return -ENOMEM;
	}

	return 0;
}

static int
ospfs_get_sb(struct file_system_type *fs_type, int flags, const char *dev_name, void *data, struct vfsmount *mount)
{
	return get_sb_single(fs_type, flags, data, ospfs_fill_super, mount);
}


// ospfs_delete_dentry
//	Another bookkeeping function.

static int
ospfs_delete_dentry(struct dentry *dentry)
{
	return 1;
}


/*****************************************************************************
 * DIRECTORY OPERATIONS
 *
 * EXERCISE: Finish 'ospfs_dir_readdir' and 'ospfs_symlink'.
 */

// ospfs_dir_lookup(dir, dentry, ignore)
//	This function implements the "lookup" directory operation, which
//	looks up a named entry.
//
//	We have written this function for you.
//
//   Input:  dir    -- The Linux 'struct inode' for the directory.
//		       You can extract the corresponding 'ospfs_inode_t'
//		       by calling 'ospfs_inode' with the relevant inode number.
//	     dentry -- The name of the entry being looked up.
//   Effect: Looks up the entry named 'dentry'.  If found, attaches the
//	     entry's 'struct inode' to the 'dentry'.  If not found, returns
//	     a "negative dentry", which has no inode attachment.

static struct dentry *
ospfs_dir_lookup(struct inode *dir, struct dentry *dentry, struct nameidata *ignore)
{
	// Find the OSPFS inode corresponding to 'dir'
	ospfs_inode_t *dir_oi = ospfs_inode(dir->i_ino);
	struct inode *entry_inode = NULL;
	int entry_off;

	// Make sure filename is not too long
	if (dentry->d_name.len > OSPFS_MAXNAMELEN)
		return (struct dentry *) ERR_PTR(-ENAMETOOLONG);

	// Mark with our operations
	dentry->d_op = &ospfs_dentry_ops;

	// Search through the directory block
	for (entry_off = 0; entry_off < dir_oi->oi_size;
	     entry_off += OSPFS_DIRENTRY_SIZE) {
		// Find the OSPFS inode for the entry
		ospfs_direntry_t *od = ospfs_inode_data(dir_oi, entry_off);

		// Set 'entry_inode' if we find the file we are looking for
		if (od->od_ino > 0
		    && strlen(od->od_name) == dentry->d_name.len
		    && memcmp(od->od_name, dentry->d_name.name, dentry->d_name.len) == 0) {
			entry_inode = ospfs_mk_linux_inode(dir->i_sb, od->od_ino);
			if (!entry_inode)
				return (struct dentry *) ERR_PTR(-EINVAL);
			break;
		}
	}

	// We return a dentry whether or not the file existed.
	// The file exists if and only if 'entry_inode != NULL'.
	// If the file doesn't exist, the dentry is called a "negative dentry".

	// d_splice_alias() attaches the inode to the dentry.
	// If it returns a new dentry, we need to set its operations.
	if ((dentry = d_splice_alias(entry_inode, dentry)))
		dentry->d_op = &ospfs_dentry_ops;
	return dentry;
}


// ospfs_dir_readdir(filp, dirent, filldir)
//   This function is called when the kernel reads the contents of a directory
//   (i.e. when file_operations.readdir is called for the inode).
//
//   Inputs:  filp	-- The 'struct file' structure correspoding to
//			   the open directory.
//			   The most important member is 'filp->f_pos', the
//			   File POSition.  This remembers how far into the
//			   directory we are, so if the user calls 'readdir'
//			   twice, we don't forget our position.
//			   This function must update 'filp->f_pos'.
//	      dirent	-- Used to pass to 'filldir'.
//	      filldir	-- A pointer to a callback function.
//			   This function should call 'filldir' once for each
//			   directory entry, passing it six arguments:
//		  (1) 'dirent'.
//		  (2) The directory entry's name.
//		  (3) The length of the directory entry's name.
//		  (4) The 'f_pos' value corresponding to the directory entry.
//		  (5) The directory entry's inode number.
//		  (6) DT_REG, for regular files; DT_DIR, for subdirectories;
//		      or DT_LNK, for symbolic links.
//			   This function should stop returning directory
//			   entries either when the directory is complete, or
//			   when 'filldir' returns < 0, whichever comes first.
//
//   Returns: 1 at end of directory, 0 if filldir returns < 0 before the end
//     of the directory, and -(error number) on error.
//
//   EXERCISE: Finish implementing this function.

static int
ospfs_dir_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
	struct inode *dir_inode = filp->f_dentry->d_inode;
	ospfs_inode_t *dir_oi = ospfs_inode(dir_inode->i_ino);
	uint32_t f_pos = filp->f_pos;
	int r = 0;		/* Error return value, if any */
	int ok_so_far = 0;	/* Return value from 'filldir' */

	// f_pos is an offset into the directory's data, plus two.
	// The "plus two" is to account for "." and "..".
	if (r == 0 && f_pos == 0) {
		ok_so_far = filldir(dirent, ".", 1, f_pos, dir_inode->i_ino, DT_DIR);
		if (ok_so_far >= 0)
			f_pos++;
	}

	if (r == 0 && ok_so_far >= 0 && f_pos == 1) {
		ok_so_far = filldir(dirent, "..", 2, f_pos, filp->f_dentry->d_parent->d_inode->i_ino, DT_DIR);
		if (ok_so_far >= 0)
			f_pos++;
	}

	// actual entries
	while (r == 0 && ok_so_far >= 0 && f_pos >= 2) {
		ospfs_direntry_t *od;
		ospfs_inode_t *entry_oi;
		int inode_type;

		/* If at the end of the directory, set 'r' to 1 and exit
		 * the loop.  For now we do this all the time.
		 *
		 * EXERCISE: Your code here */
		// Check if we have reached the end of the file
		if(dir_oi->oi_size < (f_pos - 2)*sizeof(ospfs_direntry_t)) {
			r = 1;
			break;
		}

		/* Get a pointer to the next entry (od) in the directory.
		 * The file system interprets the contents of a
		 * directory-file as a sequence of ospfs_direntry structures.
		 * You will find 'f_pos' and 'ospfs_inode_data' useful.
		 *
		 * Then use the fields of that file to fill in the directory
		 * entry.  To figure out whether a file is a regular file or
		 * another directory, use 'ospfs_inode' to get the directory
		 * entry's corresponding inode, and check out its 'oi_ftype'
		 * member.
		 *
		 * Make sure you ignore blank directory entries!  (Which have
		 * an inode number of 0.)
		 *
		 * If the current entry is successfully read (the call to
		 * filldir returns >= 0), or the current entry is skipped,
		 * your function should advance f_pos by the proper amount to
		 * advance to the next directory entry.
		 */

		/* EXERCISE: Your code here */
		od = ospfs_inode_data(dir_oi, (f_pos - 2)*sizeof(ospfs_direntry_t));
		if(od->od_ino == 0) {
			f_pos++;
			continue;
		}

		entry_oi = ospfs_inode(od->od_ino);
		if(entry_oi->oi_ftype == OSPFS_FTYPE_REG) {
			inode_type = DT_REG;
		}
		else if(entry_oi->oi_ftype == OSPFS_FTYPE_DIR) {
			inode_type = DT_DIR;
		}
		else if(entry_oi->oi_ftype == OSPFS_FTYPE_SYMLINK) {
			inode_type = DT_LNK;
		}
		else {
			panic("OSPFS: unknown inode type!");
		}

		ok_so_far = filldir(dirent, od->od_name, OSPFS_MAXNAMELEN, f_pos, od->od_ino, inode_type);
		
		// Check if we exit early
		if(ok_so_far < 0) {
			r = 0;
			break;
		}


		f_pos++;
	}

	// Save the file position and return!
	filp->f_pos = f_pos;
	return r;
}


// ospfs_unlink(dirino, dentry)
//   This function is called to remove a file.
//
//   Inputs: dirino  -- You may ignore this.
//           dentry  -- The 'struct dentry' structure, which contains the inode
//                      the directory entry points to and the directory entry's
//                      directory.
//
//   Returns: 0 if success and -ENOENT on entry not found.
//
//   EXERCISE: Make sure that deleting symbolic links works correctly.

static int
ospfs_unlink(struct inode *dirino, struct dentry *dentry)
{
	ospfs_inode_t *oi = ospfs_inode(dentry->d_inode->i_ino);
	ospfs_inode_t *dir_oi = ospfs_inode(dentry->d_parent->d_inode->i_ino);
	int entry_off;
	ospfs_direntry_t *od;
	od = NULL; // silence compiler warning; entry_off indicates when !od
	for (entry_off = 0; entry_off < dir_oi->oi_size;
	     entry_off += OSPFS_DIRENTRY_SIZE) {
		od = ospfs_inode_data(dir_oi, entry_off);
		if (od->od_ino > 0
		    && strlen(od->od_name) == dentry->d_name.len
		    && memcmp(od->od_name, dentry->d_name.name, dentry->d_name.len) == 0)
			break;
	}

	if (entry_off == dir_oi->oi_size) {
		printk("<1>ospfs_unlink should not fail!\n");
		return -ENOENT;
	}


	od->od_ino = 0;
	oi->oi_nlink--;

	// Check for symlinks
	if(oi->oi_ftype == OSPFS_FTYPE_SYMLINK) {
		memset(oi, 0, sizeof(ospfs_symlink_inode_t));
		return 0;
	}

	// Check if we can free the blocks
	if(oi->oi_nlink == 0) {

		change_size(oi, dentry->d_inode->i_ino, 0);
	}

	return 0;
}



/*****************************************************************************
 * FREE-BLOCK BITMAP OPERATIONS
 *
 * EXERCISE: Implement these functions.
 */

// allocate_block()
//	Use this function to allocate a block.
//
//   Inputs:  none
//   Returns: block number of the allocated block,
//	      or 0 if the disk is full
//
//   This function searches the free-block bitmap, which starts at Block 2, for
//   a free block, allocates it (by marking it non-free), and returns the block
//   number to the caller.  The block itself is not touched.
//
//   Note:  A value of 0 for a bit indicates the corresponding block is
//      allocated; a value of 1 indicates the corresponding block is free.
//
//   You can use the functions bitvector_set(), bitvector_clear(), and
//   bitvector_test() to do bit operations on the map.
#define OSPFS_FIRST_VALID_BLOCK ospfs_super->os_firstdatab

// Find a free block NOT within range min to max
static uint32_t
find_free_block(uint32_t lower_bound, uint32_t upper_bound)
{
	int blockno;
	uint32_t * bitvector = ospfs_block(2);

	// Start with the upper bound and using modulo arithmetic wrap 
	// around the the lower bound, and see if there are any free blocks 
	// in between
	blockno = upper_bound;
	while(blockno != lower_bound) {
		if(bitvector_test(bitvector, blockno)) {
			return blockno;
		}
		blockno = (blockno + 1) % ospfs_super->os_nblocks;
	}

	// Couldn't find any free blocks
	return 0;
}

int
allocate_blockno(uint32_t blockno)
{
	uint32_t * bitvector = ospfs_block(2);
	// Check that the range is valid and clear the bit
	if(ospfs_super->os_firstdatab < blockno && 
				blockno < ospfs_super->os_nblocks) {
		bitvector_clear(bitvector, blockno);
	}
	return 0;
}


// free_block(blockno)
//	Use this function to free an allocated block.
//
//   Inputs:  blockno -- the block number to be freed
//   Returns: none
//
//   This function should mark the named block as free in the free-block
//   bitmap.  (You might want to program defensively and make sure the block
//   number isn't obviously bogus: the boot sector, superblock, free-block
//   bitmap, and inode blocks must never be freed.  But this is not required.)

static void
free_block(uint32_t blockno)
{
	/* EXERCISE: Your code here */
	uint32_t * bitvector = ospfs_block(2);
	if(ospfs_super->os_nblocks < blockno || blockno < OSPFS_FIRST_VALID_BLOCK) { // Check for validity
		return;
	}

	bitvector_set(bitvector, blockno);
}


/*****************************************************************************
 * FILE OPERATIONS
 *
 * EXERCISE: Finish off change_size, read, and write.
 *
 * The find_*, add_block, and remove_block functions are only there to support
 * the change_size function.  If you prefer to code change_size a different
 * way, then you may not need these functions.
 *
 */

// Returns -1 if not in indirect2 block range, 0 if it is
int block_indirect2_index(uint32_t blockno)
{
	return ((blockno < OSPFS_NDIRECT + OSPFS_NINDIRECT) ? -1 : 0);
}
// Returns -1 if not in indirect or direct block range, or the index >= 0 of
// blockno if it is
int block_indirect_index(uint32_t blockno)
{
	// Check if in direct block range
	if(blockno < OSPFS_NDIRECT)
		return -1;
	blockno -= OSPFS_NDIRECT;

	// Check if in the indirect block range
	if(blockno < OSPFS_NINDIRECT)
		return 0;
	blockno -= OSPFS_NINDIRECT;

	// Now in the indirect2 block range
	return blockno / OSPFS_NINDIRECT;
}
// Returns the direct block index of blockno
int block_direct_index(uint32_t blockno)
{
	if(blockno < OSPFS_NDIRECT)
		return blockno;
	blockno -= OSPFS_NDIRECT;
	return blockno % OSPFS_NINDIRECT;
}

// Initializes a file_index_t based on a given inode
int
init_file_index(ospfs_inode_t *oi, file_index_t *idx)
{
	// Current number of blocks in file
	idx->blk_size = ospfs_size2nblocks(oi->oi_size);

	// Indexes of the direct, indirect, and indirect2 blocks
	idx->indir2_idx = block_indirect2_index(idx->blk_size);
	idx->indir_idx = block_indirect_index(idx->blk_size);
	idx->dir_idx = block_direct_index(idx->blk_size);

	return 0;
}

// For debugging, print block
void print_super(void) {
	eprintk("Sizes:\n");
	eprintk("Super nblocks: %d\n", ospfs_super->os_nblocks);
	eprintk("Super ninodes: %d\n", ospfs_super->os_ninodes);
	eprintk("Super firstinob: %d\n", ospfs_super->os_firstinob);
	eprintk("Super firstjournalb: %d\n", ospfs_super->os_firstjournalb);
	eprintk("Super njournalb: %d\n", ospfs_super->os_njournalb);
	eprintk("Super firstdatab: %d\n", ospfs_super->os_firstdatab);
}

// Prepares a resize_request for a file resize. Works regardless of increasing 
// or decreasing file size. Initializes the resize_request based on the 
// current size of the request
// Inputs: oi - copy of the inode we will change (represents what the inode 
//              will be)
//         r - request_struct to contain relevant information for the resize
// Returns: 0 on success, error code on failure
int
init_resize_request(ospfs_inode_t *oi, resize_request *r)
{
	// Initialize file index
	init_file_index(oi, &r->index);

	// In case we are growing size
	r->lower_bound = ospfs_super->os_firstdatab - 1;
	r->upper_bound = ospfs_super->os_firstdatab;

	r->indirect_blockno = 0;
	r->indirect2_blockno = 0;
	// See if we need to fill the indirect and doubly indirect blocks
	if(r->index.indir2_idx == 0) {
		r->indirect2_blockno = oi->oi_indirect2;
		memcpy(&r->indirect2_block, ospfs_block(r->indirect2_blockno), 
			OSPFS_BLKSIZE);
	}
	else { // zero out the doubly indirect block buffer
		memset(&r->indirect2_block, 0, OSPFS_BLKSIZE);
	}

	// See if we need to copy over the indirect block
	// Doubly indirect block range
	if(r->index.indir2_idx == 0) {
		r->indirect_blockno = r->indirect2_block[r->index.indir2_idx];
		memcpy(&r->indirect_block, ospfs_block(r->indirect_blockno), 
			OSPFS_BLKSIZE);
	}
	// Indirect block range
	else if(r->index.indir2_idx < 0 && r->index.indir_idx == 0) {
		r->indirect_blockno = oi->oi_indirect;
		memcpy(&r->indirect_block, ospfs_block(r->indirect_blockno), 
			OSPFS_BLKSIZE);
	}
	else { // zero out the indirect block buffer
		memset(&r->indirect_block, 0, OSPFS_BLKSIZE);
	}

	// Not changing doubly indirect or indirect blocks yet
	r->resize_type = 0;

	// No blocks affected yet
	r->n = 0;
	memset(&r->blocknos, 0, JOURNAL_MAX_BLOCKS*(sizeof(uint32_t)));

	return 0;
}

// Book-keeping funciton for updating the bounds for block checking for
//  allocating blocks
void
update_bounds(resize_request *r, uint32_t new_num)
{
	if(r->n == 0) {
		r->lower_bound = new_num;
		r->upper_bound = new_num + 1;
	}
	else {
		r->upper_bound = new_num + 1;
	}
}

// Free one memory block in oi
// Inputs: oi - copy of the inode we will change (represents what the inode 
//              will be)
//         r - request_struct containing relevant information for the resize
// Preconditions: Assumes we are not going make r free more than 
//                JOURNAL_MAX_BLOCKS
// Returns: 0 on success, error code on failure
int
free_block_file(ospfs_inode_t *oi, resize_request *r)
{
	file_index_t *idx;
	// File index for reference
	idx = &r->index;
	init_file_index(oi, idx);

	// Direct block range
	if(idx->indir2_idx < 0 && idx->indir_idx < 0) {
		r->blocknos[r->n] = oi->oi_direct[idx->dir_idx];
		oi->oi_direct[idx->dir_idx] = 0;
		r->n++;
	}
	// Indirect or Doubly indirect block range - assumes that r's
	//   indirect_block and indirect2_block have been written to
	else {
		r->blocknos[r->n] = r->indirect_block[idx->dir_idx];
		r->indirect_block[idx->dir_idx] = 0;
		r->n++;

		// Check if we can free the indirect block
		if(idx->dir_idx == 0) {
			// Check if its a doubly indirect or indirect block that we 
			//   need to free
			if(idx->indir2_idx == 0) {
				r->indirect2_block[idx->indir_idx] = 0;
			}
			else {
				oi->oi_indirect = 0;
			}

			// Tell the resize request that we are freeing the indirect block
			r->resize_type |= JOURNAL_RESIZE_INDIRECT;
			if(idx->indir2_idx == 0 && idx->indir_idx == 0) {
				// Zero out the doubly indirect block
				oi->oi_indirect2 = 0;
				r->resize_type |= JOURNAL_RESIZE_INDIRECT2;
			}
		}
	}
	/* EXERCISE: Your code here */
	oi->oi_size = (idx->blk_size - 1)*OSPFS_BLKSIZE;
	return 0; // Replace this line
}

// Add one memory block to file
int
add_block_file(ospfs_inode_t *oi, resize_request *r)
{
	file_index_t *idx;
	// File index for reference
	idx = &r->index;
	init_file_index(oi, idx);

	// Get the next free block
	r->blocknos[r->n] =
		find_free_block(r->lower_bound, r->upper_bound);

	// Check if there is any space left, then update min and max
	if(r->blocknos[r->n] == 0)
		return -ENOSPC;
	update_bounds(r, r->blocknos[r->n]);

	// Check if we need to allocate the doubly indirect block
	if(idx->indir2_idx == 0 && idx->indir_idx == 0 && idx->dir_idx == 0) {
		// Notify that we allocated the doubly indirect block
		r->resize_type |= JOURNAL_RESIZE_INDIRECT2;

		// Check that we are the first on
		// If we do this when we are not the first block, we may over-ride the 
		//  existing indirect block
		if(r->n != 0) {
			r->blocknos[r->n] = 0;
			return 0;
		}

		// Allocate a new block for the doubly indirect block
		r->indirect2_blockno = 
			find_free_block(r->lower_bound, r->upper_bound);
		
		// Check if there is any space left and update bounds
		if(r->indirect2_blockno == 0)
			return -ENOSPC;
		update_bounds(r, r->indirect2_blockno);

		// Change the inode
		oi->oi_indirect2 = r->indirect2_blockno;
	}

	// Check if we need to do the indirect block
	if(idx->indir_idx >= 0 && idx->dir_idx == 0) {
		// Notify that we need to allocate the indirect block
		r->resize_type |= JOURNAL_RESIZE_INDIRECT;

		// Check that we are the first on
		// If we do this when we are not the first block, we may over-ride the 
		//  existing indirect block
		if(r->n != 0) {
			r->blocknos[r->n] = 0;
			return 0;
		}

		// Allocate a new block for the indirect block
		r->indirect_blockno = 
			find_free_block(r->lower_bound, r->upper_bound);

		// Check if there is any space left
		if(r->indirect_blockno == 0)
			return -ENOSPC;
		update_bounds(r, r->indirect_blockno);

		// Check if its a doubly indirect or indirect block that we 
		//   need to allocate
		if(idx->indir2_idx == 0) {
			r->indirect2_block[idx->indir_idx] = r->indirect_blockno;
		}
		else { // In indirect block range
			oi->oi_indirect = r->indirect_blockno;
		}
	}

	// Change the inode to match the new size
	if(idx->indir2_idx < 0 && idx->indir_idx < 0) { 
		// Direct block range
		oi->oi_direct[idx->dir_idx] = r->blocknos[r->n];
	}
	else {
		// Indirect or Doubly indirect block range
		r->indirect_block[idx->dir_idx] = r->blocknos[r->n];
	}

	r->n++;
	oi->oi_size = (idx->blk_size + 1)*OSPFS_BLKSIZE;

	return 0; // Replace this line
}


// Change size journal operations
int
change_size_to_journal(journal_header_t *header, resize_request *r)
{
	journal_header_t *journal_header;
	// Set up header
	header->n_blocks_affected = r->n;
	header->indirect2_blockno = r->indirect2_blockno;
	header->indirect_blockno = r->indirect_blockno;
	header->file_resize_type = r->resize_type;

	// Write header to journal
	memcpy(ospfs_block(ospfs_super->os_firstjournalb + JOURNAL_HEADER_POS),
			header, (sizeof(journal_header_t)));
	// Now journal has officially started

	// Copy block list
	memcpy(ospfs_block(ospfs_super->os_firstjournalb +
		JOURNAL_BLOCKNO_LIST_POS), &r->blocknos,
		JOURNAL_MAX_BLOCKS * (sizeof(uint32_t)));

	// Copy over the indirect block, if needed
	if(r->indirect_blockno != 0) {
		memcpy(ospfs_block(ospfs_super->os_firstjournalb + 
			JOURNAL_INDIR_BLOCK_POS), &r->indirect_block, OSPFS_BLKSIZE);
	}

	// Copy doubly indirect block, if needed
	if(r->indirect2_blockno != 0) {
		memcpy(ospfs_block(ospfs_super->os_firstjournalb + 
			JOURNAL_INDIR2_BLOCK_POS), &r->indirect2_block, OSPFS_BLKSIZE);
	}

	// Set the completed journal flag
	journal_header = (journal_header_t*)
			ospfs_block(ospfs_super->os_firstjournalb + JOURNAL_HEADER_POS);
	journal_header->completed = 1;

	return 0;
}

// Create direntry journal operation
int
create_to_journal(journal_header_t *header, ospfs_direntry_t *direntries)
{
	journal_header_t *journal_header;

	// Write header to journal
	memcpy(ospfs_block(ospfs_super->os_firstjournalb + JOURNAL_HEADER_POS),
			header, (sizeof(journal_header_t)));
	// Now journal has officially started

	// Copy the directory data over
	memcpy(ospfs_block(ospfs_super->os_firstjournalb +
		JOURNAL_DATA_BLOCKS_POS), direntries, OSPFS_BLKSIZE);

	// Set the completed journal flag
	journal_header = (journal_header_t*)
			ospfs_block(ospfs_super->os_firstjournalb + JOURNAL_HEADER_POS);
	journal_header->completed = 1;

	return 0;
}

// Executes the journal based on whatever is in its header currently
int
execute_journal(void)
{
	int i;
	journal_header_t *journal_header;
	uint32_t *blocknos;
	uint32_t *indirect_block;
	uint32_t *indirect2_block;
	char *data_blocks;
	ospfs_inode_t *oi;

	// Get the journal elements
	journal_header = (journal_header_t*)
			ospfs_block(ospfs_super->os_firstjournalb + JOURNAL_HEADER_POS);
	blocknos = (uint32_t*)
		ospfs_block(ospfs_super->os_firstjournalb + JOURNAL_BLOCKNO_LIST_POS);
	indirect_block = (uint32_t*)
		ospfs_block(ospfs_super->os_firstjournalb + JOURNAL_INDIR_BLOCK_POS);
	indirect2_block = (uint32_t*)
		ospfs_block(ospfs_super->os_firstjournalb + JOURNAL_INDIR2_BLOCK_POS);
	data_blocks = (char *)
		ospfs_block(ospfs_super->os_firstjournalb + JOURNAL_DATA_BLOCKS_POS);

	// Get the inode
	oi = ospfs_inode(journal_header->inode_num);

	// Check what kind of execution we are doing
	if(journal_header->execute_type == JOURNAL_FREE) {
		// Copy over the inode
		*oi = journal_header->inode;

		// Syncronize the doubly indirect block
		if(journal_header->file_resize_type & JOURNAL_RESIZE_INDIRECT2) {
			free_block(journal_header->indirect2_blockno);
		}
		if(journal_header->indirect2_blockno != 0) {
			memcpy(ospfs_block(journal_header->indirect2_blockno),
					indirect2_block, OSPFS_BLKSIZE);
		}

		// Syncronize the indirect block
		if(journal_header->file_resize_type & JOURNAL_RESIZE_INDIRECT) {
			free_block(journal_header->indirect_blockno);
		}
		if(journal_header->indirect_blockno != 0) {
			memcpy(ospfs_block(journal_header->indirect_blockno),
					indirect_block, OSPFS_BLKSIZE);
		}

		// Free the data blocks
		for(i = 0; i < journal_header->n_blocks_affected; i++) {
			free_block(blocknos[i]);
		}

	}
	else if(journal_header->execute_type == JOURNAL_ALLOC) {

		// eprintk("Blocks affected: %d\n", journal_header->n_blocks_affected);
		// Copy over the inode
		*oi = journal_header->inode;
		// eprintk("Increasing size of inode %d\n",
		// 				journal_header->inode_num);

		// Free the data blocks
		for(i = 0; i < journal_header->n_blocks_affected; i++) {
			// eprintk("Allocating block %d\n", blocknos[i]);
			allocate_blockno(blocknos[i]);
		}

		// Syncronize the indirect block
		if(journal_header->file_resize_type & JOURNAL_RESIZE_INDIRECT) {
			allocate_blockno(journal_header->indirect_blockno);
			// eprintk("Allocating indirect block %d\n", 
			// 			journal_header->indirect_blockno);
		}
		if(journal_header->indirect_blockno != 0) {
			memcpy(ospfs_block(journal_header->indirect_blockno),
					indirect_block, OSPFS_BLKSIZE);
			// eprintk("Writing to indirect block...\n");
		}

		// Syncronize the doubly indirect block
		if(journal_header->file_resize_type & JOURNAL_RESIZE_INDIRECT2) {
			allocate_blockno(journal_header->indirect2_blockno);
			// eprintk("Allocating indirect2 block %d\n", 
			// 		journal_header->indirect2_blockno);
		}
		if(journal_header->indirect2_blockno != 0) {
			memcpy(ospfs_block(journal_header->indirect2_blockno),
					indirect2_block, OSPFS_BLKSIZE);
			// eprintk("Writing to indirect2 block...\n");
		}

	}
	else if(journal_header->execute_type == JOURNAL_WRITE) {
		for(i = 0; i < journal_header->n_blocks_affected; i++) {
			memcpy(ospfs_block(blocknos[i]),
				ospfs_block(ospfs_super->os_firstjournalb + 
					JOURNAL_DATA_BLOCKS_POS + i), OSPFS_BLKSIZE);
		}
	}
	else if(journal_header->execute_type == JOURNAL_HRDLNK
		|| journal_header->execute_type == JOURNAL_CREATE) {
		// Copy over inode
		*oi = journal_header->inode;

		// Copy over the directory data block
		memcpy(ospfs_block(journal_header->dir_data_blockno), 
			ospfs_block(ospfs_super->os_firstjournalb + 
				JOURNAL_DATA_BLOCKS_POS), OSPFS_BLKSIZE);
	}

	// Done our tasks, now empty the journal
	journal_header->completed = 0;
	journal_header->execute_type = JOURNAL_EMPTY;
	return 0;
}

// Freeing memory of inode_num to size new_size
static int
free_memory(uint32_t inode_num, uint32_t new_size)
{
	int error; // error codes
	uint32_t desired_size;
	journal_header_t header;
	ospfs_inode_t *oi;
	resize_request r; // To keep track of what we have to do

	// Get the inode
	oi = ospfs_inode(inode_num);

	memset(&header, 0, sizeof(journal_header_t));
	header.inode = *oi; // make copy of inode
	header.inode_num = inode_num;
	header.execute_type = JOURNAL_FREE; // Freeing memory

	desired_size = ospfs_size2nblocks(new_size);
	while(header.inode.oi_size > new_size) {
		// Initialize the change size request
		error = init_resize_request(&header.inode, &r);
		if(error < 0) {
			return error;
		}

		while(r.n < JOURNAL_MAX_BLOCKS && // only can do 256 blocks at once
				ospfs_size2nblocks(header.inode.oi_size) > desired_size) {
			error = free_block_file(&header.inode, &r);
			if(error < 0) {
				return error;
			}
			// Check to see if we hit an edge of a indirect block
			if(r.resize_type & JOURNAL_RESIZE_INDIRECT) {
				break;
			}
		}

		// For the case of completely removing the file
		// NO, there is not a more elegant way to do this. Sorry.
		if(header.inode.oi_size == 0 && desired_size == 0) {
			r.blocknos[r.n] = header.inode.oi_direct[0];
			header.inode.oi_direct[0] = 0;
			r.n++;
		}
		else if(ospfs_size2nblocks(header.inode.oi_size) <= desired_size) {
			header.inode.oi_size = new_size;
		}

		// Write out information to journal
		error = change_size_to_journal(&header, &r);
		if(error < 0)
			return error;

		// Execute the journal
		error = execute_journal();
		if(error < 0)
			return error;
	}

	return 0;
}

static int
grow_size(uint32_t inode_num, uint32_t new_size)
{
	int error; // error codes
	uint32_t desired_size;
	journal_header_t header;
	ospfs_inode_t *oi;
	resize_request r; // To keep track of what we have to do

	// Get the inode
	oi = ospfs_inode(inode_num);

	memset(&header, 0, sizeof(journal_header_t));
	header.inode = *oi; // make copy of inode
	header.inode_num = inode_num;
	header.execute_type = JOURNAL_ALLOC; // Freeing memory

	desired_size = ospfs_size2nblocks(new_size);
	while(header.inode.oi_size < new_size) {
		// Initialize the change size request
		error = init_resize_request(&header.inode, &r);
		if(error < 0) {
			break;
		}

		while(r.n < JOURNAL_MAX_BLOCKS && // only can do 256 blocks at once
				ospfs_size2nblocks(header.inode.oi_size) < desired_size) {
			error = add_block_file(&header.inode, &r);
			if(error < 0) {
				return error;
			}
			// Check to see if we hit an edge of a indirect block
			if(r.resize_type & JOURNAL_RESIZE_INDIRECT ||
				r.resize_type & JOURNAL_RESIZE_INDIRECT2) {
				break;
			}
		}
		// Change the size if appropriate
		if(ospfs_size2nblocks(header.inode.oi_size) >= desired_size) {
			header.inode.oi_size = new_size;
		}

		// Write out information to journal
		error = change_size_to_journal(&header, &r);
		if(error < 0)
			return error;

		// Execute the journal
		error = execute_journal();
		if(error < 0)
			return error;
	}

	return 0;
}


// change_size(oi, want_size)
//	Use this function to change a file's size, allocating and freeing
//	blocks as necessary.
//
//   Inputs:  oi	-- pointer to the file whose size we're changing
//	      want_size -- the requested size in bytes
//   Returns: 0 on success, < 0 on error.  In particular:
//		-ENOSPC: if there are no free blocks available
//		-EIO:    an I/O error -- for example an indirect block should
//			 exist, but doesn't
//	      If the function succeeds, the file's oi_size member should be
//	      changed to want_size, with blocks allocated as appropriate.
//	      Any newly-allocated blocks should be erased (set to 0).
//	      If there is an -ENOSPC error when growing a file,
//	      the file size and allocated blocks should not change from their
//	      original values!!!
//            (However, if there is an -EIO error, do not worry too much about
//	      restoring the file.)
//
//   If want_size has the same number of blocks as the current file, life
//   is good -- the function is pretty easy.  But the function might have
//   to add or remove blocks.
//
//   If you need to grow the file, then do so by adding one block at a time
//   using the add_block function you coded above. If one of these additions
//   fails with -ENOSPC, you must shrink the file back to its original size!
//
//   If you need to shrink the file, remove blocks from the end of
//   the file one at a time using the remove_block function you coded above.
//
//   Also: Don't forget to change the size field in the metadata of the file.
//         (The value that the final add_block or remove_block set it to
//          is probably not correct).
//
//   EXERCISE: Finish off this function.

static int
change_size(ospfs_inode_t *oi, uint32_t inode_num, uint32_t new_size)
{
	// Simple check to make sure we don't try to go over the file size limit
	if(OSPFS_MAXFILESIZE < new_size)
		return -ENOSPC;

	if(new_size < oi->oi_size) {
		return free_memory(inode_num, new_size);
	}
	else if(oi->oi_size < new_size) { 
		return grow_size(inode_num, new_size);
	}

	// Don't have to change its size
	return 0;
}


// ospfs_notify_change
//	This function gets called when the user changes a file's size,
//	owner, or permissions, among other things.
//	OSPFS only pays attention to file size changes (see change_size above).
//	We have written this function for you -- except for file quotas.

static int
ospfs_notify_change(struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = dentry->d_inode;
	ospfs_inode_t *oi = ospfs_inode(inode->i_ino);
	int retval = 0;

	if (attr->ia_valid & ATTR_SIZE) {
		// We should not be able to change directory size
		if (oi->oi_ftype == OSPFS_FTYPE_DIR)
			return -EPERM;
		if ((retval = change_size(oi, inode->i_ino, attr->ia_size)) < 0)
			goto out;
	}

	if (attr->ia_valid & ATTR_MODE)
		// Set this inode's mode to the value 'attr->ia_mode'.
		oi->oi_mode = attr->ia_mode;

	if ((retval = inode_change_ok(inode, attr)) < 0
	    || (retval = inode_setattr(inode, attr)) < 0)
		goto out;

    out:
	return retval;
}


// ospfs_read
//	Linux calls this function to read data from a file.
//	It is the file_operations.read callback.
//
//   Inputs:  filp	-- a file pointer
//            buffer    -- a user space ptr where data should be copied
//            count     -- the amount of data requested
//            f_pos     -- points to the file position
//   Returns: Number of chars read on success, -(error code) on error.
//
//   This function copies the corresponding bytes from the file into the user
//   space ptr (buffer).  Use copy_to_user() to accomplish this.
//   The current file position is passed into the function
//   as 'f_pos'; read data starting at that position, and update the position
//   when you're done.
//
//   EXERCISE: Complete this function.


static ssize_t
ospfs_read(struct file *filp, char __user *buffer, size_t count, loff_t *f_pos)
{
	ospfs_inode_t *oi = ospfs_inode(filp->f_dentry->d_inode->i_ino);
	int retval = 0;
	size_t amount = 0;

	// Make sure we don't read past the end of the file!
	// Change 'count' so we never read past the end of the file.
	/* EXERCISE: Your code here */
	if(oi->oi_size <= *f_pos)
		goto done;

	if(oi->oi_size < *f_pos + count)
		count = oi->oi_size - *f_pos;

	// Copy the data to user block by block
	while (amount < count && retval >= 0) {
		uint32_t blockno = ospfs_inode_blockno(oi, *f_pos);
		uint32_t n;
		char *data;

		// ospfs_inode_blockno returns 0 on error
		if (blockno == 0) {
			retval = -EIO;
			goto done;
		}

		data = ospfs_block(blockno);
		// Get to the right position in the block - is this right??
		data += (*f_pos % OSPFS_BLKSIZE);
		
		// Figure out how much data is left in this block to read.
		// Copy data into user space. Return -EFAULT if unable to write
		// into user space.
		// Use variable 'n' to track number of bytes moved.
		/* EXERCISE: Your code here */
		
		// Go only to the end of a block, or the end of the read if before that
		n = MIN(OSPFS_BLKSIZE - (*f_pos % OSPFS_BLKSIZE), (count - amount));

		if(n == 0)
			goto done;

		retval = copy_to_user(buffer, data, n);
		
		if(retval != 0) {
			retval = -EFAULT;
			goto done;
		}

		amount += n;
		buffer += n;
		*f_pos += n;
	}

	done:
	return (retval >= 0 ? amount : retval);
}

// Journal write functions
int
write_to_journal(journal_header_t *header, uint32_t *blocknos,
	uint32_t *blocks_stored)
{
	journal_header_t *journal_header;

	// Write out header (simple for write)
	header->n_blocks_affected = *blocks_stored;
	memcpy(ospfs_block(ospfs_super->os_firstjournalb + JOURNAL_HEADER_POS),
			header, (sizeof(journal_header_t)));

	// Write out blocknos
	memcpy(ospfs_block(ospfs_super->os_firstjournalb +
		JOURNAL_BLOCKNO_LIST_POS), blocknos,
		JOURNAL_MAX_BLOCKS * (sizeof(uint32_t)));	

	// Set the completed journal flag
	journal_header = (journal_header_t*)
			ospfs_block(ospfs_super->os_firstjournalb + JOURNAL_HEADER_POS);
	journal_header->completed = 1;

	return 0;
}

// Restart the write journal header
int
restart_write_journal(journal_header_t *header, uint32_t *blocknos,
	uint32_t *blocks_stored)
{
	journal_header_t *journal_header;

	// Restart block count
	*blocks_stored = 0;

	// Clear the blocknos
	memset(blocknos, 0, OSPFS_BLKSIZE);

	// Set the started flag of the journal
	journal_header = (journal_header_t*)
			ospfs_block(ospfs_super->os_firstjournalb + JOURNAL_HEADER_POS);
	journal_header->execute_type = JOURNAL_WRITE;

	return 0;
}

// ospfs_write
//	Linux calls this function to write data to a file.
//	It is the file_operations.write callback.
//
//   Inputs:  filp	-- a file pointer
//            buffer    -- a user space ptr where data should be copied from
//            count     -- the amount of data to write
//            f_pos     -- points to the file position
//   Returns: Number of chars written on success, -(error code) on error.
//
//   This function copies the corresponding bytes from the user space ptr
//   into the file.  Use copy_from_user() to accomplish this. Unlike read(),
//   where you cannot read past the end of the file, it is OK to write past
//   the end of the file; this should simply change the file's size.
//
//   EXERCISE: Complete this function.

static ssize_t
ospfs_write(struct file *filp, const char __user *buffer, size_t count, 
	loff_t *f_pos)
{
	journal_header_t header;
	ospfs_inode_t *oi;
	char data_buf[OSPFS_BLKSIZE];
	uint32_t inode_num, blocks_stored;
	uint32_t blocknos[JOURNAL_MAX_BLOCKS];
	int retval;
	size_t amount;

	// Initialize variables
	inode_num = filp->f_dentry->d_inode->i_ino;
	oi = ospfs_inode(inode_num);
	blocks_stored = 0;
	retval = 0;
	amount = 0;

	// Support files opened with the O_APPEND flag.  To detect O_APPEND,
	// use struct file's f_flags field and the O_APPEND bit.
	/* EXERCISE: Your code here */
	if(filp->f_flags & O_APPEND) {
		*f_pos = oi->oi_size;
	}

	// If the user is writing past the end of the file, change the file's
	// size to accomodate the request.  (Use change_size().)
	if(oi->oi_size < (*f_pos) + count) {
		// We gotta do something about this one...
		retval = change_size(oi, inode_num, (*f_pos + count));
		if(retval < 0) {
			return retval;
		}
	}

	// Initialize header
	memset(&header, 0, sizeof(journal_header_t));
	header.inode = *oi; // make copy of inode
	header.inode_num = inode_num;
	header.execute_type = JOURNAL_WRITE; // Writing data


	// Copy data block by block
	while (amount < count && retval >= 0) {
		uint32_t blockno, n, pos;

		// Get the block
		blockno = ospfs_inode_blockno(oi, *f_pos);
		if (blockno == 0) {
			retval = -EIO;
			goto done;
		}

		// Get the block number
		blocknos[blocks_stored] = blockno;

		// Copy to buffer
		memcpy(data_buf, ospfs_block(blockno), OSPFS_BLKSIZE);
		// Get to the right position in the block - is this right?
		pos = (*f_pos % OSPFS_BLKSIZE);
		
		// Figure out how much data is left in this block to write.
		// Copy data from user space. Return -EFAULT if unable to read
		// read user space.
		// Keep track of the number of bytes moved in 'n'.

		// Go only to the end of a block, or the end of the read if before that
		n = MIN(OSPFS_BLKSIZE - pos, (count - amount));

		if(n == 0)
			goto done;

		retval = copy_from_user((data_buf + pos), buffer, n);
		
		if(retval != 0) {
			retval = -EFAULT;
			goto done;
		}

		// Write to the journal blocks_stored
		memcpy(ospfs_block(ospfs_super->os_firstjournalb + 
			JOURNAL_DATA_BLOCKS_POS + blocks_stored), data_buf, OSPFS_BLKSIZE);
		blocks_stored++;
		if(blocks_stored == JOURNAL_MAX_BLOCKS) {
			// Data blocks full, execute journal
			write_to_journal(&header, blocknos, &blocks_stored);
			execute_journal();
			restart_write_journal(&header, blocknos, &blocks_stored);
		}

		// Increment the position
		buffer += n;
		amount += n;
		*f_pos += n;
	}

	if(0 < blocks_stored) {
		// Data blocks need to be written, execute journal
		write_to_journal(&header, blocknos, &blocks_stored);
		execute_journal();
	}

    done:
	return (retval >= 0 ? amount : retval);
}


// find_direntry(dir_oi, name, namelen)
//	Looks through the directory to find an entry with name 'name' (length
//	in characters 'namelen').  Returns a pointer to the directory entry,
//	if one exists, or NULL if one does not.
//
//   Inputs:  dir_oi  -- the OSP inode for the directory
//	      name    -- name to search for
//	      namelen -- length of 'name'.  (If -1, then use strlen(name).)
//
//	We have written this function for you.

static ospfs_direntry_t *
find_direntry(ospfs_inode_t *dir_oi, const char *name, int namelen)
{
	int off;
	if (namelen < 0)
		namelen = strlen(name);
	for (off = 0; off < dir_oi->oi_size; off += OSPFS_DIRENTRY_SIZE) {
		ospfs_direntry_t *od = ospfs_inode_data(dir_oi, off);
		if (od->od_ino
		    && strlen(od->od_name) == namelen
		    && memcmp(od->od_name, name, namelen) == 0)
			return od;
	}
	return 0;
}


// create_blank_direntry(dir_oi)
//	'dir_oi' is an OSP inode for a directory.
//	Return a blank directory entry in that directory.  This might require
//	adding a new block to the directory.  Returns an error pointer (see
//	below) on failure.
//
// ERROR POINTERS: The Linux kernel uses a special convention for returning
// error values in the form of pointers.  Here's how it works.
//	- ERR_PTR(errno): Creates a pointer value corresponding to an error.
//	- IS_ERR(ptr): Returns true iff 'ptr' is an error value.
//	- PTR_ERR(ptr): Returns the error value for an error pointer.
//	For example:
//
//	static ospfs_direntry_t *create_blank_direntry(...) {
//		return ERR_PTR(-ENOSPC);
//	}
//	static int ospfs_create(...) {
//		...
//		ospfs_direntry_t *od = create_blank_direntry(...);
//		if (IS_ERR(od))
//			return PTR_ERR(od);
//		...
//	}
//
//	The create_blank_direntry function should use this convention.
//
// EXERCISE: Write this function.

static ospfs_direntry_t *
create_blank_direntry(ospfs_inode_t *dir_oi, ino_t inode_num)
{
	// Outline:
	// 1. Check the existing directory data for an empty entry.  Return one
	//    if you find it.
	// 2. If there's no empty entries, add a block to the directory.
	//    Use ERR_PTR if this fails; otherwise, clear out all the directory
	//    entries and return one of them.
	int blockno, dirno;
	ospfs_direntry_t *direntry_list = 0, *direntry = 0;
	const uint32_t direntries_per_block = (OSPFS_BLKSIZE / OSPFS_DIRENTRY_SIZE);

	// Go through the whole directory to see if there are any free blocks
	uint32_t blocks_size = ospfs_size2nblocks(dir_oi->oi_size);
	for(blockno = 0; blockno < blocks_size; blockno++) {
		direntry_list = ospfs_inode_data(dir_oi, blockno * OSPFS_BLKSIZE);
		
		// Loop through all the entries, see if any have inode number 0
		for(dirno = 0; dirno < direntries_per_block; dirno++) {
			if(direntry_list[dirno].od_ino == 0) {
				direntry = &direntry_list[dirno];
				break;
			}
		}
		// See if we found a direntry
		if(direntry != 0)
			break;
	}
	// See if we found any blank direntries
	if(direntry == 0) {
		// Check to see if we can add a new block to the directory
		int error = change_size(dir_oi, inode_num ,dir_oi->oi_size + OSPFS_BLKSIZE);
		if(error < 0)
			ERR_PTR(-ENOSPC);

		// Clear the memory and set the direntry pointer to the first direntry
		//  in the new block, found by the new dir_oi size
		direntry_list =
			ospfs_inode_data(
				dir_oi,
				(ospfs_size2nblocks(dir_oi->oi_size)) * OSPFS_BLKSIZE
			);
		memset(direntry_list, 0, OSPFS_BLKSIZE);

		direntry = &direntry_list[0];
	}

	// In the clear, now return the new direntry
	return direntry;
}

static int
find_blank_direntry(ospfs_inode_t *dir_oi, uint32_t *offset, ino_t inode_num) {
	
	int blockno, direntry_no, blank_found;
	ospfs_direntry_t *direntry_list = 0;
	const uint32_t direntries_per_block = (OSPFS_BLKSIZE / OSPFS_DIRENTRY_SIZE);
	uint32_t blocks_size;
	blank_found = 0;

	// Go through the whole directory to see if there are any free blocks
	blocks_size = ospfs_size2nblocks(dir_oi->oi_size);
	for(blockno = 0; blockno < blocks_size; blockno++) {
		direntry_list = ospfs_inode_data(dir_oi, blockno * OSPFS_BLKSIZE);
		
		// Loop through all the entries, see if any have inode number 0
		for(direntry_no = 0; direntry_no < direntries_per_block; direntry_no++) {
			if(direntry_list[direntry_no].od_ino == 0) {
				*offset = direntry_no;
				blank_found = 1;
				return blockno;
			}
		}
	}

	if(blank_found == 0) {
		// Check to see if we can add a new block to the directory
		int error = change_size(dir_oi, inode_num ,dir_oi->oi_size + OSPFS_BLKSIZE);
		if(error < 0)
			return -ENOSPC;

		// Clear the memory and set the direntry pointer to the first direntry
		//  in the new block, found by the new dir_oi size
		direntry_list =
			ospfs_inode_data(
				dir_oi,
				(ospfs_size2nblocks(dir_oi->oi_size)) * OSPFS_BLKSIZE
			);
		memset(direntry_list, 0, OSPFS_BLKSIZE);

		blockno = ospfs_size2nblocks(dir_oi->oi_size) - 1;
		*offset = 0;
	}
	// In the clear, now return the new direntry
	return blockno;
}

// ospfs_link(src_dentry, dir, dst_dentry
//   Linux calls this function to create hard links.
//   It is the ospfs_dir_inode_ops.link callback.
//
//   Inputs: src_dentry   -- a pointer to the dentry for the source file.  This
//                           file's inode contains the real data for the hard
//                           linked filae.  The important elements are:
//                             src_dentry->d_name.name
//                             src_dentry->d_name.len
//                             src_dentry->d_inode->i_ino
//           dir          -- a pointer to the containing directory for the new
//                           hard link.
//           dst_dentry   -- a pointer to the dentry for the new hard link file.
//                           The important elements are:
//                             dst_dentry->d_name.name
//                             dst_dentry->d_name.len
//                             dst_dentry->d_inode->i_ino
//                           Two of these values are already set.  One must be
//                           set by you, which one?
//   Returns: 0 on success, -(error code) on error.  In particular:
//               -ENAMETOOLONG if dst_dentry->d_name.len is too large, or
//			       'symname' is too long;
//               -EEXIST       if a file named the same as 'dst_dentry' already
//                             exists in the given 'dir';
//               -ENOSPC       if the disk is full & the file can't be created;
//               -EIO          on I/O error.
//
//   EXERCISE: Complete this function.

static int
ospfs_link(struct dentry *src_dentry, struct inode *dir, struct dentry *dst_dentry) {
	/* EXERCISE: Your code here. */
	
	journal_header_t link_header, *journal_header;
	ospfs_direntry_t *direntry, *b_direntry, *direntry_list;
	uint32_t journal_block_pos, offset, direntry_blockno,
		 direntry_no, *journal_block;
	ospfs_inode_t *f_inode, *dir_oi;
	ospfs_direntry_t direntries[OSPFS_BLKSIZE/OSPFS_DIRENTRY_SIZE];
	
	dir_oi = ospfs_inode(dir->i_ino);
	offset = 0;

	
	if(src_dentry->d_inode->i_ino == 0) {
		return -EIO;
	}

	if(OSPFS_MAXNAMELEN < dst_dentry->d_name.len) {
		return -ENAMETOOLONG;
	}

	if(find_direntry(dir_oi, dst_dentry->d_name.name, 
			dst_dentry->d_name.len)) {
		return -EEXIST;
	}
	
	
	memset(&link_header, 0, sizeof(journal_header_t));

	// Change Header Information
	link_header.execute_type = JOURNAL_HRDLNK;
	link_header.n_blocks_affected = 1;
	
	// Record the inode numbers into the journal
	link_header.inode_num = src_dentry->d_inode->i_ino;

	// Find inode pointer to be converted to actual inode and placed into the journal
	f_inode = ospfs_inode(link_header.inode_num);

	// Convert Inode Pointers found earlier and put into journal
	link_header.inode = *f_inode;

	// Finds the First Blank Direntry within the directory and records the actual block number into journal	
	direntry_blockno = find_blank_direntry(dir_oi, &offset, dir->i_ino);
	link_header.dir_data_blockno = ospfs_inode_blockno(dir_oi, direntry_blockno * OSPFS_BLKSIZE);

	// Create Local Copy of Block with the Blank Direntry that was found earlier
	direntry_list = ospfs_inode_data(dir_oi,direntry_blockno * OSPFS_BLKSIZE);
	for( direntry_no = 0; direntry_no < OSPFS_BLKSIZE/OSPFS_DIRENTRY_SIZE; direntry_no++) {
		 b_direntry = &direntry_list[direntry_no];
		 direntries[direntry_no] = *b_direntry;
	}

	// Change the Blank Direntry to What it should look like
	direntry = &direntries[offset];
	strncpy(direntry->od_name, dst_dentry->d_name.name, dst_dentry->d_name.len);
	direntry->od_ino = link_header.inode_num;
	link_header.inode.oi_nlink++;
	//link_inode->oi_nlink++;

	// Record direntry block into the journal's data block
	journal_block_pos = ospfs_super->os_firstjournalb + JOURNAL_DATA_BLOCKS_POS;
	journal_block = ospfs_block(journal_block_pos);
	memcpy(journal_block, direntries, OSPFS_BLKSIZE);
	
	// Copy Header Information into Journal Header
	memcpy(ospfs_block(ospfs_super->os_firstjournalb + JOURNAL_HEADER_POS), &link_header, sizeof(journal_header_t));

	// Update Journal Header Completion Flag
	journal_header = (journal_header_t*)
			ospfs_block(ospfs_super->os_firstjournalb + JOURNAL_HEADER_POS);
	journal_header->completed = 1;
	
	execute_journal();

	return 0;
}

// ospfs_create
//   Linux calls this function to create a regular file.
//   It is the ospfs_dir_inode_ops.create callback.
//
//   Inputs:  dir	-- a pointer to the containing directory's inode
//            dentry    -- the name of the file that should be created
//                         The only important elements are:
//                         dentry->d_name.name: filename (char array, not null
//                            terminated)
//                         dentry->d_name.len: length of filename
//            mode	-- the permissions mode for the file (set the new
//			   inode's oi_mode field to this value)
//	      nd	-- ignore this
//   Returns: 0 on success, -(error code) on error.  In particular:
//               -ENAMETOOLONG if dentry->d_name.len is too large;
//               -EEXIST       if a file named the same as 'dentry' already
//                             exists in the given 'dir';
//               -ENOSPC       if the disk is full & the file can't be created;
//               -EIO          on I/O error.
//
//   We have provided strictly less skeleton code for this function than for
//   the others.  Here's a brief outline of what you need to do:
//   1. Check for the -EEXIST error and find an empty directory entry using the
//	helper functions above.
//   2. Find an empty inode.  Set the 'entry_ino' variable to its inode number.
//   3. Initialize the directory entry and inode.
//
//   EXERCISE: Complete this function.

static int
ospfs_create(struct inode *dir, struct dentry *dentry, int mode, struct nameidata *nd)
{
	int i, offset, error;
	uint32_t direntry_block_pos, direntry_no, entry_ino;
	journal_header_t header;
	ospfs_direntry_t *direntry_list;
	ospfs_inode_t *inodes;

	// Local copy of direntry block
	ospfs_direntry_t direntries[OSPFS_BLKSIZE/OSPFS_DIRENTRY_SIZE];

	// Get the directory data
	ospfs_inode_t *dir_oi = ospfs_inode(dir->i_ino);

	// Initialize header
	memset(&header, 0, sizeof(journal_header_t));
	header.execute_type = JOURNAL_CREATE;

	// Check if we can add the new directory in the directory
	direntry_block_pos = find_blank_direntry(dir_oi, &offset, dir->i_ino);
	if(direntry_block_pos < 0)
		return direntry_block_pos;
	
	// Get the data block number
	header.dir_data_blockno = ospfs_inode_blockno(dir_oi, 
		direntry_block_pos * OSPFS_BLKSIZE);

	// The next free inode number
	entry_ino = 0;
	inodes = ospfs_block(ospfs_super->os_firstinob);
	// Find an open inode
	for(i = 0; i < ospfs_super->os_ninodes; i++) {
		if(inodes[i].oi_nlink == 0) {
			entry_ino = i;
			break;
		}
	}
	if(i == ospfs_super->os_ninodes)
		return -ENOSPC;

	// Set the value of the inode number
	header.inode_num = entry_ino;

	// Set the values of the inode
	header.inode.oi_nlink = 1;
	header.inode.oi_size = 0;
	header.inode.oi_ftype = OSPFS_FTYPE_REG;
	header.inode.oi_mode = mode;


	// Create Local Copy of Block with the Blank Direntry that was found 
	// earlier
	direntry_list = ospfs_inode_data(dir_oi, 
						direntry_block_pos * OSPFS_BLKSIZE);
	direntry_no = 0;
	while(direntry_no < OSPFS_BLKSIZE/OSPFS_DIRENTRY_SIZE) {
		direntries[direntry_no] = direntry_list[direntry_no];
		direntry_no++; 
	}

	// Change the local copy of the direntry
	direntries[offset].od_ino = entry_ino;
	// Create the name and null byte padding
	for(i = 0; i < OSPFS_MAXNAMELEN + 1; i++) {
		if(i < dentry->d_name.len)
			direntries[offset].od_name[i] = dentry->d_name.name[i];
		else
			direntries[offset].od_name[i] = '\0';
	}

	error = create_to_journal(&header, direntries);
	if(error < 0)
		return error;

	execute_journal();


	/* Execute this code after your function has successfully created the
	   file.  Set entry_ino to the created file's inode number before
	   getting here. */
	{
		struct inode *in = ospfs_mk_linux_inode(dir->i_sb, entry_ino);
		if (!in)
			return -ENOMEM;
		d_instantiate(dentry, in);
		return 0;
	}
}


// ospfs_symlink(dirino, dentry, symname)
//   Linux calls this function to create a symbolic link.
//   It is the ospfs_dir_inode_ops.symlink callback.
//
//   Inputs: dir     -- a pointer to the containing directory's inode
//           dentry  -- the name of the file that should be created
//                      The only important elements are:
//                      dentry->d_name.name: filename (char array, not null
//                           terminated)
//                      dentry->d_name.len: length of filename
//           symname -- the symbolic link's destination
//
//   Returns: 0 on success, -(error code) on error.  In particular:
//               -ENAMETOOLONG if dentry->d_name.len is too large, or
//			       'symname' is too long;
//               -EEXIST       if a file named the same as 'dentry' already
//                             exists in the given 'dir';
//               -ENOSPC       if the disk is full & the file can't be created;
//               -EIO          on I/O error.
//
//   EXERCISE: Complete this function.

static int
ospfs_symlink(struct inode *dir, struct dentry *dentry, const char *symname)
{
	int len, i;
	ospfs_inode_t *dir_oi = ospfs_inode(dir->i_ino);
	ospfs_symlink_inode_t *symlink = 0;
	ospfs_inode_t *inodes = ospfs_block(ospfs_super->os_firstinob);
	ospfs_direntry_t *direntry = 0;
	uint32_t entry_ino = 0;

	if(OSPFS_MAXNAMELEN < dentry->d_name.len) {
		return -ENAMETOOLONG;
	}

	if(find_direntry(dir_oi, dentry->d_name.name, 
			dentry->d_name.len)) {
		return -EEXIST;
	}

	// Now check the symbolic link name
	len = strlen(symname);
	if(OSPFS_MAXSYMLINKLEN < len) {
		return -ENAMETOOLONG;
	}

	// Get a new direntry
	direntry = create_blank_direntry(dir_oi, dir->i_ino);
	if(IS_ERR(direntry))
		return PTR_ERR(direntry);

	// Find an open inode
	for(i = 0; i < ospfs_super->os_ninodes; i++) {
		if(inodes[i].oi_nlink == 0) {
			entry_ino = i;
			break;
		}
	}
	if(i == ospfs_super->os_ninodes)
		return -ENOSPC;

	// Set the symlink to the appropriate inode
	symlink = (ospfs_symlink_inode_t*)&inodes[entry_ino];


	// Set the values of the members
	symlink->oi_nlink = 1;
	symlink->oi_ftype = OSPFS_FTYPE_SYMLINK;
	symlink->oi_size = len;
	strcpy(symlink->oi_symlink, symname);

	// Make root?ln1:ln2<null> into root?ln1<null>ln2<null>
	// This is to make conditional symlinks easier later
	if(strncmp (symlink->oi_symlink, "root?", 5) == 0) {
		i = 0;
		// Find the dividing point
		while(i < len && symlink->oi_symlink[i] != ':') {
			i++;
		}
		// This should never happen
		if(i == len)
			return -ENAMETOOLONG;

		// Divide the two with a null byte
		symlink->oi_symlink[i] = '\0';
	}

	// Finish making the direntry
	direntry->od_ino = entry_ino;
	strncpy(direntry->od_name, dentry->d_name.name, dentry->d_name.len);
	direntry->od_name[dentry->d_name.len] = '\0';

	/* Execute this code after your function has successfully created the
	   file.  Set entry_ino to the created file's inode number before
	   getting here. */
	{
		struct inode *i = ospfs_mk_linux_inode(dir->i_sb, entry_ino);
		if (!i)
			return -ENOMEM;
		d_instantiate(dentry, i);
		return 0;
	}
}


// ospfs_follow_link(dentry, nd)
//   Linux calls this function to follow a symbolic link.
//   It is the ospfs_symlink_inode_ops.follow_link callback.
//
//   Inputs: dentry -- the symbolic link's directory entry
//           nd     -- to be filled in with the symbolic link's destination
//
//   Exercise: Expand this function to handle conditional symlinks.  Conditional
//   symlinks will always be created by users in the following form
//     root?/path/1:/path/2.
//   (hint: Should the given form be changed in any way to make this method
//   easier?  With which character do most functions expect C strings to end?)

static void *
ospfs_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	ospfs_symlink_inode_t *oi =
		(ospfs_symlink_inode_t *) ospfs_inode(dentry->d_inode->i_ino);
	// Exercise: Your code here.

	char* symlink = oi->oi_symlink;
	// Check for conditional
	if(strncmp (oi->oi_symlink, "root?", 5) == 0) {
		// If we are root go to the character right after '?'
		if(current->euid == 0) { 
			symlink = &symlink[5];
		}
		// Otherwise find the start of the next work
		else {
			symlink = &symlink[5];
			while(*symlink) {
				symlink++;
			}
			symlink++;
		}
	}

	nd_set_link(nd, symlink);
	return (void *) 0;
}


// Define the file system operations structures mentioned above.

static struct file_system_type ospfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "ospfs",
	.get_sb		= ospfs_get_sb,
	.kill_sb	= kill_anon_super
};

static struct inode_operations ospfs_reg_inode_ops = {
	.setattr	= ospfs_notify_change
};

static struct file_operations ospfs_reg_file_ops = {
	.llseek		= generic_file_llseek,
	.read		= ospfs_read,
	.write		= ospfs_write
};

static struct inode_operations ospfs_dir_inode_ops = {
	.lookup		= ospfs_dir_lookup,
	.link		= ospfs_link,
	.unlink		= ospfs_unlink,
	.create		= ospfs_create,
	.symlink	= ospfs_symlink
};

static struct file_operations ospfs_dir_file_ops = {
	.read		= generic_read_dir,
	.readdir	= ospfs_dir_readdir
};

static struct inode_operations ospfs_symlink_inode_ops = {
	.readlink	= generic_readlink,
	.follow_link	= ospfs_follow_link
};

static struct dentry_operations ospfs_dentry_ops = {
	.d_delete	= ospfs_delete_dentry
};

static struct super_operations ospfs_superblock_ops = {
};


// Functions used to hook the module into the kernel!

static int __init init_ospfs_fs(void)
{
	eprintk("Loading ospfs module...\n");
	return register_filesystem(&ospfs_fs_type);
}

static void __exit exit_ospfs_fs(void)
{
	unregister_filesystem(&ospfs_fs_type);
	eprintk("Unloading ospfs module\n");
}

module_init(init_ospfs_fs)
module_exit(exit_ospfs_fs)

// Information about the module
MODULE_AUTHOR("Skeletor");
MODULE_DESCRIPTION("OSPFS");
MODULE_LICENSE("GPL");
