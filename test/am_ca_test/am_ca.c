#ifdef _FORTIFY_SOURCE
#undef _FORTIFY_SOURCE
#endif
/***************************************************************************
 * Copyright (c) 2014 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */
/**\file
 * \brief AMLogic 解扰器驱动
 *
 * \author Gong Ke <ke.gong@amlogic.com>
 * \date 2010-08-06: create the document
 ***************************************************************************/

#define AM_DEBUG_LEVEL 5

//#include <am_debug.h>
//#include <am_mem.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include <errno.h>
/*add for config define for linux dvb *.h*/
//#include <am_config.h>
#include "ca.h"
#include "am_ca.h"
#include <stdio.h>
/****************************************************************************
 * Macro definitions
 ***************************************************************************/

//#define DEV_NAME "/dev/dvb/adapter0/ca"
#define DEV_NAME "/dev/dvb0.ca"

/****************************************************************************
 * Static data
 ***************************************************************************/

/****************************************************************************
 * API functions
 ***************************************************************************/
struct _dsc_dev{
    int used;
    int fd;
};
typedef struct _dsc_dev dsc_dev;

#define MAX_DSC_DEV        32
static dsc_dev rec_dsc_dev[MAX_DSC_DEV];

int ca_open (int devno)
{
    int fd;
    char buf[32];
    static int init_flag = 0;

    printf("ca_open enter\n");
    if (!init_flag) {
        init_flag = 1;
        memset(&rec_dsc_dev, 0, sizeof(rec_dsc_dev));
    }
    if (devno >= MAX_DSC_DEV) {
        return -1;
    }
    if (rec_dsc_dev[devno].used)
        return 0;

    snprintf(buf, sizeof(buf), DEV_NAME"%d", devno);
    fd = open(buf, O_RDWR);
    if (fd == -1)
    {
        printf("cannot open \"%s\" (%d:%s)", DEV_NAME, errno, strerror(errno));
        return 0;
    }
    rec_dsc_dev[devno].fd = fd;
    rec_dsc_dev[devno].used = 1;
    return 0;
}

int ca_alloc_chan (int devno, unsigned int pid, int algo, int dsc_type)
{
    int ret = 0;
    int fd = 0;
    struct ca_sc2_descr_ex desc;

    printf("ca_alloc_chan dev:%d, pid:0x%0x, algo:%d, dsc_type:%d\n", devno, pid, algo, dsc_type);
    desc.cmd = CA_ALLOC;
    desc.params.alloc_params.pid = pid;
    desc.params.alloc_params.algo = algo;
    desc.params.alloc_params.dsc_type = dsc_type;
    desc.params.alloc_params.ca_index = -1;

    if (devno >= MAX_DSC_DEV || !rec_dsc_dev[devno].used) {
        return -1;
    }
    fd = rec_dsc_dev[devno].fd;
    ret = ioctl(fd, CA_SC2_SET_DESCR_EX, &desc);
    if (ret != 0) {
        printf(" ca_alloc_chan ioctl fail, ret:0x%0x\n", ret);
        return -1;
    }
    printf("ca_alloc_chan, index:%d\n",desc.params.alloc_params.ca_index);
    return desc.params.alloc_params.ca_index;
}

int ca_free_chan (int devno, int index)
{
    int ret = 0;
    int fd = rec_dsc_dev[devno].fd;
    struct ca_sc2_descr_ex desc;

    printf("ca_free_chan:%d, index:%d\n", devno, index);
    desc.cmd = CA_FREE;
    desc.params.free_params.ca_index = index;

    if (devno >= MAX_DSC_DEV || !rec_dsc_dev[devno].used) {
        return -1;
    }
    fd = rec_dsc_dev[devno].fd;
    ret = ioctl(fd, CA_SC2_SET_DESCR_EX, &desc);
    if (ret != 0) {
        printf(" ca_free_chan ioctl fail\n");
        return -1;
    }
    printf("ca_free_chan, index:%d\n",index);
    return 0;
}


int ca_set_key (int devno, int index, int parity, int key_index)
{
    int ret = 0;
    int fd = 0;
    struct ca_sc2_descr_ex desc;

    printf("ca_set_key dev:%d, index:%d, parity:%d, key_index:%d\n",
            devno, index, parity, key_index);
    desc.cmd = CA_KEY;
    desc.params.key_params.ca_index = index;
    desc.params.key_params.parity = parity;
    desc.params.key_params.key_index = key_index;

    if (devno >= MAX_DSC_DEV || !rec_dsc_dev[devno].used) {
        return -1;
    }
    fd = rec_dsc_dev[devno].fd;
    ret = ioctl(fd, CA_SC2_SET_DESCR_EX, &desc);
    if (ret != 0) {
        printf(" ca_set_key ioctl fail, ret:0x%0x\n", ret);
        return -1;
    }
    printf("ca_set_key, index:%d, parity:%d, key_index:%d\n",index, parity, key_index);
    return 0;
}

int ca_close (int devno)
{
    int fd = 0;

    printf("ca_close dev:%d\n", devno);
    if (devno >= MAX_DSC_DEV || !rec_dsc_dev[devno].used) {
        return -1;
    }
    fd = rec_dsc_dev[devno].fd;
    close(fd);
    rec_dsc_dev[devno].fd = 0;
    rec_dsc_dev[devno].used = 0;
    return 0;
}

