/*****************************************************************************

Copyright (c) 2011-2012 Percona Inc. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
Street, Fifth Floor, Boston, MA 02110-1301, USA

*****************************************************************************/

/**************************************************//**
@file log/log0online.cc
Online database log parsing for changed page tracking

*******************************************************/

#include "log0online.h"

#include "my_dbug.h"

#include "log0recv.h"
#include "mach0data.h"
#include "mtr0log.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "trx0sys.h"
#include "ut0rbt.h"

#ifdef __WIN__
/* error LNK2001: unresolved external symbol _debug_sync_C_callback_ptr */
# define DEBUG_SYNC_C(dummy) ((void) 0)
#else
# include "m_string.h" /* for my_sys.h */
# include "my_sys.h" /* DEBUG_SYNC_C */
#endif

enum { FOLLOW_SCAN_SIZE = 4 * (UNIV_PAGE_SIZE_MAX) };

#ifdef UNIV_PFS_MUTEX
/* Key to register log_bmp_sys->mutex with PFS */
mysql_pfs_key_t	log_bmp_sys_mutex_key;
#endif /* UNIV_PFS_MUTEX */

/** Log parsing and bitmap output data structure */
struct log_bitmap_struct {
	byte*		read_buf_ptr;	/*!< Unaligned log read buffer */
	byte*		read_buf;	/*!< log read buffer */
	byte		parse_buf[RECV_PARSING_BUF_SIZE];
					/*!< log parse buffer */
	byte*		parse_buf_end;  /*!< parse buffer position where the
					next read log data should be copied to.
					If the previous log records were fully
					parsed, it points to the start,
					otherwise points immediatelly past the
					end of the incomplete log record. */
	char		bmp_file_home[FN_REFLEN];
					/*!< directory for bitmap files */
	log_online_bitmap_file_t out;	/*!< The current bitmap file */
	ulint		out_seq_num;	/*!< the bitmap file sequence number */
	lsn_t		start_lsn;	/*!< the LSN of the next unparsed
					record and the start of the next LSN
					interval to be parsed.  */
	lsn_t		end_lsn;	/*!< the end of the LSN interval to be
					parsed, equal to the next checkpoint
					LSN at the time of parse */
	lsn_t		next_parse_lsn;	/*!< the LSN of the next unparsed
					record in the current parse */
	ib_rbt_t*	modified_pages; /*!< the current modified page set,
					organized as the RB-tree with the keys
					of (space, 4KB-block-start-page-id)
					pairs */
	ib_rbt_node_t*	page_free_list; /*!< Singly-linked list of freed nodes
					of modified_pages tree for later
					reuse.  Nodes are linked through
					ib_rbt_node_t.left as this field has
					both the correct type and the tree does
					not mind its overwrite during
					rbt_next() tree traversal. */
};

/* The log parsing and bitmap output struct instance */
static struct log_bitmap_struct* log_bmp_sys;

/* Mutex protecting log_bmp_sys */
static ib_mutex_t	log_bmp_sys_mutex;

/** File name stem for bitmap files. */
static const char* bmp_file_name_stem = "ib_modified_log_";

/** File name template for bitmap files.  The 1st format tag is a directory
name, the 2nd tag is the stem, the 3rd tag is a file sequence number, the 4th
tag is the start LSN for the file. */
static const char* bmp_file_name_template = "%s%s%lu_" LSN_PF ".xdb";

/* On server startup with empty database srv_start_lsn == 0, in
which case the first LSN of actual log records will be this. */
#define MIN_TRACKED_LSN (LOG_START_LSN + OS_FILE_LOG_BLOCK_SIZE + \
			 LOG_BLOCK_HDR_SIZE)

/* Tests if num bit of bitmap is set */
#define IS_BIT_SET(bitmap, num) \
	(*((bitmap) + ((num) >> 3)) & (1UL << ((num) & 7UL)))

/** The bitmap file block size in bytes.  All writes will be multiples of this.
 */
enum {
	MODIFIED_PAGE_BLOCK_SIZE = 4096
};


/** Offsets in a file bitmap block */
enum {
	MODIFIED_PAGE_IS_LAST_BLOCK = 0,/* 1 if last block in the current
					write, 0 otherwise. */
	MODIFIED_PAGE_START_LSN = 4,	/* The starting tracked LSN of this and
					other blocks in the same write */
	MODIFIED_PAGE_END_LSN = 12,	/* The ending tracked LSN of this and
					other blocks in the same write */
	MODIFIED_PAGE_SPACE_ID = 20,	/* The space ID of tracked pages in
					this block */
	MODIFIED_PAGE_1ST_PAGE_ID = 24,	/* The page ID of the first tracked
					page in this block */
	MODIFIED_PAGE_BLOCK_UNUSED_1 = 28,/* Unused in order to align the start
					of bitmap at 8 byte boundary */
	MODIFIED_PAGE_BLOCK_BITMAP = 32,/* Start of the bitmap itself */
	MODIFIED_PAGE_BLOCK_UNUSED_2 = MODIFIED_PAGE_BLOCK_SIZE - 8,
					/* Unused in order to align the end of
					bitmap at 8 byte boundary */
	MODIFIED_PAGE_BLOCK_CHECKSUM = MODIFIED_PAGE_BLOCK_SIZE - 4
					/* The checksum of the current block */
};

/** Length of the bitmap data in a block in bytes */
enum { MODIFIED_PAGE_BLOCK_BITMAP_LEN
       = MODIFIED_PAGE_BLOCK_UNUSED_2 - MODIFIED_PAGE_BLOCK_BITMAP };

/** Length of the bitmap data in a block in page ids */
enum { MODIFIED_PAGE_BLOCK_ID_COUNT = MODIFIED_PAGE_BLOCK_BITMAP_LEN * 8 };

/****************************************************************//**
Provide a comparisson function for the RB-tree tree (space,
block_start_page) pairs.  Actual implementation does not matter as
long as the ordering is full.
@return -1 if p1 < p2, 0 if p1 == p2, 1 if p1 > p2
*/
static
int
log_online_compare_bmp_keys(
/*========================*/
	const void* p1,	/*!<in: 1st key to compare */
	const void* p2)	/*!<in: 2nd key to compare */
{
	const byte *k1 = (const byte *)p1;
	const byte *k2 = (const byte *)p2;

	ulint k1_space = mach_read_from_4(k1 + MODIFIED_PAGE_SPACE_ID);
	ulint k2_space = mach_read_from_4(k2 + MODIFIED_PAGE_SPACE_ID);
	if (k1_space == k2_space) {
		ulint k1_start_page
			= mach_read_from_4(k1 + MODIFIED_PAGE_1ST_PAGE_ID);
		ulint k2_start_page
			= mach_read_from_4(k2 + MODIFIED_PAGE_1ST_PAGE_ID);
		return k1_start_page < k2_start_page
			? -1 : k1_start_page > k2_start_page ? 1 : 0;
	}
	return k1_space < k2_space ? -1 : 1;
}

/****************************************************************//**
Set a bit for tracked page in the bitmap. Expand the bitmap tree as
necessary. */
static
void
log_online_set_page_bit(
/*====================*/
	ulint	space,	/*!<in: log record space id */
	ulint	page_no)/*!<in: log record page id */
{
	ut_ad(mutex_own(&log_bmp_sys_mutex));

	ut_a(space != ULINT_UNDEFINED);
	ut_a(page_no != ULINT_UNDEFINED);

	ulint block_start_page = page_no / MODIFIED_PAGE_BLOCK_ID_COUNT
		* MODIFIED_PAGE_BLOCK_ID_COUNT;
	ulint block_pos = block_start_page ? (page_no % block_start_page / 8)
		: (page_no / 8);
	uint bit_pos = page_no % 8;

	byte search_page[MODIFIED_PAGE_BLOCK_SIZE];
	mach_write_to_4(search_page + MODIFIED_PAGE_SPACE_ID, space);
	mach_write_to_4(search_page + MODIFIED_PAGE_1ST_PAGE_ID,
			block_start_page);

	byte	       *page_ptr;
	ib_rbt_bound_t  tree_search_pos;
	if (!rbt_search(log_bmp_sys->modified_pages, &tree_search_pos,
			search_page)) {
		page_ptr = rbt_value(byte, tree_search_pos.last);
	}
	else {
		ib_rbt_node_t *new_node;

		if (log_bmp_sys->page_free_list) {
			new_node = log_bmp_sys->page_free_list;
			log_bmp_sys->page_free_list = new_node->left;
		}
		else {
			new_node = static_cast<ib_rbt_node_t *>
				(ut_malloc
				 (SIZEOF_NODE(log_bmp_sys->modified_pages),
				  mem_key_log_online_modified_pages));
		}
		memset(new_node, 0, SIZEOF_NODE(log_bmp_sys->modified_pages));

		page_ptr = rbt_value(byte, new_node);
		mach_write_to_4(page_ptr + MODIFIED_PAGE_SPACE_ID, space);
		mach_write_to_4(page_ptr + MODIFIED_PAGE_1ST_PAGE_ID,
				block_start_page);

		rbt_add_preallocated_node(log_bmp_sys->modified_pages,
					  &tree_search_pos, new_node);
	}
	page_ptr[MODIFIED_PAGE_BLOCK_BITMAP + block_pos] |= (1U << bit_pos);
}

