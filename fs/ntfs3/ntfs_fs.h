/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  linux/fs/ntfs3/ntfs_fs.h
 *
 * Copyright (C) 2019-2020 Paragon Software GmbH, All rights reserved.
 *
 */

/* "true" when [s,s+c) intersects with [l,l+w) */
#define IS_IN_RANGE(s, c, l, w)                                                \
	(((c) > 0 && (w) > 0) &&                                               \
	 (((l) <= (s) && (s) < ((l) + (w))) ||                                 \
	  ((s) <= (l) && ((s) + (c)) >= ((l) + (w))) ||                        \
	  ((l) < ((s) + (c)) && ((s) + (c)) < ((l) + (w)))))

/* "true" when [s,se) intersects with [l,le) */
#define IS_IN_RANGE2(s, se, l, le)                                             \
	(((se) > (s) && (le) > (l)) &&                                         \
	 (((l) <= (s) && (s) < (le)) || ((s) <= (l) && (se) >= (le)) ||        \
	  ((l) < (se) && (se) < (le))))

#define MINUS_ONE_T ((size_t)(-1))
/* Biggest MFT / smallest cluster */
#define MAXIMUM_BYTES_PER_MFT 4096 // ??
#define NTFS_BLOCKS_PER_MFT_RECORD (MAXIMUM_BYTES_PER_MFT / 512)

#define MAXIMUM_BYTES_PER_INDEX 4096 // ??
#define NTFS_BLOCKS_PER_INODE (MAXIMUM_BYTES_PER_INDEX / 512)

struct ntfs_inode;
struct ntfs_sb_info;
struct lznt;

struct mount_options {
	kuid_t fs_uid;
	kgid_t fs_gid;
	u16 fs_fmask_inv;
	u16 fs_dmask_inv;

	unsigned uid : 1, /* uid was set */
		gid : 1, /* gid was set */
		fmask : 1, /* fmask was set */
		dmask : 1, /*dmask was set*/
		sys_immutable : 1, /* set = system files are immutable */
		discard : 1, /* issue discard requests on deletions */
		sparse : 1, /*create sparse files*/
		showmeta : 1, /*show meta files*/
		nohidden : 1, /*do not show hidden files*/
		force : 1, /*rw mount dirty volume*/
		no_acs_rules : 1, /*exclude acs rules*/
		prealloc : 1 /*preallocate space when file is growing*/
		;
};

struct ntfs_run;

/* TODO: use rb tree instead of array */
struct runs_tree {
	struct ntfs_run *runs_;
	size_t count; // Currently used size a ntfs_run storage.
	size_t allocated; // Currently allocated ntfs_run storage size.
};

struct ntfs_buffers {
	/* Biggest MFT / smallest cluster = 4096 / 512 = 8 */
	/* Biggest index / smallest cluster = 4096 / 512 = 8 */
	struct buffer_head *bh[PAGE_SIZE >> SECTOR_SHIFT];
	u32 bytes;
	u32 nbufs;
	u32 off;
};

#define NTFS_FLAGS_NODISCARD 0x00000001
#define NTFS_FLAGS_NEED_REPLAY 0x04000000

enum ALLOCATE_OPT {
	ALLOCATE_DEF = 0, // Allocate all clusters
	ALLOCATE_MFT = 1, // Allocate for MFT
};

enum bitmap_mutex_classes {
	BITMAP_MUTEX_CLUSTERS = 0,
	BITMAP_MUTEX_MFT = 1,
};

struct wnd_bitmap {
	struct super_block *sb;
	struct rw_semaphore rw_lock;

	struct runs_tree run;
	size_t nbits;

	u16 free_holder[8]; // holder for free_bits

	size_t total_zeroes; // total number of free bits
	u16 *free_bits; // free bits in each window
	size_t nwnd;
	u32 bits_last; // bits in last window

	struct rb_root start_tree; // extents, sorted by 'start'
	struct rb_root count_tree; // extents, sorted by 'count + start'
	size_t count; // extents count
	int uptodated; // -1 Tree is activated but not updated (too many fragments)
		// 0 - Tree is not activated
		// 1 - Tree is activated and updated
	size_t extent_min; // Minimal extent used while building
	size_t extent_max; // Upper estimate of biggest free block

	bool set_tail; // not necessary in driver
	bool inited;

	/* Zone [bit, end) */
	size_t zone_bit;
	size_t zone_end;
};

typedef int (*NTFS_CMP_FUNC)(const void *key1, size_t len1, const void *key2,
			     size_t len2, const void *param);

enum index_mutex_classed {
	INDEX_MUTEX_I30 = 0,
	INDEX_MUTEX_SII = 1,
	INDEX_MUTEX_SDH = 2,
	INDEX_MUTEX_SO = 3,
	INDEX_MUTEX_SQ = 4,
	INDEX_MUTEX_SR = 5,
	INDEX_MUTEX_TOTAL
};

/* This struct works with indexes */
struct ntfs_index {
	struct runs_tree bitmap_run;
	struct runs_tree alloc_run;

	/*TODO: remove 'cmp'*/
	NTFS_CMP_FUNC cmp;

	u8 index_bits; // log2(root->index_block_size)
	u8 idx2vbn_bits; // log2(root->index_block_clst)
	u8 vbn2vbo_bits; // index_block_size < cluster? 9 : cluster_bits
	u8 changed; // set when tree is changed
	u8 type; // index_mutex_classed
};

