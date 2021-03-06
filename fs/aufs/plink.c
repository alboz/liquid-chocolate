/*
 * Copyright (C) 2005-2010 Junjiro R. Okajima
 *
 * This program, aufs is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

/*
 * pseudo-link
 */

#include "aufs.h"

/*
 * during a user process maintains the pseudo-links,
 * prohibit adding a new plink and branch manipulation.
 */
void au_plink_maint_block(struct super_block *sb)
{
	struct au_sbinfo *sbi = au_sbi(sb);

	SiMustAnyLock(sb);

	/* gave up wake_up_bit() */
	wait_event(sbi->si_plink_wq, !sbi->si_plink_maint);
}

void au_plink_maint_leave(struct file *file)
{
	struct au_sbinfo *sbinfo;
	int iam;

	AuDebugOn(atomic_long_read(&file->f_count));

	sbinfo = au_sbi(file->f_dentry->d_sb);
	spin_lock(&sbinfo->si_plink_maint_lock);
	iam = (sbinfo->si_plink_maint == file);
	if (iam)
		sbinfo->si_plink_maint = NULL;
	spin_unlock(&sbinfo->si_plink_maint_lock);
	if (iam)
		wake_up_all(&sbinfo->si_plink_wq);
}

static int au_plink_maint_enter(struct file *file)
{
	int err;
	struct super_block *sb;
	struct au_sbinfo *sbinfo;

	err = 0;
	sb = file->f_dentry->d_sb;
	sbinfo = au_sbi(sb);
	/* make sure i am the only one in this fs */
	si_write_lock(sb);
	/* spin_lock(&sbinfo->si_plink_maint_lock); */
	if (!sbinfo->si_plink_maint)
		sbinfo->si_plink_maint = file;
	else
		err = -EBUSY;
	/* spin_unlock(&sbinfo->si_plink_maint_lock); */
	si_write_unlock(sb);

	return err;
}

/* ---------------------------------------------------------------------- */

struct pseudo_link {
	union {
		struct list_head list;
		struct rcu_head rcu;
	};
	struct inode *inode;
};

#ifdef CONFIG_AUFS_DEBUG
void au_plink_list(struct super_block *sb)
{
	struct au_sbinfo *sbinfo;
	struct list_head *plink_list;
	struct pseudo_link *plink;

	SiMustAnyLock(sb);

	sbinfo = au_sbi(sb);
	AuDebugOn(!au_opt_test(au_mntflags(sb), PLINK));

	plink_list = &sbinfo->si_plink.head;
	rcu_read_lock();
	list_for_each_entry_rcu(plink, plink_list, list)
		AuDbg("%lu\n", plink->inode->i_ino);
	rcu_read_unlock();
}
#endif

/* is the inode pseudo-linked? */
int au_plink_test(struct inode *inode)
{
	int found;
	struct au_sbinfo *sbinfo;
	struct list_head *plink_list;
	struct pseudo_link *plink;

	sbinfo = au_sbi(inode->i_sb);
	AuRwMustAnyLock(&sbinfo->si_rwsem);
	AuDebugOn(!au_opt_test(au_mntflags(inode->i_sb), PLINK));

	found = 0;
	plink_list = &sbinfo->si_plink.head;
	rcu_read_lock();
	list_for_each_entry_rcu(plink, plink_list, list)
		if (plink->inode == inode) {
			found = 1;
			break;
		}
	rcu_read_unlock();
	return found;
}

/* ---------------------------------------------------------------------- */

/*
 * generate a name for plink.
 * the file will be stored under AUFS_WH_PLINKDIR.
 */
/* 20 is max digits length of ulong 64 */
#define PLINK_NAME_LEN	((20 + 1) * 2)

static int plink_name(char *name, int len, struct inode *inode,
		      aufs_bindex_t bindex)
{
	int rlen;
	struct inode *h_inode;

	h_inode = au_h_iptr(inode, bindex);
	rlen = snprintf(name, len, "%lu.%lu", inode->i_ino, h_inode->i_ino);
	return rlen;
}

/* lookup the plink-ed @inode under the branch at @bindex */
struct dentry *au_plink_lkup(struct inode *inode, aufs_bindex_t bindex)
{
	struct dentry *h_dentry, *h_parent;
	struct au_branch *br;
	struct inode *h_dir;
	char a[PLINK_NAME_LEN];
	struct qstr tgtname = {
		.name	= a
	};

