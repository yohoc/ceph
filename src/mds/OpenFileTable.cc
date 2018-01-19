// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2018 Red Hat
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "mds/CInode.h"
#include "mds/MDSRank.h"
#include "mds/MDCache.h"
#include "osdc/Objecter.h"
#include "OpenFileTable.h"

#include "common/config.h"
#include "common/errno.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_mds
#undef dout_prefix
#define dout_prefix _prefix(_dout, mds)
static ostream& _prefix(std::ostream *_dout, MDSRank *mds) {
  return *_dout << "mds." << mds->get_nodeid() << ".openfiles ";
}

void OpenFileTable::get_ref(CInode *in)
{
  do {
    auto p = anchor_map.find(in->ino());
    if (p != anchor_map.end()) {
      assert(in->state_test(CInode::STATE_TRACKEDBYOFT));
      assert(p->second.nref > 0);
      p->second.nref++;
      break;
    }

    CDentry *dn = in->get_parent_dn();
    CInode *pin = dn ? dn->get_dir()->get_inode() : nullptr;

    auto ret = anchor_map.emplace(std::piecewise_construct, std::forward_as_tuple(in->ino()),
				  std::forward_as_tuple(in->ino(), (pin ? pin->ino() : inodeno_t(0)),
				  (dn ? dn->get_name() : string()), in->d_type(), 1));
    assert(ret.second == true);
    in->state_set(CInode::STATE_TRACKEDBYOFT);

    dirty_items.emplace(in->ino(), DIRTY_NEW);

    in = pin;
  } while (in);
}

void OpenFileTable::put_ref(CInode *in)
{
  do {
    assert(in->state_test(CInode::STATE_TRACKEDBYOFT));
    auto p = anchor_map.find(in->ino());
    assert(p != anchor_map.end());
    assert(p->second.nref > 0);

    if (p->second.nref > 1) {
      p->second.nref--;
      break;
    }

    CDentry *dn = in->get_parent_dn();
    CInode *pin = dn ? dn->get_dir()->get_inode() : nullptr;
    if (dn) {
      assert(p->second.dirino == pin->ino());
      assert(p->second.d_name == dn->get_name());
    } else {
      assert(p->second.dirino == inodeno_t(0));
      assert(p->second.d_name == "");
    }

    anchor_map.erase(p);
    in->state_clear(CInode::STATE_TRACKEDBYOFT);

    auto ret = dirty_items.emplace(in->ino(), 0);
    if (!ret.second && (ret.first->second & DIRTY_NEW))
      dirty_items.erase(ret.first);

    in = pin;
  } while (in);
}

void OpenFileTable::add_inode(CInode *in)
{
  dout(10) << __func__ << " " << *in << dendl;
  if (!in->is_dir()) {
    auto p = anchor_map.find(in->ino());
    assert(p == anchor_map.end());
  }
  get_ref(in);
}

void OpenFileTable::remove_inode(CInode *in)
{
  dout(10) << __func__ << " " << *in << dendl;
  if (!in->is_dir()) {
    auto p = anchor_map.find(in->ino());
    assert(p != anchor_map.end());
    assert(p->second.nref == 1);
  }
  put_ref(in);
}

void OpenFileTable::notify_link(CInode *in)
{
  dout(10) << __func__ << " " << *in << dendl;
  auto p = anchor_map.find(in->ino());
  assert(p != anchor_map.end());
  assert(p->second.nref > 0);
  assert(p->second.dirino == inodeno_t(0));
  assert(p->second.d_name == "");

  CDentry *dn = in->get_parent_dn();
  CInode *pin = dn->get_dir()->get_inode();

  p->second.dirino = pin->ino();
  p->second.d_name = dn->get_name();
  dirty_items.emplace(in->ino(), 0);

  get_ref(pin);
}

void OpenFileTable::notify_unlink(CInode *in)
{
  dout(10) << __func__ << " " << *in << dendl;
  auto p = anchor_map.find(in->ino());
  assert(p != anchor_map.end());
  assert(p->second.nref > 0);

  CDentry *dn = in->get_parent_dn();
  CInode *pin = dn->get_dir()->get_inode();
  assert(p->second.dirino == pin->ino());
  assert(p->second.d_name == dn->get_name());

  p->second.dirino = inodeno_t(0);
  p->second.d_name = "";
  dirty_items.emplace(in->ino(), 0);

  put_ref(pin);
}