/* Set when $LogFile is replaying */
#define NTFS_FLAGS_LOG_REPLAYING 0x00000008

/* Set when we changed first MFT's which copy must be updated in $MftMirr */
#define NTFS_FLAGS_MFTMIRR 0x00001000

/* Minimum mft zone */
#define NTFS_MIN_MFT_ZONE 100

struct COMPRESS_CTX {
	u64 chunk_num; // Number of chunk cmpr_buffer/unc_buffer
	u64 first_chunk, last_chunk, total_chunks;
	u64 chunk0_off;
	void *ctx;
	u8 *cmpr_buffer;
	u8 *unc_buffer;
	void *chunk_off_mem;
	size_t chunk_off;
	u32 *chunk_off32; // pointer inside ChunkOffsetsMem
	u64 *chunk_off64; // pointer inside ChunkOffsetsMem
	u32 compress_format;
	u32 offset_bits;
	u32 chunk_bits;
	u32 chunk_size;
};

/* ntfs file system in-core superblock data */
struct ntfs_sb_info {
	struct super_block *sb;

	u32 discard_granularity;
	u64 discard_granularity_mask_inv; // ~(discard_granularity_mask_inv-1)

	u32 cluster_size; // bytes per cluster
	u32 cluster_mask; // == cluster_size - 1
	u64 cluster_mask_inv; // ~(cluster_size - 1)
	u32 block_mask; // sb->s_blocksize - 1
	u32 blocks_per_cluster; // cluster_size / sb->s_blocksize

	u32 record_size;
	u32 sector_size;
	u32 index_size;

	u8 sector_bits;
	u8 cluster_bits;
	u8 record_bits;

	u64 maxbytes; // Maximum size for normal files
	u64 maxbytes_sparse; // Maximum size for sparse file

	u32 flags; // See NTFS_FLAGS_XXX

	CLST bad_clusters; // The count of marked bad clusters

	u16 max_bytes_per_attr; // maximum attribute size in record
	u16 attr_size_tr; // attribute size threshold (320 bytes)

	/* Records in $Extend */
	CLST objid_no;
	CLST quota_no;
	CLST reparse_no;
	CLST usn_jrnl_no;

	struct ATTR_DEF_ENTRY *def_table; // attribute definition table
	u32 def_entries;

	struct MFT_REC *new_rec;

	u16 *upcase;

	struct nls_table *nls[2];

	struct {
		u64 lbo, lbo2;
		struct ntfs_inode *ni;
		struct wnd_bitmap bitmap; // $MFT::Bitmap
		ulong reserved_bitmap;
		size_t next_free; // The next record to allocate from
		size_t used;
		u32 recs_mirr; // Number of records MFTMirr
		u8 next_reserved;
		u8 reserved_bitmap_inited;
	} mft;

	struct {
		struct wnd_bitmap bitmap; // $Bitmap::Data
		CLST next_free_lcn;
	} used;

	struct {
		u64 size; // in bytes
		u64 blocks; // in blocks
		u64 ser_num;
		struct ntfs_inode *ni;
		__le16 flags; // see VOLUME_FLAG_XXX
		u8 major_ver;
		u8 minor_ver;
		char label[65];
		bool real_dirty; /* real fs state*/
	} volume;

	struct {
		struct ntfs_index index_sii;
		struct ntfs_index index_sdh;
		struct ntfs_inode *ni;
		u32 next_id;
		u64 next_off;

		__le32 def_security_id;
	} security;

	struct {
		struct ntfs_index index_r;
		struct ntfs_inode *ni;
		u64 max_size; // 16K
	} reparse;

	struct {
		struct ntfs_index index_o;
		struct ntfs_inode *ni;
	} objid;

	struct {
		/*protect 'frame_unc' and 'ctx'*/
		spinlock_t lock;
		u8 *frame_unc;
		struct lznt *ctx;
	} compress;

	struct mount_options options;
	struct ratelimit_state msg_ratelimit;
};

struct mft_inode {
	struct rb_node node;
	struct ntfs_sb_info *sbi;

	CLST rno;
	struct MFT_REC *mrec;
	struct ntfs_buffers nb;

	bool dirty;
};

#define NI_FLAG_DIR 0x00000001
#define NI_FLAG_RESIDENT 0x00000002
#define NI_FLAG_UPDATE_PARENT 0x00000004

/* Data attribute is compressed special way */
#define NI_FLAG_COMPRESSED_MASK 0x00000f00 //
/* Data attribute is deduplicated */
#define NI_FLAG_DEDUPLICATED 0x00001000
#define NI_FLAG_EA 0x00002000

/* ntfs file system inode data memory */
struct ntfs_inode {
	struct mft_inode mi; // base record

	loff_t i_valid; /* valid size */
	struct timespec64 i_crtime;

	struct mutex ni_lock;

	/* file attributes from std */
	enum FILE_ATTRIBUTE std_fa;
	__le32 std_security_id;

	// subrecords tree
	struct rb_root mi_tree;

	union {
		struct ntfs_index dir;
		struct {
			struct rw_semaphore run_lock;
			struct runs_tree run;
		} file;
	};