/****************************************************************//**
Calculate a bitmap block checksum.  Algorithm borrowed from
log_block_calc_checksum.
@return checksum */
UNIV_INLINE
ulint
log_online_calc_checksum(
/*=====================*/
	const byte*	block)	/*!<in: bitmap block */
{
	ulint	sum;
	ulint	sh;
	ulint	i;

	sum = 1;
	sh = 0;

	for (i = 0; i < MODIFIED_PAGE_BLOCK_CHECKSUM; i++) {

		ulint	b = block[i];
		sum &= 0x7FFFFFFFUL;
		sum += b;
		sum += b << sh;
		sh++;
		if (sh > 24) {
			sh = 0;
		}
	}

	return sum;
}

/****************************************************************//**
Read one bitmap data page and check it for corruption.

@return true if page read OK, false if I/O error */
static
bool
log_online_read_bitmap_page(
/*========================*/
	log_online_bitmap_file_t	*bitmap_file,	/*!<in/out: bitmap
							file */
	byte				*page,	       /*!<out: read page.
						       Must be at least
						       MODIFIED_PAGE_BLOCK_SIZE
						       bytes long */
	bool				*checksum_ok)	/*!<out: true if page
							checksum OK */
{
	ulint	checksum;
	ulint	actual_checksum;
	bool	success;

	ut_a(bitmap_file->size >= MODIFIED_PAGE_BLOCK_SIZE);
	ut_a(bitmap_file->offset
	     <= bitmap_file->size - MODIFIED_PAGE_BLOCK_SIZE);
	ut_a(bitmap_file->offset % MODIFIED_PAGE_BLOCK_SIZE == 0);

	IORequest io_request(IORequest::LOG | IORequest::READ | IORequest::NO_ENCRYPTION);
	success = os_file_read(io_request, bitmap_file->file, page,
			       bitmap_file->offset, MODIFIED_PAGE_BLOCK_SIZE);

	if (UNIV_UNLIKELY(!success)) {

		/* The following call prints an error message */
		os_file_get_last_error(true);
		ib::warn() << "Failed reading changed page bitmap file \'"
			   << bitmap_file->name << "\'";
		return false;
	}

	bitmap_file->offset += MODIFIED_PAGE_BLOCK_SIZE;
	ut_ad(bitmap_file->offset <= bitmap_file->size);

	checksum = mach_read_from_4(page + MODIFIED_PAGE_BLOCK_CHECKSUM);
	actual_checksum = log_online_calc_checksum(page);
	*checksum_ok = (checksum == actual_checksum);

	return true;
}

/****************************************************************//**
Get the last tracked fully LSN from the bitmap file by reading
backwards untile a correct end page is found.  Detects incomplete
writes and corrupted data.  Sets the start output position for the
written bitmap data.

Multiple bitmap files are handled using the following assumptions:
1) Only the last file might be corrupted.  In case where no good data was found
in the last file, assume that the next to last file is OK.  This assumption
does not limit crash recovery capability in any way.
2) If the whole of the last file was corrupted, assume that the start LSN in
its name is correct and use it for (re-)tracking start.

@return the last fully tracked LSN */
static
lsn_t
log_online_read_last_tracked_lsn(void)
/*==================================*/
{
	byte		page[MODIFIED_PAGE_BLOCK_SIZE];
	bool		is_last_page	= false;
	bool		checksum_ok	= false;
	lsn_t		result;
	os_offset_t	read_offset	= log_bmp_sys->out.offset;

	while ((!checksum_ok || !is_last_page) && read_offset > 0)
	{
		read_offset -= MODIFIED_PAGE_BLOCK_SIZE;
		log_bmp_sys->out.offset = read_offset;

		if (!log_online_read_bitmap_page(&log_bmp_sys->out, page,
						 &checksum_ok)) {
			checksum_ok = false;
			result = 0;
			break;
		}

		if (checksum_ok) {
			is_last_page
				= mach_read_from_4
				(page + MODIFIED_PAGE_IS_LAST_BLOCK);
		} else {

			ib::warn() << "Corruption detected in \'"
				   << log_bmp_sys->out.name << "\' at offset "
				   << read_offset;
		}
	};

	result = (checksum_ok && is_last_page)
		? mach_read_from_8(page + MODIFIED_PAGE_END_LSN) : 0;

	/* Truncate the output file to discard the corrupted bitmap data, if
	any */
	if (!os_file_set_eof_at(log_bmp_sys->out.file,
				log_bmp_sys->out.offset)) {
		ib::warn() << "Failed truncating changed page bitmap file \'"
			   << log_bmp_sys->out.name << "\' to "
			   << log_bmp_sys->out.offset << " bytes";
		result = 0;
	}
	return result;
}

/****************************************************************//**
Safely write the log_sys->tracked_lsn value.  The reader counterpart function
is log_get_tracked_lsn() in log0log.ic. */
UNIV_INLINE
void
log_set_tracked_lsn(
/*================*/
	lsn_t	tracked_lsn)	/*!<in: new value */
{
	/* Single writer, no data race here */
	lsn_t old_value = os_atomic_increment_uint64(&log_sys->tracked_lsn, 0);
	(void) os_atomic_increment_uint64(&log_sys->tracked_lsn,
					  tracked_lsn - old_value);
}

/*********************************************************************//**
Check if missing, if any, LSN interval can be read and tracked using the
current LSN value, the LSN value where the tracking stopped, and the log group
capacity.

@return true if the missing interval can be tracked or if there's no missing
data.  */
MY_ATTRIBUTE((warn_unused_result))
static
bool
log_online_can_track_missing(
/*=========================*/
	lsn_t	last_tracked_lsn,	/*!<in: last tracked LSN */
	lsn_t	tracking_start_lsn)	/*!<in:	current LSN */
{
	/* last_tracked_lsn might be < MIN_TRACKED_LSN in the case of empty
	bitmap file, handle this too. */
	last_tracked_lsn = ut_max(last_tracked_lsn, MIN_TRACKED_LSN);

	if (last_tracked_lsn > tracking_start_lsn) {
		ib::fatal() << "Last tracked LSN " << last_tracked_lsn
			    << " is ahead of tracking start LSN "
			    << tracking_start_lsn << ".  This can be caused "
			"by mismatched bitmap files.";
	}

	return (last_tracked_lsn == tracking_start_lsn)
		|| (log_sys->lsn - last_tracked_lsn
		    <= log_sys->log_group_capacity);
}


