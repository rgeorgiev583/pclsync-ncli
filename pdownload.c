/* Copyright (c) 2013 Anton Titov.
 * Copyright (c) 2013 pCloud Ltd.
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

#include "pdownload.h"
#include "pstatus.h"
#include "ptimer.h"
#include "plibs.h"
#include "ptasks.h"
#include "pstatus.h"
#include "pssl.h"
#include "psettings.h"
#include "pnetlibs.h"
#include "pcallbacks.h"
#include "pfolder.h"
#include "psyncer.h"
#include "papi.h"
#include "pp2p.h"
#include "plist.h"
#include "plocalscan.h"
#include "pupload.h"

typedef struct {
  psync_list list;
  psync_fileid_t fileid;
  psync_syncid_t syncid;
  int stop;
  unsigned char hash[PSYNC_HASH_DIGEST_HEXLEN];
} download_list_t;

typedef struct {
  uint64_t taskid;
  download_list_t dwllist;
  psync_folderid_t localfolderid;
  char filename[];
} download_task_t;

static pthread_mutex_t download_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t download_cond=PTHREAD_COND_INITIALIZER;
static psync_uint_t download_wakes=0;
static const uint32_t requiredstatuses[]={
  PSTATUS_COMBINE(PSTATUS_TYPE_AUTH, PSTATUS_AUTH_PROVIDED),
  PSTATUS_COMBINE(PSTATUS_TYPE_RUN, PSTATUS_RUN_RUN),
  PSTATUS_COMBINE(PSTATUS_TYPE_ONLINE, PSTATUS_ONLINE_ONLINE)
};

static psync_uint_t started_downloads=0;
static psync_uint_t starting_downloads=0;
static psync_uint_t current_downloads_waiters=0;
static pthread_mutex_t current_downloads_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t current_downloads_cond=PTHREAD_COND_INITIALIZER;

static psync_list downloads=PSYNC_LIST_STATIC_INIT(downloads);

static void task_wait_no_downloads(){
  pthread_mutex_lock(&current_downloads_mutex);
  while (starting_downloads || started_downloads){
    current_downloads_waiters++;
    pthread_cond_wait(&current_downloads_cond, &current_downloads_mutex);
    current_downloads_waiters--;
  }
  pthread_mutex_unlock(&current_downloads_mutex);
}

static int task_mkdir(const char *path){
  int err;
  while (1){
    if (likely(!psync_mkdir(path))){ // don't change to likely_log, as it may overwrite psync_fs_err;
      psync_set_local_full(0);
      return 0;
    }
    err=psync_fs_err();
    debug(D_WARNING, "mkdir of %s failed, errno=%d", path, (int)err);
    if (err==P_NOSPC || err==P_DQUOT){
      psync_set_local_full(1);
      psync_milisleep(PSYNC_SLEEP_ON_DISK_FULL);
    }
    else {
      psync_set_local_full(0);
      if (err==P_NOENT)
        return 0; // do we have a choice? the user deleted the directory
      else if (err==P_EXIST){
        psync_stat_t st;
        if (psync_stat(path, &st)){
          debug(D_BUG, "mkdir failed with EEXIST, but stat returned error. race?");
          return -1;
        }
        if (psync_stat_isfolder(&st))
          return 0;
        if (psync_rename_conflicted_file(path))
          return -1;
      }
      else
        return -1;
    }
    psync_wait_statuses_array(requiredstatuses, ARRAY_SIZE(requiredstatuses));
  }
}

static int task_rmdir(const char *path){
  task_wait_no_downloads();
  if (likely_log(!psync_rmdir_with_trashes(path)))
    return 0;
  if (psync_fs_err()==P_BUSY || psync_fs_err()==P_ROFS)
    return -1;
  psync_wake_localscan();
  return 0;
//  if (psync_fs_err()==P_NOENT || psync_fs_err()==P_NOTDIR || psync_fs_err()==P_NOTEMPTY || psync_fs_err()==P_EXIST)
//    return 0;
}

/*static int task_rmdir_rec(const char *path){
  task_wait_no_downloads();
  if (likely_log(!psync_rmdir_recursive(path)))
    return 0;
  if (psync_fs_err()==P_BUSY || psync_fs_err()==P_ROFS)
    return -1;
  return 0;
}*/

static void do_move(void *ptr, psync_pstat *st){
  const char **arr;
  char *oldpath, *newpath;
  arr=(const char **)ptr;
  oldpath=psync_strcat(arr[0], st->name, NULL);
  newpath=psync_strcat(arr[1], st->name, NULL);
  if (psync_stat_isfolder(&st->stat))
    psync_rendir(oldpath, newpath);
  else
    psync_file_rename(oldpath, newpath);
  psync_free(newpath);
  psync_free(oldpath);
}

static int move_folder_contents(const char *oldpath, const char *newpath){
  const char *arr[2];
  arr[0]=oldpath;
  arr[1]=newpath;
  psync_list_dir(oldpath, do_move, (void *)arr);
  return psync_rmdir_with_trashes(oldpath);
}

static int task_renamedir(const char *oldpath, const char *newpath){
  while (1){
    if (likely_log(!psync_rendir(oldpath, newpath))){
      psync_set_local_full(0);
      return 0;
    }
    if (psync_fs_err()==P_NOSPC || psync_fs_err()==P_DQUOT){
      psync_set_local_full(1);
      psync_milisleep(PSYNC_SLEEP_ON_DISK_FULL);
    }
    else {
      psync_set_local_full(0);
      if (psync_fs_err()==P_BUSY || psync_fs_err()==P_ROFS)
        return -1;
      if (psync_fs_err()==P_NOENT)
        return 0;
      else if (psync_fs_err()==P_EXIST || psync_fs_err()==P_NOTEMPTY || psync_fs_err()==P_NOTDIR){
        psync_stat_t st;
        if (psync_stat(newpath, &st)){
          debug(D_BUG, "rename failed with EEXIST, but stat returned error. race?");
          return -1;
        }
        if (psync_stat_isfolder(&st))
          return move_folder_contents(oldpath, newpath);
        if (psync_rename_conflicted_file(newpath))
          return -1;
      }
      else
        return -1;
    }
    psync_wait_statuses_array(requiredstatuses, ARRAY_SIZE(requiredstatuses));
  }
}

static void update_local_folder_mtime(const char *localpath, psync_folderid_t localfolderid){
  psync_stat_t st;
  psync_sql_res *res;
  if (psync_stat(localpath, &st)){
    debug(D_ERROR, "stat failed for %s", localpath);
    return;
  }
  res=psync_sql_prep_statement("UPDATE localfolder SET inode=?, deviceid=?, mtime=?, mtimenative=? WHERE id=?");
  psync_sql_bind_uint(res, 1, psync_stat_inode(&st));
  psync_sql_bind_uint(res, 2, psync_stat_device(&st));
  psync_sql_bind_uint(res, 3, psync_stat_mtime(&st));
  psync_sql_bind_uint(res, 4, psync_stat_mtime_native(&st));
  psync_sql_bind_uint(res, 5, localfolderid);
  psync_sql_run_free(res);
}

