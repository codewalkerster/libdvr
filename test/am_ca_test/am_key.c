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

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include <errno.h>
#include "aml_key.h"
#include <stdio.h>

/****************************************************************************
 * Macro definitions
 ***************************************************************************/

#define DEV_NAME "/dev/key"
/****************************************************************************
 * Static data
 ***************************************************************************/

/****************************************************************************
 * API functions
 ***************************************************************************/
static int s_fd = -1;
int key_open (void)
{
    char buf[32];

    if (s_fd != -1)
        return s_fd;

    snprintf(buf, sizeof(buf), "/dev/key");
    s_fd = open(buf, O_RDWR);
    if (s_fd == -1)
    {
        printf("cannot open \"%s\" (%d:%s)", DEV_NAME, errno, strerror(errno));
        return -1;
    }
    printf("%s key fd:%d\n", buf, s_fd);
    return s_fd;
}

int key_close(int fd)
{
    if (fd == -1) {
        printf("key_close invalid fd\n");
        return 0;
    }
    close(fd);
    s_fd = -1;
    return 0;
}
int key_alloc(int fd, int is_iv)
{
    int ret = 0;
    struct key_alloc param;

    if (fd == -1) {
        printf("key alloc fd invalid\n");
        return -1;
    }
    param.is_iv = is_iv;
    param.key_index  = -1;

    ret = ioctl(fd, KEY_ALLOC, &param);
    if (ret == 0) {
        printf("key_alloc index:%d\n", param.key_index);
        return param.key_index;
    } else {
        printf("key_alloc key fail,fd:%d, is_iv:%d\n", fd, is_iv);
        printf("fail \"%s\" (%d:%s)", DEV_NAME, errno, strerror(errno));
        return -1;
    }
}

int key_config(int fd, int key_index, int key_userid, int key_algo, unsigned int ext_value)
{
    int ret = 0;
    struct key_config config;

    if (fd == -1) {
        printf("key config fd invalid\n");
        return -1;
    }
    config.key_userid = key_userid;
    config.key_algo   = key_algo;
    config.key_index  = key_index;
    config.ext_value  = ext_value;

    ret = ioctl(fd, KEY_CONFIG, &config);
    if (ret == 0) {
        printf("key_config index:%d\n", config.key_index);
        return config.key_index;
    } else {
        printf("key_config key fail,fd:%d, key_userid:%d, key_algo:%d\n", fd, key_userid, key_algo);
        printf("fail \"%s\" (%d:%s)", DEV_NAME, errno, strerror(errno));
        return -1;
    }
}
int key_free(int fd, int key_index)
{
    int ret = 0;

    printf("key_free fd:%d key_index:%d\n", fd, key_index);
    if (fd == -1 || key_index == -1) {
        printf("key_free invalid parameter, fd:%d, key_index:%d\n", fd, key_index);
        return -1;
    }

    ret = ioctl(fd, KEY_FREE, key_index);
    if (ret == 0) {
        printf("key_free key_index:%d success\n", key_index);
        return 0;
    } else {
        printf("key_free key_index:%d fail\n", key_index);
        return -1;
    }
}

int key_set(int fd, int key_index, char *key, int key_len)
{
    int ret = 0;
    struct key_descr key_d;

    if (fd == -1 || key_index ==  -1 || key_len > 32) {
        printf("key_set invalid parameter, fd:%d, key_index:%d, key_len:%d\n",
            fd, key_index, key_len);
        return -1;
    }

    key_d.key_index = key_index;
    memcpy(&key_d.key, key, key_len);
    key_d.key_len = key_len;
    ret = ioctl(fd, KEY_SET, &key_d);
    if (ret == 0) {
        printf("key_set success\n");
        return 0;
    } else {
        printf("key_set fail\n");
        return -1;
    }
}

