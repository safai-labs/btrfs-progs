/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include "kerncompat.h"
#include "androidcompat.h"

#include <sys/ioctl.h>
#include <sys/mount.h>
#include "ioctl.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
/* #include <sys/dir.h> included via androidcompat.h */
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <uuid/uuid.h>
#include <ctype.h>
#include <sys/xattr.h>
#include <limits.h>
#include <linux/limits.h>
#include <blkid/blkid.h>
#include <ftw.h>
#include "ctree.h"
#include "disk-io.h"
#include "volumes.h"
#include "transaction.h"
#include "utils.h"
#include "list_sort.h"
#include "help.h"
#include "mkfs/common.h"
#include "fsfeatures.h"

static u64 index_cnt = 2;
static int verbose = 1;

struct directory_name_entry {
	const char *dir_name;
	const char *path;
	ino_t inum;
	struct list_head list;
};

struct mkfs_allocation {
	u64 data;
	u64 metadata;
	u64 mixed;
	u64 system;
};

static int create_metadata_block_groups(struct btrfs_root *root, int mixed,
				struct mkfs_allocation *allocation)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_trans_handle *trans;
	u64 bytes_used;
	u64 chunk_start = 0;
	u64 chunk_size = 0;
	int ret;

	trans = btrfs_start_transaction(root, 1);
	BUG_ON(IS_ERR(trans));
	bytes_used = btrfs_super_bytes_used(fs_info->super_copy);

	root->fs_info->system_allocs = 1;
	ret = btrfs_make_block_group(trans, fs_info, bytes_used,
				     BTRFS_BLOCK_GROUP_SYSTEM,
				     BTRFS_FIRST_CHUNK_TREE_OBJECTID,
				     0, BTRFS_MKFS_SYSTEM_GROUP_SIZE);
	allocation->system += BTRFS_MKFS_SYSTEM_GROUP_SIZE;
	if (ret)
		return ret;

	if (mixed) {
		ret = btrfs_alloc_chunk(trans, fs_info,
					&chunk_start, &chunk_size,
					BTRFS_BLOCK_GROUP_METADATA |
					BTRFS_BLOCK_GROUP_DATA);
		if (ret == -ENOSPC) {
			error("no space to allocate data/metadata chunk");
			goto err;
		}
		if (ret)
			return ret;
		ret = btrfs_make_block_group(trans, fs_info, 0,
					     BTRFS_BLOCK_GROUP_METADATA |
					     BTRFS_BLOCK_GROUP_DATA,
					     BTRFS_FIRST_CHUNK_TREE_OBJECTID,
					     chunk_start, chunk_size);
		if (ret)
			return ret;
		allocation->mixed += chunk_size;
	} else {
		ret = btrfs_alloc_chunk(trans, fs_info,
					&chunk_start, &chunk_size,
					BTRFS_BLOCK_GROUP_METADATA);
		if (ret == -ENOSPC) {
			error("no space to allocate metadata chunk");
			goto err;
		}
		if (ret)
			return ret;
		ret = btrfs_make_block_group(trans, fs_info, 0,
					     BTRFS_BLOCK_GROUP_METADATA,
					     BTRFS_FIRST_CHUNK_TREE_OBJECTID,
					     chunk_start, chunk_size);
		allocation->metadata += chunk_size;
		if (ret)
			return ret;
	}

	root->fs_info->system_allocs = 0;
	ret = btrfs_commit_transaction(trans, root);

err:
	return ret;
}

static int create_data_block_groups(struct btrfs_trans_handle *trans,
		struct btrfs_root *root, int mixed,
		struct mkfs_allocation *allocation)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	u64 chunk_start = 0;
	u64 chunk_size = 0;
	int ret = 0;

	if (!mixed) {
		ret = btrfs_alloc_chunk(trans, fs_info,
					&chunk_start, &chunk_size,
					BTRFS_BLOCK_GROUP_DATA);
		if (ret == -ENOSPC) {
			error("no space to allocate data chunk");
			goto err;
		}
		if (ret)
			return ret;
		ret = btrfs_make_block_group(trans, fs_info, 0,
					     BTRFS_BLOCK_GROUP_DATA,
					     BTRFS_FIRST_CHUNK_TREE_OBJECTID,
					     chunk_start, chunk_size);
		allocation->data += chunk_size;
		if (ret)
			return ret;
	}

err:
	return ret;
}

static int make_root_dir(struct btrfs_trans_handle *trans,
		struct btrfs_root *root)
{
	struct btrfs_key location;
	int ret;

	ret = btrfs_make_root_dir(trans, root->fs_info->tree_root,
			      BTRFS_ROOT_TREE_DIR_OBJECTID);
	if (ret)
		goto err;
	ret = btrfs_make_root_dir(trans, root, BTRFS_FIRST_FREE_OBJECTID);
	if (ret)
		goto err;
	memcpy(&location, &root->fs_info->fs_root->root_key, sizeof(location));
	location.offset = (u64)-1;
	ret = btrfs_insert_dir_item(trans, root->fs_info->tree_root,
			"default", 7,
			btrfs_super_root_dir(root->fs_info->super_copy),
			&location, BTRFS_FT_DIR, 0);
	if (ret)
		goto err;

	ret = btrfs_insert_inode_ref(trans, root->fs_info->tree_root,
			     "default", 7, location.objectid,
			     BTRFS_ROOT_TREE_DIR_OBJECTID, 0);
	if (ret)
		goto err;

err:
	return ret;
}

static int __recow_root(struct btrfs_trans_handle *trans,
			 struct btrfs_root *root)
{
	struct extent_buffer *tmp;
	int ret;

	if (trans->transid != btrfs_root_generation(&root->root_item)) {
		extent_buffer_get(root->node);
		ret = __btrfs_cow_block(trans, root, root->node,
					NULL, 0, &tmp, 0, 0);
		if (ret)
			return ret;
		free_extent_buffer(tmp);
	}

	return 0;
}

static int recow_roots(struct btrfs_trans_handle *trans,
		       struct btrfs_root *root)
{
	struct btrfs_fs_info *info = root->fs_info;
	int ret;

	ret = __recow_root(trans, info->fs_root);
	if (ret)
		return ret;
	ret = __recow_root(trans, info->tree_root);
	if (ret)
		return ret;
	ret = __recow_root(trans, info->extent_root);
	if (ret)
		return ret;
	ret = __recow_root(trans, info->chunk_root);
	if (ret)
		return ret;
	ret = __recow_root(trans, info->dev_root);
	if (ret)
		return ret;
	ret = __recow_root(trans, info->csum_root);
	if (ret)
		return ret;

	return 0;
}

static int create_one_raid_group(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root, u64 type,
			      struct mkfs_allocation *allocation)

{
	struct btrfs_fs_info *fs_info = root->fs_info;
	u64 chunk_start;
	u64 chunk_size;
	int ret;