/****************************************************************//**
Diagnose a gap in tracked LSN range on server startup due to crash or
very fast shutdown and try to close it by tracking the data
immediatelly, if possible. */
static
void
log_online_track_missing_on_startup(
/*================================*/
	lsn_t	last_tracked_lsn,	/*!<in: last tracked LSN read from the
					bitmap file */
	lsn_t	tracking_start_lsn)	/*!<in: last checkpoint LSN of the
					current server startup */
{
	ut_ad(last_tracked_lsn != tracking_start_lsn);
	ut_ad(srv_track_changed_pages);

	ib::warn() << "Last tracked LSN in \'" << log_bmp_sys->out.name
		   << "\' is " << last_tracked_lsn
		   << ", but the last checkpoint LSN is "
		   << tracking_start_lsn << ".  This might be due to a server "
		"crash or a very fast shutdown.";

	/* See if we can fully recover the missing interval */
	if (log_online_can_track_missing(last_tracked_lsn,
					 tracking_start_lsn)) {

		ib::info() << "Reading the log to advance the last tracked LSN.";

		log_bmp_sys->start_lsn = ut_max(last_tracked_lsn,
						MIN_TRACKED_LSN);
		log_set_tracked_lsn(log_bmp_sys->start_lsn);
		if (!log_online_follow_redo_log()) {
			exit(1);
		}
		ut_ad(log_bmp_sys->end_lsn >= tracking_start_lsn);

		ib::info() << "Continuing tracking changed pages from LSN "
			   << log_bmp_sys->end_lsn;
	}
	else {
		ib::warn() << "The age of last tracked LSN exceeds log "
			"capacity, tracking-based incremental backups will "
			"work only from the higher LSN!";

		log_bmp_sys->end_lsn = log_bmp_sys->start_lsn
			= tracking_start_lsn;
		log_set_tracked_lsn(log_bmp_sys->start_lsn);

		ib::info() << "Starting tracking changed pages from LSN "
			   << log_bmp_sys->end_lsn;
	}
}

/*********************************************************************//**
Format a bitmap output file name to log_bmp_sys->out.name.  */
static
void
log_online_make_bitmap_name(
/*=========================*/
	lsn_t	start_lsn)	/*!< in: the start LSN name part */
{
	ut_snprintf(log_bmp_sys->out.name, sizeof(log_bmp_sys->out.name), bmp_file_name_template,
		    log_bmp_sys->bmp_file_home, bmp_file_name_stem,
		    log_bmp_sys->out_seq_num, start_lsn);
}

/*********************************************************************//**
Check if an old file that has the name of a new bitmap file we are about to
create should be overwritten.  */
static
bool
log_online_should_overwrite(
/*========================*/
	const char	*path)	/*!< in: path to file */
{
	dberr_t		err;
	os_file_stat_t	file_info;

	/* Currently, it's OK to overwrite 0-sized files only */
	err = os_file_get_status(path, &file_info, false, srv_read_only_mode);
	return err == DB_SUCCESS && file_info.type == OS_FILE_TYPE_FILE
		&& file_info.size == 0LL;
}

/*********************************************************************//**
Create a new empty bitmap output file.

@return true if operation succeeded, false if I/O error */
static
bool
log_online_start_bitmap_file(void)
/*==============================*/
{
	bool	success	= true;

	/* Check for an old file that should be deleted first */
	if (log_online_should_overwrite(log_bmp_sys->out.name)) {

		success = os_file_delete_if_exists(innodb_bmp_file_key,
						   log_bmp_sys->out.name,
						   NULL);
	}

	if (UNIV_LIKELY(success)) {
		log_bmp_sys->out.file
			= os_file_create_simple_no_error_handling(
							innodb_bmp_file_key,
							log_bmp_sys->out.name,
							OS_FILE_CREATE,
							OS_FILE_READ_WRITE,
							srv_read_only_mode,
							&success);
	}
	if (UNIV_UNLIKELY(!success)) {

		/* The following call prints an error message */
		os_file_get_last_error(true);
		ib::error() << "Cannot create \'" << log_bmp_sys->out.name
			    << "\'";
		return false;
	}

	log_bmp_sys->out.offset = 0;
	return true;
}

/*********************************************************************//**
Close the current bitmap output file and create the next one.

@return true if operation succeeded, false if I/O error */
static
bool
log_online_rotate_bitmap_file(
/*===========================*/
	lsn_t	next_file_start_lsn)	/*!<in: the start LSN name
					part */
{
	if (!log_bmp_sys->out.file.is_closed()) {
		os_file_close(log_bmp_sys->out.file);
		log_bmp_sys->out.file.set_closed();
	}
	log_bmp_sys->out_seq_num++;
	log_online_make_bitmap_name(next_file_start_lsn);
	return log_online_start_bitmap_file();
}

/*********************************************************************//**
Check the name of a given file if it's a changed page bitmap file and
return file sequence and start LSN name components if it is.  If is not,
the values of output parameters are undefined.

@return true if a given file is a changed page bitmap file.  */
static
bool
log_online_is_bitmap_file(
/*======================*/
	const os_file_stat_t*	file_info,		/*!<in: file to
							check */
	ulong*			bitmap_file_seq_num,	/*!<out: bitmap file
							sequence number */
	lsn_t*			bitmap_file_start_lsn)	/*!<out: bitmap file
							start LSN */
{
	char	stem[FN_REFLEN];

	ut_ad (strlen(file_info->name) < OS_FILE_MAX_PATH);

	return ((file_info->type == OS_FILE_TYPE_FILE
		 || file_info->type == OS_FILE_TYPE_LINK)
		&& (sscanf(file_info->name, "%[a-z_]%lu_" LSN_PF ".xdb", stem,
			   bitmap_file_seq_num, bitmap_file_start_lsn) == 3)
		&& (!strcmp(stem, bmp_file_name_stem)));
}

/** Initialize the constant part of the log tracking subsystem */

void
log_online_init(void)
{
	mutex_create(LATCH_ID_LOG_ONLINE, &log_bmp_sys_mutex);
}

/** Initialize the dynamic part of the log tracking subsystem */

