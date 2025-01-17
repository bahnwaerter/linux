// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2010 Red Hat, Inc.
 * Copyright (c) 2016-2019 Christoph Hellwig.
 */
#include <linux/module.h>
#include <linux/compiler.h>
#include <linux/fs.h>
#include <linux/iomap.h>
#include <linux/pagemap.h>
#include <linux/uio.h>
#include <linux/buffer_head.h>
#include <linux/dax.h>
#include <linux/writeback.h>
#include <linux/list_sort.h>
#include <linux/swap.h>
#include <linux/bio.h>
#include <linux/sched/signal.h>
#include <linux/migrate.h>

#include "../internal.h"

#define CREATE_TRACE_POINTS
#include <trace/events/iomap.h>

static struct bio_set iomap_ioend_bioset;

static struct iomap_page *
iomap_page_create(struct inode *inode, struct page *page)
{
	struct iomap_page *iop = to_iomap_page(page);

	if (iop || i_blocksize(inode) == PAGE_SIZE)
		return iop;

	iop = kmalloc(sizeof(*iop), GFP_NOFS | __GFP_NOFAIL);
	atomic_set(&iop->read_count, 0);
	atomic_set(&iop->write_count, 0);
	bitmap_zero(iop->uptodate, PAGE_SIZE / SECTOR_SIZE);

	/*
	 * migrate_page_move_mapping() assumes that pages with private data have
	 * their count elevated by 1.
	 */
	get_page(page);
	set_page_private(page, (unsigned long)iop);
	SetPagePrivate(page);
	return iop;
}

static void
iomap_page_release(struct page *page)
{
	struct iomap_page *iop = to_iomap_page(page);

	if (!iop)
		return;
	WARN_ON_ONCE(atomic_read(&iop->read_count));
	WARN_ON_ONCE(atomic_read(&iop->write_count));
	ClearPagePrivate(page);
	set_page_private(page, 0);
	put_page(page);
	kfree(iop);
}

/*
 * Calculate the range inside the page that we actually need to read.
 */
static void
iomap_adjust_read_range(struct inode *inode, struct iomap_page *iop,
		loff_t *pos, loff_t length, unsigned *offp, unsigned *lenp)
{
	loff_t orig_pos = *pos;
	loff_t isize = i_size_read(inode);
	unsigned block_bits = inode->i_blkbits;
	unsigned block_size = (1 << block_bits);
	unsigned poff = offset_in_page(*pos);
	unsigned plen = min_t(loff_t, PAGE_SIZE - poff, length);
	unsigned first = poff >> block_bits;
	unsigned last = (poff + plen - 1) >> block_bits;

	/*
	 * If the block size is smaller than the page size we need to check the
	 * per-block uptodate status and adjust the offset and length if needed
	 * to avoid reading in already uptodate ranges.
	 */
	if (iop) {
		unsigned int i;

		/* move forward for each leading block marked uptodate */
		for (i = first; i <= last; i++) {
			if (!test_bit(i, iop->uptodate))
				break;
			*pos += block_size;
			poff += block_size;
			plen -= block_size;
			first++;
		}

		/* truncate len if we find any trailing uptodate block(s) */
		for ( ; i <= last; i++) {
			if (test_bit(i, iop->uptodate)) {
				plen -= (last - i + 1) * block_size;
				last = i - 1;
				break;
			}
		}
	}

	/*
	 * If the extent spans the block that contains the i_size we need to
	 * handle both halves separately so that we properly zero data in the
	 * page cache for blocks that are entirely outside of i_size.
	 */
	if (orig_pos <= isize && orig_pos + length > isize) {
		unsigned end = offset_in_page(isize - 1) >> block_bits;

		if (first <= end && last > end)
			plen -= (last - end) * block_size;
	}

	*offp = poff;
	*lenp = plen;
}

static void
iomap_set_range_uptodate(struct page *page, unsigned off, unsigned len)
{
	struct iomap_page *iop = to_iomap_page(page);
	struct inode *inode = page->mapping->host;
	unsigned first = off >> inode->i_blkbits;
	unsigned last = (off + len - 1) >> inode->i_blkbits;
	unsigned int i;
	bool uptodate = true;

	if (iop) {
		for (i = 0; i < PAGE_SIZE / i_blocksize(inode); i++) {
			if (i >= first && i <= last)
				set_bit(i, iop->uptodate);
			else if (!test_bit(i, iop->uptodate))
				uptodate = false;
		}
	}

	if (uptodate && !PageError(page))
		SetPageUptodate(page);
}

static void
iomap_read_finish(struct iomap_page *iop, struct page *page)
{
	if (!iop || atomic_dec_and_test(&iop->read_count))
		unlock_page(page);
}

static void
iomap_read_page_end_io(struct bio_vec *bvec, int error)
{
	struct page *page = bvec->bv_page;
	struct iomap_page *iop = to_iomap_page(page);

	if (unlikely(error)) {
		ClearPageUptodate(page);
		SetPageError(page);
	} else {
		iomap_set_range_uptodate(page, bvec->bv_offset, bvec->bv_len);
	}

	iomap_read_finish(iop, page);
}

static void
iomap_read_end_io(struct bio *bio)
{
	int error = blk_status_to_errno(bio->bi_status);
	struct bio_vec *bvec;
	struct bvec_iter_all iter_all;

	bio_for_each_segment_all(bvec, bio, iter_all)
		iomap_read_page_end_io(bvec, error);
	bio_put(bio);
}

struct iomap_readpage_ctx {
	struct page		*cur_page;
	bool			cur_page_in_bio;
	bool			is_readahead;
	struct bio		*bio;
	struct list_head	*pages;
};

static void
iomap_read_inline_data(struct inode *inode, struct page *page,
		struct iomap *iomap)
{
	size_t size = i_size_read(inode);
	void *addr;

	if (PageUptodate(page))
		return;

	BUG_ON(page->index);
	BUG_ON(size > PAGE_SIZE - offset_in_page(iomap->inline_data));

	addr = kmap_atomic(page);
	memcpy(addr, iomap->inline_data, size);
	memset(addr + size, 0, PAGE_SIZE - size);
	kunmap_atomic(addr);
	SetPageUptodate(page);
}

static inline bool iomap_block_needs_zeroing(struct inode *inode,
		struct iomap *iomap, loff_t pos)
{
	return iomap->type != IOMAP_MAPPED ||
		(iomap->flags & IOMAP_F_NEW) ||
		pos >= i_size_read(inode);
}

