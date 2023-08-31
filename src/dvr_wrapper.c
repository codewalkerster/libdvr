#include <stddef.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/prctl.h>
#include <time.h>

#include "dvr_types.h"
#include "dvr_record.h"
#include "dvr_crypto.h"
#include "dvr_playback.h"
#include "dvr_segment.h"
#include "dvr_utils.h"

#include "AmTsPlayer.h"

#include "list.h"

#include "dvr_wrapper.h"

#define WRAPPER_LOG_TAG "libdvr-wrapper"
#define DVR_WRAPPER_DEBUG(...) DVR_LOG_PRINT(LOG_LV_DEBUG, WRAPPER_LOG_TAG, __VA_ARGS__)
#define DVR_WRAPPER_INFO(...) DVR_LOG_PRINT(LOG_LV_INFO, WRAPPER_LOG_TAG, __VA_ARGS__)
#define DVR_WRAPPER_WARN(...) DVR_LOG_PRINT(LOG_LV_WARN, WRAPPER_LOG_TAG, __VA_ARGS__)
#define DVR_WRAPPER_ERROR(...) DVR_LOG_PRINT(LOG_LV_ERROR, WRAPPER_LOG_TAG, __VA_ARGS__)
#define DVR_WRAPPER_FATAL(...) DVR_LOG_PRINT(LOG_LV_FATAL, WRAPPER_LOG_TAG, __VA_ARGS__)

/*duration of data to resume if paused with EVT_REACHED_END in timeshifting*/
#define TIMESHIFT_DATA_DURATION_TO_RESUME (600)
/*a tolerant gap*/
#define DVR_PLAYBACK_END_GAP              (1000)

int g_dvr_log_level = LOG_LV_DEFAULT;

enum {
  W_REC      = 1,
  W_PLAYBACK = 2,
};

enum {
  U_PIDS     = 0x01,
  U_STAT     = 0x02,
  U_ALL      = U_PIDS | U_STAT,
};

typedef struct {
  pthread_mutex_t lock;
  pthread_cond_t  cond;
  int             inited;
  int             locked;
} DVR_WrapperMutex_t;

static void
wrapper_mutex_init (DVR_WrapperMutex_t *lock)
{
  pthread_condattr_t cattr;

  if (lock->inited)
    return;

  pthread_mutex_init(&lock->lock, NULL);

  pthread_condattr_init(&cattr);
  pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
  pthread_cond_init(&lock->cond, &cattr);
  pthread_condattr_destroy(&cattr);

  // It is not necessary to protect code block below with
  // DVR_WrapperMutex_t.lock, so the following annotation
  // is given to suppress related Coverity complaint.
  // coverity[missing_lock]
  lock->locked = 0;
  lock->inited = 1;
}

static int
wrapper_mutex_lock (DVR_WrapperMutex_t *lock)
{
  pthread_mutex_lock(&lock->lock);
  // This couldn't be a infinite loop as Coverity reported.
  // Loop can finish when another thread calls wrapper_mutex_unlock.
  // coverity[loop_condition]
  while (lock->locked) {
    pthread_cond_wait(&lock->cond, &lock->lock);
  }
  lock->locked = 1;
  pthread_mutex_unlock(&lock->lock);

  return 0;
}

static int
wrapper_mutex_unlock (DVR_WrapperMutex_t *lock)
{
  pthread_mutex_lock(&lock->lock);
  lock->locked = 0;
  pthread_mutex_unlock(&lock->lock);
  pthread_cond_signal(&lock->cond);

  return 0;
}

static int
wrapper_mutex_timedlock (DVR_WrapperMutex_t *lock, struct timespec *tv)
{
  int r = 0;

  pthread_mutex_lock(&lock->lock);
  if (lock->locked) {
    //DVR_WRAPPER_DEBUG("Enter cond_timedwait");
    r = pthread_cond_timedwait(&lock->cond, &lock->lock, tv);
    //DVR_WRAPPER_DEBUG("Leave cond_timedwait");
  }
  if (r == 0) {
    if (!lock->locked) {
      lock->locked = 1;
    } else {
      r = ETIMEDOUT;
    }
  }
  pthread_mutex_unlock(&lock->lock);

  return r;
}

#define WRAPPER_RETURN_IF_FALSE_WITH_UNLOCK(expr, lock)\
  do {\
    if (!(expr)) {\
      DVR_INFO("%s-%d failed", __func__, __LINE__);\
      wrapper_mutex_unlock(lock);\
      return DVR_FAILURE;\
    }\
  } while (0);


typedef struct {
  /*make lock the 1st item in the structure*/
  DVR_WrapperMutex_t            wrapper_lock;

  /*rec or play*/
  int                           type;

  /*valid if (sn != 0)*/
  unsigned long                 sn;
  unsigned long                 sn_linked;

  struct list_head              segments;                    /**<head-add list*/
  uint64_t                      current_segment_id;          /**<id of the current segment*/

  union {
    struct {
      DVR_WrapperRecordOpenParams_t   param_open;
      DVR_RecordStartParams_t         param_start;
      DVR_RecordStartParams_t         param_update;
      DVR_RecordHandle_t              recorder;
      DVR_RecordEventFunction_t       event_fn;
      void                            *event_userdata;

      /*total status = seg_status + status + obsolete*/
      DVR_RecordStatus_t              seg_status;            /**<status of current segment*/
      DVR_WrapperRecordStatus_t       status;                /**<status of remaining segments*/
      uint64_t                        next_segment_id;

      DVR_WrapperInfo_t               obsolete;             /**<data obsolete due to the max limit*/
    } record;

    struct {
      DVR_WrapperPlaybackOpenParams_t param_open;
      DVR_PlaybackHandle_t            player;
      DVR_PlaybackEventFunction_t     event_fn;
      void                            *event_userdata;

      /*total status = seg_status + status*/
      DVR_PlaybackStatus_t            seg_status;
      DVR_WrapperPlaybackStatus_t     status;
      DVR_PlaybackPids_t              pids_req;
      DVR_PlaybackEvent_t             last_event;
      float                           speed;
      DVR_Bool_t                      reach_end;

      DVR_WrapperInfo_t               obsolete;
      DVR_Bool_t                      tf_full;
    } playback;
  };
} DVR_WrapperCtx_t;

typedef struct {
  struct list_head head;
  unsigned long sn;

  /* rec or playback */
  int type;

  union {
    struct {
      DVR_RecordEvent_t event;
      DVR_RecordStatus_t status;
    } record;
    struct {
      DVR_PlaybackEvent_t event;
      DVR_Play_Notify_t status;
    } playback;
  };
} DVR_WrapperEventCtx_t;

typedef struct {
  pthread_mutex_t lock;
  char            *name;
  int             running;
  pthread_cond_t  cond;
  pthread_t       thread;
  int             type;
} DVR_WrapperThreadCtx_t;

typedef struct {
  struct list_head head;

  DVR_RecordSegmentInfo_t seg_info;
  DVR_PlaybackSegmentInfo_t playback_info;
} DVR_WrapperPlaybackSegmentInfo_t;

typedef struct {
  struct list_head head;

  DVR_RecordSegmentInfo_t info;
} DVR_WrapperRecordSegmentInfo_t;

/* serial num generater */
static unsigned long sn = 1;
static pthread_mutex_t sn_lock = PTHREAD_MUTEX_INITIALIZER;

static inline unsigned long get_sn()
{
  unsigned long no = 0;

  pthread_mutex_lock(&sn_lock);
  no = sn++;
  if (!no)
    no = sn++;
  pthread_mutex_unlock(&sn_lock);
  return no;
}

/* entity ctx */
#define DVR_WRAPPER_MAX 10

static DVR_WrapperCtx_t record_list[DVR_WRAPPER_MAX] =
{
  [0 ... (DVR_WRAPPER_MAX - 1)] =
  {
    .type = W_REC,
  }
};

static DVR_WrapperCtx_t playback_list[DVR_WRAPPER_MAX] =
{
  [0 ... (DVR_WRAPPER_MAX - 1)] =
  {
    .type = W_PLAYBACK,
  }
};

/* events lists */
static struct list_head record_evt_list = LIST_HEAD_INIT(record_evt_list);
static struct list_head playback_evt_list = LIST_HEAD_INIT(playback_evt_list);

static pthread_mutex_t record_evt_list_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t playback_evt_list_lock = PTHREAD_MUTEX_INITIALIZER;

static DVR_WrapperThreadCtx_t wrapper_thread[2] =
{
  [0] =
  {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .running = 0,
    .name = "record",
    .type = W_REC,
  },
  [1] =
  {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .running = 0,
    .name = "playback",
    .type = W_PLAYBACK,
  },
};

/*now only support one timeshift now*/
static unsigned long sn_timeshift_record;
static unsigned long sn_timeshift_playback;

static void *wrapper_task(void *arg);
static inline int process_handleEvents(DVR_WrapperEventCtx_t *evt, DVR_WrapperCtx_t *ctx);

static DVR_Result_t wrapper_record_event_handler(DVR_RecordEvent_t event, void *params, void *userdata);
static DVR_Result_t wrapper_playback_event_handler(DVR_PlaybackEvent_t event, void *params, void *userdata);

static int process_generateRecordStatus(DVR_WrapperCtx_t *ctx, DVR_WrapperRecordStatus_t *status);
static int process_generatePlaybackStatus(DVR_WrapperCtx_t *ctx, DVR_WrapperPlaybackStatus_t *status);

static int get_timespec_timeout(int timeout, struct timespec *ts)
{
  struct timespec ots;
  int left, diff;

  clock_gettime(CLOCK_MONOTONIC, &ots);

  ts->tv_sec  = ots.tv_sec + timeout / 1000;
  ts->tv_nsec = ots.tv_nsec;

  left = timeout % 1000;
  left *= 1000000;
  diff = 1000000000 - ots.tv_nsec;

  if (diff <= left) {
    ts->tv_sec++;
    ts->tv_nsec = left-diff;
  } else {
    ts->tv_nsec += left;
  }

  return 0;
}

static DVR_WrapperEventCtx_t *ctx_getEvent(struct list_head *list, pthread_mutex_t *list_lock)
{
  DVR_WrapperEventCtx_t *p_evt;

  pthread_mutex_lock(list_lock);
  if (list_empty(list))
    p_evt = NULL;
  else {
    p_evt = list_first_entry(list, DVR_WrapperEventCtx_t, head);
    list_del(&p_evt->head);
  }
  pthread_mutex_unlock(list_lock);

  return p_evt;
}

static inline DVR_WrapperEventCtx_t *ctx_getRecordEvent()
{
  return ctx_getEvent(&record_evt_list, &record_evt_list_lock);
}

static inline DVR_WrapperEventCtx_t *ctx_getPlaybackEvent()
{
  return ctx_getEvent(&playback_evt_list, &playback_evt_list_lock);
}

static int ctx_addEvent(struct list_head *list, pthread_mutex_t *lock, DVR_WrapperEventCtx_t *evt)
{
  DVR_WrapperEventCtx_t *padd;
  padd = (DVR_WrapperEventCtx_t *)calloc(1, sizeof(DVR_WrapperEventCtx_t));
  DVR_RETURN_IF_FALSE(padd);

  *padd = *evt;
  pthread_mutex_lock(lock);
  list_add_tail(padd, list);
  pthread_mutex_unlock(lock);
  return DVR_SUCCESS;
}

static inline void ctx_freeEvent(DVR_WrapperEventCtx_t *evt)
{
  free(evt);
}

/*useless*/
static void ctx_cleanOutdatedEvents(struct list_head *evt_list,
    pthread_mutex_t *evt_list_lock,
    DVR_WrapperCtx_t *list)
{
  DVR_WrapperEventCtx_t *p_evt, *p_evt_tmp;
  unsigned long sns[DVR_WRAPPER_MAX];
  int cnt = 0;
  int i;
  int found = 0;

  /*copy all valid sns*/
  for (i = 0; i < DVR_WRAPPER_MAX; i++) {
    sns[cnt] = list[i].sn;
    if (!sns[cnt])
      cnt++;
  }

  /*free evts that not belong to any valid sns*/
  pthread_mutex_lock(evt_list_lock);
  list_for_each_entry_safe(p_evt, p_evt_tmp, evt_list, head) {
    for (i = 0; i < cnt; i++) {
      if (p_evt->sn == sns[i]) {
        found = 1;
        break;
      }
    }
    if (!found) {
      list_del(&p_evt->head);
      ctx_freeEvent(p_evt);
    }
  }
  pthread_mutex_unlock(evt_list_lock);
}

static inline void ctx_cleanOutdatedRecordEvents()
{
  ctx_cleanOutdatedEvents(&record_evt_list, &record_evt_list_lock, record_list);
}

static inline void ctx_cleanOutdatedPlaybackEvents()
{
  ctx_cleanOutdatedEvents(&playback_evt_list, &playback_evt_list_lock, playback_list);
}

//check this play is recording file
//return 0 if not the recording
//else return record id
static inline int ctx_isPlay_recording(char *play_location)
{
  int i;
  DVR_WrapperCtx_t *cnt;

  for (i = 0; i < DVR_WRAPPER_MAX; i++) {
    cnt = &record_list[i];
    //DVR_WRAPPER_INFO("[%d]sn[%d]R:[%s]P:[%s] ...\n", i, cnt->sn, cnt->record.param_open.location, play_location);
    if (!strcmp(cnt->record.param_open.location, play_location)) {
      DVR_WRAPPER_INFO("[%d]sn[%d]R:[%s]P:[%s] .found..\n", i, cnt->sn, cnt->record.param_open.location, play_location);
      return cnt->sn;
    }
  }
  DVR_WRAPPER_INFO(" not found any play is in recording [%d]", DVR_WRAPPER_MAX);
  return 0;
}

// Check if the given record is being played.
// Return 0 if it is not being played, otherwise return its playback id.
static inline int ctx_isRecord_playing(char *rec_location)
{
    int i;
    DVR_WrapperCtx_t *cnt;
    for (i = 0; i < DVR_WRAPPER_MAX; i++) {
      cnt = &playback_list[i];
      //DVR_WRAPPER_INFO("[%d]sn[%d]P[%s]R[%s] ...\n", i, cnt->sn, cnt->playback.param_open.location, rec_location);
      if (!strcmp(cnt->playback.param_open.location, rec_location)) {
        DVR_WRAPPER_DEBUG("[%d]sn[%d]P[%s]R[%s] ..found.",
            i, cnt->sn, cnt->playback.param_open.location, rec_location);
        return cnt->sn;
      }
    }
    return 0;
}

static inline DVR_WrapperCtx_t *ctx_get(unsigned long sn, DVR_WrapperCtx_t *list)
{
  int i;
  for (i = 0; i < DVR_WRAPPER_MAX; i++) {
    if (list[i].sn == sn) {
      wrapper_mutex_init(&list[i].wrapper_lock);
      return &list[i];
    }
  }
  return NULL;
}

static inline void ctx_reset(DVR_WrapperCtx_t *ctx)
{
  memset((char *)ctx + offsetof(DVR_WrapperCtx_t, sn),
    0,
    sizeof(DVR_WrapperCtx_t) - offsetof(DVR_WrapperCtx_t, sn));
}

static inline int ctx_valid(DVR_WrapperCtx_t *ctx)
{
  return (ctx->sn != 0);
}

static inline DVR_WrapperCtx_t *ctx_getRecord(unsigned long sn)
{
  return ctx_get(sn, record_list);
}

static inline DVR_WrapperCtx_t *ctx_getPlayback(unsigned long sn)
{
  return ctx_get(sn, playback_list);
}

static int wrapper_requestThread(DVR_WrapperThreadCtx_t *ctx, void *(thread_fn)(void *))
{
  pthread_mutex_lock(&ctx->lock);
  ctx->running++;
  if (ctx->running == 1) {
    pthread_condattr_t attr = {0};
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&ctx->cond, &attr);
    pthread_condattr_destroy(&attr);
    DVR_WRAPPER_INFO("start wrapper thread(%s) ...\n", ctx->name);
    pthread_create(&ctx->thread, NULL, thread_fn, ctx);
    DVR_WRAPPER_INFO("wrapper thread(%s) started\n", ctx->name);
  }
  pthread_mutex_unlock(&ctx->lock);
  return 0;
}