	ret = btrfs_alloc_chunk(trans, fs_info,
				&chunk_start, &chunk_size, type);
	if (ret == -ENOSPC) {
		error("not enough free space to allocate chunk");
		exit(1);
	}
	if (ret)
		return ret;

	ret = btrfs_make_block_group(trans, fs_info, 0,
				     type, BTRFS_FIRST_CHUNK_TREE_OBJECTID,
				     chunk_start, chunk_size);

	type &= BTRFS_BLOCK_GROUP_TYPE_MASK;
	if (type == BTRFS_BLOCK_GROUP_DATA) {
		allocation->data += chunk_size;
	} else if (type == BTRFS_BLOCK_GROUP_METADATA) {
		allocation->metadata += chunk_size;
	} else if (type == BTRFS_BLOCK_GROUP_SYSTEM) {
		allocation->system += chunk_size;
	} else if (type ==
			(BTRFS_BLOCK_GROUP_METADATA | BTRFS_BLOCK_GROUP_DATA)) {
		allocation->mixed += chunk_size;
	} else {
		error("unrecognized profile type: 0x%llx",
				(unsigned long long)type);
		ret = -EINVAL;
	}

	return ret;
}

static int create_raid_groups(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root, u64 data_profile,
			      u64 metadata_profile, int mixed,
			      struct mkfs_allocation *allocation)
{
	int ret;

	if (metadata_profile) {
		u64 meta_flags = BTRFS_BLOCK_GROUP_METADATA;

		ret = create_one_raid_group(trans, root,
					    BTRFS_BLOCK_GROUP_SYSTEM |
					    metadata_profile, allocation);
		if (ret)
			return ret;

		if (mixed)
			meta_flags |= BTRFS_BLOCK_GROUP_DATA;

		ret = create_one_raid_group(trans, root, meta_flags |
					    metadata_profile, allocation);
		if (ret)
			return ret;

	}
	if (!mixed && data_profile) {
		ret = create_one_raid_group(trans, root,
					    BTRFS_BLOCK_GROUP_DATA |
					    data_profile, allocation);
		if (ret)
			return ret;
	}
	ret = recow_roots(trans, root);

	return ret;
}

static int create_tree(struct btrfs_trans_handle *trans,
			struct btrfs_root *root, u64 objectid)
{
	struct btrfs_key location;
	struct btrfs_root_item root_item;
	struct extent_buffer *tmp;
	int ret;

	ret = btrfs_copy_root(trans, root, root->node, &tmp, objectid);
	if (ret)
		return ret;

	memcpy(&root_item, &root->root_item, sizeof(root_item));
	btrfs_set_root_bytenr(&root_item, tmp->start);
	btrfs_set_root_level(&root_item, btrfs_header_level(tmp));
	btrfs_set_root_generation(&root_item, trans->transid);
	free_extent_buffer(tmp);

	location.objectid = objectid;
	location.type = BTRFS_ROOT_ITEM_KEY;
	location.offset = 0;
	ret = btrfs_insert_root(trans, root->fs_info->tree_root,
				&location, &root_item);

	return ret;
}

static void print_usage(int ret)
{
	printf("Usage: mkfs.btrfs [options] dev [ dev ... ]\n");
	printf("Options:\n");
	printf("  allocation profiles:\n");
	printf("\t-d|--data PROFILE       data profile, raid0, raid1, raid5, raid6, raid10, dup or single\n");
	printf("\t-m|--metadata PROFILE   metadata profile, values like for data profile\n");
	printf("\t-M|--mixed              mix metadata and data together\n");
	printf("  features:\n");
	printf("\t-n|--nodesize SIZE      size of btree nodes\n");
	printf("\t-s|--sectorsize SIZE    data block size (may not be mountable by current kernel)\n");
	printf("\t-O|--features LIST      comma separated list of filesystem features (use '-O list-all' to list features)\n");
	printf("\t-L|--label LABEL        set the filesystem label\n");
	printf("\t-U|--uuid UUID          specify the filesystem UUID (must be unique)\n");
	printf("  creation:\n");
	printf("\t-b|--byte-count SIZE    set filesystem size to SIZE (on the first device)\n");
	printf("\t-r|--rootdir DIR        copy files from DIR to the image root directory\n");
	printf("\t-K|--nodiscard          do not perform whole device TRIM\n");
	printf("\t-f|--force              force overwrite of existing filesystem\n");
	printf("  general:\n");
	printf("\t-q|--quiet              no messages except errors\n");
	printf("\t-V|--version            print the mkfs.btrfs version and exit\n");
	printf("\t--help                  print this help and exit\n");
	printf("  deprecated:\n");
	printf("\t-A|--alloc-start START  the offset to start the filesystem\n");
	printf("\t-l|--leafsize SIZE      deprecated, alias for nodesize\n");
	exit(ret);
}

static u64 parse_profile(const char *s)
{
	if (strcasecmp(s, "raid0") == 0) {
		return BTRFS_BLOCK_GROUP_RAID0;
	} else if (strcasecmp(s, "raid1") == 0) {
		return BTRFS_BLOCK_GROUP_RAID1;
	} else if (strcasecmp(s, "raid5") == 0) {
		return BTRFS_BLOCK_GROUP_RAID5;
	} else if (strcasecmp(s, "raid6") == 0) {
		return BTRFS_BLOCK_GROUP_RAID6;
	} else if (strcasecmp(s, "raid10") == 0) {
		return BTRFS_BLOCK_GROUP_RAID10;
	} else if (strcasecmp(s, "dup") == 0) {
		return BTRFS_BLOCK_GROUP_DUP;
	} else if (strcasecmp(s, "single") == 0) {
		return 0;
	} else {
		error("unknown profile %s", s);
		exit(1);
	}
	/* not reached */
	return 0;
}

static char *parse_label(const char *input)
{
	int len = strlen(input);

	if (len >= BTRFS_LABEL_SIZE) {
		error("label %s is too long (max %d)", input,
			BTRFS_LABEL_SIZE - 1);
		exit(1);
	}
	return strdup(input);
}

static int add_directory_items(struct btrfs_trans_handle *trans,
			       struct btrfs_root *root, u64 objectid,
			       ino_t parent_inum, const char *name,
			       struct stat *st, int *dir_index_cnt)
{
	int ret;
	int name_len;
	struct btrfs_key location;
	u8 filetype = 0;

	name_len = strlen(name);

	location.objectid = objectid;
	location.offset = 0;
	location.type = BTRFS_INODE_ITEM_KEY;

	if (S_ISDIR(st->st_mode))
		filetype = BTRFS_FT_DIR;
	if (S_ISREG(st->st_mode))
		filetype = BTRFS_FT_REG_FILE;
	if (S_ISLNK(st->st_mode))
		filetype = BTRFS_FT_SYMLINK;
	if (S_ISSOCK(st->st_mode))
		filetype = BTRFS_FT_SOCK;
	if (S_ISCHR(st->st_mode))
		filetype = BTRFS_FT_CHRDEV;
	if (S_ISBLK(st->st_mode))
		filetype = BTRFS_FT_BLKDEV;
	if (S_ISFIFO(st->st_mode))
		filetype = BTRFS_FT_FIFO;

