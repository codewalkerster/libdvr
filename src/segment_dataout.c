#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "dvr_types.h"
#include "segment_dataout.h"


/**\brief Segment context*/
typedef struct {
  uint64_t        segment_id;

  uint64_t        cur_time;
  uint64_t        size_written;
  uint64_t        pkts_written;

  Segment_DataoutCallback_t dataout_callback;
} Segment_Context_t;

int segment_dataout_open(Segment_OpenParams_t *params, Segment_Handle_t *p_handle)
{
  Segment_Context_t *p_ctx;

  DVR_RETURN_IF_FALSE(params);
  DVR_RETURN_IF_FALSE(p_handle);

  p_ctx = (void*)malloc(sizeof(Segment_Context_t));
  DVR_RETURN_IF_FALSE(p_ctx);

  memset(p_ctx, 0, sizeof(Segment_Context_t));

  p_ctx->segment_id = params->segment_id;

  *p_handle = (Segment_Handle_t)p_ctx;
  return DVR_SUCCESS;
}

int segment_dataout_close(Segment_Handle_t handle)
{
  Segment_Context_t *p_ctx = (void *)handle;

  DVR_RETURN_IF_FALSE(p_ctx);

  free(p_ctx);
  return DVR_SUCCESS;
}

int segment_dataout_ioctl(Segment_Handle_t handle, int cmd, void *data, size_t size)
{
  Segment_Context_t *p_ctx = (void *)handle;

  DVR_RETURN_IF_FALSE(p_ctx);

  switch (cmd) {
    case SEGMENT_DATAOUT_CMD_SET_CALLBACK:
      DVR_RETURN_IF_FALSE(data != NULL);
      p_ctx->dataout_callback = *(Segment_DataoutCallback_t *)data;
    break;

    default:
      return DVR_FAILURE;
  }
  return DVR_SUCCESS;
}

ssize_t segment_dataout_write(Segment_Handle_t handle, void *buf, size_t count)
{
  ssize_t len = 0;
  Segment_Context_t *p_ctx = (Segment_Context_t *)handle;

  DVR_RETURN_IF_FALSE(p_ctx);
  DVR_RETURN_IF_FALSE(buf);

  if (p_ctx->dataout_callback.callback)
    len = p_ctx->dataout_callback.callback(buf, count, p_ctx->dataout_callback.priv);

  p_ctx->size_written += count;

  return count;
}

loff_t segment_dataout_tell_total_time(Segment_Handle_t handle)
{
  uint64_t pts = ULLONG_MAX;
  Segment_Context_t *p_ctx = (Segment_Context_t *)handle;

  DVR_RETURN_IF_FALSE(p_ctx);

  pts = p_ctx->cur_time;

  return pts;
}

int segment_dataout_store_info(Segment_Handle_t handle, Segment_StoreInfo_t *p_info)
{
  Segment_Context_t *p_ctx = (Segment_Context_t *)handle;

  DVR_RETURN_IF_FALSE(p_ctx);
  DVR_RETURN_IF_FALSE(p_info);

  p_ctx->segment_id = p_info->id;
  p_ctx->cur_time = p_info->duration;
  p_ctx->size_written = p_info->size;
  p_ctx->pkts_written = p_info->nb_packets;

  return DVR_SUCCESS;
}

int segment_dataout_store_allInfo(Segment_Handle_t handle, Segment_StoreInfo_t *p_info)
{
  return segment_dataout_store_info(handle, p_info);
}


int segment_dataout_update_pts_force(Segment_Handle_t handle, uint64_t pts, loff_t offset)
{
  Segment_Context_t *p_ctx = (Segment_Context_t *)handle;
  DVR_RETURN_IF_FALSE(p_ctx);

  if (offset == p_ctx->size_written)
    p_ctx->cur_time = pts;
  return DVR_SUCCESS;
}

int segment_dataout_update_pts(Segment_Handle_t handle, uint64_t pts, loff_t offset)
{
  return segment_dataout_update_pts_force(handle, pts, offset);
}

loff_t segment_dataout_tell_position(Segment_Handle_t handle)
{
  loff_t pos;
  Segment_Context_t *p_ctx = (Segment_Context_t *)handle;

  DVR_RETURN_IF_FALSE(p_ctx);

  pos = p_ctx->size_written;

  return pos;
}