static int wrapper_releaseThread(DVR_WrapperThreadCtx_t *ctx)
{
  pthread_mutex_lock(&ctx->lock);
  ctx->running--;
  if (!ctx->running) {
    pthread_cond_broadcast(&ctx->cond);
    pthread_mutex_unlock(&ctx->lock);

    DVR_WRAPPER_INFO("stop wrapper thread(%s) ...\n", ctx->name);
    pthread_join(ctx->thread, NULL);
    DVR_WRAPPER_INFO("wrapper thread(%s) stopped\n", ctx->name);

    pthread_mutex_lock(&ctx->lock);
    if (!ctx->running) /*protect*/
        pthread_cond_destroy(&ctx->cond);
  }
  pthread_mutex_unlock(&ctx->lock);
  return 0;
}

#define WRAPPER_THREAD_RECORD    (&wrapper_thread[0])
#define WRAPPER_THREAD_PLAYBACK  (&wrapper_thread[1])

static inline int wrapper_requestThreadFor(DVR_WrapperCtx_t *ctx)
{
  DVR_WrapperThreadCtx_t *thread_ctx = (ctx->type == W_REC)?
    WRAPPER_THREAD_RECORD : WRAPPER_THREAD_PLAYBACK;
  return wrapper_requestThread(thread_ctx, wrapper_task);
}

static inline int wrapper_releaseThreadFor(DVR_WrapperCtx_t *ctx)
{
  DVR_WrapperThreadCtx_t *thread_ctx = (ctx->type == W_REC)?
    WRAPPER_THREAD_RECORD : WRAPPER_THREAD_PLAYBACK;
  return wrapper_releaseThread(thread_ctx);
}

static inline int wrapper_releaseThreadForType(int type)
{
  DVR_WrapperThreadCtx_t *thread_ctx = (type == W_REC)?
    WRAPPER_THREAD_RECORD : WRAPPER_THREAD_PLAYBACK;
  return wrapper_releaseThread(thread_ctx);
}

static inline void wrapper_threadSignal(DVR_WrapperThreadCtx_t *thread_ctx)
{
  pthread_cond_signal(&thread_ctx->cond);
}

static inline int wrapper_threadWait(DVR_WrapperThreadCtx_t *thread_ctx)
{
  struct timespec rt;
  get_timespec_timeout(200, &rt);
  pthread_cond_timedwait(&thread_ctx->cond, &thread_ctx->lock, &rt);
  return 0;
}

static inline void wrapper_threadSignalForType(int type)
{
  DVR_WrapperThreadCtx_t *thread_ctx = (type == W_REC) ?
    WRAPPER_THREAD_RECORD : WRAPPER_THREAD_PLAYBACK;
  wrapper_threadSignal(thread_ctx);
}

static inline void wrapper_threadSignalFor(DVR_WrapperCtx_t *ctx)
{
  DVR_WrapperThreadCtx_t *thread_ctx = (ctx->type == W_REC) ?
    WRAPPER_THREAD_RECORD : WRAPPER_THREAD_PLAYBACK;
  wrapper_threadSignal(thread_ctx);
}

static inline int wrapper_threadWaitFor(DVR_WrapperCtx_t *ctx)
{
  DVR_WrapperThreadCtx_t *thread_ctx = (ctx->type == W_REC) ?
    WRAPPER_THREAD_RECORD : WRAPPER_THREAD_PLAYBACK;
  wrapper_threadWait(thread_ctx);
  return 0;
}

/*return condition, locked if condition == true*/
static int wrapper_mutex_lock_if(DVR_WrapperMutex_t *lock, int *condition)
{
  int r2;
  do {
    struct timespec rt2;
    get_timespec_timeout(10, &rt2);
    r2 = wrapper_mutex_timedlock(lock, &rt2);
  } while (*condition && (r2 == ETIMEDOUT));

  if (!(*condition) && (r2 == 0))
    wrapper_mutex_unlock(lock);

  return *condition;
}

static void *wrapper_task(void *arg)
{
  DVR_WrapperThreadCtx_t *thread_ctx = (DVR_WrapperThreadCtx_t *)arg;
  DVR_WrapperEventCtx_t *evt;

  prctl(PR_SET_NAME,"DvrWrapper");

  while (thread_ctx->running) {
    int ret;

    evt = (thread_ctx->type == W_REC)? ctx_getRecordEvent() : ctx_getPlaybackEvent();
    if (!evt) {
      pthread_mutex_lock(&thread_ctx->lock);
      ret = wrapper_threadWait(thread_ctx);
      pthread_mutex_unlock(&thread_ctx->lock);
    }

    while (evt) {
      DVR_WrapperCtx_t *ctx = (evt->type == W_REC)?
        ctx_getRecord(evt->sn) : ctx_getPlayback(evt->sn);
      if (ctx == NULL) {
        DVR_WRAPPER_ERROR("Wrapper context is NULL");
        goto processed;
      }
      DVR_WRAPPER_DEBUG("start name(%s) sn(%d) running(%d) type(%d)\n", thread_ctx->name, (int)ctx->sn, thread_ctx->running, thread_ctx->type);
      if (thread_ctx->running) {
        /*
          continue not break,
          make all events consumed, or mem leak
        */
        if (!wrapper_mutex_lock_if(&ctx->wrapper_lock, &thread_ctx->running))
            goto processed;

        if (ctx_valid(ctx)) {
          /*double check after lock*/
          if (evt->sn == ctx->sn) {
            process_handleEvents(evt, ctx);
          }
        }

        wrapper_mutex_unlock(&ctx->wrapper_lock);
      }

processed:
      ctx_freeEvent(evt);

      evt = (thread_ctx->type == W_REC)? ctx_getRecordEvent() : ctx_getPlaybackEvent();
    }
  }

  DVR_WRAPPER_DEBUG("wrapper thread(%s) exit, running(%d) type(%d)\n", thread_ctx->name, thread_ctx->running, thread_ctx->type);
  return NULL;
}

static inline int ctx_addRecordEvent(DVR_WrapperEventCtx_t *evt)
{
  pthread_mutex_lock(&WRAPPER_THREAD_RECORD->lock);
  if (ctx_addEvent(&record_evt_list, &record_evt_list_lock, evt) == 0)
    wrapper_threadSignalForType(evt->type);
  pthread_mutex_unlock(&WRAPPER_THREAD_RECORD->lock);
  return 0;
}

static inline int ctx_addPlaybackEvent(DVR_WrapperEventCtx_t *evt)
{
  pthread_mutex_lock(&WRAPPER_THREAD_PLAYBACK->lock);
  if (ctx_addEvent(&playback_evt_list, &playback_evt_list_lock, evt) == 0)
    wrapper_threadSignalForType(evt->type);
  pthread_mutex_unlock(&WRAPPER_THREAD_PLAYBACK->lock);
  return 0;
}

static inline void ctx_freeSegments(DVR_WrapperCtx_t *ctx)
{
  DVR_WrapperPlaybackSegmentInfo_t *p_seg, *p_seg_tmp;
  list_for_each_entry_safe(p_seg, p_seg_tmp, &ctx->segments, head) {
    list_del(&p_seg->head);
    free(p_seg);
  }
}

static inline void _updatePlaybackSegment(DVR_WrapperPlaybackSegmentInfo_t *p_seg,
    DVR_RecordSegmentInfo_t *seg_info, int update_flags, DVR_WrapperCtx_t *ctx)
{
  (void)ctx;
  if ((update_flags & U_PIDS) && (update_flags & U_STAT))
    p_seg->seg_info = *seg_info;
  else if (update_flags & U_PIDS) {
    p_seg->seg_info.nb_pids = seg_info->nb_pids;
    memcpy(p_seg->seg_info.pids, seg_info->pids, sizeof(p_seg->seg_info.pids));
  } else if (update_flags & U_STAT) {
    p_seg->seg_info.duration = seg_info->duration;
    p_seg->seg_info.size = seg_info->size;
    p_seg->seg_info.nb_packets = seg_info->nb_packets;
  }
  //update current segment duration on timeshift mode
  if (ctx->playback.param_open.is_timeshift
      || ctx_isPlay_recording(ctx->playback.param_open.location))
    dvr_playback_update_duration(ctx->playback.player,p_seg->seg_info.id,p_seg->seg_info.duration);
  /*no changes
  DVR_PlaybackSegmentFlag_t flags;
  p_seg->playback_info.segment_id = p_seg->seg_info.id;
  strncpy(p_seg->playback_info.location,
    ctx->playback.param_open.location, sizeof(p_seg->playback_info.location));
  p_seg->playback_info.pids = ctx->playback.pids_req;
  flags = DVR_PLAYBACK_SEGMENT_DISPLAYABLE | DVR_PLAYBACK_SEGMENT_CONTINUOUS;
  if (ctx->record.param_open.flags | DVR_RECORD_FLAG_SCRAMBLED)
    flags |= DVR_PLAYBACK_SEGMENT_ENCRYPTED;
  p_seg->playback_info.flags = flags;
  */
}

static int wrapper_updatePlaybackSegment(DVR_WrapperCtx_t *ctx, DVR_RecordSegmentInfo_t *seg_info, int update_flags)
{
  DVR_WrapperPlaybackSegmentInfo_t *p_seg;

  DVR_WRAPPER_INFO("timeshift, update playback segments(wrapper), seg:%lld t/s/p(%ld/%zu/%u)\n",
    seg_info->id, seg_info->duration, seg_info->size, seg_info->nb_packets);

  if (list_empty(&ctx->segments)) {
    DVR_WRAPPER_INFO("timeshift, update while no segment exists, ignore\n");
    return DVR_SUCCESS;
  }

  /*normally, the last segment added will be updated*/
  p_seg =
    list_first_entry(&ctx->segments, DVR_WrapperPlaybackSegmentInfo_t, head);
  if (p_seg->seg_info.id == seg_info->id) {
    _updatePlaybackSegment(p_seg, seg_info, update_flags, ctx);
  } else {
    // This error is suppressed as the macro code is picked from kernel.
    // prefetch() here incurring self_assign is used to avoid some compiling
    // warnings.
    // coverity[self_assign]
    list_for_each_entry_reverse(p_seg, &ctx->segments, head) {
      if (p_seg->seg_info.id == seg_info->id) {
        _updatePlaybackSegment(p_seg, seg_info, update_flags, ctx);
        break;
      }
    }
  }

  /*need to notify the dvr_playback*/
  if ((ctx->playback.param_open.is_timeshift/*should must be timeshift*/
      || ctx_isPlay_recording(ctx->playback.param_open.location))
    && ctx->playback.last_event == DVR_PLAYBACK_EVENT_REACHED_END
    && ctx->playback.seg_status.state == DVR_PLAYBACK_STATE_PAUSE) {
    if (
      /*there's $TIMESHIFT_DATA_DURATION_TO_RESUME more of data in the current segment playing*/
      (ctx->playback.seg_status.segment_id == seg_info->id
      && (seg_info->duration >= ((time_t)ctx->playback.seg_status.time_cur + TIMESHIFT_DATA_DURATION_TO_RESUME))
      && ctx->playback.speed != 0.0f)
      ||
      /*or there's a new segment and has $TIMESHIFT_DATA_DURATION_TO_RESUME of data*/
      (ctx->playback.seg_status.segment_id != seg_info->id
      && (seg_info->duration >= TIMESHIFT_DATA_DURATION_TO_RESUME)
      && ctx->playback.speed != 0.0f)
      )
    {
      int error;
      //clear end event
      if (ctx->playback.last_event == DVR_PLAYBACK_EVENT_REACHED_END)
          ctx->playback.last_event = DVR_PLAYBACK_EVENT_TRANSITION_OK;

      error = dvr_playback_resume(ctx->playback.player);
      DVR_WRAPPER_INFO("timeshift, resume playback(sn:%ld) (%d) id/dur: rec(%lld/%ld) play(%lld/%u)\n",
        ctx->sn, error,
        seg_info->id, seg_info->duration,
        ctx->playback.seg_status.segment_id, ctx->playback.seg_status.time_cur);
    }
  }

  return DVR_SUCCESS;
}

static void _updateRecordSegment(DVR_WrapperRecordSegmentInfo_t *p_seg,
  DVR_RecordSegmentInfo_t *seg_info, int update_flags, DVR_WrapperCtx_t *ctx)
{
  (void)ctx;
  if ((update_flags & U_PIDS) && (update_flags & U_STAT))
    p_seg->info = *seg_info;
  else if (update_flags & U_PIDS) {
    p_seg->info.nb_pids = seg_info->nb_pids;
    memcpy(p_seg->info.pids, seg_info->pids, sizeof(p_seg->info.pids));
  } else if (update_flags & U_STAT) {
    p_seg->info.duration = seg_info->duration;
    p_seg->info.size = seg_info->size;
    p_seg->info.nb_packets = seg_info->nb_packets;
  }
}

static int wrapper_updateRecordSegment(DVR_WrapperCtx_t *ctx, DVR_RecordSegmentInfo_t *seg_info, int update_flags)
{
  DVR_WrapperRecordSegmentInfo_t *p_seg = NULL;

  /*normally, the last segment added will be updated*/
  if (!list_empty(&ctx->segments)) {
    p_seg =
      list_first_entry(&ctx->segments, DVR_WrapperRecordSegmentInfo_t, head);
    if (p_seg->info.id == seg_info->id) {
      _updateRecordSegment(p_seg, seg_info, update_flags, ctx);
    } else {
      // This error is suppressed as the macro code is picked from kernel.
      // prefetch() here incurring self_assign is used to avoid some compiling
      // warnings.
      // coverity[self_assign]
      list_for_each_entry_reverse(p_seg, &ctx->segments, head) {
        if (p_seg->info.id == seg_info->id) {
          _updateRecordSegment(p_seg, seg_info, update_flags, ctx);
          break;
        }
      }
    }
  }

  /*timeshift, update the segment for playback*/
  /*
    the playback should grab the segment info other than the id,
    and the id will be updated by each segment-add during the recording
  */
  /*
    the playback paused if no data been checked from recording,
         should resume the player later when there's more data
  */
  int sn = 0;
  if (ctx->record.param_open.is_timeshift ||
    (sn = ctx_isRecord_playing(ctx->record.param_open.location))) {
    DVR_WrapperCtx_t *ctx_playback;
    if (ctx->record.param_open.is_timeshift)
      ctx_playback = ctx_getPlayback(sn_timeshift_playback);
    else
      ctx_playback = ctx_getPlayback(sn);

    if (ctx_playback) {
      wrapper_mutex_lock(&ctx_playback->wrapper_lock);
      if (ctx_valid(ctx_playback)
          && (ctx_playback->sn == sn_timeshift_playback ||
              ctx_playback->sn == sn)) {
          wrapper_updatePlaybackSegment(ctx_playback, seg_info, update_flags);
      }
      wrapper_mutex_unlock(&ctx_playback->wrapper_lock);
    }
  }

  return DVR_SUCCESS;
}

static int wrapper_addPlaybackSegment(DVR_WrapperCtx_t *ctx,
    DVR_RecordSegmentInfo_t *seg_info,
    DVR_PlaybackPids_t *p_pids,
    DVR_PlaybackSegmentFlag_t flags)
{
  DVR_WrapperPlaybackSegmentInfo_t *p_seg;
  int error;

  error = 0;
  p_seg = (DVR_WrapperPlaybackSegmentInfo_t *)calloc(1, sizeof(DVR_WrapperPlaybackSegmentInfo_t));
  if (!p_seg) {
    error = DVR_FAILURE;
    DVR_WRAPPER_INFO("memory fail\n");
    return error;
  }

  /*copy the original segment info*/
  p_seg->seg_info = *seg_info;
  /*generate the segment info used in playback*/
  p_seg->playback_info.segment_id = p_seg->seg_info.id;
  const int len = strlen(ctx->playback.param_open.location);
  if (len >= DVR_MAX_LOCATION_SIZE || len <= 0) {
    DVR_WRAPPER_ERROR("Invalid playback.param_open.location length %d", len);
    free(p_seg);
    return DVR_FAILURE;
  }
  strncpy(p_seg->playback_info.location, ctx->playback.param_open.location, len+1);
  p_seg->playback_info.pids = *p_pids;
  p_seg->playback_info.flags = flags;
  p_seg->playback_info.duration = p_seg->seg_info.duration;
  list_add(p_seg, &ctx->segments);
  DVR_WRAPPER_INFO("start to add segment %lld\n", p_seg->playback_info.segment_id);

  error = dvr_playback_add_segment(ctx->playback.player, &p_seg->playback_info);
  if (error) {
    DVR_WRAPPER_INFO("fail to add segment %lld (%d)\n", p_seg->playback_info.segment_id, error);
  } else {
    ctx->playback.status.info_full.time += p_seg->seg_info.duration;
    ctx->playback.status.info_full.size += p_seg->seg_info.size;
    ctx->playback.status.info_full.pkts += p_seg->seg_info.nb_packets;
  }

  return error;
}