	struct {
		struct runs_tree run;
		struct ATTR_LIST_ENTRY *le; // 1K aligned memory
		size_t size;
		bool dirty;
	} attr_list;

	size_t ni_flags; // NI_FLAG_XXX

	struct inode vfs_inode;
};

struct indx_node {
	struct ntfs_buffers nb;
	struct INDEX_BUFFER *index;
};

struct ntfs_fnd {
	int level;
	struct indx_node *nodes[20];
	struct NTFS_DE *de[20];
	struct NTFS_DE *root_de;
};

enum REPARSE_SIGN {
	REPARSE_NONE = 0,
	REPARSE_COMPRESSED = 1,
	REPARSE_DEDUPLICATED = 2,
	REPARSE_LINK = 3
};

/* functions from attrib.c*/
int attr_load_runs(struct ATTRIB *attr, struct ntfs_inode *ni,
		   struct runs_tree *run);
int attr_allocate_clusters(struct ntfs_sb_info *sbi, struct runs_tree *run,
			   CLST vcn, CLST lcn, CLST len, CLST *pre_alloc,
			   enum ALLOCATE_OPT opt, CLST *alen, const size_t fr,
			   CLST *new_lcn);
int attr_set_size(struct ntfs_inode *ni, enum ATTR_TYPE type,
		  const __le16 *name, u8 name_len, struct runs_tree *run,
		  u64 new_size, const u64 *new_valid, bool keep_prealloc,
		  struct ATTRIB **ret);
int attr_data_get_block(struct ntfs_inode *ni, CLST vcn, CLST clen, CLST *lcn,
			CLST *len, bool *new);
int attr_load_runs_vcn(struct ntfs_inode *ni, enum ATTR_TYPE type,
		       const __le16 *name, u8 name_len, struct runs_tree *run,
		       CLST vcn);
int attr_is_frame_compressed(struct ntfs_inode *ni, struct ATTRIB *attr,
			     CLST frame, CLST *clst_data, bool *is_compr);
int attr_allocate_frame(struct ntfs_inode *ni, CLST frame, size_t compr_size,
			u64 new_valid);

/* functions from attrlist.c*/
void al_destroy(struct ntfs_inode *ni);
bool al_verify(struct ntfs_inode *ni);
int ntfs_load_attr_list(struct ntfs_inode *ni, struct ATTRIB *attr);
struct ATTR_LIST_ENTRY *al_enumerate(struct ntfs_inode *ni,
				     struct ATTR_LIST_ENTRY *le);
struct ATTR_LIST_ENTRY *al_find_le(struct ntfs_inode *ni,
				   struct ATTR_LIST_ENTRY *le,
				   const struct ATTRIB *attr);
struct ATTR_LIST_ENTRY *al_find_ex(struct ntfs_inode *ni,
				   struct ATTR_LIST_ENTRY *le,
				   enum ATTR_TYPE type, const __le16 *name,
				   u8 name_len, const CLST *vcn);
int al_add_le(struct ntfs_inode *ni, enum ATTR_TYPE type, const __le16 *name,
	      u8 name_len, CLST svcn, __le16 id, const struct MFT_REF *ref,
	      struct ATTR_LIST_ENTRY **new_le);
bool al_remove_le(struct ntfs_inode *ni, struct ATTR_LIST_ENTRY *le);
bool al_delete_le(struct ntfs_inode *ni, enum ATTR_TYPE type, CLST vcn,
		  const __le16 *name, size_t name_len,
		  const struct MFT_REF *ref);
int al_update(struct ntfs_inode *ni);
static inline size_t al_aligned(size_t size)
{
	return (size + 1023) & ~(size_t)1023;
}

/* globals from bitfunc.c */
bool are_bits_clear(const ulong *map, size_t bit, size_t nbits);
bool are_bits_set(const ulong *map, size_t bit, size_t nbits);
size_t get_set_bits_ex(const ulong *map, size_t bit, size_t nbits);

/* globals from dir.c */
int ntfs_utf16_to_nls(struct ntfs_sb_info *sbi, const struct le_str *uni,
		      u8 *buf, int buf_len);
int ntfs_nls_to_utf16(struct ntfs_sb_info *sbi, const u8 *name, u32 name_len,
		      struct cpu_str *uni, u32 max_ulen,
		      enum utf16_endian endian);
struct inode *dir_search_u(struct inode *dir, const struct cpu_str *uni,
			   struct ntfs_fnd *fnd);
bool dir_is_empty(struct inode *dir);
extern const struct file_operations ntfs_dir_operations;

/* globals from file.c*/
int ntfs_getattr(const struct path *path, struct kstat *stat, u32 request_mask,
		 u32 flags);
void ntfs_sparse_cluster(struct inode *inode, struct page *page0, loff_t vbo,
			 u32 bytes);
int ntfs_file_fsync(struct file *filp, loff_t start, loff_t end, int datasync);
void ntfs_truncate_blocks(struct inode *inode, loff_t offset);
int ntfs_setattr(struct dentry *dentry, struct iattr *attr);
int ntfs_file_open(struct inode *inode, struct file *file);
int ntfs_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
		__u64 start, __u64 len);
extern const struct inode_operations ntfs_special_inode_operations;
extern const struct inode_operations ntfs_file_inode_operations;
extern const struct file_operations ntfs_file_operations;