	ret = btrfs_insert_dir_item(trans, root, name, name_len,
				    parent_inum, &location,
				    filetype, index_cnt);
	if (ret)
		return ret;
	ret = btrfs_insert_inode_ref(trans, root, name, name_len,
				     objectid, parent_inum, index_cnt);
	*dir_index_cnt = index_cnt;
	index_cnt++;

	return ret;
}

static int fill_inode_item(struct btrfs_trans_handle *trans,
			   struct btrfs_root *root,
			   struct btrfs_inode_item *dst, struct stat *src)
{
	u64 blocks = 0;
	u64 sectorsize = root->fs_info->sectorsize;

	/*
	 * btrfs_inode_item has some reserved fields
	 * and represents on-disk inode entry, so
	 * zero everything to prevent information leak
	 */
	memset(dst, 0, sizeof (*dst));

	btrfs_set_stack_inode_generation(dst, trans->transid);
	btrfs_set_stack_inode_size(dst, src->st_size);
	btrfs_set_stack_inode_nbytes(dst, 0);
	btrfs_set_stack_inode_block_group(dst, 0);
	btrfs_set_stack_inode_nlink(dst, src->st_nlink);
	btrfs_set_stack_inode_uid(dst, src->st_uid);
	btrfs_set_stack_inode_gid(dst, src->st_gid);
	btrfs_set_stack_inode_mode(dst, src->st_mode);
	btrfs_set_stack_inode_rdev(dst, 0);
	btrfs_set_stack_inode_flags(dst, 0);
	btrfs_set_stack_timespec_sec(&dst->atime, src->st_atime);
	btrfs_set_stack_timespec_nsec(&dst->atime, 0);
	btrfs_set_stack_timespec_sec(&dst->ctime, src->st_ctime);
	btrfs_set_stack_timespec_nsec(&dst->ctime, 0);
	btrfs_set_stack_timespec_sec(&dst->mtime, src->st_mtime);
	btrfs_set_stack_timespec_nsec(&dst->mtime, 0);
	btrfs_set_stack_timespec_sec(&dst->otime, 0);
	btrfs_set_stack_timespec_nsec(&dst->otime, 0);

	if (S_ISDIR(src->st_mode)) {
		btrfs_set_stack_inode_size(dst, 0);
		btrfs_set_stack_inode_nlink(dst, 1);
	}
	if (S_ISREG(src->st_mode)) {
		btrfs_set_stack_inode_size(dst, (u64)src->st_size);
		if (src->st_size <= BTRFS_MAX_INLINE_DATA_SIZE(root))
			btrfs_set_stack_inode_nbytes(dst, src->st_size);
		else {
			blocks = src->st_size / sectorsize;
			if (src->st_size % sectorsize)
				blocks += 1;
			blocks *= sectorsize;
			btrfs_set_stack_inode_nbytes(dst, blocks);
		}
	}
	if (S_ISLNK(src->st_mode))
		btrfs_set_stack_inode_nbytes(dst, src->st_size + 1);

	return 0;
}

static int directory_select(const struct direct *entry)
{
	if (entry->d_name[0] == '.' &&
		(entry->d_name[1] == 0 ||
		 (entry->d_name[1] == '.' && entry->d_name[2] == 0)))
		return 0;
	return 1;
}

static void free_namelist(struct direct **files, int count)
{
	int i;

	if (count < 0)
		return;

	for (i = 0; i < count; ++i)
		free(files[i]);
	free(files);
}

static u64 calculate_dir_inode_size(const char *dirname)
{
	int count, i;
	struct direct **files, *cur_file;
	u64 dir_inode_size = 0;

	count = scandir(dirname, &files, directory_select, NULL);

	for (i = 0; i < count; i++) {
		cur_file = files[i];
		dir_inode_size += strlen(cur_file->d_name);
	}

	free_namelist(files, count);

	dir_inode_size *= 2;
	return dir_inode_size;
}

static int add_inode_items(struct btrfs_trans_handle *trans,
			   struct btrfs_root *root,
			   struct stat *st, const char *name,
			   u64 self_objectid,
			   struct btrfs_inode_item *inode_ret)
{
	int ret;
	struct btrfs_inode_item btrfs_inode;
	u64 objectid;
	u64 inode_size = 0;

	fill_inode_item(trans, root, &btrfs_inode, st);
	objectid = self_objectid;

	if (S_ISDIR(st->st_mode)) {
		inode_size = calculate_dir_inode_size(name);
		btrfs_set_stack_inode_size(&btrfs_inode, inode_size);
	}

	ret = btrfs_insert_inode(trans, root, objectid, &btrfs_inode);

	*inode_ret = btrfs_inode;
	return ret;
}

static int add_xattr_item(struct btrfs_trans_handle *trans,
			  struct btrfs_root *root, u64 objectid,
			  const char *file_name)
{
	int ret;
	int cur_name_len;
	char xattr_list[XATTR_LIST_MAX];
	char *cur_name;
	char cur_value[XATTR_SIZE_MAX];
	char delimiter = '\0';
	char *next_location = xattr_list;

	ret = llistxattr(file_name, xattr_list, XATTR_LIST_MAX);
	if (ret < 0) {
		if(errno == ENOTSUP)
			return 0;
		error("getting a list of xattr failed for %s: %s", file_name,
				strerror(errno));
		return ret;
	}
	if (ret == 0)
		return ret;

	cur_name = strtok(xattr_list, &delimiter);
	while (cur_name != NULL) {
		cur_name_len = strlen(cur_name);
		next_location += cur_name_len + 1;

		ret = getxattr(file_name, cur_name, cur_value, XATTR_SIZE_MAX);
		if (ret < 0) {
			if(errno == ENOTSUP)
				return 0;
			error("gettig a xattr value failed for %s attr %s: %s",
				file_name, cur_name, strerror(errno));
			return ret;
		}

		ret = btrfs_insert_xattr_item(trans, root, cur_name,
					      cur_name_len, cur_value,
					      ret, objectid);
		if (ret) {
			error("inserting a xattr item failed for %s: %s",
					file_name, strerror(-ret));
		}

		cur_name = strtok(next_location, &delimiter);
	}

	return ret;
}

static int add_symbolic_link(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root,
			     u64 objectid, const char *path_name)
{
	int ret;
	char buf[PATH_MAX];

	ret = readlink(path_name, buf, sizeof(buf));
	if (ret <= 0) {
		error("readlink failed for %s: %s", path_name, strerror(errno));
		goto fail;
	}
	if (ret >= sizeof(buf)) {
		error("symlink too long for %s", path_name);
		ret = -1;
		goto fail;
	}

	buf[ret] = '\0'; /* readlink does not do it for us */
	ret = btrfs_insert_inline_extent(trans, root, objectid, 0,
					 buf, ret + 1);
fail:
	return ret;
}

