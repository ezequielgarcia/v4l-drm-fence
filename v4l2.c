#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <fcntl.h>              /* low-level i/o */
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <libv4l2.h>

#include "videodev2.h"
#include "v4l2.h"

#define CLEAR(x) memset(&(x), 0, sizeof(x))
#define PCLEAR(x) memset(x, 0, sizeof(*x))

static enum v4l2_memory memory_type;

void v4l2_queue_buffer(int fd, int index, int dmabuf_fd, int type)
{
	struct v4l2_buffer buf;

	CLEAR(buf);
	buf.type = type;
	buf.index = index;
	buf.m.fd = dmabuf_fd;
	buf.memory = V4L2_MEMORY_DMABUF;
	if (-1 == ioctl(fd, VIDIOC_QBUF, &buf))
		errno_print("VIDIOC_QBUF");
}

int v4l2_dequeue_buffer(int fd, struct v4l2_buffer *buf, int type)
{	
	PCLEAR(buf);

	buf->type = type;
	buf->memory = memory_type;

	if (-1 == ioctl(fd, VIDIOC_DQBUF, buf)) {
		switch (errno) {
		case EAGAIN:
			return 0;
		case EIO:
			/* Could ignore EIO, see spec. */
			/* fall through */
		default:
			errno_print("VIDIOC_DQBUF");
		}
	}

	return 1;
}

void v4l2_stop(int fd, enum v4l2_buf_type type)
{
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (-1 == ioctl(fd, VIDIOC_STREAMOFF, &type))
		errno_print("VIDIOC_STREAMOFF");
}

void v4l2_start(int fd, enum v4l2_buf_type type)
{
	if (-1 == ioctl(fd, VIDIOC_STREAMON, &type))
		errno_print("VIDIOC_STREAMON");
}

void v4l2_init_dmabuf(int fd, int count, int type, struct buffer *buffers)
{
	struct v4l2_requestbuffers req;
	unsigned int i;

	CLEAR(req);

	req.count = count;
	req.type = type;
	req.memory = V4L2_MEMORY_DMABUF;

	if (-1 == ioctl(fd, VIDIOC_REQBUFS, &req)) {
		if (EINVAL == errno) {
			fprintf(stderr, "does not support dmabuf\n");
			exit(EXIT_FAILURE);
		} else {
			errno_print("VIDIOC_REQBUFS");
		}
	}

	if (req.count < 2) {
		fprintf(stderr, "Insufficient buffer memory\n");
		exit(EXIT_FAILURE);
	}

	for (i = 0; i < req.count; ++i) {
		struct v4l2_buffer buf;

		CLEAR(buf);

		buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory      = V4L2_MEMORY_DMABUF;
		buf.index       = i;

		if (-1 == ioctl(fd, VIDIOC_QUERYBUF, &buf))
			errno_print("VIDIOC_QUERYBUF");
		buffers[i].v4l_index = buf.index;
	}
}

void v4l2_set_fmt(int fd, int width, int height, enum v4l2_buf_type type, int pixel_format)
{
	struct v4l2_format fmt;

	CLEAR(fmt);
	fmt.type = type;
	fmt.fmt.pix.width       = width;
	fmt.fmt.pix.height      = height;
	fmt.fmt.pix.pixelformat = pixel_format;
	fmt.fmt.pix.field       = V4L2_FIELD_NONE;
	fmt.fmt.pix.colorspace  = V4L2_COLORSPACE_SRGB;

	if (-1 == ioctl(fd, VIDIOC_S_FMT, &fmt))
		errno_print("VIDIOC_S_FMT");

	printf("v4l2 negotiated format for type %d: ", type);
	printf("size = %dx%d, ", fmt.fmt.pix.width, fmt.fmt.pix.height);
	printf("pitch = %d bytes\n", fmt.fmt.pix.bytesperline);
}
