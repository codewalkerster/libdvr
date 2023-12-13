#include <stdio.h>
#include <stdlib.h>

#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include "dvr_utils.h"
#include "dvr_types.h"
#include "dvr_playback.h"
#include "am_crypt.h"

#define PB_LOG_TAG "libdvr-playback"
#define DVR_PB_DEBUG(...) DVR_LOG_PRINT(LOG_LV_DEBUG, PB_LOG_TAG, __VA_ARGS__)
#define DVR_PB_INFO(...) DVR_LOG_PRINT(LOG_LV_INFO, PB_LOG_TAG, __VA_ARGS__)
#define DVR_PB_WARN(...) DVR_LOG_PRINT(LOG_LV_WARN, PB_LOG_TAG, __VA_ARGS__)
#define DVR_PB_ERROR(...) DVR_LOG_PRINT(LOG_LV_ERROR, PB_LOG_TAG, __VA_ARGS__)
#define DVR_PB_FATAL(...) DVR_LOG_PRINT(LOG_LV_FATAL, PB_LOG_TAG, __VA_ARGS__)

#define VALID_PID(_pid_) ((_pid_)>0 && (_pid_)<0x1fff)

#define FF_SPEED (2.0f)
#define FB_SPEED (-1.0f)
#define IS_FFFB(_SPEED_) ((_SPEED_) > FF_SPEED && (_SPEED_) < FB_SPEED)
#define IS_FB(_SPEED_) ((_SPEED_) <= FB_SPEED)

#define IS_KERNEL_SPEED(_SPEED_) (((_SPEED_) == PLAYBACK_SPEED_X2) || ((_SPEED_) == PLAYBACK_SPEED_X1) || ((_SPEED_) == PLAYBACK_SPEED_S2) || ((_SPEED_) == PLAYBACK_SPEED_S4) || ((_SPEED_) == PLAYBACK_SPEED_S8))
#define IS_FAST_SPEED(_SPEED_) (((_SPEED_) == PLAYBACK_SPEED_X2) || ((_SPEED_) == PLAYBACK_SPEED_S2) || ((_SPEED_) == PLAYBACK_SPEED_S4) || ((_SPEED_) == PLAYBACK_SPEED_S8))

#define DVR_PLAYER_CHANGE_STATE(player,newstate)\
  DVR_PB_INFO("%s:%d player %p changes state from %s to %s",__func__,__LINE__,\
    player,_dvr_playback_state_toString(player->state),_dvr_playback_state_toString(newstate));\
    player->state=newstate;

//#define FOR_SKYWORTH_FETCH_RDK

#define FFFB_SLEEP_TIME    (1000)//500ms
#define FB_DEFAULT_LEFT_TIME    (3000)
//if tsplayer delay time < 200 and no data can read, we will pause
#define MIN_TSPLAYER_DELAY_TIME (200)

#define MAX_CACHE_TIME    (30000)
//used pcr to control avsync,default not used
//#define AVSYNC_USED_PCR 1
static int write_success = 0;
//
static int _dvr_playback_fffb(DVR_PlaybackHandle_t handle);
static int _do_handle_pid_update(DVR_PlaybackHandle_t handle, DVR_PlaybackPids_t  now_pids, DVR_PlaybackPids_t pids, int type);
static int _dvr_get_cur_time(DVR_PlaybackHandle_t handle);
static int _dvr_get_end_time(DVR_PlaybackHandle_t handle);
static int _dvr_playback_calculate_seekpos(DVR_PlaybackHandle_t handle);
static int _dvr_playback_replay(DVR_PlaybackHandle_t handle, DVR_Bool_t trick) ;
static int _dvr_playback_get_status(DVR_PlaybackHandle_t handle,
  DVR_PlaybackStatus_t *p_status, DVR_Bool_t is_lock);
static int _dvr_playback_sent_transition_ok(DVR_PlaybackHandle_t handle, DVR_Bool_t is_lock);
static uint32_t dvr_playback_calculate_last_valid_segment(
  DVR_PlaybackHandle_t handle, uint64_t *segmentid, uint32_t *pos);
static int get_effective_tsplayer_delay_time(DVR_Playback_t* playback, int *time);
static int _dvr_get_play_cur_time(DVR_PlaybackHandle_t handle, uint64_t *id);



static char* _cmd_toString(int cmd)
{

  char *string[DVR_PLAYBACK_CMD_NONE+1]={
        "start",
        "stop",
        "v_start",
        "a_start",
        "v_stop",
        "a_stop",
        "v_restart",
        "a_restart",
        "av_restart",
        "v_stop_a_start",
        "a_stop_v_start",
        "v_stop_a_restart",
        "a_stop_v_restart",
        "v_start_a_restart",
        "a_start_v_restart",
        "pause",
        "resume",
        "seek",
        "ff",
        "fb",
        "NONE"
  };

  if (cmd > DVR_PLAYBACK_CMD_NONE) {
    return "unknown";
  } else {
    return string[cmd];
  }
}


static char* _dvr_playback_state_toString(int state)
{
  char *strings[5]={
        "START",
        "STOP",
        "PAUSE",
        "FF",
        "FB",
  };

  if (state >= 5 || state < 0) {
    return "UNKNOWN";
  }
  return strings[state];
}

static DVR_Bool_t _dvr_support_speed(int speed) {

  DVR_Bool_t ret = DVR_FALSE;

  switch (speed) {
    case PLAYBACK_SPEED_FBX1:
    case PLAYBACK_SPEED_FBX2:
    case PLAYBACK_SPEED_FBX4:
    case PLAYBACK_SPEED_FBX8:
    case PLAYBACK_SPEED_FBX16:
    case PLAYBACK_SPEED_FBX12:
    case PLAYBACK_SPEED_FBX32:
    case PLAYBACK_SPEED_FBX48:
    case PLAYBACK_SPEED_FBX64:
    case PLAYBACK_SPEED_FBX128:
    case PLAYBACK_SPEED_S2:
    case PLAYBACK_SPEED_S4:
    case PLAYBACK_SPEED_S8:
    case PLAYBACK_SPEED_X1:
    case PLAYBACK_SPEED_X2:
    case PLAYBACK_SPEED_X4:
    case PLAYBACK_SPEED_X3:
    case PLAYBACK_SPEED_X5:
    case PLAYBACK_SPEED_X6:
    case PLAYBACK_SPEED_X7:
    case PLAYBACK_SPEED_X8:
    case PLAYBACK_SPEED_X12:
    case PLAYBACK_SPEED_X16:
    case PLAYBACK_SPEED_X32:
    case PLAYBACK_SPEED_X48:
    case PLAYBACK_SPEED_X64:
    case PLAYBACK_SPEED_X128:
      ret = DVR_TRUE;
      break;
    default:
      DVR_PB_INFO("not support speed is set [%d]", speed);
      break;
  }
  return ret;
}

void _dvr_tsplayer_callback_test(void *user_data, am_tsplayer_event *event)
{
  DVR_PB_INFO("in callback test ");
  DVR_Playback_t *player = NULL;
  if (user_data != NULL) {
    player = (DVR_Playback_t *) user_data;
    DVR_PB_INFO("play speed [%f] in callback test ", player->speed);
  }
  switch (event->type) {
      case AM_TSPLAYER_EVENT_TYPE_VIDEO_CHANGED:
      {
          DVR_PB_INFO("[evt] test AM_TSPLAYER_EVENT_TYPE_VIDEO_CHANGED: %d x %d @%d\n",
              event->event.video_format.frame_width,
              event->event.video_format.frame_height,
              event->event.video_format.frame_rate);
          break;
      }
      case AM_TSPLAYER_EVENT_TYPE_FIRST_FRAME:
      {
          if (player == NULL) {
            DVR_PB_WARN("player is null at line %d",__LINE__);
            break;
          }
          DVR_PB_INFO("[evt] test  AM_TSPLAYER_EVENT_TYPE_FIRST_FRAME\n");
          player->first_frame = 1;
          break;
      }
      default:
          break;
    }
}

void _dvr_tsplayer_callback(void *user_data, am_tsplayer_event *event)
{
  DVR_Playback_t *play = (DVR_Playback_t*)user_data;
  if (play == NULL) {
    DVR_PB_WARN("play is invalid in %s",__func__);
    return;
  }
  switch (event->type) {
    case AM_TSPLAYER_EVENT_TYPE_FIRST_FRAME:
      DVR_PB_INFO("Received AM_TSPLAYER_EVENT_TYPE_FIRST_FRAME");
      if (play->first_trans_ok == DVR_FALSE) {
        play->first_trans_ok = DVR_TRUE;
        _dvr_playback_sent_transition_ok((DVR_PlaybackHandle_t)play, DVR_FALSE);
      }
      play->first_frame = 1;
      play->seek_pause = DVR_FALSE;
      break;
    case AM_TSPLAYER_EVENT_TYPE_DECODE_FIRST_FRAME_AUDIO:
      DVR_PB_INFO("Received AM_TSPLAYER_EVENT_TYPE_DECODE_FIRST_FRAME_AUDIO");
      if (play->first_trans_ok == DVR_FALSE && play->has_video == DVR_FALSE) {
        play->first_trans_ok = DVR_TRUE;
        _dvr_playback_sent_transition_ok((DVR_PlaybackHandle_t)play, DVR_FALSE);
      }
      if (play->has_video == DVR_FALSE) {
        play->first_frame = 1;
        play->seek_pause = DVR_FALSE;
      }
      break;
    default:
      break;
  }
  if (play->player_callback_func == NULL) {
    DVR_PB_WARN("play callback function %p is invalid",play->player_callback_func);
    return;
  }
  play->player_callback_func(play->player_callback_userdata, event);
}

//convert video and audio fmt
static int _dvr_convert_stream_fmt(int fmt, DVR_Bool_t is_audio) {
  int format = 0;
  if (is_audio == DVR_FALSE) {
    //for video fmt
    switch (fmt)
    {
        case DVR_VIDEO_FORMAT_MPEG1:
          format = AV_VIDEO_CODEC_MPEG1;
          break;
        case DVR_VIDEO_FORMAT_MPEG2:
          format = AV_VIDEO_CODEC_MPEG2;
          break;
        case DVR_VIDEO_FORMAT_HEVC:
          format = AV_VIDEO_CODEC_H265;
          break;
        case DVR_VIDEO_FORMAT_H264:
          format = AV_VIDEO_CODEC_H264;
          break;
        case DVR_VIDEO_FORMAT_VP9:
          format = AV_VIDEO_CODEC_VP9;
          break;
    }
  } else {
    //for audio fmt
    switch (fmt)
    {
        case DVR_AUDIO_FORMAT_MPEG:
          format = AV_AUDIO_CODEC_MP2;
          break;
        case DVR_AUDIO_FORMAT_AC3:
          format = AV_AUDIO_CODEC_AC3;
          break;
        case DVR_AUDIO_FORMAT_EAC3:
          format = AV_AUDIO_CODEC_EAC3;
          break;
        case DVR_AUDIO_FORMAT_DTS:
          format = AV_AUDIO_CODEC_DTS;
          break;
        case DVR_AUDIO_FORMAT_AAC:
          format = AV_AUDIO_CODEC_AAC;
          break;
        case DVR_AUDIO_FORMAT_LATM:
          format = AV_AUDIO_CODEC_LATM;
          break;
        case DVR_AUDIO_FORMAT_PCM:
          format = AV_AUDIO_CODEC_PCM;
          break;
        case DVR_AUDIO_FORMAT_AC4:
          format = AV_AUDIO_CODEC_AC4;
          break;
    }
  }
  return format;
}
static int _dvr_playback_get_trick_stat(DVR_PlaybackHandle_t handle)
{
  DVR_Playback_t *player = (DVR_Playback_t *) handle;

  if (player == NULL || player->handle == (am_tsplayer_handle)NULL)
    return -1;

  return player->first_frame;
}


//get sys time sec
static uint32_t _dvr_getClock_sec(void)
{
  struct timespec ts;
  uint32_t s;
  clock_gettime(CLOCK_REALTIME, &ts);
  s = (uint32_t)(ts.tv_sec);
  DVR_PB_INFO("n:%u", s);
  return s;
}

//get sys time ms
static uint32_t _dvr_time_getClock(void)
{
  struct timespec ts;
  uint32_t ms;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  ms = (uint32_t)(ts.tv_sec*1000+ts.tv_nsec/1000000);
  return ms;
}

//timeout wait signal
static int _dvr_playback_timeoutwait(DVR_PlaybackHandle_t handle , int ms)
{
  DVR_Playback_t *player = (DVR_Playback_t *) handle;


  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }

  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  //ms为毫秒，换算成秒
  ts.tv_sec += ms/1000;
  //在outtime的基础上，增加ms毫秒
  //outtime.tv_nsec为纳秒，1微秒=1000纳秒
  //tv_nsec此值再加上剩余的毫秒数 ms%1000，有可能超过1秒。需要特殊处理
  uint64_t  us = ts.tv_nsec/1000 + 1000 * (ms % 1000); //微秒
  //us的值有可能超过1秒，
  ts.tv_sec += us / 1000000;
  us = us % 1000000;
  ts.tv_nsec = us * 1000;//换算成纳秒

  int val = dvr_mutex_save(&player->lock);
  pthread_cond_timedwait(&player->cond, &player->lock.lock, &ts);
  dvr_mutex_restore(&player->lock, val);
  return 0;
}

//send signal
static int _dvr_playback_sendSignal(DVR_PlaybackHandle_t handle)
{
  DVR_Playback_t *player = (DVR_Playback_t *) handle;\

  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }
  DVR_PB_DEBUG("lock---");
  dvr_mutex_lock(&player->lock);
  pthread_cond_signal(&player->cond);
  DVR_PB_DEBUG("unlock---");
  dvr_mutex_unlock(&player->lock);
  return 0;
}

//send playback event, need check is need lock first
static int _dvr_playback_sent_event(DVR_PlaybackHandle_t handle, DVR_PlaybackEvent_t evt, DVR_Play_Notify_t *notify, DVR_Bool_t is_lock) {

  DVR_Playback_t *player = (DVR_Playback_t *) handle;

  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }

  switch (evt) {
    case DVR_PLAYBACK_EVENT_ERROR:
      _dvr_playback_get_status(handle, &(notify->play_status), is_lock);
      break;
    case DVR_PLAYBACK_EVENT_TRANSITION_OK:
      //GET STATE
      DVR_PB_INFO("trans ok EVENT");
      _dvr_playback_get_status(handle, &(notify->play_status), is_lock);
      break;
    case DVR_PLAYBACK_EVENT_TRANSITION_FAILED:
      break;
    case DVR_PLAYBACK_EVENT_KEY_FAILURE:
      break;
    case DVR_PLAYBACK_EVENT_NO_KEY:
      break;
    case DVR_PLAYBACK_EVENT_REACHED_BEGIN:
      //GET STATE
      DVR_PB_INFO("reached begin EVENT");
      _dvr_playback_get_status(handle, &(notify->play_status), is_lock);
      break;
    case DVR_PLAYBACK_EVENT_REACHED_END:
      //GET STATE
      DVR_PB_INFO("reached end EVENT");
      _dvr_playback_get_status(handle, &(notify->play_status), is_lock);
      break;
    case DVR_PLAYBACK_EVENT_NOTIFY_PLAYTIME:
      _dvr_playback_get_status(handle, &(notify->play_status), is_lock);
      break;
    default:
      break;
  }
  if (player->openParams.event_fn != NULL)
      player->openParams.event_fn(evt, (void*)notify, player->openParams.event_userdata);
  return DVR_SUCCESS;
}
static int _dvr_playback_sent_transition_ok(DVR_PlaybackHandle_t handle, DVR_Bool_t is_lock)
{
  DVR_Play_Notify_t notify;
  memset(&notify, 0 , sizeof(DVR_Play_Notify_t));
  notify.event = DVR_PLAYBACK_EVENT_TRANSITION_OK;
  //get play statue not here
  _dvr_playback_sent_event(handle, DVR_PLAYBACK_EVENT_TRANSITION_OK, &notify, is_lock);
  return DVR_SUCCESS;
}

static int _dvr_playback_sent_playtime(DVR_PlaybackHandle_t handle, DVR_Bool_t is_lock)
{
  DVR_Playback_t *player = (DVR_Playback_t *) handle;

  if (player == NULL) {
    DVR_PB_ERROR("player is NULL");
    return DVR_FAILURE;
  }
  if (player->openParams.is_notify_time == DVR_FALSE) {
    if (player->control_speed_enable == 0)
       return DVR_SUCCESS;
  }

  if (player->send_time == 0) {
    if (player->control_speed_enable == 0)
      player->send_time = _dvr_time_getClock() + 500;
    else
      player->send_time = _dvr_time_getClock() + 20;
  } else if (player->send_time >= _dvr_time_getClock()) {
    if ((player->send_time - _dvr_time_getClock()) > 1000) {
      player->send_time = _dvr_time_getClock() + 500;
      DVR_PB_INFO("player send time occur system time changed!!!!!");
    } else {
      return DVR_SUCCESS;
    }
  }
  if (player->control_speed_enable == 0)
    player->send_time = _dvr_time_getClock() + 500;
  else
    player->send_time = _dvr_time_getClock() + 20;

  DVR_Play_Notify_t notify;
  memset(&notify, 0 , sizeof(DVR_Play_Notify_t));
  notify.event = DVR_PLAYBACK_EVENT_NOTIFY_PLAYTIME;
  //get play statue not here
  _dvr_playback_sent_event(handle, DVR_PLAYBACK_EVENT_NOTIFY_PLAYTIME, &notify, is_lock);
  return DVR_SUCCESS;
}

static int _dvr_init_fffb_t(DVR_PlaybackHandle_t handle) {
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  player->fffb_start = _dvr_time_getClock();
  DVR_PB_INFO(" player->fffb_start:%u", player->fffb_start);
  player->fffb_current = player->fffb_start;
  //get segment current time pos
  player->fffb_start_pcr = _dvr_get_cur_time(handle);
  player->next_fffb_time = _dvr_time_getClock();

  return DVR_SUCCESS;
}

static int _dvr_init_fffb_time(DVR_PlaybackHandle_t handle) {
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  player->fffb_start = _dvr_time_getClock();
  DVR_PB_INFO(" player->fffb_start:%u", player->fffb_start);
  player->fffb_current = player->fffb_start;
  //get segment current time pos
  player->fffb_start_pcr = _dvr_get_cur_time(handle);

  player->next_fffb_time = _dvr_time_getClock();
  player->last_send_time_id = UINT64_MAX;
  return DVR_SUCCESS;
}
//get next segment id
static int _dvr_has_next_segmentId(DVR_PlaybackHandle_t handle, int segmentid) {

  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  DVR_PlaybackSegmentInfo_t *segment;
  DVR_PlaybackSegmentInfo_t *pre_segment = NULL;

  if (player == NULL) {
    DVR_PB_INFO(" player is NULL");
    return DVR_FAILURE;
  }

  int found = 0;
  int found_eq_id = 0;
  // This error is suppressed as the macro code is picked from kernel.
  // prefetch() here incurring self_assign is used to avoid some compiling
  // warnings.
  // coverity[self_assign]
  list_for_each_entry(segment, &player->segment_list, head)
  {
    if (player->segment_is_open == DVR_FALSE) {
      //get first segment from list, case segment is not open
      if (!IS_FB(player->speed))
        found = 1;
    } else if (segment->segment_id == segmentid) {
      //find cur segment, we need get next one
      found_eq_id = 1;
      if (!IS_FB(player->speed)) {
        found = 1;
        continue;
      } else {
        //if is fb mode.we need used pre segment
         if (pre_segment != NULL) {
           found = 1;
         } else {
          //not find next id.
          DVR_PB_INFO("not has find next segment on fb mode");
          return DVR_FAILURE;
         }
      }
    }
    if (found == 1) {
      found = 2;
      break;
    }
    pre_segment = segment;
  }
  if (found != 2) {
    //list is null or reache list  end
    DVR_PB_INFO("not found next segment return failure");
    return DVR_FAILURE;
  }
  DVR_PB_INFO("found next segment return success");
  return DVR_SUCCESS;
}