void
log_online_read_init(void)
{
	bool	success;
	lsn_t	tracking_start_lsn
		= ut_max(log_sys->last_checkpoint_lsn, MIN_TRACKED_LSN);
	os_file_dir_t	bitmap_dir;
	os_file_stat_t	bitmap_dir_file_info;
	lsn_t	last_file_start_lsn	= MIN_TRACKED_LSN;
	size_t	srv_data_home_len;

	/* Bitmap data start and end in a bitmap block must be 8-byte
	aligned. */
	compile_time_assert(MODIFIED_PAGE_BLOCK_BITMAP % 8 == 0);
	compile_time_assert(MODIFIED_PAGE_BLOCK_BITMAP_LEN % 8 == 0);

	ut_ad(srv_track_changed_pages);

	log_bmp_sys = static_cast<log_bitmap_struct *>
		(ut_malloc(sizeof(*log_bmp_sys), mem_key_log_online_sys));
	log_bmp_sys->read_buf_ptr = static_cast<byte *>
		(ut_malloc(FOLLOW_SCAN_SIZE + MAX_SRV_LOG_WRITE_AHEAD_SIZE,
			   mem_key_log_online_read_buf));
	log_bmp_sys->read_buf = static_cast<byte *>
		(ut_align(log_bmp_sys->read_buf_ptr,
			  MAX_SRV_LOG_WRITE_AHEAD_SIZE));

	/* Initialize bitmap file directory from srv_data_home and add a path
	separator if needed.  */
	srv_data_home_len = strlen(srv_data_home);
	ut_a (srv_data_home_len < FN_REFLEN);
	strcpy(log_bmp_sys->bmp_file_home, srv_data_home);
	if (srv_data_home_len
	    && log_bmp_sys->bmp_file_home[srv_data_home_len - 1]
	    != SRV_PATH_SEPARATOR) {

		ut_a (srv_data_home_len < FN_REFLEN - 1);
		log_bmp_sys->bmp_file_home[srv_data_home_len]
			= SRV_PATH_SEPARATOR;
		log_bmp_sys->bmp_file_home[srv_data_home_len + 1] = '\0';
	}

	/* Enumerate existing bitmap files to either open the last one to get
	the last tracked LSN either to find that there are none and start
	tracking from scratch.  */
	log_bmp_sys->out.name[0] = '\0';
	log_bmp_sys->out_seq_num = 0;

	bitmap_dir = os_file_opendir(log_bmp_sys->bmp_file_home, true);
	ut_a(bitmap_dir);
	while (!os_file_readdir_next_file(log_bmp_sys->bmp_file_home,
					  bitmap_dir, &bitmap_dir_file_info)) {

		ulong	file_seq_num;
		lsn_t	file_start_lsn;

		if (!log_online_is_bitmap_file(&bitmap_dir_file_info,
					      &file_seq_num,
					      &file_start_lsn)) {
			continue;
		}

		if (file_seq_num > log_bmp_sys->out_seq_num
		    && bitmap_dir_file_info.size > 0) {
			log_bmp_sys->out_seq_num = file_seq_num;
			last_file_start_lsn = file_start_lsn;
			/* No dir component (log_bmp_sys->bmp_file_home) here,
			because	that's the cwd */
			strncpy(log_bmp_sys->out.name,
				bitmap_dir_file_info.name, FN_REFLEN - 1);
			log_bmp_sys->out.name[FN_REFLEN - 1] = '\0';
		}
	}

	if (os_file_closedir(bitmap_dir)) {
		os_file_get_last_error(true);
		ib::fatal() << "Cannot close \'" << log_bmp_sys->bmp_file_home
			    << "\'";
	}

	if (!log_bmp_sys->out_seq_num) {
		log_bmp_sys->out_seq_num = 1;
		log_online_make_bitmap_name(0);
	}

	log_bmp_sys->modified_pages = rbt_create(MODIFIED_PAGE_BLOCK_SIZE,
						 log_online_compare_bmp_keys);
	log_bmp_sys->page_free_list = NULL;

	log_bmp_sys->out.file
		= os_file_create_simple_no_error_handling
		(innodb_bmp_file_key, log_bmp_sys->out.name, OS_FILE_OPEN,
		 OS_FILE_READ_WRITE, srv_read_only_mode, &success);

	if (!success) {

		/* New file, tracking from scratch */
		if (!log_online_start_bitmap_file()) {
			exit(1);
		}
	}
	else {

		/* Read the last tracked LSN from the last file */
		lsn_t	last_tracked_lsn;
		lsn_t	file_start_lsn;

		log_bmp_sys->out.size
			= os_file_get_size(log_bmp_sys->out.file);
		log_bmp_sys->out.offset	= log_bmp_sys->out.size;

		if (log_bmp_sys->out.offset % MODIFIED_PAGE_BLOCK_SIZE != 0) {

			ib::warn() << "Truncated block detected in \'"
				   << log_bmp_sys->out.name << "\' at offset "
				   << log_bmp_sys->out.offset;
			log_bmp_sys->out.offset -=
				log_bmp_sys->out.offset
				% MODIFIED_PAGE_BLOCK_SIZE;
		}

		last_tracked_lsn = log_online_read_last_tracked_lsn();
		/* Do not rotate if we truncated the file to zero length - we
		can just start writing there */
		const bool need_rotate = (last_tracked_lsn != 0);
		if (!last_tracked_lsn) {

			last_tracked_lsn = last_file_start_lsn;
		}

		/* Start a new file.  Choose the LSN value in its name based on
		if we can retrack any missing data. */
		if (log_online_can_track_missing(last_tracked_lsn,
						 tracking_start_lsn)) {
			file_start_lsn = last_tracked_lsn;
		} else {
			file_start_lsn = tracking_start_lsn;
		}

		if (need_rotate
		    && !log_online_rotate_bitmap_file(file_start_lsn)) {

			exit(1);
		}

		if (last_tracked_lsn < tracking_start_lsn) {

			log_online_track_missing_on_startup
				(last_tracked_lsn, tracking_start_lsn);
			return;
		}

		if (last_tracked_lsn > tracking_start_lsn) {

			ib::warn() << "Last tracked LSN is "
				   << last_tracked_lsn << ", but the last "
				"checkpoint LSN is " << tracking_start_lsn
				   << ". The tracking-based incremental "
				"backups will work only from the latter LSN!";
		}

	}

	ib::info() << "Starting tracking changed pages from LSN "
		   << tracking_start_lsn;
	log_bmp_sys->start_lsn = tracking_start_lsn;
	log_set_tracked_lsn(tracking_start_lsn);
}

/** Shut down the dynamic part of the log tracking subsystem */

void
log_online_read_shutdown(void)
{
	mutex_enter(&log_bmp_sys_mutex);

	srv_track_changed_pages = FALSE;

	ib_rbt_node_t *free_list_node = log_bmp_sys->page_free_list;

	if (!log_bmp_sys->out.file.is_closed()) {
		os_file_close(log_bmp_sys->out.file);
		log_bmp_sys->out.file.set_closed();
	}

	rbt_free(log_bmp_sys->modified_pages);

	while (free_list_node) {
		ib_rbt_node_t *next = free_list_node->left;
		ut_free(free_list_node);
		free_list_node = next;
	}

	ut_free(log_bmp_sys->read_buf_ptr);
	ut_free(log_bmp_sys);
	log_bmp_sys = NULL;

	srv_redo_log_thread_started = false;

	mutex_exit(&log_bmp_sys_mutex);
}

/** Shut down the constant part of the log tracking subsystem */

void
log_online_shutdown(void)
{
	mutex_free(&log_bmp_sys_mutex);
}

/*********************************************************************//**
For the given minilog record type determine if the record has (space; page)
associated with it.
@return true if the record has (space; page) in it */
static
bool
log_online_rec_has_page(
/*====================*/
	mlog_id_t	type)	/*!<in: the minilog record type */
{
	return type != MLOG_MULTI_REC_END
		&& type != MLOG_DUMMY_RECORD
		&& type != MLOG_CHECKPOINT
		&& type != MLOG_TRUNCATE;
}

/*********************************************************************//**
Check if a page field for a given log record type actually contains a page
id. It does not for file operations and MLOG_LSN.
@return true if page field contains actual page id, false otherwise */
static
bool
log_online_rec_page_means_page(
/*===========================*/
	mlog_id_t	type)	/*!<in: log record type */
{
	return log_online_rec_has_page(type)
#ifdef UNIV_LOG_LSN_DEBUG
		&& type != MLOG_LSN
#endif
		;
}

/*********************************************************************//**
Parse the log data in the parse buffer for the (space, page) pairs and add
them to the modified page set as necessary.  Removes the fully-parsed records
from the buffer.  If an incomplete record is found, moves it to the end of the
buffer. */
static
void
log_online_parse_redo_log(void)
/*===========================*/
{
	ut_ad(mutex_own(&log_bmp_sys_mutex));

	byte *ptr = log_bmp_sys->parse_buf;
	byte *end = log_bmp_sys->parse_buf_end;
	ulint len = 0;

	while (ptr != end
	       && log_bmp_sys->next_parse_lsn < log_bmp_sys->end_lsn) {

		mlog_id_t	type;
		ulint		space;
		ulint		page_no;
		byte*		body;

		/* recv_sys is not initialized, so on corrupt log we will
		SIGSEGV.  But the log of a live database should not be
		corrupt. */
		len = recv_parse_log_rec(&type, ptr, end, &space, &page_no,
					 false, &body);
		if (len > 0) {

			if (log_online_rec_page_means_page(type)) {

				ut_a(len >= 3);
				log_online_set_page_bit(space, page_no);
				if (type == MLOG_INDEX_LOAD) {
					const ulint space_size =
						fil_space_get_size(space);
					for (ulint i = 0; i < space_size; i++) {
						log_online_set_page_bit(
							space, i);
					}
				}
			}

			ptr += len;
			ut_ad(ptr <= end);
			log_bmp_sys->next_parse_lsn
			    = recv_calc_lsn_on_data_add
				(log_bmp_sys->next_parse_lsn, len);
		}
		else {

			/* Incomplete log record.  Shift it to the
			beginning of the parse buffer and leave it to be
			completed on the next read.  */
			ut_memmove(log_bmp_sys->parse_buf, ptr, end - ptr);
			log_bmp_sys->parse_buf_end
				= log_bmp_sys->parse_buf + (end - ptr);
			ptr = end;
		}
	}

	if (len > 0) {

		log_bmp_sys->parse_buf_end = log_bmp_sys->parse_buf;
	}
}

/*********************************************************************//**
Check the log block checksum.
@return true if the log block checksum is OK, false otherwise.  */
MY_ATTRIBUTE((warn_unused_result))
static
bool
log_online_is_valid_log_seg(
/*========================*/
	const byte* log_block,	/*!< in: read log data */
	lsn_t	    log_block_lsn)/*!< in: expected LSN of the log block */
{
	bool checksum_is_ok = log_block_checksum_is_ok(log_block);

	if (!checksum_is_ok) {

		// We are reading empty log blocks in some cases (such as
		// tracking log on server startup with log resizing). Such
		// blocks are benign, silently accept them.
		byte zero_block[OS_FILE_LOG_BLOCK_SIZE] = { 0 };
		if (!memcmp(log_block, zero_block, OS_FILE_LOG_BLOCK_SIZE))
			return true;

		ulint no = log_block_get_hdr_no(log_block);
		ulint expected_no = log_block_convert_lsn_to_no(log_block_lsn);
		ib::error() << "Log block checksum mismatch: LSN "
			    << log_block_lsn << ", expected "
			    << log_block_get_checksum(log_block) << ", "
			    << "calculated checksum "
			    << log_block_calc_checksum(log_block) << ", "
			    << "stored log block n:o " << no << ", "
			    << "expected log block n:o " << expected_no;
		ut_error;
	}

	return checksum_is_ok;
}

