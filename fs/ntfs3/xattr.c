// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/ntfs3/xattr.c
 *
 * Copyright (C) 2019-2020 Paragon Software GmbH, All rights reserved.
 *
 */

#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/nls.h>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <linux/xattr.h>

#include "debug.h"
#include "ntfs.h"
#include "ntfs_fs.h"

#define SYSTEM_DOS_ATTRIB "system.dos_attrib"
#define SYSTEM_NTFS_ATTRIB "system.ntfs_attrib"
#define SYSTEM_NTFS_SECURITY "system.ntfs_security"
#define USER_DOSATTRIB "user.DOSATTRIB"

static inline size_t unpacked_ea_size(const struct EA_FULL *ea)
{
	return !ea->size ? DwordAlign(offsetof(struct EA_FULL, name) + 1 +
				      ea->name_len + le16_to_cpu(ea->elength)) :
			   le32_to_cpu(ea->size);
}

static inline size_t packed_ea_size(const struct EA_FULL *ea)
{
	return offsetof(struct EA_FULL, name) + 1 -
	       offsetof(struct EA_FULL, flags) + ea->name_len +
	       le16_to_cpu(ea->elength);
}

/*
 * find_ea
 *
 * assume there is at least one xattr in the list
 */
static inline bool find_ea(const struct EA_FULL *ea_all, u32 bytes,
			   const char *name, u8 name_len, u32 *off)
{
	*off = 0;

	if (!ea_all || !bytes)
		return false;

	for (;;) {
		const struct EA_FULL *ea = Add2Ptr(ea_all, *off);
		u32 next_off = *off + unpacked_ea_size(ea);

		if (next_off > bytes)
			return false;

		if (ea->name_len == name_len &&
		    !memcmp(ea->name, name, name_len))
			return true;

		*off = next_off;
		if (next_off >= bytes)
			return false;
	}
}

/*
 * ntfs_read_ea
 *
 * reads all extended attributes
 * ea - new allocated memory
 * info - pointer into resident data
 */
static int ntfs_read_ea(struct ntfs_inode *ni, struct EA_FULL **ea,
			size_t add_bytes, const struct EA_INFO **info)
{
	int err;
	struct ATTR_LIST_ENTRY *le = NULL;
	struct ATTRIB *attr_info, *attr_ea;
	void *ea_p;
	u32 size;

	static_assert(le32_to_cpu(ATTR_EA_INFO) < le32_to_cpu(ATTR_EA));

	*ea = NULL;
	*info = NULL;

	attr_info =
		ni_find_attr(ni, NULL, &le, ATTR_EA_INFO, NULL, 0, NULL, NULL);
	attr_ea =
		ni_find_attr(ni, attr_info, &le, ATTR_EA, NULL, 0, NULL, NULL);

	if (!attr_ea || !attr_info)
		return 0;

	*info = resident_data_ex(attr_info, sizeof(struct EA_INFO));
	if (!*info)
		return -EINVAL;

	/* Check Ea limit */
	size = le32_to_cpu((*info)->size);
	if (size > MAX_EA_DATA_SIZE || size + add_bytes > MAX_EA_DATA_SIZE)
		return -EINVAL;

	/* Allocate memory for packed Ea */
	ea_p = ntfs_alloc(size + add_bytes, 0);
	if (!ea_p)
		return -ENOMEM;

	if (attr_ea->non_res) {
		struct runs_tree run;

		run_init(&run);

		err = attr_load_runs(attr_ea, ni, &run);
		if (!err)
			err = ntfs_read_run_nb(ni->mi.sbi, &run, 0, ea_p, size,
					       NULL);
		run_close(&run);

		if (err)
			goto out;
	} else {
		void *p = resident_data_ex(attr_ea, size);

		if (!p) {
			err = -EINVAL;
			goto out;
		}
		memcpy(ea_p, p, size);
	}

	memset(Add2Ptr(ea_p, size), 0, add_bytes);
	*ea = ea_p;
	return 0;

out:
	ntfs_free(ea_p);
	*ea = NULL;
	return err;
}

/*
 * ntfs_listxattr_hlp
 *
 * copy a list of xattrs names into the buffer
 * provided, or compute the buffer size required
 */
