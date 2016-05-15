/******************************************************************************
 * Copyright (C) 2014-2015
 * file:    v4l2.c
 * author:  gozfree <gozfree@163.com>
 * created: 2016-04-21 22:49
 * updated: 2016-04-21 22:49
 ******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <libgzf.h>
#include <liblog.h>
#include "device.h"
#include "device_io.h"
#include "common.h"
#include "imgconvert.h"

#define MAX_V4L_BUF         (32)
#define MAX_V4L_REQBUF_CNT  (256)

//suppose v4l2 control cmds
#define MAX_V4L2_CID        (9)
static const uint32_t queried_ctrls[] = {
    V4L2_CID_BRIGHTNESS,
    V4L2_CID_CONTRAST,
    V4L2_CID_SATURATION,
    V4L2_CID_HUE,
    V4L2_CID_RED_BALANCE,
    V4L2_CID_BLUE_BALANCE,
    V4L2_CID_GAMMA,
    V4L2_CID_EXPOSURE,
    V4L2_CID_AUTOGAIN,
    V4L2_CID_GAIN
};


struct v4l2_ctx {
    int fd;
    int on_read_fd;
    int on_write_fd;
    int width;
    int height;
    struct iovec buf[MAX_V4L_BUF];
    int buf_index;
    int req_count;
    struct v4l2_capability cap;
    uint32_t ctrl_flags;
    struct v4l2_queryctrl controls[MAX_V4L2_CID];
};

static int v4l2_get_input(struct v4l2_ctx *vc)
{
    struct v4l2_input input;
    struct v4l2_standard standard;
    v4l2_std_id std_id;

    memset(&input, 0, sizeof (input));
    input.index = 0;

    if (ioctl(vc->fd, VIDIOC_ENUMINPUT, &input) == -1) {
        loge("Unable to query input %d."
             " VIDIOC_ENUMINPUT, if you use a WEBCAM change input value in conf by -1",
             input.index);
        return -1;
    }

    logi("V4L2 Input information:\n");
    logi("\tinput.name: \t\"%s\"\n", input.name);
    if (input.type & V4L2_INPUT_TYPE_TUNER)
        logi("\t\t- TUNER\n");
    if (input.type & V4L2_INPUT_TYPE_CAMERA)
        logi("\t\t- CAMERA\n");

    if (ioctl(vc->fd, VIDIOC_G_STD, &std_id) == -1) {
        logd("Device doesn't support VIDIOC_G_STD\n");
    } else {
        memset(&standard, 0, sizeof(standard));
        standard.index = 0;
        while (ioctl(vc->fd, VIDIOC_ENUMSTD, &standard) == 0) {
            if (standard.id & std_id)
                logi("\t\t- video standard %s\n", standard.name);
            standard.index++;
        }
        logi("Set standard method %d\n", (int)std_id);
    }
    return 0;
}
static int v4l2_get_cap(struct v4l2_ctx *vc)
{
    struct v4l2_fmtdesc fmtdesc;

    if (ioctl(vc->fd, VIDIOC_QUERYCAP, &vc->cap) < 0) {
        loge("failed to VIDIOC_QUERYCAP\n");
        return -1;
    }

    logi("V4L2 Capability information:\n");
    logi("\tcap.driver: \t\"%s\"\n", vc->cap.driver);
    logi("\tcap.card: \t\"%s\"\n", vc->cap.card);
    logi("\tcap.bus_info: \t\"%s\"\n", vc->cap.bus_info);
    logi("\tcap.capabilities:\n", vc->cap.capabilities);
    if (vc->cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)
        logi("\t\t- VIDEO_CAPTURE\n");
    if (vc->cap.capabilities & V4L2_CAP_VIDEO_OUTPUT)
        logi("\t\t- VIDEO_OUTPUT\n");
    if (vc->cap.capabilities & V4L2_CAP_VIDEO_OVERLAY)
        logi("\t\t- VIDEO_OVERLAY\n");
    if (vc->cap.capabilities & V4L2_CAP_VBI_CAPTURE)
        logi("\t\t- VBI_CAPTURE\n");
    if (vc->cap.capabilities & V4L2_CAP_VBI_OUTPUT)
        logi("\t\t- VBI_OUTPUT\n");
    if (vc->cap.capabilities & V4L2_CAP_RDS_CAPTURE)
        logi("\t\t- RDS_CAPTURE\n");
    if (vc->cap.capabilities & V4L2_CAP_TUNER)
        logi("\t\t- TUNER\n");
    if (vc->cap.capabilities & V4L2_CAP_AUDIO)
        logi("\t\t- AUDIO\n");
    if (vc->cap.capabilities & V4L2_CAP_READWRITE)
        logi("\t\t- READWRITE\n");
    if (vc->cap.capabilities & V4L2_CAP_ASYNCIO)
        logi("\t\t- ASYNCIO\n");
    if (vc->cap.capabilities & V4L2_CAP_STREAMING)
        logi("\t\t- STREAMING\n");
    if (vc->cap.capabilities & V4L2_CAP_TIMEPERFRAME)
        logi("\t\t- TIMEPERFRAME\n");

    if (!(vc->cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        loge("s: Device does not support capturing.\n");
        return -1;
    }

    logi("V4L2 Support Format:\n");
    fmtdesc.index = 0;
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    while (-1 != ioctl(vc->fd, VIDIOC_ENUM_FMT, &fmtdesc)) {
        logi("\t%d. %s\n", fmtdesc.index, fmtdesc.description);
        ++fmtdesc.index;
    }
    return 0;
}

static int v4l2_scan_controls(struct v4l2_ctx *vc)
{
    int i;
    struct v4l2_queryctrl *qctrl;
    struct v4l2_control control;

    for (i = 0; queried_ctrls[i]; i++) {
        qctrl = &vc->controls[i];
        qctrl->id = queried_ctrls[i];
        if (ioctl(vc->fd, VIDIOC_QUERYCTRL, qctrl))
            continue;
        vc->ctrl_flags |= 1 << i;
    }

    logi("V4L2 Supported Control:\n");
    for (i = 0; queried_ctrls[i]; i++) {
        if (!(vc->ctrl_flags & (1 << i))) {
            continue;
        }

        qctrl->id = queried_ctrls[i];
        if (ioctl(vc->fd, VIDIOC_QUERYCTRL, qctrl))
            continue;
        memset(&control, 0, sizeof (control));
        control.id = queried_ctrls[i];
        ioctl(vc->fd, VIDIOC_G_CTRL, &control);

        logi("\t%s, %s range: [%d, %d], default: %d, current: %d\n",
             qctrl->name,
             qctrl->flags & V4L2_CTRL_FLAG_DISABLED ? "!DISABLED!" : "",
             qctrl->minimum, qctrl->maximum,
             qctrl->default_value, control.value);
    }
    return 0;
}

static int v4l2_set_control(struct v4l2_ctx *vc, uint32_t cid, int value);
static int v4l2_get_info(struct v4l2_ctx *vc)
{
    v4l2_scan_controls(vc);

    v4l2_get_cap(vc);
    v4l2_get_input(vc);
    return 0;
}


static int v4l2_set_format(struct v4l2_ctx *vc)
{
    struct v4l2_fmtdesc fmtdesc;
    struct v4l2_format fmt;
    struct v4l2_pix_format *pix = &fmt.fmt.pix;

    fmtdesc.index = 0;
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    logd("V4L2 Support Format:\n");
    while (-1 != ioctl(vc->fd,VIDIOC_ENUM_FMT,&fmtdesc)) {
        logd("\t%d. %s\n", fmtdesc.index, fmtdesc.description);
        ++fmtdesc.index;
    }

    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (-1 == ioctl(vc->fd, VIDIOC_G_FMT, &fmt)) {
        loge("ioctl(VIDIOC_G_FMT) failed(%d): %s\n", errno, strerror(errno));
        return -1;
    }
    logd("v4l2 default pix format: %d*%d\n", pix->width, pix->height);

    if (vc->width > 0 || vc->height > 0) {
        //set v4l2 pixel format
        pix->width = vc->width;
        pix->height = vc->height;
    } else {
        vc->width = pix->width;
        vc->height = pix->height;
    }
    logd("v4l2 using pix format: %d*%d\n", pix->width, pix->height);
    if (-1 == ioctl(vc->fd, VIDIOC_S_FMT, &fmt)) {
        loge("ioctl(VIDIOC_S_FMT) failed(%d): %s\n", errno, strerror(errno));
        return -1;
    }

    logi("Using palette %c%c%c%c (%dx%d) bytesperlines %d sizeimage %d colorspace %08x\n",
         fmt.fmt.pix.pixelformat >> 0,
         fmt.fmt.pix.pixelformat >> 8,
         fmt.fmt.pix.pixelformat >> 16,
         fmt.fmt.pix.pixelformat >> 24,
         vc->width,
         vc->height, fmt.fmt.pix.bytesperline,
         fmt.fmt.pix.sizeimage,
         fmt.fmt.pix.colorspace);
    return 0;
}

static int v4l2_buf_enqueue(struct v4l2_ctx *vc)
{
    struct v4l2_buffer buf;
    char notify = '1';
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = vc->buf_index;

    if (-1 == ioctl(vc->fd, VIDIOC_QBUF, &buf)) {
        loge("ioctl(VIDIOC_QBUF) failed(%d): %s\n", errno, strerror(errno));
        return -1;
    }
    if (write(vc->on_write_fd, &notify, 1) != 1) {
        loge("Failed writing to notify pipe: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

static int v4l2_req_buf(struct v4l2_ctx *vc)
{
    int i;
    struct v4l2_requestbuffers req;
    enum v4l2_buf_type type;
    char notify = '1';

    memset(&req, 0, sizeof(req));
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.count = MAX_V4L_REQBUF_CNT;
    req.memory = V4L2_MEMORY_MMAP;
    //request buffer
    if (-1 == ioctl(vc->fd, VIDIOC_REQBUFS, &req)) {
        loge("ioctl(VIDIOC_REQBUFS) failed(%d): %s\n", errno, strerror(errno));
        return -1;
    }
    vc->req_count = req.count;
    if (req.count > MAX_V4L_REQBUF_CNT || req.count < 2) {
        loge("Insufficient buffer memory\n");
        return -1;
    }
    for (i = 0; i < vc->req_count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        //query buffer
        if (-1 == ioctl(vc->fd, VIDIOC_QUERYBUF, &buf)) {
            loge("ioctl(VIDIOC_QUERYBUF) failed(%d): %s\n",
                 errno, strerror(errno));
            return -1;
        }

        //mmap buffer
        vc->buf[i].iov_len = buf.length;
        vc->buf[i].iov_base = mmap(NULL, buf.length, PROT_READ|PROT_WRITE,
                               MAP_SHARED, vc->fd, buf.m.offset);
        if (MAP_FAILED == vc->buf[i].iov_base) {
            loge("mmap failed: %s\n", strerror(errno));
            return -1;
        }

        //enqueue buffer
        if (-1 == ioctl(vc->fd, VIDIOC_QBUF, &buf)) {
            loge("ioctl(VIDIOC_QBUF) failed(%d): %s\n", errno, strerror(errno));
            return -1;
        }
    }

    //stream on
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (-1 == ioctl(vc->fd, VIDIOC_STREAMON, &type)) {
        loge("ioctl(VIDIOC_STREAMON) failed(%d): %s\n", errno, strerror(errno));
        return -1;
    }
    if (write(vc->on_write_fd, &notify, 1) != 1) {
        loge("Failed writing to notify pipe\n");
        return -1;
    }
    return 0;
}

static int v4l2_buf_dequeue(struct v4l2_ctx *vc, struct frame *f)
{
    struct v4l2_buffer buf;
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    while (1) {
        if (-1 == ioctl(vc->fd, VIDIOC_DQBUF, &buf)) {
            loge("ioctl(VIDIOC_DQBUF) failed(%d): %s\n",
                 errno, strerror(errno));
            if (errno == EINTR || errno == EAGAIN)
                continue;
            else
                return -1;
        }
        break;
    }
    vc->buf_index = buf.index;
    f->index = buf.index;
    f->buf.iov_base = vc->buf[buf.index].iov_base;
    f->buf.iov_len = buf.bytesused;
//    loge("v4l2 frame buffer addr = %p, len = %d, index = %d\n",
//    f->addr, f->len, f->index);

    return f->buf.iov_len;
}

static int v4l2_open(struct device_ctx *dc, const char *dev)
{
    int fd = -1;
    int fds[2] = {0};
    struct v4l2_ctx *vc = CALLOC(1, struct v4l2_ctx);
    if (!vc) {
        loge("malloc v4l2_ctx failed!\n");
        return -1;
    }
    if (pipe(fds)) {
        loge("create pipe failed(%d): %s\n", errno, strerror(errno));
        goto failed;
    }
    vc->on_read_fd = fds[0];
    vc->on_write_fd = fds[1];
    logd("pipe: rfd = %d, wfd = %d\n", vc->on_read_fd, vc->on_write_fd);

    fd = open(dev, O_RDWR);
    if (fd == -1) {
        loge("open %s failed: %s\n", dev, strerror(errno));
        goto failed;
    }
    vc->fd = fd;
    vc->width = 320;//XXX
    vc->height = 240;//XXX

    if (-1 == v4l2_get_info(vc)) {
        loge("v4l2_get_info failed\n");
        goto failed;
    }
    if (-1 == v4l2_set_format(vc)) {
        loge("v4l2_set_format failed\n");
        goto failed;
    }
    if (-1 == v4l2_req_buf(vc)) {
        loge("v4l2_req_buf failed\n");
        goto failed;
    }

    dc->fd = vc->on_read_fd;//use pipe fd to trigger event
    dc->media.video.width = vc->width;
    dc->media.video.height = vc->height;
    dc->priv = vc;
    return 0;

failed:
    if (fd != -1) {
        close(fd);
    }
    if (vc) {
        free(vc);
    }
    if (fds[0] != -1 || fds[1] != -1) {
        close(fds[0]);
        close(fds[1]);
    }
    return -1;
}

static int v4l2_read(struct device_ctx *dc, void *buf, int len)
{
    struct v4l2_ctx *vc = (struct v4l2_ctx *)dc->priv;
    struct frame f;
    struct iovec tmp;
    int i, flen;
    char notify;

    if (read(vc->on_read_fd, &notify, sizeof(notify)) != 1) {
        perror("Failed read from notify pipe");
    }

    flen = v4l2_buf_dequeue(vc, &f);
    if (flen == -1) {
        loge("v4l2 dequeue failed!\n");
        return -1;
    }
    if (flen > len) {
        loge("v4l2 frame is %d bytes, but buffer len is %d, not enough!\n",
             flen, len);
        return -1;
    }
    if (len < (int)f.buf.iov_len) {
        loge("error occur!\n");
        return -1;
    }
#if 1
    tmp.iov_len = f.buf.iov_len;
    tmp.iov_base = calloc(1, tmp.iov_len);
    conv_yuv422to420p(tmp.iov_base, f.buf.iov_base, vc->width, vc->height);
    for (i = 0; i < (int)f.buf.iov_len; i++) {//8 byte copy
        *((char *)buf + i) = *((char *)tmp.iov_base + i);
    }
    free(tmp.iov_base);
#else

    for (i = 0; i < f.buf.iov_len; i++) {//8 byte copy
        *((char *)buf + i) = *((char *)f.buf.iov_base + i);
    }
#endif
    return f.buf.iov_len;
}

static int v4l2_write(struct device_ctx *dc, void *buf, int len)
{
    struct v4l2_ctx *vc = (struct v4l2_ctx *)dc->priv;
    return v4l2_buf_enqueue(vc);
}

static void v4l2_close(struct device_ctx *dc)
{
    int i;
    struct v4l2_ctx *vc = (struct v4l2_ctx *)dc->priv;
    enum v4l2_buf_type type;
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (-1 == ioctl(vc->fd, VIDIOC_STREAMOFF, &type)) {
        loge("ioctl(VIDIOC_STREAMOFF) failed(%d): %s\n",
             errno, strerror(errno));
    }
    for (i = 0; i < vc->req_count; i++) {
        munmap(vc->buf[i].iov_base, vc->buf[i].iov_len);
    }
    close(vc->fd);
    close(vc->on_read_fd);
    close(vc->on_write_fd);
    free(vc);
}

static int v4l2_set_control(struct v4l2_ctx *vc, uint32_t cid, int value)
{
    int i;
    struct v4l2_control control;
    int ret = -1;
    for (i = 0; queried_ctrls[i]; i++) {
        if (!(vc->ctrl_flags & (1 << i))) {
            continue;
        }
        if (cid != queried_ctrls[i]) {
            continue;
        }
        struct v4l2_queryctrl *ctrl = &vc->controls[i];


        memset(&control, 0, sizeof (control));
        control.id = queried_ctrls[i];

        switch (ctrl->type) {
        case V4L2_CTRL_TYPE_INTEGER://0~255
            value = control.value =
                    (value * (ctrl->maximum - ctrl->minimum) / 256) + ctrl->minimum;
            ret = ioctl(vc->fd, VIDIOC_S_CTRL, &control);
            break;

        case V4L2_CTRL_TYPE_BOOLEAN:
            value = control.value = value ? 1 : 0;
            ret = ioctl(vc->fd, VIDIOC_S_CTRL, &control);
            break;

        default:
            logi("control type not supported yet");
            return -1;
        }

        logi("setting %s to %d (ret %d %s) %s\n", ctrl->name, value);
    }
    return ret;
}

static int v4l2_ioctl(struct device_ctx *dc, uint32_t cmd, void *buf, int len)
{
    struct v4l2_ctx *vc = (struct v4l2_ctx *)dc->priv;
    struct video_ctrl *vctrl;
    switch (cmd) {
    case DEV_VIDEO_GET_CAP:
        v4l2_get_cap(vc);
        break;
    case DEV_VIDEO_SET_CTRL:
        vctrl = (struct video_ctrl *)buf;
        v4l2_set_control(vc, vctrl->cmd, vctrl->val);
        break;
    default:
        break;
    }
    return 0;
}

struct device aq_v4l2_device = {
    .name = "v4l2",
    .open = v4l2_open,
    .read = v4l2_read,
    .write = v4l2_write,
    .ioctl = v4l2_ioctl,
    .close = v4l2_close,
};