/*********************************************************************//**
Copy new log data to the parse buffer while skipping log block header,
trailer and already parsed data.  */
static
void
log_online_add_to_parse_buf(
/*========================*/
	const byte*	log_block,	/*!< in: read log data */
	ulint		data_len,	/*!< in: length of read log data */
	ulint		skip_len)	/*!< in: how much of log data to
					skip */
{
	ut_ad(mutex_own(&log_bmp_sys_mutex));

	ulint start_offset = skip_len ? skip_len : LOG_BLOCK_HDR_SIZE;
	ulint end_offset
		= (data_len == OS_FILE_LOG_BLOCK_SIZE)
		? data_len - LOG_BLOCK_TRL_SIZE
		: data_len;
	ulint actual_data_len = (end_offset >= start_offset)
		? end_offset - start_offset : 0;

	ut_memcpy(log_bmp_sys->parse_buf_end, log_block + start_offset,
		  actual_data_len);

	log_bmp_sys->parse_buf_end += actual_data_len;

	ut_a(log_bmp_sys->parse_buf_end - log_bmp_sys->parse_buf
	     <= RECV_PARSING_BUF_SIZE);
}

/*********************************************************************//**
Parse the log block: first copies the read log data to the parse buffer while
skipping log block header, trailer and already parsed data.  Then it actually
parses the log to add to the modified page bitmap. */
static
void
log_online_parse_redo_log_block(
/*============================*/
	const byte*	log_block,		  /*!< in: read log data */
	ulint		skip_already_parsed_len)  /*!< in: how many bytes of
						  log data should be skipped as
						  they were parsed before */
{
	ut_ad(mutex_own(&log_bmp_sys_mutex));

	ulint block_data_len = log_block_get_data_len(log_block);

	ut_ad(block_data_len % OS_FILE_LOG_BLOCK_SIZE == 0
	      || block_data_len < OS_FILE_LOG_BLOCK_SIZE);

	log_online_add_to_parse_buf(log_block, block_data_len,
				    skip_already_parsed_len);
	log_online_parse_redo_log();
}

/*********************************************************************//**
Read and parse one redo log chunk and updates the modified page bitmap. */
MY_ATTRIBUTE((warn_unused_result))
static
bool
log_online_follow_log_seg(
/*======================*/
	log_group_t*	group,		       /*!< in: the log group to use */
	lsn_t		block_start_lsn,       /*!< in: the LSN to read from */
	lsn_t		block_end_lsn)	       /*!< in: the LSN to read to */
{
	ut_ad(mutex_own(&log_bmp_sys_mutex));

	/* Pointer to the current OS_FILE_LOG_BLOCK-sized chunk of the read log
	data to parse */
	byte* log_block = log_bmp_sys->read_buf;
	byte* log_block_end = log_bmp_sys->read_buf
		+ (block_end_lsn - block_start_lsn);

	log_mutex_enter();
	log_group_read_log_seg(log_bmp_sys->read_buf, group, block_start_lsn,
			       block_end_lsn, true);
	/* log_group_read_log_seg will release the log_sys->mutex for us */

	while (log_block < log_block_end
	       && log_bmp_sys->next_parse_lsn < log_bmp_sys->end_lsn) {

		/* How many bytes of log data should we skip in the current log
		block.  Skipping is necessary because we round down the next
		parse LSN thus it is possible to read the already-processed log
		data many times */
		ulint skip_already_parsed_len = 0;

		if (!log_online_is_valid_log_seg(log_block, block_start_lsn)) {
			return false;
		}

		if ((block_start_lsn <= log_bmp_sys->next_parse_lsn)
		    && (block_start_lsn + OS_FILE_LOG_BLOCK_SIZE
			> log_bmp_sys->next_parse_lsn)) {

			/* The next parse LSN is inside the current block, skip
			data preceding it. */
			skip_already_parsed_len
				= (ulint)(log_bmp_sys->next_parse_lsn
					  - block_start_lsn);
		}
		else {

			/* If the next parse LSN is not inside the current
			block, then the only option is that we have processed
			ahead already. */
			ut_a(block_start_lsn > log_bmp_sys->next_parse_lsn);
		}

		/* TODO: merge the copying to the parse buf code with
		skip_already_len calculations */
		log_online_parse_redo_log_block(log_block,
						skip_already_parsed_len);

		log_block += OS_FILE_LOG_BLOCK_SIZE;
		block_start_lsn += OS_FILE_LOG_BLOCK_SIZE;
	}

	return true;
}

/*********************************************************************//**
Read and parse the redo log in a given group in FOLLOW_SCAN_SIZE-sized
chunks and updates the modified page bitmap. */
MY_ATTRIBUTE((warn_unused_result))
static
bool
log_online_follow_log_group(
/*========================*/
	log_group_t*	group,		/*!< in: the log group to use */
	lsn_t		contiguous_lsn)	/*!< in: the LSN of log block start
					containing the log_parse_start_lsn */
{
	ut_ad(mutex_own(&log_bmp_sys_mutex));

	lsn_t	block_start_lsn = contiguous_lsn;
	lsn_t	block_end_lsn;

	log_bmp_sys->next_parse_lsn = log_bmp_sys->start_lsn;
	log_bmp_sys->parse_buf_end = log_bmp_sys->parse_buf;

	do {
		block_end_lsn = block_start_lsn + FOLLOW_SCAN_SIZE;

		if (!log_online_follow_log_seg(group, block_start_lsn,
					       block_end_lsn))
			return false;

		/* Next parse LSN can become higher than the last read LSN
		only in the case when the read LSN falls right on the block
		boundary, in which case next parse lsn is bumped to the actual
		data LSN on the next (not yet read) block.  This assert is
		slightly conservative.  */
		ut_a(log_bmp_sys->next_parse_lsn
		     <= block_end_lsn + LOG_BLOCK_HDR_SIZE
		     + LOG_BLOCK_TRL_SIZE);

		block_start_lsn = block_end_lsn;
	} while (block_end_lsn < log_bmp_sys->end_lsn);

	/* Assert that the last read log record is a full one */
	ut_a(log_bmp_sys->parse_buf_end == log_bmp_sys->parse_buf);
	return true;
}

/*********************************************************************//**
Write, flush one bitmap block to disk and advance the output position if
successful.

@return true if page written OK, false if I/O error */
static
bool
log_online_write_bitmap_page(
/*=========================*/
	const byte *block)	/*!< in: block to write */
{
	ut_ad(srv_track_changed_pages);
	ut_ad(mutex_own(&log_bmp_sys_mutex));

	/* Simulate a write error */
	DBUG_EXECUTE_IF("bitmap_page_write_error",
			{
				ulint space_id
					= mach_read_from_4(block
					+ MODIFIED_PAGE_SPACE_ID);
				if (space_id > 0) {
					ib::error() <<
						"simulating bitmap write "
						"error in "
						"log_online_write_bitmap_page "
						"for space ID " << space_id;
					return false;
				}
			});

	/* A crash injection site that ensures last checkpoint LSN > last
	tracked LSN, so that LSN tracking for this interval is tested. */
	DBUG_EXECUTE_IF("crash_before_bitmap_write",
			{
				ulint space_id
					= mach_read_from_4(block
						+ MODIFIED_PAGE_SPACE_ID);
				if (space_id > 0)
					DBUG_SUICIDE();
			});

	IORequest io_request(IORequest::WRITE | IORequest::NO_COMPRESSION);
	bool success = os_file_write(io_request, log_bmp_sys->out.name,
				log_bmp_sys->out.file, block,
				log_bmp_sys->out.offset,
				MODIFIED_PAGE_BLOCK_SIZE);
	if (UNIV_UNLIKELY(!success)) {

		/* The following call prints an error message */
		os_file_get_last_error(true);
		ib::error() << "Failed writing changed page bitmap file \'"
			    << log_bmp_sys->out.name << "\'";
		return false;
	}

	success = os_file_flush(log_bmp_sys->out.file);
	if (UNIV_UNLIKELY(!success)) {

		/* The following call prints an error message */
		os_file_get_last_error(true);
		ib::error() << "Failed flushing changed page bitmap file \'"
			    << log_bmp_sys->out.name << "\'";
		return false;
	}

	os_file_advise(log_bmp_sys->out.file, log_bmp_sys->out.offset,
		       MODIFIED_PAGE_BLOCK_SIZE, OS_FILE_ADVISE_DONTNEED);

	log_bmp_sys->out.offset += MODIFIED_PAGE_BLOCK_SIZE;
	return true;
}