//get next segment id
static int _dvr_get_next_segmentId(DVR_PlaybackHandle_t handle) {

  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  DVR_PlaybackSegmentInfo_t *segment;
  DVR_PlaybackSegmentInfo_t *pre_segment = NULL;
  uint64_t segmentid;
  uint32_t pos;
  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }

  if (IS_FB(player->speed)
      && dvr_playback_check_limit(handle)) {
    dvr_playback_calculate_last_valid_segment(handle, &segmentid, &pos);
    //case cur id < segment id
    if (player->cur_segment_id <= segmentid) {
      //expired ts data is player,return error
      DVR_PB_INFO("reach start segment ,return error");
      return DVR_FAILURE;
    }
    DVR_PB_INFO("has segment to fb play [%lld][%u]", segmentid, pos);
  }

  int found = 0;
  int found_eq_id = 0;

  // This error is suppressed as the macro code is picked from kernel.
  // prefetch() here incurring self_assign is used to avoid some compiling
  // warnings.
  // coverity[self_assign]
  list_for_each_entry(segment, &player->segment_list, head)
  {
    if (player->segment_is_open == DVR_FALSE) {
      //get first segment from list, case segment is not open
      if (!IS_FB(player->speed))
        found = 1;
    } else if (segment->segment_id == player->cur_segment_id) {
      //find cur segment, we need get next one
      found_eq_id = 1;
      if (!IS_FB(player->speed)) {
        found = 1;
        continue;
      } else {
        //if is fb mode.we need used pre segment
         if (pre_segment != NULL) {
           found = 1;
         } else {
          //not find next id.
          DVR_PB_INFO("not find next segment on fb mode");
          return DVR_FAILURE;
         }
      }
    }
    if (found == 1) {
      if (IS_FB(player->speed)) {
        //used pre segment
        segment = pre_segment;
      }
      //save segment info
      player->last_segment_id = player->cur_segment_id;
      if (player->segment_handle) {
        player->last_segment_total = segment_tell_total_time(player->segment_handle);
      }
      player->last_segment.segment_id = player->cur_segment.segment_id;
      player->last_segment.flags = player->cur_segment.flags;
      memcpy(player->last_segment.location, player->cur_segment.location, DVR_MAX_LOCATION_SIZE);
      //pids
      memcpy(&player->last_segment.pids, &player->cur_segment.pids, sizeof(DVR_PlaybackPids_t));

      //get segment info
      player->segment_is_open = DVR_TRUE;
      player->cur_segment_id = segment->segment_id;
      player->cur_segment.segment_id = segment->segment_id;
      player->cur_segment.flags = segment->flags;
      DVR_PB_INFO("set cur id cur flag[0x%x]segment->flags flag[0x%x] id [%lld]", player->cur_segment.flags, segment->flags, segment->segment_id);
      memcpy(player->cur_segment.location, segment->location, DVR_MAX_LOCATION_SIZE);
      //pids
      memcpy(&player->cur_segment.pids, &segment->pids, sizeof(DVR_PlaybackPids_t));
      found = 2;
      break;
    }
    pre_segment = segment;
  }
  if (player->segment_is_open == DVR_FALSE && IS_FB(player->speed)) {
    //used the last one segment to open
    //get segment info
    player->segment_is_open = DVR_TRUE;
    player->cur_segment_id = pre_segment->segment_id;
    player->cur_segment.segment_id = pre_segment->segment_id;
    player->cur_segment.flags = pre_segment->flags;
    DVR_PB_INFO("set cur id fb last one cur flag[0x%x]segment->flags flag[0x%x] id [%lld]", player->cur_segment.flags, pre_segment->flags, pre_segment->segment_id);
    memcpy(player->cur_segment.location, pre_segment->location, DVR_MAX_LOCATION_SIZE);
    //pids
    memcpy(&player->cur_segment.pids, &pre_segment->pids, sizeof(DVR_PlaybackPids_t));
    return DVR_SUCCESS;
  }
  if (found != 2) {
    //list is null or reache list  end
    return DVR_FAILURE;
  }
  return DVR_SUCCESS;
}
//open next segment to play,if reach list end return errro.
static int _change_to_next_segment(DVR_PlaybackHandle_t handle)
{
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  Segment_OpenParams_t  params;
  int ret = DVR_SUCCESS;

  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }
  pthread_mutex_lock(&player->segment_lock);
retry:
  ret = _dvr_get_next_segmentId(handle);
  if (ret == DVR_FAILURE) {
    DVR_PB_INFO("not found segment info");
    pthread_mutex_unlock(&player->segment_lock);
    return DVR_FAILURE;
  }

  if (player->segment_handle != NULL) {
    DVR_PB_INFO("close segment");
    segment_close(player->segment_handle);
    player->segment_handle = NULL;
  }

  memset((void*)&params,0,sizeof(params));
  //cp current segment path to location
  memcpy(params.location, player->cur_segment.location, DVR_MAX_LOCATION_SIZE);
  params.segment_id = (uint64_t)player->cur_segment.segment_id;
  params.mode = SEGMENT_MODE_READ;
  DVR_PB_INFO("open segment location[%s]id[%lld]flag[0x%x]", params.location, params.segment_id, player->cur_segment.flags);

  ret = segment_open(&params, &(player->segment_handle));
  if (ret == DVR_FAILURE) {
    DVR_PB_INFO("open segment error");
    goto retry;
  }
  // Keep the start segment_id when the first segment_open is called during a playback
  if (player->first_start_id == UINT64_MAX) {
    player->first_start_id = player->cur_segment.segment_id;
  }
  pthread_mutex_unlock(&player->segment_lock);
  int total = _dvr_get_end_time( handle);
  pthread_mutex_lock(&player->segment_lock);
  if (IS_FB(player->speed)) {
      //seek end pos -FB_DEFAULT_LEFT_TIME
      player->ts_cache_len = 0;
      segment_seek(player->segment_handle, total - FB_DEFAULT_LEFT_TIME, player->openParams.block_size);
      DVR_PB_INFO("seek pos [%d]", total - FB_DEFAULT_LEFT_TIME);
  }
  player->dur = total;
  player->con_spe.ply_dur = 0;
  player->con_spe.ply_sta = 0;
  player->con_spe.sys_dur = 0;
  player->con_spe.sys_sta = 0;
  pthread_mutex_unlock(&player->segment_lock);
  DVR_PB_INFO("next segment dur [%d] flag [0x%x]", player->dur, player->cur_segment.flags);
  return ret;
}

//open next segment to play,if reach list end return errro.
static int _dvr_open_segment(DVR_PlaybackHandle_t handle, uint64_t segment_id)
{
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  Segment_OpenParams_t  params;
  int ret = DVR_SUCCESS;
  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }
  if (segment_id == player->cur_segment_id && player->segment_is_open == DVR_TRUE) {
    return DVR_SUCCESS;
  }
  uint64_t id = segment_id;
  DVR_PB_INFO("start finding segment[%lld] info", id);
  pthread_mutex_lock(&player->segment_lock);

  DVR_PlaybackSegmentInfo_t *segment;

  int found = 0;

  // This error is suppressed as the macro code is picked from kernel.
  // prefetch() here incurring self_assign is used to avoid some compiling
  // warnings.
  // coverity[self_assign]
  list_for_each_entry(segment, &player->segment_list, head)
  {
    DVR_PB_INFO("see 1 location [%s]id[%lld]flag[%x]segment_id[%lld]", segment->location, segment->segment_id, segment->flags, segment_id);
    if (segment->segment_id == segment_id) {
      found = 1;
    }
    if (found == 1) {
      DVR_PB_INFO("found  [%s]id[%lld]flag[%x]segment_id[%lld]", segment->location, segment->segment_id, segment->flags, segment_id);
      //get segment info
      player->segment_is_open = DVR_TRUE;
      player->cur_segment_id = segment->segment_id;
      player->cur_segment.segment_id = segment->segment_id;
      player->cur_segment.flags = segment->flags;
      const int len = strlen(segment->location);
      if (len >= DVR_MAX_LOCATION_SIZE || len <= 0) {
        DVR_PB_ERROR("Invalid segment.location length %d",len);
        pthread_mutex_unlock(&player->segment_lock);
        return DVR_FAILURE;
      }
      strncpy(player->cur_segment.location, segment->location, len+1);
      //pids
      memcpy(&player->cur_segment.pids, &segment->pids, sizeof(DVR_PlaybackPids_t));
      DVR_PB_INFO("cur found location [%s]id[%lld]flag[%x]", player->cur_segment.location, player->cur_segment.segment_id,player->cur_segment.flags);
      break;
    }
  }
  if (found == 0) {
    DVR_PB_INFO("not found segment info.error..");
    pthread_mutex_unlock(&player->segment_lock);
    return DVR_FAILURE;
  }
  memset((void*)&params,0,sizeof(params));

  const int len2 = strlen(player->cur_segment.location);
  if (len2 >= DVR_MAX_LOCATION_SIZE || len2 <= 0) {
    DVR_PB_ERROR("Invalid cur_segment.location length %d",len2);
    pthread_mutex_unlock(&player->segment_lock);
    return DVR_FAILURE;
  }
  strncpy(params.location, player->cur_segment.location, len2+1);
  params.segment_id = (uint64_t)player->cur_segment.segment_id;
  params.mode = SEGMENT_MODE_READ;
  DVR_PB_INFO("open segment location[%s][%lld]cur flag[0x%x]", params.location, params.segment_id, player->cur_segment.flags);
  if (player->segment_handle != NULL) {
    segment_close(player->segment_handle);
    player->segment_handle = NULL;
  }
  ret = segment_open(&params, &(player->segment_handle));
  if (ret == DVR_FAILURE) {
    DVR_PB_INFO("segment open error");
  }
  // Keep the start segment_id when the first segment_open is called during a playback
  if (player->first_start_id == UINT64_MAX) {
    player->first_start_id = player->cur_segment.segment_id;
  }
  pthread_mutex_unlock(&player->segment_lock);
  player->dur = _dvr_get_end_time(handle);

  DVR_PB_INFO("player->dur [%d]cur id [%lld]cur flag [0x%x]\r\n", player->dur,player->cur_segment.segment_id, player->cur_segment.flags);
  return ret;
}


//get play info by segment id
static int _dvr_playback_get_playinfo(DVR_PlaybackHandle_t handle,
  uint64_t segment_id,
  am_tsplayer_video_params *video_param,
  am_tsplayer_audio_params *audio_param, am_tsplayer_audio_params *ad_param) {

  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  DVR_PlaybackSegmentInfo_t *segment;
  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }

  int found = 0;

  // This error is suppressed as the macro code is picked from kernel.
  // prefetch() here incurring self_assign is used to avoid some compiling
  // warnings.
  // coverity[self_assign]
  list_for_each_entry(segment, &player->segment_list, head)
  {
    if (segment_id == UINT64_MAX) {
      //get first segment from list
      found = 1;
    }
    if (segment->segment_id == segment_id) {
      found = 1;
    }
    if (found == 1) {
      //get segment info
      if (player->cur_segment_id != UINT64_MAX)
        player->cur_segment_id = segment->segment_id;
      player->cur_segment.segment_id = segment->segment_id;
      player->cur_segment.flags = segment->flags;
      //pids
      player->cur_segment.pids.video.pid = segment->pids.video.pid;
      player->cur_segment.pids.video.format = segment->pids.video.format;
      player->cur_segment.pids.video.type = segment->pids.video.type;
      player->cur_segment.pids.audio.pid = segment->pids.audio.pid;
      player->cur_segment.pids.audio.format = segment->pids.audio.format;
      player->cur_segment.pids.audio.type = segment->pids.audio.type;
      player->cur_segment.pids.ad.pid = segment->pids.ad.pid;
      player->cur_segment.pids.ad.format = segment->pids.ad.format;
      player->cur_segment.pids.ad.type = segment->pids.ad.type;
      player->cur_segment.pids.pcr.pid = segment->pids.pcr.pid;
      //
      video_param->codectype = _dvr_convert_stream_fmt(segment->pids.video.format, DVR_FALSE);
      video_param->pid = segment->pids.video.pid;
      audio_param->codectype = _dvr_convert_stream_fmt(segment->pids.audio.format, DVR_TRUE);
      audio_param->pid = segment->pids.audio.pid;
      ad_param->codectype =_dvr_convert_stream_fmt(segment->pids.ad.format, DVR_TRUE);
      ad_param->pid =segment->pids.ad.pid;
      DVR_PB_DEBUG("get_playinfo, segment_id:%lld, vpid[0x%x], apid[0x%x], vfmt[%d], afmt[%d]",
          player->cur_segment_id, video_param->pid, audio_param->pid,
          video_param->codectype, audio_param->codectype);
      found = 2;
      break;
    }
  }
  if (found != 2) {
    //list is null or reache list  end
    DVR_PB_INFO("get play info fail");
    return DVR_FAILURE;
  }

  return DVR_SUCCESS;
}
static int _dvr_replay_changed_pid(DVR_PlaybackHandle_t handle) {
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }

  //compare cur segment
  //if (player->cmd.state == DVR_PLAYBACK_STATE_START)
  {
    //check video pids, stop or restart
    _do_handle_pid_update(handle, player->last_segment.pids, player->cur_segment.pids, 0);
    //check sub audio pids stop or restart
    _do_handle_pid_update(handle, player->last_segment.pids, player->cur_segment.pids, 2);
    //check audio pids stop or restart
    _do_handle_pid_update(handle, player->last_segment.pids, player->cur_segment.pids, 1);
    DVR_PB_INFO(":last apid: %d  set apid: %d", player->last_segment.pids.audio.pid,player->cur_segment.pids.audio.pid);
    //check pcr pids stop or restart
    _do_handle_pid_update(handle, player->last_segment.pids, player->cur_segment.pids, 3);
  }
  return DVR_SUCCESS;
}

static int _dvr_check_speed_con(DVR_PlaybackHandle_t handle)
{
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_TRUE;
  }

  DVR_PB_INFO(":play speed: %f  ply dur: %d sys_dur: %u",
                player->speed,
                player->con_spe.ply_dur,
                player->con_spe.sys_dur);

  if (player->speed != 1.0f)
   return DVR_TRUE;

  if (player->con_spe.ply_dur > 0
      && 2 * player->con_spe.ply_dur > 3 * player->con_spe.sys_dur)
    return DVR_FALSE;

  return DVR_TRUE;
}