static int wrapper_addRecordSegment(DVR_WrapperCtx_t *ctx, DVR_RecordSegmentInfo_t *seg_info)
{
  DVR_WrapperRecordSegmentInfo_t *p_seg;
  int error = DVR_SUCCESS;
  int sn = 0;

  p_seg = (DVR_WrapperRecordSegmentInfo_t *)calloc(1, sizeof(DVR_WrapperRecordSegmentInfo_t));
  if (!p_seg) {
    DVR_WRAPPER_ERROR("memory allocation failed");
    return DVR_FAILURE;
  }
  p_seg->info = *seg_info;
  list_add(p_seg, &ctx->segments);

  if (ctx->record.param_open.is_timeshift ||
      (sn = ctx_isRecord_playing(ctx->record.param_open.location))) {

    DVR_WrapperCtx_t *ctx_playback;
    if (ctx->record.param_open.is_timeshift)
      ctx_playback = ctx_getPlayback(sn_timeshift_playback);
    else
      ctx_playback = ctx_getPlayback(sn);

    DVR_WRAPPER_INFO("rec(sn:%ld) add_seg: playback(sn:%ld) add seg\n", ctx->sn, sn);

    if (ctx_playback) {
      wrapper_mutex_lock(&ctx_playback->wrapper_lock);
      if (ctx_valid(ctx_playback)) {
        DVR_PlaybackSegmentFlag_t flags;

        /*only if playback has started, the previous segments have been loaded*/
        if (!list_empty(&ctx_playback->segments)) {
          flags = DVR_PLAYBACK_SEGMENT_DISPLAYABLE | DVR_PLAYBACK_SEGMENT_CONTINUOUS;
          if (ctx->record.param_open.flags & DVR_RECORD_FLAG_SCRAMBLED)
            flags |= DVR_PLAYBACK_SEGMENT_ENCRYPTED;
          error = wrapper_addPlaybackSegment(ctx_playback, seg_info, &ctx_playback->playback.pids_req, flags);
          if (error == DVR_FAILURE) {
            DVR_WRAPPER_WARN("adding playback segment fails");
          }
        } else {
          DVR_WRAPPER_INFO("rec(sn:%ld) add_seg: playback(sn:%ld) list empty\n", ctx->sn, sn);
        }
      } else {
            DVR_WRAPPER_INFO("rec(sn:%ld) add_seg: playback(sn:%ld) not valid\n", ctx->sn, sn);
      }
      wrapper_mutex_unlock(&ctx_playback->wrapper_lock);
    }
    else
    {
      DVR_WRAPPER_INFO("rec(sn:%ld) add_seg: playback(sn:%ld) not valid 2\n", ctx->sn, sn);
    }

    /*if it is not a timeshift recording, but a playing recording,
      do not forget to obey the recording rule: link the segment!*/
    if (!ctx->record.param_open.is_timeshift) {
        DVR_WRAPPER_INFO("rec(sn:%ld) add_seg: update link\n", ctx->sn);
        dvr_segment_link_op(ctx->record.param_open.location, 1, &seg_info->id, SEGMENT_OP_ADD);
    }
  } else {
    DVR_WRAPPER_INFO("rec(sn:%ld) add_seg: update link\n", ctx->sn);
    dvr_segment_link_op(ctx->record.param_open.location, 1, &seg_info->id, SEGMENT_OP_ADD);
  }

  return error;
}

static int wrapper_removePlaybackSegment(DVR_WrapperCtx_t *ctx, DVR_RecordSegmentInfo_t *seg_info)
{
  int error = -1;
  DVR_WrapperPlaybackSegmentInfo_t *p_seg = NULL, *p_seg_tmp;
  uint32_t off_set = 0;
  DVR_WRAPPER_INFO("timeshift, remove playback(sn:%ld) segment(%lld) ...\n", ctx->sn, seg_info->id);

  list_for_each_entry_safe_reverse(p_seg, p_seg_tmp, &ctx->segments, head) {
    if (p_seg->seg_info.id == seg_info->id) {

      if (ctx->current_segment_id == seg_info->id) {
        DVR_WrapperPlaybackSegmentInfo_t *next_seg;

        /*drive the player out of this will-be-deleted segment*/
        next_seg = list_prev_entry(p_seg, head);

        if (ctx->playback.param_open.vendor == DVR_PLAYBACK_VENDOR_AMAZON)
            off_set = 10 * 1000;
        error = dvr_playback_seek(ctx->playback.player, next_seg->seg_info.id, off_set);
        DVR_WRAPPER_INFO("timeshift, playback(sn:%ld), seek(seg:%llu 0) from new start (%d)\n", ctx->sn, next_seg->seg_info.id, error);
      }

      error = dvr_playback_remove_segment(ctx->playback.player, seg_info->id);
      if (error) {
        /*remove playback segment fail*/
        DVR_WRAPPER_INFO("timeshift, playback(sn:%ld), failed to remove segment(%llu) (%d)\n", ctx->sn, seg_info->id, error);
      }

      list_del(&p_seg->head);

      /*record the obsolete*/
      ctx->playback.obsolete.time += p_seg->seg_info.duration;
      ctx->playback.obsolete.size += p_seg->seg_info.size;
      ctx->playback.obsolete.pkts += p_seg->seg_info.nb_packets;
      DVR_WRAPPER_INFO("timeshift, remove playback(sn:%ld) segment(%lld) ..obs(%d).\n", ctx->sn, seg_info->id, ctx->playback.obsolete.time);
      dvr_playback_set_obsolete(ctx->playback.player, ctx->playback.obsolete.time);
      free(p_seg);
      break;
    }
  }

  DVR_WRAPPER_INFO("timeshift, remove playback(sn:%ld) segment(%lld) =(%d)\n", ctx->sn, seg_info->id, error);

  return error;
}

static int wrapper_removeRecordSegment(DVR_WrapperCtx_t *ctx, DVR_WrapperRecordSegmentInfo_t *seg_info)
{
  int error;
  DVR_WrapperRecordSegmentInfo_t *p_seg, *p_seg_tmp;

  DVR_WRAPPER_INFO("calling %s on record(sn:%ld) segment(%lld) ...",
          __func__, ctx->sn, seg_info->info.id);

  /*if timeshifting, notify the playback first, then deal with record*/
  if (ctx->record.param_open.is_timeshift) {
    DVR_WrapperCtx_t *ctx_playback = ctx_getPlayback(sn_timeshift_playback);

    if (ctx_playback) {
      wrapper_mutex_lock(&ctx_playback->wrapper_lock);
      if (ctx_playback->current_segment_id == seg_info->info.id && ctx_playback->playback.speed == 100.0f) {
          ctx_playback->playback.tf_full = DVR_TRUE;
          DVR_WRAPPER_INFO("%s, cannot remove record(sn:%ld) segment(%lld) for it is being"
            " played on segment(%lld) at speed %f.", __func__, ctx->sn, seg_info->info.id,
            ctx_playback->current_segment_id, ctx_playback->playback.speed);
          wrapper_mutex_unlock(&ctx_playback->wrapper_lock);
          return DVR_SUCCESS;
      } else {
          DVR_WRAPPER_INFO("%s, removing record(sn:%ld) segment(%lld) which is being played "
            "on segment (%lld) at speed (%f).", __func__, ctx->sn, seg_info->info.id,
            ctx_playback->current_segment_id,ctx_playback->playback.speed);
      }
      if (ctx_valid(ctx_playback)
        && ctx_playback->sn == sn_timeshift_playback
        && !list_empty(&ctx_playback->segments)) {
        error = wrapper_removePlaybackSegment(ctx_playback, &seg_info->info);
        if (error != DVR_SUCCESS) {
          DVR_WRAPPER_ERROR("wrapper_removePlaybackSegment failed with return value %d",error);
        }
      }
      ctx_playback->playback.tf_full = DVR_FALSE;
      wrapper_mutex_unlock(&ctx_playback->wrapper_lock);
    }
  }

  uint64_t id = seg_info->info.id;

  list_for_each_entry_safe_reverse(p_seg, p_seg_tmp, &ctx->segments, head) {
    if (p_seg->info.id == id) {
      list_del(&p_seg->head);

      /*record the obsolete*/
      ctx->record.obsolete.time += p_seg->info.duration;
      ctx->record.obsolete.size += p_seg->info.size;
      ctx->record.obsolete.pkts += p_seg->info.nb_packets;

      free(p_seg);
      break;
    }
  }

  error = dvr_segment_delete(ctx->record.param_open.location, id);

  DVR_WRAPPER_INFO("%s, removed record(sn:%ld) segment(%lld), ret=(%d)\n",
    __func__, ctx->sn, id, error);

  return error;
}

int dvr_wrapper_open_record (DVR_WrapperRecord_t *rec, DVR_WrapperRecordOpenParams_t *params)
{
  int error;
  DVR_WrapperCtx_t *ctx;
  DVR_RecordOpenParams_t open_param;

  DVR_RETURN_IF_FALSE(rec);
  DVR_RETURN_IF_FALSE(params);

  /*get a free ctx*/
  ctx = ctx_getRecord(0);
  DVR_RETURN_IF_FALSE(ctx);

  wrapper_mutex_lock(&ctx->wrapper_lock);

  DVR_WRAPPER_INFO("open record(dmx:%d) .is_tf(%d)..time (%ld)ms max size(%lld)byte seg size(%lld)byte\n",
  params->dmx_dev_id, params->is_timeshift, params->max_time, params->max_size, params->segment_size);

  ctx_reset(ctx);

  ctx->record.param_open = *params;
  ctx->record.event_fn = params->event_fn;
  ctx->record.event_userdata = params->event_userdata;

  INIT_LIST_HEAD(&ctx->segments);
  ctx->sn = get_sn();

  wrapper_requestThreadFor(ctx);

  memset(&open_param, 0, sizeof(DVR_RecordOpenParams_t));
  open_param.fend_dev_id = params->fend_dev_id;
  open_param.dmx_dev_id = params->dmx_dev_id;
  open_param.data_from_memory = 0;
  open_param.flags = params->flags;
  if (params->flush_size) {
    open_param.notification_size = params->flush_size;
  } else {
    open_param.notification_size = 64*1024;
  }
  open_param.notification_time = 400;//ms
  open_param.flush_size = params->flush_size;
  open_param.ringbuf_size = params->ringbuf_size;
  open_param.event_fn = wrapper_record_event_handler;
  open_param.event_userdata = (void*)ctx->sn;
  if (params->keylen) {
    open_param.clearkey = params->clearkey;
    open_param.cleariv = params->cleariv;
    open_param.keylen = params->keylen;
  }
  open_param.force_sysclock = params->force_sysclock;
  open_param.guarded_segment_size = params->segment_size/2*3;

  error = dvr_record_open(&ctx->record.recorder, &open_param);
  if (error) {
    DVR_WRAPPER_INFO("record(dmx:%d) open fail(error:%d).\n", params->dmx_dev_id, error);
    ctx_reset(ctx);
    wrapper_mutex_unlock(&ctx->wrapper_lock);
    wrapper_releaseThreadForType(ctx->type);
    return DVR_FAILURE;
  }
  if (params->is_timeshift)
    sn_timeshift_record = ctx->sn;

  DVR_WRAPPER_INFO("record(dmx:%d) openned ok(sn:%ld).\n", params->dmx_dev_id, ctx->sn);

  if (params->crypto_fn) {
    error = dvr_record_set_encrypt_callback(ctx->record.recorder, params->crypto_fn, params->crypto_data);
    if (error) {
      DVR_WRAPPER_INFO("record(dmx:%d) set encrypt callback fail(error:%d).\n", params->dmx_dev_id, error);
    }
  }

  wrapper_mutex_unlock(&ctx->wrapper_lock);

  *rec = (DVR_WrapperRecord_t)ctx->sn;
  return DVR_SUCCESS;
}

int dvr_wrapper_close_record (DVR_WrapperRecord_t rec)
{
  DVR_WrapperCtx_t *ctx;
  DVR_RecordSegmentInfo_t seg_info;
  int error;

  DVR_RETURN_IF_FALSE(rec);

  ctx = ctx_getRecord((unsigned long)rec);
  DVR_RETURN_IF_FALSE(ctx);

  wrapper_mutex_lock(&ctx->wrapper_lock);
  DVR_WRAPPER_INFO("close record(sn:%ld)\n", ctx->sn);
  WRAPPER_RETURN_IF_FALSE_WITH_UNLOCK(ctx_valid(ctx), &ctx->wrapper_lock);

  memset(&seg_info, 0, sizeof(seg_info));
  dvr_record_stop_segment(ctx->record.recorder, &seg_info);

  error = dvr_record_close(ctx->record.recorder);

  if (ctx->record.param_open.is_timeshift)
    sn_timeshift_record = 0;

  ctx_freeSegments(ctx);

  DVR_WRAPPER_INFO("record(sn:%ld) closed = (%d).\n", ctx->sn, error);
  ctx_reset(ctx);
  wrapper_mutex_unlock(&ctx->wrapper_lock);

  wrapper_releaseThreadForType(ctx->type);

  return error;
}

int dvr_wrapper_start_record (DVR_WrapperRecord_t rec, DVR_WrapperRecordStartParams_t *params)
{
  DVR_WrapperCtx_t *ctx;
  DVR_RecordStartParams_t *start_param;
  int i;
  int error;

  DVR_RETURN_IF_FALSE(rec);
  DVR_RETURN_IF_FALSE(params);

  ctx = ctx_getRecord((unsigned long)rec);
  DVR_RETURN_IF_FALSE(ctx);

  wrapper_mutex_lock(&ctx->wrapper_lock);
  DVR_WRAPPER_INFO("libdvr_api, start_record (sn:%ld) location:%s, save:%d",
      ctx->sn, ctx->record.param_open.location, params->save_rec_file);
  WRAPPER_RETURN_IF_FALSE_WITH_UNLOCK(ctx_valid(ctx), &ctx->wrapper_lock);

  start_param = &ctx->record.param_start;
  memset(start_param, 0, sizeof(*start_param));
  const int len = strlen(ctx->record.param_open.location);
  if (len >= DVR_MAX_LOCATION_SIZE || len <= 0) {
    DVR_WRAPPER_ERROR("Invalid record.param_open.location length %d",len);
    pthread_mutex_unlock(&ctx->wrapper_lock);
    return DVR_FAILURE;
  }
  strncpy(start_param->location, ctx->record.param_open.location, len+1);
  start_param->segment.nb_pids = params->pids_info.nb_pids;
  for (i = 0; i < params->pids_info.nb_pids; i++) {
    start_param->segment.pids[i] = params->pids_info.pids[i];
    start_param->segment.pid_action[i] = DVR_RECORD_PID_CREATE;
  }

  if (params->save_rec_file == 0)//default is not save
    dvr_segment_del_by_location(start_param->location);

  //wait for the file status to stabilize before set the new segment id
  uint64_t new_segment_id = 0;

  if (params->save_rec_file != 0) {
    uint32_t segment_nb = 0;
    uint64_t *p_segment_ids = NULL;
    error = dvr_segment_get_list(ctx->record.param_open.location, &segment_nb, &p_segment_ids);
    if (error == DVR_SUCCESS && segment_nb>0) {
      // Tainted data issue originating from fgets seem false positive, so we
      // just suppress it here.
      // coverity[tainted_data]
      new_segment_id = p_segment_ids[segment_nb-1]+1;
    }
    if (p_segment_ids != NULL) {
      free(p_segment_ids);
    }
  }
  DVR_WRAPPER_DEBUG("new_segment_id:%lld\n", new_segment_id);

  ctx->record.next_segment_id = new_segment_id;
  ctx->current_segment_id = new_segment_id;

  start_param->segment.segment_id = ctx->record.next_segment_id++;

  {
    /*sync to update for further use*/
    DVR_RecordStartParams_t *update_param;
    update_param = &ctx->record.param_update;
    memcpy(update_param, start_param, sizeof(*update_param));
    for (i = 0; i < update_param->segment.nb_pids; i++)
      update_param->segment.pid_action[i] = DVR_RECORD_PID_KEEP;
  }

  error = dvr_record_start_segment(ctx->record.recorder, start_param);
  {
    DVR_RecordSegmentInfo_t new_seg_info =
      { .id = start_param->segment.segment_id, };
    wrapper_addRecordSegment(ctx, &new_seg_info);
  }

  DVR_WRAPPER_INFO("record(sn:%ld) started = (%d)\n", ctx->sn, error);

  wrapper_mutex_unlock(&ctx->wrapper_lock);

  return error;
}

