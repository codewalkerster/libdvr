/**
 * \file
 * \brief Record module
 */

#ifndef _DVR_RECORD_H_
#define _DVR_RECORD_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "dvr_types.h"
#include "dvr_crypto.h"

/**\brief DVR record handle*/
typedef void* DVR_RecordHandle_t;

/**\brief DVR record state*/
typedef enum {
  DVR_RECORD_STATE_OPENED,        /**< Record state is opened*/
  DVR_RECORD_STATE_STARTED,       /**< Record state is started*/
  DVR_RECORD_STATE_STOPPED,       /**< Record state is stopped*/
  DVR_RECORD_STATE_CLOSED,        /**< Record state is closed*/
  DVR_RECORD_STATE_PAUSE,        /**< Record state is pause*/
} DVR_RecordState_t;

/**\brief DVR record pid action*/
typedef enum
{
  DVR_RECORD_PID_CREATE,          /**< Create a new pid used to record*/
  DVR_RECORD_PID_KEEP,            /**< Indicate this pid keep last state*/
  DVR_RECORD_PID_CLOSE            /**< Close this pid record*/
} DVR_RecordPidAction_t;

/**\brief DVR record flag*/
typedef enum {
  DVR_RECORD_FLAG_SCRAMBLED = (1 << 0),
  DVR_RECORD_FLAG_ACCURATE  = (1 << 1),
  DVR_RECORD_FLAG_DATAOUT   = (1 << 2),
} DVR_RecordFlag_t;

/**\brief DVR crypto parity flag*/
typedef enum {
  DVR_CRYPTO_PARITY_CLEAR,        /**< Current period is clear*/
  DVR_CRYPTO_PARITY_ODD,          /**< Current period is ODD*/
  DVR_CRYPTO_PARITY_EVEN,         /**< Current period is EVEN*/
} DVR_CryptoParity_t;

/**\brief DVR crypto filter type*/
typedef enum {
  DVR_CRYPTO_FILTER_TYPE_AUDIO,   /**< Indicate current notification concerns audio packets*/
  DVR_CRYPTO_FILTER_TYPE_VIDEO,   /**< Indicate current notification concerns video packets*/
} DVR_CryptoFilterType_t;

/**\brief DVR record event*/
typedef enum {
  DVR_RECORD_EVENT_ERROR              = 0x1000,         /**< Signal a critical DVR error*/
  DVR_RECORD_EVENT_STATUS             = 0x1001,         /**< Signal the current record status which reach a certain size*/
  DVR_RECORD_EVENT_SYNC_END           = 0x1002,         /**< Signal that data sync has ended*/
  DVR_RECORD_EVENT_CRYPTO_STATUS      = 0x2001,         /**< Signal the current crypto status*/
  DVR_RECORD_EVENT_WRITE_ERROR       = 0x9001,         /**< Signal the current crypto status*/
} DVR_RecordEvent_t;

/**\brief DVR crypto period information*/
typedef struct
{
  DVR_Bool_t              transition;     /**< DVR_TRUE is transition, DVR_FALSE is not transition. At the start of a recording this shall be set to DVR_TRUE*/
  DVR_CryptoParity_t      parity;         /**< The crypto parity at the ts_offset*/
  loff_t                  ts_offset;      /**< TS packet offset corresponding to this crypto period*/
  DVR_CryptoFilterType_t  filter_type;    /**< Indicate this notification concerns audio or video*/
} DVR_CryptoPeriodInfo_t;

/**\brief DVR crypto period information*/
typedef struct {
  uint64_t                      interval_bytes;         /**< The interval between two regular notification of crypto period. For example, if the current segment is always ODD for a long time, record module would notify the current crypto period status when segment size reached the interval_bytes*/
  DVR_Bool_t                    notify_clear_periods;   /**< Indicate whether it shall track the transition to clear period. DVR_TRUE means it shall not notify clear periods, but only transition between ODD and EVEN. DVR_FALSE means it shall notify transition between ODD, EVEN and clear periods*/
} DVR_CryptoPeriod_t;