static int _dvr_check_cur_segment_flag(DVR_PlaybackHandle_t handle)
{
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }
  if (player->vendor != DVR_PLAYBACK_VENDOR_DEF) {
    DVR_PB_INFO("In case of vendor Amlogic/Amazon, do not control AV display");
    return DVR_SUCCESS;
  }
  DVR_PB_INFO("flag[0x%x]id[%lld]last[0x%x][%llu]",
                player->cur_segment.flags,
                player->cur_segment.segment_id,
                player->last_segment.flags,
                player->last_segment.segment_id);
  if ((player->cur_segment.flags & DVR_PLAYBACK_SEGMENT_DISPLAYABLE) == DVR_PLAYBACK_SEGMENT_DISPLAYABLE &&
    (player->last_segment.flags & DVR_PLAYBACK_SEGMENT_DISPLAYABLE) == 0) {
    //enable display
    DVR_PB_INFO("unmute");
    AmTsPlayer_showVideo(player->handle);
    AmTsPlayer_setAudioMute(player->handle, 0, 0);
  } else if ((player->cur_segment.flags & DVR_PLAYBACK_SEGMENT_DISPLAYABLE) == 0 &&
    (player->last_segment.flags & DVR_PLAYBACK_SEGMENT_DISPLAYABLE) == DVR_PLAYBACK_SEGMENT_DISPLAYABLE) {
    //disable display
    DVR_PB_INFO("mute");
    AmTsPlayer_hideVideo(player->handle);
    AmTsPlayer_setAudioMute(player->handle, 1, 1);
  }
  return DVR_SUCCESS;
}
/*
if decode success first time.
success:  return true
fail:    return false
*/
static DVR_Bool_t _dvr_pauselive_decode_success(DVR_PlaybackHandle_t handle) {
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_TRUE;
  }
  if (player->first_frame == 1) {
    return DVR_TRUE;
  } else {
    return DVR_FALSE;
  }
}
static void* _dvr_playback_thread(void *arg)
{
  DVR_Playback_t *player = (DVR_Playback_t *) arg;
  //int need_open_segment = 1;
  am_tsplayer_input_buffer     input_buffer;
  am_tsplayer_input_buffer     dec_bufs;
  int ret = DVR_SUCCESS;

  #define MAX_REACHEND_TIMEOUT (3000)
  int reach_end_timeout = 0;//ms
  int cache_time = 0;
  int check_no_data_time = 4;
  int buf_len = player->openParams.block_size > 0 ? player->openParams.block_size : (256 * 1024);
  DVR_Bool_t b_writed_whole_block = player->openParams.block_size > 0 ? DVR_TRUE:DVR_FALSE;
  int first_write = 0;
  int dec_buf_size = buf_len + 188;
  int real_read = 0;
  DVR_Bool_t goto_rewrite = DVR_FALSE;
  int read = 0;

  prctl(PR_SET_NAME,"DvrPlayback");

  const uint64_t write_timeout_ms = (uint64_t)dvr_prop_read_int("vendor.tv.libdvr.writetm",50);
  const int timeout = dvr_prop_read_int("vendor.tv.libdvr.waittm",200);

  if (player->is_secure_mode) {
    if (dec_buf_size > player->secure_buffer_size) {
      DVR_PB_INFO("playback blocksize too large");
      return NULL;
    }
  }

  uint8_t *buf = malloc(buf_len);
  if (!buf) {
    DVR_PB_INFO("Malloc buffer failed");
    return NULL;
  }
  input_buffer.buf_type = TS_INPUT_BUFFER_TYPE_NORMAL;
  input_buffer.buf_size = 0;

  dec_bufs.buf_data = malloc(dec_buf_size);
  if (!dec_bufs.buf_data) {
    DVR_PB_INFO("Malloc dec buffer failed");
    free(buf);
    return NULL;
  }
  dec_bufs.buf_type = TS_INPUT_BUFFER_TYPE_NORMAL;
  dec_bufs.buf_size = dec_buf_size;

  if (player->segment_is_open == DVR_FALSE) {
    ret = _change_to_next_segment((DVR_PlaybackHandle_t)player);
  }

  if (ret != DVR_SUCCESS) {
    if (buf != NULL) {
      free(buf);
    }
    free(dec_bufs.buf_data);
    DVR_PB_INFO("get segment error");
    return NULL;
  }
  DVR_PB_INFO("--player->vendor %d,player->has_video[%d] bufsize[0x%x]whole block[%d]",
                player->vendor, player->has_video, buf_len, b_writed_whole_block);
  //get play statue not here,send ok event when vendor is aml or only audio channel if not send ok event
  if (((player->first_trans_ok == DVR_FALSE) && (player->vendor == DVR_PLAYBACK_VENDOR_AML) ) ||
    (player->first_trans_ok == DVR_FALSE && player->has_video == DVR_FALSE)) {
    player->first_trans_ok = DVR_TRUE;
    _dvr_playback_sent_transition_ok((DVR_PlaybackHandle_t)player, DVR_TRUE);
  }
  _dvr_check_cur_segment_flag((DVR_PlaybackHandle_t)player);
  //set video show
  AmTsPlayer_showVideo(player->handle);
  if (player->vendor == DVR_PLAYBACK_VENDOR_AMAZON)
    check_no_data_time = 8;
  int trick_stat = 0;
  while (player->is_running/* || player->cmd.last_cmd != player->cmd.cur_cmd*/) {

    //check trick stat
    //DVR_PB_INFO("lock check_no_data_time:%d", check_no_data_time);
    dvr_mutex_lock(&player->lock);

    {
      static struct timespec _prev_ts={0,0};
      struct timespec _nowts,_diffts;
      clock_gettime(CLOCK_MONOTONIC, &_nowts);
      clock_timespec_subtract(&_nowts,&_prev_ts,&_diffts);
      if (_diffts.tv_sec>0) {
        char _logbuf[512]={0};
        char* _pbuf=_logbuf;
        int _nchar=0;
        DVR_PlaybackSegmentInfo_t* _segment;
        // This error is suppressed as the macro code is picked from kernel.
        // prefetch() here incurring self_assign is used to avoid some compiling
        // warnings.
        // coverity[self_assign]
        list_for_each_entry(_segment, &player->segment_list, head) {
          if (player->cur_segment_id == _segment->segment_id) {
            int seg_size = segment_get_cur_segment_size(player->segment_handle);
            int read_ptr = segment_tell_position(player->segment_handle);
            float progress = -1.0f;
            if (seg_size>0) {
                progress = (float)read_ptr*100/seg_size;
            }
            _nchar=sprintf(_pbuf,"%lld(%.1f%%), ",_segment->segment_id,progress);
          } else {
            _nchar=sprintf(_pbuf,"%lld, ",_segment->segment_id);
          }
          if (_nchar<0) {
            break;
          }
          _pbuf+=_nchar;
          if (_pbuf-_logbuf+10 >= sizeof(_logbuf)) {
            sprintf(_pbuf,"...");
            break;
          }
        }
        DVR_PB_INFO("clk: %08u, seg_list: %s",_nowts.tv_sec,_logbuf);
        _prev_ts=_nowts;
      }
    }

    if (player->cmd.cur_cmd == DVR_PLAYBACK_CMD_SEEK ||
      player->cmd.cur_cmd == DVR_PLAYBACK_CMD_FF ||
      player->cmd.cur_cmd == DVR_PLAYBACK_CMD_FB ||
      player->speed > FF_SPEED ||player->speed <= FB_SPEED ||
      (player->state == DVR_PLAYBACK_STATE_PAUSE) ||
      (player->play_flag&DVR_PLAYBACK_STARTED_PAUSEDLIVE) == DVR_PLAYBACK_STARTED_PAUSEDLIVE)
    {
      trick_stat = _dvr_playback_get_trick_stat((DVR_PlaybackHandle_t)player);
      if (trick_stat > 0 || (trick_stat <= 0 && _dvr_time_getClock() > player->next_fffb_time)) {
        DVR_PB_INFO("trick stat[%d], cur cmd[%d]last cmd[%d]flag[0x%x], now[%u]>next_fffb_time[%u]",
                     trick_stat, player->cmd.cur_cmd, player->cmd.last_cmd, player->play_flag, _dvr_time_getClock(), player->next_fffb_time);
        if (player->cmd.cur_cmd == DVR_PLAYBACK_CMD_SEEK || (player->play_flag&DVR_PLAYBACK_STARTED_PAUSEDLIVE) == DVR_PLAYBACK_STARTED_PAUSEDLIVE) {
          //check last cmd
          if (player->cmd.last_cmd == DVR_PLAYBACK_CMD_PAUSE
            || ((player->play_flag&DVR_PLAYBACK_STARTED_PAUSEDLIVE) == DVR_PLAYBACK_STARTED_PAUSEDLIVE
                && ( player->cmd.cur_cmd == DVR_PLAYBACK_CMD_START
                ||player->cmd.last_cmd == DVR_PLAYBACK_CMD_V_START
                    || player->cmd.last_cmd == DVR_PLAYBACK_CMD_A_START
                    || player->cmd.last_cmd == DVR_PLAYBACK_CMD_START))) {
            DVR_PB_INFO("pause play-------cur cmd[%d]last cmd[%d]flag[0x%x]",
                          player->cmd.cur_cmd, player->cmd.last_cmd, player->play_flag);
            //need change to pause state
            player->cmd.cur_cmd = DVR_PLAYBACK_CMD_PAUSE;
            player->cmd.state = DVR_PLAYBACK_STATE_PAUSE;
            DVR_PLAYER_CHANGE_STATE(player,DVR_PLAYBACK_STATE_PAUSE);
            //clear flag
            player->play_flag = player->play_flag & (~DVR_PLAYBACK_STARTED_PAUSEDLIVE);
            player->first_frame = 0;
            AmTsPlayer_setTrickMode(player->handle, AV_VIDEO_TRICK_MODE_NONE);
            AmTsPlayer_pauseVideoDecoding(player->handle);
            AmTsPlayer_pauseAudioDecoding(player->handle);

            // Audio is unmuted here, for it was muted before receiving first frame event.
            if (player->cur_segment.flags & DVR_PLAYBACK_SEGMENT_DISPLAYABLE) {
              AmTsPlayer_setAudioMute(player->handle,0,0);
            }
          } else {
            DVR_PB_INFO("clear first frame value-------");
            player->first_frame = 0;
          }
        } else if (player->cmd.cur_cmd == DVR_PLAYBACK_CMD_FF
                || player->cmd.cur_cmd == DVR_PLAYBACK_CMD_FB
                ||player->speed > FF_SPEED ||player->speed < FB_SPEED) {
            //restart play stream if speed > 2
            if (player->state == DVR_PLAYBACK_STATE_PAUSE) {
              DVR_PB_INFO("fffb pause state----speed[%f] fffb cur[%u] cur sys[%u] [%s] [%u]",
                            player->speed,
                            player->fffb_current,
                            _dvr_time_getClock(),
                            _dvr_playback_state_toString(player->state),
                            player->next_fffb_time);
              //used timeout wait need lock first,so we unlock and lock
              //dvr_mutex_unlock(&player->lock);
              //dvr_mutex_lock(&player->lock);
              _dvr_playback_timeoutwait((DVR_PlaybackHandle_t)player, timeout);
              DVR_PB_DEBUG("unlock---");
              dvr_mutex_unlock(&player->lock);
              continue;
            } else if (_dvr_time_getClock() < player->next_fffb_time) {
              DVR_PB_INFO("fffb timeout-to pause video---speed[%f] fffb cur[%u] cur sys[%u] [%s] [%u]",
                            player->speed,
                            player->fffb_current,
                            _dvr_time_getClock(),
                            _dvr_playback_state_toString(player->state),
                            player->next_fffb_time);
              //used timeout wait need lock first,so we unlock and lock
              //dvr_mutex_unlock(&player->lock);
              //dvr_mutex_lock(&player->lock);
              AmTsPlayer_pauseVideoDecoding(player->handle);
              _dvr_playback_timeoutwait((DVR_PlaybackHandle_t)player, timeout);
              DVR_PB_DEBUG("unlock---");
              dvr_mutex_unlock(&player->lock);
              continue;
            }
            DVR_PB_INFO("fffb play-------speed[%f][%d][%d][%s][%d]",
                        player->speed,
                        goto_rewrite,
                        real_read,
                        _dvr_playback_state_toString(player->state),
                        player->cmd.cur_cmd);
            goto_rewrite = DVR_FALSE;
            real_read = 0;
            player->play_flag = player->play_flag & (~DVR_PLAYBACK_STARTED_PAUSEDLIVE);
            player->first_frame = 0;
            DVR_PB_INFO("unlock---");
            dvr_mutex_unlock(&player->lock);

            _dvr_playback_fffb((DVR_PlaybackHandle_t)player);
            DVR_PB_DEBUG("lock---");
            dvr_mutex_lock(&player->lock);
            player->fffb_play = DVR_FALSE;
        } else if(player->state == DVR_PLAYBACK_STATE_PAUSE) {
            //on pause state,user seek to new pos,we need pause and wait
            //user to resume
            DVR_PB_INFO("pause, when got first frame event when user seek end");
            player->first_frame = 0;
            AmTsPlayer_setTrickMode(player->handle, AV_VIDEO_TRICK_MODE_NONE);
            AmTsPlayer_pauseVideoDecoding(player->handle);
            AmTsPlayer_pauseAudioDecoding(player->handle);
        }
      } else if (player->fffb_play == DVR_TRUE){
        //for first into fffb when reset speed
        if (player->state == DVR_PLAYBACK_STATE_PAUSE ||
          _dvr_time_getClock() < player->next_fffb_time) {
          DVR_PB_INFO("fffb timeout-fffb play---speed[%f] fffb cur[%u] cur sys[%u] [%s] [%u]",
                        player->speed,
                        player->fffb_current,
                        _dvr_time_getClock(),
                        _dvr_playback_state_toString(player->state),
                        player->next_fffb_time);
          //used timeout wait need lock first,so we unlock and lock
          //dvr_mutex_unlock(&player->lock);
          //dvr_mutex_lock(&player->lock);
          _dvr_playback_timeoutwait((DVR_PlaybackHandle_t)player, timeout);
          DVR_PB_DEBUG("unlock---");
          dvr_mutex_unlock(&player->lock);
          continue;
        }
        DVR_PB_INFO("fffb replay-------speed[%f][%d][%d][%s][%d]player->fffb_play[%d]",
                      player->speed,
                      goto_rewrite,
                      real_read,
                      _dvr_playback_state_toString(player->state),
                      player->cmd.cur_cmd,
                      player->fffb_play);
        DVR_PB_DEBUG("unlock---");
        dvr_mutex_unlock(&player->lock);
        goto_rewrite = DVR_FALSE;
        real_read = 0;
        pthread_mutex_lock(&player->segment_lock);
        player->ts_cache_len = 0;
        player->play_flag = player->play_flag & (~DVR_PLAYBACK_STARTED_PAUSEDLIVE);
        player->first_frame = 0;
        pthread_mutex_unlock(&player->segment_lock);
        _dvr_playback_fffb((DVR_PlaybackHandle_t)player);
        dvr_mutex_lock(&player->lock);
        player->fffb_play = DVR_FALSE;
      }
    }

    if (player->state == DVR_PLAYBACK_STATE_PAUSE
        && player->seek_pause == DVR_FALSE) {
      //check is need send time send end
      DVR_PB_INFO("pause, continue");
      DVR_PB_DEBUG("unlock---");
      dvr_mutex_unlock(&player->lock);
      _dvr_playback_sent_playtime((DVR_PlaybackHandle_t)player, DVR_FALSE);
      dvr_mutex_lock(&player->lock);
      _dvr_playback_timeoutwait((DVR_PlaybackHandle_t)player, timeout);
      DVR_PB_DEBUG("unlock---");
      dvr_mutex_unlock(&player->lock);
      continue;
    }
    //when seek action is done. we need drop write timeout data.
    if (player->drop_ts == DVR_TRUE) {
      goto_rewrite = DVR_FALSE;
      real_read = 0;
      player->drop_ts = DVR_FALSE;
    }
    if (goto_rewrite == DVR_TRUE) {
      goto_rewrite = DVR_FALSE;
      //DVR_PB_DEBUG("unlock---");
      dvr_mutex_unlock(&player->lock);
      _dvr_playback_sent_playtime((DVR_PlaybackHandle_t)player, DVR_FALSE);
      //DVR_PB_INFO("rewrite-player->speed[%f]", player->speed);
      goto rewrite;
    }
    //.check is need send time send end
    dvr_mutex_unlock(&player->lock);
    _dvr_playback_sent_playtime((DVR_PlaybackHandle_t)player, DVR_FALSE);
    dvr_mutex_lock(&player->lock);
    pthread_mutex_lock(&player->segment_lock);
    //DVR_PB_INFO("start read");
    read = segment_read(player->segment_handle, buf + real_read, buf_len - real_read);
    real_read = real_read + read;
    player->ts_cache_len = real_read;
    //DVR_PB_INFO("start read end [%d]", read);
    pthread_mutex_unlock(&player->segment_lock);
    //DVR_PB_DEBUG("unlock---");
    dvr_mutex_unlock(&player->lock);
    if (read < 0 && errno == EIO) {
      //EIO ERROR, EXIT THRAD
      DVR_PB_INFO("read error.EIO error, exit thread");
      DVR_Play_Notify_t notify;
      memset(&notify, 0 , sizeof(DVR_Play_Notify_t));
      notify.event = DVR_PLAYBACK_EVENT_ERROR;
      notify.info.error_reason = DVR_ERROR_REASON_READ;
      _dvr_playback_sent_event((DVR_PlaybackHandle_t)player,DVR_PLAYBACK_EVENT_ERROR, &notify, DVR_TRUE);
      goto end;
    } else if (read < 0) {
      DVR_PB_INFO("read error.:%d EIO:%d", errno, EIO);
    }
    //if on fb mode and read file end , we need calculate pos to retry read.
    if (read == 0 && IS_FB(player->speed) && real_read == 0) {
      DVR_PB_INFO("recalculate read [%d] readed [%d]buf_len[%d]speed[%f]id=[%llu]",
                    read,
                    real_read,
                    buf_len,
                    player->speed,
                    player->cur_segment_id);
      _dvr_playback_calculate_seekpos((DVR_PlaybackHandle_t)player);
      DVR_PB_DEBUG("lock---");
      dvr_mutex_lock(&player->lock);
      _dvr_playback_timeoutwait((DVR_PlaybackHandle_t)player, timeout);
      DVR_PB_DEBUG("unlock---");
      dvr_mutex_unlock(&player->lock);
      continue;
    }
    //DVR_PB_INFO("read ts [%d]buf_len[%d]speed[%f]real_read:%d", read, buf_len, player->speed, real_read);
    if (read == 0) {
      //file end.need to play next segment
      #define MIN_CACHE_TIME (3000)
      int delay = 0;
      get_effective_tsplayer_delay_time(player,&delay);
      /*if cache time is > min cache time ,not read next segment,wait cache data to play*/
      if (delay > MIN_CACHE_TIME) {
        //dvr_mutex_lock(&player->lock);
        /*if cache time > 20s , we think get time is error,*/
        if (delay - MIN_CACHE_TIME > 20 * 1000) {
            DVR_PB_WARN("read end but cache time is %d > 20s, this is an error at media_hal", delay);
        }
      }

      int ret = _change_to_next_segment((DVR_PlaybackHandle_t)player);
      //init fffb time if change segment
       _dvr_init_fffb_time((DVR_PlaybackHandle_t)player);

      get_effective_tsplayer_delay_time(player,&delay);
      if (ret != DVR_SUCCESS && delay < MIN_TSPLAYER_DELAY_TIME) {
        player->noData++;
        DVR_PB_INFO("playback nodata[%d]", player->noData);
        if (player->noData == check_no_data_time) {
            DVR_PB_INFO("playback send nodata event nodata[%d]", player->noData);
                  //send event here and pause
            DVR_Play_Notify_t notify;
            memset(&notify, 0 , sizeof(DVR_Play_Notify_t));
            notify.event = DVR_PLAYBACK_EVENT_NODATA;
            DVR_PB_INFO("send event DVR_PLAYBACK_EVENT_NODATA--");
            //get play statue not here
            _dvr_playback_sent_event((DVR_PlaybackHandle_t)player, DVR_PLAYBACK_EVENT_NODATA, &notify, DVR_FALSE);
        }
      }

      DVR_Bool_t cond1 = (ret != DVR_SUCCESS);
      DVR_Bool_t cond2 = (player->vendor != DVR_PLAYBACK_VENDOR_AMAZON);
      DVR_Bool_t cond3 = (delay <= MIN_TSPLAYER_DELAY_TIME);
      DVR_Bool_t cond4 = (player->cmd.cur_cmd == DVR_PLAYBACK_CMD_FF);
      DVR_Bool_t cond5 = (player->delay_is_effective == DVR_TRUE);
      DVR_Bool_t cond6 = (reach_end_timeout >= MAX_REACHEND_TIMEOUT);
      if ((cond1 && cond2 && (cond3 || cond4) && cond5) || cond6) {
        DVR_PB_INFO("REACHED_END conditions: cond1:%d, cond2:%d, (cond3:%d, cond4:%d),"
            " cond5:%d, cond6:%d. delay:%d, reach_end_timeout:%d",
            (int)cond1,(int)cond2,(int)cond3,(int)cond4,(int)cond5,(int)cond6,
            delay,reach_end_timeout);
        DVR_Play_Notify_t notify;
        memset(&notify, 0 , sizeof(DVR_Play_Notify_t));
        notify.event = DVR_PLAYBACK_EVENT_REACHED_END;
        dvr_playback_pause((DVR_PlaybackHandle_t)player, DVR_FALSE);
        _dvr_playback_sent_event((DVR_PlaybackHandle_t)player, DVR_PLAYBACK_EVENT_REACHED_END, &notify, DVR_TRUE);
        dvr_mutex_lock(&player->lock);
        _dvr_playback_timeoutwait((DVR_PlaybackHandle_t)player, timeout);
        dvr_mutex_unlock(&player->lock);
        continue;
      } else if (ret != DVR_SUCCESS) {
        DVR_PB_INFO("delay:%d pauselive:%d", delay, _dvr_pauselive_decode_success((DVR_PlaybackHandle_t)player));
        dvr_mutex_lock(&player->lock);
        _dvr_playback_timeoutwait((DVR_PlaybackHandle_t)player, timeout);
        dvr_mutex_unlock(&player->lock);

        get_effective_tsplayer_delay_time(player,&delay);
        //not send event and pause,sleep and go to next time to recheck
        if (delay < cache_time) {
          //delay time is changed and then has data to play, so not start timeout
          reach_end_timeout = 0;
        } else {
          reach_end_timeout = reach_end_timeout + timeout;
        }
        cache_time = delay;
        continue;
      }
      reach_end_timeout = 0;
      cache_time = 0;
      //change next segment success case
      _dvr_playback_sent_transition_ok((DVR_PlaybackHandle_t)player, DVR_FALSE);
      dvr_mutex_lock(&player->lock);
      player->noData = 0;
      DVR_PB_INFO("_dvr_replay_changed_pid:start");
      _dvr_replay_changed_pid((DVR_PlaybackHandle_t)player);
      _dvr_check_cur_segment_flag((DVR_PlaybackHandle_t)player);
      pthread_mutex_lock(&player->segment_lock);
      read = segment_read(player->segment_handle, buf + real_read, buf_len - real_read);
      real_read = real_read + read;
      player->ts_cache_len = real_read;
      pthread_mutex_unlock(&player->segment_lock);

      dvr_mutex_unlock(&player->lock);
    }//read len 0 check end

#ifdef FOR_SKYWORTH_FETCH_RDK
    if (player->openParams.is_timeshift == DVR_TRUE && player->speed >= FF_SPEED)
    {
      DVR_PlaybackSegmentInfo_t *newest_segment
        = list_last_entry(&player->segment_list, DVR_PlaybackSegmentInfo_t, head);
      DVR_Bool_t cond1 = (player->cur_segment_id == newest_segment->segment_id);
      int32_t time_end = _dvr_get_end_time((DVR_PlaybackHandle_t)player);
      uint64_t cur_seg_id = 0;
      int32_t time_cur = _dvr_get_play_cur_time((DVR_PlaybackHandle_t)player, &cur_seg_id);
      DVR_Bool_t cond2 = (cur_seg_id == player->cur_segment_id && time_end - time_cur < 1000);
      if (cond1 && cond2)
      {
        DVR_PlaybackSpeed_t normal_speed = {PLAYBACK_SPEED_X1,0};
        DVR_PB_INFO("Change to normal speed due to FF reaching end");
        dvr_playback_set_speed((DVR_PlaybackHandle_t)player,normal_speed);

        DVR_Play_Notify_t notify;
        memset(&notify, 0 , sizeof(DVR_Play_Notify_t));
        notify.event = DVR_PLAYBACK_EVENT_TIMESHIFT_FF_REACHED_END;
        _dvr_playback_sent_event((DVR_PlaybackHandle_t)player, notify.event, &notify, 0);
      }
    }
#endif

    if (player->noData >= check_no_data_time) {
      player->noData = 0;
      DVR_PB_INFO("playback send data event resume[%d]", player->noData);
      //send event here and pause
      DVR_Play_Notify_t notify;
      memset(&notify, 0 , sizeof(DVR_Play_Notify_t));
      notify.event = DVR_PLAYBACK_EVENT_DATARESUME;
      DVR_PB_INFO("----send event DVR_PLAYBACK_EVENT_DATARESUME");
      //get play statue not here
      _dvr_playback_sent_event((DVR_PlaybackHandle_t)player, DVR_PLAYBACK_EVENT_DATARESUME, &notify, DVR_FALSE);
    }
    reach_end_timeout = 0;
    //real_read = real_read + read;
    input_buffer.buf_size = real_read;
    input_buffer.buf_data = buf;

    //check read data len,if len < 0, we need continue
    if (input_buffer.buf_size <= 0 || input_buffer.buf_data == NULL) {
      DVR_PB_INFO("error occur read_read [%d],buf=[%p]",input_buffer.buf_size, input_buffer.buf_data);
      real_read = 0;
      pthread_mutex_lock(&player->segment_lock);
      player->ts_cache_len = 0;
      pthread_mutex_unlock(&player->segment_lock);
      continue;
    }
    //if need write whole block size, we need check read buf len is eq block size.
    if (b_writed_whole_block == DVR_TRUE
        && (player->has_video || player->dec_func || player->cryptor)) {
      //buf_len is block size value.
      if (real_read < buf_len) {
        //continue to read data from file
        DVR_PB_INFO("read buf len[%d] is < block size [%d]", real_read, buf_len);
        dvr_mutex_lock(&player->lock);
        _dvr_playback_timeoutwait((DVR_PlaybackHandle_t)player, timeout);
        dvr_mutex_unlock(&player->lock);
        DVR_PB_INFO("read buf len[%d] is < block size [%d] continue", real_read, buf_len);
        continue;
      } else if (real_read > buf_len) {
        DVR_PB_INFO("read buf len[%d] is > block size [%d],this error occur", real_read, buf_len);
      }
    }

    if (player->dec_func) {
      DVR_CryptoParams_t crypto_params;

      memset(&crypto_params, 0, sizeof(crypto_params));
      crypto_params.type = DVR_CRYPTO_TYPE_DECRYPT;
      memcpy(crypto_params.location, player->cur_segment.location, strlen(player->cur_segment.location));
      crypto_params.segment_id = player->cur_segment.segment_id;
      crypto_params.offset = segment_tell_position(player->segment_handle) - input_buffer.buf_size;
      if ((crypto_params.offset % (player->openParams.block_size)) != 0)
        DVR_PB_INFO("offset is not block_size %d", player->openParams.block_size);
      crypto_params.input_buffer.type = DVR_BUFFER_TYPE_NORMAL;
      crypto_params.input_buffer.addr = (size_t)buf;
      crypto_params.input_buffer.size = real_read;

      if (player->is_secure_mode) {
          crypto_params.output_buffer.type = DVR_BUFFER_TYPE_SECURE;
          crypto_params.output_buffer.addr = (size_t)player->secure_buffer;
          crypto_params.output_buffer.size = dec_buf_size;
          ret = player->dec_func(&crypto_params, player->dec_userdata);
          input_buffer.buf_data = player->secure_buffer;
          input_buffer.buf_type = TS_INPUT_BUFFER_TYPE_SECURE;
          if (ret != DVR_SUCCESS) {
            DVR_PB_INFO("decrypt failed");
          }
          input_buffer.buf_size = crypto_params.output_size;
      } else {  // only for NAGRA
          crypto_params.output_buffer.type = crypto_params.input_buffer.type;
          crypto_params.output_buffer.addr = (size_t)dec_bufs.buf_data;
          crypto_params.output_buffer.size = crypto_params.input_buffer.size;
          ret = player->dec_func(&crypto_params, player->dec_userdata);
          input_buffer.buf_data = (uint8_t*)crypto_params.output_buffer.addr;
          input_buffer.buf_type = TS_INPUT_BUFFER_TYPE_NORMAL;
          if (ret != DVR_SUCCESS) {
            DVR_PB_INFO("decrypt failed");
          }
          input_buffer.buf_size = crypto_params.output_buffer.size;
      }
    } else if (player->cryptor) {
      int len = real_read;
      am_crypt_des_crypt(player->cryptor, dec_bufs.buf_data, buf, &len, 1);
      input_buffer.buf_data = dec_bufs.buf_data;
      input_buffer.buf_type = TS_INPUT_BUFFER_TYPE_NORMAL;
      input_buffer.buf_size = len;
    }
rewrite:
    if (player->drop_ts == DVR_TRUE) {
      //need drop ts data when seek occur.we need read next loop,drop this ts data
      goto_rewrite = DVR_FALSE;
      real_read = 0;
      pthread_mutex_lock(&player->segment_lock);
      player->ts_cache_len = 0;
      player->drop_ts = DVR_FALSE;
      pthread_mutex_unlock(&player->segment_lock);
      DVR_PB_INFO("----drop ts");
      continue;
    }

    pthread_mutex_lock(&player->segment_lock);
    player->ts_cache_len = real_read;
    //used for printf first write data time.
    //to check change channel kpi.
    if (first_write == 0) {
      first_write++;
      DVR_PB_INFO("----first write ts data");
    }

    ret = AmTsPlayer_writeData(player->handle, &input_buffer, write_timeout_ms);
    if (ret == AM_TSPLAYER_OK) {
      player->ts_cache_len = 0;
      pthread_mutex_unlock(&player->segment_lock);
      real_read = 0;
      write_success++;
      if (player->control_speed_enable == 1) {
check0:
            if (!player->is_running) {
                //DVR_PB_DEBUG(1, "playback thread exit");
                break;
            }
            if (_dvr_check_speed_con((DVR_PlaybackHandle_t)player) == DVR_FALSE){
              dvr_mutex_lock(&player->lock);
              _dvr_playback_timeoutwait((DVR_PlaybackHandle_t)player, 50);
              dvr_mutex_unlock(&player->lock);
              _dvr_playback_sent_playtime((DVR_PlaybackHandle_t)player, DVR_FALSE);
              goto check0;
            }
      }
      //DVR_PB_INFO("write  write_success:%d input_buffer.buf_size:%d", write_success, input_buffer.buf_size);
    } else {
      pthread_mutex_unlock(&player->segment_lock);
      DVR_PB_DEBUG("write time out write_success:%d buf_size:%d systime:%u",
          write_success, input_buffer.buf_size, _dvr_time_getClock());
      write_success = 0;
      if (player->control_speed_enable == 1) {
check1:
        if (!player->is_running) {
            //DVR_PB_DEBUG(1, "playback thread exit");
            break;
        }
        if (_dvr_check_speed_con((DVR_PlaybackHandle_t)player) == DVR_FALSE){
          dvr_mutex_lock(&player->lock);
          _dvr_playback_timeoutwait((DVR_PlaybackHandle_t)player, 50);
          dvr_mutex_unlock(&player->lock);
          _dvr_playback_sent_playtime((DVR_PlaybackHandle_t)player, DVR_FALSE);
          goto check1;
        }
      }
      dvr_mutex_lock(&player->lock);
      _dvr_playback_timeoutwait((DVR_PlaybackHandle_t)player, timeout);
      dvr_mutex_unlock(&player->lock);
      if (!player->is_running) {
        DVR_PB_INFO("playback thread exit");
         break;
      }
      goto_rewrite = DVR_TRUE;
      //goto rewrite;
    }
  }
end:
  DVR_PB_INFO("playback thread is end");
  free(buf);
  free(dec_bufs.buf_data);
  return NULL;
}


