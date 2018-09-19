#define _GNU_SOURCE
#define _XOPEN_SOURCE 701

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libdrm/drm.h>
#include <libdrm/drm_fourcc.h>
#include "drm.h"

#define BPP 32

static int eopen(const char *path, int flag)
{
	int fd;

	if ((fd = open(path, flag)) < 0) {
		fprintf(stderr, "cannot open \"%s\"\n", path);
		error("open");
	}
	return fd;
}

static void *emmap(int addr, size_t len, int prot, int flag, int fd, off_t offset)
{
	uint32_t *fp;

	if ((fp = (uint32_t *) mmap(0, len, prot, flag, fd, offset)) == MAP_FAILED)
		error("mmap");
	return fp;
}

static int get_plane_id(int drm_fd, struct drm_dev_t *dev)
{
	drmModePlaneResPtr plane_resources;
	uint32_t i, j;
	int ret = -EINVAL;
	int found_primary = 0;

	plane_resources = drmModeGetPlaneResources(drm_fd);
	if (!plane_resources) {
		printf("drmModeGetPlaneResources failed: %s\n", strerror(errno));
		return -1;
	}

	for (i = 0; (i < plane_resources->count_planes) && !found_primary; i++) {
		uint32_t id = plane_resources->planes[i];
		drmModePlanePtr plane = drmModeGetPlane(drm_fd, id);
		if (!plane) {
			printf("drmModeGetPlane(%u) failed: %s\n", id, strerror(errno));
			continue;
		}

		printf("plane id: %d for 0x%x\n", id, plane->possible_crtcs);

		if (plane->possible_crtcs & (1 << dev->crtc_index)) {
			drmModeObjectPropertiesPtr props =
				drmModeObjectGetProperties(drm_fd, id, DRM_MODE_OBJECT_PLANE);

			/* primary or not, this plane is good enough to use: */
			ret = id;

			for (j = 0; j < props->count_props; j++) {
				drmModePropertyPtr p =
					drmModeGetProperty(drm_fd, props->props[j]);

				if ((strcmp(p->name, "type") == 0) &&
						(props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY)) {
					/* found our primary plane, lets use that: */
					found_primary = 1;
				}

				drmModeFreeProperty(p);
			}

			drmModeFreeObjectProperties(props);
		}

		drmModeFreePlane(plane);
	}

	drmModeFreePlaneResources(plane_resources);

	return ret;
}

static int add_plane_property(struct drm_dev_t *dev,
		drmModeAtomicReq *req, uint32_t obj_id,
		const char *name, uint64_t value)
{
        struct plane *obj = dev->plane;
        unsigned int i;
        int prop_id = -1;

        for (i = 0 ; i < obj->props->count_props ; i++) {
                if (strcmp(obj->props_info[i]->name, name) == 0) {
                        prop_id = obj->props_info[i]->prop_id;
                        break;
                }
        }


        if (prop_id < 0) {
                printf("no plane property: %s\n", name);
                return -EINVAL;
        }

        return drmModeAtomicAddProperty(req, obj_id, prop_id, value);
}

void drm_render_atomic(int drm_fd, int fb_id, struct drm_dev_t *dev)
{
        drmModeAtomicReq *req;
        uint32_t plane_id = dev->plane_id;
        int ret;

        req = drmModeAtomicAlloc();

        add_plane_property(dev, req, plane_id, "FB_ID", fb_id);
        add_plane_property(dev, req, plane_id, "CRTC_ID", dev->crtc_id);
        add_plane_property(dev, req, plane_id, "SRC_X", 0);
        add_plane_property(dev, req, plane_id, "SRC_Y", 0);
        add_plane_property(dev, req, plane_id, "SRC_W", dev->width << 16);
        add_plane_property(dev, req, plane_id, "SRC_H", dev->height << 16);
        add_plane_property(dev, req, plane_id, "CRTC_X", 0);
        add_plane_property(dev, req, plane_id, "CRTC_Y", 0);
        add_plane_property(dev, req, plane_id, "CRTC_W", dev->width);
        add_plane_property(dev, req, plane_id, "CRTC_H", dev->height);

        ret = drmModeAtomicCommit(drm_fd, req, DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK, dev);
	if (ret)
		printf("DRM: Failed drmModeAtomicCommit %d\n", ret);	

	drmModeAtomicFree(req);
}

void drm_render_legacy(int drm_fd, int fb_id, struct drm_dev_t *dev)
{
	drmModePageFlip(drm_fd, dev->crtc_id, fb_id,
		DRM_MODE_PAGE_FLIP_EVENT, dev);
}