object_t OpenFileTable::get_object_name() const
{
  char s[30];
  snprintf(s, sizeof(s), "mds%d_openfiles", int(mds->get_nodeid()));
  return object_t(s);
}

class C_IO_OFT_Save : public MDSIOContextBase {
protected:
  OpenFileTable *oft;
  uint64_t log_seq;
  MDSInternalContextBase *fin;
  MDSRank *get_mds() override { return oft->mds; }
public:
  C_IO_OFT_Save(OpenFileTable *t, uint64_t s, MDSInternalContextBase *c) :
    oft(t), log_seq(s), fin(c) {}
  void finish(int r) {
    oft->_commit_finish(r, log_seq, fin);
  }
};

void OpenFileTable::_commit_finish(int r, uint64_t log_seq, MDSInternalContextBase *fin)
{
  dout(10) << __func__ << " log_seq " << log_seq << dendl;
  if (r < 0) {
    mds->handle_write_error(r);
    return;
  }

  assert(log_seq <= committing_log_seq);
  assert(log_seq >= committed_log_seq);
  committed_log_seq = log_seq;
  num_pending_commit--;

  if (fin)
    fin->complete(r);
}

void OpenFileTable::commit(MDSInternalContextBase *c, uint64_t log_seq, int op_prio)
{
  dout(10) << __func__ << " log_seq " << log_seq << dendl;
  const unsigned max_write_size = mds->mdcache->max_dir_commit_size;

  assert(log_seq >= committing_log_seq);
  committing_log_seq = log_seq;

  C_GatherBuilder gather(g_ceph_context,
			 new C_OnFinisher(new C_IO_OFT_Save(this, log_seq, c),
					  mds->finisher));

  SnapContext snapc;
  object_t oid = get_object_name();
  object_locator_t oloc(mds->mdsmap->get_metadata_pool());

  bool first = true;
  unsigned write_size = 0;
  std::map<string, bufferlist> to_update;
  std::set<string> to_remove;

  auto commit_func = [&](bool last) {
    ObjectOperation op;
    op.priority = op_prio;

    if (clear_on_commit) {
      op.omap_clear();
      op.set_last_op_flags(CEPH_OSD_OP_FLAG_FAILOK);
      clear_on_commit = false;
    }

    if (last) {
      bufferlist header;
      encode(log_seq, header);
      op.omap_set_header(header);
    } else if (first) {
      // make incomplete
      bufferlist header;
      encode((uint64_t)0, header);
      op.omap_set_header(header);
    }

    if (!to_update.empty())
      op.omap_set(to_update);
    if (!to_remove.empty())
      op.omap_rm_keys(to_remove);

    mds->objecter->mutate(oid, oloc, op, snapc, ceph::real_clock::now(), 0,
			  gather.new_sub());

    first = false;
    write_size = 0;
    to_update.clear();
    to_remove.clear();
  };

  bool first_commit = !loaded_anchor_map.empty();

  using ceph::encode;
  for (auto& it : dirty_items) {
    auto p = anchor_map.find(it.first);
    if (first_commit) {
      auto q = loaded_anchor_map.find(it.first);
      if (q != loaded_anchor_map.end()) {
	bool same = (p != anchor_map.end() && p->second == q->second);
	loaded_anchor_map.erase(q);
	if (same)
	  continue;
      }
    }

    char key[32];
    int len = snprintf(key, sizeof(key), "%llx", (unsigned long long)it.first.val);
    write_size += len + sizeof(__u32);

    if (p != anchor_map.end()) {
      bufferlist bl;
      encode(p->second, bl);

      write_size += bl.length() + sizeof(__u32);

      to_update[key].swap(bl);
    } else {
      to_remove.emplace(key);
    }

    if (write_size >= max_write_size) {
      commit_func(false);
    }
  }
  dirty_items.clear();

  if (first_commit) {
    for (auto& it : loaded_anchor_map) {
      char key[32];
      int len = snprintf(key, sizeof(key), "%llx", (unsigned long long)it.first.val);
      write_size += len + sizeof(__u32);
      to_remove.emplace(key);

      if (write_size >= max_write_size) {
	commit_func(false);
      }
    }
    loaded_anchor_map.clear();
  }

  commit_func(true);

  num_pending_commit++;
  gather.activate();
}