static int call_func_for_folder(psync_folderid_t localfolderid, psync_folderid_t folderid, psync_syncid_t syncid, psync_eventtype_t event, 
                                int (*func)(const char *), int updatemtime, const char *debug){
  char *localpath;
  int res;
  localpath=psync_local_path_for_local_folder(localfolderid, syncid, NULL);
  if (likely(localpath)){
    res=func(localpath);
    if (!res){
      psync_send_event_by_id(event, syncid, localpath, folderid);
      if (updatemtime)
        update_local_folder_mtime(localpath, localfolderid);
      psync_decrease_local_folder_taskcnt(localfolderid);
      debug(D_NOTICE, "%s %s", debug, localpath);
    }
    psync_free(localpath);
  }
  else{
    debug(D_ERROR, "could not get path for local folder id %lu, syncid %u", (long unsigned)localfolderid, (unsigned)syncid);
    res=0;
  }
  return res;
}

static int call_func_for_folder_name(psync_folderid_t localfolderid, psync_folderid_t folderid, const char *name, psync_syncid_t syncid, psync_eventtype_t event, 
                                int (*func)(const char *), int updatemtime, const char *debug){
  char *localpath;
  int res;
  localpath=psync_local_path_for_local_folder(localfolderid, syncid, NULL);
  if (likely(localpath)){
    res=func(localpath);
    if (!res){
      psync_send_event_by_path(event, syncid, localpath, folderid, name);
      if (updatemtime)
        update_local_folder_mtime(localpath, localfolderid);
      psync_decrease_local_folder_taskcnt(localfolderid);
      debug(D_NOTICE, "%s %s", debug, localpath);
    }
    psync_free(localpath);
  }
  else{
    debug(D_ERROR, "could not get path for local folder id %lu, syncid %u", (long unsigned)localfolderid, (unsigned)syncid);
    res=0;
  }
  return res;
}

static void delete_local_folder_from_db(psync_folderid_t localfolderid){
  psync_sql_res *res;
  psync_uint_row row;
  if (likely(localfolderid)){
    res=psync_sql_query("SELECT id FROM localfolder WHERE localparentfolderid=?");
    psync_sql_bind_uint(res, 1, localfolderid);
    while ((row=psync_sql_fetch_rowint(res)))
      delete_local_folder_from_db(row[0]);
    psync_sql_free_result(res);
    res=psync_sql_query("SELECT id FROM localfile WHERE localparentfolderid=?");
    psync_sql_bind_uint(res, 1, localfolderid);
    while ((row=psync_sql_fetch_rowint(res)))
      psync_delete_upload_tasks_for_file(row[0]);
    psync_sql_free_result(res);
    res=psync_sql_prep_statement("DELETE FROM localfile WHERE localparentfolderid=?");
    psync_sql_bind_uint(res, 1, localfolderid);
    psync_sql_run_free(res);
    res=psync_sql_prep_statement("DELETE FROM localfolder WHERE id=?");
    psync_sql_bind_uint(res, 1, localfolderid);
    psync_sql_run_free(res);
  }
}

static int task_renamefolder(psync_syncid_t newsyncid, psync_folderid_t folderid, psync_folderid_t localfolderid,
                             psync_folderid_t newlocalparentfolderid, const char *newname){
  psync_sql_res *res;
  psync_variant_row row;
  char *oldpath, *newpath;
  psync_syncid_t oldsyncid;
  int ret;
  assert(newname!=NULL);
  task_wait_no_downloads();
  res=psync_sql_query("SELECT syncid, localparentfolderid, name FROM localfolder WHERE id=?");
  psync_sql_bind_uint(res, 1, localfolderid);
  row=psync_sql_fetch_row(res);
  if (unlikely(!row)){
    psync_sql_free_result(res);
    debug(D_ERROR, "could not find local folder id %lu", (unsigned long)localfolderid);
    return 0;
  }
  oldsyncid=psync_get_number(row[0]);
  if (oldsyncid==newsyncid && psync_get_number(row[1])==newlocalparentfolderid && !psync_filename_cmp(psync_get_string(row[2]), newname)){
    psync_sql_free_result(res);
    debug(D_NOTICE, "folder %s already renamed locally, probably update initiated from this client", newname);
    return 0;
  }
  psync_sql_free_result(res);
  oldpath=psync_local_path_for_local_folder(localfolderid, oldsyncid, NULL);
  if (unlikely(!oldpath)){
    debug(D_ERROR, "could not get local path for folder id %lu", (unsigned long)localfolderid);
    return 0;
  }
  psync_sql_start_transaction();
  psync_restart_localscan();
  res=psync_sql_prep_statement("UPDATE localfolder SET syncid=?, localparentfolderid=?, name=? WHERE id=?");
  psync_sql_bind_uint(res, 1, newsyncid);
  psync_sql_bind_uint(res, 2, newlocalparentfolderid);
  psync_sql_bind_string(res, 3, newname);
  psync_sql_bind_uint(res, 4, localfolderid);
  psync_sql_run_free(res);
  newpath=psync_local_path_for_local_folder(localfolderid, newsyncid, NULL);
  if (unlikely(!newpath)){
    psync_sql_rollback_transaction();
    psync_free(oldpath);
    debug(D_ERROR, "could not get local path for folder id %lu", (unsigned long)localfolderid);
    return 0;
  }
  ret=task_renamedir(oldpath, newpath);
  if (ret)
    psync_sql_rollback_transaction();
  else{
    psync_decrease_local_folder_taskcnt(localfolderid);
    psync_sql_commit_transaction();
    psync_send_event_by_id(PEVENT_LOCAL_FOLDER_RENAMED, newsyncid, newpath, folderid);
    debug(D_NOTICE, "local folder renamed from %s to %s", oldpath, newpath);
  }
  psync_free(newpath);
  psync_free(oldpath);
  return ret;
}

static void create_conflicted(const char *name, psync_folderid_t localfolderid, psync_syncid_t syncid, const char *filename){
  psync_sql_res *res;
  res=psync_sql_prep_statement("DELETE FROM localfile WHERE syncid=? AND localparentfolderid=? AND name=?");
  psync_sql_bind_uint(res, 1, syncid);
  psync_sql_bind_uint(res, 2, localfolderid);
  psync_sql_bind_string(res, 3, filename);
  psync_restart_localscan();
  psync_rename_conflicted_file(name);
  psync_sql_run_free(res);
  psync_wake_localscan();
}

