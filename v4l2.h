
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <libv4l2.h>
#include "videodev2.h"

#define MAX_PLANES 3

enum owner {
	NO_OWNER = 0,
	DRM_OWNED,
	V4L_OWNED
};

struct buffer {
	void *start;
	size_t length;

	int fence_fd;
	int dmabuf_fd;
	int v4l_index;
	int fb_id;

	enum owner owner;
	struct v4l2_plane planes[MAX_PLANES];
};

inline static void errno_print(const char *s)
{
	fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
}

void v4l2_init_dmabuf(int fd, int count, int type, struct buffer *buffers);
void v4l2_uninit_device(struct buffer *buffers, int count);
void v4l2_stop(int fd, enum v4l2_buf_type type);
void v4l2_start(int fd, enum v4l2_buf_type type);
void v4l2_set_fmt(int fd, int width, int height, enum v4l2_buf_type type, int pixel_format);

int v4l2_dequeue_buffer(int fd, struct v4l2_buffer *buf, int type);
void v4l2_queue_buffer(int fd, int index, int dmabuf_fd, int type);

static inline int v4l2_xioctl(int fh, int request, void *arg)
{
	int r;
	do {
		r = v4l2_ioctl(fh, request, arg);
	} while (r == -1 && ((errno == EINTR) || (errno == EAGAIN)));

	if (r == -1) {
		return r;
	}
	return 0;
}