int drm_open(const char *path, int need_dumb, int need_prime)
{
	int fd, flags;
	uint64_t has_it;

	fd = eopen(path, O_RDWR);

	/* set FD_CLOEXEC flag */
	if ((flags = fcntl(fd, F_GETFD)) < 0
		|| fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
		fatal("fcntl FD_CLOEXEC failed");

	if (need_dumb) {
		if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_it) < 0)
			error("drmGetCap DRM_CAP_DUMB_BUFFER failed!");
		if (has_it == 0)
			fatal("can't give us dumb buffers");
	}

	if (need_prime) {
		/* check prime */
		if (drmGetCap(fd, DRM_CAP_PRIME, &has_it) < 0)
			error("drmGetCap DRM_CAP_PRIME failed!");
		if (!(has_it & DRM_PRIME_CAP_EXPORT))
			fatal("can't export dmabuf");
	}

	return fd;
}

struct drm_dev_t *drm_init(int fd)
{
	int i, m, ret;
	struct drm_dev_t *dev = NULL, *dev_head = NULL;
	drmModeRes *res;
	drmModeConnector *conn;
	drmModeEncoder *enc;
	drmModeModeInfo *mode = NULL, *preferred = NULL;

	if ((res = drmModeGetResources(fd)) == NULL)
		fatal("drmModeGetResources() failed");

	/* find all available connectors */
	for (i = 0; i < res->count_connectors; i++) {
		conn = drmModeGetConnector(fd, res->connectors[i]);

		if (conn != NULL && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
			dev = (struct drm_dev_t *) malloc(sizeof(struct drm_dev_t));
			memset(dev, 0, sizeof(struct drm_dev_t));

			/* find preferred mode */
			for (m = 0; m < conn->count_modes; m++) {
				mode = &conn->modes[m];
				if (mode->type & DRM_MODE_TYPE_PREFERRED)
					preferred = mode;
				fprintf(stdout, "mode: %dx%d %s\n", mode->hdisplay, mode->vdisplay, mode->type & DRM_MODE_TYPE_PREFERRED ? "*" : "");
			}

			if (!preferred)
				preferred = &conn->modes[0];

			dev->conn_id = conn->connector_id;
			dev->enc_id = conn->encoder_id;
			dev->next = NULL;

			memcpy(&dev->mode, preferred, sizeof(drmModeModeInfo));
			dev->width = preferred->hdisplay;
			dev->height = preferred->vdisplay;

			/* FIXME: use default encoder/crtc pair */
			if ((enc = drmModeGetEncoder(fd, dev->enc_id)) == NULL)
				fatal("drmModeGetEncoder() faild");
			dev->crtc_id = enc->crtc_id;
			drmModeFreeEncoder(enc);

			dev->saved_crtc = NULL;

			/* create dev list */
			dev->next = dev_head;
			dev_head = dev;
		}
		drmModeFreeConnector(conn);
	}

        for (i = 0; i < res->count_crtcs; i++) {
		if (res->crtcs[i] == dev->crtc_id) {
			dev->crtc_index = i;
			break;
		}
	}

	drmModeFreeResources(res);

	ret = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
	if (ret) {
		printf("DRM: no atomic modesetting support: %s\n", strerror(errno));
		return NULL;
	}

	ret = get_plane_id(fd, dev);
	if (ret < 0) {
		printf("DRM: could not find a suitable plane for CRTC %d\n", dev->crtc_id);
		return NULL;
	} else {
		dev->plane_id = ret;
	}

	printf("DRM: connector id:%d\n", dev->conn_id);
	printf("DRM: plane id: %d encoder id:%d crtc id:%d\n", dev->plane_id, dev->enc_id, dev->crtc_id);
	printf("DRM: width:%d height:%d\n", dev->width, dev->height);

	/* We only do single plane to single crtc to single connector, no
	 * fancy multi-monitor or multi-plane stuff.  So just grab the
	 * plane/crtc/connector property info for one of each:
	 */
	dev->plane = calloc(1, sizeof(*dev->plane));
	dev->crtc = calloc(1, sizeof(*dev->crtc));
	dev->connector = calloc(1, sizeof(*dev->connector));

#define get_resource(type, Type, id) do { 					\
		dev->type->type = drmModeGet##Type(fd, id);			\
		if (!dev->type->type) {						\
			printf("could not get %s %i: %s\n",			\
					#type, id, strerror(errno));		\
			return NULL;						\
		}								\
	} while (0)

	get_resource(plane, Plane, dev->plane_id);
	get_resource(crtc, Crtc, dev->crtc_id);
	get_resource(connector, Connector, dev->conn_id);

#define get_properties(type, TYPE, id) do {					\
		uint32_t i;							\
		dev->type->props = drmModeObjectGetProperties(fd,		\
				id, DRM_MODE_OBJECT_##TYPE);			\
		if (!dev->type->props) {						\
			printf("could not get %s %u properties: %s\n", 		\
					#type, id, strerror(errno));		\
			return NULL;						\
		}								\
		dev->type->props_info = calloc(dev->type->props->count_props,	\
				sizeof(dev->type->props_info));			\
		for (i = 0; i < dev->type->props->count_props; i++) {		\
			dev->type->props_info[i] = drmModeGetProperty(fd,	\
					dev->type->props->props[i]);		\
		}								\
	} while (0)

	get_properties(plane, PLANE, dev->plane_id);
	get_properties(crtc, CRTC, dev->crtc_id);
	get_properties(connector, CONNECTOR, dev->conn_id);

	return dev_head;
}

static void drm_setup_buffer(int fd, struct drm_dev_t *dev,
		int width, int height,
		struct drm_buffer_t *buffer, int map, int export)
{
	struct drm_mode_create_dumb create_req;
	struct drm_mode_map_dumb map_req;