static int _start_playback_thread(DVR_PlaybackHandle_t handle)
{
  DVR_Playback_t *player = (DVR_Playback_t *) handle;

  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }
  DVR_PB_INFO("start thread is_running:[%d]", player->is_running);
  if (player->is_running == DVR_TRUE) {
    return 0;
  }
  player->is_running = DVR_TRUE;
  int rc = pthread_create(&player->playback_thread, NULL, _dvr_playback_thread, (void*)player);
  if (rc < 0)
    player->is_running = DVR_FALSE;
  return 0;
}


static int _stop_playback_thread(DVR_PlaybackHandle_t handle)
{
  DVR_Playback_t *player = (DVR_Playback_t *) handle;

  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }

  DVR_PB_INFO("stopthread------[%d]", player->is_running);
  if (player->is_running == DVR_TRUE)
  {
    player->is_running = DVR_FALSE;
    _dvr_playback_sendSignal(handle);
    pthread_join(player->playback_thread, NULL);
  }
  if (player->segment_handle) {
    segment_close(player->segment_handle);
    player->segment_handle = NULL;
  }
  DVR_PB_INFO(":end");
  return 0;
}

static int getFakePid()
{
   return dvr_prop_read_int("vendor.tv.dtv.fake_pid",0xffff);
}

void dvr_playback_change_seek_state(DVR_PlaybackHandle_t handle,int pid) {

  DVR_ASSERT(handle);
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return ;
  }
  if (player->need_seek_start == DVR_FALSE) {
    DVR_PB_INFO("player need_seek_start is false");
    return ;
  }

  if (pid != player->fake_pid) {
      player->need_seek_start = DVR_FALSE;
  }
  DVR_PB_INFO("player player->need_seek_start=%d", player->need_seek_start);
}