/* globals from frecord.c */
void ni_remove_mi(struct ntfs_inode *ni, struct mft_inode *mi);
struct ATTR_STD_INFO *ni_std(struct ntfs_inode *ni);
struct ATTR_STD_INFO5 *ni_std5(struct ntfs_inode *ni);
void ni_clear(struct ntfs_inode *ni);
int ni_load_mi_ex(struct ntfs_inode *ni, CLST rno, struct mft_inode **mi);
int ni_load_mi(struct ntfs_inode *ni, struct ATTR_LIST_ENTRY *le,
	       struct mft_inode **mi);
struct ATTRIB *ni_find_attr(struct ntfs_inode *ni, struct ATTRIB *attr,
			    struct ATTR_LIST_ENTRY **entry_o,
			    enum ATTR_TYPE type, const __le16 *name,
			    u8 name_len, const CLST *vcn,
			    struct mft_inode **mi);
struct ATTRIB *ni_enum_attr_ex(struct ntfs_inode *ni, struct ATTRIB *attr,
			       struct ATTR_LIST_ENTRY **le);
struct ATTRIB *ni_load_attr(struct ntfs_inode *ni, enum ATTR_TYPE type,
			    const __le16 *name, u8 name_len, CLST vcn,
			    struct mft_inode **pmi);
int ni_load_all_mi(struct ntfs_inode *ni);
bool ni_add_subrecord(struct ntfs_inode *ni, CLST rno, struct mft_inode **mi);
int ni_remove_attr(struct ntfs_inode *ni, enum ATTR_TYPE type,
		   const __le16 *name, size_t name_len, bool base_only,
		   const __le16 *id);
int ni_create_attr_list(struct ntfs_inode *ni);
int ni_expand_list(struct ntfs_inode *ni);
int ni_insert_nonresident(struct ntfs_inode *ni, enum ATTR_TYPE type,
			  const __le16 *name, u8 name_len,
			  const struct runs_tree *run, CLST svcn, CLST len,
			  __le16 flags, struct ATTRIB **new_attr,
			  struct mft_inode **mi);
int ni_insert_resident(struct ntfs_inode *ni, u32 data_size,
		       enum ATTR_TYPE type, const __le16 *name, u8 name_len,
		       struct ATTRIB **new_attr, struct mft_inode **mi);
int ni_remove_attr_le(struct ntfs_inode *ni, struct ATTRIB *attr,
		      struct ATTR_LIST_ENTRY *le);
int ni_delete_all(struct ntfs_inode *ni);
struct ATTR_FILE_NAME *ni_fname_name(struct ntfs_inode *ni,
				     const struct cpu_str *uni,
				     const struct MFT_REF *home,
				     struct ATTR_LIST_ENTRY **entry);
struct ATTR_FILE_NAME *ni_fname_type(struct ntfs_inode *ni, u8 name_type,
				     struct ATTR_LIST_ENTRY **entry);
u16 ni_fnames_count(struct ntfs_inode *ni);
int ni_init_compress(struct ntfs_inode *ni, struct COMPRESS_CTX *ctx);
enum REPARSE_SIGN ni_parse_reparse(struct ntfs_inode *ni, struct ATTRIB *attr,
				   void *buffer);
int ni_write_inode(struct inode *inode, int sync, const char *hint);
#define _ni_write_inode(i, w) ni_write_inode(i, w, __func__)
int ni_fiemap(struct ntfs_inode *ni, struct fiemap_extent_info *fieinfo,
	      __u64 vbo, __u64 len);
int ni_readpage_cmpr(struct ntfs_inode *ni, struct page *page);
int ni_writepage_cmpr(struct page *page, int sync);

/* globals from fslog.c */
int log_replay(struct ntfs_inode *ni);

/* globals from fsntfs.c */
bool ntfs_fix_pre_write(struct NTFS_RECORD_HEADER *rhdr, size_t bytes);
int ntfs_fix_post_read(struct NTFS_RECORD_HEADER *rhdr, size_t bytes,
		       bool simple);
int ntfs_extend_init(struct ntfs_sb_info *sbi);
int ntfs_loadlog_and_replay(struct ntfs_inode *ni, struct ntfs_sb_info *sbi);
const struct ATTR_DEF_ENTRY *ntfs_query_def(struct ntfs_sb_info *sbi,
					    enum ATTR_TYPE Type);
int ntfs_look_for_free_space(struct ntfs_sb_info *sbi, CLST lcn, CLST len,
			     CLST *new_lcn, CLST *new_len,
			     enum ALLOCATE_OPT opt);
int ntfs_look_free_mft(struct ntfs_sb_info *sbi, CLST *rno, bool mft,
		       struct ntfs_inode *ni, struct mft_inode **mi);
void ntfs_mark_rec_free(struct ntfs_sb_info *sbi, CLST nRecord);
int ntfs_clear_mft_tail(struct ntfs_sb_info *sbi, size_t from, size_t to);
int ntfs_refresh_zone(struct ntfs_sb_info *sbi);
int ntfs_update_mftmirr(struct ntfs_sb_info *sbi, int wait);
enum NTFS_DIRTY_FLAGS {
	NTFS_DIRTY_CLEAR = 0,
	NTFS_DIRTY_DIRTY = 1,
	NTFS_DIRTY_ERROR = 2,
};
int ntfs_set_state(struct ntfs_sb_info *sbi, enum NTFS_DIRTY_FLAGS dirty);
int ntfs_sb_read(struct super_block *sb, u64 lbo, size_t bytes, void *buffer);
int ntfs_sb_write(struct super_block *sb, u64 lbo, size_t bytes,
		  const void *buffer, int wait);