/*********************************************************************//**
Append the current changed page bitmap to the bitmap file.  Clears the
bitmap tree and recycles its nodes to the free list.

@return true if bitmap written OK, false if I/O error*/
static
bool
log_online_write_bitmap(void)
/*=========================*/
{
	ut_ad(mutex_own(&log_bmp_sys_mutex));

	if (log_bmp_sys->out.offset >= srv_max_bitmap_file_size) {
		if (!log_online_rotate_bitmap_file(log_bmp_sys->start_lsn)) {
			return false;
		}
	}

	ib_rbt_node_t *bmp_tree_node
		= (ib_rbt_node_t *)rbt_first(log_bmp_sys->modified_pages);
	const ib_rbt_node_t * const last_bmp_tree_node
		= rbt_last(log_bmp_sys->modified_pages);

	bool success = true;

	while (bmp_tree_node) {

		byte *page = rbt_value(byte, bmp_tree_node);

		/* In case of a bitmap page write error keep on looping over
		the tree to reclaim its memory through the free list instead of
		returning immediatelly. */
		if (UNIV_LIKELY(success)) {
			if (bmp_tree_node == last_bmp_tree_node) {
				mach_write_to_4(page
						+ MODIFIED_PAGE_IS_LAST_BLOCK,
						1);
			}

			mach_write_to_8(page + MODIFIED_PAGE_START_LSN,
				       log_bmp_sys->start_lsn);
			mach_write_to_8(page + MODIFIED_PAGE_END_LSN,
				       log_bmp_sys->end_lsn);
			mach_write_to_4(page + MODIFIED_PAGE_BLOCK_CHECKSUM,
					log_online_calc_checksum(page));

			success = log_online_write_bitmap_page(page);
		}

		bmp_tree_node->left = log_bmp_sys->page_free_list;
		log_bmp_sys->page_free_list = bmp_tree_node;

		bmp_tree_node = (ib_rbt_node_t*)
			rbt_next(log_bmp_sys->modified_pages, bmp_tree_node);

		DBUG_EXECUTE_IF("bitmap_page_2_write_error",
				if (bmp_tree_node)
				{
					DBUG_SET("+d,bitmap_page_write_error");
					DBUG_SET("-d,bitmap_page_2_write_error");
				});
	}

	rbt_reset(log_bmp_sys->modified_pages);
	return success;
}

/*********************************************************************//**
Read and parse the redo log up to last checkpoint LSN to build the changed
page bitmap which is then written to disk.

@return true if log tracking succeeded, false if bitmap write I/O error */

bool
log_online_follow_redo_log(void)
/*============================*/
{
	lsn_t		contiguous_start_lsn;
	log_group_t*	group;
	bool		result;

	ut_ad(!srv_read_only_mode);

	if (!srv_track_changed_pages)
		return true;

	DEBUG_SYNC_C("log_online_follow_redo_log");

	mutex_enter(&log_bmp_sys_mutex);

	if (!srv_track_changed_pages) {
		mutex_exit(&log_bmp_sys_mutex);
		return true;
	}

	/* Grab the LSN of the last checkpoint, we will parse up to it */
	log_mutex_enter();
	log_bmp_sys->end_lsn = log_sys->last_checkpoint_lsn;
	log_mutex_exit();

	if (log_bmp_sys->end_lsn == log_bmp_sys->start_lsn) {
		mutex_exit(&log_bmp_sys_mutex);
		return true;
	}

	group = UT_LIST_GET_FIRST(log_sys->log_groups);
	ut_a(group);

	contiguous_start_lsn = ut_uint64_align_down(log_bmp_sys->start_lsn,
						    OS_FILE_LOG_BLOCK_SIZE);

	while (group) {
		result = log_online_follow_log_group(group,
						     contiguous_start_lsn);
		if (!result)
			goto end;
		group = UT_LIST_GET_NEXT(log_groups, group);
	}

	result = log_online_write_bitmap();
	log_bmp_sys->start_lsn = log_bmp_sys->end_lsn;
	log_set_tracked_lsn(log_bmp_sys->start_lsn);

end:
	mutex_exit(&log_bmp_sys_mutex);
	return result;
}

/*********************************************************************//**
Diagnose a bitmap file range setup failure and free the partially-initialized
bitmap file range.  */
UNIV_COLD
static
void
log_online_diagnose_inconsistent_dir(
/*=================================*/
	log_online_bitmap_file_range_t	*bitmap_files)	/*!<in/out: bitmap file
							range */
{
	ib::warn() << "Inconsistent bitmap file directory for a "
		"INFORMATION_SCHEMA.INNODB_CHANGED_PAGES query";
	ut_free(bitmap_files->files);
}

/*********************************************************************//**
List the bitmap files in srv_data_home and setup their range that contains the
specified LSN interval.  This range, if non-empty, will start with a file that
has the greatest LSN equal to or less than the start LSN and will include all
the files up to the one with the greatest LSN less than the end LSN.  Caller
must free bitmap_files->files when done if bitmap_files set to non-NULL and
this function returned true.  Field bitmap_files->count might be set to a
larger value than the actual count of the files, and space for the unused array
slots will be allocated but cleared to zeroes.

@return true if succeeded
*/
static
bool
log_online_setup_bitmap_file_range(
/*===============================*/
	log_online_bitmap_file_range_t	*bitmap_files,	/*!<in/out: bitmap file
							range */
	lsn_t				range_start,	/*!<in: start LSN */
	lsn_t				range_end)	/*!<in: end LSN */
{
	os_file_dir_t	bitmap_dir;
	os_file_stat_t	bitmap_dir_file_info;
	ulong		first_file_seq_num	= ULONG_MAX;
	ulong		last_file_seq_num	= 0;
	lsn_t		first_file_start_lsn	= LSN_MAX;

	ut_ad(range_end >= range_start);

	bitmap_files->count = 0;
	bitmap_files->files = NULL;

	/* 1st pass: size the info array */

	bitmap_dir = os_file_opendir(srv_data_home, false);
	if (UNIV_UNLIKELY(!bitmap_dir)) {

		ib::error() << "Failed to open bitmap directory \'"
			    << srv_data_home << "\'";
		return false;
	}

	while (!os_file_readdir_next_file(srv_data_home, bitmap_dir,
					  &bitmap_dir_file_info)) {

		ulong	file_seq_num;
		lsn_t	file_start_lsn;

		if (!log_online_is_bitmap_file(&bitmap_dir_file_info,
					       &file_seq_num,
					       &file_start_lsn)
		    || file_start_lsn >= range_end) {

			continue;
		}

		if (file_seq_num > last_file_seq_num) {

			last_file_seq_num = file_seq_num;
		}

		if (file_start_lsn >= range_start
		    || file_start_lsn == first_file_start_lsn
		    || first_file_start_lsn > range_start) {

			/* A file that falls into the range */

			if (file_start_lsn < first_file_start_lsn) {

				first_file_start_lsn = file_start_lsn;
			}
			if (file_seq_num < first_file_seq_num) {

				first_file_seq_num = file_seq_num;
			}
		} else if (file_start_lsn > first_file_start_lsn) {

			/* A file that has LSN closer to the range start
			but smaller than it, replacing another such file */
			first_file_start_lsn = file_start_lsn;
			first_file_seq_num = file_seq_num;
		}
	}

	if (UNIV_UNLIKELY(os_file_closedir(bitmap_dir))) {

		os_file_get_last_error(true);
		ib::error() << "Cannot close \'"
			    << srv_data_home << "\'";
		return false;
	}

	if (first_file_seq_num == ULONG_MAX && last_file_seq_num == 0) {

		bitmap_files->count = 0;
		return true;
	}

	bitmap_files->count = last_file_seq_num - first_file_seq_num + 1;

	DEBUG_SYNC_C("setup_bitmap_range_middle");

	/* 2nd pass: get the file names in the file_seq_num order */

	bitmap_dir = os_file_opendir(srv_data_home, false);
	if (UNIV_UNLIKELY(!bitmap_dir)) {

		ib::error() << "Failed to open bitmap directory \'"
			    << srv_data_home << "\'";
		return false;
	}

	bitmap_files->files
		= static_cast<log_online_bitmap_file_range_struct::files_t *>
		(ut_zalloc(bitmap_files->count
			   * sizeof(bitmap_files->files[0]),
			   mem_key_log_online_iterator_files));

	while (!os_file_readdir_next_file(srv_data_home, bitmap_dir,
					  &bitmap_dir_file_info)) {

		ulong	file_seq_num;
		lsn_t	file_start_lsn;
		size_t	array_pos;

		if (!log_online_is_bitmap_file(&bitmap_dir_file_info,
					       &file_seq_num,
					       &file_start_lsn)
		    || file_start_lsn >= range_end
		    || file_start_lsn < first_file_start_lsn) {

			continue;
		}

		array_pos = file_seq_num - first_file_seq_num;
		if (UNIV_UNLIKELY(array_pos >= bitmap_files->count)) {

			log_online_diagnose_inconsistent_dir(bitmap_files);
			return false;
		}


		if (file_seq_num > bitmap_files->files[array_pos].seq_num) {

			bitmap_files->files[array_pos].seq_num = file_seq_num;
			memcpy(bitmap_files->files[array_pos].name,
				bitmap_dir_file_info.name, FN_REFLEN);
			bitmap_files->files[array_pos].name[FN_REFLEN - 1]
				= '\0';
			bitmap_files->files[array_pos].start_lsn
				= file_start_lsn;
		}
	}

	if (UNIV_UNLIKELY(os_file_closedir(bitmap_dir))) {

		os_file_get_last_error(true);
		ib::error() << "Cannot close \'"
			    << srv_data_home << "\'";
		free(bitmap_files->files);
		return false;
	}

	if (!bitmap_files->files[0].seq_num
	    || bitmap_files->files[0].seq_num != first_file_seq_num) {

		log_online_diagnose_inconsistent_dir(bitmap_files);
		return false;
	}

	{
		size_t i;
		for (i = 1; i < bitmap_files->count; i++) {
			if (!bitmap_files->files[i].seq_num) {
				break;
			}
			if ((bitmap_files->files[i].seq_num
			      <= bitmap_files->files[i - 1].seq_num)
			    || (bitmap_files->files[i].start_lsn
				< bitmap_files->files[i - 1].start_lsn)) {

				log_online_diagnose_inconsistent_dir(
								bitmap_files);
				return false;
			}
		}
	}

	return true;
}