static loff_t
iomap_readpage_actor(struct inode *inode, loff_t pos, loff_t length, void *data,
		struct iomap *iomap)
{
	struct iomap_readpage_ctx *ctx = data;
	struct page *page = ctx->cur_page;
	struct iomap_page *iop = iomap_page_create(inode, page);
	bool same_page = false, is_contig = false;
	loff_t orig_pos = pos;
	unsigned poff, plen;
	sector_t sector;

	if (iomap->type == IOMAP_INLINE) {
		WARN_ON_ONCE(pos);
		iomap_read_inline_data(inode, page, iomap);
		return PAGE_SIZE;
	}

	/* zero post-eof blocks as the page may be mapped */
	iomap_adjust_read_range(inode, iop, &pos, length, &poff, &plen);
	if (plen == 0)
		goto done;

	if (iomap_block_needs_zeroing(inode, iomap, pos)) {
		zero_user(page, poff, plen);
		iomap_set_range_uptodate(page, poff, plen);
		goto done;
	}

	ctx->cur_page_in_bio = true;

	/*
	 * Try to merge into a previous segment if we can.
	 */
	sector = iomap_sector(iomap, pos);
	if (ctx->bio && bio_end_sector(ctx->bio) == sector)
		is_contig = true;

	if (is_contig &&
	    __bio_try_merge_page(ctx->bio, page, plen, poff, &same_page)) {
		if (!same_page && iop)
			atomic_inc(&iop->read_count);
		goto done;
	}

	/*
	 * If we start a new segment we need to increase the read count, and we
	 * need to do so before submitting any previous full bio to make sure
	 * that we don't prematurely unlock the page.
	 */
	if (iop)
		atomic_inc(&iop->read_count);

	if (!ctx->bio || !is_contig || bio_full(ctx->bio, plen)) {
		gfp_t gfp = mapping_gfp_constraint(page->mapping, GFP_KERNEL);
		int nr_vecs = (length + PAGE_SIZE - 1) >> PAGE_SHIFT;

		if (ctx->bio)
			submit_bio(ctx->bio);

		if (ctx->is_readahead) /* same as readahead_gfp_mask */
			gfp |= __GFP_NORETRY | __GFP_NOWARN;
		ctx->bio = bio_alloc(gfp, min(BIO_MAX_PAGES, nr_vecs));
		ctx->bio->bi_opf = REQ_OP_READ;
		if (ctx->is_readahead)
			ctx->bio->bi_opf |= REQ_RAHEAD;
		ctx->bio->bi_iter.bi_sector = sector;
		bio_set_dev(ctx->bio, iomap->bdev);
		ctx->bio->bi_end_io = iomap_read_end_io;
	}

	bio_add_page(ctx->bio, page, plen, poff);
done:
	/*
	 * Move the caller beyond our range so that it keeps making progress.
	 * For that we have to include any leading non-uptodate ranges, but
	 * we can skip trailing ones as they will be handled in the next
	 * iteration.
	 */
	return pos - orig_pos + plen;
}

int
iomap_readpage(struct page *page, const struct iomap_ops *ops)
{
	struct iomap_readpage_ctx ctx = { .cur_page = page };
	struct inode *inode = page->mapping->host;
	unsigned poff;
	loff_t ret;

	trace_iomap_readpage(page->mapping->host, 1);

	for (poff = 0; poff < PAGE_SIZE; poff += ret) {
		ret = iomap_apply(inode, page_offset(page) + poff,
				PAGE_SIZE - poff, 0, ops, &ctx,
				iomap_readpage_actor);
		if (ret <= 0) {
			WARN_ON_ONCE(ret == 0);
			SetPageError(page);
			break;
		}
	}

	if (ctx.bio) {
		submit_bio(ctx.bio);
		WARN_ON_ONCE(!ctx.cur_page_in_bio);
	} else {
		WARN_ON_ONCE(ctx.cur_page_in_bio);
		unlock_page(page);
	}

	/*
	 * Just like mpage_readpages and block_read_full_page we always
	 * return 0 and just mark the page as PageError on errors.  This
	 * should be cleaned up all through the stack eventually.
	 */
	return 0;
}
EXPORT_SYMBOL_GPL(iomap_readpage);

static struct page *
iomap_next_page(struct inode *inode, struct list_head *pages, loff_t pos,
		loff_t length, loff_t *done)
{
	while (!list_empty(pages)) {
		struct page *page = lru_to_page(pages);

		if (page_offset(page) >= (u64)pos + length)
			break;

		list_del(&page->lru);
		if (!add_to_page_cache_lru(page, inode->i_mapping, page->index,
				GFP_NOFS))
			return page;

		/*
		 * If we already have a page in the page cache at index we are
		 * done.  Upper layers don't care if it is uptodate after the
		 * readpages call itself as every page gets checked again once
		 * actually needed.
		 */
		*done += PAGE_SIZE;
		put_page(page);
	}

	return NULL;
}

static loff_t
iomap_readpages_actor(struct inode *inode, loff_t pos, loff_t length,
		void *data, struct iomap *iomap)
{
	struct iomap_readpage_ctx *ctx = data;
	loff_t done, ret;

	for (done = 0; done < length; done += ret) {
		if (ctx->cur_page && offset_in_page(pos + done) == 0) {
			if (!ctx->cur_page_in_bio)
				unlock_page(ctx->cur_page);
			put_page(ctx->cur_page);
			ctx->cur_page = NULL;
		}
		if (!ctx->cur_page) {
			ctx->cur_page = iomap_next_page(inode, ctx->pages,
					pos, length, &done);
			if (!ctx->cur_page)
				break;
			ctx->cur_page_in_bio = false;
		}
		ret = iomap_readpage_actor(inode, pos + done, length - done,
				ctx, iomap);
	}

	return done;
}

int
iomap_readpages(struct address_space *mapping, struct list_head *pages,
		unsigned nr_pages, const struct iomap_ops *ops)
{
	struct iomap_readpage_ctx ctx = {
		.pages		= pages,
		.is_readahead	= true,
	};
	loff_t pos = page_offset(list_entry(pages->prev, struct page, lru));
	loff_t last = page_offset(list_entry(pages->next, struct page, lru));
	loff_t length = last - pos + PAGE_SIZE, ret = 0;

	trace_iomap_readpages(mapping->host, nr_pages);

	while (length > 0) {
		ret = iomap_apply(mapping->host, pos, length, 0, ops,
				&ctx, iomap_readpages_actor);
		if (ret <= 0) {
			WARN_ON_ONCE(ret == 0);
			goto done;
		}
		pos += ret;
		length -= ret;
	}
	ret = 0;
done:
	if (ctx.bio)
		submit_bio(ctx.bio);
	if (ctx.cur_page) {
		if (!ctx.cur_page_in_bio)
			unlock_page(ctx.cur_page);
		put_page(ctx.cur_page);
	}

	/*
	 * Check that we didn't lose a page due to the arcance calling
	 * conventions..
	 */
	WARN_ON_ONCE(!ret && !list_empty(ctx.pages));
	return ret;
}
EXPORT_SYMBOL_GPL(iomap_readpages);