/**\brief DVR record event notify function*/
typedef DVR_Result_t (*DVR_RecordEventFunction_t) (DVR_RecordEvent_t event, void *params, void *userdata);

/**\brief DVR record open parameters*/
typedef struct {
  int                         fend_dev_id;        /**< Frontend device id */
  int                         dmx_dev_id;         /**< Demux device id*/
  int                         data_from_memory;   /**< Indicate record data from demux or memory*/
  DVR_RecordFlag_t            flags;              /**< DVR record flag*/
  int                         flush_size;         /**< DVR record interrupt flush size*/
  DVR_RecordEventFunction_t   event_fn;           /**< DVR record event callback function*/
  void                        *event_userdata;    /**< DVR event userdata*/
  size_t                      notification_size;  /**< DVR record notification size, record module would send a notification when the size of current segment is multiple of this value. Put 0 in this argument if you don't want to receive the notification*/
  uint8_t                     *clearkey;          /**< key for encrypted PVR on FTA.*/
  uint8_t                     *cleariv;           /**< iv for encrypted PVR on FTA.*/
  uint32_t                    keylen;             /**< key/iv length.*/
  int                         ringbuf_size;       /**< DVR record ring buf size*/
  int                         notification_time;  /**< DVR record notification time, record module would send a notification when the size of current segment is multiple of this value. Put 0 in this argument if you don't want to receive the notification*/
  DVR_Bool_t                  force_sysclock;     /**< If ture, force to use system clock as PVR index time source. If false, libdvr can determine index time source based on actual situation*/
  loff_t                      guarded_segment_size;   /**< Guarded segment size in bytes. Libdvr will be forcely stopped to write anymore if current segment reaches this size*/
} DVR_RecordOpenParams_t;

/**\brief DVR record segment start parameters*/
typedef struct {
  uint64_t segment_id;                                            /**< Segment id*/
  uint32_t nb_pids;                                               /**< Number of pids*/
  DVR_StreamPid_t pids[DVR_MAX_RECORD_PIDS_COUNT];                /**< Pids information*/
  DVR_RecordPidAction_t pid_action[DVR_MAX_RECORD_PIDS_COUNT];    /**< Pids action*/
} DVR_RecordSegmentStartParams_t;

/**\brief DVR record encrypt function*/
typedef DVR_Result_t (*DVR_RecordEncryptFunction_t) (uint8_t *p_in,
    uint32_t in_len,
    uint8_t *p_out,
    uint32_t *p_out_len,
    void *userdata);

/**\brief DVR record current status*/
typedef struct {
  DVR_RecordState_t state;                                        /**< DVR record state*/
  DVR_RecordSegmentInfo_t info;                                   /**< DVR record segment information*/
} DVR_RecordStatus_t;

/**\brief DVR record start parameters*/
typedef struct {
  char location[DVR_MAX_LOCATION_SIZE];                           /**< DVR record file location*/
  DVR_RecordSegmentStartParams_t segment;                         /**< DVR record segment start parameters*/
} DVR_RecordStartParams_t;

/**\brief Open a recording session for a target giving some open parameters
 * \param[out] p_handle Return the handle of the newly created dvr session
 * \param[in] params Open parameters
 * \return DVR_SUCCESS on success
 * \return error code on failure
 */
int dvr_record_open(DVR_RecordHandle_t *p_handle, DVR_RecordOpenParams_t *params);

/**\brief Close a recording session
 * \param[in] handle DVR recording session handle
 * \return DVR_SUCCESS on success
 * \return error code on failure
 */
int dvr_record_close(DVR_RecordHandle_t handle);

/**\brief pause recording on a segment
 * \param[in] handle DVR recording session handle
 * \return DVR_SUCCESS on success
 * \return error code on failure
 */
int dvr_record_pause(DVR_RecordHandle_t handle);

/**\brief resume recording on a segment
 * \param[in] handle DVR recording session handle
 * \return DVR_SUCCESS on success
 * \return error code on failure
 */
int dvr_record_resume(DVR_RecordHandle_t handle);