static int rename_if_notex(const char *oldname, const char *newname, psync_fileid_t fileid, psync_folderid_t localfolderid,
                           psync_syncid_t syncid, const char *filename){
  uint64_t filesize;
  int ret, isrev;
  unsigned char localhashhex[PSYNC_HASH_DIGEST_HEXLEN];
  debug(D_NOTICE, "renaming %s to %s", oldname, newname);
  if (psync_get_local_file_checksum(newname, localhashhex, &filesize)==PSYNC_NET_OK){
    debug(D_NOTICE, "file %s already exists", newname);
    ret=psync_is_revision_of_file(localhashhex, filesize, fileid, &isrev);
    if (ret==PSYNC_NET_TEMPFAIL)
      return -1;
    if (ret==PSYNC_NET_OK && !isrev)
      create_conflicted(newname, localfolderid, syncid, filename);
    else if (ret==PSYNC_NET_OK && isrev)
      debug(D_NOTICE, "file %s is found to be old revision of fileid %lu, overwriting", newname, (unsigned long)fileid);
  }
  return psync_file_rename_overwrite(oldname, newname);
}

static int stat_and_create_local(psync_syncid_t syncid, psync_fileid_t fileid, psync_folderid_t localfolderid, const char *filename,
                                 const char *name, unsigned char *checksum, uint64_t serversize, uint64_t hash){
  psync_sql_res *sql;
  psync_stat_t st;
  psync_uint_row row;
  psync_fileid_t localfileid;
  if (unlikely_log(psync_stat(name, &st)) || unlikely_log(psync_stat_size(&st)!=serversize))
    return -1;
  localfileid=0;
  psync_sql_start_transaction();
  sql=psync_sql_query("SELECT id FROM localfile WHERE syncid=? AND localparentfolderid=? AND name=?");
  psync_sql_bind_uint(sql, 1, syncid);
  psync_sql_bind_uint(sql, 2, localfolderid);
  psync_sql_bind_string(sql, 3, filename);
  if ((row=psync_sql_fetch_rowint(sql)))
    localfileid=row[0];
  psync_sql_free_result(sql);
  if (localfileid){
    sql=psync_sql_prep_statement("UPDATE localfile SET localparentfolderid=?, fileid=?, hash=?, syncid=?, size=?, inode=?, mtime=?, mtimenative=?, "
                                                       "name=?, checksum=? WHERE id=?");
    psync_sql_bind_uint(sql, 1, localfolderid);
    psync_sql_bind_uint(sql, 2, fileid);
    psync_sql_bind_uint(sql, 3, hash);
    psync_sql_bind_uint(sql, 4, syncid);
    psync_sql_bind_uint(sql, 5, psync_stat_size(&st));
    psync_sql_bind_uint(sql, 6, psync_stat_inode(&st));
    psync_sql_bind_uint(sql, 7, psync_stat_mtime(&st));
    psync_sql_bind_uint(sql, 8, psync_stat_mtime_native(&st));
    psync_sql_bind_string(sql, 9, filename);
    psync_sql_bind_lstring(sql, 10, (char *)checksum, PSYNC_HASH_DIGEST_HEXLEN);
    psync_sql_bind_uint(sql, 11, localfileid);
    psync_sql_run_free(sql);
  }
  else{
    sql=psync_sql_prep_statement("REPLACE INTO localfile (localparentfolderid, fileid, hash, syncid, size, inode, mtime, mtimenative, name, checksum)"
                                                " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    psync_sql_bind_uint(sql, 1, localfolderid);
    psync_sql_bind_uint(sql, 2, fileid);
    psync_sql_bind_uint(sql, 3, hash);
    psync_sql_bind_uint(sql, 4, syncid);
    psync_sql_bind_uint(sql, 5, psync_stat_size(&st));
    psync_sql_bind_uint(sql, 6, psync_stat_inode(&st));
    psync_sql_bind_uint(sql, 7, psync_stat_mtime(&st));
    psync_sql_bind_uint(sql, 8, psync_stat_mtime_native(&st));
    psync_sql_bind_string(sql, 9, filename);
    psync_sql_bind_lstring(sql, 10, (char *)checksum, PSYNC_HASH_DIGEST_HEXLEN);
    psync_sql_run_free(sql);
  }
  psync_sql_commit_transaction();
  return 0;
}

static void task_dec_counter(psync_uint_t *cnt, uint64_t filesize, uint64_t downloadedsize, int downloading){
  pthread_mutex_lock(&current_downloads_mutex);
  if (cnt)
    (*cnt)--;
  psync_status.bytestodownloadcurrent-=filesize;
  psync_status.bytesdownloaded-=downloadedsize;
  if (current_downloads_waiters && cnt)
    pthread_cond_signal(&current_downloads_cond);
  if (downloading){
    psync_status.filesdownloading--;
    if (!psync_status.filesdownloading){
      psync_status.bytesdownloaded=0;
      psync_status.bytestodownloadcurrent=0;
    }
  }
  pthread_mutex_unlock(&current_downloads_mutex);
  psync_status_send_update();
}

static int task_download_file(psync_syncid_t syncid, psync_fileid_t fileid, psync_folderid_t localfolderid, const char *filename, download_list_t *dwl){
  binparam params[]={P_STR("auth", psync_my_auth), P_NUM("fileid", fileid)};
  psync_stat_t st;
  psync_list ranges;
  psync_range_list_t *range;
  psync_socket *api;
  binresult *res;
  psync_sql_res *sql;
  const binresult *hosts;
  char *localpath, *tmpname, *name, *tmpold;
  char *oldfiles[2];
  uint32_t oldcnt;
  const char *requestpath;
  void *buff;
  psync_http_socket *http;
  psync_uint_t *current_counter;
  uint64_t result, serversize, localsize, downloadedsize, addedsize, hash;
  int64_t freespace;
  psync_uint_row row;
  psync_file_lock_t *lock;
  psync_hash_ctx hashctx;
  unsigned char serverhashhex[PSYNC_HASH_DIGEST_HEXLEN], 
                localhashhex[PSYNC_HASH_DIGEST_HEXLEN], 
                localhashbin[PSYNC_HASH_DIGEST_LEN];
  uint32_t i;
  psync_file_t fd, ifd;
  int rd, rt, downloadingcounted;
  downloadedsize=0;
  addedsize=0;
  downloadingcounted=0;
  current_counter=NULL;
  psync_list_init(&ranges);
  tmpold=NULL;
  
  localpath=psync_local_path_for_local_folder(localfolderid, syncid, NULL);
  if (unlikely_log(!localpath))
    return 0;
  name=psync_strcat(localpath, PSYNC_DIRECTORY_SEPARATOR, filename, NULL);
  lock=psync_lock_file(name);
  if (!lock){
    debug(D_NOTICE, "file %s is currently locked, skipping for now", name);
    psync_free(name);
    psync_free(localpath);
    psync_milisleep(PSYNC_SLEEP_ON_LOCKED_FILE);
    return -1;
  }
  rt=psync_get_remote_file_checksum(fileid, serverhashhex, &serversize, &hash);
  if (unlikely_log(rt!=PSYNC_NET_OK)){
    if (rt==PSYNC_NET_TEMPFAIL)
      goto err_sl_ex;
    else
      goto ret0;
  }
  memcpy(dwl->hash, serverhashhex, PSYNC_HASH_DIGEST_HEXLEN);
  pthread_mutex_lock(&current_downloads_mutex);
  while (starting_downloads || started_downloads>=PSYNC_MAX_PARALLEL_DOWNLOADS || 
         psync_status.bytestodownloadcurrent-psync_status.bytesdownloaded>PSYNC_START_NEW_DOWNLOADS_TRESHOLD){
    current_downloads_waiters++;
    pthread_cond_wait(&current_downloads_cond, &current_downloads_mutex);
    current_downloads_waiters--;
  }
  starting_downloads++;
  psync_status.filesdownloading++;
  pthread_mutex_unlock(&current_downloads_mutex);
  current_counter=&starting_downloads;
  downloadingcounted=1;
  result=psync_setting_get_uint(_PS(minlocalfreespace));
  if (result){
    freespace=psync_get_free_space_by_path(localpath);
    debug(D_NOTICE, "free space is %lu, needed %lu+%lu", (unsigned long)freespace, (unsigned long)result, (unsigned long)serversize);
    if (likely_log(freespace!=-1)){
      if (freespace>=result+serversize)
        psync_set_local_full(0);
      else{
        psync_set_local_full(1);
        psync_free(localpath);
        psync_free(name);
        psync_unlock_file(lock);
        task_dec_counter(current_counter, addedsize, downloadedsize, downloadingcounted);
        psync_milisleep(PSYNC_SLEEP_ON_DISK_FULL);
        return -1;
      }
    }
  }
  sql=psync_sql_query("SELECT fileid, id, hash FROM localfile WHERE size=? AND checksum=? AND localparentfolderid=? AND syncid=? AND name=?");
  psync_sql_bind_uint(sql, 1, serversize);
  psync_sql_bind_lstring(sql, 2, (char *)serverhashhex, PSYNC_HASH_DIGEST_HEXLEN);
  psync_sql_bind_uint(sql, 3, localfolderid);
  psync_sql_bind_uint(sql, 4, syncid);
  psync_sql_bind_string(sql, 5, filename);
  if ((row=psync_sql_fetch_rowint(sql))){
    rt=row[0]!=fileid || row[2]!=hash;
    result=row[1];
    psync_sql_free_result(sql);
    if (rt){
      sql=psync_sql_prep_statement("UPDATE localfile SET fileid=?, hash=? WHERE id=?");
      psync_sql_bind_uint(sql, 1, fileid);
      psync_sql_bind_uint(sql, 2, hash);
      psync_sql_bind_uint(sql, 3, result);
      psync_sql_run_free(sql);
    }
    goto ret0;
  }
  psync_sql_free_result(sql);
  if (psync_get_local_file_checksum(name, localhashhex, &localsize)==PSYNC_NET_OK){
    if (localsize==serversize && !memcmp(localhashhex, serverhashhex, PSYNC_HASH_DIGEST_HEXLEN)){
      if (unlikely_log(stat_and_create_local(syncid, fileid, localfolderid, filename, name, serverhashhex, serversize, hash)))
        goto err_sl_ex;
      else{
        debug(D_NOTICE, "file already exists %s, not downloading", name);
        goto ret0;
      }
    }
  }
  else
    localsize=0;
  sql=psync_sql_query("SELECT id FROM localfile WHERE size=? AND checksum=?");
  psync_sql_bind_uint(sql, 1, serversize);
  psync_sql_bind_lstring(sql, 2, (char *)serverhashhex, PSYNC_HASH_DIGEST_HEXLEN);
  while ((row=psync_sql_fetch_rowint(sql))){
    tmpname=psync_local_path_for_local_file(row[0], NULL);
    if (unlikely_log(!tmpname))
      continue;
    rt=psync_copy_local_file_if_checksum_matches(tmpname, name, serverhashhex, serversize);
    if (likely(rt==PSYNC_NET_OK)){
      if (unlikely_log(stat_and_create_local(syncid, fileid, localfolderid, filename, name, serverhashhex, serversize, hash)))
        rt=PSYNC_NET_TEMPFAIL;
      else
        debug(D_NOTICE, "file %s copied from %s", name, tmpname);
    }
    else
      debug(D_WARNING, "failed to copy %s from %s", name, tmpname);
    psync_free(tmpname);
    if (likely_log(rt==PSYNC_NET_OK)){
      psync_sql_free_result(sql);
      goto ret0;
    }
  }
  psync_sql_free_result(sql);
  
  if (dwl->stop)
    goto ret0;
  
  psync_send_event_by_id(PEVENT_FILE_DOWNLOAD_STARTED, syncid, name, fileid);
  pthread_mutex_lock(&current_downloads_mutex);
  psync_status.bytestodownloadcurrent+=serversize;
  starting_downloads--;
  started_downloads++;
  if (current_downloads_waiters)
    pthread_cond_signal(&current_downloads_cond);
  pthread_mutex_unlock(&current_downloads_mutex);
  addedsize=serversize;
  current_counter=&started_downloads;
  psync_status_send_update();
  
  tmpname=psync_strcat(localpath, PSYNC_DIRECTORY_SEPARATOR, filename, PSYNC_APPEND_PARTIAL_FILES, NULL);
  if (serversize>=PSYNC_MIN_SIZE_FOR_P2P){
    rt=psync_p2p_check_download(fileid, serverhashhex, serversize, tmpname);
    if (rt==PSYNC_NET_OK){
      psync_stop_localscan();
      if (unlikely_log(rename_if_notex(tmpname, name, fileid, localfolderid, syncid, filename)) || 
          unlikely_log(stat_and_create_local(syncid, fileid, localfolderid, filename, name, localhashhex, serversize, hash))){
        psync_resume_localscan();
        psync_free(tmpname);
        goto err_sl_ex;
      }
      psync_resume_localscan();
      psync_free(tmpname);
      goto ret0;
    }
    else if (rt==PSYNC_NET_TEMPFAIL){
      psync_free(tmpname);
      goto err_sl_ex;
    }
  }
  api=psync_apipool_get();
  if (unlikely_log(!api)){
    psync_free(tmpname);
    goto err_sl_ex;
  }
  res=send_command(api, "getfilelink", params);
  psync_apipool_release(api);
  if (unlikely_log(!res)){
    psync_free(tmpname);
    goto err_sl_ex;
  }
  result=psync_find_result(res, "result", PARAM_NUM)->num;
  if (unlikely(result)){
    debug(D_WARNING, "got error %lu from getfilelink", (long unsigned)result);
    if (psync_handle_api_result(result)==PSYNC_NET_TEMPFAIL)
      goto err0;
    else{
      psync_free(res);
      psync_free(tmpname);
      goto ret0;
    }
  }
  
  oldcnt=0;
  if (serversize>=PSYNC_MIN_SIZE_FOR_CHECKSUMS){
    if (!psync_stat(tmpname, &st) && psync_stat_size(&st)>=PSYNC_MIN_SIZE_FOR_CHECKSUMS){
      tmpold=psync_strcat(localpath, PSYNC_DIRECTORY_SEPARATOR, filename, "-old", PSYNC_APPEND_PARTIAL_FILES, NULL);
      if (psync_file_rename_overwrite(tmpname, tmpold)){
        psync_free(tmpold);
        tmpold=NULL;
      }
      else
        oldfiles[oldcnt++]=tmpold;
    }
    if (localsize>=PSYNC_MIN_SIZE_FOR_CHECKSUMS)
      oldfiles[oldcnt++]=name;
  }
  
  fd=psync_file_open(tmpname, P_O_WRONLY, P_O_CREAT|P_O_TRUNC);
  if (unlikely_log(fd==INVALID_HANDLE_VALUE))
    goto err0;
  
  rt=psync_net_download_ranges(&ranges, fileid, hash, serversize, oldfiles, oldcnt);
  if (rt==PSYNC_NET_TEMPFAIL)
    goto err1;
  
  hosts=psync_find_result(res, "hosts", PARAM_ARRAY);
  requestpath=psync_find_result(res, "path", PARAM_STR)->str;
  buff=psync_malloc(PSYNC_COPY_BUFFER_SIZE);
  http=NULL;
  psync_hash_init(&hashctx);
  psync_list_for_each_element(range, &ranges, psync_range_list_t, list){
    if (range->type==PSYNC_RANGE_TRANSFER){
      debug(D_NOTICE, "downloading %lu bytes from offset %lu", (unsigned long)range->len, (unsigned long)range->off);
      for (i=0; i<hosts->length; i++)
        if ((http=psync_http_connect(hosts->array[i]->str, requestpath, range->off, (range->len==serversize&&range->off==0)?0:(range->len+range->off-1))))
          break;
      if (unlikely_log(!http))
        goto err2;
      rd=0;
      while (!dwl->stop){
        rd=psync_http_readall(http, buff, PSYNC_COPY_BUFFER_SIZE);
        if (rd==0)
          break;
        if (unlikely_log(rd<0) ||
            unlikely_log(psync_file_writeall_checkoverquota(fd, buff, rd)))
          goto err2;
        psync_hash_update(&hashctx, buff, rd);
        pthread_mutex_lock(&current_downloads_mutex);
        psync_status.bytesdownloaded+=rd;
        if (current_downloads_waiters && psync_status.bytestodownloadcurrent-psync_status.bytesdownloaded<=PSYNC_START_NEW_DOWNLOADS_TRESHOLD)
          pthread_cond_signal(&current_downloads_cond);
        pthread_mutex_unlock(&current_downloads_mutex);
        psync_send_status_update();
        downloadedsize+=rd;
        if (unlikely(!psync_statuses_ok_array(requiredstatuses, ARRAY_SIZE(requiredstatuses))))
          goto err2;
      }
      psync_http_close(http);
      http=NULL;
    }
    else{
      debug(D_NOTICE, "copying %lu bytes from %s offset %lu", (unsigned long)range->len, range->filename, (unsigned long)range->off);
      ifd=psync_file_open(range->filename, P_O_RDONLY, 0);
      if (unlikely_log(ifd==INVALID_HANDLE_VALUE))
        goto err2;
      if (unlikely_log(psync_file_seek(ifd, range->off, P_SEEK_SET)==-1)){
        psync_file_close(ifd);
        goto err2;
      }
      result=range->len;
      while (!dwl->stop && result){
        if (result>PSYNC_COPY_BUFFER_SIZE)
          rd=PSYNC_COPY_BUFFER_SIZE;
        else
          rd=result;
        rd=psync_file_read(ifd, buff, rd);
        if (unlikely_log(rd<=0) || unlikely_log(psync_file_writeall_checkoverquota(fd, buff, rd)) || 
            unlikely(!psync_statuses_ok_array(requiredstatuses, ARRAY_SIZE(requiredstatuses)))){
          psync_file_close(ifd);
          goto err2;
        }
        result-=rd;
        psync_hash_update(&hashctx, buff, rd);
        pthread_mutex_lock(&current_downloads_mutex);
        psync_status.bytesdownloaded+=rd;
        if (current_downloads_waiters && psync_status.bytestodownloadcurrent-psync_status.bytesdownloaded<=PSYNC_START_NEW_DOWNLOADS_TRESHOLD)
          pthread_cond_signal(&current_downloads_cond);
        pthread_mutex_unlock(&current_downloads_mutex);
        psync_send_status_update();
        downloadedsize+=rd;
      }
      psync_file_close(ifd);
    }
    if (dwl->stop)
      break;
  }
  if (unlikely(dwl->stop)){
    psync_free(buff);
    psync_file_close(fd);
    psync_hash_final(localhashbin, &hashctx);
    psync_file_delete(tmpname);
    goto err0;
  }
  if (unlikely_log(psync_file_sync(fd)))
    goto err2;
  psync_free(buff);
  psync_hash_final(localhashbin, &hashctx);
  if (unlikely_log(psync_file_close(fd)))
    goto err0;
  psync_binhex(localhashhex, localhashbin, PSYNC_HASH_DIGEST_LEN);
  if (unlikely_log(memcmp(localhashhex, serverhashhex, PSYNC_HASH_DIGEST_HEXLEN))){
    debug(D_WARNING, "got wrong file checksum for file %s", filename);
    goto err0;
  }
  psync_stop_localscan();
  if (unlikely_log(rename_if_notex(tmpname, name, fileid, localfolderid, syncid, filename)) || 
      unlikely_log(stat_and_create_local(syncid, fileid, localfolderid, filename, name, localhashhex, serversize, hash))){
    psync_resume_localscan();
    goto err0;
  }
  psync_resume_localscan();
  psync_send_event_by_id(PEVENT_FILE_DOWNLOAD_FINISHED, syncid, name, fileid);
  debug(D_NOTICE, "file downloaded %s", name);
  task_dec_counter(current_counter, addedsize, downloadedsize, downloadingcounted);
  psync_list_for_each_element_call(&ranges, psync_range_list_t, list, psync_free);
  psync_unlock_file(lock);
  psync_free(name);
  if (tmpold){
    psync_file_delete(tmpold);
    psync_free(tmpold);
  }
  psync_free(tmpname);
  psync_free(localpath);
  psync_free(res);
  return 0;
err2:
  psync_hash_final(localhashbin, &hashctx); /* just in case */
  psync_free(buff);
  if (http)
    psync_http_close(http);
err1:
  psync_file_close(fd);
  psync_send_event_by_id(PEVENT_FILE_DOWNLOAD_FAILED, syncid, name, fileid);
err0:
  task_dec_counter(current_counter, addedsize, downloadedsize, downloadingcounted);
  psync_list_for_each_element_call(&ranges, psync_range_list_t, list, psync_free);
  if (tmpold){
    psync_file_delete(tmpold);
    psync_free(tmpold);
  }
  psync_free(tmpname);
  psync_free(localpath);
  psync_unlock_file(lock);
  psync_free(name);
  psync_free(res);
  return -1;
err_sl_ex:
  task_dec_counter(current_counter, addedsize, downloadedsize, downloadingcounted);
  psync_free(localpath);
  psync_unlock_file(lock);
  psync_free(name);
  psync_timer_notify_exception();
  psync_milisleep(PSYNC_SOCK_TIMEOUT_ON_EXCEPTION*1000);
  return -1;
ret0:
  task_dec_counter(current_counter, addedsize, downloadedsize, downloadingcounted);
  psync_free(localpath);
  psync_unlock_file(lock);
  psync_free(name);
  return 0;
}

static int task_delete_file(psync_syncid_t syncid, psync_fileid_t fileid, const char *remotepath){
  psync_sql_res *res, *stmt;
  psync_uint_row row;
  char *name;
  int ret;
  ret=0;
  task_wait_no_downloads();
  if (syncid){
    res=psync_sql_query("SELECT id, syncid FROM localfile WHERE fileid=? AND syncid=?");
    psync_sql_bind_uint(res, 2, syncid);
  }
  else
    res=psync_sql_query("SELECT id, syncid FROM localfile WHERE fileid=?");
  psync_sql_bind_uint(res, 1, fileid);
  psync_restart_localscan();
  while ((row=psync_sql_fetch_rowint(res))){
    name=psync_local_path_for_local_file(row[0], NULL);
    if (likely_log(name)){
      if (unlikely(psync_file_delete(name))){
        debug(D_WARNING, "error deleting local file %s error %d", name, (int)psync_fs_err());
        if (psync_fs_err()==P_BUSY || psync_fs_err()==P_ROFS){
          ret=-1;
          psync_free(name);
          continue;
        }
      }
      else
        debug(D_NOTICE, "local file %s deleted", name);
      psync_send_event_by_path(PEVENT_LOCAL_FILE_DELETED, row[1], name, fileid, remotepath);
      psync_free(name);
    }
    stmt=psync_sql_prep_statement("DELETE FROM localfile WHERE id=?");
    psync_sql_bind_uint(stmt, 1, row[0]);
    psync_sql_run_free(stmt);
  }
  psync_sql_free_result(res);
  return ret;
}

static int task_rename_file(psync_syncid_t oldsyncid, psync_syncid_t newsyncid, psync_fileid_t fileid, psync_folderid_t oldlocalfolderid,
                                  psync_folderid_t newlocalfolderid, const char *newname){
  char *oldpath, *newfolder, *newpath;
  psync_sql_res *res;
  psync_variant_row row;
  psync_fileid_t lfileid;
  psync_stat_t st;
  psync_syncid_t syncid;
  int ret;
  task_wait_no_downloads();
  res=psync_sql_query("SELECT id, localparentfolderid, syncid, name FROM localfile WHERE fileid=?");
  psync_sql_bind_uint(res, 1, fileid);
  lfileid=0;
  while ((row=psync_sql_fetch_row(res))){
    syncid=psync_get_number(row[2]);
    if (psync_get_number(row[1])==newlocalfolderid && syncid==newsyncid && !psync_filename_cmp(psync_get_string(row[3]), newname)){
      debug(D_NOTICE, "file %s already renamed locally, probably update initiated from this client", newname);
      psync_sql_free_result(res);
      return 0;
    }
    else if (syncid==oldsyncid){
      lfileid=psync_get_number(row[0]);
      break;
    }
  }
  psync_sql_free_result(res);
  if (unlikely_log(!lfileid)){
    psync_task_download_file(newsyncid, fileid, newlocalfolderid, newname);
    return 0;
  }
  newfolder=psync_local_path_for_local_folder(newlocalfolderid, newsyncid, NULL);
  if (unlikely_log(!newfolder))
    return 0;
  oldpath=psync_local_path_for_local_file(lfileid, NULL);
  if (unlikely_log(!oldpath)){
    psync_free(newfolder);
    return 0;
  }
  newpath=psync_strcat(newfolder, PSYNC_DIRECTORY_SEPARATOR, newname, NULL);
  ret=0;
  psync_stop_localscan();
  if (psync_file_rename_overwrite(oldpath, newpath)){
    psync_resume_localscan();
    if (psync_fs_err()==P_NOENT){
      debug(D_WARNING, "renamed from %s to %s failed, downloading", oldpath, newpath);
      psync_task_download_file(newsyncid, fileid, newlocalfolderid, newname);
    }
    else
      ret=-1;
  }
  else{
    if (likely_log(!psync_stat(newpath, &st))){
      res=psync_sql_prep_statement("UPDATE localfile SET localparentfolderid=?, syncid=?, name=?, inode=?, mtime=?, mtimenative=? WHERE id=?");
      psync_sql_bind_uint(res, 1, newlocalfolderid);
      psync_sql_bind_uint(res, 2, newsyncid);
      psync_sql_bind_string(res, 3, newname);
      psync_sql_bind_uint(res, 4, psync_stat_inode(&st));
      psync_sql_bind_uint(res, 5, psync_stat_mtime(&st));
      psync_sql_bind_uint(res, 6, psync_stat_mtime_native(&st));
      psync_sql_bind_uint(res, 7, lfileid);
      psync_sql_run_free(res);
      debug(D_NOTICE, "renamed %s to %s", oldpath, newpath);
    }
    psync_resume_localscan();
  }
  psync_free(newpath);
  psync_free(oldpath);
  psync_free(newfolder);
  return ret;
}

static void task_run_download_file_thread(void *ptr){
  download_task_t *dt;
  psync_sql_res *res;
  dt=(download_task_t *)ptr;
  if (task_download_file(dt->dwllist.syncid, dt->dwllist.fileid, dt->localfolderid, dt->filename, &dt->dwllist)){
    psync_milisleep(PSYNC_SLEEP_ON_FAILED_DOWNLOAD);
    res=psync_sql_prep_statement("UPDATE task SET inprogress=0 WHERE id=?");
    psync_sql_bind_uint(res, 1, dt->taskid);
    psync_sql_run_free(res);
    psync_wake_download();
  }
  else{
    res=psync_sql_prep_statement("DELETE FROM task WHERE id=?");
    psync_sql_bind_uint(res, 1, dt->taskid);
    psync_sql_run_free(res);
    psync_status_recalc_to_download();
    psync_send_status_update();
  }
  pthread_mutex_lock(&current_downloads_mutex);
  psync_list_del(&dt->dwllist.list);
  pthread_mutex_unlock(&current_downloads_mutex);
  psync_free(dt);
}

static int task_run_download_file(uint64_t taskid, psync_syncid_t syncid, psync_fileid_t fileid, psync_folderid_t localfolderid, const char *filename){
  psync_sql_res *res;
  download_task_t *dt;
  size_t len;
  res=psync_sql_prep_statement("UPDATE task SET inprogress=1 WHERE id=?");
  psync_sql_bind_uint(res, 1, taskid);
  psync_sql_run_free(res);
  len=strlen(filename);
  dt=(download_task_t *)psync_malloc(offsetof(download_task_t, filename)+len+1);
  dt->taskid=taskid;
  dt->dwllist.fileid=fileid;
  dt->dwllist.syncid=syncid;
  dt->dwllist.stop=0;
  dt->dwllist.hash[0]=0;
  dt->localfolderid=localfolderid;
  memcpy(dt->filename, filename, len+1);
  pthread_mutex_lock(&current_downloads_mutex);
  psync_list_add_tail(&downloads, &dt->dwllist.list);
  while (!dt->dwllist.stop && (starting_downloads || started_downloads>=PSYNC_MAX_PARALLEL_DOWNLOADS || 
         psync_status.bytestodownloadcurrent-psync_status.bytesdownloaded>PSYNC_START_NEW_DOWNLOADS_TRESHOLD)){
    current_downloads_waiters++;
    pthread_cond_wait(&current_downloads_cond, &current_downloads_mutex);
    current_downloads_waiters--;
  }
  if (unlikely(dt->dwllist.stop))
    psync_list_del(&dt->dwllist.list);
  pthread_mutex_unlock(&current_downloads_mutex);
  if (unlikely(dt->dwllist.stop)){
    psync_free(dt);
    res=psync_sql_prep_statement("UPDATE task SET inprogress=0 WHERE id=?");
    psync_sql_bind_uint(res, 1, taskid);
    psync_sql_run_free(res);
  }
  else
    psync_run_thread1("download file", task_run_download_file_thread, dt);
  return -1;
}

static void task_del_folder_rec_do(const char *localpath, psync_folderid_t localfolderid, psync_syncid_t syncid){
  psync_sql_res *res;
  psync_variant_row vrow;
  char *nm;
  res=psync_sql_query("SELECT id, name FROM localfile WHERE localparentfolderid=? AND syncid=?");
  psync_sql_bind_uint(res, 1, localfolderid);
  psync_sql_bind_uint(res, 2, syncid);
  while ((vrow=psync_sql_fetch_row(res))){
    psync_delete_upload_tasks_for_file(psync_get_number(vrow[0]));
    nm=psync_strcat(localpath, PSYNC_DIRECTORY_SEPARATOR, psync_get_string(vrow[1]), NULL);
    debug(D_NOTICE, "deleting %s", nm);
    psync_file_delete(nm);
    psync_free(nm);
  }
  psync_sql_free_result(res);
  res=psync_sql_prep_statement("DELETE FROM localfile WHERE localparentfolderid=? AND syncid=?");
  psync_sql_bind_uint(res, 1, localfolderid);
  psync_sql_bind_uint(res, 2, syncid);
  psync_sql_run_free(res);
  res=psync_sql_query("SELECT id, name FROM localfolder WHERE localparentfolderid=? AND syncid=?");
  psync_sql_bind_uint(res, 1, localfolderid);
  psync_sql_bind_uint(res, 2, syncid);
  while ((vrow=psync_sql_fetch_row(res))){
    nm=psync_strcat(localpath, PSYNC_DIRECTORY_SEPARATOR, psync_get_string(vrow[1]), NULL);
    task_del_folder_rec_do(nm, psync_get_number(vrow[0]), syncid);
    psync_free(nm);
  }
  psync_sql_free_result(res);
  res=psync_sql_prep_statement("DELETE FROM localfolder WHERE localparentfolderid=? AND syncid=?");
  psync_sql_bind_uint(res, 1, localfolderid);
  psync_sql_bind_uint(res, 2, syncid);
  psync_sql_run_free(res);
  res=psync_sql_prep_statement("DELETE FROM syncedfolder WHERE localfolderid=? AND syncid=?");
  psync_sql_bind_uint(res, 1, localfolderid);
  psync_sql_bind_uint(res, 2, syncid);
  psync_sql_run_free(res);
}

static int task_del_folder_rec(psync_folderid_t localfolderid, psync_folderid_t folderid, psync_syncid_t syncid){
  char *localpath;
  psync_sql_res *res;
  task_wait_no_downloads();
  psync_stop_localscan();
  localpath=psync_local_path_for_local_folder(localfolderid, syncid, NULL);
  if (unlikely_log(!localpath)){
    psync_sql_unlock();
    return 0;
  }
  debug(D_NOTICE, "got recursive delete for localfolder %lu %s", (unsigned long)localfolderid, localpath);
  psync_sql_start_transaction();
  task_del_folder_rec_do(localpath, localfolderid, syncid);
  res=psync_sql_prep_statement("DELETE FROM localfolder WHERE id=? AND syncid=?");
  psync_sql_bind_uint(res, 1, localfolderid);
  psync_sql_bind_uint(res, 2, syncid);
  psync_sql_run_free(res);
  psync_sql_commit_transaction();
  psync_rmdir_with_trashes(localpath);
  psync_resume_localscan();
  return 0;
}
  
static int download_task(uint64_t taskid, uint32_t type, psync_syncid_t syncid, uint64_t itemid, uint64_t localitemid, uint64_t newitemid, const char *name,
                                        psync_syncid_t newsyncid){
  int res;
  switch (type) {
    case PSYNC_CREATE_LOCAL_FOLDER:
      res=call_func_for_folder(localitemid, itemid, syncid, PEVENT_LOCAL_FOLDER_CREATED, task_mkdir, 1, "local folder created");
      break;
    case PSYNC_DELETE_LOCAL_FOLDER:
      res=call_func_for_folder_name(localitemid, itemid, name, syncid, PEVENT_LOCAL_FOLDER_DELETED, task_rmdir, 0, "local folder deleted");
      if (!res){
        psync_sql_start_transaction();
        delete_local_folder_from_db(localitemid);
        psync_sql_commit_transaction();
      }
      break;
    case PSYNC_DELREC_LOCAL_FOLDER:
      res=task_del_folder_rec(localitemid, itemid, syncid);
      break;
    case PSYNC_RENAME_LOCAL_FOLDER:
      res=task_renamefolder(syncid, itemid, localitemid, newitemid, name);
      break;
    case PSYNC_DOWNLOAD_FILE:
      res=task_run_download_file(taskid, syncid, itemid, localitemid, name);
      break;
    case PSYNC_DELETE_LOCAL_FILE:
      res=task_delete_file(syncid, itemid, name);
      break;
    case PSYNC_RENAME_LOCAL_FILE:
      res=task_rename_file(syncid, newsyncid, itemid, localitemid, newitemid, name);
      break;
    default:
      debug(D_BUG, "invalid task type %u", (unsigned)type);
      res=0;
  }
  if (res && type!=PSYNC_DOWNLOAD_FILE)
    debug(D_WARNING, "task of type %u, syncid %u, id %lu localid %lu failed", (unsigned)type, (unsigned)syncid, (unsigned long)itemid, (unsigned long)localitemid);
  return res;
}

static void download_thread(){
  psync_sql_res *res;
  psync_variant *row;
  uint64_t taskid;
  uint32_t type;
  while (psync_do_run){
    psync_wait_statuses_array(requiredstatuses, ARRAY_SIZE(requiredstatuses));
    
    row=psync_sql_row("SELECT id, type, syncid, itemid, localitemid, newitemid, name, newsyncid FROM task WHERE "
                      "inprogress=0 AND type&"NTO_STR(PSYNC_TASK_DWLUPL_MASK)"="NTO_STR(PSYNC_TASK_DOWNLOAD)" ORDER BY id LIMIT 1");
    if (row){
      taskid=psync_get_number(row[0]);
      type=psync_get_number(row[1]);
      if (!download_task(taskid, type, 
                         psync_get_number_or_null(row[2]), 
                         psync_get_number(row[3]), 
                         psync_get_number(row[4]), 
                         psync_get_number_or_null(row[5]),                          
                         psync_get_string_or_null(row[6]),
                         psync_get_number_or_null(row[7]))){
        res=psync_sql_prep_statement("DELETE FROM task WHERE id=?");
        psync_sql_bind_uint(res, 1, taskid);
        psync_sql_run_free(res);
      }
      else if (type!=PSYNC_DOWNLOAD_FILE)
        psync_milisleep(PSYNC_SLEEP_ON_FAILED_DOWNLOAD);
      psync_free(row);
      continue;
    }

    pthread_mutex_lock(&download_mutex);
    if (!download_wakes)
      pthread_cond_wait(&download_cond, &download_mutex);
    download_wakes=0;
    pthread_mutex_unlock(&download_mutex);
  }
}

void psync_wake_download(){
  pthread_mutex_lock(&download_mutex);
  if (!download_wakes++)
    pthread_cond_signal(&download_cond);
  pthread_mutex_unlock(&download_mutex);  
}

void psync_download_init(){
  psync_timer_exception_handler(psync_wake_download);
  psync_run_thread("download main", download_thread);
}

void psync_delete_download_tasks_for_file(psync_fileid_t fileid){
  psync_sql_res *res;
  download_list_t *dwl;
  res=psync_sql_prep_statement("DELETE FROM task WHERE type=? AND itemid=?");
  psync_sql_bind_uint(res, 1, PSYNC_DOWNLOAD_FILE);
  psync_sql_bind_uint(res, 2, fileid);
  psync_sql_run(res);
  if (psync_sql_affected_rows()){
    psync_status_recalc_to_download();
    psync_send_status_update();
  }
  psync_sql_free_result(res);
  pthread_mutex_lock(&current_downloads_mutex);
  psync_list_for_each_element(dwl, &downloads, download_list_t, list)
    if (dwl->fileid==fileid)
      dwl->stop=1;
  pthread_mutex_unlock(&current_downloads_mutex);
}

void psync_stop_file_download(psync_fileid_t fileid, psync_syncid_t syncid){
  download_list_t *dwl;
  pthread_mutex_lock(&current_downloads_mutex);
  psync_list_for_each_element(dwl, &downloads, download_list_t, list)
    if (dwl->fileid==fileid && dwl->syncid==syncid)
      dwl->stop=1;
  pthread_mutex_unlock(&current_downloads_mutex);
}

void psync_stop_sync_download(psync_syncid_t syncid){
  download_list_t *dwl;
  psync_sql_res *res;
  res=psync_sql_prep_statement("DELETE FROM task WHERE syncid=? AND type&"NTO_STR(PSYNC_TASK_DWLUPL_MASK)"="NTO_STR(PSYNC_TASK_DOWNLOAD));
  psync_sql_bind_uint(res, 1, syncid);
  psync_sql_run_free(res);
  psync_status_recalc_to_download();
  psync_send_status_update();
  pthread_mutex_lock(&current_downloads_mutex);
  psync_list_for_each_element(dwl, &downloads, download_list_t, list)
    if (dwl->syncid==syncid)
      dwl->stop=1;
  pthread_mutex_unlock(&current_downloads_mutex);
}

void psync_stop_all_download(){
  download_list_t *dwl;
  pthread_mutex_lock(&current_downloads_mutex);
  psync_list_for_each_element(dwl, &downloads, download_list_t, list)
    dwl->stop=1;
  pthread_mutex_unlock(&current_downloads_mutex);
}

downloading_files_hashes *psync_get_downloading_hashes(){
  download_list_t *dwl;
  downloading_files_hashes *ret;
  size_t cnt;
  cnt=0;
  pthread_mutex_lock(&current_downloads_mutex);
  psync_list_for_each_element(dwl, &downloads, download_list_t, list)
    cnt++;
  ret=(downloading_files_hashes *)psync_malloc(offsetof(downloading_files_hashes, hashes)+sizeof(psync_hex_hash)*cnt);
  cnt=0;
  psync_list_for_each_element(dwl, &downloads, download_list_t, list)
    if (dwl->hash[0]){
      memcpy(ret->hashes[cnt], dwl->hash, PSYNC_HASH_DIGEST_HEXLEN);
      cnt++;
    }
  ret->hashcnt=cnt;
  pthread_mutex_unlock(&current_downloads_mutex);
  return ret;
}