int dvr_wrapper_stop_record (DVR_WrapperRecord_t rec)
{
  DVR_WrapperCtx_t *ctx;
  DVR_RecordSegmentInfo_t seg_info;
  int error;

  DVR_RETURN_IF_FALSE(rec);

  ctx = ctx_getRecord((unsigned long)rec);
  DVR_RETURN_IF_FALSE(ctx);

  wrapper_mutex_lock(&ctx->wrapper_lock);
  DVR_WRAPPER_INFO("libdvr_api, stop_record (sn:%ld)", ctx->sn);
  WRAPPER_RETURN_IF_FALSE_WITH_UNLOCK(ctx_valid(ctx), &ctx->wrapper_lock);

  memset(&seg_info, 0, sizeof(seg_info));
  error = dvr_record_stop_segment(ctx->record.recorder, &seg_info);
  wrapper_updateRecordSegment(ctx, &seg_info, U_ALL);

  ctx_freeSegments(ctx);

  DVR_WRAPPER_INFO("record(sn:%ld) stopped = (%d)\n", ctx->sn, error);
  wrapper_mutex_unlock(&ctx->wrapper_lock);

  return error;
}

int dvr_wrapper_pause_record (DVR_WrapperRecord_t rec)
{
  DVR_WrapperCtx_t *ctx;
  int error;

  DVR_RETURN_IF_FALSE(rec);

  ctx = ctx_getRecord((unsigned long)rec);
  DVR_RETURN_IF_FALSE(ctx);

  wrapper_mutex_lock(&ctx->wrapper_lock);
  DVR_WRAPPER_INFO("libdvr_api, pause_record (sn:%ld)", ctx->sn);
  WRAPPER_RETURN_IF_FALSE_WITH_UNLOCK(ctx_valid(ctx), &ctx->wrapper_lock);

  error = dvr_record_pause(ctx->record.recorder);

  DVR_WRAPPER_INFO("record(sn:%ld) pauseed = (%d)\n", ctx->sn, error);
  wrapper_mutex_unlock(&ctx->wrapper_lock);

  return error;
}

int dvr_wrapper_resume_record (DVR_WrapperRecord_t rec)
{
  DVR_WrapperCtx_t *ctx;
  int error;

  DVR_RETURN_IF_FALSE(rec);

  ctx = ctx_getRecord((unsigned long)rec);
  DVR_RETURN_IF_FALSE(ctx);

  wrapper_mutex_lock(&ctx->wrapper_lock);
  DVR_WRAPPER_INFO("libdvr_api, resume_record (sn:%ld)", ctx->sn);
  WRAPPER_RETURN_IF_FALSE_WITH_UNLOCK(ctx_valid(ctx), &ctx->wrapper_lock);

  error = dvr_record_resume(ctx->record.recorder);

  DVR_WRAPPER_INFO("record(sn:%ld) resumed = (%d)\n", ctx->sn, error);
  wrapper_mutex_unlock(&ctx->wrapper_lock);

  return error;
}

/* Return true if arr1 contains all elements in arr2 */
static DVR_Bool_t pids_test_include(
                  DVR_StreamPid_t* arr1, DVR_RecordPidAction_t *act1, int size1,
                  DVR_StreamPid_t* arr2, DVR_RecordPidAction_t *act2, int size2)
{
  DVR_Bool_t ret = DVR_TRUE;
  for (int i=0;i<size2;i++)
  { // iterate all elements in arr2 to check if they exist in arr1
    DVR_Bool_t found=DVR_FALSE;

    if (act2[i] == DVR_RECORD_PID_CLOSE)
      continue;

    for (int j=0;j<size1;j++)
    {
      if (act1[j] != DVR_RECORD_PID_CLOSE
          && arr2[i].pid == arr1[j].pid)
      {
        found=DVR_TRUE;
        break;
      }
    }
    if (found == DVR_FALSE)
    {
      ret=DVR_FALSE;
      break;
    }
  }
  return ret;
}

static DVR_Bool_t pids_equal(const DVR_RecordSegmentStartParams_t* p1,
                             const DVR_WrapperUpdatePidsParams_t* p2)
{
  int i=0;
  char buf[128]={0};
  int cnt=0;
  int chars=0;

  DVR_RETURN_IF_FALSE(p1 != NULL && p2 != NULL);
  DVR_RETURN_IF_FALSE(p1->nb_pids>0 && p2->nb_pids>0);

  DVR_Bool_t cond1 = pids_test_include(p1->pids,p1->pid_action,p1->nb_pids,
                        p2->pids,p2->pid_action,p2->nb_pids);
  DVR_Bool_t cond2 = pids_test_include(p2->pids,p2->pid_action,p2->nb_pids,
                        p1->pids,p1->pid_action,p1->nb_pids);
  DVR_Bool_t is_equal = (cond1 && cond2);
  int removed;

  removed = 0;
  for (i=0;i<p1->nb_pids;i++)
  {
    if (p1->pid_action[i] == DVR_RECORD_PID_CLOSE) {
      removed++;
      continue;
    }
    chars = snprintf(buf+cnt,sizeof(buf)-cnt,"0x%hx,",p1->pids[i].pid);
    if (chars<0)
    {
      break;
    }
    cnt += chars;
  }
  DVR_INFO("%s nb_pids1:%d, pids1: %s",__func__,p1->nb_pids-removed,buf);
  memset(buf,0,sizeof(buf));

  removed = 0;
  for (i=0,cnt=0;i<p2->nb_pids;i++)
  {
    if (p2->pid_action[i] == DVR_RECORD_PID_CLOSE) {
      removed++;
      continue;
    }
    chars = snprintf(buf+cnt,sizeof(buf)-cnt,"0x%hx,",p2->pids[i].pid);
    if (chars<0)
    {
      break;
    }
    cnt += chars;
  }
  DVR_INFO("%s nb_pids2:%d, pids2: %s",__func__,p2->nb_pids-removed,buf);
  DVR_INFO("%s is_equal:%d",__func__,is_equal);
  return is_equal;
}

int dvr_wrapper_update_record_pids (DVR_WrapperRecord_t rec, DVR_WrapperUpdatePidsParams_t *params)
{
  DVR_WrapperCtx_t *ctx;
  DVR_RecordStartParams_t *start_param;
  DVR_RecordSegmentInfo_t seg_info;
  int i;
  int error;

  DVR_RETURN_IF_FALSE(rec);
  DVR_RETURN_IF_FALSE(params);

  ctx = ctx_getRecord((unsigned long)rec);
  DVR_RETURN_IF_FALSE(ctx);

  wrapper_mutex_lock(&ctx->wrapper_lock);
  DVR_WRAPPER_INFO("libdvr_api, update_record (sn:%ld)", ctx->sn);
  WRAPPER_RETURN_IF_FALSE_WITH_UNLOCK(ctx_valid(ctx), &ctx->wrapper_lock);

  start_param = &ctx->record.param_update;
  if (pids_equal(&(start_param->segment),params))
  {
    wrapper_mutex_unlock(&ctx->wrapper_lock);
    return DVR_TRUE;
  }

  memset(start_param, 0, sizeof(*start_param));
  const int len = strlen(ctx->record.param_open.location);
  if (len >= DVR_MAX_LOCATION_SIZE || len <= 0) {
    DVR_WRAPPER_ERROR("Invalid record.param_open.location length %d",len);
    pthread_mutex_unlock(&ctx->wrapper_lock);
    return DVR_FAILURE;
  }
  strncpy(start_param->location, ctx->record.param_open.location, len+1);
  start_param->segment.segment_id = ctx->record.next_segment_id++;
  start_param->segment.nb_pids = params->nb_pids;
  for (i = 0; i < params->nb_pids; i++) {
    start_param->segment.pids[i] = params->pids[i];
    start_param->segment.pid_action[i] = params->pid_action[i];
  }
  error = dvr_record_next_segment(ctx->record.recorder, start_param, &seg_info);
  {
    DVR_RecordSegmentInfo_t new_seg_info =
      { .id = start_param->segment.segment_id, };
    wrapper_updateRecordSegment(ctx, &seg_info, U_PIDS);
    wrapper_addRecordSegment(ctx, &new_seg_info);
  }

  DVR_WRAPPER_INFO("record(sn:%ld) updated = (%d)\n", ctx->sn, error);
  wrapper_mutex_unlock(&ctx->wrapper_lock);

  return error;
}

int dvr_wrapper_get_record_status(DVR_WrapperRecord_t rec, DVR_WrapperRecordStatus_t *status)
{
  DVR_WrapperCtx_t *ctx;
  DVR_WrapperRecordStatus_t s;
  int error;

  DVR_RETURN_IF_FALSE(rec);
  DVR_RETURN_IF_FALSE(status);

  ctx = ctx_getRecord((unsigned long)rec);
  DVR_RETURN_IF_FALSE(ctx);

  wrapper_mutex_lock(&ctx->wrapper_lock);

  DVR_WRAPPER_INFO("libdvr_api, get_record_status (sn:%ld)", ctx->sn);
  WRAPPER_RETURN_IF_FALSE_WITH_UNLOCK(ctx_valid(ctx), &ctx->wrapper_lock);

  error = process_generateRecordStatus(ctx, &s);

  DVR_WRAPPER_INFO("record(sn:%ld) state/time/size/pkts(%d/%ld/%lld/%u) (%d)\n",
    ctx->sn,
    s.state,
    s.info.time,
    s.info.size,
    s.info.pkts,
    error);

  *status = s;

  wrapper_mutex_unlock(&ctx->wrapper_lock);

  return error;
}

int dvr_wrapper_record_is_secure_mode(DVR_WrapperRecord_t rec)
{
  DVR_WrapperCtx_t *ctx;
  int error;

  DVR_RETURN_IF_FALSE(rec);

  ctx = ctx_getRecord((unsigned long)rec);
  DVR_RETURN_IF_FALSE(ctx);

  wrapper_mutex_lock(&ctx->wrapper_lock);
  error = dvr_record_is_secure_mode(ctx->record.recorder);
  wrapper_mutex_unlock(&ctx->wrapper_lock);
  return error;
}

int dvr_wrapper_set_record_secure_buffer (DVR_WrapperRecord_t rec,  uint8_t *p_secure_buf, uint32_t len)
{
  DVR_WrapperCtx_t *ctx;
  int error;

  DVR_RETURN_IF_FALSE(rec);
  DVR_RETURN_IF_FALSE(p_secure_buf);

  ctx = ctx_getRecord((unsigned long)rec);
  DVR_RETURN_IF_FALSE(ctx);

  wrapper_mutex_lock(&ctx->wrapper_lock);
  error = dvr_record_set_secure_buffer(ctx->record.recorder, p_secure_buf, len);
  wrapper_mutex_unlock(&ctx->wrapper_lock);
  return error;
}

int dvr_wrapper_set_record_decrypt_callback (DVR_WrapperRecord_t rec,  DVR_CryptoFunction_t func, void *userdata)
{
  DVR_WrapperCtx_t *ctx;
  int error;

  DVR_RETURN_IF_FALSE(rec);
  DVR_RETURN_IF_FALSE(func);

  ctx = ctx_getRecord((unsigned long)rec);
  DVR_RETURN_IF_FALSE(ctx);

  wrapper_mutex_lock(&ctx->wrapper_lock);
  error = dvr_record_set_encrypt_callback(ctx->record.recorder, func, userdata);
  wrapper_mutex_unlock(&ctx->wrapper_lock);
  return error;
}


int dvr_wrapper_open_playback (DVR_WrapperPlayback_t *playback, DVR_WrapperPlaybackOpenParams_t *params)
{
  DVR_WrapperCtx_t *ctx;
  DVR_PlaybackOpenParams_t open_param;
  int error;

  DVR_RETURN_IF_FALSE(playback);
  DVR_RETURN_IF_FALSE(params);
  DVR_RETURN_IF_FALSE(params->playback_handle);

  /*get a free ctx*/
  ctx = ctx_getPlayback(0);
  DVR_RETURN_IF_FALSE(ctx);

  wrapper_mutex_lock(&ctx->wrapper_lock);

  DVR_WRAPPER_INFO("libdvr_api, open_playback (dmx:%d) ..vendor[%d]params->block_size[%d].",
      params->dmx_dev_id, params->vendor, params->block_size);

  ctx_reset(ctx);

  ctx->playback.param_open = *params;
  ctx->playback.event_fn = params->event_fn;
  ctx->playback.event_userdata = params->event_userdata;
  ctx->current_segment_id = 0;
  INIT_LIST_HEAD(&ctx->segments);
  ctx->sn = get_sn();

  wrapper_requestThreadFor(ctx);

  memset(&open_param, 0, sizeof(DVR_PlaybackOpenParams_t));
  open_param.dmx_dev_id = params->dmx_dev_id;
  open_param.block_size = params->block_size;
  open_param.is_timeshift = params->is_timeshift;
  //open_param.notification_size = 10*1024; //not supported
  open_param.event_fn = wrapper_playback_event_handler;
  open_param.event_userdata = (void*)ctx->sn;
  /*open_param.has_pids = 0;*/
  open_param.is_notify_time = params->is_notify_time;
  open_param.player_handle = (am_tsplayer_handle)params->playback_handle;
  open_param.vendor = params->vendor;

  if (params->keylen) {
    open_param.clearkey = params->clearkey;
    open_param.cleariv = params->cleariv;
    open_param.keylen = params->keylen;
  }
  open_param.control_speed_enable = params->control_speed_enable;

  error = dvr_playback_open(&ctx->playback.player, &open_param);
  if (error) {
    DVR_WRAPPER_INFO("playback(dmx:%d) openned fail(error:%d).\n", params->dmx_dev_id, error);
    ctx_reset(ctx);
    wrapper_mutex_unlock(&ctx->wrapper_lock);
    wrapper_releaseThreadForType(ctx->type);
    return DVR_FAILURE;
  }
  if (params->is_timeshift)
    sn_timeshift_playback = ctx->sn;

  DVR_WRAPPER_INFO("playback(dmx:%d) openned ok(sn:%ld).\n", params->dmx_dev_id, ctx->sn);
  error = dvr_playback_set_decrypt_callback(ctx->playback.player, params->crypto_fn, params->crypto_data);
  if (error) {
    DVR_WRAPPER_INFO("playback set decrypt callback fail(error:%d).\n", error);
  }
  wrapper_mutex_unlock(&ctx->wrapper_lock);

  *playback = (DVR_WrapperPlayback_t)ctx->sn;
  return DVR_SUCCESS;
}

int dvr_wrapper_close_playback (DVR_WrapperPlayback_t playback)
{
  DVR_WrapperCtx_t *ctx;
  int error;

  DVR_RETURN_IF_FALSE(playback);

  ctx = ctx_getPlayback((unsigned long)playback);
  DVR_RETURN_IF_FALSE(ctx);

  wrapper_mutex_lock(&ctx->wrapper_lock);
  DVR_WRAPPER_INFO("libdvr_api, close_playback (sn:%ld)", ctx->sn);
  WRAPPER_RETURN_IF_FALSE_WITH_UNLOCK(ctx_valid(ctx), &ctx->wrapper_lock);

  if (ctx->playback.param_open.is_timeshift)
    sn_timeshift_playback = 0;

  /*try stop first*/
  dvr_playback_stop(ctx->playback.player, DVR_TRUE);

  {
    /*remove all segments*/
    DVR_WrapperPlaybackSegmentInfo_t *p_seg;

    // This error is suppressed as the macro code is picked from kernel.
    // prefetch() here incurring self_assign is used to avoid some compiling
    // warnings.
    // coverity[self_assign]
    list_for_each_entry(p_seg, &ctx->segments, head) {
      error = dvr_playback_remove_segment(ctx->playback.player, p_seg->playback_info.segment_id);
      DVR_WRAPPER_INFO("playback(sn:%ld) remove seg(%lld) (%d)\n",
        ctx->sn, p_seg->playback_info.segment_id, error);
    }
    ctx_freeSegments(ctx);
  }

  error = dvr_playback_close(ctx->playback.player);

  DVR_WRAPPER_INFO("playback(sn:%ld) closed.\n", ctx->sn);
  ctx_reset(ctx);
  wrapper_mutex_unlock(&ctx->wrapper_lock);

  wrapper_releaseThreadForType(ctx->type);

  return error;
}