/*
 * iomap_is_partially_uptodate checks whether blocks within a page are
 * uptodate or not.
 *
 * Returns true if all blocks which correspond to a file portion
 * we want to read within the page are uptodate.
 */
int
iomap_is_partially_uptodate(struct page *page, unsigned long from,
		unsigned long count)
{
	struct iomap_page *iop = to_iomap_page(page);
	struct inode *inode = page->mapping->host;
	unsigned len, first, last;
	unsigned i;

	/* Limit range to one page */
	len = min_t(unsigned, PAGE_SIZE - from, count);

	/* First and last blocks in range within page */
	first = from >> inode->i_blkbits;
	last = (from + len - 1) >> inode->i_blkbits;

	if (iop) {
		for (i = first; i <= last; i++)
			if (!test_bit(i, iop->uptodate))
				return 0;
		return 1;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(iomap_is_partially_uptodate);

int
iomap_releasepage(struct page *page, gfp_t gfp_mask)
{
	trace_iomap_releasepage(page->mapping->host, page, 0, 0);

	/*
	 * mm accommodates an old ext3 case where clean pages might not have had
	 * the dirty bit cleared. Thus, it can send actual dirty pages to
	 * ->releasepage() via shrink_active_list(), skip those here.
	 */
	if (PageDirty(page) || PageWriteback(page))
		return 0;
	iomap_page_release(page);
	return 1;
}
EXPORT_SYMBOL_GPL(iomap_releasepage);

void
iomap_invalidatepage(struct page *page, unsigned int offset, unsigned int len)
{
	trace_iomap_invalidatepage(page->mapping->host, page, offset, len);

	/*
	 * If we are invalidating the entire page, clear the dirty state from it
	 * and release it to avoid unnecessary buildup of the LRU.
	 */
	if (offset == 0 && len == PAGE_SIZE) {
		WARN_ON_ONCE(PageWriteback(page));
		cancel_dirty_page(page);
		iomap_page_release(page);
	}
}
EXPORT_SYMBOL_GPL(iomap_invalidatepage);

#ifdef CONFIG_MIGRATION
int
iomap_migrate_page(struct address_space *mapping, struct page *newpage,
		struct page *page, enum migrate_mode mode)
{
	int ret;

	ret = migrate_page_move_mapping(mapping, newpage, page, 0);
	if (ret != MIGRATEPAGE_SUCCESS)
		return ret;

	if (page_has_private(page)) {
		ClearPagePrivate(page);
		get_page(newpage);
		set_page_private(newpage, page_private(page));
		set_page_private(page, 0);
		put_page(page);
		SetPagePrivate(newpage);
	}

	if (mode != MIGRATE_SYNC_NO_COPY)
		migrate_page_copy(newpage, page);
	else
		migrate_page_states(newpage, page);
	return MIGRATEPAGE_SUCCESS;
}
EXPORT_SYMBOL_GPL(iomap_migrate_page);
#endif /* CONFIG_MIGRATION */

static void
iomap_write_failed(struct inode *inode, loff_t pos, unsigned len)
{
	loff_t i_size = i_size_read(inode);

	/*
	 * Only truncate newly allocated pages beyoned EOF, even if the
	 * write started inside the existing inode size.
	 */
	if (pos + len > i_size)
		truncate_pagecache_range(inode, max(pos, i_size), pos + len);
}

static int
iomap_read_page_sync(struct inode *inode, loff_t block_start, struct page *page,
		unsigned poff, unsigned plen, unsigned from, unsigned to,
		struct iomap *iomap)
{
	struct bio_vec bvec;
	struct bio bio;

	if (iomap_block_needs_zeroing(inode, iomap, block_start)) {
		zero_user_segments(page, poff, from, to, poff + plen);
		iomap_set_range_uptodate(page, poff, plen);
		return 0;
	}

	bio_init(&bio, &bvec, 1);
	bio.bi_opf = REQ_OP_READ;
	bio.bi_iter.bi_sector = iomap_sector(iomap, block_start);
	bio_set_dev(&bio, iomap->bdev);
	__bio_add_page(&bio, page, plen, poff);
	return submit_bio_wait(&bio);
}

static int
__iomap_write_begin(struct inode *inode, loff_t pos, unsigned len,
		struct page *page, struct iomap *iomap)
{
	struct iomap_page *iop = iomap_page_create(inode, page);
	loff_t block_size = i_blocksize(inode);
	loff_t block_start = pos & ~(block_size - 1);
	loff_t block_end = (pos + len + block_size - 1) & ~(block_size - 1);
	unsigned from = offset_in_page(pos), to = from + len, poff, plen;
	int status = 0;

	if (PageUptodate(page))
		return 0;

	do {
		iomap_adjust_read_range(inode, iop, &block_start,
				block_end - block_start, &poff, &plen);
		if (plen == 0)
			break;

		if ((from > poff && from < poff + plen) ||
		    (to > poff && to < poff + plen)) {
			status = iomap_read_page_sync(inode, block_start, page,
					poff, plen, from, to, iomap);
			if (status)
				break;
		}

	} while ((block_start += plen) < block_end);

	return status;
}

static int
iomap_write_begin(struct inode *inode, loff_t pos, unsigned len, unsigned flags,
		struct page **pagep, struct iomap *iomap)
{
	const struct iomap_page_ops *page_ops = iomap->page_ops;
	pgoff_t index = pos >> PAGE_SHIFT;
	struct page *page;
	int status = 0;

	BUG_ON(pos + len > iomap->offset + iomap->length);

	if (fatal_signal_pending(current))
		return -EINTR;

	if (page_ops && page_ops->page_prepare) {
		status = page_ops->page_prepare(inode, pos, len, iomap);
		if (status)
			return status;
	}

	page = grab_cache_page_write_begin(inode->i_mapping, index, flags);
	if (!page) {
		status = -ENOMEM;
		goto out_no_page;
	}

	if (iomap->type == IOMAP_INLINE)
		iomap_read_inline_data(inode, page, iomap);
	else if (iomap->flags & IOMAP_F_BUFFER_HEAD)
		status = __block_write_begin_int(page, pos, len, NULL, iomap);
	else
		status = __iomap_write_begin(inode, pos, len, page, iomap);

	if (unlikely(status))
		goto out_unlock;

	*pagep = page;
	return 0;

out_unlock:
	unlock_page(page);
	put_page(page);
	iomap_write_failed(inode, pos, len);

out_no_page:
	if (page_ops && page_ops->page_done)
		page_ops->page_done(inode, pos, 0, NULL, iomap);
	return status;
}

int
iomap_set_page_dirty(struct page *page)
{
	struct address_space *mapping = page_mapping(page);
	int newly_dirty;

	if (unlikely(!mapping))
		return !TestSetPageDirty(page);

	/*
	 * Lock out page->mem_cgroup migration to keep PageDirty
	 * synchronized with per-memcg dirty page counters.
	 */
	lock_page_memcg(page);
	newly_dirty = !TestSetPageDirty(page);
	if (newly_dirty)
		__set_page_dirty(page, mapping, 0);
	unlock_page_memcg(page);

	if (newly_dirty)
		__mark_inode_dirty(mapping->host, I_DIRTY_PAGES);
	return newly_dirty;
}
EXPORT_SYMBOL_GPL(iomap_set_page_dirty);

static int
__iomap_write_end(struct inode *inode, loff_t pos, unsigned len,
		unsigned copied, struct page *page, struct iomap *iomap)
{
	flush_dcache_page(page);

	/*
	 * The blocks that were entirely written will now be uptodate, so we
	 * don't have to worry about a readpage reading them and overwriting a
	 * partial write.  However if we have encountered a short write and only
	 * partially written into a block, it will not be marked uptodate, so a
	 * readpage might come in and destroy our partial write.
	 *
	 * Do the simplest thing, and just treat any short write to a non
	 * uptodate page as a zero-length write, and force the caller to redo
	 * the whole thing.
	 */
	if (unlikely(copied < len && !PageUptodate(page)))
		return 0;
	iomap_set_range_uptodate(page, offset_in_page(pos), len);
	iomap_set_page_dirty(page);
	return copied;
}

static int
iomap_write_end_inline(struct inode *inode, struct page *page,
		struct iomap *iomap, loff_t pos, unsigned copied)
{
	void *addr;

	WARN_ON_ONCE(!PageUptodate(page));
	BUG_ON(pos + copied > PAGE_SIZE - offset_in_page(iomap->inline_data));

	addr = kmap_atomic(page);
	memcpy(iomap->inline_data + pos, addr + pos, copied);
	kunmap_atomic(addr);

	mark_inode_dirty(inode);
	return copied;
}

static int
iomap_write_end(struct inode *inode, loff_t pos, unsigned len,
		unsigned copied, struct page *page, struct iomap *iomap)
{
	const struct iomap_page_ops *page_ops = iomap->page_ops;
	loff_t old_size = inode->i_size;
	int ret;

	if (iomap->type == IOMAP_INLINE) {
		ret = iomap_write_end_inline(inode, page, iomap, pos, copied);
	} else if (iomap->flags & IOMAP_F_BUFFER_HEAD) {
		ret = block_write_end(NULL, inode->i_mapping, pos, len, copied,
				page, NULL);
	} else {
		ret = __iomap_write_end(inode, pos, len, copied, page, iomap);
	}

	/*
	 * Update the in-memory inode size after copying the data into the page
	 * cache.  It's up to the file system to write the updated size to disk,
	 * preferably after I/O completion so that no stale data is exposed.
	 */
	if (pos + ret > old_size) {
		i_size_write(inode, pos + ret);
		iomap->flags |= IOMAP_F_SIZE_CHANGED;
	}
	unlock_page(page);

	if (old_size < pos)
		pagecache_isize_extended(inode, old_size, pos);
	if (page_ops && page_ops->page_done)
		page_ops->page_done(inode, pos, ret, page, iomap);
	put_page(page);

	if (ret < len)
		iomap_write_failed(inode, pos, len);
	return ret;
}

static loff_t
iomap_write_actor(struct inode *inode, loff_t pos, loff_t length, void *data,
		struct iomap *iomap)
{
	struct iov_iter *i = data;
	long status = 0;
	ssize_t written = 0;
	unsigned int flags = AOP_FLAG_NOFS;

	do {
		struct page *page;
		unsigned long offset;	/* Offset into pagecache page */
		unsigned long bytes;	/* Bytes to write to page */
		size_t copied;		/* Bytes copied from user */

		offset = offset_in_page(pos);
		bytes = min_t(unsigned long, PAGE_SIZE - offset,
						iov_iter_count(i));
again:
		if (bytes > length)
			bytes = length;

		/*
		 * Bring in the user page that we will copy from _first_.
		 * Otherwise there's a nasty deadlock on copying from the
		 * same page as we're writing to, without it being marked
		 * up-to-date.
		 *
		 * Not only is this an optimisation, but it is also required
		 * to check that the address is actually valid, when atomic
		 * usercopies are used, below.
		 */
		if (unlikely(iov_iter_fault_in_readable(i, bytes))) {
			status = -EFAULT;
			break;
		}

		status = iomap_write_begin(inode, pos, bytes, flags, &page,
				iomap);
		if (unlikely(status))
			break;

		if (mapping_writably_mapped(inode->i_mapping))
			flush_dcache_page(page);

		copied = iov_iter_copy_from_user_atomic(page, i, offset, bytes);

		flush_dcache_page(page);

		status = iomap_write_end(inode, pos, bytes, copied, page,
				iomap);
		if (unlikely(status < 0))
			break;
		copied = status;

		cond_resched();

		iov_iter_advance(i, copied);
		if (unlikely(copied == 0)) {
			/*
			 * If we were unable to copy any data at all, we must
			 * fall back to a single segment length write.
			 *
			 * If we didn't fallback here, we could livelock
			 * because not all segments in the iov can be copied at
			 * once without a pagefault.
			 */
			bytes = min_t(unsigned long, PAGE_SIZE - offset,
						iov_iter_single_seg_count(i));
			goto again;
		}
		pos += copied;
		written += copied;
		length -= copied;

		balance_dirty_pages_ratelimited(inode->i_mapping);
	} while (iov_iter_count(i) && length);

	return written ? written : status;
}

ssize_t
iomap_file_buffered_write(struct kiocb *iocb, struct iov_iter *iter,
		const struct iomap_ops *ops)
{
	struct inode *inode = iocb->ki_filp->f_mapping->host;
	loff_t pos = iocb->ki_pos, ret = 0, written = 0;

	while (iov_iter_count(iter)) {
		ret = iomap_apply(inode, pos, iov_iter_count(iter),
				IOMAP_WRITE, ops, iter, iomap_write_actor);
		if (ret <= 0)
			break;
		pos += ret;
		written += ret;
	}

	return written ? written : ret;
}
EXPORT_SYMBOL_GPL(iomap_file_buffered_write);

static struct page *
__iomap_read_page(struct inode *inode, loff_t offset)
{
	struct address_space *mapping = inode->i_mapping;
	struct page *page;

	page = read_mapping_page(mapping, offset >> PAGE_SHIFT, NULL);
	if (IS_ERR(page))
		return page;
	if (!PageUptodate(page)) {
		put_page(page);
		return ERR_PTR(-EIO);
	}
	return page;
}

static loff_t
iomap_dirty_actor(struct inode *inode, loff_t pos, loff_t length, void *data,
		struct iomap *iomap)
{
	long status = 0;
	ssize_t written = 0;

	do {
		struct page *page, *rpage;
		unsigned long offset;	/* Offset into pagecache page */
		unsigned long bytes;	/* Bytes to write to page */

		offset = offset_in_page(pos);
		bytes = min_t(loff_t, PAGE_SIZE - offset, length);

		rpage = __iomap_read_page(inode, pos);
		if (IS_ERR(rpage))
			return PTR_ERR(rpage);

		status = iomap_write_begin(inode, pos, bytes,
					   AOP_FLAG_NOFS, &page, iomap);
		put_page(rpage);
		if (unlikely(status))
			return status;

		WARN_ON_ONCE(!PageUptodate(page));

		status = iomap_write_end(inode, pos, bytes, bytes, page, iomap);
		if (unlikely(status <= 0)) {
			if (WARN_ON_ONCE(status == 0))
				return -EIO;
			return status;
		}

		cond_resched();

		pos += status;
		written += status;
		length -= status;

		balance_dirty_pages_ratelimited(inode->i_mapping);
	} while (length);

	return written;
}

int
iomap_file_dirty(struct inode *inode, loff_t pos, loff_t len,
		const struct iomap_ops *ops)
{
	loff_t ret;

	while (len) {
		ret = iomap_apply(inode, pos, len, IOMAP_WRITE, ops, NULL,
				iomap_dirty_actor);
		if (ret <= 0)
			return ret;
		pos += ret;
		len -= ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(iomap_file_dirty);

static int iomap_zero(struct inode *inode, loff_t pos, unsigned offset,
		unsigned bytes, struct iomap *iomap)
{
	struct page *page;
	int status;

	status = iomap_write_begin(inode, pos, bytes, AOP_FLAG_NOFS, &page,
				   iomap);
	if (status)
		return status;

	zero_user(page, offset, bytes);
	mark_page_accessed(page);

	return iomap_write_end(inode, pos, bytes, bytes, page, iomap);
}

static int iomap_dax_zero(loff_t pos, unsigned offset, unsigned bytes,
		struct iomap *iomap)
{
	return __dax_zero_page_range(iomap->bdev, iomap->dax_dev,
			iomap_sector(iomap, pos & PAGE_MASK), offset, bytes);
}

static loff_t
iomap_zero_range_actor(struct inode *inode, loff_t pos, loff_t count,
		void *data, struct iomap *iomap)
{
	bool *did_zero = data;
	loff_t written = 0;
	int status;

	/* already zeroed?  we're done. */
	if (iomap->type == IOMAP_HOLE || iomap->type == IOMAP_UNWRITTEN)
		return count;

	do {
		unsigned offset, bytes;

		offset = offset_in_page(pos);
		bytes = min_t(loff_t, PAGE_SIZE - offset, count);

		if (IS_DAX(inode))
			status = iomap_dax_zero(pos, offset, bytes, iomap);
		else
			status = iomap_zero(inode, pos, offset, bytes, iomap);
		if (status < 0)
			return status;

		pos += bytes;
		count -= bytes;
		written += bytes;
		if (did_zero)
			*did_zero = true;
	} while (count > 0);

	return written;
}

int
iomap_zero_range(struct inode *inode, loff_t pos, loff_t len, bool *did_zero,
		const struct iomap_ops *ops)
{
	loff_t ret;

	while (len > 0) {
		ret = iomap_apply(inode, pos, len, IOMAP_ZERO,
				ops, did_zero, iomap_zero_range_actor);
		if (ret <= 0)
			return ret;

		pos += ret;
		len -= ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(iomap_zero_range);

int
iomap_truncate_page(struct inode *inode, loff_t pos, bool *did_zero,
		const struct iomap_ops *ops)
{
	unsigned int blocksize = i_blocksize(inode);
	unsigned int off = pos & (blocksize - 1);

	/* Block boundary? Nothing to do */
	if (!off)
		return 0;
	return iomap_zero_range(inode, pos, blocksize - off, did_zero, ops);
}
EXPORT_SYMBOL_GPL(iomap_truncate_page);

static loff_t
iomap_page_mkwrite_actor(struct inode *inode, loff_t pos, loff_t length,
		void *data, struct iomap *iomap)
{
	struct page *page = data;
	int ret;

	if (iomap->flags & IOMAP_F_BUFFER_HEAD) {
		ret = __block_write_begin_int(page, pos, length, NULL, iomap);
		if (ret)
			return ret;
		block_commit_write(page, 0, length);
	} else {
		WARN_ON_ONCE(!PageUptodate(page));
		iomap_page_create(inode, page);
		set_page_dirty(page);
	}

	return length;
}

vm_fault_t iomap_page_mkwrite(struct vm_fault *vmf, const struct iomap_ops *ops)
{
	struct page *page = vmf->page;
	struct inode *inode = file_inode(vmf->vma->vm_file);
	unsigned long length;
	loff_t offset, size;
	ssize_t ret;

	lock_page(page);
	size = i_size_read(inode);
	if ((page->mapping != inode->i_mapping) ||
	    (page_offset(page) > size)) {
		/* We overload EFAULT to mean page got truncated */
		ret = -EFAULT;
		goto out_unlock;
	}

	/* page is wholly or partially inside EOF */
	if (((page->index + 1) << PAGE_SHIFT) > size)
		length = offset_in_page(size);
	else
		length = PAGE_SIZE;

	offset = page_offset(page);
	while (length > 0) {
		ret = iomap_apply(inode, offset, length,
				IOMAP_WRITE | IOMAP_FAULT, ops, page,
				iomap_page_mkwrite_actor);
		if (unlikely(ret <= 0))
			goto out_unlock;
		offset += ret;
		length -= ret;
	}

	wait_for_stable_page(page);
	return VM_FAULT_LOCKED;
out_unlock:
	unlock_page(page);
	return block_page_mkwrite_return(ret);
}
EXPORT_SYMBOL_GPL(iomap_page_mkwrite);

static void
iomap_finish_page_writeback(struct inode *inode, struct bio_vec *bvec,
		int error)
{
	struct iomap_page *iop = to_iomap_page(bvec->bv_page);

	if (error) {
		SetPageError(bvec->bv_page);
		mapping_set_error(inode->i_mapping, -EIO);
	}

	WARN_ON_ONCE(i_blocksize(inode) < PAGE_SIZE && !iop);
	WARN_ON_ONCE(iop && atomic_read(&iop->write_count) <= 0);

	if (!iop || atomic_dec_and_test(&iop->write_count))
		end_page_writeback(bvec->bv_page);
}

/*
 * We're now finished for good with this ioend structure.  Update the page
 * state, release holds on bios, and finally free up memory.  Do not use the
 * ioend after this.
 */
static void
iomap_finish_ioend(struct iomap_ioend *ioend, int error)
{
	struct inode *inode = ioend->io_inode;
	struct bio *bio = &ioend->io_inline_bio;
	struct bio *last = ioend->io_bio, *next;
	u64 start = bio->bi_iter.bi_sector;
	bool quiet = bio_flagged(bio, BIO_QUIET);

	for (bio = &ioend->io_inline_bio; bio; bio = next) {
		struct bio_vec	*bvec;
		struct bvec_iter_all iter_all;

		/*
		 * For the last bio, bi_private points to the ioend, so we
		 * need to explicitly end the iteration here.
		 */
		if (bio == last)
			next = NULL;
		else
			next = bio->bi_private;

		/* walk each page on bio, ending page IO on them */
		bio_for_each_segment_all(bvec, bio, iter_all)
			iomap_finish_page_writeback(inode, bvec, error);
		bio_put(bio);
	}

	if (unlikely(error && !quiet)) {
		printk_ratelimited(KERN_ERR
			"%s: writeback error on sector %llu",
			inode->i_sb->s_id, start);
	}
}

void
iomap_finish_ioends(struct iomap_ioend *ioend, int error)
{
	struct list_head tmp;

	list_replace_init(&ioend->io_list, &tmp);
	iomap_finish_ioend(ioend, error);
	while ((ioend = list_pop_entry(&tmp, struct iomap_ioend, io_list)))
		iomap_finish_ioend(ioend, error);
}
EXPORT_SYMBOL_GPL(iomap_finish_ioends);

/*
 * We can merge two adjacent ioends if they have the same set of work to do.
 */
static bool
iomap_ioend_can_merge(struct iomap_ioend *ioend, struct iomap_ioend *next)
{
	if (ioend->io_bio->bi_status != next->io_bio->bi_status)
		return false;
	if ((ioend->io_flags & IOMAP_F_SHARED) ^
	    (next->io_flags & IOMAP_F_SHARED))
		return false;
	if ((ioend->io_type == IOMAP_UNWRITTEN) ^
	    (next->io_type == IOMAP_UNWRITTEN))
		return false;
	if (ioend->io_offset + ioend->io_size != next->io_offset)
		return false;
	return true;
}

void
iomap_ioend_try_merge(struct iomap_ioend *ioend, struct list_head *more_ioends,
		void (*merge_private)(struct iomap_ioend *ioend,
				struct iomap_ioend *next))
{
	struct iomap_ioend *next;

	INIT_LIST_HEAD(&ioend->io_list);

	while ((next = list_first_entry_or_null(more_ioends, struct iomap_ioend,
			io_list))) {
		if (!iomap_ioend_can_merge(ioend, next))
			break;
		list_move_tail(&next->io_list, &ioend->io_list);
		ioend->io_size += next->io_size;
		if (next->io_private && merge_private)
			merge_private(ioend, next);
	}
}
EXPORT_SYMBOL_GPL(iomap_ioend_try_merge);

static int
iomap_ioend_compare(void *priv, struct list_head *a, struct list_head *b)
{
	struct iomap_ioend *ia, *ib;

	ia = container_of(a, struct iomap_ioend, io_list);
	ib = container_of(b, struct iomap_ioend, io_list);
	if (ia->io_offset < ib->io_offset)
		return -1;
	else if (ia->io_offset > ib->io_offset)
		return 1;
	return 0;
}

void
iomap_sort_ioends(struct list_head *ioend_list)
{
	list_sort(NULL, ioend_list, iomap_ioend_compare);
}
EXPORT_SYMBOL_GPL(iomap_sort_ioends);

static void iomap_writepage_end_bio(struct bio *bio)
{
	struct iomap_ioend *ioend = bio->bi_private;

	iomap_finish_ioend(ioend, blk_status_to_errno(bio->bi_status));
}

/*
 * Submit the bio for an ioend. We are passed an ioend with a bio attached to
 * it, and we submit that bio. The ioend may be used for multiple bio
 * submissions, so we only want to allocate an append transaction for the ioend
 * once. In the case of multiple bio submission, each bio will take an IO
 * reference to the ioend to ensure that the ioend completion is only done once
 * all bios have been submitted and the ioend is really done.
 *
 * If @error is non-zero, it means that we have a situation where some part of
 * the submission process has failed after we have marked pages for writeback
 * and unlocked them. In this situation, we need to fail the bio and ioend
 * rather than submit it to IO. This typically only happens on a filesystem
 * shutdown.
 */
static int
iomap_submit_ioend(struct iomap_writepage_ctx *wpc, struct iomap_ioend *ioend,
		int error)
{
	ioend->io_bio->bi_private = ioend;
	ioend->io_bio->bi_end_io = iomap_writepage_end_bio;

	/*
	 * File systems can perform actions at submit time and/or override
	 * the end_io handler here for complex operations like copy on write
	 * extent manipulation or unwritten extent conversions.
	 */
	if (wpc->ops->submit_ioend)
		error = wpc->ops->submit_ioend(ioend, error);
	if (error) {
		/*
		 * If we are failing the IO now, just mark the ioend with an
		 * error and finish it.  This will run IO completion immediately
		 * as there is only one reference to the ioend at this point in
		 * time.
		 */
		ioend->io_bio->bi_status = errno_to_blk_status(error);
		bio_endio(ioend->io_bio);
		return error;
	}

	submit_bio(ioend->io_bio);
	return 0;
}

static struct iomap_ioend *
iomap_alloc_ioend(struct inode *inode, struct iomap_writepage_ctx *wpc,
		loff_t offset, sector_t sector, struct writeback_control *wbc)
{
	struct iomap_ioend *ioend;
	struct bio *bio;

	bio = bio_alloc_bioset(GFP_NOFS, BIO_MAX_PAGES, &iomap_ioend_bioset);
	bio_set_dev(bio, wpc->iomap.bdev);
	bio->bi_iter.bi_sector = sector;
	bio->bi_opf = REQ_OP_WRITE | wbc_to_write_flags(wbc);
	bio->bi_write_hint = inode->i_write_hint;
	wbc_init_bio(wbc, bio);

	ioend = container_of(bio, struct iomap_ioend, io_inline_bio);
	INIT_LIST_HEAD(&ioend->io_list);
	ioend->io_type = wpc->iomap.type;
	ioend->io_flags = wpc->iomap.flags;
	ioend->io_inode = inode;
	ioend->io_size = 0;
	ioend->io_offset = offset;
	ioend->io_private = NULL;
	ioend->io_bio = bio;
	return ioend;
}

/*
 * Allocate a new bio, and chain the old bio to the new one.
 *
 * Note that we have to do perform the chaining in this unintuitive order
 * so that the bi_private linkage is set up in the right direction for the
 * traversal in iomap_finish_ioend().
 */
static struct bio *
iomap_chain_bio(struct bio *prev)
{
	struct bio *new;

	new = bio_alloc(GFP_NOFS, BIO_MAX_PAGES);
	bio_copy_dev(new, prev);/* also copies over blkcg information */
	new->bi_iter.bi_sector = bio_end_sector(prev);
	new->bi_opf = prev->bi_opf;
	new->bi_write_hint = prev->bi_write_hint;

	bio_chain(prev, new);
	bio_get(prev);		/* for iomap_finish_ioend */
	submit_bio(prev);
	return new;
}

/*
 * Test to see if we have an existing ioend structure that we could append to
 * first, otherwise finish off the current ioend and start another.
 */
static void
iomap_add_to_ioend(struct inode *inode, loff_t offset, struct page *page,
		struct iomap_page *iop, struct iomap_writepage_ctx *wpc,
		struct writeback_control *wbc, struct list_head *iolist)
{
	sector_t sector = iomap_sector(&wpc->iomap, offset);
	unsigned len = i_blocksize(inode);
	unsigned poff = offset & (PAGE_SIZE - 1);
	bool merged, same_page = false;

	if (!wpc->ioend ||
	    (wpc->iomap.flags & IOMAP_F_SHARED) !=
	    (wpc->ioend->io_flags & IOMAP_F_SHARED) ||
	    wpc->iomap.type != wpc->ioend->io_type ||
	    sector != bio_end_sector(wpc->ioend->io_bio) ||
	    offset != wpc->ioend->io_offset + wpc->ioend->io_size) {
		if (wpc->ioend)
			list_add(&wpc->ioend->io_list, iolist);
		wpc->ioend = iomap_alloc_ioend(inode, wpc, offset, sector, wbc);
	}

	merged = __bio_try_merge_page(wpc->ioend->io_bio, page, len, poff,
			&same_page);

	if (iop && !same_page)
		atomic_inc(&iop->write_count);
	if (!merged) {
		if (bio_full(wpc->ioend->io_bio, len)) {
			wpc->ioend->io_bio =
				iomap_chain_bio(wpc->ioend->io_bio);
		}
		bio_add_page(wpc->ioend->io_bio, page, len, poff);
	}

	wpc->ioend->io_size += len;
	wbc_account_cgroup_owner(wbc, page, len);
}

/*
 * We implement an immediate ioend submission policy here to avoid needing to
 * chain multiple ioends and hence nest mempool allocations which can violate
 * forward progress guarantees we need to provide. The current ioend we are
 * adding blocks to is cached on the writepage context, and if the new block
 * does not append to the cached ioend it will create a new ioend and cache that
 * instead.
 *
 * If a new ioend is created and cached, the old ioend is returned and queued
 * locally for submission once the entire page is processed or an error has been
 * detected.  While ioends are submitted immediately after they are completed,
 * batching optimisations are provided by higher level block plugging.
 *
 * At the end of a writeback pass, there will be a cached ioend remaining on the
 * writepage context that the caller will need to submit.
 */
static int
iomap_writepage_map(struct iomap_writepage_ctx *wpc,
		struct writeback_control *wbc, struct inode *inode,
		struct page *page, u64 end_offset)
{
	struct iomap_page *iop = to_iomap_page(page);
	struct iomap_ioend *ioend, *next;
	unsigned len = i_blocksize(inode);
	u64 file_offset; /* file offset of page */
	int error = 0, count = 0, i;
	LIST_HEAD(submit_list);

	WARN_ON_ONCE(i_blocksize(inode) < PAGE_SIZE && !iop);
	WARN_ON_ONCE(iop && atomic_read(&iop->write_count) != 0);

	/*
	 * Walk through the page to find areas to write back. If we run off the
	 * end of the current map or find the current map invalid, grab a new
	 * one.
	 */
	for (i = 0, file_offset = page_offset(page);
	     i < (PAGE_SIZE >> inode->i_blkbits) && file_offset < end_offset;
	     i++, file_offset += len) {
		if (iop && !test_bit(i, iop->uptodate))
			continue;

		error = wpc->ops->map_blocks(wpc, inode, file_offset);
		if (error)
			break;
		if (WARN_ON_ONCE(wpc->iomap.type == IOMAP_INLINE))
			continue;
		if (wpc->iomap.type == IOMAP_HOLE)
			continue;
		iomap_add_to_ioend(inode, file_offset, page, iop, wpc, wbc,
				 &submit_list);
		count++;
	}

	WARN_ON_ONCE(!wpc->ioend && !list_empty(&submit_list));
	WARN_ON_ONCE(!PageLocked(page));
	WARN_ON_ONCE(PageWriteback(page));

	/*
	 * On error, we have to fail the ioend here because we may have set
	 * pages under writeback, we have to make sure we run IO completion to
	 * mark the error state of the IO appropriately, so we can't cancel the
	 * ioend directly here.  That means we have to mark this page as under
	 * writeback if we included any blocks from it in the ioend chain so
	 * that completion treats it correctly.
	 *
	 * If we didn't include the page in the ioend, the on error we can
	 * simply discard and unlock it as there are no other users of the page
	 * now.  The caller will still need to trigger submission of outstanding
	 * ioends on the writepage context so they are treated correctly on
	 * error.
	 */
	if (unlikely(error)) {
		if (!count) {
			if (wpc->ops->discard_page)
				wpc->ops->discard_page(page);
			ClearPageUptodate(page);
			unlock_page(page);
			goto done;
		}

		/*
		 * If the page was not fully cleaned, we need to ensure that the
		 * higher layers come back to it correctly.  That means we need
		 * to keep the page dirty, and for WB_SYNC_ALL writeback we need
		 * to ensure the PAGECACHE_TAG_TOWRITE index mark is not removed
		 * so another attempt to write this page in this writeback sweep
		 * will be made.
		 */
		set_page_writeback_keepwrite(page);
	} else {
		clear_page_dirty_for_io(page);
		set_page_writeback(page);
	}

	unlock_page(page);

	/*
	 * Preserve the original error if there was one, otherwise catch
	 * submission errors here and propagate into subsequent ioend
	 * submissions.
	 */
	list_for_each_entry_safe(ioend, next, &submit_list, io_list) {
		int error2;

		list_del_init(&ioend->io_list);
		error2 = iomap_submit_ioend(wpc, ioend, error);
		if (error2 && !error)
			error = error2;
	}

	/*
	 * We can end up here with no error and nothing to write only if we race
	 * with a partial page truncate on a sub-page block sized filesystem.
	 */
	if (!count)
		end_page_writeback(page);
done:
	mapping_set_error(page->mapping, error);
	return error;
}

/*
 * Write out a dirty page.
 *
 * For delalloc space on the page we need to allocate space and flush it.
 * For unwritten space on the page we need to start the conversion to
 * regular allocated space.
 */
static int
iomap_do_writepage(struct page *page, struct writeback_control *wbc, void *data)
{
	struct iomap_writepage_ctx *wpc = data;
	struct inode *inode = page->mapping->host;
	pgoff_t end_index;
	u64 end_offset;
	loff_t offset;

	trace_iomap_writepage(inode, page, 0, 0);

	/*
	 * Refuse to write the page out if we are called from reclaim context.
	 *
	 * This avoids stack overflows when called from deeply used stacks in
	 * random callers for direct reclaim or memcg reclaim.  We explicitly
	 * allow reclaim from kswapd as the stack usage there is relatively low.
	 *
	 * This should never happen except in the case of a VM regression so
	 * warn about it.
	 */
	if (WARN_ON_ONCE((current->flags & (PF_MEMALLOC|PF_KSWAPD)) ==
			PF_MEMALLOC))
		goto redirty;

	/*
	 * Given that we do not allow direct reclaim to call us, we should
	 * never be called while in a filesystem transaction.
	 */
	if (WARN_ON_ONCE(current->flags & PF_MEMALLOC_NOFS))
		goto redirty;

	/*
	 * Is this page beyond the end of the file?
	 *
	 * The page index is less than the end_index, adjust the end_offset
	 * to the highest offset that this page should represent.
	 * -----------------------------------------------------
	 * |			file mapping	       | <EOF> |
	 * -----------------------------------------------------
	 * | Page ... | Page N-2 | Page N-1 |  Page N  |       |
	 * ^--------------------------------^----------|--------
	 * |     desired writeback range    |      see else    |
	 * ---------------------------------^------------------|
	 */
	offset = i_size_read(inode);
	end_index = offset >> PAGE_SHIFT;
	if (page->index < end_index)
		end_offset = (loff_t)(page->index + 1) << PAGE_SHIFT;
	else {
		/*
		 * Check whether the page to write out is beyond or straddles
		 * i_size or not.
		 * -------------------------------------------------------
		 * |		file mapping		        | <EOF>  |
		 * -------------------------------------------------------
		 * | Page ... | Page N-2 | Page N-1 |  Page N   | Beyond |
		 * ^--------------------------------^-----------|---------
		 * |				    |      Straddles     |
		 * ---------------------------------^-----------|--------|
		 */
		unsigned offset_into_page = offset & (PAGE_SIZE - 1);

		/*
		 * Skip the page if it is fully outside i_size, e.g. due to a
		 * truncate operation that is in progress. We must redirty the
		 * page so that reclaim stops reclaiming it. Otherwise
		 * iomap_vm_releasepage() is called on it and gets confused.
		 *
		 * Note that the end_index is unsigned long, it would overflow
		 * if the given offset is greater than 16TB on 32-bit system
		 * and if we do check the page is fully outside i_size or not
		 * via "if (page->index >= end_index + 1)" as "end_index + 1"
		 * will be evaluated to 0.  Hence this page will be redirtied
		 * and be written out repeatedly which would result in an
		 * infinite loop, the user program that perform this operation
		 * will hang.  Instead, we can verify this situation by checking
		 * if the page to write is totally beyond the i_size or if it's
		 * offset is just equal to the EOF.
		 */
		if (page->index > end_index ||
		    (page->index == end_index && offset_into_page == 0))
			goto redirty;

		/*
		 * The page straddles i_size.  It must be zeroed out on each
		 * and every writepage invocation because it may be mmapped.
		 * "A file is mapped in multiples of the page size.  For a file
		 * that is not a multiple of the page size, the remaining
		 * memory is zeroed when mapped, and writes to that region are
		 * not written out to the file."
		 */
		zero_user_segment(page, offset_into_page, PAGE_SIZE);

		/* Adjust the end_offset to the end of file */
		end_offset = offset;
	}

	return iomap_writepage_map(wpc, wbc, inode, page, end_offset);

redirty:
	redirty_page_for_writepage(wbc, page);
	unlock_page(page);
	return 0;
}

int
iomap_writepage(struct page *page, struct writeback_control *wbc,
		struct iomap_writepage_ctx *wpc,
		const struct iomap_writeback_ops *ops)
{
	int ret;

	wpc->ops = ops;
	ret = iomap_do_writepage(page, wbc, wpc);
	if (!wpc->ioend)
		return ret;
	return iomap_submit_ioend(wpc, wpc->ioend, ret);
}
EXPORT_SYMBOL_GPL(iomap_writepage);

int
iomap_writepages(struct address_space *mapping, struct writeback_control *wbc,
		struct iomap_writepage_ctx *wpc,
		const struct iomap_writeback_ops *ops)
{
	int			ret;

	wpc->ops = ops;
	ret = write_cache_pages(mapping, wbc, iomap_do_writepage, wpc);
	if (!wpc->ioend)
		return ret;
	return iomap_submit_ioend(wpc, wpc->ioend, ret);
}
EXPORT_SYMBOL_GPL(iomap_writepages);

static int __init iomap_init(void)
{
	return bioset_init(&iomap_ioend_bioset, 4 * (PAGE_SIZE / SECTOR_SIZE),
			   offsetof(struct iomap_ioend, io_inline_bio),
			   BIOSET_NEED_BVECS);
}
fs_initcall(iomap_init);