int ntfs_sb_write_run(struct ntfs_sb_info *sbi, struct runs_tree *run, u64 vbo,
		      const void *buf, size_t bytes);
struct buffer_head *ntfs_bread_run(struct ntfs_sb_info *sbi,
				   struct runs_tree *run, u64 vbo);
int ntfs_read_run_nb(struct ntfs_sb_info *sbi, struct runs_tree *run, u64 vbo,
		     void *buf, u32 bytes, struct ntfs_buffers *nb);
int ntfs_read_bh(struct ntfs_sb_info *sbi, struct runs_tree *run, u64 vbo,
		 struct NTFS_RECORD_HEADER *rhdr, u32 bytes,
		 struct ntfs_buffers *nb);
int ntfs_get_bh(struct ntfs_sb_info *sbi, struct runs_tree *run, u64 vbo,
		u32 bytes, struct ntfs_buffers *nb);
int ntfs_write_bh(struct ntfs_sb_info *sbi, struct NTFS_RECORD_HEADER *rhdr,
		  struct ntfs_buffers *nb, int sync);
int ntfs_vbo_to_lbo(struct ntfs_sb_info *sbi, struct runs_tree *run, u64 vbo,
		    u64 *lbo, u64 *bytes);
struct ntfs_inode *ntfs_new_inode(struct ntfs_sb_info *sbi, CLST nRec,
				  bool dir);
extern const u8 s_default_security[0x50];
int ntfs_security_init(struct ntfs_sb_info *sbi);
int ntfs_get_security_by_id(struct ntfs_sb_info *sbi, __le32 security_id,
			    void **sd, size_t *size);
int ntfs_insert_security(struct ntfs_sb_info *sbi, const void *sd, u32 size,
			 __le32 *security_id, bool *inserted);
int ntfs_reparse_init(struct ntfs_sb_info *sbi);
int ntfs_objid_init(struct ntfs_sb_info *sbi);
int ntfs_objid_remove(struct ntfs_sb_info *sbi, struct GUID *guid);
int ntfs_insert_reparse(struct ntfs_sb_info *sbi, __le32 rtag,
			const struct MFT_REF *ref);
int ntfs_remove_reparse(struct ntfs_sb_info *sbi, __le32 rtag,
			const struct MFT_REF *ref);
void mark_as_free_ex(struct ntfs_sb_info *sbi, CLST lcn, CLST len, bool trim);
int run_deallocate(struct ntfs_sb_info *sbi, struct runs_tree *run, bool trim);

/* globals from index.c */
int indx_used_bit(struct ntfs_index *indx, struct ntfs_inode *ni, size_t *bit);
void fnd_clear(struct ntfs_fnd *fnd);
struct ntfs_fnd *fnd_get(struct ntfs_index *indx);
void fnd_put(struct ntfs_fnd *fnd);
void indx_clear(struct ntfs_index *idx);
int indx_init(struct ntfs_index *indx, struct ntfs_sb_info *sbi,
	      const struct ATTRIB *attr, enum index_mutex_classed type);
struct INDEX_ROOT *indx_get_root(struct ntfs_index *indx, struct ntfs_inode *ni,
				 struct ATTRIB **attr, struct mft_inode **mi);
int indx_read(struct ntfs_index *idx, struct ntfs_inode *ni, CLST vbn,
	      struct indx_node **node);
int indx_find(struct ntfs_index *indx, struct ntfs_inode *dir,
	      const struct INDEX_ROOT *root, const void *Key, size_t KeyLen,
	      const void *param, int *diff, struct NTFS_DE **entry,
	      struct ntfs_fnd *fnd);
int indx_find_sort(struct ntfs_index *indx, struct ntfs_inode *ni,
		   const struct INDEX_ROOT *root, struct NTFS_DE **entry,
		   struct ntfs_fnd *fnd);
int indx_find_raw(struct ntfs_index *indx, struct ntfs_inode *ni,
		  const struct INDEX_ROOT *root, struct NTFS_DE **entry,
		  size_t *off, struct ntfs_fnd *fnd);
int indx_insert_entry(struct ntfs_index *indx, struct ntfs_inode *ni,
		      const struct NTFS_DE *new_de, const void *param,
		      struct ntfs_fnd *fnd);
int indx_delete_entry(struct ntfs_index *indx, struct ntfs_inode *ni,
		      const void *key, u32 key_len, const void *param);
int indx_update_dup(struct ntfs_inode *ni, struct ntfs_sb_info *sbi,
		    const struct ATTR_FILE_NAME *fname,
		    const struct NTFS_DUP_INFO *dup, int sync);

/* globals from inode.c */
struct inode *ntfs_iget5(struct super_block *sb, const struct MFT_REF *ref,
			 const struct cpu_str *name);
int ntfs_set_size(struct inode *inode, u64 new_size);
int reset_log_file(struct inode *inode);
int ntfs_get_block(struct inode *inode, sector_t vbn,
		   struct buffer_head *bh_result, int create);