static int ntfs_listxattr_hlp(struct ntfs_inode *ni, char *buffer,
			      size_t bytes_per_buffer, size_t *bytes)
{
	const struct EA_INFO *info;
	struct EA_FULL *ea_all = NULL;
	const struct EA_FULL *ea;
	u32 off, size;
	int err;

	*bytes = 0;

	err = ntfs_read_ea(ni, &ea_all, 0, &info);
	if (err)
		return err;

	if (!info || !ea_all)
		return 0;

	size = le32_to_cpu(info->size);

	/* Enumerate all xattrs */
	for (off = 0; off < size; off += unpacked_ea_size(ea)) {
		ea = Add2Ptr(ea_all, off);

		if (buffer) {
			if (*bytes + ea->name_len + 1 > bytes_per_buffer) {
				err = -ERANGE;
				goto out;
			}

			memcpy(buffer + *bytes, ea->name, ea->name_len);
			buffer[*bytes + ea->name_len] = 0;
		}

		*bytes += ea->name_len + 1;
	}

out:
	ntfs_free(ea_all);
	return err;
}

/*
 * ntfs_get_ea
 *
 * reads xattr
 */
static int ntfs_get_ea(struct ntfs_inode *ni, const char *name, size_t name_len,
		       void *buffer, size_t bytes_per_buffer, u32 *len)
{
	const struct EA_INFO *info;
	struct EA_FULL *ea_all = NULL;
	const struct EA_FULL *ea;
	u32 off;
	int err;

	*len = 0;

	if (name_len > 255) {
		err = -ENAMETOOLONG;
		goto out;
	}

	err = ntfs_read_ea(ni, &ea_all, 0, &info);
	if (err)
		goto out;

	if (!info)
		goto out;

	/* Enumerate all xattrs */
	if (!find_ea(ea_all, le32_to_cpu(info->size), name, name_len, &off)) {
		err = -ENODATA;
		goto out;
	}
	ea = Add2Ptr(ea_all, off);

	*len = le16_to_cpu(ea->elength);
	if (!buffer) {
		err = 0;
		goto out;
	}

	if (*len > bytes_per_buffer) {
		err = -ERANGE;
		goto out;
	}
	memcpy(buffer, ea->name + ea->name_len + 1, *len);
	err = 0;

out:
	ntfs_free(ea_all);

	return err;
}

static noinline int ntfs_getxattr_hlp(struct inode *inode, const char *name,
				      void *value, size_t size,
				      size_t *required)
{
	struct ntfs_inode *ni = ntfs_i(inode);
	int err;
	u32 len;

	if (!(ni->ni_flags & NI_FLAG_EA))
		return -ENODATA;

	if (!required)
		ni_lock(ni);

	err = ntfs_get_ea(ni, name, strlen(name), value, size, &len);
	if (!err)
		err = len;
	else if (-ERANGE == err && required)
		*required = len;

	if (!required)
		ni_unlock(ni);

	return err;
}