static int add_file_items(struct btrfs_trans_handle *trans,
			  struct btrfs_root *root,
			  struct btrfs_inode_item *btrfs_inode, u64 objectid,
			  struct stat *st, const char *path_name)
{
	int ret = -1;
	ssize_t ret_read;
	u64 bytes_read = 0;
	struct btrfs_key key;
	int blocks;
	u32 sectorsize = root->fs_info->sectorsize;
	u64 first_block = 0;
	u64 file_pos = 0;
	u64 cur_bytes;
	u64 total_bytes;
	struct extent_buffer *eb = NULL;
	int fd;

	if (st->st_size == 0)
		return 0;

	fd = open(path_name, O_RDONLY);
	if (fd == -1) {
		error("cannot open %s: %s", path_name, strerror(errno));
		return ret;
	}

	blocks = st->st_size / sectorsize;
	if (st->st_size % sectorsize)
		blocks += 1;

	if (st->st_size <= BTRFS_MAX_INLINE_DATA_SIZE(root)) {
		char *buffer = malloc(st->st_size);

		if (!buffer) {
			ret = -ENOMEM;
			goto end;
		}

		ret_read = pread64(fd, buffer, st->st_size, bytes_read);
		if (ret_read == -1) {
			error("cannot read %s at offset %llu length %llu: %s",
				path_name, (unsigned long long)bytes_read,
				(unsigned long long)st->st_size,
				strerror(errno));
			free(buffer);
			goto end;
		}

		ret = btrfs_insert_inline_extent(trans, root, objectid, 0,
						 buffer, st->st_size);
		free(buffer);
		goto end;
	}

	/* round up our st_size to the FS blocksize */
	total_bytes = (u64)blocks * sectorsize;

	/*
	 * do our IO in extent buffers so it can work
	 * against any raid type
	 */
	eb = calloc(1, sizeof(*eb) + sectorsize);
	if (!eb) {
		ret = -ENOMEM;
		goto end;
	}

again:

	/*
	 * keep our extent size at 1MB max, this makes it easier to work inside
	 * the tiny block groups created during mkfs
	 */
	cur_bytes = min(total_bytes, (u64)SZ_1M);
	ret = btrfs_reserve_extent(trans, root, cur_bytes, 0, 0, (u64)-1,
				   &key, 1);
	if (ret)
		goto end;

	first_block = key.objectid;
	bytes_read = 0;

	while (bytes_read < cur_bytes) {

		memset(eb->data, 0, sectorsize);

		ret_read = pread64(fd, eb->data, sectorsize, file_pos + bytes_read);
		if (ret_read == -1) {
			error("cannot read %s at offset %llu length %llu: %s",
				path_name,
				(unsigned long long)file_pos + bytes_read,
				(unsigned long long)sectorsize,
				strerror(errno));
			goto end;
		}

		eb->start = first_block + bytes_read;
		eb->len = sectorsize;

		/*
		 * we're doing the csum before we record the extent, but
		 * that's ok
		 */
		ret = btrfs_csum_file_block(trans, root->fs_info->csum_root,
					    first_block + bytes_read + sectorsize,
					    first_block + bytes_read,
					    eb->data, sectorsize);
		if (ret)
			goto end;

		ret = write_and_map_eb(root->fs_info, eb);
		if (ret) {
			error("failed to write %s", path_name);
			goto end;
		}

		bytes_read += sectorsize;
	}

	if (bytes_read) {
		ret = btrfs_record_file_extent(trans, root, objectid, btrfs_inode,
					       file_pos, first_block, cur_bytes);
		if (ret)
			goto end;

	}

	file_pos += cur_bytes;
	total_bytes -= cur_bytes;

	if (total_bytes)
		goto again;

end:
	free(eb);
	close(fd);
	return ret;
}

static char *make_path(const char *dir, const char *name)
{
	char *path;

	path = malloc(strlen(dir) + strlen(name) + 2);
	if (!path)
		return NULL;
	strcpy(path, dir);
	if (dir[strlen(dir) - 1] != '/')
		strcat(path, "/");
	strcat(path, name);
	return path;
}

static int traverse_directory(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root, const char *dir_name,
			      struct directory_name_entry *dir_head)
{
	int ret = 0;

	struct btrfs_inode_item cur_inode;
	struct btrfs_inode_item *inode_item;
	int count, i, dir_index_cnt;
	struct direct **files;
	struct stat st;
	struct directory_name_entry *dir_entry, *parent_dir_entry;
	struct direct *cur_file;
	ino_t parent_inum, cur_inum;
	ino_t highest_inum = 0;
	const char *parent_dir_name;
	char real_path[PATH_MAX];
	struct btrfs_path path;
	struct extent_buffer *leaf;
	struct btrfs_key root_dir_key;
	u64 root_dir_inode_size = 0;

	/* Add list for source directory */
	dir_entry = malloc(sizeof(struct directory_name_entry));
	if (!dir_entry)
		return -ENOMEM;
	dir_entry->dir_name = dir_name;
	dir_entry->path = realpath(dir_name, real_path);
	if (!dir_entry->path) {
		error("realpath  failed for %s: %s", dir_name, strerror(errno));
		ret = -1;
		goto fail_no_dir;
	}

	parent_inum = highest_inum + BTRFS_FIRST_FREE_OBJECTID;
	dir_entry->inum = parent_inum;
	list_add_tail(&dir_entry->list, &dir_head->list);

	btrfs_init_path(&path);

	root_dir_key.objectid = btrfs_root_dirid(&root->root_item);
	root_dir_key.offset = 0;
	root_dir_key.type = BTRFS_INODE_ITEM_KEY;
	ret = btrfs_lookup_inode(trans, root, &path, &root_dir_key, 1);
	if (ret) {
		error("failed to lookup root dir: %d", ret);
		goto fail_no_dir;
	}

	leaf = path.nodes[0];
	inode_item = btrfs_item_ptr(leaf, path.slots[0],
				    struct btrfs_inode_item);

	root_dir_inode_size = calculate_dir_inode_size(dir_name);
	btrfs_set_inode_size(leaf, inode_item, root_dir_inode_size);
	btrfs_mark_buffer_dirty(leaf);

	btrfs_release_path(&path);