int ntfs_write_inode(struct inode *inode, struct writeback_control *wbc);
int ntfs_sync_inode(struct inode *inode);
int ntfs_flush_inodes(struct super_block *sb, struct inode *i1,
		      struct inode *i2);
int inode_write_data(struct inode *inode, const void *data, size_t bytes);
int ntfs_create_inode(struct inode *dir, struct dentry *dentry,
		      const struct cpu_str *uni, struct file *file,
		      umode_t mode, dev_t dev, const char *symname,
		      unsigned int size, int excl, struct ntfs_fnd *fnd,
		      struct inode **new_inode);
int ntfs_link_inode(struct inode *inode, struct dentry *dentry);
int ntfs_unlink_inode(struct inode *dir, const struct dentry *dentry);
void ntfs_evict_inode(struct inode *inode);
int ntfs_readpage(struct file *file, struct page *page);
extern const struct inode_operations ntfs_link_inode_operations;
extern const struct address_space_operations ntfs_aops;
extern const struct address_space_operations ntfs_aops_cmpr;

/* globals from name_i.c*/
int fill_name_de(struct ntfs_sb_info *sbi, void *buf, const struct qstr *name,
		 const struct cpu_str *uni);
struct dentry *ntfs_get_parent(struct dentry *child);

extern const struct inode_operations ntfs_dir_inode_operations;

/* globals from record.c */
int mi_get(struct ntfs_sb_info *sbi, CLST rno, struct mft_inode **mi);
void mi_put(struct mft_inode *mi);
int mi_init(struct mft_inode *mi, struct ntfs_sb_info *sbi, CLST rno);
int mi_read(struct mft_inode *mi, bool is_mft);
struct ATTRIB *mi_enum_attr(struct mft_inode *mi, struct ATTRIB *attr);
// TODO: id?
struct ATTRIB *mi_find_attr(struct mft_inode *mi, struct ATTRIB *attr,
			    enum ATTR_TYPE type, const __le16 *name,
			    size_t name_len, const __le16 *id);
static inline struct ATTRIB *rec_find_attr_le(struct mft_inode *rec,
					      struct ATTR_LIST_ENTRY *le)
{
	return mi_find_attr(rec, NULL, le->type, le_name(le), le->name_len,
			    &le->id);
}
int mi_write(struct mft_inode *mi, int wait);
int mi_format_new(struct mft_inode *mi, struct ntfs_sb_info *sbi, CLST rno,
		  __le16 flags, bool is_mft);
void mi_mark_free(struct mft_inode *mi);
struct ATTRIB *mi_insert_attr(struct mft_inode *mi, enum ATTR_TYPE type,
			      const __le16 *name, u8 name_len, u32 asize,
			      u16 name_off);

bool mi_remove_attr(struct mft_inode *mi, struct ATTRIB *attr);
bool mi_resize_attr(struct mft_inode *mi, struct ATTRIB *attr, int bytes);
int mi_pack_runs(struct mft_inode *mi, struct ATTRIB *attr,
		 struct runs_tree *run, CLST len);
static inline bool mi_is_ref(const struct mft_inode *mi,
			     const struct MFT_REF *ref)
{
	if (le32_to_cpu(ref->low) != mi->rno)
		return false;
	if (ref->seq != mi->mrec->seq)
		return false;

#ifdef NTFS3_64BIT_CLUSTER
	return le16_to_cpu(ref->high) == (mi->rno >> 32);
#else
	return !ref->high;
#endif
}

/* globals from run.c */
bool run_lookup_entry(const struct runs_tree *run, CLST vcn, CLST *lcn,
		      CLST *len, size_t *index);
void run_truncate(struct runs_tree *run, CLST vcn);
void run_truncate_head(struct runs_tree *run, CLST vcn);
bool run_lookup(const struct runs_tree *run, CLST Vcn, size_t *Index);
bool run_add_entry(struct runs_tree *run, CLST vcn, CLST lcn, CLST len);
bool run_get_entry(const struct runs_tree *run, size_t index, CLST *vcn,
		   CLST *lcn, CLST *len);
bool run_is_mapped_full(const struct runs_tree *run, CLST svcn, CLST evcn);

int run_pack(const struct runs_tree *run, CLST svcn, CLST len, u8 *run_buf,
	     u32 run_buf_size, CLST *packed_vcns);
int run_unpack(struct runs_tree *run, struct ntfs_sb_info *sbi, CLST ino,
	       CLST svcn, CLST evcn, const u8 *run_buf, u32 run_buf_size);

#ifdef NTFS3_CHECK_FREE_CLST
int run_unpack_ex(struct runs_tree *run, struct ntfs_sb_info *sbi, CLST ino,
		  CLST svcn, CLST evcn, const u8 *run_buf, u32 run_buf_size);
#else
#define run_unpack_ex run_unpack
#endif
int run_get_highest_vcn(CLST vcn, const u8 *run_buf, u64 *highest_vcn);

/* globals from super.c */
void *ntfs_set_shared(void *ptr, u32 bytes);
void *ntfs_put_shared(void *ptr);
void ntfs_unmap_meta(struct super_block *sb, CLST lcn, CLST len);
int ntfs_discard(struct ntfs_sb_info *sbi, CLST Lcn, CLST Len);