static noinline int ntfs_set_ea(struct inode *inode, const char *name,
				const void *value, size_t val_size, int flags,
				int locked)
{
	struct ntfs_inode *ni = ntfs_i(inode);
	struct ntfs_sb_info *sbi = ni->mi.sbi;
	int err;
	struct EA_INFO ea_info;
	const struct EA_INFO *info;
	struct EA_FULL *new_ea;
	struct EA_FULL *ea_all = NULL;
	size_t name_len, add;
	u32 off, size;
	__le16 size_pack;
	struct ATTRIB *attr;
	struct ATTR_LIST_ENTRY *le;
	struct mft_inode *mi;
	struct runs_tree ea_run;
	u64 new_sz;
	void *p;

	if (!locked)
		ni_lock(ni);

	run_init(&ea_run);
	name_len = strlen(name);

	if (name_len > 255) {
		err = -ENAMETOOLONG;
		goto out;
	}

	add = DwordAlign(offsetof(struct EA_FULL, name) + 1 + name_len +
			 val_size);

	err = ntfs_read_ea(ni, &ea_all, add, &info);
	if (err)
		goto out;

	if (!info) {
		memset(&ea_info, 0, sizeof(ea_info));
		size = 0;
		size_pack = 0;
	} else {
		memcpy(&ea_info, info, sizeof(ea_info));
		size = le32_to_cpu(ea_info.size);
		size_pack = ea_info.size_pack;
	}

	if (info && find_ea(ea_all, size, name, name_len, &off)) {
		struct EA_FULL *ea;
		size_t ea_sz;

		if (flags & XATTR_CREATE) {
			err = -EEXIST;
			goto out;
		}

		/* Remove current xattr */
		ea = Add2Ptr(ea_all, off);
		if (ea->flags & FILE_NEED_EA)
			le16_add_cpu(&ea_info.count, -1);

		ea_sz = unpacked_ea_size(ea);

		le16_add_cpu(&ea_info.size_pack, 0 - packed_ea_size(ea));

		memmove(ea, Add2Ptr(ea, ea_sz), size - off - ea_sz);

		size -= ea_sz;
		memset(Add2Ptr(ea_all, size), 0, ea_sz);

		ea_info.size = cpu_to_le32(size);

		if ((flags & XATTR_REPLACE) && !val_size)
			goto update_ea;
	} else {
		if (flags & XATTR_REPLACE) {
			err = -ENODATA;
			goto out;
		}

		if (!ea_all) {
			ea_all = ntfs_alloc(add, 1);
			if (!ea_all) {
				err = -ENOMEM;
				goto out;
			}
		}
	}

	/* append new xattr */
	new_ea = Add2Ptr(ea_all, size);
	new_ea->size = cpu_to_le32(add);
	new_ea->flags = 0;
	new_ea->name_len = name_len;
	new_ea->elength = cpu_to_le16(val_size);
	memcpy(new_ea->name, name, name_len);
	new_ea->name[name_len] = 0;
	memcpy(new_ea->name + name_len + 1, value, val_size);

	le16_add_cpu(&ea_info.size_pack, packed_ea_size(new_ea));
	size += add;
	ea_info.size = cpu_to_le32(size);

update_ea:

	if (!info) {
		/* Create xattr */
		if (!size) {
			err = 0;
			goto out;
		}

		err = ni_insert_resident(ni, sizeof(struct EA_INFO),
					 ATTR_EA_INFO, NULL, 0, NULL, NULL);
		if (err)
			goto out;

		err = ni_insert_resident(ni, 0, ATTR_EA, NULL, 0, NULL, NULL);
		if (err)
			goto out;
	}

	new_sz = size;
	err = attr_set_size(ni, ATTR_EA, NULL, 0, &ea_run, new_sz, &new_sz,
			    false, NULL);
	if (err)
		goto out;

	le = NULL;
	attr = ni_find_attr(ni, NULL, &le, ATTR_EA_INFO, NULL, 0, NULL, &mi);
	if (!attr) {
		err = -EINVAL;
		goto out;
	}

	if (!size) {
		/* delete xattr, ATTR_EA_INFO */
		err = ni_remove_attr_le(ni, attr, le);
		if (err)
			goto out;
	} else {
		p = resident_data_ex(attr, sizeof(struct EA_INFO));
		if (!p) {
			err = -EINVAL;
			goto out;
		}
		memcpy(p, &ea_info, sizeof(struct EA_INFO));
		mi->dirty = true;
	}

	le = NULL;
	attr = ni_find_attr(ni, NULL, &le, ATTR_EA, NULL, 0, NULL, &mi);
	if (!attr) {
		err = -EINVAL;
		goto out;
	}

	if (!size) {
		/* delete xattr, ATTR_EA */
		err = ni_remove_attr_le(ni, attr, le);
		if (err)
			goto out;
	} else if (attr->non_res) {
		err = ntfs_sb_write_run(sbi, &ea_run, 0, ea_all, size);
		if (err)
			goto out;
	} else {
		p = resident_data_ex(attr, size);
		if (!p) {
			err = -EINVAL;
			goto out;
		}
		memcpy(p, ea_all, size);
		mi->dirty = true;
	}

	if (ea_info.size_pack != size_pack)
		ni->ni_flags |= NI_FLAG_UPDATE_PARENT;
	mark_inode_dirty(&ni->vfs_inode);

	/* Check if we delete the last xattr */
	if (val_size || flags != XATTR_REPLACE ||
	    ntfs_listxattr_hlp(ni, NULL, 0, &val_size) || val_size) {
		ni->ni_flags |= NI_FLAG_EA;
	} else {
		ni->ni_flags &= ~NI_FLAG_EA;
	}

out:
	if (!locked)
		ni_unlock(ni);

	run_close(&ea_run);
	ntfs_free(ea_all);

	return err;
}