class C_IO_OFT_Load : public MDSIOContextBase {
protected:
  OpenFileTable *oft;
  MDSRank *get_mds() override { return oft->mds; }

public:
  int header_r = 0;  //< Return value from OMAP header read
  int values_r = 0;  //< Return value from OMAP value read
  bufferlist header_bl;
  std::map<std::string, bufferlist> values;
  bool more = false;
  bool first;

  C_IO_OFT_Load(OpenFileTable *t, bool f) : oft(t), first(f) {}
  void finish(int r) {
    oft->_load_finish(r, header_r, values_r, first, more, header_bl, values);
  }
};

void OpenFileTable::_load_finish(int op_r, int header_r, int values_r,
				 bool first, bool more,
				 bufferlist &header_bl,
				 std::map<std::string, bufferlist> &values)
{
  if (op_r < 0) {
    derr << __func__ << " got " << cpp_strerror(op_r) << dendl;
    clear_on_commit = true;
    if (!first)
      loaded_anchor_map.clear();
    goto out;
  }

  try {
    using ceph::decode;
    if (first) {
      bufferlist::iterator p = header_bl.begin();
      uint64_t log_seq;
      decode(log_seq, p);
      committed_log_seq = committing_log_seq = log_seq;
      if (log_seq == 0) {
	dout(1) << __func__ << ": incomplete values" << dendl;
	goto out;
      }
    }

    for (auto& it : values) {
      inodeno_t ino;
      sscanf(it.first.c_str(), "%llx", (unsigned long long*)&ino.val);

      auto r = loaded_anchor_map.emplace_hint(loaded_anchor_map.end(),
					      std::piecewise_construct,
					      std::make_tuple(ino),
					      std::make_tuple());
      Anchor& anchor = r->second;

      bufferlist::iterator p = it.second.begin();
      decode(anchor, p);
      assert(ino == anchor.ino);
      anchor.auth = MDS_RANK_NONE;
    }
  } catch (buffer::error &e) {
    derr << __func__ << ": corrupted header/values: " << e.what() << dendl;
    clear_on_commit = true;
    loaded_anchor_map.clear();
    goto out;
  }

  if (more) {
    // Issue another read if we're not at the end of the omap
    const std::string last_key = values.rbegin()->first;
    dout(10) << __func__ << ": continue to load from '" << last_key << "'" << dendl;
    object_t oid = get_object_name();
    object_locator_t oloc(mds->mdsmap->get_metadata_pool());
    C_IO_OFT_Load *c = new C_IO_OFT_Load(this, false);
    ObjectOperation op;
    op.omap_get_vals(last_key, "", uint64_t(-1),
		     &c->values, &c->more, &c->values_r);
    mds->objecter->read(oid, oloc, op, CEPH_NOSNAP, nullptr, 0,
			new C_OnFinisher(c, mds->finisher));
    return;
  }

  dout(10) << __func__ << ": load complete" << dendl;
out:
  load_done = true;
  finish_contexts(g_ceph_context, waiting_for_load);
  waiting_for_load.clear();
}

void OpenFileTable::load(MDSInternalContextBase *onload)
{
  dout(10) << __func__ << dendl;
  assert(!load_done);
  if (onload)
    waiting_for_load.push_back(onload);

  C_IO_OFT_Load *c = new C_IO_OFT_Load(this, true);
  object_t oid = get_object_name();
  object_locator_t oloc(mds->mdsmap->get_metadata_pool());

  ObjectOperation op;
  op.omap_get_header(&c->header_bl, &c->header_r);
  op.omap_get_vals("", "", uint64_t(-1),
		   &c->values, &c->more, &c->values_r);

  mds->objecter->read(oid, oloc, op, CEPH_NOSNAP, nullptr, 0,
		      new C_OnFinisher(c, mds->finisher));
}