	do {
		parent_dir_entry = list_entry(dir_head->list.next,
					      struct directory_name_entry,
					      list);
		list_del(&parent_dir_entry->list);

		parent_inum = parent_dir_entry->inum;
		parent_dir_name = parent_dir_entry->dir_name;
		if (chdir(parent_dir_entry->path)) {
			error("chdir failed for %s: %s",
				parent_dir_name, strerror(errno));
			ret = -1;
			goto fail_no_files;
		}

		count = scandir(parent_dir_entry->path, &files,
				directory_select, NULL);
		if (count == -1)
		{
			error("scandir failed for %s: %s",
				parent_dir_name, strerror (errno));
			ret = -1;
			goto fail;
		}

		for (i = 0; i < count; i++) {
			cur_file = files[i];

			if (lstat(cur_file->d_name, &st) == -1) {
				error("lstat failed for %s: %s",
					cur_file->d_name, strerror(errno));
				ret = -1;
				goto fail;
			}

			cur_inum = st.st_ino;
			ret = add_directory_items(trans, root,
						  cur_inum, parent_inum,
						  cur_file->d_name,
						  &st, &dir_index_cnt);
			if (ret) {
				error("unable to add directory items for %s: %d",
					cur_file->d_name, ret);
				goto fail;
			}

			ret = add_inode_items(trans, root, &st,
					      cur_file->d_name, cur_inum,
					      &cur_inode);
			if (ret == -EEXIST) {
				if (st.st_nlink <= 1) {
					error(
			"item %s already exists but has wrong st_nlink %lu <= 1",
						cur_file->d_name,
						(unsigned long)st.st_nlink);
					goto fail;
				}
				continue;
			}
			if (ret) {
				error("unable to add inode items for %s: %d",
					cur_file->d_name, ret);
				goto fail;
			}

			ret = add_xattr_item(trans, root,
					     cur_inum, cur_file->d_name);
			if (ret) {
				error("unable to add xattr items for %s: %d",
					cur_file->d_name, ret);
				if(ret != -ENOTSUP)
					goto fail;
			}

			if (S_ISDIR(st.st_mode)) {
				dir_entry = malloc(sizeof(struct directory_name_entry));
				if (!dir_entry) {
					ret = -ENOMEM;
					goto fail;
				}
				dir_entry->dir_name = cur_file->d_name;
				dir_entry->path = make_path(parent_dir_entry->path,
							    cur_file->d_name);
				dir_entry->inum = cur_inum;
				list_add_tail(&dir_entry->list,	&dir_head->list);
			} else if (S_ISREG(st.st_mode)) {
				ret = add_file_items(trans, root, &cur_inode,
						     cur_inum, &st,
						     cur_file->d_name);
				if (ret) {
					error("unable to add file items for %s: %d",
						cur_file->d_name, ret);
					goto fail;
				}
			} else if (S_ISLNK(st.st_mode)) {
				ret = add_symbolic_link(trans, root,
						        cur_inum, cur_file->d_name);
				if (ret) {
					error("unable to add symlink for %s: %d",
						cur_file->d_name, ret);
					goto fail;
				}
			}
		}

		free_namelist(files, count);
		free(parent_dir_entry);

		index_cnt = 2;

	} while (!list_empty(&dir_head->list));

out:
	return !!ret;
fail:
	free_namelist(files, count);
fail_no_files:
	free(parent_dir_entry);
	goto out;
fail_no_dir:
	free(dir_entry);
	goto out;
}

static int make_image(const char *source_dir, struct btrfs_root *root)
{
	int ret;
	struct btrfs_trans_handle *trans;
	struct stat root_st;
	struct directory_name_entry dir_head;
	struct directory_name_entry *dir_entry = NULL;

	ret = lstat(source_dir, &root_st);
	if (ret) {
		error("unable to lstat %s: %s", source_dir, strerror(errno));
		ret = -errno;
		goto out;
	}

	INIT_LIST_HEAD(&dir_head.list);

	trans = btrfs_start_transaction(root, 1);
	BUG_ON(IS_ERR(trans));
	ret = traverse_directory(trans, root, source_dir, &dir_head);
	if (ret) {
		error("unable to traverse directory %s: %d", source_dir, ret);
		goto fail;
	}
	ret = btrfs_commit_transaction(trans, root);
	if (ret) {
		error("transaction commit failed: %d", ret);
		goto out;
	}

	if (verbose)
		printf("Making image is completed.\n");
	return 0;
fail:
	while (!list_empty(&dir_head.list)) {
		dir_entry = list_entry(dir_head.list.next,
				       struct directory_name_entry, list);
		list_del(&dir_entry->list);
		free(dir_entry);
	}
out:
	return ret;
}

static int is_ssd(const char *file)
{
	blkid_probe probe;
	char wholedisk[PATH_MAX];
	char sysfs_path[PATH_MAX];
	dev_t devno;
	int fd;
	char rotational;
	int ret;

	probe = blkid_new_probe_from_filename(file);
	if (!probe)
		return 0;

	/* Device number of this disk (possibly a partition) */
	devno = blkid_probe_get_devno(probe);
	if (!devno) {
		blkid_free_probe(probe);
		return 0;
	}

	/* Get whole disk name (not full path) for this devno */
	ret = blkid_devno_to_wholedisk(devno,
			wholedisk, sizeof(wholedisk), NULL);
	if (ret) {
		blkid_free_probe(probe);
		return 0;
	}

	snprintf(sysfs_path, PATH_MAX, "/sys/block/%s/queue/rotational",
		 wholedisk);

	blkid_free_probe(probe);

	fd = open(sysfs_path, O_RDONLY);
	if (fd < 0) {
		return 0;
	}

	if (read(fd, &rotational, 1) < 1) {
		close(fd);
		return 0;
	}
	close(fd);

	return rotational == '0';
}

static int _cmp_device_by_id(void *priv, struct list_head *a,
			     struct list_head *b)
{
	return list_entry(a, struct btrfs_device, dev_list)->devid -
	       list_entry(b, struct btrfs_device, dev_list)->devid;
}

static void list_all_devices(struct btrfs_root *root)
{
	struct btrfs_fs_devices *fs_devices;
	struct btrfs_device *device;
	int number_of_devices = 0;
	u64 total_block_count = 0;

	fs_devices = root->fs_info->fs_devices;

	list_for_each_entry(device, &fs_devices->devices, dev_list)
		number_of_devices++;

	list_sort(NULL, &fs_devices->devices, _cmp_device_by_id);

	printf("Number of devices:  %d\n", number_of_devices);
	/* printf("Total devices size: %10s\n", */
		/* pretty_size(total_block_count)); */
	printf("Devices:\n");
	printf("   ID        SIZE  PATH\n");
	list_for_each_entry(device, &fs_devices->devices, dev_list) {
		printf("  %3llu  %10s  %s\n",
			device->devid,
			pretty_size(device->total_bytes),
			device->name);
		total_block_count += device->total_bytes;
	}

	printf("\n");
}