/****************************************************************//**
Open a bitmap file for reading.

@return true if opened successfully */
static
bool
log_online_open_bitmap_file_read_only(
/*==================================*/
	const char*			name,		/*!<in: bitmap file
							name without directory,
							which is assumed to be
							srv_data_home */
	log_online_bitmap_file_t*	bitmap_file)	/*!<out: opened bitmap
							file */
{
	bool	success	= false;
	size_t  srv_data_home_len;

	ut_ad(name[0] != '\0');

	srv_data_home_len = strlen(srv_data_home);
	if (srv_data_home_len
			&& srv_data_home[srv_data_home_len-1]
			!= SRV_PATH_SEPARATOR) {
		ut_snprintf(bitmap_file->name, sizeof(bitmap_file->name), "%s%c%s",
				srv_data_home, SRV_PATH_SEPARATOR, name);
	} else {
		ut_snprintf(bitmap_file->name, sizeof(bitmap_file->name), "%s%s",
				srv_data_home, name);
	}
	bitmap_file->file
		= os_file_create_simple_no_error_handling(innodb_bmp_file_key,
							  bitmap_file->name,
							  OS_FILE_OPEN,
							  OS_FILE_READ_ONLY,
							  srv_read_only_mode,
							  &success);
	if (UNIV_UNLIKELY(!success)) {

		/* Here and below assume that bitmap file names do not
		contain apostrophes, thus no need for ut_print_filename(). */
		ib::warn() << "Error opening the changed page bitmap \'"
			   << bitmap_file->name << "\'";
		return false;
	}

	bitmap_file->size = os_file_get_size(bitmap_file->file);
	bitmap_file->offset = 0;

	os_file_advise(bitmap_file->file, 0, 0, OS_FILE_ADVISE_SEQUENTIAL);
	os_file_advise(bitmap_file->file, 0, 0, OS_FILE_ADVISE_NOREUSE);

	return true;
}

/****************************************************************//**
Diagnose one or both of the following situations if we read close to
the end of bitmap file:
1) Warn if the remainder of the file is less than one page.
2) Error if we cannot read any more full pages but the last read page
did not have the last-in-run flag set.

@return false for the error */
static
bool
log_online_diagnose_bitmap_eof(
/*===========================*/
	const log_online_bitmap_file_t*	bitmap_file,	/*!< in: bitmap file */
	bool				last_page_in_run)/*!< in: "last page in
							run" flag value in the
							last read page */
{
	/* Check if we are too close to EOF to read a full page */
	if ((bitmap_file->size < MODIFIED_PAGE_BLOCK_SIZE)
	    || (bitmap_file->offset
		> bitmap_file->size - MODIFIED_PAGE_BLOCK_SIZE)) {

		if (UNIV_UNLIKELY(bitmap_file->offset != bitmap_file->size)) {

			/* If we are not at EOF and we have less than one page
			to read, it's junk.  This error is not fatal in
			itself. */

			ib::warn() << "Junk at the end of changed page bitmap "
				"file \'" << bitmap_file->name << "\'.";
		}

		if (UNIV_UNLIKELY(!last_page_in_run)) {

			/* We are at EOF but the last read page did not finish
			a run */
			/* It's a "Warning" here because it's not a fatal error
			for the whole server */
			ib::warn() << "Changed page bitmap file \'"
				   << bitmap_file->name << "\', size "
				   << bitmap_file->size << " bytes, does not "
				"contain a complete run at the next read "
				"offset " << bitmap_file->offset;
			return false;
		}
	}
	return true;
}

/*********************************************************************//**
Initialize the log bitmap iterator for a given range.  The records are
processed at a bitmap block granularity, i.e. all the records in the same block
share the same start and end LSN values, the exact LSN of each record is
unavailable (nor is it defined for blocks that are touched more than once in
the LSN interval contained in the block).  Thus min_lsn and max_lsn should be
set at block boundaries or bigger, otherwise the records at the 1st and the
last blocks will not be returned.  Also note that there might be returned
records with LSN < min_lsn, as min_lsn is used to select the correct starting
file but not block.

@return true if the iterator is initialized OK, false otherwise. */

bool
log_online_bitmap_iterator_init(
/*============================*/
	log_bitmap_iterator_t	*i,	/*!<in/out:  iterator */
	lsn_t			min_lsn,/*!< in: start LSN */
	lsn_t			max_lsn)/*!< in: end LSN */
{
	ut_a(i);

	i->max_lsn = max_lsn;

	if (UNIV_UNLIKELY(min_lsn > max_lsn)) {

		/* Empty range */
		i->in_files.count = 0;
		i->in_files.files = NULL;
		i->in.file.set_closed();
		i->page = NULL;
		i->failed = false;
		return true;
	}

	if (!log_online_setup_bitmap_file_range(&i->in_files, min_lsn,
		max_lsn)) {

		i->failed = true;
		return false;
	}

	i->in_i = 0;

	if (i->in_files.count == 0) {

		/* Empty range */
		i->in.file.set_closed();
		i->page = NULL;
		i->failed = false;
		return true;
	}

	/* Open the 1st bitmap file */
	if (UNIV_UNLIKELY(!log_online_open_bitmap_file_read_only(
				i->in_files.files[i->in_i].name,
				&i->in))) {

		i->in_i = i->in_files.count;
		ut_free(i->in_files.files);
		i->failed = true;
		return false;
	}

	i->page = static_cast<byte *>
		(ut_malloc(MODIFIED_PAGE_BLOCK_SIZE,
			   mem_key_log_online_iterator_page));
	i->bit_offset = MODIFIED_PAGE_BLOCK_BITMAP_LEN;
	i->start_lsn = i->end_lsn = 0;
	i->space_id = 0;
	i->first_page_id = 0;
	i->last_page_in_run = true;
	i->changed = false;
	i->failed = false;

	return true;
}

