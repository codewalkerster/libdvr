/**
 * \file
 * \brief Playback module.
 */

#ifndef DVR_PLAYBACK_H_
#define DVR_PLAYBACK_H_
#include "list.h"
#include "dvr_types.h"
#include "segment.h"
#include "AmTsPlayer.h"
#include "dvr_types.h"
#include "dvr_crypto.h"
#include "dvr_mutex.h"

#ifdef __cplusplus
extern "C" {
#endif

/**\brief dvr play segment flag */
typedef enum
{
  DVR_PLAYBACK_SEGMENT_ENCRYPTED   = (1 << 0), /**< encrypted stream */
  DVR_PLAYBACK_SEGMENT_DISPLAYABLE = (1 << 1), /**< displayable stream */
  DVR_PLAYBACK_SEGMENT_CONTINUOUS  = (1 << 2)  /**< continuous stream with pre one */
} DVR_PlaybackSegmentFlag_t;

/**\brief dvr pid type*/
typedef enum
{
  DVR_PLAYBACK_SYNC,    /**< sync play mode */
  DVR_PLAYBACK_ASYNC    /**< out of sync play mode */
} DVR_PlaybackSyncMode_t;


/**\brief dvr play pids */
typedef struct
{
  DVR_StreamInfo_t video;    /**< Video.*/
  DVR_StreamInfo_t audio;    /**< Audio.*/
  DVR_StreamInfo_t ad;       /**< AD.*/
  DVR_StreamInfo_t subtitle; /**< Subtitle.*/
  DVR_StreamInfo_t pcr;      /**< PCR.*/
} DVR_PlaybackPids_t;

/**\brief dvr play pids */
typedef struct
{
  DVR_PlaybackPids_t pids;    /**< Video.*/
  int av_scale;
} DVR_Playback_info_t;

/**\brief dvr segments info */
typedef struct
{
  struct  list_head head;         /**< Segment node.*/
  uint64_t     segment_id;        /**< Segment's index.*/
  char   location[DVR_MAX_LOCATION_SIZE];  /**< chunk location */
  DVR_PlaybackPids_t  pids;       /**< Streams' PIDs.*/
  DVR_PlaybackSegmentFlag_t flags; /**< Segment's flag */
  int key_data_id;                /**< ??? */
  int duration;                        /**< Segment dur time ms*/
} DVR_PlaybackSegmentInfo_t;

/**\brief play flag, if set this flag, player need pause when decode first frame */
typedef enum
{
  DVR_PLAYBACK_STARTED_PAUSEDLIVE = (1 << 0) /**< dvr play stat,need change to pause state if set */
} DVR_PlaybackFlag_t;

/**\brief playback speed mode*/
typedef enum
{
  DVR_PLAYBACK_FAST_FORWARD = 0,        /**< fast forward */
  DVR_PLAYBACK_FAST_BACKWARD = 1,       /**< fast backward */
  DVR_PLAYBACK_KERNEL_SUPPORT = 2,      /**< kernel support mode */
} DVR_PlaybackSpeedMode_t;

/**\brief playback speed*/
typedef enum
{
  PLAYBACK_SPEED_FBX1 = -100,          /**<FB X 1 speed*/
  PLAYBACK_SPEED_FBX2 = -200,          /**<FB X 2 speed*/
  PLAYBACK_SPEED_FBX4 = -400,          /**<FB X 4 speed*/
  PLAYBACK_SPEED_FBX8 = -800,          /**<FB X 8 speed*/
  PLAYBACK_SPEED_FBX12 = -1200,        /**< FB 12 speed*/
  PLAYBACK_SPEED_FBX16 = -1600,          /**<FB X 16 speed*/
  PLAYBACK_SPEED_FBX32 = -3200,          /**<FB X 32 speed*/
  PLAYBACK_SPEED_FBX48 = -4800,          /**<FB X 48 speed*/
  PLAYBACK_SPEED_FBX64 = -6400,          /**<FB X 64 speed*/
  PLAYBACK_SPEED_FBX128 = -12800,         /**<FB X 128 speed*/
  PLAYBACK_SPEED_S8 = 12,          /**<slow 1/8 speed*/
  PLAYBACK_SPEED_S4 = 25,          /**<slow 1/4 speed*/
  PLAYBACK_SPEED_S2 = 50,          /**<slow 1/2 speed*/
  PLAYBACK_SPEED_X1 = 100,          /**< X 1 normal speed*/
  PLAYBACK_SPEED_X2 = 200,          /**< X 2 speed*/
  PLAYBACK_SPEED_X3 = 300,          /**< X 3 speed*/
  PLAYBACK_SPEED_X4 = 400,          /**< X 4 speed*/
  PLAYBACK_SPEED_X5 = 500,          /**< X 5 speed*/
  PLAYBACK_SPEED_X6 = 600,          /**< X 6 speed*/
  PLAYBACK_SPEED_X7 = 700,          /**< X 7 speed*/
  PLAYBACK_SPEED_X8 = 800,          /**< X 8 speed*/
  PLAYBACK_SPEED_X12 = 1200,        /**< X 12 speed*/
  PLAYBACK_SPEED_X16 = 1600,        /**< X 16 speed*/
  PLAYBACK_SPEED_X32 = 3200,        /**< X 32 speed*/
  PLAYBACK_SPEED_X48 = 4800,          /**< X 48 speed*/
  PLAYBACK_SPEED_X64 = 6400,          /**< X 64 speed*/
  PLAYBACK_SPEED_X128 = 12800,          /**< X 128 speed*/
  PlayBack_Speed_MAX,
} Playback_SpeedValue_t;

/**\brief playback speed*/
typedef struct Playback_Speeds_s {
  int speed; /**< playback speed value*/
} Playback_Speeds_t;

/**\brief playback play speed*/
typedef struct
{
  Playback_Speeds_t speed;  /**< playback speed */
  DVR_PlaybackSpeedMode_t mode;   /**< playback 0: fast forword or 1: fast backword*/
} DVR_PlaybackSpeed_t;


/**Maximum supported speed modes.*/
#define DVR_MAX_SUPPORTED_SPEEDS  32

/**\brief playback capability*/
typedef struct
{
  int nb_supported_speeds;       /**< support playback speed count*/
  int supported_speeds[DVR_MAX_SUPPORTED_SPEEDS]; /**< support playback speed*/
} DVR_PlaybackCapability_t;

/**Playback handle.*/
typedef void* DVR_PlaybackHandle_t;


/**\brief playback error reason*/
typedef enum
{
  DVR_PLAYBACK_PID_ERROR,            /**< uninit state */
  DVR_PLAYBACK_FMT_ERROR           /**< fmt not support backword */
} DVR_PlaybackError_t;


/**\brief playback play state*/
typedef enum
{
  DVR_PLAYBACK_STATE_START,       /**< start play */
  DVR_PLAYBACK_STATE_STOP,        /**< stop */
  DVR_PLAYBACK_STATE_PAUSE,       /**< pause */
  DVR_PLAYBACK_STATE_FF,          /**< fast forward */
  DVR_PLAYBACK_STATE_FB           /**< fast backword */
} DVR_PlaybackPlayState_t;

/**\brief playback play status*/
typedef struct
{
  DVR_PlaybackPlayState_t state;  /**< playback play state */
  uint64_t segment_id;                 /**< playback ongoing segment index */
  int32_t time_cur;               /**< current playback time position in ms of a segment.
                                   It can be a negative value to refer to relative time
                                   position of previous segment*/
  int32_t time_end;               /**< end time position of a segment in ms */
  DVR_PlaybackPids_t pids;        /**< playback played pids */
  int                    speed;  /**< playback speed */
  DVR_PlaybackSegmentFlag_t flags; /**< playback played segment flag */
} DVR_PlaybackStatus_t;

/**\brief DVR playback vendor*/
typedef enum {
  DVR_PLAYBACK_VENDOR_DEF,                    /**< default, for Irdeto*/
  DVR_PLAYBACK_VENDOR_AML,                    /**< aml*/
  DVR_PLAYBACK_VENDOR_AMAZON                  /**< amazon*/
} DVR_PlaybackVendor_t;


/**\brief DVR playback event*/
typedef enum {
  DVR_PLAYBACK_EVENT_ERROR              = 0x1000,   /**< Signal a critical playback error*/
  DVR_PLAYBACK_EVENT_TRANSITION_OK    ,             /**< transition ok*/
  DVR_PLAYBACK_EVENT_TRANSITION_FAILED,             /**< transition failed*/
  DVR_PLAYBACK_EVENT_KEY_FAILURE,                   /**< key failure*/
  DVR_PLAYBACK_EVENT_NO_KEY,                        /**< no key*/
  DVR_PLAYBACK_EVENT_REACHED_BEGIN     ,            /**< reached begin*/
  DVR_PLAYBACK_EVENT_REACHED_END,                    /**< reached end*/
  DVR_PLAYBACK_EVENT_NOTIFY_PLAYTIME,               /**< notify play cur segment time ms*/
  DVR_PLAYBACK_EVENT_FIRST_FRAME,                   /**< first frame*/
  DVR_PLAYBACK_EVENT_NODATA,                        /**< no data*/
  DVR_PLAYBACK_EVENT_DATARESUME,                     /**< data resume*/
  DVR_PLAYBACK_EVENT_TIMESHIFT_FR_REACHED_BEGIN,
  DVR_PLAYBACK_EVENT_TIMESHIFT_FF_REACHED_END,
} DVR_PlaybackEvent_t;

/**\brief DVR playback event notify function*/
typedef struct
{
  DVR_PlaybackEvent_t  event;       /**< event type*/
  DVR_PlaybackStatus_t play_status; /**< play status*/
  union
  {
    uint8_t            unused;
    uint8_t            error_reason;/**< error reason*/
    struct
    {
      uint64_t         segment_id;  /**< error segment id*/
      uint32_t         key_data_id; /**< key data id*/
      uint8_t          error;       /**< error*/
    } transition_failed_data;
  } info;                           /**< information*/
} DVR_Play_Notify_t;

/**\brief DVR playback event notify function*/
typedef DVR_Result_t (*DVR_PlaybackEventFunction_t) (DVR_PlaybackEvent_t event, void *params, void *userdata);


/**\brief playback open params*/
typedef struct
{
  int                    dmx_dev_id;      /**< playback used dmx device index*/
  int                    block_size;      /**< playback inject block size*/
  DVR_Bool_t             is_timeshift;    /**< 0:playback mode, 1 : is timeshift mode*/
  am_tsplayer_handle     player_handle;   /**< am tsplayer handle.*/
  DVR_CryptoFunction_t   crypto_fn;       /**< Crypto function.*/
  void                   *crypto_data;    /**< Crypto function's user data.*/
  uint8_t                *clearkey;       /**< key for encrypted PVR on FTA.*/
  uint8_t                *cleariv;        /**< iv for encrypted PVR on FTA.*/
  uint32_t               keylen;          /**< key/iv length.*/
  DVR_Bool_t             has_pids;        /**< has video audo pid fmt info*/
  DVR_PlaybackEventFunction_t  event_fn;           /**< playback event callback function*/
  void                        *event_userdata;    /**< event userdata*/
  DVR_PlaybackVendor_t         vendor;    /**< vendor type,default is 0*/
  DVR_Bool_t                 is_notify_time;  /**< notify play time info true or not*/
  DVR_Bool_t                 control_speed_enable;  /**< 1: system clock, 0: libdvr can determine index time source based on actual situation*/
} DVR_PlaybackOpenParams_t;

/**\brief playback play state*/
typedef enum
{
  DVR_PLAYBACK_CMD_START,               /**< start av */
  DVR_PLAYBACK_CMD_STOP,                /**< stop av */
  DVR_PLAYBACK_CMD_V_START,              /**< v start */
  DVR_PLAYBACK_CMD_A_START   ,           /**< a start */
  DVR_PLAYBACK_CMD_V_STOP  ,             /**< v stop */
  DVR_PLAYBACK_CMD_A_STOP,               /**< a stop */
  DVR_PLAYBACK_CMD_V_RESTART,            /**< v restart */
  DVR_PLAYBACK_CMD_A_RESTART,            /**< a restart */
  DVR_PLAYBACK_CMD_AV_RESTART,            /**< av restart */
  DVR_PLAYBACK_CMD_V_STOP_A_START,         /**< v stop a start*/
  DVR_PLAYBACK_CMD_A_STOP_V_START,         /**< a stop v_start */
  DVR_PLAYBACK_CMD_V_STOP_A_RESTART,       /**<v stop a restart*/
  DVR_PLAYBACK_CMD_A_STOP_V_RESTART,       /**<a stop v restart*/
  DVR_PLAYBACK_CMD_V_START_A_RESTART,       /**<v start a restart*/
  DVR_PLAYBACK_CMD_A_START_V_RESTART,       /**<a start v restart*/
  DVR_PLAYBACK_CMD_PAUSE,               /**< pause */
  DVR_PLAYBACK_CMD_RESUME,              /**< resume */
  DVR_PLAYBACK_CMD_SEEK,                /**< seek */
  DVR_PLAYBACK_CMD_FF,                  /**< fast forward */
  DVR_PLAYBACK_CMD_FB,                  /**< fast backword */
  DVR_PLAYBACK_CMD_NONE,                  /**< none */
} DVR_PlaybackCmd_t;


/**\brief playback struct*/
typedef struct
{
  DVR_PlaybackSpeed_t       speed;     /**< play speed */
  DVR_PlaybackPlayState_t   state;     /**< play state */
  DVR_PlaybackCmd_t         cur_cmd;    /**< cur send cmd */
  DVR_PlaybackCmd_t         last_cmd;   /**< last cmd */
  int                       pos;        /**< seek pos at cur segment*/
} DVR_PlaybackCmdInfo_t;

/**\brief playback struct*/
typedef struct
{
  uint32_t       sys_dur;     /**< system duration */
  uint32_t       sys_sta;     /**< system start time */
  int32_t       ply_dur;     /**< play duration */
  int32_t       ply_sta;     /**< play start time */
} DVR_PlaybackConSpe_t;


/**\brief DVR playback decrypt function*/
typedef DVR_Result_t (*DVR_PlaybackDecryptFunction_t) (uint8_t *p_in,
    uint32_t in_len,
    uint8_t *p_out,
    uint32_t *p_out_len,
    void *userdata);

/**\cond */
/**\brief playback struct*/
typedef struct
{
  am_tsplayer_handle         handle;             /**< tsplayer handle */
  DVR_Bool_t                 segment_is_open;  /**<segment is opend*/
  uint64_t                   cur_segment_id;        /**< Current segment id*/
  DVR_PlaybackSegmentInfo_t  cur_segment;          /**< Current playing segment*/
  uint64_t                   last_segment_id;        /**< last segment id*/
  DVR_PlaybackSegmentInfo_t  last_segment;          /**< last playing segment*/
  struct list_head           segment_list;         /**< segment list head*/
  pthread_t                  playback_thread;    /**< playback thread*/
  dvr_mutex_t                lock;               /**< playback lock*/
  pthread_mutex_t            segment_lock;      /**< playback segment lock*/
  pthread_cond_t             cond;               /**< playback cond*/
  void                       *user_data;         /**< playback userdata, used to send event*/
  float                      speed;           /**< playback speed*/
  DVR_PlaybackPlayState_t    state;           /**< playback state*/
  DVR_PlaybackFlag_t         play_flag;           /**< playback play flag*/
  int                        is_running;           /**< playback htread is runing*/
  DVR_PlaybackCmdInfo_t      cmd;           /**< playback cmd*/
  int                        offset;         /**< segment read offset*/
  uint32_t                   dur;         /**< segment dur*/
  Segment_Handle_t           segment_handle;           /**< playback current segment handle*/
  DVR_PlaybackOpenParams_t   openParams;           /**< playback openParams*/
  DVR_Bool_t                 has_video;    /**< has video playing*/
  DVR_Bool_t                 has_audio;    /**< has audio playing*/
  DVR_Bool_t                 has_ad_audio;    /**< has ad audio playing*/
  DVR_Bool_t                 has_pids;     /**< has video audo pid fmt info*/
  uint32_t                   fffb_start;    /**< fffb start time ms*/
  uint32_t                   fffb_current;  /**< fffb current time*/
  int                        fffb_start_pcr;     /**< fffb start pcr time*/
  uint32_t                   next_fffb_time;/**< fffb start pcr time*/
  int                        seek_time;/**< fffb start pcr time*/
  event_callback             player_callback_func;/**< tsplayer cb*/
  void                       *player_callback_userdata;/**< tsplayer cb data*/
  uint32_t                   send_time;/**< send event time*/
  int                        first_frame;/**< show first frame*/
  DVR_CryptoFunction_t       dec_func;                             /**< Decrypt function*/
  void                       *dec_userdata;                        /**< Decrypt userdata*/
  void                       *cryptor;                             /**< Cryptor for encrypted PVR on FTA.*/
  int                        is_secure_mode;                       /**< Playback session run in secure pipeline */
  uint8_t                    *secure_buffer;                       /* Playback session secure buffer */
  uint32_t                   secure_buffer_size;                   /* Playback session secure buffer size */
  DVR_Bool_t                 drop_ts;
  DVR_Bool_t                 fffb_play;
  int32_t                    last_cur_time;
  uint64_t                   last_send_time_id;
  int                        ts_cache_len;
  DVR_Bool_t                 first_trans_ok;
  DVR_PlaybackVendor_t       vendor;
  int                        noData;
  DVR_Bool_t                 seek_pause;                /**<set true when user call seek at pause state,we will start inject data, if first frame is got,set false and stop inject data*/
  int                        last_segment_total;        /**< last segment total time*/

  DVR_PlaybackConSpe_t       con_spe;   /**< inject data speed info*/

  //limit info
  int                        obsolete;         /**< rec obsolete time in ms*/
  uint32_t                   rec_start;        /**< rec start time in ms*/
  uint32_t                       limit;            /**< rec data limit time in ms*/
  //first play need seek to start time
  uint32_t                   first_start_time;
  //The segment id where a playback is initially started
  uint64_t                   first_start_id;

  // Tells whether the delay from AmTsPlayer_getDelayTime is valid.
  // Notice it can be invalid in a short period at starting phase of a playback.
  DVR_Bool_t                 delay_is_effective;

  DVR_Bool_t                 need_seek_start;
  //init fake pid
  int                         fake_pid;
  //Audio presentation id, used for dolby AC4 audio
  int32_t                     audio_presentation_id;

  /**< 1: system clock, 0: libdvr can determine index time source based on actual situation*/
  DVR_Bool_t                 control_speed_enable;
} DVR_Playback_t;
/**\endcond*/

/**\brief Open an dvr palyback
 * \param[out] p_handle dvr playback addr
 * \param[in] params dvr playback open parameters
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_open(DVR_PlaybackHandle_t *p_handle, DVR_PlaybackOpenParams_t *params);

/**\brief Close an dvr palyback
 * \param[in] handle playback handle
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_close(DVR_PlaybackHandle_t handle);


/**\brief Start play audio and video, used start audio api and start video api
 * \param[in] handle playback handle
 * \param[in] flag playback flag
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_start(DVR_PlaybackHandle_t handle, DVR_PlaybackFlag_t flag);

/**\brief dvr play back add segment info to segment list
 * \param[in] handle playback handle
 * \param[in] info added segment info,con vpid fmt apid fmt.....
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_add_segment(DVR_PlaybackHandle_t handle, DVR_PlaybackSegmentInfo_t *info);

/**\brief dvr play back remove segment info by segmentkid
 * \param[in] handle playback handle
 * \param[in] segmentid need removed segment id
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_remove_segment(DVR_PlaybackHandle_t handle, uint64_t segmentid);

/**\brief dvr play back add segment info
 * \param[in] handle playback handle
 * \param[in] segment_id the segment's index
 * \param[in] flags the segment's flags
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_update_segment_flags(DVR_PlaybackHandle_t handle,
    uint64_t segment_id,
    DVR_PlaybackSegmentFlag_t flags);

/**\brief dvr play back up1date segment pids
 * if updated segment is ongoing segment, we need start new
 * add pid stream and stop remove pid stream.
 * \param[in] handle playback handle
 * \param[in] segment_id need updated pids segment id
 * \param[in] p_pids the new PIDs
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_update_segment_pids(DVR_PlaybackHandle_t handle, uint64_t segment_id,
DVR_PlaybackPids_t *p_pids);

/**\brief dvr play back only up1date segment pids
 * only update segment info, not stop start codec.
 * \param[in] handle playback handle
 * \param[in] segment_id need updated pids segment id
 * \param[in] p_pids the new PIDs
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_only_update_segment_pids(DVR_PlaybackHandle_t handle, uint64_t segment_id, DVR_PlaybackPids_t *p_pids);

/**\brief Stop playing, will stop video and audio
 * \param[in] handle playback handle
 * \param[in] clear is clear last frame
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_stop(DVR_PlaybackHandle_t handle, DVR_Bool_t clear);

/**\brief Start audio playing
 * \param[in] handle playback handle
 * \param[in] params audio playback params,contains fmt and pid...
 * \param[in] ad_params ad audio playback params,contains fmt and pid...
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_audio_start(DVR_PlaybackHandle_t handle, am_tsplayer_audio_params *param, am_tsplayer_audio_params *ad_param);


/**\brief Stop audio playing
 * \param[in] handle playback handle
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_audio_stop(DVR_PlaybackHandle_t handle);

/**\brief Start video playing
 * \param[in] handle playback handle
 * \param[in] params video playback params,contains fmt and pid...
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_video_start(DVR_PlaybackHandle_t handle, am_tsplayer_video_params *params);

/**\brief Stop play video
 * \param[in] handle playback handle
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_video_stop(DVR_PlaybackHandle_t handle);

/**\brief Pause play
 * \param[in] handle playback handle
 * \param[in] flush whether its internal buffers should be flushed
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_pause(DVR_PlaybackHandle_t handle, DVR_Bool_t flush);

/**\brief resume play
 * \param[in] handle playback handle
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_resume(DVR_PlaybackHandle_t handle);

/**\brief set limit
 * \param[in] handle playback handle
 * \param[in] rec start time ms
 * \param[in] rec limit time ms
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_setlimit(DVR_PlaybackHandle_t handle, uint32_t time, uint32_t limit);

/**\brief Seek the playing position
 * \param[in] handle playback handle
 * \param[in] segment_id the segment's index
 * \param[in] time_offset time offset base cur segment
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_seek(DVR_PlaybackHandle_t handle, uint64_t segment_id, uint32_t time_offset);

/**\brief Set play speed
 * \param[in] handle playback handle
 * \param[in] speed playback speed
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_set_speed(DVR_PlaybackHandle_t handle, DVR_PlaybackSpeed_t speed);

/**\brief Get playback status
 * \param[in] handle playback handle
 * \param[out] p_status playback status
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_get_status(DVR_PlaybackHandle_t handle, DVR_PlaybackStatus_t *p_status);

/**\brief Get playback capabilities
 * \param[out] p_capability playback capability
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_playback_get_capabilities(DVR_PlaybackCapability_t *p_capability);

/**\brief dump segmentinfo throw print log
 * \param[in] handle playback
 * \param[in] segment_id if segment_id > 0, only dump this log. else dump all segment info
 * \retval DVR_SUCCESS On success
 * \return Error code
 */
int dvr_dump_segmentinfo(DVR_PlaybackHandle_t handle, uint64_t segment_id);

/**\brief Set DVR playback decrypt function
 * \param[in] handle, DVR playback session handle
 * \param[in] func, DVR playback encrypt function
 * \param[in] userdata, DVR playback userdata from the caller
 * \return DVR_SUCCESS on success
 * \return error code on failure
 */
int dvr_playback_set_decrypt_callback(DVR_PlaybackHandle_t handle, DVR_CryptoFunction_t func, void *userdata);

/**\brief Set DVR playback secure buffer used for protect the secure content
 * \param[in] handle, DVR playback session handle
 * \param[in] p_secure_buf, Secure buffer address which can NOT access by ACPU
 * \param[in] len, Secure buffer length
 * \return DVR_SUCCESS on success
 * \return error code on failure
 */
int dvr_playback_set_secure_buffer(DVR_PlaybackHandle_t handle, uint8_t *p_secure_buf, uint32_t len);

/**\brief set DVR playback calculate expired time len
 * \param[in] handle, DVR playback session handle
 * \return DVR_SUCCESS on success
 * \return error code on failure
 */
uint32_t dvr_playback_calculate_expiredlen(DVR_PlaybackHandle_t handle);

/**\brief set DVR playback obsolete time
 * \param[in] handle, DVR playback session handle
 * \param[in] obsolete, obsolete len
 * \return DVR_SUCCESS on success
 * \return error code on failure
 */
int dvr_playback_set_obsolete(DVR_PlaybackHandle_t handle, int obsolete);

/**\brief update DVR playback newest segment duration
 * \param[in] handle, DVR playback session handle
 * \param[in] segmentid, newest segment id
 * \param[in] dur dur time ms
 * \return DVR_SUCCESS on success
 * \return error code on failure
 */
int dvr_playback_update_duration(DVR_PlaybackHandle_t handle,
uint64_t segmentid, int dur);

/**\brief check DVR playback is set limit info
 * \param[in] handle, DVR playback session handle
 * \return DVR_FALSE or DVR_TRUE
 */
DVR_Bool_t dvr_playback_check_limit(DVR_PlaybackHandle_t handle);

/**\brief set ac4 audio preselection id for playback
 * \param[in] handle, DVR playback session handle
 * \param[in] presel_id, ac4 preselection id
 * \return DVR_SUCCESS on success
 * \return error code on failure
 */
int dvr_playback_set_ac4_preselection_id(DVR_PlaybackHandle_t handle, int presel_id);

#ifdef __cplusplus
}
#endif

#endif /*END DVR_PLAYBACK_H_*/
