/* Copyright (c) 2014 Anton Titov.
 * Copyright (c) 2014 pCloud Ltd.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of pCloud Ltd nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL pCloud Ltd BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "pcompat.h"
#include "pfstasks.h"
#include "plibs.h"
#include "ptimer.h"
#include "pfsupload.h"
#include "psettings.h"
#include <string.h>
#include <stddef.h>

#define FOLDER_HASH 256

static psync_tree *folders=PSYNC_TREE_EMPTY;

psync_uint_t folder_hash(psync_fsfolderid_t folderid){
  return ((uint64_t)folderid)%FOLDER_HASH;
}

psync_fstask_folder_t *psync_fstask_get_or_create_folder_tasks(psync_fsfolderid_t folderid){
  psync_fstask_folder_t *folder;
  psync_sql_lock();
  folder=psync_fstask_get_or_create_folder_tasks_locked(folderid);
  psync_sql_unlock();
  return folder;
}

psync_fstask_folder_t *psync_fstask_get_folder_tasks(psync_fsfolderid_t folderid){
  psync_fstask_folder_t *folder;
  psync_sql_lock();
  folder=psync_fstask_get_folder_tasks_locked(folderid);
  psync_sql_unlock();
  return folder;
}

void psync_fstask_release_folder_tasks(psync_fstask_folder_t *folder){
  psync_sql_lock();
  psync_fstask_release_folder_tasks_locked(folder);
  psync_sql_unlock();
}

psync_fstask_folder_t *psync_fstask_get_or_create_folder_tasks_locked(psync_fsfolderid_t folderid){
  psync_fstask_folder_t *folder;
  psync_tree *tr;
  int64_t d;
  tr=folders;
  d=-1;
  while (tr){
    folder=psync_tree_element(tr, psync_fstask_folder_t, tree);
    d=folderid-folder->folderid;
    if (d<0){
      if (tr->left)
        tr=tr->left;
      else
        break;
    }
    else if (d>0)
      if (tr->right)
        tr=tr->right;
      else
        break;
    else{
      folder->refcnt++;
      return folder;
    }
  }
  folder=psync_new(psync_fstask_folder_t);
  memset(folder, 0, sizeof(psync_fstask_folder_t));
  if (d<0)
    psync_tree_add_before(&folders, tr, &folder->tree);
  else
    psync_tree_add_after(&folders, tr, &folder->tree);
  folder->folderid=folderid;
  folder->refcnt=1;
  return folder;
}

psync_fstask_folder_t *psync_fstask_get_folder_tasks_locked(psync_fsfolderid_t folderid){
  psync_fstask_folder_t *folder;
  psync_tree *tr;
  tr=folders;
  while (tr){
    folder=psync_tree_element(tr, psync_fstask_folder_t, tree);
    if (folderid<folder->folderid)
      tr=tr->left;
    else if (folderid>folder->folderid)
      tr=tr->right;
    else{
      folder->refcnt++;
      return folder;
    }
  }
  return NULL;
}

void psync_fstask_release_folder_tasks_locked(psync_fstask_folder_t *folder){
  if (--folder->refcnt==0 && !folder->taskscnt){
    debug(D_NOTICE, "releasing folder id %ld", (long int)folder->folderid);
    psync_tree_del(&folders, &folder->tree);
    psync_free(folder);
  }
}

static psync_tree *psync_fstask_search_tree(psync_tree *tree, size_t nameoff, const char *name, uint64_t taskid, size_t taskidoff){
  int c;
  while (tree){
    c=psync_filename_cmp(name, ((char *)tree)+nameoff);
    if (c<0)
      tree=tree->left;
    else if (c>0)
      tree=tree->right;
    else
      break;
  }
  if (!tree || !taskid || *((uint64_t *)(((char *)tree)+taskidoff))==taskid)
    return tree;
  else{
    psync_tree *tn;
    tn=psync_tree_get_prev(tree);
    while (tn){
      if (psync_filename_cmp(name, ((char *)tn)+nameoff))
        break;
      if (*((uint64_t *)(((char *)tn)+taskidoff))==taskid)
        return tn;
      tn=psync_tree_get_prev(tn);
    }
    tn=psync_tree_get_next(tree);
    while (tn){
      if (psync_filename_cmp(name, ((char *)tn)+nameoff))
        break;
      if (*((uint64_t *)(((char *)tn)+taskidoff))==taskid)
        return tn;
      tn=psync_tree_get_next(tn);
    }
    return NULL;
  }
}

static void psync_fstask_insert_into_tree(psync_tree **tree, size_t nameoff, psync_tree *element){
  const char *name;
  psync_tree *node;
  int c;
  if (!*tree){
    psync_tree_add_after(tree, NULL, element);
    return;
  }
  name=((char *)element)+nameoff;
  node=*tree;
  while (1){
    c=psync_filename_cmp(name, ((char *)node)+nameoff);
    if (c<0){
      if (node->left)
        node=node->left;
      else{
        psync_tree_add_before(tree, node, element);
        return;
      }
    }
    else{
      if (c==0)
        debug(D_BUG, "duplicate entry %s, should not happen", name);
      if (node->right)
        node=node->right;
      else{
        psync_tree_add_after(tree, node, element);
        return;
      }
    }
  }
}

psync_fstask_mkdir_t *psync_fstask_find_mkdir(psync_fstask_folder_t *folder, const char *name, uint64_t taskid){
  return psync_tree_element(
    psync_fstask_search_tree(folder->mkdirs, offsetof(psync_fstask_mkdir_t, name), name, taskid, offsetof(psync_fstask_mkdir_t, taskid)), 
    psync_fstask_mkdir_t, tree);
}

psync_fstask_rmdir_t *psync_fstask_find_rmdir(psync_fstask_folder_t *folder, const char *name, uint64_t taskid){
  return psync_tree_element(
    psync_fstask_search_tree(folder->rmdirs, offsetof(psync_fstask_rmdir_t, name), name, taskid, offsetof(psync_fstask_rmdir_t, taskid)), 
    psync_fstask_rmdir_t, tree);
}

psync_fstask_creat_t *psync_fstask_find_creat(psync_fstask_folder_t *folder, const char *name, uint64_t taskid){
  return psync_tree_element(
    psync_fstask_search_tree(folder->creats, offsetof(psync_fstask_creat_t, name), name, taskid, offsetof(psync_fstask_creat_t, taskid)), 
    psync_fstask_creat_t, tree);
}

psync_fstask_unlink_t *psync_fstask_find_unlink(psync_fstask_folder_t *folder, const char *name, uint64_t taskid){
  return psync_tree_element(
    psync_fstask_search_tree(folder->unlinks, offsetof(psync_fstask_unlink_t, name), name, taskid, offsetof(psync_fstask_unlink_t, taskid)), 
    psync_fstask_unlink_t, tree);
}

static void psync_fstask_depend(uint64_t taskid, uint64_t dependontaskid){
  psync_sql_res *res;
  res=psync_sql_prep_statement("INSERT OR IGNORE INTO fstaskdepend (fstaskid, dependfstaskid) VALUES (?, ?)");
  psync_sql_bind_uint(res, 1, taskid);
  psync_sql_bind_uint(res, 2, dependontaskid);
  psync_sql_run_free(res);
}

int psync_fstask_mkdir(psync_fsfolderid_t folderid, const char *name){
  psync_sql_res *res;
  psync_uint_row row;
  psync_fstask_folder_t *folder;
  psync_fstask_mkdir_t *task;
  uint64_t taskid;
  size_t len;
  time_t ctime;
  folder=psync_fstask_get_or_create_folder_tasks_locked(folderid);
  len=strlen(name);
  if (folderid>=0){
    res=psync_sql_query("SELECT id FROM folder WHERE parentfolderid=? AND name=?");
    psync_sql_bind_uint(res, 1, folderid);
    psync_sql_bind_lstring(res, 2, name, len);
    row=psync_sql_fetch_rowint(res);
    psync_sql_free_result(res);
    if (row && !psync_fstask_find_rmdir(folder, name, 0)){
      psync_fstask_release_folder_tasks_locked(folder);
      return -EEXIST;
    }
  }
  if (psync_fstask_find_mkdir(folder, name, 0)){
    psync_fstask_release_folder_tasks_locked(folder);
    return -EEXIST;
  }
  ctime=psync_timer_time();
  psync_sql_start_transaction();
  res=psync_sql_prep_statement("INSERT INTO fstask (type, status, folderid, text1, int1) VALUES ("NTO_STR(PSYNC_FS_TASK_MKDIR)", 0, ?, ?, ?)");
  psync_sql_bind_int(res, 1, folderid);
  psync_sql_bind_lstring(res, 2, name, len);
  psync_sql_bind_uint(res, 3, ctime);
  psync_sql_run_free(res);
  taskid=psync_sql_insertid();
  if (folderid<0)
    psync_fstask_depend(taskid, -folderid);
  if (unlikely_log(psync_sql_commit_transaction())){
    psync_fstask_release_folder_tasks_locked(folder);
    return -EIO;
  }
  len++;
  task=(psync_fstask_mkdir_t *)psync_malloc(offsetof(psync_fstask_mkdir_t, name)+len);
  task->taskid=taskid;
  task->ctime=task->mtime=ctime;
  task->folderid=-task->taskid;
  task->subdircnt=0;
  memcpy(task->name, name, len);
  psync_fstask_insert_into_tree(&folder->mkdirs, offsetof(psync_fstask_mkdir_t, name), &task->tree);
  folder->taskscnt++;
  psync_fstask_release_folder_tasks_locked(folder);
  if (folderid>=0)
    psync_fsupload_wake();
  return 0;
}

int psync_fstask_rmdir(psync_fsfolderid_t folderid, const char *name){
  psync_sql_res *res;
  psync_uint_row row;
  psync_fstask_folder_t *folder;
  psync_fstask_rmdir_t *task;
  psync_fstask_mkdir_t *mk;
  uint64_t depend, taskid;
  psync_fsfolderid_t cfolderid;
  size_t len;
  len=strlen(name);
  folder=psync_fstask_get_or_create_folder_tasks_locked(folderid);
  mk=psync_fstask_find_mkdir(folder, name, 0);
  if (mk==NULL){
    res=psync_sql_query("SELECT id FROM folder WHERE parentfolderid=? AND name=?");
    psync_sql_bind_uint(res, 1, folderid);
    psync_sql_bind_lstring(res, 2, name, len);
    row=psync_sql_fetch_rowint(res);
    if (!row || psync_fstask_find_rmdir(folder, name, 0)){
      psync_sql_free_result(res);
      psync_fstask_release_folder_tasks_locked(folder);
      return -ENOENT;
    }
    cfolderid=row[0];
    psync_sql_free_result(res);
    depend=0;
  }
  else{
    depend=mk->taskid;
    cfolderid=mk->folderid;
    psync_tree_del(&folder->mkdirs, &mk->tree);
    psync_free(mk);
    folder->taskscnt--;
  }
  psync_sql_start_transaction();
  res=psync_sql_prep_statement("INSERT INTO fstask (type, status, folderid, int1, text1) VALUES ("NTO_STR(PSYNC_FS_TASK_RMDIR)", 0, ?, ?, ?)");
  psync_sql_bind_int(res, 1, folderid);
  psync_sql_bind_int(res, 2, cfolderid);
  psync_sql_bind_lstring(res, 3, name, len);
  psync_sql_run_free(res);
  taskid=psync_sql_insertid();
  if (depend)
    psync_fstask_depend(taskid, depend);
  res=psync_sql_query("SELECT id FROM fstask WHERE folderid=?");
  psync_sql_bind_int(res, 1, cfolderid);
  while ((row=psync_sql_fetch_rowint(res))){
    psync_fstask_depend(taskid, row[0]);
    depend++;
  }
  psync_sql_free_result(res);
  if (unlikely_log(psync_sql_commit_transaction())){
    psync_fstask_release_folder_tasks_locked(folder);
    return -EIO;
  }
  len++;
  task=(psync_fstask_rmdir_t *)psync_malloc(offsetof(psync_fstask_rmdir_t, name)+len);
  task->taskid=taskid;
  task->folderid=cfolderid;
  memcpy(task->name, name, len);
  psync_fstask_insert_into_tree(&folder->rmdirs, offsetof(psync_fstask_rmdir_t, name), &task->tree);
  folder->taskscnt++;
  psync_fstask_release_folder_tasks_locked(folder);
  if (depend==0)
    psync_fsupload_wake();
  return 0;
}

psync_fstask_creat_t *psync_fstask_add_creat(psync_fstask_folder_t *folder, const char *name){
  psync_sql_res *res;
  psync_fstask_creat_t *task;
  uint64_t taskid;
  size_t len;
  len=strlen(name);
  psync_sql_start_transaction();
  res=psync_sql_prep_statement("INSERT INTO fstask (type, status, folderid, fileid, text1) VALUES ("NTO_STR(PSYNC_FS_TASK_CREAT)", 1, ?, 0, ?)");
  psync_sql_bind_int(res, 1, folder->folderid);
  psync_sql_bind_lstring(res, 2, name, len);
  psync_sql_run_free(res);
  taskid=psync_sql_insertid();
  if (folder->folderid<0)
    psync_fstask_depend(taskid, folder->folderid);
  if (unlikely_log(psync_sql_commit_transaction()))
    return NULL;
  len++;
  task=(psync_fstask_creat_t *)psync_malloc(offsetof(psync_fstask_creat_t, name)+len);
  task->taskid=taskid;
  task->fileid=-task->taskid;
  task->newfile=1;
  memcpy(task->name, name, len);
  psync_fstask_insert_into_tree(&folder->creats, offsetof(psync_fstask_creat_t, name), &task->tree);
  folder->taskscnt++;
  return task;
}

int psync_fstask_unlink(psync_fsfolderid_t folderid, const char *name){
  psync_sql_res *res;
  psync_uint_row row;
  psync_fstask_folder_t *folder;
  psync_fstask_unlink_t *task;
  psync_fstask_creat_t *cr;
  uint64_t depend, taskid;
  psync_fsfileid_t fileid;
  size_t len;
  len=strlen(name);
  folder=psync_fstask_get_or_create_folder_tasks_locked(folderid);
  cr=psync_fstask_find_creat(folder, name, 0);
  if (cr==NULL){
    res=psync_sql_query("SELECT id FROM file WHERE parentfolderid=? AND name=?");
    psync_sql_bind_uint(res, 1, folderid);
    psync_sql_bind_lstring(res, 2, name, len);
    row=psync_sql_fetch_rowint(res);
    if (!row || psync_fstask_find_unlink(folder, name, 0)){
      psync_sql_free_result(res);
      psync_fstask_release_folder_tasks_locked(folder);
      return -ENOENT;
    }
    fileid=row[0];
    psync_sql_free_result(res);
    depend=0;
  }
  else{
    depend=cr->taskid;
    fileid=cr->fileid;
    psync_tree_del(&folder->creats, &cr->tree);
    psync_free(cr);
    folder->taskscnt--;
  }
  psync_sql_start_transaction();
  res=psync_sql_prep_statement("INSERT INTO fstask (type, status, folderid, fileid, text1) VALUES ("NTO_STR(PSYNC_FS_TASK_UNLINK)", 0, ?, ?, ?)");
  psync_sql_bind_int(res, 1, folderid);
  psync_sql_bind_int(res, 2, fileid);
  psync_sql_bind_lstring(res, 3, name, len);
  psync_sql_run_free(res);
  taskid=psync_sql_insertid();
  if (depend)
    psync_fstask_depend(taskid, depend);
  if (unlikely_log(psync_sql_commit_transaction())){
    psync_fstask_release_folder_tasks_locked(folder);
    return -EIO;
  }
  len++;
  task=(psync_fstask_unlink_t *)psync_malloc(offsetof(psync_fstask_unlink_t, name)+len);
  task->taskid=taskid;
  task->fileid=fileid;
  memcpy(task->name, name, len);
  psync_fstask_insert_into_tree(&folder->unlinks, offsetof(psync_fstask_unlink_t, name), &task->tree);
  folder->taskscnt++;
  psync_fstask_release_folder_tasks_locked(folder);
  if (depend==0)
    psync_fsupload_wake();
  return 0;
}

int psync_fstask_rename_file(psync_fsfileid_t fileid, psync_fsfolderid_t parentfolderid, const char *name, psync_fsfolderid_t to_folderid, const char *new_name){
  psync_sql_res *res;
  psync_uint_row row;
  psync_fstask_folder_t *folder;
  psync_fstask_creat_t *cr;
  psync_fstask_unlink_t *rm;
  size_t nlen, nnlen;
  uint64_t ftaskid, ttaskid;
  nlen=strlen(name);
  if (new_name)
    nnlen=strlen(new_name);
  else{
    new_name=name;
    nnlen=nlen;
  }
  psync_sql_start_transaction();
  res=psync_sql_prep_statement("INSERT INTO fstask (type, status, folderid, fileid, text1) VALUES ("NTO_STR(PSYNC_FS_TASK_RENFILE_FROM)", 10, ?, ?, ?)");
  psync_sql_bind_int(res, 1, parentfolderid);
  psync_sql_bind_int(res, 2, fileid);
  psync_sql_bind_lstring(res, 3, name, nlen);
  psync_sql_run_free(res);
  ftaskid=psync_sql_insertid();
  res=psync_sql_prep_statement("INSERT INTO fstask (type, status, folderid, fileid, text1, int1) VALUES ("NTO_STR(PSYNC_FS_TASK_RENFILE_TO)", 0, ?, ?, ?, ?)");
  psync_sql_bind_int(res, 1, to_folderid);
  psync_sql_bind_int(res, 2, fileid);
  psync_sql_bind_lstring(res, 3, new_name, nnlen);
  psync_sql_bind_uint(res, 4, ftaskid);
  psync_sql_run_free(res);
  ttaskid=psync_sql_insertid();
  if (fileid<0)
    psync_fstask_depend(ttaskid, -fileid);
  if (parentfolderid<0)
    psync_fstask_depend(ttaskid, -parentfolderid);
  if (to_folderid<0 && to_folderid!=parentfolderid)
    psync_fstask_depend(ttaskid, -to_folderid);
  res=psync_sql_query("SELECT id FROM fstask WHERE folderid=? AND text1=?");
  psync_sql_bind_int(res, 1, to_folderid);
  psync_sql_bind_lstring(res, 2, new_name, nnlen);
  while ((row=psync_sql_fetch_rowint(res)))
    if (row[0]!=ftaskid && row[0]!=ttaskid)
      psync_fstask_depend(ttaskid, row[0]);
  psync_sql_free_result(res);
  if (psync_sql_commit_transaction())
    return -EIO;
  folder=psync_fstask_get_or_create_folder_tasks_locked(parentfolderid);
  if ((cr=psync_fstask_find_creat(folder, name, 0))){
    psync_tree_del(&folder->creats, &cr->tree);
    psync_free(cr);
    folder->taskscnt--;
  }
  nlen++;
  rm=(psync_fstask_unlink_t *)psync_malloc(offsetof(psync_fstask_unlink_t, name)+nlen);
  rm->taskid=ftaskid;
  rm->fileid=fileid;
  memcpy(rm->name, name, nlen);
  psync_fstask_insert_into_tree(&folder->unlinks, offsetof(psync_fstask_unlink_t, name), &rm->tree);
  folder->taskscnt++;
  psync_fstask_release_folder_tasks_locked(folder);
  folder=psync_fstask_get_or_create_folder_tasks_locked(to_folderid);
  nnlen++;
  cr=(psync_fstask_creat_t *)psync_malloc(offsetof(psync_fstask_creat_t, name)+nnlen);
  cr->taskid=ttaskid;
  cr->fileid=fileid;
  cr->newfile=0;
  memcpy(cr->name, new_name, nnlen);
  psync_fstask_insert_into_tree(&folder->creats, offsetof(psync_fstask_creat_t, name), &cr->tree);
  folder->taskscnt++;
  psync_fstask_release_folder_tasks_locked(folder);
  psync_fsupload_wake();
  return 0;
}

static int folder_cmp(const psync_tree *t1, const psync_tree *t2){
  int64_t d=psync_tree_element(t1, psync_fstask_folder_t, tree)->folderid-psync_tree_element(t2, psync_fstask_folder_t, tree)->folderid;
  if (d<0)
    return -1;
  else if (d>0)
    return 1;
  else
    return 0;
}

void psync_fstask_folder_created(psync_folderid_t parentfolderid, uint64_t taskid, psync_folderid_t folderid, const char *name){
  psync_fstask_folder_t *folder;
  psync_fstask_mkdir_t *mk;
  folder=psync_fstask_get_folder_tasks_locked(parentfolderid);
  if (folder){
    mk=psync_fstask_find_mkdir(folder, name, taskid);
    if (mk){
      psync_tree_del(&folder->mkdirs, &mk->tree);
      psync_free(mk);
      folder->taskscnt--;
    }
    psync_fstask_release_folder_tasks_locked(folder);
  }
  folder=psync_fstask_get_folder_tasks_locked(-taskid);
  if (folder){
    psync_tree_del(&folders, &folder->tree);
    folder->folderid=folderid;
    psync_tree_add(&folders, &folder->tree, folder_cmp);
    psync_fstask_release_folder_tasks_locked(folder);
  }
}

void psync_fstask_folder_deleted(psync_folderid_t parentfolderid, uint64_t taskid, const char *name){
  psync_fstask_folder_t *folder;
  psync_fstask_rmdir_t *rm;
  folder=psync_fstask_get_folder_tasks_locked(parentfolderid);
  if (folder){
    rm=psync_fstask_find_rmdir(folder, name, taskid);
    if (rm){
      psync_tree_del(&folder->rmdirs, &rm->tree);
      psync_free(rm);
      folder->taskscnt--;
    }
    psync_fstask_release_folder_tasks_locked(folder);
  }
}

void psync_fstask_file_created(psync_folderid_t parentfolderid, uint64_t taskid, const char *name){
  psync_fstask_folder_t *folder;
  psync_fstask_creat_t *cr;
  folder=psync_fstask_get_folder_tasks_locked(parentfolderid);
  if (folder){
    cr=psync_fstask_find_creat(folder, name, taskid);
    if (cr){
      psync_tree_del(&folder->creats, &cr->tree);
      psync_free(cr);
      folder->taskscnt--;
    }
    psync_fstask_release_folder_tasks_locked(folder);
  }
}

void psync_fstask_file_deleted(psync_folderid_t parentfolderid, uint64_t taskid, const char *name){
  psync_fstask_folder_t *folder;
  psync_fstask_unlink_t *un;
  folder=psync_fstask_get_folder_tasks_locked(parentfolderid);
  if (folder){
    un=psync_fstask_find_unlink(folder, name, taskid);
    if (un){
      psync_tree_del(&folder->unlinks, &un->tree);
      psync_free(un);
      folder->taskscnt--;
    }
    psync_fstask_release_folder_tasks_locked(folder);
  }
}

void psync_fstask_file_renamed(psync_folderid_t folderid, uint64_t taskid, const char *name, uint64_t frtaskid){
  psync_sql_res *res;
  psync_fstask_folder_t *folder;
  psync_fstask_unlink_t *un;
  psync_fstask_creat_t *cr;
  psync_variant_row row;
  folder=psync_fstask_get_folder_tasks_locked(folderid);
  if (folder){
    cr=psync_fstask_find_creat(folder, name, taskid);
    if (cr){
      psync_tree_del(&folder->creats, &cr->tree);
      psync_free(cr);
      folder->taskscnt--;
    }
    psync_fstask_release_folder_tasks_locked(folder);
  }
  res=psync_sql_query("SELECT id, folderid, text1 FROM fstask WHERE id=?");
  psync_sql_bind_uint(res, 1, frtaskid);
  if (likely_log(row=psync_sql_fetch_row(res))){
    folder=psync_fstask_get_folder_tasks_locked(psync_get_snumber(row[1]));
    if (folder){
      un=psync_fstask_find_unlink(folder, psync_get_string(row[2]), psync_get_number(row[0]));
      if (un){
        psync_tree_del(&folder->unlinks, &un->tree);
        psync_free(un);
        folder->taskscnt--;
      }
      psync_fstask_release_folder_tasks_locked(folder);
    }
  }
  psync_sql_free_result(res);
  res=psync_sql_prep_statement("DELETE FROM fstask WHERE id=?");
  psync_sql_bind_uint(res, 1, frtaskid);
  psync_sql_run_free(res);
}

static void psync_init_task_mkdir(psync_variant_row row){
  uint64_t taskid;
  psync_fsfolderid_t folderid;
  const char *name;
  psync_fstask_folder_t *folder;
  psync_fstask_mkdir_t *task;
  time_t ctime;
  size_t len;
  taskid=psync_get_number(row[0]);
  folderid=psync_get_snumber(row[2]);
  name=psync_get_lstring(row[4], &len);
  ctime=psync_get_number(row[6]);
  folder=psync_fstask_get_or_create_folder_tasks_locked(folderid);
  len++;
  task=(psync_fstask_mkdir_t *)psync_malloc(offsetof(psync_fstask_mkdir_t, name)+len);
  task->taskid=taskid;
  task->ctime=task->mtime=ctime;
  task->folderid=-taskid;
  task->subdircnt=0;
  memcpy(task->name, name, len);
  psync_fstask_insert_into_tree(&folder->mkdirs, offsetof(psync_fstask_mkdir_t, name), &task->tree);
  folder->taskscnt++;
  psync_fstask_release_folder_tasks_locked(folder);
}

static void psync_init_task_rmdir(psync_variant_row row){
  uint64_t taskid;
  psync_fsfolderid_t cfolderid, folderid;
  const char *name;
  psync_fstask_folder_t *folder;
  psync_fstask_rmdir_t *task;
  psync_fstask_mkdir_t *mk;
  size_t len;
  taskid=psync_get_number(row[0]);
  folderid=psync_get_snumber(row[2]);
  name=psync_get_lstring(row[4], &len);
  cfolderid=psync_get_snumber(row[6]);
  folder=psync_fstask_get_or_create_folder_tasks_locked(folderid);
  mk=psync_fstask_find_mkdir(folder, name, 0);
  if (mk){
    psync_tree_del(&folder->mkdirs, &mk->tree);
    psync_free(mk);
    folder->taskscnt--;
  }
  len++;
  task=(psync_fstask_rmdir_t *)psync_malloc(offsetof(psync_fstask_rmdir_t, name)+len);
  task->taskid=taskid;
  task->folderid=cfolderid;
  memcpy(task->name, name, len);
  psync_fstask_insert_into_tree(&folder->rmdirs, offsetof(psync_fstask_rmdir_t, name), &task->tree);
  folder->taskscnt++;
  psync_fstask_release_folder_tasks_locked(folder);
}

static void psync_init_task_creat(psync_variant_row row){
  uint64_t taskid;
  psync_fstask_creat_t *task;
  psync_fstask_folder_t *folder;
  const char *name;
  psync_fsfolderid_t folderid;
  size_t len;
  taskid=psync_get_number(row[0]);
  folderid=psync_get_snumber(row[2]);
  name=psync_get_lstring(row[4], &len);
  folder=psync_fstask_get_or_create_folder_tasks_locked(folderid);
  len++;
  task=(psync_fstask_creat_t *)psync_malloc(offsetof(psync_fstask_creat_t, name)+len);
  task->taskid=taskid;
  task->fileid=-taskid;
  task->newfile=1;
  memcpy(task->name, name, len);
  psync_fstask_insert_into_tree(&folder->creats, offsetof(psync_fstask_creat_t, name), &task->tree);
  folder->taskscnt++;
  psync_fstask_release_folder_tasks_locked(folder);
}

static void psync_init_task_unlink(psync_variant_row row){
  uint64_t taskid;
  psync_fsfolderid_t folderid;
  const char *name;
  psync_fstask_folder_t *folder;
  psync_fstask_unlink_t *task;
  psync_fstask_creat_t *cr;
  size_t len;
  taskid=psync_get_number(row[0]);
  folderid=psync_get_snumber(row[2]);
  name=psync_get_lstring(row[4], &len);
  folder=psync_fstask_get_or_create_folder_tasks_locked(folderid);
  cr=psync_fstask_find_creat(folder, name, 0);
  if (cr){
    psync_tree_del(&folder->creats, &cr->tree);
    psync_free(cr);
    folder->taskscnt--;
  }
  len++;
  task=(psync_fstask_unlink_t *)psync_malloc(offsetof(psync_fstask_unlink_t, name)+len);
  task->taskid=taskid;
  task->fileid=psync_get_snumber(row[3]);
  memcpy(task->name, name, len);
  psync_fstask_insert_into_tree(&folder->unlinks, offsetof(psync_fstask_unlink_t, name), &task->tree);
  folder->taskscnt++;
  psync_fstask_release_folder_tasks_locked(folder);
}

static void psync_init_task_renfile_from(psync_variant_row row){
  const char *name;
  psync_fstask_folder_t *folder;
  psync_fstask_creat_t *cr;
  psync_fstask_unlink_t *rm;
  size_t len;
  name=psync_get_lstring(row[4], &len);
  folder=psync_fstask_get_or_create_folder_tasks_locked(psync_get_number(row[2]));
  if ((cr=psync_fstask_find_creat(folder, name, 0))){
    psync_tree_del(&folder->creats, &cr->tree);
    psync_free(cr);
    folder->taskscnt--;
  }
  len++;
  rm=(psync_fstask_unlink_t *)psync_malloc(offsetof(psync_fstask_unlink_t, name)+len);
  rm->taskid=psync_get_number(row[0]);
  rm->fileid=psync_get_snumber(row[3]);
  memcpy(rm->name, name, len);
  psync_fstask_insert_into_tree(&folder->unlinks, offsetof(psync_fstask_unlink_t, name), &rm->tree);
  folder->taskscnt++;
  psync_fstask_release_folder_tasks_locked(folder);
}

static void psync_init_task_renfile_to(psync_variant_row row){
  const char *name;
  psync_fstask_creat_t *cr;
  psync_fstask_folder_t *folder;
  size_t len;
  name=psync_get_lstring(row[4], &len);
  folder=psync_fstask_get_or_create_folder_tasks_locked(psync_get_number(row[2]));
  len++;
  cr=(psync_fstask_creat_t *)psync_malloc(offsetof(psync_fstask_creat_t, name)+len);
  cr->taskid=psync_get_number(row[0]);
  cr->fileid=psync_get_snumber(row[3]);
  cr->newfile=0;
  memcpy(cr->name, name, len);
  psync_fstask_insert_into_tree(&folder->creats, offsetof(psync_fstask_creat_t, name), &cr->tree);
  folder->taskscnt++;
  psync_fstask_release_folder_tasks_locked(folder);
}

typedef void (*psync_init_task_ptr)(psync_variant_row);

static psync_init_task_ptr psync_init_task_func[]={
  NULL,
  psync_init_task_mkdir,
  psync_init_task_rmdir,
  psync_init_task_creat,
  psync_init_task_unlink,
  psync_init_task_renfile_from,
  psync_init_task_renfile_to
};

void psync_fstask_init(){
  psync_uint_t tp;
  psync_sql_res *res;
  psync_variant_row row;
//  res=psync_sql_prep_statement("UPDATE fstask SET status=0 WHERE status!=0");
//  psync_sql_run_free(res);
  res=psync_sql_query("SELECT id, type, folderid, fileid, text1, text2, int1, int2 FROM fstask ORDER BY id");
  while ((row=psync_sql_fetch_row(res))){
    tp=psync_get_number(row[1]);
    if (!tp || tp>=ARRAY_SIZE(psync_init_task_func)){
      debug(D_BUG, "invalid fstask type %lu", (long unsigned)tp);
      continue;
    }
    psync_init_task_func[tp](row);
  }
  psync_sql_free_result(res);
  psync_fsupload_init();
}