/**\brief Start recording on a segment
 * \param[in] handle DVR recording session handle
 * \param[in] params DVR start parameters
 * \return DVR_SUCCESS on success
 * \return error code on failure
 */
int dvr_record_start_segment(DVR_RecordHandle_t handle, DVR_RecordStartParams_t *params);

/**\brief Stop the ongoing segment and start recording a new segment
 * \param[in] handle DVR recording session handle
 * \param[in] params DVR start parameters
 * \param[out] p_info DVR record segment information
 * \return DVR_SUCCESS on success
 * \return error code on failure
 */
int dvr_record_next_segment(DVR_RecordHandle_t handle, DVR_RecordStartParams_t *params, DVR_RecordSegmentInfo_t *p_info);

/**\brief Stop the ongoing segment
 * \param[in] handle DVR recording session handle
 * \param[out] p_info DVR record segment information
 * \return DVR_SUCCESS on success
 * \return error code on failure
 */
int dvr_record_stop_segment(DVR_RecordHandle_t handle, DVR_RecordSegmentInfo_t *p_info);

/**\brief Resume the recording on a segment
 * \param[in] handle DVR recording session handle
 * \param[in] params DVR start parameters
 * \param[inout] p_resume_size HAL propose a resume size as a input parameter and output is the real resume size
 * \return DVR_SUCCESS on success
 * \return error code on failure
 */
int dvr_record_resume_segment(DVR_RecordHandle_t handle, DVR_RecordStartParams_t *params, uint64_t *p_resume_size);

/**\brief DVR record write data, used for VOD mode
 * \param[in] handle DVR recording session handle
 * \param[in] buffer The record data buffer
 * \param[in] len The record data length
 * \return DVR_SUCCESS on success
 * \return error code on failure
 */
int dvr_record_write(DVR_RecordHandle_t handle, void *buffer, uint32_t len);

/**\brief DVR record get status
 * \param[in] handle DVR recording session handle
 * \param[out] p_status Return current DVR record status
 * \return DVR_SUCCESS on success
 * \return error code on failure
 */
int dvr_record_get_status(DVR_RecordHandle_t handle, DVR_RecordStatus_t *p_status);

/**\brief Set DVR record encrypt function
 * \param[in] handle, DVR recording session handle
 * \param[in] func, DVR recording encrypt function
 * \param[in] userdata, DVR record userdata from the caller
 * \return DVR_SUCCESS on success
 * \return error code on failure
 */
int dvr_record_set_encrypt_callback(DVR_RecordHandle_t handle, DVR_CryptoFunction_t func, void *userdata);

/**\brief Set DVR record secure buffer used for protect the secure content
 * \param[in] handle, DVR recording session handle
 * \param[in] p_secure_buf, Secure buffer address which can NOT access by ACPU
 * \param[in] len, Secure buffer length
 * \return DVR_SUCCESS on success
 * \return error code on failure
 */
int dvr_record_set_secure_buffer(DVR_RecordHandle_t handle, uint8_t *p_secure_buf, uint32_t len);

/**
 * check record mode is secure or free.
 * \param handle The record  session handle.
 * \retval 1:secure 0: free mode.
 * \return secure or free.
 */
int dvr_record_is_secure_mode(DVR_RecordHandle_t handle);

/**\brief Turn on/off recording data discarding
 * \param[in] handle, DVR recording session handle
 * \param[in] discard, 0: normal processing, 1: discard coming recording data
 * \return DVR_SUCCESS on success
 * \return error code on failure
 */
int dvr_record_discard_coming_data(DVR_RecordHandle_t handle, DVR_Bool_t discard);

/**\brief control the recording logic
 * \param[in] handle, DVR recording session handle
 * \param[in] cmd, command
 * \param[in/out] data, parameters
 * \param[in/out] size, size of parameters
 * \return DVR_SUCCESS on success
 * \return error code on failure
 */
int dvr_record_ioctl(DVR_RecordHandle_t handle, unsigned int cmd, void *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /*END _DVR_RECORD_H_*/