int dvr_wrapper_start_playback (DVR_WrapperPlayback_t playback, DVR_PlaybackFlag_t flags, DVR_PlaybackPids_t *p_pids)
{
  DVR_WrapperCtx_t *ctx;
  int error=0;
  uint64_t *p_segment_ids;
  uint32_t segment_nb;
  uint32_t i;
  DVR_RecordSegmentInfo_t seg_info_1st;
  int got_1st_seg=0;
  DVR_WrapperCtx_t *ctx_record;/*for timeshift*/
  DVR_Bool_t is_timeshift = DVR_FALSE;
  DVR_PlaybackSegmentFlag_t seg_flags = 0;

  DVR_RETURN_IF_FALSE(playback);
  DVR_RETURN_IF_FALSE(p_pids);

  ctx_record = NULL;

  /*lock the recorder to avoid changing the recording segments*/
  ctx_record = ctx_getRecord(sn_timeshift_record);

  if (ctx_record) {
    wrapper_mutex_lock(&ctx_record->wrapper_lock);
    if (!ctx_valid(ctx_record)
      || ctx_record->sn != sn_timeshift_record) {
      DVR_WRAPPER_INFO("timeshift, record is not for timeshifting, FATAL error found\n");
      wrapper_mutex_unlock(&ctx_record->wrapper_lock);
      is_timeshift  = DVR_FALSE;
    } else {
      is_timeshift  = DVR_TRUE;
    }
  }

  ctx = ctx_getPlayback((unsigned long)playback);
  DVR_RETURN_IF_FALSE(ctx);

  wrapper_mutex_lock(&ctx->wrapper_lock);

  DVR_WRAPPER_INFO("libdvr_api, start_playback (sn:%ld) location:%s"
      " flags:0x%x v/a/ad/sub/pcr(%d:%d %d:%d %d:%d %d:%d %d)",
      ctx->sn,
      ctx->playback.param_open.location,
      flags,
      p_pids->video.pid, p_pids->video.format,
      p_pids->audio.pid, p_pids->audio.format,
      p_pids->ad.pid, p_pids->ad.format,
      p_pids->subtitle.pid, p_pids->subtitle.format,
      p_pids->pcr.pid);

  WRAPPER_RETURN_IF_FALSE_WITH_UNLOCK(ctx_valid(ctx), &ctx->wrapper_lock);

  if (ctx->playback.param_open.is_timeshift) {
    /*lock the recorder to avoid changing the recording segments*/
    if (is_timeshift == DVR_FALSE) {
      DVR_WRAPPER_INFO("timeshift, record is not for timeshifting, FATAL error return\n");
      wrapper_mutex_unlock(&ctx->wrapper_lock);
      return DVR_FAILURE;
    } else {
      DVR_WRAPPER_INFO("playback(sn:%ld) record(sn:%ld) locked ok due to timeshift\n",
        ctx->sn, ctx_record->sn);
    }
  }

  /*obtain all segments in a list*/
  segment_nb = 0;
  p_segment_ids = NULL;
  error = dvr_segment_get_list(ctx->playback.param_open.location, &segment_nb, &p_segment_ids);
  if (!error) {
    got_1st_seg = 0;
    struct list_head           info_list;         /**< segment list head*/
    INIT_LIST_HEAD(&info_list);

    DVR_WRAPPER_INFO("get list segment_nb::%d",segment_nb);
    //we need free info list buf when we used end.
    error = dvr_segment_get_allInfo(ctx->playback.param_open.location, &info_list);
    if (error == DVR_FAILURE) {
          error = DVR_FAILURE;
          DVR_WRAPPER_INFO("fail to get all seg info (location:%s), (error:%d)\n",
            ctx->playback.param_open.location, error);
          // Tainted data issue originating from fgets seem false positive, so we
          // just suppress it here.
          // coverity[tainted_data]
          for (i = 0; i < segment_nb; i++) {
            DVR_RecordSegmentInfo_t seg_info;
            memset((void*)&seg_info,0,sizeof(seg_info));

            error = dvr_segment_get_info(ctx->playback.param_open.location, p_segment_ids[i], &seg_info);
            if (error) {
              error = DVR_FAILURE;
              DVR_WRAPPER_INFO("fail to get seg info (location:%s, seg:%llu), (error:%d)\n",
                ctx->playback.param_open.location, p_segment_ids[i], error);
              break;
            }
            //add check if has audio  or video pid. if not exist. not add segment to playback
            int ii = 0;
            int has_av = 0;
            // Tainted data issue originating from fgets seem false positive, so we
            // just suppress it here.
            // coverity[tainted_data]
            for (ii = 0; ii < seg_info.nb_pids; ii++) {
              int type = (seg_info.pids[ii].type >> 24) & 0x0f;
              if (type == DVR_STREAM_TYPE_VIDEO ||
                type == DVR_STREAM_TYPE_AUDIO ||
                type == DVR_STREAM_TYPE_AD) {
              DVR_WRAPPER_INFO("success to get seg av info \n");
                DVR_WRAPPER_INFO("success to get seg av info type[0x%x][%d] [%d][%d][%d]\n",(seg_info.pids[ii].type >> 24)&0x0f,seg_info.pids[ii].pid,
                DVR_STREAM_TYPE_VIDEO,
                DVR_STREAM_TYPE_AUDIO,
                DVR_STREAM_TYPE_AD);
                has_av = 1;
                //break;
              } else {
                DVR_WRAPPER_INFO("error to get seg av info type[0x%x][%d] [%d][%d][%d]\n",(seg_info.pids[ii].type >> 24)&0x0f,seg_info.pids[ii].pid,
                DVR_STREAM_TYPE_VIDEO,
                DVR_STREAM_TYPE_AUDIO,
                DVR_STREAM_TYPE_AD);
              }
            }
            if (has_av == 0) {
              DVR_WRAPPER_INFO("fail to get seg av info \n");
              continue;
            } else {
              DVR_WRAPPER_INFO("success to get seg av info \n");
            }
            seg_flags = DVR_PLAYBACK_SEGMENT_DISPLAYABLE | DVR_PLAYBACK_SEGMENT_CONTINUOUS;
            error = wrapper_addPlaybackSegment(ctx, &seg_info, p_pids, seg_flags);
            if (error == DVR_FAILURE) {
              DVR_WRAPPER_WARN("adding playback segment fails");
              break;
            }
            /*copy the 1st segment*/
            if (got_1st_seg == 0) {
                seg_info_1st = seg_info;
                got_1st_seg = 1;
            }
          }
    } else {
        // Tainted data issue originating from fgets seem false positive, so we
        // just suppress it here.
        // coverity[tainted_data]
        for (i = 0; i < segment_nb; i++) {
          DVR_RecordSegmentInfo_t *p_seg_info;
          int found = 0;
          // This error is suppressed as the macro code is picked from kernel.
          // prefetch() here incurring self_assign is used to avoid some compiling
          // warnings.
          // coverity[self_assign]
          list_for_each_entry(p_seg_info, &info_list, head)
          {
            if (p_seg_info->id == p_segment_ids[i]) {
              found = 1;
              DVR_WRAPPER_INFO("get segment info::%d", i);
              break;
            }
          }
          if (!found) {
            //last info is not found if when recording occured power off.
            if (p_segment_ids[i] == segment_nb - 1) {
              DVR_RecordSegmentInfo_t seg_info;
              memset((void*)&seg_info,0,sizeof(seg_info));

              error = dvr_segment_get_info(ctx->playback.param_open.location, p_segment_ids[i], &seg_info);
              if (error) {
                error = DVR_FAILURE;
                DVR_WRAPPER_INFO("fail to get seg info (location:%s, seg:%llu), (error:%d)\n",
                  ctx->playback.param_open.location, p_segment_ids[i], error);
                break;
              }
              //
              //add check if has audio  or video pid. if not exist. not add segment to playback
              int ii = 0;
              int has_av = 0;
              // Tainted data issue originating from fgets seem false positive, so we
              // just suppress it here.
              // coverity[tainted_data]
              for (ii = 0; ii < seg_info.nb_pids; ii++) {
                int type = (seg_info.pids[ii].type >> 24) & 0x0f;
                if (type == DVR_STREAM_TYPE_VIDEO ||
                  type == DVR_STREAM_TYPE_AUDIO ||
                  type == DVR_STREAM_TYPE_AD) {
                DVR_WRAPPER_INFO("success to get seg av info \n");
                  DVR_WRAPPER_INFO("success to get seg av info type[0x%x][%d] [%d][%d][%d]\n",(seg_info.pids[ii].type >> 24)&0x0f,seg_info.pids[ii].pid,
                  DVR_STREAM_TYPE_VIDEO,
                  DVR_STREAM_TYPE_AUDIO,
                  DVR_STREAM_TYPE_AD);
                  has_av = 1;
                  //break;
                } else {
                  DVR_WRAPPER_INFO("error to get seg av info type[0x%x][%d] [%d][%d][%d]\n",(seg_info.pids[ii].type >> 24)&0x0f,seg_info.pids[ii].pid,
                  DVR_STREAM_TYPE_VIDEO,
                  DVR_STREAM_TYPE_AUDIO,
                  DVR_STREAM_TYPE_AD);
                }
              }
              if (has_av == 0) {
                DVR_WRAPPER_INFO("fail to get seg av info \n");
                continue;
              } else {
                DVR_WRAPPER_INFO("success to get seg av info \n");
              }
              seg_flags = DVR_PLAYBACK_SEGMENT_DISPLAYABLE | DVR_PLAYBACK_SEGMENT_CONTINUOUS;
              error = wrapper_addPlaybackSegment(ctx, &seg_info, p_pids, seg_flags);
              if (error == DVR_FAILURE) {
                DVR_WRAPPER_WARN("adding playback segment fails");
                break;
              }
            }
            continue;
          }

          //add check if has audio  or video pid. if not exist. not add segment to playback
          int ii = 0;
          int has_av = 0;
          for (ii = 0; ii < p_seg_info->nb_pids; ii++) {
            int type = (p_seg_info->pids[ii].type >> 24) & 0x0f;
            if (type == DVR_STREAM_TYPE_VIDEO ||
              type == DVR_STREAM_TYPE_AUDIO ||
              type == DVR_STREAM_TYPE_AD) {
            DVR_WRAPPER_INFO("success to get seg av info \n");
              DVR_WRAPPER_INFO("success to get seg av info type[0x%x][%d] [%d][%d][%d]\n",(p_seg_info->pids[ii].type >> 24)&0x0f,p_seg_info->pids[ii].pid,
              DVR_STREAM_TYPE_VIDEO,
              DVR_STREAM_TYPE_AUDIO,
              DVR_STREAM_TYPE_AD);
              has_av = 1;
              //break;
            } else {
              DVR_WRAPPER_INFO("error to get seg av info type[0x%x][%d] [%d][%d][%d]\n",(p_seg_info->pids[ii].type >> 24)&0x0f,p_seg_info->pids[ii].pid,
              DVR_STREAM_TYPE_VIDEO,
              DVR_STREAM_TYPE_AUDIO,
              DVR_STREAM_TYPE_AD);
            }
          }
          if (has_av == 0) {
            DVR_WRAPPER_INFO("fail to get seg av info \n");
            continue;
          } else {
            DVR_WRAPPER_INFO("success to get seg av info \n");
          }
          seg_flags = DVR_PLAYBACK_SEGMENT_DISPLAYABLE | DVR_PLAYBACK_SEGMENT_CONTINUOUS;
          error = wrapper_addPlaybackSegment(ctx, p_seg_info, p_pids, seg_flags);
          if (error == DVR_FAILURE) {
            DVR_WRAPPER_WARN("adding playback segment fails");
            break;
          }

          /*copy the 1st segment*/
          if (got_1st_seg == 0) {
              seg_info_1st = *p_seg_info;
              got_1st_seg = 1;
          }
        }
        //free list
        DVR_RecordSegmentInfo_t *segment = NULL;
        DVR_RecordSegmentInfo_t *segment_tmp = NULL;
        list_for_each_entry_safe(segment, segment_tmp, &info_list, head)
        {
            if (segment) {
              list_del(&segment->head);
              free(segment);
            }
        }
    }

    free(p_segment_ids);

    /* return if no segment or fail to add */
    if (!error && got_1st_seg) {

      /*copy the obsolete information, must for timeshifting*/
      if (ctx->playback.param_open.is_timeshift && ctx_record) {
        ctx->playback.obsolete = ctx_record->record.obsolete;
      }

      DVR_WRAPPER_INFO("playback(sn:%ld) (%d) segments added\n", ctx->sn, i);

      ctx->playback.reach_end = DVR_FALSE;
      if ((flags&DVR_PLAYBACK_STARTED_PAUSEDLIVE) == DVR_PLAYBACK_STARTED_PAUSEDLIVE)
        ctx->playback.speed = 0.0f;
      else
        ctx->playback.speed = 100.0f;

      ctx->playback.pids_req = *p_pids;
      //calculate segment id and pos
      if (dvr_playback_check_limit(ctx->playback.player)) {
        wrapper_mutex_unlock(&ctx->wrapper_lock);
        dvr_wrapper_seek_playback(playback, 0);
        wrapper_mutex_lock(&ctx->wrapper_lock);
        error = dvr_playback_start(ctx->playback.player, flags);
      } else {
        dvr_playback_seek(ctx->playback.player, seg_info_1st.id, 0);
        error = dvr_playback_start(ctx->playback.player, flags);
        DVR_WRAPPER_INFO("playback(sn:%ld) seek(seg:%llu 0) for start (%d)\n",
          ctx->sn, seg_info_1st.id, error);
      }
      DVR_WRAPPER_INFO("playback(sn:%ld) started (%d)\n", ctx->sn, error);
    } else {
      DVR_WRAPPER_INFO("playback(sn:%ld) started (%d)got_1st_seg:%d\n", ctx->sn, error, got_1st_seg);
    }
  }

  if (ctx->playback.param_open.is_timeshift) {
    /*unlock the recorder locked above*/
    if (ctx_record && ctx_valid(ctx_record)) {
        wrapper_mutex_unlock(&ctx_record->wrapper_lock);
        DVR_WRAPPER_INFO("playback(sn:%ld), record(sn:%ld) unlocked ok due to timeshift\n",
          ctx->sn, ctx_record->sn);
    }
  }
  wrapper_mutex_unlock(&ctx->wrapper_lock);

  return error;
}
//stop record and playback
int dvr_wrapper_stop_timeshift (DVR_WrapperPlayback_t playback)
{
  DVR_WrapperCtx_t *ctx_record = NULL;/*for timeshift*/
  int error;
  DVR_WRAPPER_INFO("libdvr_api, stop_timeshift");

  //stop timeshift record
  ctx_record = ctx_getRecord(sn_timeshift_record);
  dvr_wrapper_stop_record((DVR_WrapperRecord_t)sn_timeshift_record);

  DVR_WRAPPER_INFO("stop timeshift ...stop play\n");
  //stop play
  error = dvr_wrapper_stop_playback(playback);
  //del timeshift file
  if (ctx_record != NULL) {
    DVR_WRAPPER_INFO("del timeshift(sn:%ld) ...3\n", ctx_record->sn);
    error = dvr_segment_del_by_location(ctx_record->record.param_open.location);
  }
  return error;
}
//start record and start playback
int dvr_wrapper_restart_timeshift(DVR_WrapperPlayback_t playback, DVR_PlaybackFlag_t flags, DVR_PlaybackPids_t *p_pids)
{
  DVR_WrapperCtx_t *ctx;
  DVR_RecordStartParams_t *start_param;
  int error;

  ctx = ctx_getRecord((unsigned long)sn_timeshift_record);
  DVR_RETURN_IF_FALSE(ctx);

  wrapper_mutex_lock(&ctx->wrapper_lock);
  DVR_WRAPPER_INFO("libdvr_api, restart_timeshift (sn:%ld) location:%s",
      ctx->sn, ctx->record.param_open.location);
  WRAPPER_RETURN_IF_FALSE_WITH_UNLOCK(ctx_valid(ctx), &ctx->wrapper_lock);

  {
    //clear old record status
    //   struct {
    //   DVR_WrapperRecordOpenParams_t   param_open;
    //   DVR_RecordStartParams_t         param_start;
    //   DVR_RecordStartParams_t         param_update;
    //   DVR_RecordHandle_t              recorder;
    //   DVR_RecordEventFunction_t       event_fn;
    //   void                            *event_userdata;

    //   /*total status = seg_status + status + obsolete*/
    //   DVR_RecordStatus_t              seg_status;            /**<status of current segment*/
    //   DVR_WrapperRecordStatus_t       status;                /**<status of remaining segments*/
    //   uint64_t                        next_segment_id;

    //   DVR_WrapperInfo_t               obsolete;             /**<data obsolete due to the max limit*/
    // } record;
    memset(&(ctx->record.seg_status), 0, sizeof(DVR_RecordStatus_t));
    memset(&(ctx->record.status), 0, sizeof(DVR_WrapperRecordStatus_t));
    memset(&(ctx->record.obsolete), 0, sizeof(DVR_WrapperInfo_t));
  }

  start_param = &ctx->record.param_start;

  error = dvr_record_start_segment(ctx->record.recorder, start_param);
  {
    DVR_RecordSegmentInfo_t new_seg_info =
      { .id = start_param->segment.segment_id, };
    wrapper_addRecordSegment(ctx, &new_seg_info);
    DVR_WRAPPER_INFO("re record(sn:%ld) started = (%d)start id[%lld]id+[%lld]update id[%lld]\n", ctx->sn, error, start_param->segment.segment_id, ctx->record.next_segment_id, ctx->record.param_update.segment.segment_id);
    ctx->record.next_segment_id = start_param->segment.segment_id + 1;
    DVR_RecordStartParams_t *update_param;
    update_param = &ctx->record.param_update;
    memcpy(update_param, start_param, sizeof(*update_param));
    int i = 0;
    for (i = 0; i < update_param->segment.nb_pids; i++)
      update_param->segment.pid_action[i] = DVR_RECORD_PID_KEEP;
  }

  DVR_WRAPPER_INFO("re record(sn:%ld) started = (%d)\n", ctx->sn, error);

  wrapper_mutex_unlock(&ctx->wrapper_lock);

  //start play
  DVR_WRAPPER_INFO("re start play and clear old status\n");
  //clear play statue
  ctx = ctx_getPlayback((unsigned long)playback);
  if (ctx) {
    //reset old playback status
    //     struct {
    //   DVR_WrapperPlaybackOpenParams_t param_open;
    //   DVR_PlaybackHandle_t            player;
    //   DVR_PlaybackEventFunction_t     event_fn;
    //   void                            *event_userdata;

    //   /*total status = seg_status + status*/
    //   DVR_PlaybackStatus_t            seg_status;
    //   DVR_WrapperPlaybackStatus_t     status;
    //   DVR_PlaybackPids_t              pids_req;
    //   DVR_PlaybackEvent_t             last_event;
    //   float                           speed;
    //   DVR_Bool_t                      reach_end;

    //   DVR_WrapperInfo_t               obsolete;
    //   DVR_Bool_t                      tf_full;
    // } playback;
    ctx->playback.tf_full = DVR_FALSE;
    ctx->playback.reach_end = DVR_FALSE;
    memset(&(ctx->playback.last_event), 0, sizeof(DVR_PlaybackEvent_t));
    memset(&(ctx->playback.seg_status), 0, sizeof(DVR_PlaybackStatus_t));
    memset(&(ctx->playback.status), 0, sizeof(DVR_WrapperPlaybackStatus_t));
    memset(&(ctx->playback.obsolete), 0, sizeof(DVR_WrapperInfo_t));
  }
  error = dvr_wrapper_start_playback(playback, flags, p_pids);
  return error;
}