	br = au_sbr(inode->i_sb, bindex);
	h_parent = br->br_wbr->wbr_plink;
	h_dir = h_parent->d_inode;
	tgtname.len = plink_name(a, sizeof(a), inode, bindex);

	/* always superio. */
	mutex_lock_nested(&h_dir->i_mutex, AuLsc_I_CHILD2);
	h_dentry = au_sio_lkup_one(&tgtname, h_parent, br);
	mutex_unlock(&h_dir->i_mutex);
	return h_dentry;
}

/* create a pseudo-link */
static int do_whplink(struct qstr *tgt, struct dentry *h_parent,
		      struct dentry *h_dentry, struct au_branch *br)
{
	int err;
	struct path h_path = {
		.mnt = br->br_mnt
	};
	struct inode *h_dir;

	h_dir = h_parent->d_inode;
again:
	h_path.dentry = au_lkup_one(tgt, h_parent, br, /*nd*/NULL);
	err = PTR_ERR(h_path.dentry);
	if (IS_ERR(h_path.dentry))
		goto out;

	err = 0;
	/* wh.plink dir is not monitored */
	if (h_path.dentry->d_inode
	    && h_path.dentry->d_inode != h_dentry->d_inode) {
		err = vfsub_unlink(h_dir, &h_path, /*force*/0);
		dput(h_path.dentry);
		h_path.dentry = NULL;
		if (!err)
			goto again;
	}
	if (!err && !h_path.dentry->d_inode)
		err = vfsub_link(h_dentry, h_dir, &h_path);
	dput(h_path.dentry);

out:
	return err;
}

struct do_whplink_args {
	int *errp;
	struct qstr *tgt;
	struct dentry *h_parent;
	struct dentry *h_dentry;
	struct au_branch *br;
};

static void call_do_whplink(void *args)
{
	struct do_whplink_args *a = args;
	*a->errp = do_whplink(a->tgt, a->h_parent, a->h_dentry, a->br);
}

static int whplink(struct dentry *h_dentry, struct inode *inode,
		   aufs_bindex_t bindex, struct au_branch *br)
{
	int err, wkq_err;
	struct au_wbr *wbr;
	struct dentry *h_parent;
	struct inode *h_dir;
	char a[PLINK_NAME_LEN];
	struct qstr tgtname = {
		.name = a
	};

	wbr = au_sbr(inode->i_sb, bindex)->br_wbr;
	h_parent = wbr->wbr_plink;
	h_dir = h_parent->d_inode;
	tgtname.len = plink_name(a, sizeof(a), inode, bindex);

	/* always superio. */
	mutex_lock_nested(&h_dir->i_mutex, AuLsc_I_CHILD2);
	if (current_fsuid()) {
		struct do_whplink_args args = {
			.errp		= &err,
			.tgt		= &tgtname,
			.h_parent	= h_parent,
			.h_dentry	= h_dentry,
			.br		= br
		};
		wkq_err = au_wkq_wait(call_do_whplink, &args);
		if (unlikely(wkq_err))
			err = wkq_err;
	} else
		err = do_whplink(&tgtname, h_parent, h_dentry, br);
	mutex_unlock(&h_dir->i_mutex);

	return err;
}

/* free a single plink */
static void do_put_plink(struct pseudo_link *plink, int do_del)
{
	if (do_del)
		list_del(&plink->list);
	iput(plink->inode);
	kfree(plink);
}

static void do_put_plink_rcu(struct rcu_head *rcu)
{
	struct pseudo_link *plink;

	plink = container_of(rcu, struct pseudo_link, rcu);
	iput(plink->inode);
	kfree(plink);
}

/*
 * create a new pseudo-link for @h_dentry on @bindex.
 * the linked inode is held in aufs @inode.
 */
void au_plink_append(struct inode *inode, aufs_bindex_t bindex,
		     struct dentry *h_dentry)
{
	struct super_block *sb;
	struct au_sbinfo *sbinfo;
	struct list_head *plink_list;
	struct pseudo_link *plink, *tmp;
	int found, err, cnt;

	sb = inode->i_sb;
	sbinfo = au_sbi(sb);
	AuDebugOn(!au_opt_test(au_mntflags(sb), PLINK));

	cnt = 0;
	found = 0;
	plink_list = &sbinfo->si_plink.head;
	rcu_read_lock();
	list_for_each_entry_rcu(plink, plink_list, list) {
		cnt++;
		if (plink->inode == inode) {
			found = 1;
			break;
		}
	}
	rcu_read_unlock();
	if (found)
		return;