static int is_temp_block_group(struct extent_buffer *node,
			       struct btrfs_block_group_item *bgi,
			       u64 data_profile, u64 meta_profile,
			       u64 sys_profile)
{
	u64 flag = btrfs_disk_block_group_flags(node, bgi);
	u64 flag_type = flag & BTRFS_BLOCK_GROUP_TYPE_MASK;
	u64 flag_profile = flag & BTRFS_BLOCK_GROUP_PROFILE_MASK;
	u64 used = btrfs_disk_block_group_used(node, bgi);

	/*
	 * Chunks meets all the following conditions is a temp chunk
	 * 1) Empty chunk
	 * Temp chunk is always empty.
	 *
	 * 2) profile mismatch with mkfs profile.
	 * Temp chunk is always in SINGLE
	 *
	 * 3) Size differs with mkfs_alloc
	 * Special case for SINGLE/SINGLE btrfs.
	 * In that case, temp data chunk and real data chunk are always empty.
	 * So we need to use mkfs_alloc to be sure which chunk is the newly
	 * allocated.
	 *
	 * Normally, new chunk size is equal to mkfs one (One chunk)
	 * If it has multiple chunks, we just refuse to delete any one.
	 * As they are all single, so no real problem will happen.
	 * So only use condition 1) and 2) to judge them.
	 */
	if (used != 0)
		return 0;
	switch (flag_type) {
	case BTRFS_BLOCK_GROUP_DATA:
	case BTRFS_BLOCK_GROUP_DATA | BTRFS_BLOCK_GROUP_METADATA:
		data_profile &= BTRFS_BLOCK_GROUP_PROFILE_MASK;
		if (flag_profile != data_profile)
			return 1;
		break;
	case BTRFS_BLOCK_GROUP_METADATA:
		meta_profile &= BTRFS_BLOCK_GROUP_PROFILE_MASK;
		if (flag_profile != meta_profile)
			return 1;
		break;
	case BTRFS_BLOCK_GROUP_SYSTEM:
		sys_profile &= BTRFS_BLOCK_GROUP_PROFILE_MASK;
		if (flag_profile != sys_profile)
			return 1;
		break;
	}
	return 0;
}

/* Note: if current is a block group, it will skip it anyway */
static int next_block_group(struct btrfs_root *root,
			    struct btrfs_path *path)
{
	struct btrfs_key key;
	int ret = 0;

	while (1) {
		ret = btrfs_next_item(root, path);
		if (ret)
			goto out;

		btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
		if (key.type == BTRFS_BLOCK_GROUP_ITEM_KEY)
			goto out;
	}
out:
	return ret;
}

/* This function will cleanup  */
static int cleanup_temp_chunks(struct btrfs_fs_info *fs_info,
			       struct mkfs_allocation *alloc,
			       u64 data_profile, u64 meta_profile,
			       u64 sys_profile)
{
	struct btrfs_trans_handle *trans = NULL;
	struct btrfs_block_group_item *bgi;
	struct btrfs_root *root = fs_info->extent_root;
	struct btrfs_key key;
	struct btrfs_key found_key;
	struct btrfs_path path;
	int ret = 0;

	btrfs_init_path(&path);
	trans = btrfs_start_transaction(root, 1);
	BUG_ON(IS_ERR(trans));

	key.objectid = 0;
	key.type = BTRFS_BLOCK_GROUP_ITEM_KEY;
	key.offset = 0;

	while (1) {
		/*
		 * as the rest of the loop may modify the tree, we need to
		 * start a new search each time.
		 */
		ret = btrfs_search_slot(trans, root, &key, &path, 0, 0);
		if (ret < 0)
			goto out;

		btrfs_item_key_to_cpu(path.nodes[0], &found_key,
				      path.slots[0]);
		if (found_key.objectid < key.objectid)
			goto out;
		if (found_key.type != BTRFS_BLOCK_GROUP_ITEM_KEY) {
			ret = next_block_group(root, &path);
			if (ret < 0)
				goto out;
			if (ret > 0) {
				ret = 0;
				goto out;
			}
			btrfs_item_key_to_cpu(path.nodes[0], &found_key,
					      path.slots[0]);
		}

		bgi = btrfs_item_ptr(path.nodes[0], path.slots[0],
				     struct btrfs_block_group_item);
		if (is_temp_block_group(path.nodes[0], bgi,
					data_profile, meta_profile,
					sys_profile)) {
			u64 flags = btrfs_disk_block_group_flags(path.nodes[0],
							     bgi);

			ret = btrfs_free_block_group(trans, fs_info,
					found_key.objectid, found_key.offset);
			if (ret < 0)
				goto out;

			if ((flags & BTRFS_BLOCK_GROUP_TYPE_MASK) ==
			    BTRFS_BLOCK_GROUP_DATA)
				alloc->data -= found_key.offset;
			else if ((flags & BTRFS_BLOCK_GROUP_TYPE_MASK) ==
				 BTRFS_BLOCK_GROUP_METADATA)
				alloc->metadata -= found_key.offset;
			else if ((flags & BTRFS_BLOCK_GROUP_TYPE_MASK) ==
				 BTRFS_BLOCK_GROUP_SYSTEM)
				alloc->system -= found_key.offset;
			else if ((flags & BTRFS_BLOCK_GROUP_TYPE_MASK) ==
				 (BTRFS_BLOCK_GROUP_METADATA |
				  BTRFS_BLOCK_GROUP_DATA))
				alloc->mixed -= found_key.offset;
		}
		btrfs_release_path(&path);
		key.objectid = found_key.objectid + found_key.offset;
	}
out:
	if (trans)
		btrfs_commit_transaction(trans, root);
	btrfs_release_path(&path);
	return ret;
}