static inline void ntfs_posix_acl_release(struct posix_acl *acl)
{
	if (acl && refcount_dec_and_test(&acl->a_refcount))
		kfree(acl);
}

static struct posix_acl *ntfs_get_acl_ex(struct inode *inode, int type,
					 int locked)
{
	struct ntfs_inode *ni = ntfs_i(inode);
	const char *name;
	struct posix_acl *acl;
	size_t req;
	int err;
	void *buf;

	buf = __getname();
	if (!buf)
		return ERR_PTR(-ENOMEM);

	/* Possible values of 'type' was already checked above */
	name = type == ACL_TYPE_ACCESS ? XATTR_NAME_POSIX_ACL_ACCESS :
					 XATTR_NAME_POSIX_ACL_DEFAULT;

	if (!locked)
		ni_lock(ni);

	err = ntfs_getxattr_hlp(inode, name, buf, PATH_MAX, &req);

	if (!locked)
		ni_unlock(ni);

	/* Translate extended attribute to acl */
	if (err > 0) {
		acl = posix_acl_from_xattr(&init_user_ns, buf, err);
		if (!IS_ERR(acl))
			set_cached_acl(inode, type, acl);
	} else {
		acl = err == -ENODATA ? NULL : ERR_PTR(err);
	}

	__putname(buf);

	return acl;
}

/*
 * ntfs_get_acl
 *
 * inode_operations::get_acl
 */
struct posix_acl *ntfs_get_acl(struct inode *inode, int type)
{
	return ntfs_get_acl_ex(inode, type, 0);
}

static noinline int ntfs_set_acl_ex(struct inode *inode, struct posix_acl *acl,
				    int type, int locked)
{
	const char *name;
	size_t size;
	void *value = NULL;
	int err = 0;

	if (S_ISLNK(inode->i_mode))
		return -EOPNOTSUPP;

	switch (type) {
	case ACL_TYPE_ACCESS:
		if (acl) {
			umode_t mode = inode->i_mode;

			err = posix_acl_equiv_mode(acl, &mode);
			if (err < 0)
				return err;

			if (inode->i_mode != mode) {
				inode->i_mode = mode;
				mark_inode_dirty(inode);
			}

			if (!err) {
				/*
				 * acl can be exactly represented in the
				 * traditional file mode permission bits
				 */
				acl = NULL;
				goto out;
			}
		}
		name = XATTR_NAME_POSIX_ACL_ACCESS;
		break;

	case ACL_TYPE_DEFAULT:
		if (!S_ISDIR(inode->i_mode))
			return acl ? -EACCES : 0;
		name = XATTR_NAME_POSIX_ACL_DEFAULT;
		break;

	default:
		return -EINVAL;
	}

	if (!acl)
		goto out;

	size = posix_acl_xattr_size(acl->a_count);
	value = ntfs_alloc(size, 0);
	if (!value)
		return -ENOMEM;

	err = posix_acl_to_xattr(&init_user_ns, acl, value, size);
	if (err)
		goto out;

	err = ntfs_set_ea(inode, name, value, size, 0, locked);
	if (err)
		goto out;

out:
	if (!err)
		set_cached_acl(inode, type, acl);

	kfree(value);

	return err;
}

/*
 * ntfs_set_acl
 *
 * inode_operations::set_acl
 */
int ntfs_set_acl(struct inode *inode, struct posix_acl *acl, int type)
{
	return ntfs_set_acl_ex(inode, acl, type, 0);
}

static int ntfs_xattr_get_acl(struct inode *inode, int type, void *buffer,
			      size_t size)
{
	struct super_block *sb = inode->i_sb;
	struct posix_acl *acl;
	int err;

	if (!(sb->s_flags & SB_POSIXACL))
		return -EOPNOTSUPP;

	acl = ntfs_get_acl(inode, type);
	if (IS_ERR(acl))
		return PTR_ERR(acl);

	if (!acl)
		return -ENODATA;

	err = posix_acl_to_xattr(&init_user_ns, acl, buffer, size);
	ntfs_posix_acl_release(acl);

	return err;
}