	buffer->dmabuf_fd = -1;

	memset(&create_req, 0, sizeof(struct drm_mode_create_dumb));
	create_req.width = width;
	create_req.height = height;
	create_req.bpp = BPP;

	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req) < 0)
		fatal("drmIoctl DRM_IOCTL_MODE_CREATE_DUMB failed");

	buffer->pitch = create_req.pitch;
	buffer->size = create_req.size;
	/* GEM buffer handle */
	buffer->bo_handle = create_req.handle;

	if (export) {
		int ret;

		ret = drmPrimeHandleToFD(fd, buffer->bo_handle,
			DRM_CLOEXEC | DRM_RDWR, &buffer->dmabuf_fd);
		if (ret < 0)
			fatal("could not export the dump buffer");
		printf("DRM buffer exported as fd=%d\n", buffer->dmabuf_fd);
	}

	if (map) {
		memset(&map_req, 0, sizeof(struct drm_mode_map_dumb));
		map_req.handle = buffer->bo_handle;

		if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req))
			fatal("drmIoctl DRM_IOCTL_MODE_MAP_DUMB failed");
		buffer->buf = (uint32_t *) emmap(0, buffer->size,
			PROT_READ | PROT_WRITE, MAP_SHARED,
			fd, map_req.offset);
		printf("DRM buffer mapped as %p\n", buffer->buf);
	}
}

void drm_setup_dummy(int fd, struct drm_dev_t *dev, int map, int export)
{
	int i;

	for (i = 0; i < BUFCOUNT; i++)
		drm_setup_buffer(fd, dev, dev->width, dev->height,
				 &dev->bufs[i], map, export);

	/* Assume all buffers have the same pitch */
	dev->pitch = dev->bufs[0].pitch;
	printf("DRM: buffer pitch = %d bytes\n", dev->pitch);
}

void drm_setup_fb(int fd, struct drm_dev_t *dev, int map, int export)
{
	int i;

	for (i = 0; i < BUFCOUNT; i++) {
		uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
		int ret;

		drm_setup_buffer(fd, dev, dev->width, dev->height,
				 &dev->bufs[i], map, export);
                handles[0] = dev->bufs[i].bo_handle;
                pitches[0] = dev->bufs[i].pitch;
		ret = drmModeAddFB2(fd, dev->width, dev->height, DRM_FORMAT_ARGB8888,
				handles, pitches, offsets, &dev->bufs[i].fb_id, 0);
		if (ret)
			fatal("drmModeAddFB2 failed");
	}

	/* Assume all buffers have the same pitch */
	dev->pitch = dev->bufs[0].pitch;
	printf("DRM: buffer pitch %d bytes\n", dev->pitch);

	dev->saved_crtc = drmModeGetCrtc(fd, dev->crtc_id); /* must store crtc data */

	/* First buffer goes to DRM */
	if (drmModeSetCrtc(fd, dev->crtc_id, dev->bufs[0].fb_id, 0, 0, &dev->conn_id, 1, &dev->mode))
		fatal("drmModeSetCrtc() failed");
}

void drm_destroy(int fd, struct drm_dev_t *dev_head)
{
	struct drm_dev_t *devp, *devp_tmp;
	int i;

	for (devp = dev_head; devp != NULL;) {
		if (devp->saved_crtc) {
			drmModeSetCrtc(fd, devp->saved_crtc->crtc_id, devp->saved_crtc->buffer_id,
				devp->saved_crtc->x, devp->saved_crtc->y, &devp->conn_id, 1, &devp->saved_crtc->mode);
			drmModeFreeCrtc(devp->saved_crtc);
		}

		for (i = 0; i < BUFCOUNT; i++) {
			struct drm_mode_destroy_dumb dreq = { .handle = devp->bufs[i].bo_handle };

			if (devp->bufs[i].buf)
				munmap(devp->bufs[i].buf, devp->bufs[i].size);
			if (devp->bufs[i].dmabuf_fd >= 0)
				close(devp->bufs[i].dmabuf_fd);
			drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
			drmModeRmFB(fd, devp->bufs[i].fb_id);
		}

		devp_tmp = devp;
		devp = devp->next;
		free(devp_tmp);
	}

	close(fd);
}