/**\brief Open an dvr palyback
 * \param[out] p_handle dvr playback addr
 * \param[in] params dvr playback open parameters
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_open(DVR_PlaybackHandle_t *p_handle, DVR_PlaybackOpenParams_t *params) {

  DVR_Playback_t *player;
  pthread_condattr_t  cattr;

  player = (DVR_Playback_t*)calloc(1, sizeof(DVR_Playback_t));
  DVR_RETURN_IF_FALSE(player);

  dvr_mutex_init(&player->lock);
  pthread_mutex_init(&player->segment_lock, NULL);
  pthread_condattr_init(&cattr);
  pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
  pthread_cond_init(&player->cond, &cattr);
  pthread_condattr_destroy(&cattr);

  //init segment list head
  INIT_LIST_HEAD(&player->segment_list);
  player->cmd.last_cmd = DVR_PLAYBACK_CMD_STOP;
  player->cmd.cur_cmd = DVR_PLAYBACK_CMD_STOP;
  player->cmd.speed.speed.speed = PLAYBACK_SPEED_X1;
  player->cmd.state = DVR_PLAYBACK_STATE_STOP;
  DVR_PLAYER_CHANGE_STATE(player,DVR_PLAYBACK_STATE_STOP);
  player->cmd.pos = 0;
  player->speed = 1.0f;
  player->first_trans_ok = DVR_FALSE;

  //store open params
  player->openParams.dmx_dev_id = params->dmx_dev_id;
  player->openParams.block_size = params->block_size;
  DVR_PB_INFO("playback open block_size:[%d]",params->block_size);
  player->openParams.is_timeshift = params->is_timeshift;
  player->openParams.event_fn = params->event_fn;
  player->openParams.event_userdata = params->event_userdata;
  player->openParams.is_notify_time = params->is_notify_time;
  player->vendor = params->vendor;

  player->has_pids = params->has_pids;

  player->handle = params->player_handle ;
  player->control_speed_enable = params->control_speed_enable;

  AmTsPlayer_getCb(player->handle, &player->player_callback_func, &player->player_callback_userdata);
  //for test get callback
  if (0 && player->player_callback_func == NULL) {
    AmTsPlayer_registerCb(player->handle, _dvr_tsplayer_callback_test, player);
    AmTsPlayer_getCb(player->handle, &player->player_callback_func, &player->player_callback_userdata);
    DVR_PB_INFO("playback open get callback[%p][%p][%p][%p]",
                  player->player_callback_func,
                  player->player_callback_userdata,
                  _dvr_tsplayer_callback_test,
                  player);
  }
  AmTsPlayer_registerCb(player->handle, _dvr_tsplayer_callback, player);

  //init has audio and video
  player->has_video = DVR_FALSE;
  player->has_audio = DVR_FALSE;
  player->cur_segment_id = UINT64_MAX;
  player->last_segment_id = 0LL;
  player->segment_is_open = DVR_FALSE;
  player->audio_presentation_id = -1;

  //init ff fb time
  player->fffb_current = 0;
  player->fffb_start = 0;
  player->fffb_start_pcr = 0;
  //seek time
  player->seek_time = 0;
  player->send_time = 0;

  //allocate cryptor if have clearkey
  if (params->keylen > 0) {
    player->cryptor = am_crypt_des_open((uint8_t *)params->clearkey,
                (uint8_t *)params->cleariv,
                params->keylen * 8);
    if (!player->cryptor) {
      DVR_INFO("%s , open des cryptor failed!!!\n", __func__);
    }
  } else {
    player->cryptor = NULL;
  }

  //init secure stuff
  player->dec_func = NULL;
  player->dec_userdata = NULL;
  player->is_secure_mode = 0;
  player->secure_buffer = NULL;
  player->secure_buffer_size = 0;
  player->drop_ts = DVR_FALSE;

  player->fffb_play = DVR_FALSE;

  player->last_send_time_id = UINT64_MAX;
  player->last_cur_time = 0;
  player->seek_pause = DVR_FALSE;

  //speed con init
  if (player->control_speed_enable == 1) {
    player->con_spe.ply_dur = 0;
    player->con_spe.ply_sta = 0;
    player->con_spe.sys_dur = 0;
    player->con_spe.sys_sta = 0;
  }

  //limit info
  player->rec_start = 0;
  player->limit = 0;
  //need seek to start pos
  player->first_start_time = 0;
  player->first_start_id = UINT64_MAX;
  player->delay_is_effective = DVR_FALSE;
  player->need_seek_start = DVR_TRUE;
  //fake_pid init
  player->fake_pid = getFakePid();
  *p_handle = player;
  return DVR_SUCCESS;
}

/**\brief Close an dvr palyback
 * \param[in] handle playback handle
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_close(DVR_PlaybackHandle_t handle) {

  DVR_ASSERT(handle);
  DVR_PB_INFO(":into");
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }

  if (player->state != DVR_PLAYBACK_STATE_STOP)
  {
    DVR_PB_INFO("player->state %s", _dvr_playback_state_toString(player->state));
    if (player->cryptor) {
      am_crypt_des_close(player->cryptor);
      player->cryptor = NULL;
    }
    dvr_playback_stop(handle, DVR_TRUE);
    DVR_PB_INFO("player->state %s", _dvr_playback_state_toString(player->state));
  } else {
    DVR_PB_INFO(":is stoped state");
  }
  DVR_PB_INFO(":into");
  dvr_mutex_destroy(&player->lock);
  pthread_mutex_destroy(&player->segment_lock);
  pthread_cond_destroy(&player->cond);

  if (player) {
    free(player);
  }
  DVR_PB_INFO(":end");
  return DVR_SUCCESS;
}

/**\brief Start play audio and video, used start audio api and start video api
 * \param[in] handle playback handle
 * \param[in] params audio playback params,contains fmt and pid...
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_start(DVR_PlaybackHandle_t handle, DVR_PlaybackFlag_t flag) {
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  am_tsplayer_video_params    video_params;
  am_tsplayer_audio_params    audio_params;
  am_tsplayer_audio_params    ad_params;

  memset(&video_params, 0, sizeof(video_params));
  memset(&audio_params, 0, sizeof(audio_params));

  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }
  uint64_t segment_id = player->cur_segment_id;
  DVR_PB_INFO("[%p]segment_id:[%lld]", handle, segment_id);

  player->first_frame = 0;
  //can used start api to resume playback
  if (player->cmd.state == DVR_PLAYBACK_STATE_PAUSE) {
    return dvr_playback_resume(handle);
  }
  if (player->cmd.state == DVR_PLAYBACK_STATE_START) {
    //if flag is paused and not decode first frame. if user resume, we need
    //clear flag and set trickmode none
    if ((player->play_flag&DVR_PLAYBACK_STARTED_PAUSEDLIVE) == DVR_PLAYBACK_STARTED_PAUSEDLIVE) {
      DVR_PB_INFO("[%p]clear pause live flag and clear trick mode", handle);
      player->play_flag = player->play_flag & (~DVR_PLAYBACK_STARTED_PAUSEDLIVE);
      AmTsPlayer_setTrickMode(player->handle, AV_VIDEO_TRICK_MODE_NONE);
    }
    DVR_PB_INFO("stat is start, not need into start play");
    return DVR_SUCCESS;
  }
  player->play_flag = flag;
  player->first_trans_ok = DVR_FALSE;
  //get segment info and audio video pid fmt ;
  DVR_PB_INFO("lock flag:0x%x", flag);
  dvr_mutex_lock(&player->lock);
  _dvr_playback_get_playinfo(handle, segment_id, &video_params, &audio_params, &ad_params);
  //start audio and video
  if (video_params.pid != player->fake_pid && !VALID_PID(video_params.pid) && !VALID_PID(audio_params.pid)) {
    //audio and video pids are all invalid, return error.
    DVR_PB_ERROR("unlock dvr play back start error, not found audio and video info [0x%x]", video_params.pid);
    dvr_mutex_unlock(&player->lock);
    DVR_Play_Notify_t notify;
    memset(&notify, 0 , sizeof(DVR_Play_Notify_t));
    notify.event = DVR_PLAYBACK_EVENT_TRANSITION_FAILED;
    notify.info.error_reason = DVR_PLAYBACK_PID_ERROR;
    notify.info.transition_failed_data.segment_id = segment_id;
    //get play statue not here
    _dvr_playback_sent_event(handle, DVR_PLAYBACK_EVENT_TRANSITION_FAILED, &notify, DVR_TRUE);
    return -1;
  }

  {
    if (VALID_PID(video_params.pid)) {
      player->has_video = DVR_TRUE;
      //if set flag is pause live, we need set trick mode
      if ((player->play_flag&DVR_PLAYBACK_STARTED_PAUSEDLIVE) == DVR_PLAYBACK_STARTED_PAUSEDLIVE) {
        DVR_PB_INFO("set trick mode -pauselive flag--");
        AmTsPlayer_setTrickMode(player->handle, AV_VIDEO_TRICK_MODE_PAUSE_NEXT);
      } else if (player->cmd.cur_cmd == DVR_PLAYBACK_CMD_FB
        || player->cmd.cur_cmd == DVR_PLAYBACK_CMD_FF) {
        DVR_PB_INFO("set trick mode -fffb--at pause live");
        AmTsPlayer_setTrickMode(player->handle, AV_VIDEO_TRICK_MODE_PAUSE_NEXT);
      } else {
        DVR_PB_INFO("set trick mode ---none");
        AmTsPlayer_setTrickMode(player->handle, AV_VIDEO_TRICK_MODE_NONE);
      }
      AmTsPlayer_showVideo(player->handle);
      AmTsPlayer_setVideoParams(player->handle,  &video_params);
      AmTsPlayer_setVideoBlackOut(player->handle, 1);
      AmTsPlayer_startVideoDecoding(player->handle);
    }

    DVR_PB_INFO("player->cmd.cur_cmd:%d vpid[0x%x]apis[0x%x]", player->cmd.cur_cmd, video_params.pid, audio_params.pid);
    player->last_send_time_id = UINT64_MAX;
    if (player->cmd.cur_cmd == DVR_PLAYBACK_CMD_FB
      || player->cmd.cur_cmd == DVR_PLAYBACK_CMD_FF) {
      player->cmd.state = DVR_PLAYBACK_STATE_START;
      DVR_PLAYER_CHANGE_STATE(player,DVR_PLAYBACK_STATE_START);
    } else {
      player->cmd.last_cmd = player->cmd.cur_cmd;
      player->cmd.cur_cmd = DVR_PLAYBACK_CMD_START;
      if (IS_FAST_SPEED(player->cmd.speed.speed.speed)) {
        //set fast play
        DVR_PB_INFO("start fast");
        AmTsPlayer_startFast(player->handle, (float)player->cmd.speed.speed.speed/100.0f);
      } else {
        if (VALID_PID(ad_params.pid)) {
          player->has_ad_audio = DVR_TRUE;
          DVR_PB_INFO("start ad audio");
          dvr_playback_change_seek_state(handle, ad_params.pid);
          AmTsPlayer_setADParams(player->handle,  &ad_params);
          AmTsPlayer_enableADMix(player->handle);
        }
        if (VALID_PID(audio_params.pid)) {
          DVR_PB_INFO("start audio");
          player->has_audio = DVR_TRUE;
          dvr_playback_change_seek_state(handle, audio_params.pid);
          AmTsPlayer_setAudioParams(player->handle,  &audio_params);
          if (player->audio_presentation_id > -1) {
            AmTsPlayer_setParams(player->handle, AM_TSPLAYER_KEY_AUDIO_PRESENTATION_ID, &player->audio_presentation_id);
          }
          AmTsPlayer_startAudioDecoding(player->handle);
        }
      }
      player->cmd.state = DVR_PLAYBACK_STATE_START;
      DVR_PLAYER_CHANGE_STATE(player,DVR_PLAYBACK_STATE_START);
    }
  }
#ifdef AVSYNC_USED_PCR
  if (player && VALID_PID(player->cur_segment.pids.pcr.pid)) {
    DVR_PB_INFO("start set pcr [%d]", player->cur_segment.pids.pcr.pid);
    AmTsPlayer_setPcrPid(player->handle, player->cur_segment.pids.pcr.pid);
  }
#endif
  DVR_PB_DEBUG("unlock");
  dvr_mutex_unlock(&player->lock);
  _start_playback_thread(handle);
  return DVR_SUCCESS;
}
/**\brief dvr play back add segment info to segment list
 * \param[in] handle playback handle
 * \param[in] info added segment info,con vpid fmt apid fmt.....
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_add_segment(DVR_PlaybackHandle_t handle, DVR_PlaybackSegmentInfo_t *info) {
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  DVR_RETURN_IF_FALSE(player);

  DVR_PB_INFO("add segment id: %lld %p", info->segment_id, handle);
  DVR_PlaybackSegmentInfo_t *segment;

  segment = malloc(sizeof(DVR_PlaybackSegmentInfo_t));
  DVR_RETURN_IF_FALSE(segment);
  memset(segment, 0, sizeof(DVR_PlaybackSegmentInfo_t));

  //not memcpy chunk info.
  segment->segment_id = info->segment_id;
  //cp location
  memcpy(segment->location, info->location, DVR_MAX_LOCATION_SIZE);

  DVR_PB_INFO("add location [%s]id[%lld]flag[%x]", segment->location, segment->segment_id, info->flags);
  segment->flags = info->flags;

  //pids
  segment->pids.video.pid = info->pids.video.pid;
  segment->pids.video.format = info->pids.video.format;
  segment->pids.video.type = info->pids.video.type;

  segment->pids.audio.pid = info->pids.audio.pid;
  segment->pids.audio.format = info->pids.audio.format;
  segment->pids.audio.type = info->pids.audio.type;

  segment->pids.ad.pid = info->pids.ad.pid;
  segment->pids.ad.format = info->pids.ad.format;
  segment->pids.ad.type = info->pids.ad.type;

  segment->pids.pcr.pid = info->pids.pcr.pid;

  DVR_PB_INFO("lock pid [0x%x][0x%x][0x%x][0x%x]", segment->pids.video.pid,segment->pids.audio.pid, info->pids.video.pid,info->pids.audio.pid);
  dvr_mutex_lock(&player->lock);
  list_add_tail(segment, &player->segment_list);
  dvr_mutex_unlock(&player->lock);
  DVR_PB_DEBUG("unlock");

  return DVR_SUCCESS;
}
/**\brief dvr play back remove segment info by segment_id
 * \param[in] handle playback handle
 * \param[in] segment_id need removed segment id
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_remove_segment(DVR_PlaybackHandle_t handle, uint64_t segment_id) {
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  DVR_PB_INFO("remove segment id: %lld", segment_id);
  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }

  if (segment_id == player->cur_segment_id) {
    DVR_PB_INFO("not support remove current segment id: %lld", segment_id);
    return DVR_FAILURE;
  }
  DVR_PB_DEBUG("lock");
  dvr_mutex_lock(&player->lock);
  DVR_PlaybackSegmentInfo_t *segment = NULL;
  DVR_PlaybackSegmentInfo_t *segment_tmp = NULL;
  list_for_each_entry_safe(segment, segment_tmp, &player->segment_list, head)
  {
    if (segment->segment_id == segment_id) {
      list_del(&segment->head);
      free(segment);
      break;
    }
  }
  DVR_PB_DEBUG("unlock");
  dvr_mutex_unlock(&player->lock);

  return DVR_SUCCESS;
}
/**\brief dvr play back add segment info
 * \param[in] handle playback handle
 * \param[in] info added segment info,con vpid fmt apid fmt.....
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_update_segment_flags(DVR_PlaybackHandle_t handle,
  uint64_t segment_id, DVR_PlaybackSegmentFlag_t flags) {
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  DVR_PB_INFO("update segment id: %lld flag:%d", segment_id, flags);
  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }
  if (player->vendor != DVR_PLAYBACK_VENDOR_DEF) {
    DVR_PB_INFO("In case of vendor Amlogic/Amazon, do not control AV display");
    return DVR_SUCCESS;
  }

  DVR_PlaybackSegmentInfo_t *segment;
  DVR_PB_DEBUG("lock");
  dvr_mutex_lock(&player->lock);
  // This error is suppressed as the macro code is picked from kernel.
  // prefetch() here incurring self_assign is used to avoid some compiling
  // warnings.
  // coverity[self_assign]
  list_for_each_entry(segment, &player->segment_list, head)
  {
    if (segment->segment_id != segment_id) {
      continue;
    }
    // if encramble to free， only set flag and return;

    //if displayable to none, we need mute audio and video
    if (segment_id == player->cur_segment_id) {
      if ((segment->flags & DVR_PLAYBACK_SEGMENT_DISPLAYABLE) == DVR_PLAYBACK_SEGMENT_DISPLAYABLE
        && (flags & DVR_PLAYBACK_SEGMENT_DISPLAYABLE) == 0) {
        //disable display, mute
        DVR_PB_INFO("mute av");
        AmTsPlayer_hideVideo(player->handle);
        AmTsPlayer_setAudioMute(player->handle, 1, 1);
      } else if ((segment->flags & DVR_PLAYBACK_SEGMENT_DISPLAYABLE) == 0 &&
          (flags & DVR_PLAYBACK_SEGMENT_DISPLAYABLE) == DVR_PLAYBACK_SEGMENT_DISPLAYABLE) {
        //enable display, unmute
        DVR_PB_INFO("unmute av");
        AmTsPlayer_showVideo(player->handle);
        AmTsPlayer_setAudioMute(player->handle, 0, 0);
      } else {
        //do nothing
      }
    } else {
      //do nothing
    }
    //continue , only set flag
    segment->flags = flags;
  }
  DVR_PB_DEBUG("unlock");
  dvr_mutex_unlock(&player->lock);
  return DVR_SUCCESS;
}


static int _do_handle_pid_update(DVR_PlaybackHandle_t handle, DVR_PlaybackPids_t  now_pids, DVR_PlaybackPids_t set_pids, int type) {
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  DVR_StreamInfo_t set_pid;
  DVR_StreamInfo_t now_pid;

  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }

  if (type == 0) {
    set_pid = set_pids.video;
    now_pid = now_pids.video;
  } else if (type == 1) {
    set_pid = set_pids.audio;
    now_pid = now_pids.audio;
  } else if (type == 2) {
    set_pid = set_pids.ad;
    now_pid = now_pids.ad;
  } else {
    set_pid = set_pids.pcr;
    now_pid = now_pids.pcr;
  }

  if (type == 1 && VALID_PID(set_pid.pid) && player->cmd.state == DVR_PLAYBACK_STATE_START
      && player->play_flag&DVR_PLAYBACK_STARTED_PAUSEDLIVE) {
    // Here we mute audio no matter it is displayable or not in starting phase of a playback.
    // Audio will be unmuted shortly on receiving first frame event.
    AmTsPlayer_setAudioMute(player->handle,1,1);
  }

  if (now_pid.pid == set_pid.pid) {
    //do nothing
    return 0;
  } else if (player->cmd.state == DVR_PLAYBACK_STATE_START) {
    if (VALID_PID(now_pid.pid)) {
      //stop now stream
      if (type == 0) {
        //stop video
        if (player->has_video == DVR_TRUE) {
          DVR_PB_INFO("stop video");
          AmTsPlayer_stopVideoDecoding(player->handle);
          player->has_video = DVR_FALSE;
        }
      } else if (type == 1) {
        //stop audio
        if (player->has_audio == DVR_TRUE) {
          DVR_PB_INFO("stop audio");
          AmTsPlayer_stopAudioDecoding(player->handle);
          player->has_audio = DVR_FALSE;
        }
      } else if (type == 2) {
        //stop sub audio
        DVR_PB_INFO("stop ad");
        AmTsPlayer_disableADMix(player->handle);
      } else if (type == 3) {
        //pcr
      }
    }
    if (VALID_PID(set_pid.pid)) {
      //start
      if (type == 0) {
        //start video
        am_tsplayer_video_params video_params;
        video_params.pid = set_pid.pid;
        video_params.codectype = _dvr_convert_stream_fmt(set_pid.format, DVR_FALSE);
        player->has_video = DVR_TRUE;
        DVR_PB_INFO("start video pid[%d]fmt[%d]",video_params.pid, video_params.codectype);
        AmTsPlayer_setVideoParams(player->handle,  &video_params);
        AmTsPlayer_startVideoDecoding(player->handle);
        //playback_device_video_start(player->handle,&video_params);
      } else if (type == 1) {
        //start audio
        if (player->cmd.speed.speed.speed == PLAYBACK_SPEED_X1) {
          if (VALID_PID(set_pids.ad.pid)) {
            am_tsplayer_audio_params ad_params;
            ad_params.pid = set_pids.ad.pid;
            ad_params.codectype= _dvr_convert_stream_fmt(set_pids.ad.format, DVR_TRUE);
            DVR_PB_INFO("start ad audio pid[%d]fmt[%d]",ad_params.pid, ad_params.codectype);
            AmTsPlayer_setADParams(player->handle,  &ad_params);
            AmTsPlayer_enableADMix(player->handle);
          }

          am_tsplayer_audio_params audio_params;

          memset(&audio_params, 0, sizeof(audio_params));

          audio_params.pid = set_pid.pid;
          audio_params.codectype= _dvr_convert_stream_fmt(set_pid.format, DVR_TRUE);
          player->has_audio = DVR_TRUE;
          DVR_PB_INFO("start audio pid[%d]fmt[%d]",audio_params.pid, audio_params.codectype);
          AmTsPlayer_setAudioParams(player->handle,  &audio_params);
          if (player->audio_presentation_id > -1) {
            AmTsPlayer_setParams(player->handle, AM_TSPLAYER_KEY_AUDIO_PRESENTATION_ID, &player->audio_presentation_id);
          }

          AmTsPlayer_startAudioDecoding(player->handle);
          //playback_device_audio_start(player->handle,&audio_params);
        }
      } else if (type == 2) {
        if (player->cmd.speed.speed.speed == PLAYBACK_SPEED_X1) {
          if (set_pids.audio.pid == now_pids.audio.pid) {
            //stop audio if audio pid not change
            DVR_PB_INFO("stop audio when start ad");
            AmTsPlayer_stopAudioDecoding(player->handle);
          }
          am_tsplayer_audio_params audio_params;

          memset(&audio_params, 0, sizeof(audio_params));
          audio_params.pid = set_pid.pid;
          audio_params.codectype= _dvr_convert_stream_fmt(set_pid.format, DVR_TRUE);
          player->has_audio = DVR_TRUE;
          DVR_PB_INFO("start ad audio pid[%d]fmt[%d]",audio_params.pid, audio_params.codectype);
          AmTsPlayer_setADParams(player->handle,  &audio_params);
          AmTsPlayer_enableADMix(player->handle);

          if (set_pids.audio.pid == now_pids.audio.pid) {
              am_tsplayer_audio_params audio_params;

              memset(&audio_params, 0, sizeof(audio_params));

              audio_params.pid = set_pids.audio.pid;
              audio_params.codectype= _dvr_convert_stream_fmt(set_pids.audio.format, DVR_TRUE);
              player->has_audio = DVR_TRUE;
              DVR_PB_INFO("restart audio when start ad");
              AmTsPlayer_setAudioParams(player->handle,  &audio_params);
              if (player->audio_presentation_id > -1) {
                AmTsPlayer_setParams(player->handle, AM_TSPLAYER_KEY_AUDIO_PRESENTATION_ID, &player->audio_presentation_id);
              }
              AmTsPlayer_startAudioDecoding(player->handle);
          }
        }
      } else if (type == 3) {
        //pcr
        DVR_PB_INFO("start set pcr [%d]", set_pid.pid);
        AmTsPlayer_setPcrPid(player->handle, set_pid.pid);
      }
      //audio and video all close
      if (!player->has_audio && !player->has_video) {
        DVR_PLAYER_CHANGE_STATE(player,DVR_PLAYBACK_STATE_STOP);
      }
    } else if (type == 2) {
      //case disable ad
        DVR_PB_INFO("restart  audio when stop ad");
      if (player->cmd.speed.speed.speed == PLAYBACK_SPEED_X1) {
        if (VALID_PID(now_pids.audio.pid)) {
          //stop audio if audio pid not change
          DVR_PB_INFO("stop audio when stop ad pid [0x%x]", now_pids.audio.pid);
          AmTsPlayer_stopAudioDecoding(player->handle);
          am_tsplayer_audio_params audio_params;

          memset(&audio_params, 0, sizeof(audio_params));

          audio_params.pid = now_pids.audio.pid;
          audio_params.codectype= _dvr_convert_stream_fmt(now_pids.audio.format, DVR_TRUE);
          player->has_audio = DVR_TRUE;
          DVR_PB_INFO("restart audio when stop ad");
          AmTsPlayer_setAudioParams(player->handle,  &audio_params);
          if (player->audio_presentation_id > -1) {
            AmTsPlayer_setParams(player->handle, AM_TSPLAYER_KEY_AUDIO_PRESENTATION_ID, &player->audio_presentation_id);
          }
          AmTsPlayer_startAudioDecoding(player->handle);
        }
      }
    }
  }
  return 0;
}
/**\brief dvr play back only update segment pids info
 * only update pid info not to start stop codec.
 * \param[in] handle playback handle
 * \param[in] segment_id need updated pids segment id
 * \param[in] p_pids need updated pids
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_only_update_segment_pids(DVR_PlaybackHandle_t handle, uint64_t segment_id, DVR_PlaybackPids_t *p_pids) {
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }

  DVR_PlaybackSegmentInfo_t *segment;
  DVR_PB_DEBUG("lock");
  dvr_mutex_lock(&player->lock);
  DVR_PB_INFO("get lock update segment id: %lld cur id %lld", segment_id, player->cur_segment_id);
  // This error is suppressed as the macro code is picked from kernel.
  // prefetch() here incurring self_assign is used to avoid some compiling
  // warnings.
  // coverity[self_assign]
  list_for_each_entry(segment, &player->segment_list, head)
  {
    if (segment->segment_id == segment_id) {
      if (player->cur_segment_id == segment_id) {
        if (player->cmd.state == DVR_PLAYBACK_STATE_FF
          || player->cmd.state == DVR_PLAYBACK_STATE_FB) {
          //do nothing when ff fb
          DVR_PB_INFO("unlock now is ff fb, not to update cur segment info\r\n");
          dvr_mutex_unlock(&player->lock);
          return 0;
        }
        memcpy(&player->cur_segment.pids, p_pids, sizeof(DVR_PlaybackPids_t));
      }
      //save pids info
      DVR_PB_INFO(":apid :%d %d", segment->pids.audio.pid, p_pids->audio.pid);
      memcpy(&segment->pids, p_pids, sizeof(DVR_PlaybackPids_t));
      DVR_PB_INFO(":cp apid :%d %d", segment->pids.audio.pid, p_pids->audio.pid);
      break;
    }
  }
  DVR_PB_DEBUG("unlock");
  dvr_mutex_unlock(&player->lock);
  return DVR_SUCCESS;
}

/**\brief dvr play back update segment pids
 * if updated segment is ongoing segment, we need start new
 * add pid stream and stop remove pid stream.
 * \param[in] handle playback handle
 * \param[in] segment_id need updated pids segment id
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_update_segment_pids(DVR_PlaybackHandle_t handle, uint64_t segment_id, DVR_PlaybackPids_t *p_pids) {
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }

  DVR_PlaybackSegmentInfo_t *segment;
  DVR_PB_DEBUG("lock");
  dvr_mutex_lock(&player->lock);
  DVR_PB_INFO("get lock update segment id: %lld cur id %lld", segment_id, player->cur_segment_id);

  // This error is suppressed as the macro code is picked from kernel.
  // prefetch() here incurring self_assign is used to avoid some compiling
  // warnings.
  // coverity[self_assign]
  list_for_each_entry(segment, &player->segment_list, head)
  {
    if (segment->segment_id == segment_id) {

      if (player->cur_segment_id == segment_id) {
        if (player->cmd.state == DVR_PLAYBACK_STATE_FF
          || player->cmd.state == DVR_PLAYBACK_STATE_FB) {
          //do nothing when ff fb
          DVR_PB_INFO("unlock now is ff fb, not to update cur segment info\r\n");
          dvr_mutex_unlock(&player->lock);
          return 0;
        }

        //if segment is on going segment,we need stop start stream
        if (player->cmd.state == DVR_PLAYBACK_STATE_START) {
          DVR_PB_DEBUG("unlock ---\r\n");
          dvr_mutex_unlock(&player->lock);
          if (segment->pids.audio.pid != p_pids->audio.pid &&
             segment->pids.audio.pid == 0x1fff) {
               //not used this to seek to start pos.we will
               //add update only api. if need seek to start
               //pos, we will call only update api and used seek api
               //to start and stop av codec
            if (0 && player->need_seek_start == DVR_TRUE) {
                player->need_seek_start = DVR_FALSE;
                pthread_mutex_lock(&player->segment_lock);
                player->drop_ts = DVR_TRUE;
                player->ts_cache_len = 0;
                if (player->first_start_time > 0)
                  player->first_start_time = player->first_start_time - 1;
                segment_seek(player->segment_handle, (uint64_t)(player->first_start_time), player->openParams.block_size);
                DVR_PB_ERROR("unlock segment update need seek time_offset %llu [0x%x][0x%x]", player->first_start_time, segment->pids.audio.pid, segment->pids.ad.pid);
                pthread_mutex_unlock(&player->segment_lock);
            }
          }
          //check video pids, stop or restart
          _do_handle_pid_update((DVR_PlaybackHandle_t)player, segment->pids, *p_pids, 0);
          //check sub audio pids stop or restart
          _do_handle_pid_update((DVR_PlaybackHandle_t)player, segment->pids, *p_pids, 2);
          //check audio pids stop or restart
          _do_handle_pid_update((DVR_PlaybackHandle_t)player, segment->pids, *p_pids, 1);
          //check pcr pids stop or restart
          _do_handle_pid_update((DVR_PlaybackHandle_t)player, segment->pids, *p_pids, 3);

          dvr_mutex_lock(&player->lock);
        } else if (player->cmd.state == DVR_PLAYBACK_STATE_PAUSE) {
          //if state is pause, we need process at resume api. we only record change info
            int v_cmd = DVR_PLAYBACK_CMD_NONE;
            int a_cmd = DVR_PLAYBACK_CMD_NONE;

            #define CHECK_IF_NEED_RESTART(_now, _next, _cmd, _v_cmd) \
              if (VALID_PID(_now) && VALID_PID(_next) && (_now) != (_next)) \
                (_cmd) = (_v_cmd);

            #define CHECK_IF_NEED_STOP(_now, _next, _cmd, _v_cmd) \
              if (VALID_PID(_now) && !VALID_PID(_next)) \
                (_cmd) = (_v_cmd);

            #define CHECK_IF_NEED_START(_now, _next, _cmd, _v_cmd) \
              if (!VALID_PID(_now) && VALID_PID(_next)) \
                (_cmd) = (_v_cmd);

            CHECK_IF_NEED_RESTART(segment->pids.video.pid, p_pids->video.pid, v_cmd, DVR_PLAYBACK_CMD_V_RESTART);
            CHECK_IF_NEED_STOP(segment->pids.video.pid, p_pids->video.pid, v_cmd, DVR_PLAYBACK_CMD_V_STOP);
            CHECK_IF_NEED_START(segment->pids.video.pid, p_pids->video.pid, v_cmd, DVR_PLAYBACK_CMD_V_START);

            CHECK_IF_NEED_RESTART(segment->pids.audio.pid, p_pids->audio.pid, a_cmd, DVR_PLAYBACK_CMD_A_RESTART);
            CHECK_IF_NEED_STOP(segment->pids.audio.pid, p_pids->audio.pid, a_cmd, DVR_PLAYBACK_CMD_A_STOP);
            CHECK_IF_NEED_START(segment->pids.audio.pid, p_pids->audio.pid, a_cmd, DVR_PLAYBACK_CMD_A_START);

            /*process the ad, if main audio exists, but no action*/
            if (a_cmd == DVR_PLAYBACK_CMD_NONE && VALID_PID(p_pids->audio.pid)) {

              CHECK_IF_NEED_RESTART(segment->pids.ad.pid, p_pids->ad.pid, a_cmd, DVR_PLAYBACK_CMD_A_RESTART);
              CHECK_IF_NEED_STOP(segment->pids.ad.pid, p_pids->ad.pid, a_cmd, DVR_PLAYBACK_CMD_A_RESTART);
              CHECK_IF_NEED_START(segment->pids.ad.pid, p_pids->ad.pid, a_cmd, DVR_PLAYBACK_CMD_A_RESTART);

            }

            DVR_PB_INFO("%s, v_cmd[%#x] a_cmd[%#x]", __func__, v_cmd, a_cmd);

            if (player->cmd.last_cmd == DVR_PLAYBACK_CMD_PAUSE
              && player->cmd.cur_cmd != DVR_PLAYBACK_CMD_NONE) {
              /*another cmd coming in pause mode, should check and merge with last cmd*/
              player->cmd.cur_cmd |= a_cmd | v_cmd;
            } else {
              player->cmd.cur_cmd = a_cmd | v_cmd;
            }
            player->cmd.last_cmd = DVR_PLAYBACK_CMD_PAUSE;

            DVR_PB_INFO("%s, last_cmd[%#x] cur_cmd[%#x]", __func__, player->cmd.last_cmd, player->cmd.cur_cmd);
        }

        memcpy(&player->cur_segment.pids, p_pids, sizeof(DVR_PlaybackPids_t));
      }
      //save pids info
      DVR_PB_INFO(":vpid :%d -> %d", segment->pids.video.pid, p_pids->video.pid);
      DVR_PB_INFO(":apid :%d -> %d", segment->pids.audio.pid, p_pids->audio.pid);
      DVR_PB_INFO(":adpid :%d -> %d", segment->pids.ad.pid, p_pids->ad.pid);
      memcpy(&segment->pids, p_pids, sizeof(DVR_PlaybackPids_t));
      break;
    }
  }
  DVR_PB_DEBUG("unlock");
  dvr_mutex_unlock(&player->lock);
  return DVR_SUCCESS;
}
/**\brief Stop play, will stop video and audio
 * \param[in] handle playback handle
 * \param[in] clear is clear last frame
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_stop(DVR_PlaybackHandle_t handle, DVR_Bool_t clear) {
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  DVR_UNUSED(clear);
  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }
  if (player->state == DVR_PLAYBACK_STATE_STOP) {
    DVR_PB_INFO(":playback is stoped");
    return DVR_SUCCESS;
  }
  if (player->state == DVR_PLAYBACK_STATE_STOP) {
    DVR_PB_INFO(":playback is stoped");
    return DVR_SUCCESS;
  }
  _stop_playback_thread(handle);
  DVR_PB_DEBUG("lock");
  dvr_mutex_lock(&player->lock);
  DVR_PB_INFO(":get lock into stop fast");
  AmTsPlayer_stopFast(player->handle);
  if (player->has_video) {
    AmTsPlayer_resumeVideoDecoding(player->handle);
  }
  if (player->has_audio) {
    AmTsPlayer_resumeAudioDecoding(player->handle);
  }
  if (player->has_video) {
    player->has_video = DVR_FALSE;
    AmTsPlayer_hideVideo(player->handle);
    AmTsPlayer_stopVideoDecoding(player->handle);
  }
  if (player->has_audio) {
    player->has_audio = DVR_FALSE;
    AmTsPlayer_stopAudioDecoding(player->handle);
  }
  if (player->has_ad_audio) {
    player->has_ad_audio =DVR_FALSE;
    AmTsPlayer_disableADMix(player->handle);
  }

  player->cmd.last_cmd = player->cmd.cur_cmd;
  player->cmd.cur_cmd = DVR_PLAYBACK_CMD_STOP;
  player->cmd.state = DVR_PLAYBACK_STATE_STOP;
  DVR_PLAYER_CHANGE_STATE(player,DVR_PLAYBACK_STATE_STOP);
  player->cur_segment_id = UINT64_MAX;
  player->segment_is_open = DVR_FALSE;
  DVR_PB_DEBUG("unlock");
  DVR_PB_INFO("player->state %s", _dvr_playback_state_toString(player->state));
  dvr_mutex_unlock(&player->lock);
  return DVR_SUCCESS;
}
/**\brief Start play audio
 * \param[in] handle playback handle
 * \param[in] params audio playback params,contains fmt and pid...
 * \retval DVR_SUCCESS On success
 * \return Error code
 */

