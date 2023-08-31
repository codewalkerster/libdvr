#ifndef _SEGMENT_DATAOUT_H_
#define _SEGMENT_DATAOUT_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "dvr_types.h"
#include "segment_ops.h"

/**
 * Segment implementation 2
 * for data loop out
 */

#define SEGMENT_DATAOUT_CMD_SET_CALLBACK 0x1001
typedef struct Segment_DataoutCallback_s {
    int (*callback)(unsigned char *buf, size_t size, void *priv);
    void *priv;
} Segment_DataoutCallback_t;



int segment_dataout_open(Segment_OpenParams_t *params, Segment_Handle_t *p_handle);
int segment_dataout_close(Segment_Handle_t handle);
int segment_dataout_ioctl(Segment_Handle_t handle, int cmd, void *data, size_t size);
ssize_t segment_dataout_write(Segment_Handle_t handle, void *buf, size_t count);
loff_t segment_dataout_tell_total_time(Segment_Handle_t handle);
int segment_dataout_store_info(Segment_Handle_t handle, Segment_StoreInfo_t *p_info);
int segment_dataout_store_allInfo(Segment_Handle_t handle, Segment_StoreInfo_t *p_info);
int segment_dataout_update_pts_force(Segment_Handle_t handle, uint64_t pts, loff_t offset);
int segment_dataout_update_pts(Segment_Handle_t handle, uint64_t pts, loff_t offset);
loff_t segment_dataout_tell_position(Segment_Handle_t handle);

#ifdef __cplusplus
}
#endif

#endif