bool OpenFileTable::get_ancestors(inodeno_t ino, vector<inode_backpointer_t>& ancestors,
				  mds_rank_t& auth_hint)
{
  auto p = loaded_anchor_map.find(ino);
  if (p == loaded_anchor_map.end())
    return false;

  inodeno_t dirino = p->second.dirino;
  if (dirino == inodeno_t(0))
    return false;

  bool first = true;
  ancestors.clear();
  while (true) {
    ancestors.push_back(inode_backpointer_t(dirino, p->second.d_name, 0));

    p = loaded_anchor_map.find(dirino);
    if (p == loaded_anchor_map.end())
      break;

    if (first)
      auth_hint = p->second.auth;

    dirino = p->second.dirino;
    if (dirino == inodeno_t(0))
      break;

    first = false;
  }
  return true;
}

class C_OFT_OpenInoFinish: public MDSInternalContextBase {
  OpenFileTable *oft;
  inodeno_t ino;
  MDSRank *get_mds() override { return oft->mds; }
public:
  C_OFT_OpenInoFinish(OpenFileTable *t, inodeno_t i) : oft(t), ino(i) {}
  void finish(int r) override {
    oft->_open_ino_finish(ino, r);
  }
};

void OpenFileTable::_open_ino_finish(inodeno_t ino, int r)
{
  if (prefetch_state == DIR_INODES && r >= 0 && ino != inodeno_t(0)) {
    auto p = loaded_anchor_map.find(ino);
    assert(p != loaded_anchor_map.end());
    p->second.auth = mds_rank_t(r);
  }

  if (r != mds->get_nodeid())
    mds->mdcache->rejoin_prefetch_ino_finish(ino, r);

  num_opening_inodes--;
  if (num_opening_inodes == 0) {
    if (prefetch_state == DIR_INODES)  {
      prefetch_state = FILE_INODES;
      _prefetch_inodes();
    } else if (prefetch_state == FILE_INODES) {
      prefetch_state = DONE;
      finish_contexts(g_ceph_context, waiting_for_prefetch);
      waiting_for_prefetch.clear();
    } else {
      assert(0);
    }
  }
}

void OpenFileTable::_prefetch_inodes()
{
  dout(10) << __func__ << " state " << prefetch_state << dendl;
  assert(!num_opening_inodes);
  num_opening_inodes = 1;

  int64_t pool;
  if (prefetch_state == DIR_INODES)
    pool = mds->mdsmap->get_metadata_pool();
  else if (prefetch_state == FILE_INODES)
    pool = mds->mdsmap->get_first_data_pool();
  else
    assert(0);

  MDCache *mdcache = mds->mdcache;

  for (auto& it : loaded_anchor_map) {
    if (it.second.d_type == DT_DIR) {
      if (prefetch_state != DIR_INODES)
	continue;
      if (MDS_INO_IS_MDSDIR(it.first)) {
	it.second.auth = MDS_INO_MDSDIR_OWNER(it.first);
	continue;
      }
      if (MDS_INO_IS_STRAY(it.first)) {
	it.second.auth = MDS_INO_STRAY_OWNER(it.first);
	continue;
      }
    } else {
      if (prefetch_state != FILE_INODES)
	continue;
      // load all file inodes for MDCache::identify_files_to_recover()
    }
    CInode *in = mdcache->get_inode(it.first);
    if (in)
      continue;

    num_opening_inodes++;
    mdcache->open_ino(it.first, pool, new C_OFT_OpenInoFinish(this, it.first), false);
  }

  _open_ino_finish(inodeno_t(0), 0);
}

bool OpenFileTable::prefetch_inodes()
{
  dout(10) << __func__ << dendl;
  assert(!prefetch_state);
  prefetch_state = DIR_INODES;

  if (!load_done) {
    wait_for_load(
	new MDSInternalContextWrapper(mds,
	  new FunctionContext([this](int r) {
	    _prefetch_inodes();
	    })
	  )
	);
    return true;
  }

  _prefetch_inodes();
  return !is_prefetched();
}

bool OpenFileTable::should_log_open(CInode *in)
{
  if (in->state_test(CInode::STATE_TRACKEDBYOFT)) {
    // inode just journaled
    if (in->last_journaled >= committing_log_seq)
      return false;
    // item not dirty. it means the item has already been saved
    auto p = dirty_items.find(in->ino());
    if (p == dirty_items.end())
      return false;
  }
  return true;
}