	tmp = kmalloc(sizeof(*plink), GFP_NOFS);
	if (tmp)
		tmp->inode = au_igrab(inode);
	else {
		err = -ENOMEM;
		goto out;
	}

	spin_lock(&sbinfo->si_plink.spin);
	list_for_each_entry(plink, plink_list, list) {
		if (plink->inode == inode) {
			found = 1;
			break;
		}
	}
	if (!found)
		list_add_rcu(&tmp->list, plink_list);
	spin_unlock(&sbinfo->si_plink.spin);
	if (!found) {
		cnt++;
		WARN_ONCE(cnt > AUFS_PLINK_WARN,
			  "unexpectedly many pseudo links, %d\n", cnt);
		au_plink_maint_block(sb);
		err = whplink(h_dentry, inode, bindex, au_sbr(sb, bindex));
	} else {
		do_put_plink(tmp, 0);
		return;
	}

out:
	if (unlikely(err)) {
		pr_warning("err %d, damaged pseudo link.\n", err);
		if (tmp) {
			au_spl_del_rcu(&tmp->list, &sbinfo->si_plink);
			call_rcu(&tmp->rcu, do_put_plink_rcu);
		}
	}
}

/* free all plinks */
void au_plink_put(struct super_block *sb)
{
	struct au_sbinfo *sbinfo;
	struct list_head *plink_list;
	struct pseudo_link *plink, *tmp;

	SiMustWriteLock(sb);

	sbinfo = au_sbi(sb);
	AuDebugOn(!au_opt_test(au_mntflags(sb), PLINK));

	plink_list = &sbinfo->si_plink.head;
	/* no spin_lock since sbinfo is write-locked */
	list_for_each_entry_safe(plink, tmp, plink_list, list)
		do_put_plink(plink, 0);
	INIT_LIST_HEAD(plink_list);
}

/* free the plinks on a branch specified by @br_id */
void au_plink_half_refresh(struct super_block *sb, aufs_bindex_t br_id)
{
	struct au_sbinfo *sbinfo;
	struct list_head *plink_list;
	struct pseudo_link *plink, *tmp;
	struct inode *inode;
	aufs_bindex_t bstart, bend, bindex;
	unsigned char do_put;

	SiMustWriteLock(sb);

	sbinfo = au_sbi(sb);
	AuDebugOn(!au_opt_test(au_mntflags(sb), PLINK));

	plink_list = &sbinfo->si_plink.head;
	/* no spin_lock since sbinfo is write-locked */
	list_for_each_entry_safe(plink, tmp, plink_list, list) {
		do_put = 0;
		inode = au_igrab(plink->inode);
		ii_write_lock_child(inode);
		bstart = au_ibstart(inode);
		bend = au_ibend(inode);
		if (bstart >= 0) {
			for (bindex = bstart; bindex <= bend; bindex++) {
				if (!au_h_iptr(inode, bindex)
				    || au_ii_br_id(inode, bindex) != br_id)
					continue;
				au_set_h_iptr(inode, bindex, NULL, 0);
				do_put = 1;
				break;
			}
		} else
			do_put_plink(plink, 1);

		if (do_put) {
			for (bindex = bstart; bindex <= bend; bindex++)
				if (au_h_iptr(inode, bindex)) {
					do_put = 0;
					break;
				}
			if (do_put)
				do_put_plink(plink, 1);
		}
		ii_write_unlock(inode);
		iput(inode);
	}
}

/* ---------------------------------------------------------------------- */

long au_plink_ioctl(struct file *file, unsigned int cmd)
{
	long err;
	struct super_block *sb;

	err = -EACCES;
	if (!capable(CAP_SYS_ADMIN))
		goto out;

	err = 0;
	sb = file->f_dentry->d_sb;
	switch (cmd) {
	case AUFS_CTL_PLINK_MAINT:
		/*
		 * pseudo-link maintenance mode,
		 * cleared by aufs_release_dir()
		 */
		err = au_plink_maint_enter(file);
		break;
	case AUFS_CTL_PLINK_CLEAN:
		aufs_write_lock(sb->s_root);
		if (au_opt_test(au_mntflags(sb), PLINK))
			au_plink_put(sb);
		aufs_write_unlock(sb->s_root);
		break;
	default:
		/* err = -ENOTTY; */
		err = -EINVAL;
	}
out:
	return err;
}