int main(int argc, char **argv)
{
	char *file;
	struct btrfs_root *root;
	struct btrfs_fs_info *fs_info;
	struct btrfs_trans_handle *trans;
	char *label = NULL;
	u64 block_count = 0;
	u64 dev_block_count = 0;
	u64 alloc_start = 0;
	u64 metadata_profile = 0;
	u64 data_profile = 0;
	u32 nodesize = max_t(u32, sysconf(_SC_PAGESIZE),
			BTRFS_MKFS_DEFAULT_NODE_SIZE);
	u32 sectorsize = 4096;
	u32 stripesize = 4096;
	int zero_end = 1;
	int fd = -1;
	int ret;
	int i;
	int mixed = 0;
	int nodesize_forced = 0;
	int data_profile_opt = 0;
	int metadata_profile_opt = 0;
	int discard = 1;
	int ssd = 0;
	int force_overwrite = 0;
	char *source_dir = NULL;
	int source_dir_set = 0;
	int dev_cnt = 0;
	int saved_optind;
	char fs_uuid[BTRFS_UUID_UNPARSED_SIZE] = { 0 };
	u64 features = BTRFS_MKFS_DEFAULT_FEATURES;
	struct mkfs_allocation allocation = { 0 };
	struct btrfs_mkfs_config mkfs_cfg;

	while(1) {
		int c;
		static const struct option long_options[] = {
			{ "alloc-start", required_argument, NULL, 'A'},
			{ "byte-count", required_argument, NULL, 'b' },
			{ "force", no_argument, NULL, 'f' },
			{ "leafsize", required_argument, NULL, 'l' },
			{ "label", required_argument, NULL, 'L'},
			{ "metadata", required_argument, NULL, 'm' },
			{ "mixed", no_argument, NULL, 'M' },
			{ "nodesize", required_argument, NULL, 'n' },
			{ "sectorsize", required_argument, NULL, 's' },
			{ "data", required_argument, NULL, 'd' },
			{ "version", no_argument, NULL, 'V' },
			{ "rootdir", required_argument, NULL, 'r' },
			{ "nodiscard", no_argument, NULL, 'K' },
			{ "features", required_argument, NULL, 'O' },
			{ "uuid", required_argument, NULL, 'U' },
			{ "quiet", 0, NULL, 'q' },
			{ "help", no_argument, NULL, GETOPT_VAL_HELP },
			{ NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "A:b:fl:n:s:m:d:L:O:r:U:VMKq",
				long_options, NULL);
		if (c < 0)
			break;
		switch(c) {
			case 'A':
				alloc_start = parse_size(optarg);
				break;
			case 'f':
				force_overwrite = 1;
				break;
			case 'd':
				data_profile = parse_profile(optarg);
				data_profile_opt = 1;
				break;
			case 'l':
				warning("--leafsize is deprecated, use --nodesize");
				/* fall through */
			case 'n':
				nodesize = parse_size(optarg);
				nodesize_forced = 1;
				break;
			case 'L':
				label = parse_label(optarg);
				break;
			case 'm':
				metadata_profile = parse_profile(optarg);
				metadata_profile_opt = 1;
				break;
			case 'M':
				mixed = 1;
				break;
			case 'O': {
				char *orig = strdup(optarg);
				char *tmp = orig;

				tmp = btrfs_parse_fs_features(tmp, &features);
				if (tmp) {
					error("unrecognized filesystem feature '%s'",
							tmp);
					free(orig);
					goto error;
				}
				free(orig);
				if (features & BTRFS_FEATURE_LIST_ALL) {
					btrfs_list_all_fs_features(0);
					goto success;
				}
				break;
				}
			case 's':
				sectorsize = parse_size(optarg);
				break;
			case 'b':
				block_count = parse_size(optarg);
				zero_end = 0;
				break;
			case 'V':
				printf("mkfs.btrfs, part of %s\n",
						PACKAGE_STRING);
				goto success;
			case 'r':
				source_dir = optarg;
				source_dir_set = 1;
				break;
			case 'U':
				strncpy(fs_uuid, optarg,
					BTRFS_UUID_UNPARSED_SIZE - 1);
				break;
			case 'K':
				discard = 0;
				break;
			case 'q':
				verbose = 0;
				break;
			case GETOPT_VAL_HELP:
			default:
				print_usage(c != GETOPT_VAL_HELP);
		}
	}

	if (verbose) {
		printf("%s\n", PACKAGE_STRING);
		printf("See %s for more information.\n\n", PACKAGE_URL);
	}

	sectorsize = max(sectorsize, (u32)sysconf(_SC_PAGESIZE));
	stripesize = sectorsize;
	saved_optind = optind;
	dev_cnt = argc - optind;
	if (dev_cnt == 0)
		print_usage(1);

	if (source_dir_set && dev_cnt > 1) {
		error("the option -r is limited to a single device");
		goto error;
	}

	if (*fs_uuid) {
		uuid_t dummy_uuid;

		if (uuid_parse(fs_uuid, dummy_uuid) != 0) {
			error("could not parse UUID: %s", fs_uuid);
			goto error;
		}
		if (!test_uuid_unique(fs_uuid)) {
			error("non-unique UUID: %s", fs_uuid);
			goto error;
		}
	}

	while (dev_cnt-- > 0) {
		file = argv[optind++];
		if (is_block_device(file) == 1)
			if (test_dev_for_mkfs(file, force_overwrite))
				goto error;
	}

	optind = saved_optind;
	dev_cnt = argc - optind;

	file = argv[optind++];
	ssd = is_ssd(file);

	/*
	* Set default profiles according to number of added devices.
	* For mixed groups defaults are single/single.
	*/
	if (!mixed) {
		if (!metadata_profile_opt) {
			if (dev_cnt == 1 && ssd && verbose)
				printf("Detected a SSD, turning off metadata "
				"duplication.  Mkfs with -m dup if you want to "
				"force metadata duplication.\n");

			metadata_profile = (dev_cnt > 1) ?
					BTRFS_BLOCK_GROUP_RAID1 : (ssd) ?
					0: BTRFS_BLOCK_GROUP_DUP;
		}
		if (!data_profile_opt) {
			data_profile = (dev_cnt > 1) ?
				BTRFS_BLOCK_GROUP_RAID0 : 0; /* raid0 or single */
		}
	} else {
		u32 best_nodesize = max_t(u32, sysconf(_SC_PAGESIZE), sectorsize);

		if (metadata_profile_opt || data_profile_opt) {
			if (metadata_profile != data_profile) {
				error(
	"with mixed block groups data and metadata profiles must be the same");
				goto error;
			}
		}

		if (!nodesize_forced)
			nodesize = best_nodesize;
	}

	/*
	 * FS features that can be set by other means than -O
	 * just set the bit here
	 */
	if (mixed)
		features |= BTRFS_FEATURE_INCOMPAT_MIXED_GROUPS;

	if ((data_profile | metadata_profile) &
	    (BTRFS_BLOCK_GROUP_RAID5 | BTRFS_BLOCK_GROUP_RAID6)) {
		features |= BTRFS_FEATURE_INCOMPAT_RAID56;
	}

	if (btrfs_check_nodesize(nodesize, sectorsize,
				 features))
		goto error;

	if (sectorsize < sizeof(struct btrfs_super_block)) {
		error("sectorsize smaller than superblock: %u < %zu",
				sectorsize, sizeof(struct btrfs_super_block));
		goto error;
	}

	/* Check device/block_count after the nodesize is determined */
	if (block_count && block_count < btrfs_min_dev_size(nodesize)) {
		error("size %llu is too small to make a usable filesystem",
			block_count);
		error("minimum size for btrfs filesystem is %llu",
			btrfs_min_dev_size(nodesize));
		goto error;
	}
	for (i = saved_optind; i < saved_optind + dev_cnt; i++) {
		char *path;

		path = argv[i];
		ret = test_minimum_size(path, nodesize);
		if (ret < 0) {
			error("failed to check size for %s: %s",
				path, strerror(-ret));
			goto error;
		}
		if (ret > 0) {
			error("'%s' is too small to make a usable filesystem",
				path);
			error("minimum size for each btrfs device is %llu",
				btrfs_min_dev_size(nodesize));
			goto error;
		}
	}
	ret = test_num_disk_vs_raid(metadata_profile, data_profile,
			dev_cnt, mixed, ssd);
	if (ret)
		goto error;

	dev_cnt--;

	/*
	 * open without O_EXCL so that the problem should not
	 * occur by the following processing.
	 * (btrfs_register_one_device() fails if O_EXCL is on)
	 */
	fd = open(file, O_RDWR);
	if (fd < 0) {
		error("unable to open %s: %s", file, strerror(errno));
		goto error;
	}
	ret = btrfs_prepare_device(fd, file, &dev_block_count,
			block_count,
			(zero_end ? PREP_DEVICE_ZERO_END : 0) |
			(discard ? PREP_DEVICE_DISCARD : 0) |
			(verbose ? PREP_DEVICE_VERBOSE : 0));
	if (ret)
		goto error;
	if (block_count && block_count > dev_block_count) {
		error(
		"%s is smaller than requested size, expected %llu, found %llu",
			file,
			(unsigned long long)block_count,
			(unsigned long long)dev_block_count);
		goto error;
	}

	/* To create the first block group and chunk 0 in make_btrfs */
	if (dev_block_count < BTRFS_MKFS_SYSTEM_GROUP_SIZE) {
		error("device is too small to make filesystem, must be at least %llu",
				(unsigned long long)BTRFS_MKFS_SYSTEM_GROUP_SIZE);
		goto error;
	}

	if (group_profile_max_safe_loss(metadata_profile) <
		group_profile_max_safe_loss(data_profile)){
		warning("metadata has lower redundancy than data!\n");
	}

	mkfs_cfg.label = label;
	memcpy(mkfs_cfg.fs_uuid, fs_uuid, sizeof(mkfs_cfg.fs_uuid));
	mkfs_cfg.num_bytes = dev_block_count;
	mkfs_cfg.nodesize = nodesize;
	mkfs_cfg.sectorsize = sectorsize;
	mkfs_cfg.stripesize = stripesize;
	mkfs_cfg.features = features;

	ret = make_btrfs(fd, &mkfs_cfg);
	if (ret) {
		error("error during mkfs: %s", strerror(-ret));
		goto error;
	}

	fs_info = open_ctree_fs_info(file, 0, 0, 0,
			OPEN_CTREE_WRITES | OPEN_CTREE_FS_PARTIAL);
	if (!fs_info) {
		error("open ctree failed");
		goto error;
	}
	close(fd);
	fd = -1;
	root = fs_info->fs_root;
	fs_info->alloc_start = alloc_start;

	ret = create_metadata_block_groups(root, mixed, &allocation);
	if (ret) {
		error("failed to create default block groups: %d", ret);
		goto error;
	}

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		error("failed to start transaction");
		goto error;
	}

	ret = create_data_block_groups(trans, root, mixed, &allocation);
	if (ret) {
		error("failed to create default data block groups: %d", ret);
		goto error;
	}

	ret = make_root_dir(trans, root);
	if (ret) {
		error("failed to setup the root directory: %d", ret);
		goto error;
	}

	ret = btrfs_commit_transaction(trans, root);
	if (ret) {
		error("unable to commit transaction: %d", ret);
		goto out;
	}

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		error("failed to start transaction");
		goto error;
	}

	if (dev_cnt == 0)
		goto raid_groups;

	while (dev_cnt-- > 0) {
		file = argv[optind++];

		/*
		 * open without O_EXCL so that the problem should not
		 * occur by the following processing.
		 * (btrfs_register_one_device() fails if O_EXCL is on)
		 */
		fd = open(file, O_RDWR);
		if (fd < 0) {
			error("unable to open %s: %s", file, strerror(errno));
			goto error;
		}
		ret = btrfs_device_already_in_root(root, fd,
						   BTRFS_SUPER_INFO_OFFSET);
		if (ret) {
			error("skipping duplicate device %s in the filesystem",
				file);
			close(fd);
			continue;
		}
		ret = btrfs_prepare_device(fd, file, &dev_block_count,
				block_count,
				(verbose ? PREP_DEVICE_VERBOSE : 0) |
				(zero_end ? PREP_DEVICE_ZERO_END : 0) |
				(discard ? PREP_DEVICE_DISCARD : 0));
		if (ret) {
			goto error;
		}

		ret = btrfs_add_to_fsid(trans, root, fd, file, dev_block_count,
					sectorsize, sectorsize, sectorsize);
		if (ret) {
			error("unable to add %s to filesystem: %d", file, ret);
			goto out;
		}
		if (verbose >= 2) {
			struct btrfs_device *device;

			device = container_of(fs_info->fs_devices->devices.next,
					struct btrfs_device, dev_list);
			printf("adding device %s id %llu\n", file,
				(unsigned long long)device->devid);
		}
	}