int dvr_wrapper_stop_playback (DVR_WrapperPlayback_t playback)
{
  DVR_WrapperCtx_t *ctx;
  int error;

  DVR_RETURN_IF_FALSE(playback);

  ctx = ctx_getPlayback((unsigned long)playback);
  DVR_RETURN_IF_FALSE(ctx);

  wrapper_mutex_lock(&ctx->wrapper_lock);
  DVR_WRAPPER_INFO("libdvr_api, stop_playback (sn:%ld)", ctx->sn);
  WRAPPER_RETURN_IF_FALSE_WITH_UNLOCK(ctx_valid(ctx), &ctx->wrapper_lock);

  error = dvr_playback_stop(ctx->playback.player, DVR_TRUE);

  {
    /*remove all segments*/
    DVR_WrapperPlaybackSegmentInfo_t *p_seg;

    // This error is suppressed as the macro code is picked from kernel.
    // prefetch() here incurring self_assign is used to avoid some compiling
    // warnings.
    // coverity[self_assign]
    list_for_each_entry(p_seg, &ctx->segments, head) {
      error = dvr_playback_remove_segment(ctx->playback.player, p_seg->playback_info.segment_id);
      DVR_WRAPPER_INFO("playback(sn:%ld) remove seg(%lld) (%d)\n",
        ctx->sn, p_seg->playback_info.segment_id, error);
    }
    ctx_freeSegments(ctx);
  }

  DVR_WRAPPER_INFO("playback(sn:%ld) stopped (%d)\n", ctx->sn, error);
  wrapper_mutex_unlock(&ctx->wrapper_lock);

  return error;
}

int dvr_wrapper_pause_playback (DVR_WrapperPlayback_t playback)
{
  DVR_WrapperCtx_t *ctx;
  int error;

  DVR_RETURN_IF_FALSE(playback);

  ctx = ctx_getPlayback((unsigned long)playback);
  DVR_RETURN_IF_FALSE(ctx);

  wrapper_mutex_lock(&ctx->wrapper_lock);
  DVR_WRAPPER_INFO("libdvr_api, pause_playback (sn:%ld)", ctx->sn);
  WRAPPER_RETURN_IF_FALSE_WITH_UNLOCK(ctx_valid(ctx), &ctx->wrapper_lock);
  //clear end event
  if (ctx->playback.last_event == DVR_PLAYBACK_EVENT_REACHED_END)
      ctx->playback.last_event = DVR_PLAYBACK_EVENT_TRANSITION_OK;

  error = dvr_playback_pause(ctx->playback.player, DVR_FALSE);

  ctx->playback.speed = 0.0f;

  DVR_WRAPPER_INFO("playback(sn:%ld) paused (%d)\n", ctx->sn, error);
  wrapper_mutex_unlock(&ctx->wrapper_lock);

  return error;
}

int dvr_wrapper_resume_playback (DVR_WrapperPlayback_t playback)
{
  DVR_WrapperCtx_t *ctx;
  int error;

  DVR_RETURN_IF_FALSE(playback);

  ctx = ctx_getPlayback((unsigned long)playback);
  DVR_RETURN_IF_FALSE(ctx);
  //if set limit.we need check if seek to valid data when resume
  uint32_t time_offset = ctx->playback.status.info_cur.time + ctx->playback.status.info_obsolete.time;
  if (dvr_playback_check_limit(ctx->playback.player)) {
    int expired = dvr_playback_calculate_expiredlen(ctx->playback.player);
    if (expired > time_offset) {
      DVR_WRAPPER_INFO("seek before resume reset offset playback(sn:%ld) (off:%d expired:%d)\n",
        ctx->sn, time_offset, expired);
      time_offset = expired;
      dvr_wrapper_seek_playback(playback, time_offset);
    }
  }
  wrapper_mutex_lock(&ctx->wrapper_lock);
  DVR_WRAPPER_INFO("libdvr_api, resume_playback (sn:%ld)", ctx->sn);
  WRAPPER_RETURN_IF_FALSE_WITH_UNLOCK(ctx_valid(ctx), &ctx->wrapper_lock);

  error = dvr_playback_resume(ctx->playback.player);
  ctx->playback.speed = 100.0f;

  DVR_WRAPPER_INFO("playback(sn:%ld) resumed (%d)\n", ctx->sn, error);
  wrapper_mutex_unlock(&ctx->wrapper_lock);

  return error;
}

int dvr_wrapper_set_playback_speed (DVR_WrapperPlayback_t playback, float speed)
{
  DVR_WrapperCtx_t *ctx;
  int error;
  DVR_PlaybackSpeed_t dvr_speed = {
     .speed = { speed },
     .mode = (speed > 0) ? DVR_PLAYBACK_FAST_FORWARD : DVR_PLAYBACK_FAST_BACKWARD
  };

  DVR_RETURN_IF_FALSE(playback);

  ctx = ctx_getPlayback((unsigned long)playback);
  DVR_RETURN_IF_FALSE(ctx);

  wrapper_mutex_lock(&ctx->wrapper_lock);
  DVR_WRAPPER_INFO("libdvr_api, set_playback_speed (sn:%ld) speed:%d", ctx->sn, (int)speed);
  WRAPPER_RETURN_IF_FALSE_WITH_UNLOCK(ctx_valid(ctx), &ctx->wrapper_lock);

  error = dvr_playback_set_speed(ctx->playback.player, dvr_speed);

  if (ctx->playback.speed != 0.0f && ctx->playback.speed != 100.0f
      && ctx->playback.last_event == DVR_PLAYBACK_EVENT_REACHED_BEGIN
      && ctx->playback.seg_status.state == DVR_PLAYBACK_STATE_PAUSE) {
    DVR_WRAPPER_INFO("x%f -> x%f, paused, do resume first\n", ctx->playback.speed, speed);
    error = dvr_playback_resume(ctx->playback.player);
  } else if (ctx->playback.speed == 0.0f
      && speed != 0.0f
      && speed != 100.0f) {
    /*libdvr do not support pause with speed=0, will not be here*/
    DVR_WRAPPER_INFO("x%f -> x%f, do resume first\n", ctx->playback.speed, speed);
    error = dvr_playback_resume(ctx->playback.player);
  }

  ctx->playback.speed = speed;

  DVR_WRAPPER_INFO("playback(sn:%ld) speeded(x%f) (%d)\n",
    ctx->sn, speed, error);
  wrapper_mutex_unlock(&ctx->wrapper_lock);

  return error;
}

int dvr_wrapper_setlimit_playback (DVR_WrapperPlayback_t playback, uint64_t time, int32_t limit)
{
  DVR_WrapperCtx_t *ctx;
  int error;

  DVR_RETURN_IF_FALSE(playback);

  ctx = ctx_getPlayback((unsigned long)playback);
  DVR_RETURN_IF_FALSE(ctx);

  wrapper_mutex_lock(&ctx->wrapper_lock);

  DVR_WRAPPER_INFO("libdvr_api, setlimit_playback (sn:%ld) time:%lld, limit:%d",
      ctx->sn, time, limit);
  WRAPPER_RETURN_IF_FALSE_WITH_UNLOCK(ctx_valid(ctx), &ctx->wrapper_lock);

  error = dvr_playback_setlimit(ctx->playback.player, time, limit);
  DVR_WRAPPER_INFO("playback(sn:%ld) set_limit(time:%lld limit:%d) ...\n", ctx->sn, time, limit);

  wrapper_mutex_unlock(&ctx->wrapper_lock);

  return error;
}

int dvr_wrapper_seek_playback (DVR_WrapperPlayback_t playback, uint32_t time_offset)
{
  DVR_WrapperCtx_t *ctx;
  int error;
  DVR_WrapperPlaybackSegmentInfo_t *p_seg;
  uint64_t segment_id = ULLONG_MAX;
  uint32_t segment_offset = 0;

  DVR_RETURN_IF_FALSE(playback);

  ctx = ctx_getPlayback((unsigned long)playback);
  DVR_RETURN_IF_FALSE(ctx);

  wrapper_mutex_lock(&ctx->wrapper_lock);

  DVR_WRAPPER_INFO("libdvr_api, seek_playback (sn:%ld) offset:%dms", ctx->sn, time_offset);
  WRAPPER_RETURN_IF_FALSE_WITH_UNLOCK(ctx_valid(ctx), &ctx->wrapper_lock);

  //if set limit info we need check ts data is
  //expired when seek
  if (dvr_playback_check_limit(ctx->playback.player)) {
    int expired = dvr_playback_calculate_expiredlen(ctx->playback.player);
    if (expired > time_offset) {
      DVR_WRAPPER_INFO("seek reset offset playback(sn:%ld) (off:%d expired:%d)\n",
        ctx->sn, time_offset, expired);
      time_offset = expired;
    }
  }
  ctx->playback.reach_end = DVR_FALSE;

  const uint32_t obsolete_time = (uint32_t)ctx->playback.obsolete.time;
  DVR_WrapperPlaybackSegmentInfo_t *p_seg_first = ctx->segments.c_prev;
  DVR_WrapperPlaybackSegmentInfo_t *p_seg_last = ctx->segments.c_next;
  const uint64_t first_id = p_seg_first->seg_info.id;
  const uint64_t last_id = p_seg_last->seg_info.id;
  const uint32_t last_duration = p_seg_last->seg_info.duration;

  if (time_offset <= obsolete_time) {
    segment_id = first_id;
    segment_offset = 0;
    DVR_WRAPPER_WARN("time_offset %u isn't greater than obsolete time %u, "
        "so seek to beginning position of segment %llu",
        time_offset,obsolete_time,segment_id);
  } else {
    uint32_t total_duration = 0;
    // This error is suppressed as the macro code is picked from kernel.
    // prefetch() here incurring self_assign is used to avoid some compiling
    // warnings.
    // coverity[self_assign]
    list_for_each_entry_reverse(p_seg, &ctx->segments, head) {
      const uint32_t segment_begin = obsolete_time + total_duration;
      const uint32_t segment_end = segment_begin + p_seg->seg_info.duration;
      if (time_offset >= segment_begin && time_offset <= segment_end) {
        segment_id = p_seg->seg_info.id;
        segment_offset = time_offset - segment_begin;
        break;
      }
      total_duration += p_seg->seg_info.duration;
    }
    if (segment_id == ULLONG_MAX) {
      segment_id = last_id;
      segment_offset = last_duration;
      DVR_WRAPPER_WARN("time_offset %u is out of range, so seek to"
          " the end position of segment %llu",time_offset,segment_id);
    }
  }

  DVR_WRAPPER_INFO("seek playback(sn:%ld) (segment_id:%llu, segment_offset:%u)\n",
      ctx->sn, segment_id, segment_offset);
  error = dvr_playback_seek(ctx->playback.player, segment_id, segment_offset);

  wrapper_mutex_unlock(&ctx->wrapper_lock);

  return error;
}

int dvr_wrapper_update_playback (DVR_WrapperPlayback_t playback, DVR_PlaybackPids_t *p_pids)
{
  DVR_WrapperCtx_t *ctx;
  int error;
  DVR_WrapperPlaybackSegmentInfo_t *p_seg;

  DVR_RETURN_IF_FALSE(playback);
  DVR_RETURN_IF_FALSE(p_pids);

  ctx = ctx_getPlayback((unsigned long)playback);
  DVR_RETURN_IF_FALSE(ctx);

  wrapper_mutex_lock(&ctx->wrapper_lock);

  DVR_WRAPPER_INFO("libdvr_api, update_playback (sn:%ld) v/a(%d:%d/%d:%d)",
    ctx->sn,
    p_pids->video.pid, p_pids->video.format,
    p_pids->audio.pid, p_pids->audio.format);
  WRAPPER_RETURN_IF_FALSE_WITH_UNLOCK(ctx_valid(ctx), &ctx->wrapper_lock);

  ctx->playback.pids_req = *p_pids;

  error = 0;
  // This error is suppressed as the macro code is picked from kernel.
  // prefetch() here incurring self_assign is used to avoid some compiling
  // warnings.
  // coverity[self_assign]
  list_for_each_entry_reverse(p_seg, &ctx->segments, head) {
    /*should update the whole list of segments*/
    /*if (p_seg->seg_info.id == ctx->current_segment_id)*/ {
      /*list_for_each_entry_from(p_seg, &ctx->segments, head)*/ {
        /*check update for pids*/
        if (memcmp(&p_seg->playback_info.pids, p_pids, sizeof(*p_pids)) != 0) {
          p_seg->playback_info.pids = *p_pids;
          error = dvr_playback_update_segment_pids(ctx->playback.player, p_seg->seg_info.id, p_pids);
          if (error) {
            DVR_WRAPPER_INFO("failed to playback(sn:%ld) update segment(id:%lld) pids (%d)\n",
              ctx->sn, p_seg->seg_info.id, error);
            /*do not break, let list updated*/
          }
        }
      }
      /*break;*/
    }
  }

  DVR_WRAPPER_INFO("update playback(sn:%ld) v/a(%d:%d/%d:%d) (%d)\n",
    ctx->sn,
    p_pids->video.pid, p_pids->video.format,
    p_pids->audio.pid, p_pids->audio.format,
    error);

  wrapper_mutex_unlock(&ctx->wrapper_lock);

  return error;
}

