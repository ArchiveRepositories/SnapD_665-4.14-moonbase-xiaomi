/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  linux/fs/ntfs3/debug.h
 *
 * Copyright (C) 2019-2020 Paragon Software GmbH, All rights reserved.
 *
 * useful functions for debuging
 */

#ifndef Add2Ptr
#define Add2Ptr(P, I) (void *)((u8 *)(P) + (I))
#define PtrOffset(B, O) ((size_t)((size_t)(O) - (size_t)(B)))
#endif

#define QuadAlign(n) (((n) + 7u) & (~7u))
#define IsQuadAligned(n) (!((size_t)(n)&7u))
#define Quad2Align(n) (((n) + 15u) & (~15u))
#define IsQuad2Aligned(n) (!((size_t)(n)&15u))
#define Quad4Align(n) (((n) + 31u) & (~31u))
#define IsSizeTAligned(n) (!((size_t)(n) & (sizeof(size_t) - 1)))
#define DwordAlign(n) (((n) + 3u) & (~3u))
#define IsDwordAligned(n) (!((size_t)(n)&3u))
#define WordAlign(n) (((n) + 1u) & (~1u))
#define IsWordAligned(n) (!((size_t)(n)&1u))

#ifdef CONFIG_PRINTK
__printf(2, 3) void ntfs_printk(const struct super_block *sb, const char *fmt,
				...);
__printf(2, 3) void ntfs_inode_printk(struct inode *inode, const char *fmt,
				      ...);
#else
static inline __printf(2, 3) void ntfs_printk(const struct super_block *sb,
					      const char *fmt, ...)
{
}

static inline __printf(2, 3) void ntfs_inode_printk(struct inode *inode,
						    const char *fmt, ...)
{
}
#endif

/*
 * Logging macros ( thanks Joe Perches <joe@perches.com> for implementation )
 */

#define ntfs_err(sb, fmt, ...) ntfs_printk(sb, KERN_ERR fmt, ##__VA_ARGS__)
#define ntfs_warn(sb, fmt, ...) ntfs_printk(sb, KERN_WARNING fmt, ##__VA_ARGS__)
#define ntfs_notice(sb, fmt, ...)                                              \
	ntfs_printk(sb, KERN_NOTICE fmt, ##__VA_ARGS__)

#define ntfs_inode_err(inode, fmt, ...)                                        \
	ntfs_inode_printk(inode, KERN_ERR fmt, ##__VA_ARGS__)
#define ntfs_inode_warn(inode, fmt, ...)                                       \
	ntfs_inode_printk(inode, KERN_WARNING fmt, ##__VA_ARGS__)

#define ntfs_alloc(s, z) kmalloc(s, z ? (GFP_NOFS | __GFP_ZERO) : GFP_NOFS)
#define ntfs_free(p) kfree(p)
#define ntfs_memdup(src, len) kmemdup(src, len, GFP_NOFS)
