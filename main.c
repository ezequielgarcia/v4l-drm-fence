
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>

#include "videodev2.h"
#include "drm.h"
#include "v4l2.h"

static const char *dri_path = "/dev/dri/card0";
static const char *v4l2_path = "/dev/video0";
static struct buffer buffers[BUFCOUNT];
static struct buffer *front_buffer, *back_buffer;
static int debug = 1;

#define error(fmt, arg...)		\
do {					\
	printf("ERROR: " fmt, ## arg);	\
} while (0);				\

#define debug(fmt, arg...)		\
do {					\
	if (debug) {			\
		printf(fmt, ## arg);	\
	}				\
} while (0);				\

static struct buffer *find_buffer_from_v4l_index(int index)
{
	int i;

	for (i = 0; i < BUFCOUNT; i++)
		if (buffers[i].v4l_index == index)
			return &buffers[i];
	return NULL;
}

static void page_flip_handler(int fd, unsigned int frame,
			    unsigned int sec, unsigned int usec,
			    void *data)
{
	struct drm_dev_t *dev = data;
	struct buffer *buf = front_buffer;

	if (back_buffer) {
		/* Back-buffer is now Front-buffer. And former front-buffer
		 * is now idle and can be queued to V4L.
		 */
		debug("Buffer rendered: fd=%d, index=%d\n",
			back_buffer->dmabuf_fd, back_buffer->v4l_index);
		front_buffer = back_buffer;
		back_buffer = NULL;
	
		v4l2_queue_buffer(dev->v4l2_fd, buf->v4l_index,
				buf->dmabuf_fd,
				V4L2_BUF_TYPE_VIDEO_CAPTURE);
		buf->owner = V4L_OWNED;
	}
}

static void handle_new_buffer(int v4l2_fd, int drm_fd, struct drm_dev_t *dev)
{
	struct v4l2_buffer v4l_buf;
	struct buffer *buf;
	int dequeued;

	dequeued = v4l2_dequeue_buffer(v4l2_fd, &v4l_buf, V4L2_BUF_TYPE_VIDEO_CAPTURE);
	if (!dequeued)
		return;

	buf = find_buffer_from_v4l_index(v4l_buf.index);
	if (!buf) {
		error("Buffer captured index=%d, not found!\n",
			v4l_buf.index);
		return;
	}

	debug("Buffer captured: fd=%d, index=%d\n",
		buf->dmabuf_fd, buf->v4l_index);

	/* Page-flip will happen on the next vertical blank.
	 * This is a non-blocking, schedule operation.
	 */
	if (!back_buffer) {
		drm_render_atomic(drm_fd, buf->fb_id, dev);
		back_buffer = buf; 
		buf->owner = DRM_OWNED;
	} else {
		error("Display busy, dropping captured frame!\n");

		/* Display busy, drop the frame and simply queue it back. */
		v4l2_queue_buffer(dev->v4l2_fd, buf->v4l_index,
			buf->dmabuf_fd,
			V4L2_BUF_TYPE_VIDEO_CAPTURE);
	}
}

static void mainloop(int v4l2_fd, int drm_fd, struct drm_dev_t *dev)
{
	drmEventContext ev;
	int r;

	struct pollfd fds[] = {
		{ .fd = STDIN_FILENO, .events = POLLIN },
		{ .fd = v4l2_fd, .events = POLLIN },
		{ .fd = drm_fd, .events = POLLIN },
	};

	memset(&ev, 0, sizeof(ev));
	ev.version = 2;
	ev.page_flip_handler = page_flip_handler;

	while (1) {
		/* Wait until there is something to do */
		r = poll(fds, 3, 3000);
		if (-1 == r) {
			if (EINTR == errno)
				continue;
			error("error in poll %d", errno);
			return;
		}

		if (0 == r) {
			error("timeout in poll\n");
			return;
		}

		if (fds[0].revents & POLLIN) {
			printf("User requested exit\n");
			return;
		}

		if (fds[1].revents & POLLIN) {
			handle_new_buffer(v4l2_fd, drm_fd, dev);
		}

		if (fds[2].revents & POLLIN) {
			drmHandleEvent(drm_fd, &ev);
		}
	}
}

int main()
{
	struct drm_dev_t *dev_head, *dev;
	int v4l2_fd, drm_fd;
	int i;

	drm_fd = drm_open(dri_path, 1, 1);
	dev_head = drm_init(drm_fd);

	if (dev_head == NULL) {
		error("available drm_dev not found\n");
		return EXIT_FAILURE;
	}

	dev = dev_head;

	/* This creates four dmabuf exported buffers,
	 * and then renders index-0.
	 */
	drm_setup_fb(drm_fd, dev, 0, 1);

	for (i = 0; i < BUFCOUNT; i++) {
		buffers[i].dmabuf_fd = dev->bufs[i].dmabuf_fd;
		buffers[i].fb_id = dev->bufs[i].fb_id;
	}

	/* drm_setup_fb() renders the first frame,
	 * so it becomes the front buffer.
	 */
	buffers[0].owner = DRM_OWNED;
	front_buffer = &buffers[0];
	back_buffer = NULL;

	/*
	 * This is just a demo, so there's no modesetting.
	 * The program assumes DRM will start on 640x480.
	 * It's easily fixable, by adding some modesetting code
	 * in the DRM side.
	 */
	v4l2_fd = v4l2_open(v4l2_path, O_RDWR | O_NONBLOCK);
	v4l2_set_fmt(v4l2_fd, 640, 480, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_PIX_FMT_BGR32);
	v4l2_init_dmabuf(v4l2_fd, BUFCOUNT, V4L2_BUF_TYPE_VIDEO_CAPTURE, buffers);

	/* index-0 starts owned by DRM, queue the remaining to V4L */
	for (i = 1; i < BUFCOUNT; ++i) {
		v4l2_queue_buffer(v4l2_fd, i, buffers[i].dmabuf_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE);
		buffers[i].owner = V4L_OWNED;
	}
	v4l2_start(v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE);

	dev->v4l2_fd = v4l2_fd;
	dev->drm_fd = drm_fd;

	mainloop(v4l2_fd, drm_fd, dev);
	drm_destroy(drm_fd, dev_head);
	return 0;
}