static int ntfs_xattr_set_acl(struct inode *inode, int type, const void *value,
			      size_t size)
{
	struct super_block *sb = inode->i_sb;
	struct posix_acl *acl;
	int err;

	if (!(sb->s_flags & SB_POSIXACL))
		return -EOPNOTSUPP;

	if (!inode_owner_or_capable(inode))
		return -EPERM;

	if (!value)
		return 0;

	acl = posix_acl_from_xattr(&init_user_ns, value, size);
	if (IS_ERR(acl))
		return PTR_ERR(acl);

	if (acl) {
		err = posix_acl_valid(sb->s_user_ns, acl);
		if (err)
			goto release_and_out;
	}

	err = ntfs_set_acl(inode, acl, type);

release_and_out:
	ntfs_posix_acl_release(acl);
	return err;
}

/*
 * ntfs_acl_chmod
 *
 * helper for 'ntfs_setattr'
 */
int ntfs_acl_chmod(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	int err;

	if (!(sb->s_flags & SB_POSIXACL))
		return 0;

	if (S_ISLNK(inode->i_mode))
		return -EOPNOTSUPP;

	err = posix_acl_chmod(inode, inode->i_mode);

	return err;
}

/*
 * ntfs_permission
 *
 * inode_operations::permission
 */
int ntfs_permission(struct inode *inode, int mask)
{
	struct super_block *sb = inode->i_sb;
	struct ntfs_sb_info *sbi = sb->s_fs_info;
	int err;

	if (sbi->options.no_acs_rules) {
		/* "no access rules" mode - allow all changes */
		return 0;
	}

	err = generic_permission(inode, mask);

	return err;
}

/*
 * ntfs_listxattr
 *
 * inode_operations::listxattr
 */
ssize_t ntfs_listxattr(struct dentry *dentry, char *buffer, size_t size)
{
	struct inode *inode = d_inode(dentry);
	struct ntfs_inode *ni = ntfs_i(inode);
	ssize_t ret = -1;
	int err;

	if (!(ni->ni_flags & NI_FLAG_EA)) {
		ret = 0;
		goto out;
	}

	ni_lock(ni);

	err = ntfs_listxattr_hlp(ni, buffer, size, (size_t *)&ret);

	ni_unlock(ni);

	if (err)
		ret = err;
out:

	return ret;
}

static int ntfs_getxattr(const struct xattr_handler *handler, struct dentry *de,
			 struct inode *inode, const char *name, void *buffer,
			 size_t size)
{
	int err;
	struct ntfs_inode *ni = ntfs_i(inode);
	size_t name_len = strlen(name);

	/* Dispatch request */
	if (name_len == sizeof(SYSTEM_DOS_ATTRIB) - 1 &&
	    !memcmp(name, SYSTEM_DOS_ATTRIB, sizeof(SYSTEM_DOS_ATTRIB))) {
		/* system.dos_attrib */
		if (!buffer) {
			err = sizeof(u8);
		} else if (size < sizeof(u8)) {
			err = -ENODATA;
		} else {
			err = sizeof(u8);
			*(u8 *)buffer = le32_to_cpu(ni->std_fa);
		}
		goto out;
	}

	if (name_len == sizeof(SYSTEM_NTFS_ATTRIB) - 1 &&
	    !memcmp(name, SYSTEM_NTFS_ATTRIB, sizeof(SYSTEM_NTFS_ATTRIB))) {
		/* system.ntfs_attrib */
		if (!buffer) {
			err = sizeof(u32);
		} else if (size < sizeof(u32)) {
			err = -ENODATA;
		} else {
			err = sizeof(u32);
			*(u32 *)buffer = le32_to_cpu(ni->std_fa);
		}
		goto out;
	}

	if (name_len == sizeof(USER_DOSATTRIB) - 1 &&
	    !memcmp(name, USER_DOSATTRIB, sizeof(USER_DOSATTRIB))) {
		/* user.DOSATTRIB */
		if (!buffer) {
			err = 5;
		} else if (size < 5) {
			err = -ENODATA;
		} else {
			err = sprintf((char *)buffer, "0x%x",
				      le32_to_cpu(ni->std_fa) & 0xff) +
			      1;
		}
		goto out;
	}

	if (name_len == sizeof(SYSTEM_NTFS_SECURITY) - 1 &&
	    !memcmp(name, SYSTEM_NTFS_SECURITY, sizeof(SYSTEM_NTFS_SECURITY))) {
		/* system.ntfs_security*/
		void *sd = NULL;
		size_t sd_size = 0;

		if (!is_ntfs3(ni->mi.sbi)) {
			/* we should get nt4 security */
			err = -EINVAL;
			goto out;
		} else if (le32_to_cpu(ni->std_security_id) <
			   SECURITY_ID_FIRST) {
			err = -ENOENT;
			goto out;
		}

		err = ntfs_get_security_by_id(ni->mi.sbi, ni->std_security_id,
					      &sd, &sd_size);
		if (err)
			goto out;

		if (!buffer) {
			err = sd_size;
		} else if (size < sd_size) {
			err = -ENODATA;
		} else {
			err = sd_size;
			memcpy(buffer, sd, sd_size);
		}
		ntfs_free(sd);
		goto out;
	}

	if ((name_len == sizeof(XATTR_NAME_POSIX_ACL_ACCESS) - 1 &&
	     !memcmp(name, XATTR_NAME_POSIX_ACL_ACCESS,
		     sizeof(XATTR_NAME_POSIX_ACL_ACCESS))) ||
	    (name_len == sizeof(XATTR_NAME_POSIX_ACL_DEFAULT) - 1 &&
	     !memcmp(name, XATTR_NAME_POSIX_ACL_DEFAULT,
		     sizeof(XATTR_NAME_POSIX_ACL_DEFAULT)))) {
		err = ntfs_xattr_get_acl(
			inode,
			name_len == sizeof(XATTR_NAME_POSIX_ACL_ACCESS) - 1 ?
				ACL_TYPE_ACCESS :
				ACL_TYPE_DEFAULT,
			buffer, size);
	} else {
		err = ntfs_getxattr_hlp(inode, name, buffer, size, NULL);
	}

out:
	return err;
}