raid_groups:
	ret = create_raid_groups(trans, root, data_profile,
				 metadata_profile, mixed, &allocation);
	if (ret) {
		error("unable to create raid groups: %d", ret);
		goto out;
	}

	ret = create_tree(trans, root, BTRFS_DATA_RELOC_TREE_OBJECTID);
	if (ret) {
		error("unable to create data reloc tree: %d", ret);
		goto out;
	}

	ret = btrfs_commit_transaction(trans, root);
	if (ret) {
		error("unable to commit transaction: %d", ret);
		goto out;
	}

	ret = cleanup_temp_chunks(fs_info, &allocation, data_profile,
				  metadata_profile, metadata_profile);
	if (ret < 0) {
		error("failed to cleanup temporary chunks: %d", ret);
		goto out;
	}

	if (source_dir_set) {
		ret = make_image(source_dir, root);
		if (ret) {
			error("error wihle filling filesystem: %d", ret);
			goto out;
		}
	}

	if (verbose) {
		char features_buf[64];

		printf("Label:              %s\n", label);
		printf("UUID:               %s\n", mkfs_cfg.fs_uuid);
		printf("Node size:          %u\n", nodesize);
		printf("Sector size:        %u\n", sectorsize);
		printf("Filesystem size:    %s\n",
			pretty_size(btrfs_super_total_bytes(fs_info->super_copy)));
		printf("Block group profiles:\n");
		if (allocation.data)
			printf("  Data:             %-8s %16s\n",
				btrfs_group_profile_str(data_profile),
				pretty_size(allocation.data));
		if (allocation.metadata)
			printf("  Metadata:         %-8s %16s\n",
				btrfs_group_profile_str(metadata_profile),
				pretty_size(allocation.metadata));
		if (allocation.mixed)
			printf("  Data+Metadata:    %-8s %16s\n",
				btrfs_group_profile_str(data_profile),
				pretty_size(allocation.mixed));
		printf("  System:           %-8s %16s\n",
			btrfs_group_profile_str(metadata_profile),
			pretty_size(allocation.system));
		printf("SSD detected:       %s\n", ssd ? "yes" : "no");
		btrfs_parse_features_to_string(features_buf, features);
		printf("Incompat features:  %s", features_buf);
		printf("\n");

		list_all_devices(root);
	}

	/*
	 * The filesystem is now fully set up, commit the remaining changes and
	 * fix the signature as the last step before closing the devices.
	 */
	fs_info->finalize_on_close = 1;
out:
	ret = close_ctree(root);

	if (!ret) {
		optind = saved_optind;
		dev_cnt = argc - optind;
		while (dev_cnt-- > 0) {
			file = argv[optind++];
			if (is_block_device(file) == 1)
				btrfs_register_one_device(file);
		}
	}

	btrfs_close_all_devices();
	free(label);

	return !!ret;
error:
	if (fd > 0)
		close(fd);

	free(label);
	exit(1);
success:
	exit(0);
}