int dvr_wrapper_only_update_playback (DVR_WrapperPlayback_t playback, DVR_PlaybackPids_t *p_pids)
{
  DVR_WrapperCtx_t *ctx;
  int error;
  DVR_WrapperPlaybackSegmentInfo_t *p_seg;

  DVR_RETURN_IF_FALSE(playback);
  DVR_RETURN_IF_FALSE(p_pids);

  ctx = ctx_getPlayback((unsigned long)playback);
  DVR_RETURN_IF_FALSE(ctx);

  wrapper_mutex_lock(&ctx->wrapper_lock);

  DVR_WRAPPER_INFO("libdvr_api, only_update_playback (sn:%ld) v/a(%d:%d/%d:%d)",
    ctx->sn,
    p_pids->video.pid, p_pids->video.format,
    p_pids->audio.pid, p_pids->audio.format);
  WRAPPER_RETURN_IF_FALSE_WITH_UNLOCK(ctx_valid(ctx), &ctx->wrapper_lock);

  ctx->playback.pids_req = *p_pids;

  error = 0;
  // This error is suppressed as the macro code is picked from kernel.
  // prefetch() here incurring self_assign is used to avoid some compiling
  // warnings.
  // coverity[self_assign]
  list_for_each_entry_reverse(p_seg, &ctx->segments, head) {
    /*should update the whole list of segments*/
    /*if (p_seg->seg_info.id == ctx->current_segment_id)*/ {
      /*list_for_each_entry_from(p_seg, &ctx->segments, head)*/ {
        /*check update for pids*/
        if (memcmp(&p_seg->playback_info.pids, p_pids, sizeof(*p_pids)) != 0) {
          p_seg->playback_info.pids = *p_pids;
          error = dvr_playback_only_update_segment_pids(ctx->playback.player, p_seg->seg_info.id, p_pids);
          if (error) {
            DVR_WRAPPER_INFO("failed to playback(sn:%ld) update segment(id:%lld) pids (%d)\n",
              ctx->sn, p_seg->seg_info.id, error);
            /*do not break, let list updated*/
          }
        }
      }
      /*break;*/
    }
  }

  DVR_WRAPPER_INFO("update playback(sn:%ld) v/a(%d:%d/%d:%d) (%d)\n",
    ctx->sn,
    p_pids->video.pid, p_pids->video.format,
    p_pids->audio.pid, p_pids->audio.format,
    error);

  wrapper_mutex_unlock(&ctx->wrapper_lock);

  return error;
}

int dvr_wrapper_get_playback_status(DVR_WrapperPlayback_t playback, DVR_WrapperPlaybackStatus_t *status)
{
  DVR_WrapperCtx_t *ctx;
  DVR_WrapperPlaybackStatus_t s;
  DVR_PlaybackStatus_t play_status;
  int error;

  DVR_RETURN_IF_FALSE(playback);
  DVR_RETURN_IF_FALSE(status);

  ctx = ctx_getPlayback((unsigned long)playback);
  DVR_RETURN_IF_FALSE(ctx);

  wrapper_mutex_lock(&ctx->wrapper_lock);

  WRAPPER_RETURN_IF_FALSE_WITH_UNLOCK(ctx_valid(ctx), &ctx->wrapper_lock);

  dvr_playback_get_status(ctx->playback.player, &play_status);

  ctx->playback.seg_status = play_status;
  error = process_generatePlaybackStatus(ctx, &s);

  if (ctx->playback.reach_end == DVR_TRUE && ctx->playback.param_open.is_timeshift == DVR_FALSE) {
    //reach end need set full time to cur.so app can exist playback.
    DVR_WRAPPER_INFO("set cur time to full time, reach end occur");
    s.info_cur.time = s.info_full.time;
  }
  DVR_WRAPPER_INFO("get_playback_status (sn:%ld) state/cur/full/obsolete(%d/%ld/%ld/%ld)",
    ctx->sn, s.state, s.info_cur.time, s.info_full.time, s.info_obsolete.time);

  *status = s;

  wrapper_mutex_unlock(&ctx->wrapper_lock);

  return error;
}

int dvr_wrapper_set_playback_secure_buffer (DVR_WrapperPlayback_t playback,  uint8_t *p_secure_buf, uint32_t len)
{
  DVR_WrapperCtx_t *ctx;
  int error;

  DVR_RETURN_IF_FALSE(playback);
  DVR_RETURN_IF_FALSE(p_secure_buf);

  ctx = ctx_getPlayback((unsigned long)playback);
  DVR_RETURN_IF_FALSE(ctx);

  wrapper_mutex_lock(&ctx->wrapper_lock);
  error = dvr_playback_set_secure_buffer(ctx->playback.player, p_secure_buf, len);
  wrapper_mutex_unlock(&ctx->wrapper_lock);
  return error;
}

int dvr_wrapper_set_playback_decrypt_callback (DVR_WrapperPlayback_t playback,  DVR_CryptoFunction_t func, void *userdata)
{
  DVR_WrapperCtx_t *ctx;
  int error;

  DVR_RETURN_IF_FALSE(playback);
  DVR_RETURN_IF_FALSE(func);

  ctx = ctx_getPlayback((unsigned long)playback);
  DVR_RETURN_IF_FALSE(ctx);

  wrapper_mutex_lock(&ctx->wrapper_lock);
  error = dvr_playback_set_decrypt_callback(ctx->playback.player, func, userdata);
  wrapper_mutex_unlock(&ctx->wrapper_lock);
  return error;
}

int dvr_wrapper_segment_del_by_location (const char *location)
{
  char fpath[DVR_MAX_LOCATION_SIZE];

  DVR_RETURN_IF_FALSE(location);

  /*del the stats file*/
  sprintf(fpath, "%s.stats", location);
  unlink(fpath);

  return dvr_segment_del_by_location(location);
}

int dvr_wrapper_segment_get_info_by_location (const char *location, DVR_WrapperInfo_t *p_info)
{
  FILE *fp;
  char fpath[DVR_MAX_LOCATION_SIZE];

  DVR_RETURN_IF_FALSE(location);
  DVR_RETURN_IF_FALSE(p_info);

  if (p_info)
    memset(p_info, 0, sizeof(p_info[0]));

  memset(fpath, 0, sizeof(fpath));
  sprintf(fpath, "%s.stats", location);

  /*stats file exists*/
  if ((fp = fopen(fpath, "r"))) {
    char buf[256];

    if (fgets(buf, sizeof(buf), fp) != NULL
      && (sscanf(buf, ":%llu:%lu:%u",
        &p_info->size,
        &p_info->time,
        &p_info->pkts) == 3)) {
      fclose(fp);
      DVR_WRAPPER_INFO("rec(%s) t/s/p:(%lu/%llu/%u)\n", location, p_info->time, p_info->size, p_info->pkts);
      return DVR_SUCCESS;
    }
    fclose(fp);
  }

  /*fallback, slow on mass files*/
  DVR_WRAPPER_INFO("rec '%s.stats' invalid.\n", location);

  int error;
  uint32_t n_ids;
  uint64_t *p_ids;

  error = dvr_segment_get_list(location, &n_ids, &p_ids);

  if (!error) {
     int i;
     struct list_head           info_list;         /**< segment list head*/
    INIT_LIST_HEAD(&info_list);

    //we need free info list buf when we used end.
    error = dvr_segment_get_allInfo(location, &info_list);
    if (error == DVR_FAILURE) {
      DVR_RecordSegmentInfo_t info = { .id = 0, .nb_pids = 0,
        .pids = {0}, .duration = 0, .size = 0, .nb_packets = 0 };

      memset(&info, 0, sizeof(info));
      error = DVR_FAILURE;
      DVR_WRAPPER_INFO("fail to get seg info (location:%s, seg:%llu), (error:%d)\n",
        location, 0, error);

      // Tainted data issue originating from fgets seem false positive, so we
      // just suppress it here.
      // coverity[tainted_data]
      for (i = 0; i < n_ids; i++) {
          error = dvr_segment_get_info(location, p_ids[i], &info);
          if (!error) {
            p_info->size += info.size;
            p_info->time += info.duration;
            p_info->pkts += info.nb_packets;
          } else {
            DVR_WRAPPER_INFO("%s:%lld get seg info fail.\n", location, p_ids[i]);
            break;
          }
      }
    } else {
      DVR_WRAPPER_INFO("get list segment_nb::%d",n_ids);
      // Tainted data issue originating from fgets seem false positive, so we
      // just suppress it here.
      // coverity[tainted_data]
      for (i = 0; i < n_ids; i++) {

          DVR_RecordSegmentInfo_t *seg_info;
          DVR_PlaybackSegmentFlag_t flags;
          int found = 0;
          // This error is suppressed as the macro code is picked from kernel.
          // prefetch() here incurring self_assign is used to avoid some compiling
          // warnings.
          // coverity[self_assign]
          list_for_each_entry(seg_info, &info_list, head)
          {
            if (seg_info->id == p_ids[i]) {
              found = 1;
              break;
            }
          }
          if (!found) {
              DVR_WRAPPER_INFO("get segment info::%d [%d]n_ids[%d]error", i, p_ids[i], n_ids);
              if (p_ids[i] == n_ids - 1) {
                DVR_RecordSegmentInfo_t info = { .id = 0, .nb_pids = 0,
                  .pids = {0}, .duration = 0, .size = 0, .nb_packets = 0 };
                DVR_WRAPPER_INFO("get last segment info::%d [%d]n_ids[%d] from subfile", i, p_ids[i], n_ids);
                error = dvr_segment_get_info(location, p_ids[i], &info);
                if (!error) {
                  p_info->size += info.size;
                  p_info->time += info.duration;
                  p_info->pkts += info.nb_packets;
                } else {
                  DVR_WRAPPER_INFO("%s:%lld get seg info fail.\n", location, p_ids[i]);
                  break;
                }
              }
              continue;
          }

          if (!error) {
            p_info->size += seg_info->size;
            p_info->time += seg_info->duration;
            p_info->pkts += seg_info->nb_packets;
          } else {
            DVR_WRAPPER_INFO("%s:%lld get seg info fail.\n", location, p_ids[i]);
            break;
          }
      }
      //free list
      DVR_RecordSegmentInfo_t *segment = NULL;
      DVR_RecordSegmentInfo_t *segment_tmp = NULL;
      list_for_each_entry_safe(segment, segment_tmp, &info_list, head)
      {
          if (segment) {
            list_del(&segment->head);
            free(segment);
          }
      }
    }
    free(p_ids);
  } else {
     n_ids = 0;
  }
  DVR_WRAPPER_INFO("rec(%s)... t/s/p:(%lu/%llu/%u) segs(%u) error(%d)\n", location, p_info->time, p_info->size, p_info->pkts, n_ids, error);

  return (error)? DVR_FAILURE : DVR_SUCCESS;
}

static DVR_Result_t wrapper_record_event_handler(DVR_RecordEvent_t event, void *params, void *userdata)
{
  DVR_WrapperEventCtx_t evt = {
    .sn = (unsigned long)userdata,
    .type = W_REC,
    .record.event = event,
    .record.status = *(DVR_RecordStatus_t *)params
  };

  DVR_RETURN_IF_FALSE(userdata);

  DVR_WRAPPER_DEBUG("record event 0x%x (sn:%ld)", evt.record.event, evt.sn);
  return ctx_addRecordEvent(&evt);
}

static DVR_Result_t wrapper_playback_event_handler(DVR_PlaybackEvent_t event, void *params, void *userdata)
{
  DVR_WrapperEventCtx_t evt = {
    .sn = (unsigned long)userdata,
    .type = W_PLAYBACK,
    .playback.event = event,
    .playback.status = *(DVR_Play_Notify_t *)params
  };

  DVR_RETURN_IF_FALSE(userdata);

  evt.sn = (unsigned long)userdata;
  evt.type = W_PLAYBACK;
  evt.playback.event = event;
  evt.playback.status = *(DVR_Play_Notify_t *)params;
  DVR_WRAPPER_DEBUG("playback event 0x%x (sn:%ld)", evt.playback.event, evt.sn);
  return ctx_addPlaybackEvent(&evt);
}

static inline int process_notifyRecord(DVR_WrapperCtx_t *ctx, DVR_RecordEvent_t evt, DVR_WrapperRecordStatus_t *status)
{
  DVR_WRAPPER_INFO("notify(sn:%ld) evt(0x%x) statistic:time/size/pkts(%ld/%lld/%u) obsolete:(%ld/%llu/%u)\n",
    ctx->sn,
    evt,
    status->info.time,
    status->info.size,
    status->info.pkts,
    status->info_obsolete.time,
    status->info_obsolete.size,
    status->info_obsolete.pkts);

  if (ctx->record.event_fn)
    return ctx->record.event_fn(evt, status, ctx->record.event_userdata);
  return 0;
}

static int wrapper_saveRecordStatistics(const char *location, DVR_WrapperRecordStatus_t *p_status)
{
  FILE *fp;
  char fpath[DVR_MAX_LOCATION_SIZE];

  DVR_RETURN_IF_FALSE(location);
  DVR_RETURN_IF_FALSE(p_status);

  sprintf(fpath, "%s.stats", location);

  /*stats file*/
  if ((fp = fopen(fpath, "w"))) {
    char buf[256];
    snprintf(buf, sizeof(buf), ":%llu:%lu:%u\n",
        p_status->info.size - p_status->info_obsolete.size,
        p_status->info.time - p_status->info_obsolete.time,
        p_status->info.pkts - p_status->info_obsolete.pkts);
    fputs(buf, fp);
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    return DVR_SUCCESS;
  }

  return DVR_FAILURE;
}


static inline int record_startNextSegment(DVR_WrapperCtx_t *ctx)
{
  DVR_RecordStartParams_t param;
  DVR_RecordSegmentInfo_t seg_info;
  int i;
  int error;

  memcpy(&param, &ctx->record.param_update, sizeof(param));
  memset(&ctx->record.param_update.segment, 0, sizeof(ctx->record.param_update.segment));
  ctx->record.param_update.segment.segment_id = ctx->record.next_segment_id++;
  for (i = 0; i < param.segment.nb_pids; i++) {
    if (param.segment.pid_action[i] != DVR_RECORD_PID_CLOSE) {
      ctx->record.param_update.segment.pids[ctx->record.param_update.segment.nb_pids] = param.segment.pids[i];
      ctx->record.param_update.segment.pid_action[ctx->record.param_update.segment.nb_pids] = DVR_RECORD_PID_KEEP;
      ctx->record.param_update.segment.nb_pids++;
    }
  }
  error = dvr_record_next_segment(ctx->record.recorder, &ctx->record.param_update, &seg_info);
  {
    DVR_RecordSegmentInfo_t new_seg_info =
      { .id = ctx->record.param_update.segment.segment_id, };
    wrapper_updateRecordSegment(ctx, &seg_info, U_ALL);
    wrapper_addRecordSegment(ctx, &new_seg_info);
  }

  DVR_WRAPPER_INFO("record next segment(%llu)=(%d)\n", ctx->record.param_update.segment.segment_id, error);
  return error;
}

static inline int record_removeSegment(DVR_WrapperCtx_t *ctx, DVR_WrapperRecordSegmentInfo_t *p_seg)
{
  return wrapper_removeRecordSegment(ctx, p_seg);
}

/*should run periodically to update the current status*/
static int process_generateRecordStatus(DVR_WrapperCtx_t *ctx, DVR_WrapperRecordStatus_t *status)
{
  /*the current seg is not covered in the statistics*/
  DVR_WrapperRecordSegmentInfo_t *p_seg;

  /*re-calculate the all segments*/
  memset(&ctx->record.status, 0, sizeof(ctx->record.status));

  ctx->record.status.state = ctx->record.seg_status.state;
  ctx->record.status.pids.nb_pids = ctx->record.seg_status.info.nb_pids;
  memcpy(ctx->record.status.pids.pids,
    ctx->record.seg_status.info.pids,
    sizeof(ctx->record.status.pids.pids));
  ctx->current_segment_id = ctx->record.seg_status.info.id;

  // This error is suppressed as the macro code is picked from kernel.
  // prefetch() here incurring self_assign is used to avoid some compiling
  // warnings.
  // coverity[self_assign]
  list_for_each_entry_reverse(p_seg, &ctx->segments, head) {
    if (p_seg->info.id != ctx->record.seg_status.info.id) {
      ctx->record.status.info.time += p_seg->info.duration;
      ctx->record.status.info.size += p_seg->info.size;
      ctx->record.status.info.pkts += p_seg->info.nb_packets;
    }
  }

  ctx->record.status.info_obsolete = ctx->record.obsolete;

  wrapper_updateRecordSegment(ctx, &ctx->record.seg_status.info, U_ALL);

  if (status) {
    *status = ctx->record.status;
    status->info.time += ctx->record.seg_status.info.duration;
    status->info.size += ctx->record.seg_status.info.size;
    status->info.pkts += ctx->record.seg_status.info.nb_packets;
  }

  return DVR_SUCCESS;
}