int dvr_playback_audio_start(DVR_PlaybackHandle_t handle, am_tsplayer_audio_params *param, am_tsplayer_audio_params *ad_param) {
  DVR_Playback_t *player = (DVR_Playback_t *) handle;

  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }
  _start_playback_thread(handle);
  //start audio and video
  DVR_PB_DEBUG("lock");
  dvr_mutex_lock(&player->lock);

  if (VALID_PID(ad_param->pid)) {
    player->has_ad_audio = DVR_TRUE;
    DVR_PB_INFO("start ad audio");
    AmTsPlayer_setADParams(player->handle, ad_param);
    AmTsPlayer_enableADMix(player->handle);
  }
  if (VALID_PID(param->pid)) {
    DVR_PB_INFO("start audio");
    player->has_audio = DVR_TRUE;
    AmTsPlayer_setAudioParams(player->handle, param);
    if (player->audio_presentation_id > -1) {
      AmTsPlayer_setParams(player->handle, AM_TSPLAYER_KEY_AUDIO_PRESENTATION_ID, &player->audio_presentation_id);
    }
    AmTsPlayer_startAudioDecoding(player->handle);
  }

  player->cmd.last_cmd = player->cmd.cur_cmd;
  player->cmd.cur_cmd = DVR_PLAYBACK_CMD_A_START;
  player->cmd.state = DVR_PLAYBACK_STATE_START;
  DVR_PLAYER_CHANGE_STATE(player,DVR_PLAYBACK_STATE_START);
  DVR_PB_DEBUG("unlock");
  dvr_mutex_unlock(&player->lock);
  return DVR_SUCCESS;
}
/**\brief Stop play audio
 * \param[in] handle playback handle
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_audio_stop(DVR_PlaybackHandle_t handle) {
  DVR_Playback_t *player = (DVR_Playback_t *) handle;

  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }

  //playback_device_audio_stop(player->handle);
  if (player->has_video == DVR_FALSE) {
    player->cmd.state = DVR_PLAYBACK_STATE_STOP;
    DVR_PLAYER_CHANGE_STATE(player,DVR_PLAYBACK_STATE_STOP);
    //destroy thread
    _stop_playback_thread(handle);
  } else {
    //do nothing.video is playing
  }
  DVR_PB_DEBUG("lock");
  dvr_mutex_lock(&player->lock);

  if (player->has_audio) {
    player->has_audio = DVR_FALSE;
    AmTsPlayer_stopAudioDecoding(player->handle);
  }

  if (player->has_ad_audio) {
    player->has_ad_audio =DVR_FALSE;
    AmTsPlayer_disableADMix(player->handle);
  }

  player->cmd.last_cmd = player->cmd.cur_cmd;
  player->cmd.cur_cmd = DVR_PLAYBACK_CMD_A_STOP;

  DVR_PB_DEBUG("unlock");
  dvr_mutex_unlock(&player->lock);
  return DVR_SUCCESS;
}
/**\brief Start play video
 * \param[in] handle playback handle
 * \param[in] params video playback params,contains fmt and pid...
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_video_start(DVR_PlaybackHandle_t handle, am_tsplayer_video_params *param) {
  DVR_Playback_t *player = (DVR_Playback_t *) handle;

  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }

  _start_playback_thread(handle);
  //start audio and video
  DVR_PB_DEBUG("lock");
  dvr_mutex_lock(&player->lock);
  player->has_video = DVR_TRUE;
  AmTsPlayer_setVideoParams(player->handle, param);
  AmTsPlayer_setVideoBlackOut(player->handle, 1);
  AmTsPlayer_startVideoDecoding(player->handle);

  //playback_device_video_start(player->handle , param);
  //if set flag is pause live, we need set trick mode
  if ((player->play_flag&DVR_PLAYBACK_STARTED_PAUSEDLIVE) == DVR_PLAYBACK_STARTED_PAUSEDLIVE) {
    DVR_PB_INFO("settrick mode at video start");
    AmTsPlayer_setTrickMode(player->handle, AV_VIDEO_TRICK_MODE_PAUSE_NEXT);
    //playback_device_trick_mode(player->handle, 1);
  }
  player->cmd.last_cmd = player->cmd.cur_cmd;
  player->cmd.cur_cmd = DVR_PLAYBACK_CMD_V_START;
  player->cmd.state = DVR_PLAYBACK_STATE_START;
  DVR_PLAYER_CHANGE_STATE(player,DVR_PLAYBACK_STATE_START);
  DVR_PB_DEBUG("unlock");
  dvr_mutex_unlock(&player->lock);
  return DVR_SUCCESS;
}
/**\brief Stop play video
 * \param[in] handle playback handle
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_video_stop(DVR_PlaybackHandle_t handle) {
  DVR_Playback_t *player = (DVR_Playback_t *) handle;

  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }

  if (player->has_audio == DVR_FALSE) {
    player->cmd.state = DVR_PLAYBACK_STATE_STOP;
    DVR_PLAYER_CHANGE_STATE(player,DVR_PLAYBACK_STATE_STOP);
    //destroy thread
    _stop_playback_thread(handle);
  } else {
    //do nothing.audio is playing
  }

  DVR_PB_DEBUG("lock");
  dvr_mutex_lock(&player->lock);

  player->has_video = DVR_FALSE;

  AmTsPlayer_stopVideoDecoding(player->handle);
  //playback_device_video_stop(player->handle);

  player->cmd.last_cmd = player->cmd.cur_cmd;
  player->cmd.cur_cmd = DVR_PLAYBACK_CMD_V_STOP;

  DVR_PB_DEBUG("unlock");
  dvr_mutex_unlock(&player->lock);
  return DVR_SUCCESS;
}
/**\brief Pause play
 * \param[in] handle playback handle
 * \param[in] flush whether its internal buffers should be flushed
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_pause(DVR_PlaybackHandle_t handle, DVR_Bool_t flush) {
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  DVR_UNUSED(flush);
  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }
  if (player->state == DVR_PLAYBACK_STATE_PAUSE ||player->state == DVR_PLAYBACK_STATE_STOP ) {
    DVR_PB_INFO("player state  is [%d] pause or stop", player->state);
    return DVR_SUCCESS;
  }
  DVR_PB_DEBUG("lock");
  dvr_mutex_lock(&player->lock);
  DVR_PB_DEBUG("get lock");
  if (player->has_video)
    AmTsPlayer_pauseVideoDecoding(player->handle);
  if (player->has_audio)
    AmTsPlayer_pauseAudioDecoding(player->handle);

  //playback_device_pause(player->handle);
  if (player->cmd.cur_cmd == DVR_PLAYBACK_CMD_FF ||
    player->cmd.cur_cmd == DVR_PLAYBACK_CMD_FB) {
    player->cmd.state = DVR_PLAYBACK_STATE_PAUSE;
    DVR_PLAYER_CHANGE_STATE(player,DVR_PLAYBACK_STATE_PAUSE);
  } else {
    player->cmd.last_cmd = player->cmd.cur_cmd;
    player->cmd.cur_cmd = DVR_PLAYBACK_CMD_PAUSE;
    player->cmd.state = DVR_PLAYBACK_STATE_PAUSE;
    DVR_PLAYER_CHANGE_STATE(player,DVR_PLAYBACK_STATE_PAUSE);
  }
  dvr_mutex_unlock(&player->lock);
  DVR_PB_DEBUG("unlock");

  return DVR_SUCCESS;
}

//not add lock
static int _dvr_cmd(DVR_PlaybackHandle_t handle, int cmd)
{
  DVR_Playback_t *player = (DVR_Playback_t *) handle;

  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }

  //get video params and audio params
  DVR_PB_DEBUG("lock");
  dvr_mutex_lock(&player->lock);
  am_tsplayer_video_params video_params;
  am_tsplayer_audio_params audio_params;
  am_tsplayer_audio_params ad_params;
  uint64_t segmentid = player->cur_segment_id;

  memset(&video_params, 0, sizeof(video_params));
  memset(&audio_params, 0, sizeof(audio_params));

  _dvr_playback_get_playinfo(handle, segmentid, &video_params, &audio_params, &ad_params);
  DVR_PB_INFO("unlock, _dvr_cmd: %#x", cmd);
  dvr_mutex_unlock(&player->lock);

  if (DVR_PLAYBACK_CMD_IS_V_RESTART(cmd) && DVR_PLAYBACK_CMD_IS_A_RESTART(cmd)) {

    DVR_PB_INFO("do_cmd av_restart");
    _dvr_playback_replay((DVR_PlaybackHandle_t)player, DVR_FALSE);

  } else if (cmd == DVR_PLAYBACK_CMD_FF || cmd == DVR_PLAYBACK_CMD_FB) {

    _dvr_playback_fffb((DVR_PlaybackHandle_t)player);

  } else if (cmd == DVR_PLAYBACK_CMD_START || cmd == DVR_PLAYBACK_CMD_STOP) {

    //nop

  } else {

    if (DVR_PLAYBACK_CMD_IS_A_STOP(cmd)) {
      DVR_PB_INFO("do_cmd a_stop");
      dvr_playback_audio_stop((DVR_PlaybackHandle_t)player);
    }

    if (DVR_PLAYBACK_CMD_IS_V_STOP(cmd)) {
      DVR_PB_INFO("do_cmd v_stop");
      dvr_playback_video_stop((DVR_PlaybackHandle_t)player);
    }

    if (DVR_PLAYBACK_CMD_IS_V_START(cmd)) {
      DVR_PB_INFO("do_cmd v_start");
      dvr_playback_video_start((DVR_PlaybackHandle_t)player, &video_params);
    }

    if (DVR_PLAYBACK_CMD_IS_A_START(cmd)) {
      DVR_PB_INFO("do_cmd a_start");
      dvr_playback_audio_start((DVR_PlaybackHandle_t)player, &audio_params, &ad_params);
    }
  }

  return DVR_SUCCESS;
}

/**\brief Resume play
 * \param[in] handle playback handle
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_resume(DVR_PlaybackHandle_t handle) {
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  uint32_t pos = 0;
  uint64_t segmentid = 0;
  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }

  if (dvr_playback_check_limit(handle)) {
    //get id and pos to check if we can seek to this pos
    DVR_PB_INFO("player start calculate time");
    dvr_playback_calculate_last_valid_segment(handle, &segmentid, &pos);
    if (segmentid != player->cur_segment_id ||
        (segmentid == player->cur_segment_id &&
        pos > _dvr_get_cur_time(handle))) {
        //first to seek new pos and to resume
        DVR_PB_INFO("seek new pos and to resume");
        dvr_playback_seek(handle, segmentid, pos);
    }
  } else {
    DVR_PB_INFO("player is not set limit");
  }

  if (player->cmd.cur_cmd == DVR_PLAYBACK_CMD_PAUSE) {
    DVR_PB_DEBUG("lock");
    dvr_mutex_lock(&player->lock);
    player->first_frame = 0;
    if (player->has_video)
          AmTsPlayer_pauseVideoDecoding(player->handle);
    if (player->has_audio)
          AmTsPlayer_pauseAudioDecoding(player->handle);

    if (player->has_video) {
      DVR_PB_INFO("dvr_playback_resume set trick mode none");
      AmTsPlayer_setTrickMode(player->handle, AV_VIDEO_TRICK_MODE_NONE);
      AmTsPlayer_resumeVideoDecoding(player->handle);
    }
    if (player->has_audio) {
      AmTsPlayer_resumeAudioDecoding(player->handle);
    }
    //check is has audio param,if has audio .we need start audio,
    //we will stop audio when ff fb, if reach end, we will pause.so we need
    //start audio when resume play

    am_tsplayer_video_params video_params;
    am_tsplayer_audio_params audio_params;
    am_tsplayer_audio_params ad_params;
    uint64_t segmentid = player->cur_segment_id;

    memset(&video_params, 0, sizeof(video_params));
    memset(&audio_params, 0, sizeof(audio_params));
    _dvr_playback_get_playinfo(handle, segmentid, &video_params, &audio_params, &ad_params);
    //valid audio pid, start audio
    if (player->has_ad_audio == DVR_FALSE && VALID_PID(ad_params.pid) && (player->cmd.speed.speed.speed == PLAYBACK_SPEED_X1)) {
      player->has_ad_audio = DVR_TRUE;
      DVR_PB_INFO("start ad audio");
      dvr_playback_change_seek_state(handle, ad_params.pid);
      AmTsPlayer_setADParams(player->handle,  &ad_params);
      AmTsPlayer_enableADMix(player->handle);
    }

    if (player->has_audio == DVR_FALSE && VALID_PID(audio_params.pid) && (player->cmd.speed.speed.speed == PLAYBACK_SPEED_X1)) {
      player->has_audio = DVR_TRUE;
      dvr_playback_change_seek_state(handle, audio_params.pid);
      AmTsPlayer_setAudioParams(player->handle, &audio_params);
      if (player->audio_presentation_id > -1) {
        AmTsPlayer_setParams(player->handle, AM_TSPLAYER_KEY_AUDIO_PRESENTATION_ID, &player->audio_presentation_id);
      }
      AmTsPlayer_startAudioDecoding(player->handle);
    } else {
      DVR_PB_INFO("audio_params.pid:%d player->has_audio:%d speed:%d", audio_params.pid, player->has_audio, player->cmd.speed.speed.speed);
    }

    if (player->cmd.cur_cmd == DVR_PLAYBACK_CMD_FF ||
      player->cmd.cur_cmd == DVR_PLAYBACK_CMD_FB) {
      player->cmd.state = DVR_PLAYBACK_STATE_START;
      DVR_PLAYER_CHANGE_STATE(player,DVR_PLAYBACK_STATE_START);
    } else {
      player->cmd.last_cmd = player->cmd.cur_cmd;
      player->cmd.cur_cmd = DVR_PLAYBACK_CMD_RESUME;
      player->cmd.state = DVR_PLAYBACK_STATE_START;
      DVR_PLAYER_CHANGE_STATE(player,DVR_PLAYBACK_STATE_START);
    }
    DVR_PB_DEBUG("unlock");
    dvr_mutex_unlock(&player->lock);
  } else if (player->state == DVR_PLAYBACK_STATE_PAUSE){
    DVR_PB_INFO("lock");
    dvr_mutex_lock(&player->lock);
    player->first_frame = 0;
    if (player->has_video)
          AmTsPlayer_pauseVideoDecoding(player->handle);
    if (player->has_audio)
          AmTsPlayer_pauseAudioDecoding(player->handle);

    if (player->has_video) {
      DVR_PB_INFO("dvr_playback_resume set trick mode none 1");
      AmTsPlayer_setTrickMode(player->handle, AV_VIDEO_TRICK_MODE_NONE);
      AmTsPlayer_resumeVideoDecoding(player->handle);
    }
    if (player->has_audio)
      AmTsPlayer_resumeAudioDecoding(player->handle);
    DVR_PB_INFO("set start state cur cmd[%d]", player->cmd.cur_cmd);
    if (player->cmd.speed.speed.speed == PLAYBACK_SPEED_X1)
      _dvr_cmd(handle, player->cmd.cur_cmd);
    player->cmd.state = DVR_PLAYBACK_STATE_START;
    DVR_PLAYER_CHANGE_STATE(player,DVR_PLAYBACK_STATE_START);
    DVR_PB_INFO("unlock");
    dvr_mutex_unlock(&player->lock);
  } else {
    if ((player->play_flag&DVR_PLAYBACK_STARTED_PAUSEDLIVE) == DVR_PLAYBACK_STARTED_PAUSEDLIVE)
    {
      DVR_PB_DEBUG("lock ---\r\n");
      dvr_mutex_lock(&player->lock);
      player->first_frame = 0;
      if (player->has_video)
          AmTsPlayer_pauseVideoDecoding(player->handle);
      if (player->has_audio)
          AmTsPlayer_pauseAudioDecoding(player->handle);
      //clear flag
      DVR_PB_INFO("clear pause live flag cur cmd[%d]", player->cmd.cur_cmd);
      player->play_flag = player->play_flag & (~DVR_PLAYBACK_STARTED_PAUSEDLIVE);
      AmTsPlayer_setTrickMode(player->handle, AV_VIDEO_TRICK_MODE_NONE);
      if (player->has_video) {
        AmTsPlayer_resumeVideoDecoding(player->handle);
      }
      if (player->has_audio)
        AmTsPlayer_resumeAudioDecoding(player->handle);
      DVR_PB_DEBUG("unlock ---\r\n");
      dvr_mutex_unlock(&player->lock);
    }
  }
  return DVR_SUCCESS;
}

static DVR_Bool_t _dvr_check_playinfo_changed(DVR_PlaybackHandle_t handle, int segment_id, int set_seg_id){

  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  DVR_PlaybackSegmentInfo_t *segment = NULL;
  DVR_PlaybackSegmentInfo_t *cur_segment = NULL;
  DVR_PlaybackSegmentInfo_t *set_segment = NULL;

  // This error is suppressed as the macro code is picked from kernel.
  // prefetch() here incurring self_assign is used to avoid some compiling
  // warnings.
  // coverity[self_assign]
  list_for_each_entry(segment, &player->segment_list, head)
  {
    if (segment->segment_id == segment_id) {
      cur_segment = segment;
    }
    if (segment->segment_id == set_seg_id) {
      set_segment = segment;
    }
    if (cur_segment != NULL && set_segment != NULL) {
      break;
    }
  }
  if (cur_segment == NULL || set_segment == NULL) {
    DVR_PB_INFO("set segment or cur segment is null");
    return DVR_TRUE;
  }
  if (cur_segment->pids.video.format != set_segment->pids.video.format ||
    cur_segment->pids.video.pid != set_segment->pids.video.pid ||
    cur_segment->pids.audio.format != set_segment->pids.audio.format ||
    cur_segment->pids.audio.pid != set_segment->pids.audio.pid) {
    DVR_PB_INFO("cur v[%d]a[%d] set v[%d]a[%d]",cur_segment->pids.video.pid,cur_segment->pids.audio.pid,set_segment->pids.video.pid,set_segment->pids.audio.pid);
    return DVR_TRUE;
  }
  DVR_PB_INFO("play info not change");
  return DVR_FALSE;
}

/**\brief set limit
 * \param[in] handle playback handle
 * \param[in] rec start time ms
 * \param[in] rec limit time ms
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_setlimit(DVR_PlaybackHandle_t handle, uint32_t time, uint32_t limit)
{  DVR_Playback_t *player = (DVR_Playback_t *) handle;

  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }
  _dvr_getClock_sec();
  DVR_PB_INFO("lock time %lu limit: %u player->state:%d", time, limit, player->state);
  dvr_mutex_lock(&player->lock);
  player->rec_start = time;
  player->limit = limit;
  DVR_PB_DEBUG("unlock ---\r\n");
  dvr_mutex_unlock(&player->lock);
  return DVR_SUCCESS;
}

/**\brief seek
 * \param[in] handle playback handle
 * \param[in] time_offset time offset base cur segment
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_seek(DVR_PlaybackHandle_t handle, uint64_t segment_id, uint32_t time_offset) {
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  int ret = DVR_SUCCESS;
  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }

  DVR_PB_INFO("lock segment_id %llu cur id %llu time_offset %u cur end: %d player->state:%d", segment_id,player->cur_segment_id, (uint32_t)time_offset, _dvr_get_end_time(handle), player->state);
  dvr_mutex_lock(&player->lock);

  DVR_Bool_t replay = _dvr_check_playinfo_changed(handle, player->cur_segment_id, segment_id);
  DVR_PB_INFO("player->state[%d]-replay[%d]--get lock-", player->state, replay);

  //open segment if id is not current segment
  ret = _dvr_open_segment(handle, segment_id);
  if (ret ==DVR_FAILURE) {
    DVR_PB_ERROR("unlock seek error at open segment");
    dvr_mutex_unlock(&player->lock);
    return DVR_FAILURE;
  }
  if (time_offset >_dvr_get_end_time(handle) &&_dvr_has_next_segmentId(handle, segment_id) == DVR_FAILURE) {
    if (segment_ongoing(player->segment_handle) == DVR_SUCCESS) {
      DVR_PB_INFO("is ongoing segment when seek end, need return success");
      time_offset = _dvr_get_end_time(handle);
    } else {
      DVR_PB_ERROR("is not ongoing segment when seek end, return failure");
      dvr_mutex_unlock(&player->lock);
      return DVR_FAILURE;
    }
  }

  DVR_PB_INFO("seek open id[%lld]flag[0x%x] time_offset %u",
  player->cur_segment.segment_id,
  player->cur_segment.flags,
  time_offset);
  //get file offset by time
  if (player->cmd.cur_cmd == DVR_PLAYBACK_CMD_FB) {
    //forward playback.not seek end of file
    if (time_offset != 0 && time_offset > FB_DEFAULT_LEFT_TIME) {
      //default -2000ms
      time_offset = time_offset -FB_DEFAULT_LEFT_TIME;
    }
  }
  // Seek can be regarded as a new playback, so keep the start segment_id for it also
  if (player->need_seek_start == DVR_TRUE) {
    player->first_start_time = (uint64_t)time_offset + 1;//set first start time not eq 0
    player->first_start_id = player->cur_segment.segment_id;
  }
  pthread_mutex_lock(&player->segment_lock);
  player->drop_ts = DVR_TRUE;
  player->ts_cache_len = 0;
  int offset = segment_seek(player->segment_handle, (uint64_t)time_offset, player->openParams.block_size);
  DVR_PB_ERROR("seek get offset by time offset, offset=%d time_offset %u",offset, time_offset);
  pthread_mutex_unlock(&player->segment_lock);
  player->offset = offset;

  _dvr_get_end_time(handle);

  player->last_send_time_id = UINT64_MAX;
  player->last_segment_total = 0LL;
  player->last_segment_id = 0LL;
  //init fffb time
  player->fffb_current = _dvr_time_getClock();
  player->fffb_start = player->fffb_current;
  player->fffb_start_pcr = _dvr_get_cur_time(handle);
  player->next_fffb_time = player->fffb_current;
  //pause state if need to replayer false
  if (player->state == DVR_PLAYBACK_STATE_STOP) {
    //only seek file,not start
    DVR_PB_DEBUG("unlock");
    dvr_mutex_unlock(&player->lock);
    return DVR_SUCCESS;
  }
  //stop play
  DVR_PB_ERROR("seek stop play, not inject data has video[%d]audio[%d]",
  player->has_video, player->has_audio);

  if (player->has_video) {
    //player->has_video = DVR_FALSE;
    AmTsPlayer_setVideoBlackOut(player->handle, 0);
    AmTsPlayer_stopVideoDecoding(player->handle);
  }


  if (player->has_audio) {
    player->has_audio =DVR_FALSE;
    AmTsPlayer_stopAudioDecoding(player->handle);
  }
  if (player->has_ad_audio) {
      player->has_ad_audio =DVR_FALSE;
      AmTsPlayer_disableADMix(player->handle);
  }

  //start play
  am_tsplayer_video_params    video_params;
  am_tsplayer_audio_params    audio_params;
  am_tsplayer_audio_params    ad_params;

  memset(&video_params, 0, sizeof(video_params));
  memset(&audio_params, 0, sizeof(audio_params));

  player->cur_segment_id = segment_id;

  int sync = DVR_PLAYBACK_SYNC;
  //get segment info and audio video pid fmt ;
  _dvr_playback_get_playinfo(handle, segment_id, &video_params, &audio_params, &ad_params);
  //start audio and video
  if (video_params.pid != player->fake_pid && !VALID_PID(video_params.pid) && !VALID_PID(audio_params.pid)) {
    //audio and video pid is all invalid, return error.
    DVR_PB_ERROR("unlock seek start dvr play back start error, not found audio and video info [0x%x]", video_params.pid);
    dvr_mutex_unlock(&player->lock);
    return -1;
  }
  DVR_PB_ERROR("seek start[0x%x]", video_params.pid);
  //add
  int v_restarted = 0;
  int a_restarted = 0;
  if (sync == DVR_PLAYBACK_SYNC) {
    if (VALID_PID(video_params.pid)) {
      //player->has_video;
      if (player->cmd.cur_cmd == DVR_PLAYBACK_CMD_PAUSE ||
        player->state == DVR_PLAYBACK_STATE_PAUSE ||
        player->speed > 2.0f||
        player->speed <= -1.0f) {
        //if is pause state. we need set trick mode.
        DVR_PB_INFO("seek set trick mode player->speed [%f]", player->speed);
        AmTsPlayer_setTrickMode(player->handle, AV_VIDEO_TRICK_MODE_PAUSE_NEXT);
      }
      DVR_PB_INFO("start video");
      AmTsPlayer_setVideoParams(player->handle, &video_params);
      AmTsPlayer_setVideoBlackOut(player->handle, 1);
      AmTsPlayer_startVideoDecoding(player->handle);
      v_restarted = 1;
      if (IS_KERNEL_SPEED(player->cmd.speed.speed.speed) &&
        player->cmd.speed.speed.speed != PLAYBACK_SPEED_X1) {
         AmTsPlayer_startFast(player->handle, (float)player->cmd.speed.speed.speed/(float)100);
      } else if (player->cmd.speed.speed.speed == PLAYBACK_SPEED_X1) {
        AmTsPlayer_stopFast(player->handle);
      }
      player->has_video = DVR_TRUE;
    } else {
      player->has_video = DVR_FALSE;
    }
    if (VALID_PID(ad_params.pid) && player->speed == 1.0) {
      player->has_ad_audio = DVR_TRUE;
      DVR_PB_INFO("start ad audio");
      dvr_playback_change_seek_state(handle, ad_params.pid);
      AmTsPlayer_setADParams(player->handle,  &ad_params);
      AmTsPlayer_enableADMix(player->handle);
    }
    if (VALID_PID(audio_params.pid) && player->speed == 1.0) {
      DVR_PB_INFO("start audio seek");
      dvr_playback_change_seek_state(handle, audio_params.pid);
      AmTsPlayer_setAudioParams(player->handle, &audio_params);
      if (player->audio_presentation_id > -1) {
        AmTsPlayer_setParams(player->handle, AM_TSPLAYER_KEY_AUDIO_PRESENTATION_ID, &player->audio_presentation_id);
      }
      AmTsPlayer_startAudioDecoding(player->handle);
      a_restarted = 1;
      player->has_audio = DVR_TRUE;
    }
#ifdef AVSYNC_USED_PCR
    if (player && VALID_PID(player->cur_segment.pids.pcr.pid)) {
      DVR_PB_INFO("start set pcr [%d]", player->cur_segment.pids.pcr.pid);
      AmTsPlayer_setPcrPid(player->handle, player->cur_segment.pids.pcr.pid);
    }
#endif
  }
  if (player->state == DVR_PLAYBACK_STATE_PAUSE) {

    if (v_restarted) {
      DVR_PLAYBACK_CMD_RESET_V(player->cmd.cur_cmd);
    }
    if (a_restarted) {
      DVR_PLAYBACK_CMD_RESET_A(player->cmd.cur_cmd);
    }

    player->cmd.state = DVR_PLAYBACK_STATE_PAUSE;
    DVR_PLAYER_CHANGE_STATE(player,DVR_PLAYBACK_STATE_PAUSE);
    if (VALID_PID(audio_params.pid) || VALID_PID(video_params.pid))
      player->seek_pause = DVR_TRUE;
    DVR_PB_INFO("set state pause in seek vpid[0x%x]apid[0x%x]",video_params.pid, audio_params.pid);
  } else if (player->cmd.cur_cmd == DVR_PLAYBACK_CMD_FF ||
    player->cmd.cur_cmd == DVR_PLAYBACK_CMD_FB ||
    player->speed > 1.0f||
    player->speed <= -1.0f) {
    DVR_PB_INFO("not set cmd to seek");
    //not pause state, we need not set cur cmd
  } else {
    DVR_PB_INFO("set cmd to seek");
    player->cmd.last_cmd = player->cmd.cur_cmd;
    player->cmd.cur_cmd = DVR_PLAYBACK_CMD_SEEK;
    player->cmd.state = DVR_PLAYBACK_STATE_START;
    DVR_PLAYER_CHANGE_STATE(player,DVR_PLAYBACK_STATE_START);
  }
  player->last_send_time_id = UINT64_MAX;
  DVR_PB_DEBUG("unlock");
  dvr_mutex_unlock(&player->lock);

  return DVR_SUCCESS;
}

// Get current playback time position of the ongoing segment.
// Notice the return value may be negative. This is because previous segment's
// data cached in demux buffer need to be considered.
static int _dvr_get_cur_time(DVR_PlaybackHandle_t handle) {
  //get cur time of segment
  DVR_Playback_t *player = (DVR_Playback_t *) handle;

  if (player == NULL || player->handle == (am_tsplayer_handle)NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }

  int64_t cache = 0;//default es buf cache 500ms
  pthread_mutex_lock(&player->segment_lock);
  loff_t pos = segment_tell_position(player->segment_handle) -player->ts_cache_len;
  uint64_t cur = 0;
  if (player->ts_cache_len > 0 && pos < 0) {
    //this case is open new segment end,but cache data is last segment.
    //we need used last segment len to send play time.
    cur = 0;
  } else {
    cur = segment_tell_position_time(player->segment_handle, pos);
  }
  AmTsPlayer_getDelayTime(player->handle, &cache);
  pthread_mutex_unlock(&player->segment_lock);
  DVR_PB_INFO("get cur time [%lld] cache:%lld cur id [%lld]last id [%lld] pb cache len [%d] [%lld]", cur, cache, player->cur_segment_id,player->last_send_time_id,  player->ts_cache_len, pos);
  if (player->state == DVR_PLAYBACK_STATE_STOP) {
    cache = 0;
  }
  int cur_time = (int)(cur > cache ? cur - cache : 0);
  return cur_time;
}

// Get current playback time position of the ongoing segment.
// Notice the return value may be negative. This is because previous segment's
// data cached in demux buffer need to be considered.
static int _dvr_get_play_cur_time(DVR_PlaybackHandle_t handle, uint64_t *id) {
  //get cur time of segment
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  DVR_RETURN_IF_FALSE(player != NULL);
  DVR_RETURN_IF_FALSE(player->segment_handle != NULL);

  pthread_mutex_lock(&player->segment_lock);
  const loff_t pos = segment_tell_position(player->segment_handle);
  const uint64_t cur = segment_tell_position_time(player->segment_handle, pos);
  pthread_mutex_unlock(&player->segment_lock);

  int cache = 0;
  get_effective_tsplayer_delay_time(player, &cache);

  if (player->state == DVR_PLAYBACK_STATE_STOP) {
    cache = 0;
  }

  int cur_time = (int)(cur - cache);
  *id = player->cur_segment_id;

  if (*id == 0 && cur_time<0) {
    cur_time = 0;
  }

  DVR_PB_INFO("***get playback slider position within segment. segment_id [%lld],"
      " segment_slider_pos[%7d ms] = segment_read_pos[%7lld ms] - tsplayer_cache_len[%5ld ms],"
      " last id [%lld] pos [%lld]",
      player->cur_segment_id,cur_time,cur,cache,player->last_send_time_id,pos);

  return cur_time;
}

//get current segment current pcr time of read pos
static int _dvr_get_end_time(DVR_PlaybackHandle_t handle) {
  //get cur time of segment
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  DVR_RETURN_IF_FALSE(player != NULL);
  DVR_RETURN_IF_FALSE(player->segment_handle != NULL);

  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }

  pthread_mutex_lock(&player->segment_lock);
  uint64_t end = segment_tell_total_time(player->segment_handle);
  pthread_mutex_unlock(&player->segment_lock);
  return (int)end;
}

DVR_Bool_t dvr_playback_check_limit(DVR_PlaybackHandle_t handle)
{
  //check is set limit info
  DVR_Playback_t *player = (DVR_Playback_t *) handle;

  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FALSE;
  }
  if (player->rec_start > 0 || player->limit > 0) {
    return DVR_TRUE;
  }
  return DVR_FALSE;
}

/**\brief set DVR playback calculate expired time len
 * \param[in] handle, DVR playback session handle
 * \return DVR_SUCCESS on success
 * \return error code on failure
 */