/*
 * ntfs_setxattr
 *
 * inode_operations::setxattr
 */
static noinline int ntfs_setxattr(const struct xattr_handler *handler,
				  struct dentry *de, struct inode *inode,
				  const char *name, const void *value,
				  size_t size, int flags)
{
	int err = -EINVAL;
	struct ntfs_inode *ni = ntfs_i(inode);
	size_t name_len = strlen(name);
	u32 attrib = 0; /* not necessary just to suppress warnings */
	__le32 new_fa;

	/* Dispatch request */
	if (name_len == sizeof(SYSTEM_DOS_ATTRIB) - 1 &&
	    !memcmp(name, SYSTEM_DOS_ATTRIB, sizeof(SYSTEM_DOS_ATTRIB))) {
		if (sizeof(u8) != size)
			goto out;
		attrib = *(u8 *)value;
		goto set_dos_attr;
	}

	if (name_len == sizeof(SYSTEM_NTFS_ATTRIB) - 1 &&
	    !memcmp(name, SYSTEM_NTFS_ATTRIB, sizeof(SYSTEM_NTFS_ATTRIB))) {
		if (sizeof(u32) != size)
			goto out;
		attrib = *(u32 *)value;
		goto set_dos_attr;
	}

	if (name_len == sizeof(USER_DOSATTRIB) - 1 &&
	    !memcmp(name, USER_DOSATTRIB, sizeof(USER_DOSATTRIB))) {
		if (size < 4 || ((char *)value)[size - 1])
			goto out;

		/*
		 * The input value must be string in form 0x%x with last zero
		 * This means that the 'size' must be 4, 5, ...
		 *  E.g: 0x1 - 4 bytes, 0x20 - 5 bytes
		 */
		if (sscanf((char *)value, "0x%x", &attrib) != 1)
			goto out;

set_dos_attr:
		if (!value)
			goto out;

		/*
		 * Thanks Mark Harmstone:
		 * keep directory bit consistency
		 */
		new_fa = cpu_to_le32(attrib);
		if (S_ISDIR(inode->i_mode))
			new_fa |= FILE_ATTRIBUTE_DIRECTORY;
		else
			new_fa &= ~FILE_ATTRIBUTE_DIRECTORY;

		if (ni->std_fa != new_fa) {
			ni->std_fa = new_fa;
			/* std attribute always in primary record */
			ni->mi.dirty = true;
			mark_inode_dirty(inode);
		}
		err = 0;

		goto out;
	}

	if (name_len == sizeof(SYSTEM_NTFS_SECURITY) - 1 &&
	    !memcmp(name, SYSTEM_NTFS_SECURITY, sizeof(SYSTEM_NTFS_SECURITY))) {
		/* system.ntfs_security*/
		__le32 security_id;
		bool inserted;
		struct ATTR_STD_INFO5 *std;

		if (!is_ntfs3(ni->mi.sbi)) {
			/*
			 * we should replace ATTR_SECURE
			 * Skip this way cause it is nt4 feature
			 */
			err = -EINVAL;
			goto out;
		}

		err = ntfs_insert_security(ni->mi.sbi, value, size,
					   &security_id, &inserted);
		if (err)
			goto out;

		ni_lock(ni);
		std = ni_std5(ni);
		if (!std) {
			err = -EINVAL;
		} else if (std->security_id != security_id) {
			std->security_id = ni->std_security_id = security_id;
			/* std attribute always in primary record */
			ni->mi.dirty = true;
			mark_inode_dirty(&ni->vfs_inode);
		}
		ni_unlock(ni);
		goto out;
	}

	if ((name_len == sizeof(XATTR_NAME_POSIX_ACL_ACCESS) - 1 &&
	     !memcmp(name, XATTR_NAME_POSIX_ACL_ACCESS,
		     sizeof(XATTR_NAME_POSIX_ACL_ACCESS))) ||
	    (name_len == sizeof(XATTR_NAME_POSIX_ACL_DEFAULT) - 1 &&
	     !memcmp(name, XATTR_NAME_POSIX_ACL_DEFAULT,
		     sizeof(XATTR_NAME_POSIX_ACL_DEFAULT)))) {
		err = ntfs_xattr_set_acl(
			inode,
			name_len == sizeof(XATTR_NAME_POSIX_ACL_ACCESS) - 1 ?
				ACL_TYPE_ACCESS :
				ACL_TYPE_DEFAULT,
			value, size);
	} else {
		err = ntfs_set_ea(inode, name, value, size, flags, 0);
	}

out:
	return err;
}