static int process_handleRecordEvent(DVR_WrapperEventCtx_t *evt, DVR_WrapperCtx_t *ctx)
{
  DVR_WrapperRecordStatus_t status;

  memset(&status, 0, sizeof(status));

  DVR_WRAPPER_DEBUG("evt (sn:%ld) 0x%x (state:%d)\n",
      evt->sn, evt->record.event, evt->record.status.state);
  if (ctx->record.param_update.segment.segment_id != evt->record.status.info.id) {
    DVR_WRAPPER_INFO("evt (sn:%ld) cur id:0x%x (event id:%d)\n",
    evt->sn, (int)ctx->record.param_update.segment.segment_id, (int)evt->record.status.info.id);
    return 0;
  }
  switch (evt->record.event)
  {
    case DVR_RECORD_EVENT_STATUS:
    {
      switch (evt->record.status.state)
      {
        case DVR_RECORD_STATE_OPENED:
        case DVR_RECORD_STATE_CLOSED:
        {
          ctx->record.seg_status = evt->record.status;

          status.state = evt->record.status.state;
          process_notifyRecord(ctx, evt->record.event, &status);
          wrapper_saveRecordStatistics(ctx->record.param_open.location, &status);
        } break;
        case DVR_RECORD_STATE_STARTED:
        {
          ctx->record.seg_status = evt->record.status;

          process_generateRecordStatus(ctx, &status);
          process_notifyRecord(ctx, evt->record.event, &status);
          wrapper_saveRecordStatistics(ctx->record.param_open.location, &status);

          /*restart to next segment*/
          if (ctx->record.param_open.segment_size
              && evt->record.status.info.size >= ctx->record.param_open.segment_size) {
            DVR_WRAPPER_INFO("start new segment for record(%lu), reaches segment size limit, cur(%zu) max(%lld)\n",
              ctx->sn,
              evt->record.status.info.size,
              ctx->record.param_open.segment_size);
            if (record_startNextSegment(ctx) != DVR_SUCCESS) {
              /*should notify the recording's stop*/
              int error = dvr_record_close(ctx->record.recorder);
              DVR_WRAPPER_INFO("stop record(%lu)=%d, failed to start new segment for recording.",
                ctx->sn, error);
              status.state = DVR_RECORD_STATE_CLOSED;
              process_notifyRecord(ctx, DVR_RECORD_EVENT_WRITE_ERROR, &status);
              wrapper_saveRecordStatistics(ctx->record.param_open.location, &status);
            }
          }

          if (ctx->record.param_open.is_timeshift
              && ctx->record.param_open.max_time
              && status.info.time >= ctx->record.param_open.max_time) {
            DVR_WrapperRecordSegmentInfo_t *p_seg;

            /*as the player do not support null playlist,
              there must be one segment existed at any time,
              we have to keep two segments before remove one*/
            p_seg = list_last_entry(&ctx->segments, DVR_WrapperRecordSegmentInfo_t, head);
            if (p_seg == list_first_entry(&ctx->segments, DVR_WrapperRecordSegmentInfo_t, head)) {
              /*only one segment, waiting for more*/
              DVR_WRAPPER_INFO("warning: the size(%lld) of max_time(%ld) of record < max size of segment(%lld)\n",
                status.info.size,
                ctx->record.param_open.max_time,
                ctx->record.param_open.segment_size);
            } else {
              /*timeshifting, remove the 1st segment and notify the player*/
              record_removeSegment(ctx, p_seg);

              process_generateRecordStatus(ctx, &status);
              process_notifyRecord(ctx, evt->record.event, &status);
              wrapper_saveRecordStatistics(ctx->record.param_open.location, &status);
            }
          }

          const loff_t actual_size = status.info.size;
          const loff_t max_size = ctx->record.param_open.max_size;
          const loff_t segment_size = ctx->record.param_open.segment_size;

          if (ctx->record.param_open.is_timeshift && max_size) {
            if (actual_size >= max_size) {
              DVR_WrapperRecordSegmentInfo_t *p_seg;
              /*as the player do not support null playlist,
                there must be one segment existed at any time,
                we have to keep two segments before remove one*/
              p_seg = list_last_entry(&ctx->segments, DVR_WrapperRecordSegmentInfo_t, head);
              if (p_seg == list_first_entry(&ctx->segments, DVR_WrapperRecordSegmentInfo_t, head)) {
                /*only one segment, waiting for more*/
                DVR_WRAPPER_INFO("warning: the size(%lld) of record < max size of segment(%lld)\n",
                  actual_size, segment_size);
              } else {
                record_removeSegment(ctx, p_seg);

                process_generateRecordStatus(ctx, &status);
                process_notifyRecord(ctx, evt->record.event, &status);
                wrapper_saveRecordStatistics(ctx->record.param_open.location, &status);
              }
              if (actual_size >= max_size + segment_size/2) {
                dvr_record_discard_coming_data(ctx->record.recorder,DVR_TRUE);
              }
            } else {
              dvr_record_discard_coming_data(ctx->record.recorder,DVR_FALSE);
            }
          }
        } break;
        case DVR_RECORD_STATE_STOPPED:
        {
          ctx->record.seg_status = evt->record.status;

          process_generateRecordStatus(ctx, &status);
          process_notifyRecord(ctx, evt->record.event, &status);
          wrapper_saveRecordStatistics(ctx->record.param_open.location, &status);
        } break;
        default:
        break;
      }
    } break;
    case DVR_RECORD_EVENT_WRITE_ERROR: {
      ctx->record.seg_status = evt->record.status;
      status.state = evt->record.status.state;
      process_notifyRecord(ctx, evt->record.event, &status);
    }break;
    default:
    break;
  }
  return DVR_SUCCESS;
}

static inline int process_notifyPlayback(DVR_WrapperCtx_t *ctx, DVR_PlaybackEvent_t evt, DVR_WrapperPlaybackStatus_t *status)
{
  DVR_RETURN_IF_FALSE(ctx->playback.event_fn != NULL);

  const time_t cur_time = status->info_cur.time;
  const time_t full_time = status->info_full.time;
  const time_t obsolete_time = status->info_obsolete.time;
  const time_t origin_offset = cur_time + obsolete_time;
  DVR_WRAPPER_INFO("playback progress notify(sn:%ld) evt(0x%x)"
      " actual_slider_pos: %02d:%02d:%02d.%03d/%02d:%02d:%02d.%03d (%7ld ms/%7ld ms),"
      " offset_from_origin: %02d:%02d:%02d.%03d (%7ld ms),"
      " dump status:state/cur/full/obsolete(%d/%ld/%ld/%ld)",
      ctx->sn,evt,cur_time/1000/3600,cur_time/1000%3600/60,cur_time/1000%60,cur_time%1000,
      full_time/1000/3600,full_time/1000%3600/60,full_time/1000%60,full_time%1000,
      cur_time,full_time,
      origin_offset/1000/3600,origin_offset/1000%3600/60,origin_offset/1000%60,origin_offset%1000,
      origin_offset,status->state,cur_time,full_time,obsolete_time);

  return ctx->playback.event_fn(evt, status, ctx->playback.event_userdata);
}

/*should run periodically to update the current status*/
static int process_generatePlaybackStatus(DVR_WrapperCtx_t *ctx, DVR_WrapperPlaybackStatus_t *status)
{
  /*the current seg is not covered in the statistics*/
  DVR_WrapperPlaybackSegmentInfo_t *p_seg;

  memset(&ctx->playback.status, 0, sizeof(ctx->playback.status));
  ctx->playback.status.pids = ctx->playback.pids_req;

  ctx->playback.status.state = ctx->playback.seg_status.state;
  ctx->playback.status.speed = ctx->playback.seg_status.speed;
  ctx->playback.status.flags = ctx->playback.seg_status.flags;
  ctx->current_segment_id = ctx->playback.seg_status.segment_id;

  // This error is suppressed as the macro code is picked from kernel.
  // prefetch() here incurring self_assign is used to avoid some compiling
  // warnings.
  // coverity[self_assign]
  list_for_each_entry_reverse(p_seg, &ctx->segments, head) {
    if (p_seg->seg_info.id == ctx->playback.seg_status.segment_id) {
        break;
    }

    ctx->playback.status.info_cur.time += p_seg->seg_info.duration;
    ctx->playback.status.info_cur.size += p_seg->seg_info.size;
    ctx->playback.status.info_cur.pkts += p_seg->seg_info.nb_packets;
  }
  // This error is suppressed as the macro code is picked from kernel.
  // prefetch() here incurring self_assign is used to avoid some compiling
  // warnings.
  // coverity[self_assign]
  list_for_each_entry_reverse(p_seg, &ctx->segments, head) {
    ctx->playback.status.info_full.time += p_seg->seg_info.duration;
    ctx->playback.status.info_full.size += p_seg->seg_info.size;
    ctx->playback.status.info_full.pkts += p_seg->seg_info.nb_packets;
  }

  if (status) {
    *status = ctx->playback.status;
    /*deal with current, lack size and pkts with the current*/
    status->info_cur.time += ctx->playback.seg_status.time_cur;
    //get last segment id
    DVR_WrapperRecordSegmentInfo_t *p_seg;

    p_seg = list_last_entry(&ctx->segments, DVR_WrapperRecordSegmentInfo_t, head);
    if (ctx->playback.tf_full == DVR_TRUE && p_seg->info.id == ctx->current_segment_id) {
      status->disguised_info_obsolete.time = ctx->playback.obsolete.time + ctx->playback.seg_status.time_cur;
      status->info_obsolete.time = ctx->playback.obsolete.time;
      DVR_WRAPPER_INFO("warning change start time :id[%lld] [%d]cur[%d]\n", p_seg->info.id, status->info_obsolete.time, status->info_cur.time);
    }
    else
    {
      status->info_obsolete.time = ctx->playback.obsolete.time;
      status->disguised_info_obsolete.time = ctx->playback.obsolete.time;
    }
  }

  return DVR_SUCCESS;
}

static int process_handlePlaybackEvent(DVR_WrapperEventCtx_t *evt, DVR_WrapperCtx_t *ctx)
{
  DVR_WRAPPER_DEBUG("evt (sn:%ld) 0x%x (state:%d) cur(%lld:%u/%u)\n",
    evt->sn, evt->playback.event,
    evt->playback.status.play_status.state,
    evt->playback.status.play_status.segment_id,
    evt->playback.status.play_status.time_cur,
    evt->playback.status.play_status.time_end);

  /*evt PLAYTIME will break the last logic, do not save*/
  if (evt->playback.event != DVR_PLAYBACK_EVENT_NOTIFY_PLAYTIME
      && evt->playback.event != DVR_PLAYBACK_EVENT_NODATA
      && evt->playback.event != DVR_PLAYBACK_EVENT_DATARESUME
      )
    ctx->playback.last_event = evt->playback.event;

  switch (evt->playback.event)
  {
    case DVR_PLAYBACK_EVENT_FIRST_FRAME:
    case DVR_PLAYBACK_EVENT_REACHED_END:
    case DVR_PLAYBACK_EVENT_TRANSITION_OK:
    case DVR_PLAYBACK_EVENT_NOTIFY_PLAYTIME:
    case DVR_PLAYBACK_EVENT_ERROR:
    case DVR_PLAYBACK_EVENT_REACHED_BEGIN:
    case DVR_PLAYBACK_EVENT_NODATA:
    case DVR_PLAYBACK_EVENT_DATARESUME:
    case DVR_PLAYBACK_EVENT_TIMESHIFT_FR_REACHED_BEGIN:
    case DVR_PLAYBACK_EVENT_TIMESHIFT_FF_REACHED_END:
    {
      DVR_WrapperPlaybackStatus_t status;

      /*copy status of segment*/
      ctx->playback.seg_status = evt->playback.status.play_status;

      /*generate status of the whole playback*/
      process_generatePlaybackStatus(ctx, &status);

      if (evt->playback.event == DVR_PLAYBACK_EVENT_REACHED_END) {
        DVR_WRAPPER_INFO("playback(sn:%ld) event:0x%x\n", evt->sn, evt->playback.event);
        if (ctx->playback.param_open.is_timeshift
          || ctx_isPlay_recording(ctx->playback.param_open.location)) {
          /*wait for more data in recording*/
        }
        /*trust the low level, make NO check.
          As this evt is changed to only once due to some operations(paused) in low level.
        else if ((status.info_cur.time + DVR_PLAYBACK_END_GAP) >= ctx->playback.status.info_full.time) {
          process_notifyPlayback(ctx, evt->playback.event, &status);
        }
        */
        else {
          process_notifyPlayback(ctx, evt->playback.event, &status);
          ctx->playback.reach_end = DVR_TRUE;
        }
      } else {
        process_notifyPlayback(ctx, evt->playback.event, &status);
      }
    } break;
    case DVR_PLAYBACK_EVENT_TRANSITION_FAILED:
    case DVR_PLAYBACK_EVENT_KEY_FAILURE:
    case DVR_PLAYBACK_EVENT_NO_KEY:
    {
      DVR_WRAPPER_INFO("playback(sn:%ld) error event:0x%x\n", evt->sn, evt->playback.event);
    } break;
    default:
    {
      DVR_WRAPPER_INFO("playback(sn:%ld) unknown event:0x%x\n", evt->sn, evt->playback.event);
    } break;
  }
  return 0;
}

static inline int process_handleEvents(DVR_WrapperEventCtx_t *evt, DVR_WrapperCtx_t *ctx)
{
  return (evt->type == W_REC)? process_handleRecordEvent(evt, ctx) : process_handlePlaybackEvent(evt, ctx);
}

int dvr_wrapper_set_log_level (int level)
{
  DVR_WRAPPER_INFO("libdvr_api, set_log_level %d", level);
  if (level<LOG_LV_DEFAULT || level>LOG_LV_FATAL) {
    DVR_WRAPPER_ERROR("Invalid dvr log level:%d", g_dvr_log_level);
    return DVR_FAILURE;
  }
  g_dvr_log_level = level;
  DVR_WRAPPER_INFO("New dvr log level:%d", g_dvr_log_level);
  return DVR_SUCCESS;
}

int dvr_wrapper_set_ac4_preselection_id(DVR_WrapperPlayback_t playback, int presel_id)
{
  DVR_WrapperCtx_t *ctx;
  int error;

  DVR_RETURN_IF_FALSE(playback!=NULL);

  ctx = ctx_getPlayback((unsigned long)playback);
  DVR_RETURN_IF_FALSE(ctx);

  wrapper_mutex_lock(&ctx->wrapper_lock);

  WRAPPER_RETURN_IF_FALSE_WITH_UNLOCK(ctx_valid(ctx), &ctx->wrapper_lock);

  DVR_WRAPPER_INFO("libdvr_api, set_ac4_preselection_id %d", presel_id);
  error = dvr_playback_set_ac4_preselection_id(ctx->playback.player, presel_id);

  wrapper_mutex_unlock(&ctx->wrapper_lock);

  return error;
}

int dvr_wrapper_property_set(const char* prop_name, const char* prop_value)
{
  return dvr_prop_write(prop_name,prop_value);
}

int dvr_wrapper_property_get(const char* prop_name, char* prop_value, int length)
{
  return dvr_prop_read(prop_name,prop_value,length);
}

int dvr_wrapper_ioctl_record(DVR_WrapperRecord_t rec, unsigned int cmd, void *data, size_t size)
{
  DVR_WrapperCtx_t *ctx;
  int error;

  DVR_RETURN_IF_FALSE(rec);

  ctx = ctx_getRecord((unsigned long)rec);
  DVR_RETURN_IF_FALSE(ctx);

  wrapper_mutex_lock(&ctx->wrapper_lock);
  DVR_WRAPPER_INFO("libdvr_api, ioctl_record (sn:%ld) cmd:%#x data:%p", ctx->sn, cmd, data);
  WRAPPER_RETURN_IF_FALSE_WITH_UNLOCK(ctx_valid(ctx), &ctx->wrapper_lock);

  error = dvr_record_ioctl(ctx->record.recorder, cmd, data, size);

  DVR_WRAPPER_INFO("record(sn:%ld) ioctl_record = (%d)\n", ctx->sn, error);
  wrapper_mutex_unlock(&ctx->wrapper_lock);

  return error;

}