/* globals from ubitmap.c*/
void wnd_close(struct wnd_bitmap *wnd);
static inline size_t wnd_zeroes(const struct wnd_bitmap *wnd)
{
	return wnd->total_zeroes;
}
void wnd_trace(struct wnd_bitmap *wnd);
void wnd_trace_tree(struct wnd_bitmap *wnd, u32 nExtents, const char *Hint);
int wnd_init(struct wnd_bitmap *wnd, struct super_block *sb, size_t nBits);
int wnd_set_free(struct wnd_bitmap *wnd, size_t FirstBit, size_t Bits);
int wnd_set_used(struct wnd_bitmap *wnd, size_t FirstBit, size_t Bits);
bool wnd_is_free(struct wnd_bitmap *wnd, size_t FirstBit, size_t Bits);
bool wnd_is_used(struct wnd_bitmap *wnd, size_t FirstBit, size_t Bits);

/* Possible values for 'flags' 'wnd_find' */
#define BITMAP_FIND_MARK_AS_USED 0x01
#define BITMAP_FIND_FULL 0x02
size_t wnd_find(struct wnd_bitmap *wnd, size_t to_alloc, size_t hint,
		size_t flags, size_t *allocated);
int wnd_extend(struct wnd_bitmap *wnd, size_t new_bits);
void wnd_zone_set(struct wnd_bitmap *wnd, size_t Lcn, size_t Len);
int ntfs_trim_fs(struct ntfs_sb_info *sbi, struct fstrim_range *range);

/* globals from upcase.c */
int ntfs_cmp_names(const __le16 *s1, size_t l1, const __le16 *s2, size_t l2,
		   const u16 *upcase);
int ntfs_cmp_names_cpu(const struct cpu_str *uni1, const struct le_str *uni2,
		       const u16 *upcase);

/* globals from xattr.c */
struct posix_acl *ntfs_get_acl(struct inode *inode, int type);
int ntfs_set_acl(struct inode *inode, struct posix_acl *acl, int type);
int ntfs_acl_chmod(struct inode *inode);
int ntfs_permission(struct inode *inode, int mask);
ssize_t ntfs_listxattr(struct dentry *dentry, char *buffer, size_t size);
int ntfs_init_acl(struct inode *inode, struct inode *dir);
extern const struct xattr_handler *ntfs_xattr_handlers[];

/* globals from lznt.c */
struct lznt *get_compression_ctx(bool std);
size_t compress_lznt(const void *uncompressed, size_t uncompressed_size,
		     void *compressed, size_t compressed_size,
		     struct lznt *ctx);
ssize_t decompress_lznt(const void *compressed, size_t compressed_size,
			void *uncompressed, size_t uncompressed_size);

char *attr_str(const struct ATTRIB *attr, char *buf, size_t buf_len);

static inline bool is_ntfs3(struct ntfs_sb_info *sbi)
{
	return sbi->volume.major_ver >= 3;
}

/*(sb->s_flags & SB_ACTIVE)*/
static inline bool is_mounted(struct ntfs_sb_info *sbi)
{
	return !!sbi->sb->s_root;
}

static inline bool ntfs_is_meta_file(struct ntfs_sb_info *sbi, CLST rno)
{
	return rno < MFT_REC_FREE || rno == sbi->objid_no ||
	       rno == sbi->quota_no || rno == sbi->reparse_no ||
	       rno == sbi->usn_jrnl_no;
}

static inline void ntfs_unmap_page(struct page *page)
{
	kunmap(page);
	put_page(page);
}

static inline struct page *ntfs_map_page(struct address_space *mapping,
					 unsigned long index)
{
	struct page *page = read_mapping_page(mapping, index, NULL);

	if (!IS_ERR(page)) {
		kmap(page);
		if (!PageError(page))
			return page;
		ntfs_unmap_page(page);
		return ERR_PTR(-EIO);
	}
	return page;
}

static inline size_t wnd_zone_bit(const struct wnd_bitmap *wnd)
{
	return wnd->zone_bit;
}

static inline size_t wnd_zone_len(const struct wnd_bitmap *wnd)
{
	return wnd->zone_end - wnd->zone_bit;
}

static inline void run_init(struct runs_tree *run)
{
	run->runs_ = NULL;
	run->count = 0;
	run->allocated = 0;
}

static inline struct runs_tree *run_alloc(void)
{
	return ntfs_alloc(sizeof(struct runs_tree), 1);
}

static inline void run_close(struct runs_tree *run)
{
	ntfs_free(run->runs_);
	memset(run, 0, sizeof(*run));
}

static inline void run_free(struct runs_tree *run)
{
	if (run) {
		ntfs_free(run->runs_);
		ntfs_free(run);
	}
}

static inline bool run_is_empty(struct runs_tree *run)
{
	return !run->count;
}

/* NTFS uses quad aligned bitmaps */
static inline size_t bitmap_size(size_t bits)
{
	return QuadAlign((bits + 7) >> 3);
}

#define _100ns2seconds 10000000
#define SecondsToStartOf1970 0x00000002B6109100

#define NTFS_TIME_GRAN 100

/*
 * kernel2nt
 *
 * converts in-memory kernel timestamp into nt time
 */