/*
 * Initialize the ACLs of a new inode. Called from ntfs_create_inode.
 */
int ntfs_init_acl(struct inode *inode, struct inode *dir)
{
	struct posix_acl *default_acl, *acl;
	int err;

	/*
	 * TODO refactoring lock
	 * ni_lock(dir) ... -> posix_acl_create(dir,...) -> ntfs_get_acl -> ni_lock(dir)
	 */
	inode->i_default_acl = NULL;

	default_acl = ntfs_get_acl_ex(dir, ACL_TYPE_DEFAULT, 1);

	if (!default_acl || default_acl == ERR_PTR(-EOPNOTSUPP)) {
		inode->i_mode &= ~current_umask();
		err = 0;
		goto out;
	}

	if (IS_ERR(default_acl)) {
		err = PTR_ERR(default_acl);
		goto out;
	}

	acl = default_acl;
	err = __posix_acl_create(&acl, GFP_NOFS, &inode->i_mode);
	if (err < 0)
		goto out1;
	if (!err) {
		posix_acl_release(acl);
		acl = NULL;
	}

	if (!S_ISDIR(inode->i_mode)) {
		posix_acl_release(default_acl);
		default_acl = NULL;
	}

	if (default_acl)
		err = ntfs_set_acl_ex(inode, default_acl, ACL_TYPE_DEFAULT, 1);

	if (!acl)
		inode->i_acl = NULL;
	else if (!err)
		err = ntfs_set_acl_ex(inode, acl, ACL_TYPE_ACCESS, 1);

	posix_acl_release(acl);
out1:
	posix_acl_release(default_acl);

out:
	return err;
}

static bool ntfs_xattr_user_list(struct dentry *dentry)
{
	return 1;
}

static const struct xattr_handler ntfs_xattr_handler = {
	.prefix = "",
	.get = ntfs_getxattr,
	.set = ntfs_setxattr,
	.list = ntfs_xattr_user_list,
};

const struct xattr_handler *ntfs_xattr_handlers[] = { &ntfs_xattr_handler,
						      NULL };