uint32_t dvr_playback_calculate_expiredlen(DVR_PlaybackHandle_t handle)
{
  //calculate expired time to play
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  uint32_t cur_time;
  uint32_t tmp_time;
  uint32_t expired = 0;
  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return expired;
  }
  if (player->rec_start == 0 || player->limit == 0) {
    DVR_PB_INFO("rec limit 0");
    return expired;
  }
  //get system time
  cur_time = _dvr_getClock_sec();
  if ((cur_time - player->rec_start) > player->limit) {
    tmp_time = (uint32_t)((cur_time - player->rec_start) - player->limit) * 1000U;
    expired = *(int*)&tmp_time;
    DVR_PB_INFO("cur_time:%u, rec start:%u limit:%d c_r_diff:%u expired:%u tmp_time:%u",
                cur_time,
                player->rec_start,
                player->limit,
                (uint32_t)(cur_time - player->rec_start - player->limit), expired, tmp_time);
  }
  return expired;
}

/**\brief set DVR playback obsolete time
 * \param[in] handle, DVR playback session handle
 * \param[in] obsolete, obsolete len
 * \return DVR_SUCCESS on success
 * \return error code on failure
 */
int dvr_playback_set_obsolete(DVR_PlaybackHandle_t handle, int obsolete)
{
  int expired = 0;
  //calculate expired time to play
  DVR_Playback_t *player = (DVR_Playback_t *) handle;

  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FALSE;
  }
  //get system time
  DVR_PB_DEBUG("lock ---\r\n");
  dvr_mutex_lock(&player->lock);
  player->obsolete = obsolete;
  DVR_PB_DEBUG("unlock ---\r\n");
  dvr_mutex_unlock(&player->lock);
  return expired;
}

/**\brief update DVR playback newest segment duration
 * \param[in] handle, DVR playback session handle
 * \param[in] segmentid, newest segment id
 * \param[in] dur dur time ms
 * \return DVR_SUCCESS on success
 * \return error code on failure
 */
int dvr_playback_update_duration(DVR_PlaybackHandle_t handle,
uint64_t segmentid, int dur)
{
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  DVR_PlaybackSegmentInfo_t *segment;
  DVR_PlaybackSegmentInfo_t *pre_segment = NULL;

  if (player == NULL) {
    DVR_PB_INFO(" player is NULL");
    return DVR_FAILURE;
  }
  //update the newest segment duration on timeshift mode
  // This error is suppressed as the macro code is picked from kernel.
  // prefetch() here incurring self_assign is used to avoid some compiling
  // warnings.
  // coverity[self_assign]
  list_for_each_entry(segment, &player->segment_list, head)
  {
    if (segment->segment_id == segmentid) {
      segment->duration = dur;
      break;
    }
    pre_segment = segment;
  }

  return DVR_SUCCESS;
}

static uint32_t dvr_playback_calculate_last_valid_segment(
  DVR_PlaybackHandle_t handle, uint64_t *segmentid, uint32_t *pos)
{
  uint32_t off = 0;
  uint64_t segment_id = 0;
  uint32_t pre_off = 0;
  uint64_t last_segment_id = 0;
  uint32_t expired = 0;
  DVR_Playback_t *player = (DVR_Playback_t *) handle;

  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }
  expired = dvr_playback_calculate_expiredlen(handle);
  if (expired == 0) {
    *segmentid = player->cur_segment_id;
    *pos       = 0;
    return DVR_SUCCESS;
  }
  DVR_PlaybackSegmentInfo_t *p_seg;
  // This error is suppressed as the macro code is picked from kernel.
  // prefetch() here incurring self_assign is used to avoid some compiling
  // warnings.
  // coverity[self_assign]
  list_for_each_entry_reverse(p_seg, &player->segment_list, head) {
    segment_id = p_seg->segment_id;

    if ((player->obsolete + pre_off + p_seg->duration) > expired)
        break;

    last_segment_id = p_seg->segment_id;
    pre_off += p_seg->duration;
  }

  if (last_segment_id == segment_id) {
    /*1.only one seg with id:0, 2.offset exceeds the total duration*/
    off = expired;
  } else if (player->obsolete >= expired) {
    off = 0;
  } else {
    off = expired - pre_off - player->obsolete;
  }
  *segmentid = segment_id;
  *pos       = off;
  return DVR_SUCCESS;
}

#define FB_MIX_SEEK_TIME 2000
//start replay
static int _dvr_playback_calculate_seekpos(DVR_PlaybackHandle_t handle) {

  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  //calculate pcr seek time
  int t_diff = 0;
  int seek_time = 0;
  uint64_t segmentid = 0;
  int pos = 0;
  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }

  if (player->fffb_start == -1) {
    //set fffb start time ms
    player->fffb_start = _dvr_time_getClock();
    player->fffb_current = player->fffb_start;
    //get segment current time pos
    player->fffb_start_pcr = _dvr_get_cur_time(handle);
    DVR_PB_INFO("calculate seek pos player->fffb_start_pcr[%d]ms, speed[%f]",
                  player->fffb_start_pcr, player->speed);
    //default first time 2s seek
    seek_time = FB_MIX_SEEK_TIME;
  } else {
    player->fffb_current = _dvr_time_getClock();
    t_diff = player->fffb_current - player->fffb_start;
    //if speed is < 0, cmd is fb.
    seek_time = player->fffb_start_pcr + t_diff *player->speed;
    if (seek_time <= 0) {
      //need seek to pre one segment
      seek_time = 0;
    }
    //seek segment pos
    if (player->segment_handle) {
      pthread_mutex_lock(&player->segment_lock);
      player->ts_cache_len = 0;
      if (seek_time < FB_MIX_SEEK_TIME && IS_FB(player->speed)) {
        //set seek time to 0;
        DVR_PB_INFO("segment seek to 0 at fb mode [%d]id[%lld]",
                      seek_time,
                      player->cur_segment_id);
        seek_time = 0;
      }
      if (IS_FB(player->speed)
        && dvr_playback_check_limit(handle)) {
        //fb case.check expired time
        //get id and pos to check if we can seek to this pos
        dvr_playback_calculate_last_valid_segment(handle, &segmentid, &pos);
        //case cur id < segment id
        if (player->cur_segment_id < segmentid) {
          //expired ts data is player,return error
          //
          pthread_mutex_unlock(&player->segment_lock);
          return 0;
        } else if (player->cur_segment_id == segmentid) {
          //id is same,compare seek pos
          if (seek_time < pos) {
            //expired ts data is player,return error
            //
            pthread_mutex_unlock(&player->segment_lock);
            return 0;
          }
        }
        //case can play
      }
      if (segment_seek(player->segment_handle, seek_time, player->openParams.block_size) == DVR_FAILURE) {
        seek_time = 0;
      }
      pthread_mutex_unlock(&player->segment_lock);
    } else {
      //
      DVR_PB_INFO("segment not open,can not seek");
    }
    DVR_PB_INFO("calculate seek pos seek_time[%d]ms, speed[%f]id[%lld]cur [%d]",
                  seek_time,
                  player->speed,
                  player->cur_segment_id,
                  _dvr_get_cur_time(handle));
   }
  return seek_time;
}


//start replay
static int _dvr_playback_fffb_replay(DVR_PlaybackHandle_t handle) {
  //
  DVR_Playback_t *player = (DVR_Playback_t *) handle;

  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }

  //stop
  if (player->has_video) {
    DVR_PB_INFO("fffb stop video");
    AmTsPlayer_setVideoBlackOut(player->handle, 0);
    AmTsPlayer_stopVideoDecoding(player->handle);
  }
  if (player->has_audio) {
    DVR_PB_INFO("fffb stop audio");
    player->has_audio =DVR_FALSE;
    AmTsPlayer_stopAudioDecoding(player->handle);
  }
  if (player->has_ad_audio) {
    DVR_PB_INFO("fffb stop audio");
    player->has_ad_audio =DVR_FALSE;
    AmTsPlayer_disableADMix(player->handle);
  }

  //start video and audio

  am_tsplayer_video_params    video_params;
  am_tsplayer_audio_params    audio_params;
  am_tsplayer_audio_params    ad_params;

  memset(&video_params, 0, sizeof(video_params));
  memset(&audio_params, 0, sizeof(audio_params));

  uint64_t segment_id = player->cur_segment_id;

  //get segment info and audio video pid fmt ;
  //dvr_mutex_lock(&player->lock);
  _dvr_playback_get_playinfo(handle, segment_id, &video_params, &audio_params, &ad_params);
  //start audio and video
  if (!VALID_PID(video_params.pid) && !VALID_PID(audio_params.pid)) {
    //audio and video pids are all invalid, return error.
    DVR_PB_ERROR("dvr play back restart error, not found audio and video info");
    return -1;
  }

  if (VALID_PID(video_params.pid)) {
    player->has_video = DVR_TRUE;
    DVR_PB_INFO("fffb start video");
    //DVR_PB_INFO("fffb start video and save last frame");
    //AmTsPlayer_setVideoBlackOut(player->handle, 0);
    AmTsPlayer_setTrickMode(player->handle, AV_VIDEO_TRICK_MODE_NONE);
    AmTsPlayer_setTrickMode(player->handle, AV_VIDEO_TRICK_MODE_PAUSE_NEXT);
    AmTsPlayer_setVideoParams(player->handle, &video_params);
    AmTsPlayer_setVideoBlackOut(player->handle, 1);
    AmTsPlayer_startVideoDecoding(player->handle);
    //playback_device_video_start(player->handle , &video_params);
    //if set flag is pause live, we need set trick mode
    //playback_device_trick_mode(player->handle, 1);
  }
  //fffb mode need stop fast;
  DVR_PB_INFO("stop fast");
  AmTsPlayer_stopFast(player->handle);
  return 0;
}

static int _dvr_playback_fffb(DVR_PlaybackHandle_t handle) {
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }

  player->first_frame = 0;
  DVR_PB_INFO("lock  speed [%f]", player->speed);
  dvr_mutex_lock(&player->lock);

  int seek_time = _dvr_playback_calculate_seekpos(handle);
  DVR_PB_INFO("get lock  speed [%f]id [%lld]seek_time[%d]", player->speed, player->cur_segment_id, seek_time);

  if (_dvr_has_next_segmentId(handle, player->cur_segment_id) == DVR_FAILURE && seek_time < FB_MIX_SEEK_TIME && IS_FB(player->speed)) {
      //seek time set 0
      seek_time = 0;
  }
  if (seek_time == 0) {
    //for fb cmd, we need open pre segment.if reach first one segment, send begin event
    int ret = _change_to_next_segment((DVR_PlaybackHandle_t)player);
    if (ret != DVR_SUCCESS && IS_FB(player->speed)) {

      // An fffb_replay is required here to help finish the last FB play
      // at beginning position of a recording. In addition to function
      // correctness, another benefit is that after play, demux buffer
      // is cleared due to stopVideoDecoding invocation in the process.
      // The following resume operation will not be affected by the invalid
      // cache length.
      player->next_fffb_time =_dvr_time_getClock() + FFFB_SLEEP_TIME;
      _dvr_playback_fffb_replay(handle);

      dvr_mutex_unlock(&player->lock);
      DVR_PB_DEBUG("unlock");
#ifdef FOR_SKYWORTH_FETCH_RDK
      DVR_PlaybackSpeed_t normal_speed = {PLAYBACK_SPEED_X1,0};
      DVR_PB_INFO("Change to normal speed due to FB reaching beginning");
      dvr_playback_set_speed((DVR_PlaybackHandle_t)player,normal_speed);

      {
        DVR_Play_Notify_t notify;
        memset(&notify, 0 , sizeof(DVR_Play_Notify_t));
        notify.event = DVR_PLAYBACK_EVENT_TIMESHIFT_FR_REACHED_BEGIN;
        _dvr_playback_sent_event((DVR_PlaybackHandle_t)player, notify.event, &notify, 0);
      }
#else
      DVR_PB_INFO("Change to pause due to FB reaching beginning");
      dvr_playback_pause(handle, DVR_FALSE);
#endif
      {
        //send event here and pause
        DVR_Play_Notify_t notify;
        memset(&notify, 0 , sizeof(DVR_Play_Notify_t));
        notify.event = DVR_PLAYBACK_EVENT_REACHED_BEGIN;
        //get play statue not here
        _dvr_playback_sent_event(handle, DVR_PLAYBACK_EVENT_REACHED_BEGIN, &notify, DVR_TRUE);
        DVR_PB_INFO("*******************send begin event  speed [%f] cur [%d]", player->speed, _dvr_get_cur_time(handle));
      }
      return DVR_SUCCESS;
    }
    _dvr_playback_sent_transition_ok(handle, DVR_FALSE);
    _dvr_init_fffb_time(handle);
    DVR_PB_INFO("*******************send trans ok event  speed [%f]", player->speed);
  }
  player->next_fffb_time =_dvr_time_getClock() + FFFB_SLEEP_TIME;
  _dvr_playback_fffb_replay(handle);

  dvr_mutex_unlock(&player->lock);
  DVR_PB_DEBUG("unlock");

  return DVR_SUCCESS;
}