static inline __le64 kernel2nt(const struct timespec64 *ts)
{
	// 10^7 units of 100 nanoseconds one second
	return cpu_to_le64(_100ns2seconds *
				   (ts->tv_sec + SecondsToStartOf1970) +
			   ts->tv_nsec / NTFS_TIME_GRAN);
}

/*
 * nt2kernel
 *
 * converts on-disk nt time into kernel timestamp
 */
static inline void nt2kernel(const __le64 tm, struct timespec64 *ts)
{
	u64 t = le64_to_cpu(tm) - _100ns2seconds * SecondsToStartOf1970;

	// WARNING: do_div changes its first argument(!)
	ts->tv_nsec = do_div(t, _100ns2seconds) * 100;
	ts->tv_sec = t;
}

static inline struct ntfs_sb_info *ntfs_sb(struct super_block *sb)
{
	return sb->s_fs_info;
}

/* Align up on cluster boundary */
static inline u64 ntfs_up_cluster(const struct ntfs_sb_info *sbi, u64 size)
{
	return (size + sbi->cluster_mask) & ~((u64)sbi->cluster_mask);
}

/* Align up on cluster boundary */
static inline u64 ntfs_up_block(const struct super_block *sb, u64 size)
{
	return (size + sb->s_blocksize - 1) & ~(u64)(sb->s_blocksize - 1);
}

static inline CLST bytes_to_cluster(const struct ntfs_sb_info *sbi, u64 size)
{
	return (size + sbi->cluster_mask) >> sbi->cluster_bits;
}

static inline u64 bytes_to_block(const struct super_block *sb, u64 size)
{
	return (size + sb->s_blocksize - 1) >> sb->s_blocksize_bits;
}

/* calculates ((bytes + frame_size - 1)/frame_size)*frame_size; */
static inline u64 ntfs_up_frame(const struct ntfs_sb_info *sbi, u64 bytes,
				u8 c_unit)
{
	u32 bytes_per_frame = 1u << (c_unit + sbi->cluster_bits);

	return (bytes + bytes_per_frame - 1) & ~(u64)(bytes_per_frame - 1);
}

static inline struct buffer_head *ntfs_bread(struct super_block *sb,
					     sector_t block)
{
	struct buffer_head *bh;

	bh = sb_bread(sb, block);
	if (bh)
		return bh;

	ntfs_err(sb, "failed to read volume at offset 0x%llx",
		 (u64)block << sb->s_blocksize_bits);
	return NULL;
}

static inline bool is_power_of2(size_t v)
{
	return v && !(v & (v - 1));
}

static inline struct ntfs_inode *ntfs_i(struct inode *inode)
{
	return container_of(inode, struct ntfs_inode, vfs_inode);
}

static inline bool is_compressed(const struct ntfs_inode *ni)
{
	return (ni->std_fa & FILE_ATTRIBUTE_COMPRESSED) ||
	       (ni->ni_flags & NI_FLAG_COMPRESSED_MASK);
}

static inline bool is_dedup(const struct ntfs_inode *ni)
{
	return ni->ni_flags & NI_FLAG_DEDUPLICATED;
}

static inline bool is_encrypted(const struct ntfs_inode *ni)
{
	return ni->std_fa & FILE_ATTRIBUTE_ENCRYPTED;
}

static inline bool is_sparsed(const struct ntfs_inode *ni)
{
	return ni->std_fa & FILE_ATTRIBUTE_SPARSE_FILE;
}

static inline void le16_sub_cpu(__le16 *var, u16 val)
{
	*var = cpu_to_le16(le16_to_cpu(*var) - val);
}

static inline void le32_sub_cpu(__le32 *var, u32 val)
{
	*var = cpu_to_le32(le32_to_cpu(*var) - val);
}

static inline void nb_put(struct ntfs_buffers *nb)
{
	u32 i, nbufs = nb->nbufs;

	if (!nbufs)
		return;

	for (i = 0; i < nbufs; i++)
		put_bh(nb->bh[i]);
	nb->nbufs = 0;
}

static inline void put_indx_node(struct indx_node *in)
{
	if (!in)
		return;

	ntfs_free(in->index);
	nb_put(&in->nb);
	ntfs_free(in);
}

static inline void mi_clear(struct mft_inode *mi)
{
	nb_put(&mi->nb);
	ntfs_free(mi->mrec);
	mi->mrec = NULL;
}

static inline void ni_lock(struct ntfs_inode *ni)
{
	mutex_lock(&ni->ni_lock);
}

static inline void ni_unlock(struct ntfs_inode *ni)
{
	mutex_unlock(&ni->ni_lock);
}

static inline int ni_trylock(struct ntfs_inode *ni)
{
	return mutex_trylock(&ni->ni_lock);
}

static inline int ni_has_resident_data(struct ntfs_inode *ni)
{
	return ni->ni_flags & NI_FLAG_RESIDENT;
}

static inline int attr_load_runs_attr(struct ntfs_inode *ni,
				      struct ATTRIB *attr,
				      struct runs_tree *run, CLST vcn)
{
	return attr_load_runs_vcn(ni, attr->type, attr_name(attr),
				  attr->name_len, run, vcn);
}

static inline void le64_sub_cpu(__le64 *var, u64 val)
{
	*var = cpu_to_le64(le64_to_cpu(*var) - val);
}