/*********************************************************************//**
Releases log bitmap iterator. */

void
log_online_bitmap_iterator_release(
/*===============================*/
	log_bitmap_iterator_t *i) /*!<in/out:  iterator */
{
	ut_a(i);

	if (!i->in.file.is_closed()) {

		os_file_close(i->in.file);
		i->in.file.set_closed();
	}
	if (i->in_files.files) {

		ut_free(i->in_files.files);
	}
	if (i->page) {

		ut_free(i->page);
	}
	i->failed = true;
}

/*********************************************************************//**
Iterates through bits of saved bitmap blocks.
Sequentially reads blocks from bitmap file(s) and interates through
their bits. Ignores blocks with wrong checksum.
@return true if iteration is successful, false if all bits are iterated. */

bool
log_online_bitmap_iterator_next(
/*============================*/
	log_bitmap_iterator_t *i) /*!<in/out: iterator */
{
	bool	checksum_ok = false;
	bool	success;

	ut_a(i);

	if (UNIV_UNLIKELY(i->in_files.count == 0)) {

		return false;
	}

	if (UNIV_LIKELY(i->bit_offset < MODIFIED_PAGE_BLOCK_BITMAP_LEN))
	{
		++i->bit_offset;
		i->changed =
			IS_BIT_SET(i->page + MODIFIED_PAGE_BLOCK_BITMAP,
				   i->bit_offset);
		return true;
	}

	if (i->end_lsn >= i->max_lsn && i->last_page_in_run)
		return false;

	while (!checksum_ok)
	{
		while (i->in.size < MODIFIED_PAGE_BLOCK_SIZE
		       || (i->in.offset
			   > i->in.size - MODIFIED_PAGE_BLOCK_SIZE)) {

			/* Advance file */
			i->in_i++;
			success = os_file_close_no_error_handling(i->in.file);
			i->in.file.set_closed();
			if (UNIV_UNLIKELY(!success)) {

				os_file_get_last_error(true);
				i->failed = true;
				return false;
			}

			success = log_online_diagnose_bitmap_eof(
					&i->in, i->last_page_in_run);
			if (UNIV_UNLIKELY(!success)) {

				i->failed = true;
				return false;

			}

			if (i->in_i == i->in_files.count) {

				return false;
			}

			if (UNIV_UNLIKELY(i->in_files.files[i->in_i].seq_num
					  == 0)) {

				i->failed = true;
				return false;
			}

			success = log_online_open_bitmap_file_read_only(
					i->in_files.files[i->in_i].name,
					&i->in);
			if (UNIV_UNLIKELY(!success)) {

				i->failed = true;
				return false;
			}
		}

		success = log_online_read_bitmap_page(&i->in, i->page,
						      &checksum_ok);
		if (UNIV_UNLIKELY(!success)) {

			os_file_get_last_error(true);
			ib::warn() << "Failed reading changed page bitmap "
				"file \'" << i->in_files.files[i->in_i].name
				   << "\'";
			i->failed = true;
			return false;
		}
	}

	i->start_lsn = mach_read_from_8(i->page + MODIFIED_PAGE_START_LSN);
	i->end_lsn = mach_read_from_8(i->page + MODIFIED_PAGE_END_LSN);
	i->space_id = mach_read_from_4(i->page + MODIFIED_PAGE_SPACE_ID);
	i->first_page_id = mach_read_from_4(i->page
					    + MODIFIED_PAGE_1ST_PAGE_ID);
	i->last_page_in_run = mach_read_from_4(i->page
					       + MODIFIED_PAGE_IS_LAST_BLOCK);
	i->bit_offset = 0;
	i->changed = IS_BIT_SET(i->page + MODIFIED_PAGE_BLOCK_BITMAP,
				i->bit_offset);

	return true;
}

/************************************************************//**
Delete all the bitmap files for data less than the specified LSN.
If called with lsn == 0 (i.e. set by RESET request) or LSN_MAX,
restart the bitmap file sequence, otherwise continue it.

@return false to indicate success, true for failure. */

bool
log_online_purge_changed_page_bitmaps(
/*==================================*/
	lsn_t	lsn)	/*!< in: LSN to purge files up to */
{
	log_online_bitmap_file_range_t	bitmap_files;
	size_t				i;
	bool				result = false;

	if (lsn == 0) {
		lsn = LSN_MAX;
	}

	bool log_bmp_sys_inited = false;
	if (srv_redo_log_thread_started) {
		/* User requests might happen with both enabled and disabled
		tracking */
		log_bmp_sys_inited = true;
		mutex_enter(&log_bmp_sys_mutex);
		if (!srv_redo_log_thread_started) {
			log_bmp_sys_inited = false;
			mutex_exit(&log_bmp_sys_mutex);
		}
	}

	if (!log_online_setup_bitmap_file_range(&bitmap_files, 0, LSN_MAX)) {
		if (log_bmp_sys_inited) {
			mutex_exit(&log_bmp_sys_mutex);
		}
		return true;
	}

	if (srv_redo_log_thread_started && lsn > log_bmp_sys->end_lsn) {
		/* If we have to delete the current output file, close it
		first. */
		os_file_close(log_bmp_sys->out.file);
		log_bmp_sys->out.file.set_closed();
	}

	for (i = 0; i < bitmap_files.count; i++) {

		char	full_bmp_file_name[2 * FN_REFLEN + 2];

		/* We consider the end LSN of the current bitmap, derived from
		the start LSN of the subsequent bitmap file, to determine
		whether to remove the current bitmap.  Note that bitmap_files
		does not contain an entry for the bitmap past the given LSN so
		we must check the boundary conditions as well.  For example,
		consider 1_0.xdb and 2_10.xdb and querying LSN 5.  bitmap_files
		will only contain 1_0.xdb and we must not delete it since it
		represents LSNs 0-9. */
		if ((i + 1 == bitmap_files.count
		     || bitmap_files.files[i + 1].seq_num == 0
		     || bitmap_files.files[i + 1].start_lsn > lsn)
		    && (lsn != LSN_MAX)) {

			break;
		}

		/* In some non-trivial cases the sequence of .xdb files may
		have gaps. For instance:
			ib_modified_log_1_0.xdb
			ib_modified_log_2_<mmm>.xdb
			ib_modified_log_4_<nnn>.xdb
		Adding this check as a safety precaution. */
		if (bitmap_files.files[i].name[0] == '\0')
			continue;

		/* If redo log tracking is enabled, reuse 'bmp_file_home'
		from 'log_bmp_sys'. Otherwise, compose the full '.xdb' file
		path from 'srv_data_home', adding a path separator if
		necessary. */
		if (log_bmp_sys != NULL) {
			ut_snprintf(full_bmp_file_name,
				sizeof(full_bmp_file_name),
				"%s%s", log_bmp_sys->bmp_file_home,
				bitmap_files.files[i].name);
		}
		else {
			char		separator[2] = {0, 0};
			const size_t	srv_data_home_len =
				strlen(srv_data_home);

			ut_a(srv_data_home_len < FN_REFLEN);
			if (srv_data_home_len != 0 &&
				srv_data_home[srv_data_home_len - 1] !=
				SRV_PATH_SEPARATOR) {
				separator[0] = SRV_PATH_SEPARATOR;
			}
			ut_snprintf(full_bmp_file_name,
				sizeof(full_bmp_file_name), "%s%s%s",
				srv_data_home, separator,
				bitmap_files.files[i].name);
		}

		if (!os_file_delete_if_exists(innodb_bmp_file_key,
					      full_bmp_file_name,
					      NULL)) {

			os_file_get_last_error(true);
			result = true;
			break;
		}
	}

	if (log_bmp_sys_inited) {
		if (lsn > log_bmp_sys->end_lsn) {
			lsn_t	new_file_lsn;
			if (lsn == LSN_MAX) {
				/* RESET restarts the sequence */
				log_bmp_sys->out_seq_num = 0;
				new_file_lsn = 0;
			} else {
				new_file_lsn = log_bmp_sys->end_lsn;
			}
			if (!log_online_rotate_bitmap_file(new_file_lsn)) {
				/* If file create failed, stop log tracking */
				srv_track_changed_pages = FALSE;
			}
		}

		mutex_exit(&log_bmp_sys_mutex);
	}

	ut_free(bitmap_files.files);
	return result;
}