//start replay, need get lock at extern
static int _dvr_playback_replay(DVR_PlaybackHandle_t handle, DVR_Bool_t trick) {
  //
  DVR_Playback_t *player = (DVR_Playback_t *) handle;

  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }

  //stop
  if (player->has_video) {
    player->has_video = DVR_FALSE;
    AmTsPlayer_setVideoBlackOut(player->handle, 0);
    AmTsPlayer_stopVideoDecoding(player->handle);
  }

  if (player->has_audio) {
    player->has_audio = DVR_FALSE;
    AmTsPlayer_stopAudioDecoding(player->handle);
  }
  //start video and audio

  am_tsplayer_video_params    video_params;
  am_tsplayer_audio_params    audio_params;
  am_tsplayer_audio_params    ad_params;
  uint64_t segment_id = player->cur_segment_id;

  memset(&video_params, 0, sizeof(video_params));
  memset(&audio_params, 0, sizeof(audio_params));

  //get segment info and audio video pid fmt ;
  DVR_PB_INFO("into");
  _dvr_playback_get_playinfo(handle, segment_id, &video_params, &audio_params, &ad_params);
  //start audio and video
  if (!VALID_PID(video_params.pid) && !VALID_PID(audio_params.pid)) {
    //audio and video pis is all invalid, return error.
    DVR_PB_ERROR("dvr play back restart error, not found audio and video info");
    return -1;
  }

  if (VALID_PID(video_params.pid)) {
    player->has_video = DVR_TRUE;
    if (trick == DVR_TRUE) {
      DVR_PB_INFO("settrick mode at replay");
      AmTsPlayer_setTrickMode(player->handle, AV_VIDEO_TRICK_MODE_PAUSE_NEXT);
    }
    else {
      AmTsPlayer_setTrickMode(player->handle, AV_VIDEO_TRICK_MODE_NONE);
    }
    AmTsPlayer_setVideoParams(player->handle, &video_params);
    AmTsPlayer_setVideoBlackOut(player->handle, 1);
    AmTsPlayer_startVideoDecoding(player->handle);
  }

  if (IS_FAST_SPEED(player->cmd.speed.speed.speed)) {
    DVR_PB_INFO("start fast");
    AmTsPlayer_startFast(player->handle, (float)player->cmd.speed.speed.speed/(float)100);
    player->speed = (float)player->cmd.speed.speed.speed/100.0f;
  } else {
    if (VALID_PID(ad_params.pid)) {
      player->has_ad_audio = DVR_TRUE;
      DVR_PB_INFO("start ad audio");
      AmTsPlayer_setADParams(player->handle,  &ad_params);
      AmTsPlayer_enableADMix(player->handle);
    }
    if (VALID_PID(audio_params.pid)) {
      player->has_audio = DVR_TRUE;
      DVR_PB_INFO("start audio");
      AmTsPlayer_setAudioParams(player->handle, &audio_params);
      if (player->audio_presentation_id > -1) {
        AmTsPlayer_setParams(player->handle, AM_TSPLAYER_KEY_AUDIO_PRESENTATION_ID, &player->audio_presentation_id);
      }
      AmTsPlayer_startAudioDecoding(player->handle);
    }

    DVR_PB_INFO("stop fast");
    AmTsPlayer_stopFast(player->handle);
    player->cmd.speed.speed.speed = PLAYBACK_SPEED_X1;
    player->speed = (float)PLAYBACK_SPEED_X1/100.0f;
  }
#ifdef AVSYNC_USED_PCR
  if (player && VALID_PID(player->cur_segment.pids.pcr.pid)) {
    DVR_PB_INFO("start set pcr [%d]", player->cur_segment.pids.pcr.pid);
    AmTsPlayer_setPcrPid(player->handle, player->cur_segment.pids.pcr.pid);
  }
#endif
  player->cmd.last_cmd = player->cmd.cur_cmd;
  player->cmd.cur_cmd = DVR_PLAYBACK_CMD_START;
  player->cmd.state = DVR_PLAYBACK_STATE_START;
  DVR_PLAYER_CHANGE_STATE(player,DVR_PLAYBACK_STATE_START);
  return 0;
}


/**\brief Set play speed
 * \param[in] handle playback handle
 * \param[in] speed playback speed
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_set_speed(DVR_PlaybackHandle_t handle, DVR_PlaybackSpeed_t speed) {

  DVR_Playback_t *player = (DVR_Playback_t *) handle;

  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }

  if (_dvr_support_speed(speed.speed.speed) == DVR_FALSE) {
    DVR_PB_INFO(" func: not support speed [%d]", speed.speed.speed);
    return DVR_FAILURE;
  }
  if (speed.speed.speed == player->cmd.speed.speed.speed) {
    DVR_PB_INFO(" func: eq speed [%d]", speed.speed.speed);
    return DVR_SUCCESS;
  }
  DVR_PB_INFO("lock func: speed [%d]", speed.speed.speed);

  dvr_mutex_lock(&player->lock);
  if (player->cmd.cur_cmd != DVR_PLAYBACK_CMD_FF
    && player->cmd.cur_cmd != DVR_PLAYBACK_CMD_FB) {
    player->cmd.last_cmd = player->cmd.cur_cmd;
  }

  if (player->state != DVR_PLAYBACK_STATE_PAUSE &&
    IS_KERNEL_SPEED(speed.speed.speed) ) {
      //case 1. not start play.only set speed
      if (player->state == DVR_PLAYBACK_STATE_STOP) {
        //only set speed.and return;
        player->cmd.speed.mode = DVR_PLAYBACK_KERNEL_SUPPORT;
        player->cmd.speed.speed = speed.speed;
        player->speed = (float)speed.speed.speed/(float)100;
        player->fffb_play = DVR_FALSE;
        dvr_mutex_unlock(&player->lock);
        DVR_PB_DEBUG("unlock");
        return DVR_SUCCESS;
      }
     //case 2. cur speed is 100,set 200 50 25 12 .
     //we think x1 and x2 s1/2 s 1/4 s 1/8 is normal speed. is not ff fb.
     if (IS_KERNEL_SPEED(player->cmd.speed.speed.speed)) {
      //if last speed is x2 or s2, we need stop fast
      if (speed.speed.speed == PLAYBACK_SPEED_X1) {
        // resume audio and stop fast play
        DVR_PB_INFO("stop fast");
        AmTsPlayer_stopFast(player->handle);
        dvr_mutex_unlock(&player->lock);
        DVR_PB_DEBUG("unlock ---\r\n");
        _dvr_cmd(handle, DVR_PLAYBACK_CMD_A_START);
        DVR_PB_DEBUG("lock ---\r\n");
        dvr_mutex_lock(&player->lock);
      } else {
        //set play speed and if audio is start, stop audio.
        if (player->has_audio) {
          DVR_PB_INFO("fast play stop audio");
          AmTsPlayer_stopAudioDecoding(player->handle);
          player->has_audio = DVR_FALSE;
        }
        DVR_PB_INFO("start fast");
        AmTsPlayer_startFast(player->handle, (float)speed.speed.speed/(float)100);
      }
      player->fffb_play = DVR_FALSE;
      player->cmd.speed.mode = DVR_PLAYBACK_KERNEL_SUPPORT;
      player->cmd.speed.speed = speed.speed;
      player->speed = (float)speed.speed.speed/(float)100;
      DVR_PB_DEBUG("unlock ---\r\n");
      dvr_mutex_unlock(&player->lock);
      return DVR_SUCCESS;
     }
    //case 3 fffb mode
    if (player->cmd.cur_cmd == DVR_PLAYBACK_CMD_FF ||
      player->cmd.cur_cmd == DVR_PLAYBACK_CMD_FB) {
      //restart play at normal speed exit ff fb
      DVR_PB_INFO("set speed normal and replay playback");
      player->cmd.speed.mode = DVR_PLAYBACK_KERNEL_SUPPORT;
      player->cmd.speed.speed = speed.speed;
      player->speed = (float)speed.speed.speed/(float)100;
      _dvr_playback_replay(handle, DVR_FALSE);
      player->fffb_play = DVR_FALSE;
      DVR_PB_DEBUG("unlock ---\r\n");
      dvr_mutex_unlock(&player->lock);
      return DVR_SUCCESS;
    }
  }
  else if (player->state == DVR_PLAYBACK_STATE_PAUSE &&
    IS_KERNEL_SPEED(speed.speed.speed)) {
     //case 1. cur speed is kernel support speed,set kernel speed.
    if (IS_KERNEL_SPEED(player->cmd.speed.speed.speed)) {
     //if last speed is x2 or s2, we need stop fast
     if (speed.speed.speed == PLAYBACK_SPEED_X1) {
        // resume audio and stop fast play
        DVR_PB_INFO("stop fast");
        AmTsPlayer_stopFast(player->handle);
        player->cmd.cur_cmd = DVR_PLAYBACK_CMD_A_START;
      } else {
        //set play speed and if audio is start, stop audio.
        if (player->has_audio) {
          DVR_PB_INFO("fast play stop audio at pause");
          AmTsPlayer_stopAudioDecoding(player->handle);
          player->has_audio = DVR_FALSE;
       }
       DVR_PB_INFO("start fast");
       AmTsPlayer_startFast(player->handle, (float)speed.speed.speed/(float)100);
     }
     player->cmd.speed.mode = DVR_PLAYBACK_KERNEL_SUPPORT;
     player->cmd.speed.speed = speed.speed;
     player->speed = (float)speed.speed.speed/(float)100;
     player->fffb_play = DVR_FALSE;
     DVR_PB_DEBUG("unlock ---\r\n");
     dvr_mutex_unlock(&player->lock);
     return DVR_SUCCESS;
    }
    //case 2 fffb mode
    if (player->cmd.cur_cmd == DVR_PLAYBACK_CMD_FF ||
      player->cmd.cur_cmd == DVR_PLAYBACK_CMD_FB) {
      //restart play at normal speed exit ff fb
      DVR_PB_INFO("set speed x1 s2 and replay playback");
      player->cmd.speed.mode = DVR_PLAYBACK_KERNEL_SUPPORT;
      player->cmd.speed.speed = speed.speed;
      player->speed = (float)speed.speed.speed/(float)100;
      player->cmd.cur_cmd = DVR_PLAYBACK_CMD_AV_RESTART;
      player->fffb_play = DVR_FALSE;
      DVR_PB_DEBUG("unlock ---\r\n");
      dvr_mutex_unlock(&player->lock);
      return DVR_SUCCESS;
    }
  }
  if (IS_KERNEL_SPEED(speed.speed.speed)) {
    //we think x1 and s2 s4 s8 x2is normal speed. is not ff fb.
    player->fffb_play = DVR_FALSE;
  } else {
      if ((float)speed.speed.speed > 1.0f)
        player->cmd.cur_cmd = DVR_PLAYBACK_CMD_FF;
      else
        player->cmd.cur_cmd = DVR_PLAYBACK_CMD_FB;
      player->fffb_play = DVR_TRUE;
  }
  DVR_Bool_t init_last_time = DVR_FALSE;
  if (player->speed > 0.0f && speed.speed.speed < 0) {
    init_last_time = DVR_TRUE;
  } else if (player->speed < 0.0f && speed.speed.speed > 0) {
    init_last_time = DVR_TRUE;
  }
  player->cmd.speed.mode = speed.mode;
  player->cmd.speed.speed = speed.speed;
  player->speed = (float)speed.speed.speed/(float)100;
  //reset fffb time, if change speed value
  _dvr_init_fffb_t(handle);
  if (init_last_time == DVR_TRUE)
    player->last_send_time_id = UINT64_MAX;

  if (speed.speed.speed == PLAYBACK_SPEED_X1 &&
    (player->cmd.cur_cmd == DVR_PLAYBACK_CMD_FF ||
    player->cmd.cur_cmd == DVR_PLAYBACK_CMD_FB)) {
    //restart play at normal speed exit ff fb
    DVR_PB_INFO("set speed normal and replay playback");
    _dvr_playback_replay(handle, DVR_FALSE);
  } else if (speed.speed.speed == PLAYBACK_SPEED_X1 &&
    (player->state == DVR_PLAYBACK_STATE_PAUSE)) {
    player->cmd.cur_cmd = DVR_PLAYBACK_CMD_AV_RESTART;
    DVR_PB_INFO("set speed normal at pause state ,set cur cmd");
  }
  DVR_PB_INFO("unlock  speed[%f]cmd[%d]", player->speed, player->cmd.cur_cmd);
  dvr_mutex_unlock(&player->lock);
  return DVR_SUCCESS;
}

/**\brief Get playback status
 * \param[in] handle playback handle
 * \param[out] p_status playback status
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
static int _dvr_playback_get_status(DVR_PlaybackHandle_t handle,
  DVR_PlaybackStatus_t *p_status, DVR_Bool_t is_lock) {
//
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  uint64_t   segment_id = 0LL;
  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }
  if (is_lock ==DVR_TRUE) {
    DVR_PB_DEBUG("lock");
    dvr_mutex_lock(&player->lock);
  }

  p_status->state = player->state;
  //when got first frame we will change to pause state.this only from start play to got first frame
  if ((player->play_flag&DVR_PLAYBACK_STARTED_PAUSEDLIVE) == DVR_PLAYBACK_STARTED_PAUSEDLIVE &&
    player->state == DVR_PLAYBACK_STATE_START) {
    p_status->state = DVR_PLAYBACK_STATE_PAUSE;
  }

  p_status->time_end = _dvr_get_end_time(handle);
  p_status->time_cur = _dvr_get_play_cur_time(handle, &segment_id);

  if (player->control_speed_enable == 1) {
    if (player->con_spe.ply_sta == 0) {
          DVR_PB_INFO("player dur[%d] sta[%d] cur[%d] -----reinit",
                        player->con_spe.ply_dur,
                        player->con_spe.ply_sta,
                        p_status->time_cur);
          player->con_spe.ply_sta = p_status->time_cur;
      } else if (player->speed == 1.0f && player->con_spe.ply_sta < p_status->time_cur) {
        player->con_spe.ply_dur += (p_status->time_cur - player->con_spe.ply_sta);
        DVR_PB_INFO("player dur[%d] sta[%d] cur[%d]",
                      player->con_spe.ply_dur,
                      player->con_spe.ply_sta,
                      p_status->time_cur);
        player->con_spe.ply_sta = p_status->time_cur;
      }

      if (player->con_spe.sys_sta == 0) {
          player->con_spe.sys_sta = _dvr_time_getClock();
      } else if (player->speed == 1.0f && player->con_spe.sys_sta > 0) {
        player->con_spe.sys_dur += (_dvr_time_getClock() - player->con_spe.sys_sta);
        player->con_spe.sys_sta = _dvr_time_getClock();
      }
  }

  if (player->last_send_time_id == UINT64_MAX) {
    player->last_send_time_id = player->cur_segment_id;
    player->last_cur_time = p_status->time_cur;
  }
  if (player->last_send_time_id == player->cur_segment_id) {
    if (player->speed > 1.0f ) {
      //ff
      if (p_status->time_cur < player->last_cur_time ) {
        DVR_PB_INFO("get ff time error last[%d]cur[%d]diff[%d]",
                      player->last_cur_time,
                      p_status->time_cur,
                      player->last_cur_time - p_status->time_cur);
        p_status->time_cur = player->last_cur_time;
      } else {
        player->last_cur_time = p_status->time_cur;
      }
    } else if (player->speed <= -1.0f){
      //fb
      if (p_status->time_cur > player->last_cur_time ) {
        DVR_PB_INFO("get fb time error last[%d]cur[%d]diff[%d]",
                      player->last_cur_time,
                      p_status->time_cur,
                      p_status->time_cur - player->last_cur_time );
        p_status->time_cur = player->last_cur_time;
      } else {
        player->last_cur_time = p_status->time_cur;
      }
    }
  } else {
    player->last_cur_time = p_status->time_cur;
  }
  player->last_send_time_id = segment_id;
  p_status->segment_id = segment_id;

  memcpy(&p_status->pids, &player->cur_segment.pids, sizeof(DVR_PlaybackPids_t));
  p_status->speed = player->cmd.speed.speed.speed;
  p_status->flags = player->cur_segment.flags;
  DVR_PB_INFO("player real state[%s]state[%s]cur[%d]end[%d] id[%lld]playflag[%d]speed[%f]is_lock[%d]",
                _dvr_playback_state_toString(player->state),
                _dvr_playback_state_toString(p_status->state),
                p_status->time_cur, p_status->time_end,
                p_status->segment_id,player->play_flag,
                player->speed,
                is_lock);
  if (is_lock ==DVR_TRUE) {
    DVR_PB_DEBUG("unlock ---\r\n");
    dvr_mutex_unlock(&player->lock);
  }
  return DVR_SUCCESS;
}


/**\brief Get playback status
 * \param[in] handle playback handle
 * \param[out] p_status playback status
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_get_status(DVR_PlaybackHandle_t handle,
  DVR_PlaybackStatus_t *p_status) {
//
  DVR_Playback_t *player = (DVR_Playback_t *) handle;

  _dvr_playback_get_status(handle, p_status, DVR_TRUE);

  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }
  DVR_PB_DEBUG("lock---");
  dvr_mutex_lock(&player->lock);
  if (!player->has_video && !player->has_audio)
    p_status->time_cur = 0;
  DVR_PB_DEBUG("unlock---");
  dvr_mutex_unlock(&player->lock);

  return DVR_SUCCESS;
}

void _dvr_dump_segment(DVR_PlaybackSegmentInfo_t *segment) {
  if (segment != NULL) {
    DVR_PB_INFO("segment id: %lld", segment->segment_id);
    DVR_PB_INFO("segment flag: %d", segment->flags);
    DVR_PB_INFO("segment location: [%s]", segment->location);
    DVR_PB_INFO("segment vpid: 0x%x vfmt:0x%x", segment->pids.video.pid,segment->pids.video.format);
    DVR_PB_INFO("segment apid: 0x%x afmt:0x%x", segment->pids.audio.pid,segment->pids.audio.format);
    DVR_PB_INFO("segment pcr pid: 0x%x pcr fmt:0x%x", segment->pids.pcr.pid,segment->pids.pcr.format);
    DVR_PB_INFO("segment sub apid: 0x%x sub afmt:0x%x", segment->pids.ad.pid,segment->pids.ad.format);
  }
}

int dvr_dump_segmentinfo(DVR_PlaybackHandle_t handle, uint64_t segment_id) {
  DVR_Playback_t *player = (DVR_Playback_t *) handle;

  if (player == NULL) {
    DVR_PB_INFO("player is NULL");
    return DVR_FAILURE;
  }

  DVR_PlaybackSegmentInfo_t *segment;
  // This error is suppressed as the macro code is picked from kernel.
  // prefetch() here incurring self_assign is used to avoid some compiling
  // warnings.
  // coverity[self_assign]
  list_for_each_entry(segment, &player->segment_list, head)
  {
    if (segment->segment_id == segment_id) {
      _dvr_dump_segment(segment);
      break;
    }
  }
  return 0;
}

int dvr_playback_set_decrypt_callback(DVR_PlaybackHandle_t handle, DVR_CryptoFunction_t func, void *userdata)
{
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  DVR_RETURN_IF_FALSE(player);
  DVR_RETURN_IF_FALSE(func);

  DVR_PB_INFO("in ");
  dvr_mutex_lock(&player->lock);

  player->dec_func = func;
  player->dec_userdata = userdata;

  dvr_mutex_unlock(&player->lock);
  DVR_PB_INFO("out ");
  return DVR_SUCCESS;
}

int dvr_playback_set_secure_buffer(DVR_PlaybackHandle_t handle, uint8_t *p_secure_buf, uint32_t len)
{
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  DVR_RETURN_IF_FALSE(player);
  DVR_RETURN_IF_FALSE(p_secure_buf);
  DVR_RETURN_IF_FALSE(len);

  DVR_PB_INFO("in ");
  dvr_mutex_lock(&player->lock);

  player->is_secure_mode = 1;
  player->secure_buffer = p_secure_buf;
  player->secure_buffer_size = len;

  dvr_mutex_unlock(&player->lock);
  DVR_PB_INFO("out");
  return DVR_SUCCESS;
}

int dvr_playback_set_ac4_preselection_id(DVR_PlaybackHandle_t handle, int presel_id)
{
  DVR_Playback_t *player = (DVR_Playback_t *) handle;
  DVR_RETURN_IF_FALSE(player != NULL);

  player->audio_presentation_id = presel_id;
  am_tsplayer_result ret = AmTsPlayer_setParams(player->handle,
      AM_TSPLAYER_KEY_AUDIO_PRESENTATION_ID, &presel_id);
  DVR_RETURN_IF_FALSE(ret == AM_TSPLAYER_OK);

  return DVR_SUCCESS;
}

// This function ensures a valid TsPlayer delay time is provided or else it
// returns DVR_FAILURE. It is designed to workaround a weakness of
// AmTsPlayer_getDelayTime at starting phase of a playback in a short period
// of 20ms or less. During the said period, getDelayTime does NOT work as
// expect to return real delay length because demux isn't actually running
// to provide valid pts to TsPlayer.
static int get_effective_tsplayer_delay_time(DVR_Playback_t* play, int *time)
{
  int64_t delay=0;
  uint64_t pts_a=0;
  uint64_t pts_v=0;

  DVR_RETURN_IF_FALSE(play != NULL);
  DVR_RETURN_IF_FALSE(play->handle != NULL);

  AmTsPlayer_getDelayTime(play->handle, &delay);
  // In scambled stream situation, the returned TsPlayer delay time is
  // invalid and dirty. An additional time check agaginst 15 minutes (900s)
  // is introduced to insure such error condition is handled properly.
  DVR_RETURN_IF_FALSE((delay >= 0) && (delay <= 900*1000));

  if (play->delay_is_effective) {
    *time = (int)delay;
    return DVR_SUCCESS;
  } else if (delay > 0) {
    *time = (int)delay;
    play->delay_is_effective=DVR_TRUE;
    return DVR_SUCCESS;
  }

  AmTsPlayer_getPts(play->handle, TS_STREAM_AUDIO, &pts_a);
  AmTsPlayer_getPts(play->handle, TS_STREAM_VIDEO, &pts_v);
  if ((int64_t)pts_a > 0 || (int64_t)pts_v > 0) {
    *time = (int)delay;
    play->delay_is_effective=DVR_TRUE;
    return DVR_SUCCESS;
  }

  return DVR_FAILURE;
}

