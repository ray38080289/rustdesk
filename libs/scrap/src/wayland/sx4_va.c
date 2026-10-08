// sx4: a PipeWire dma-buf sample -> NV12 in caller memory through VA-API VideoProc.
//
// The compositor's buffer (AFBC or linear RGB) is imported as a VA surface and converted by the
// VA driver's VideoProc into an NV12 surface; that surface is exported and read through a cached
// CPU mapping. GL (glupload/glcolorconvert/gldownload) would leave the result in write-combined GPU
// memory that the CPU reads at a fraction of the speed; this keeps the CPU to one plain copy.
// Written against GStreamer 1.14 / libva 2.1 headers (the build container); the caps are parsed by
// hand because the DMA_DRM helpers are newer than that.

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gst/allocators/gstdmabuf.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

#ifndef VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2
#define VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 0x40000000
#endif

struct dma_buf_sync { uint64_t flags; };
#define DMA_BUF_SYNC_READ (1 << 0)
#define DMA_BUF_SYNC_START (0 << 2)
#define DMA_BUF_SYNC_END (1 << 2)
#define DMA_BUF_IOCTL_SYNC _IOW('b', 0, struct dma_buf_sync)

static struct {
	int fd;
	VADisplay dpy;
	VAConfigID cfg;
	VAContextID ctx;
	VASurfaceID out;
	int w, h, ready, failed;
	/* the exported NV12 output, mapped once */
	ino_t ino;
	int map_fd;
	uint8_t *map;
	size_t map_len;
	uint32_t off[2], pitch[2];
} va = { .fd = -1, .map_fd = -1 };

static void sx4_va_drop_output(void)
{
	if (va.map)
		munmap(va.map, va.map_len);
	if (va.map_fd >= 0)
		close(va.map_fd);
	va.map = NULL;
	va.map_fd = -1;
	va.ino = 0;
	if (va.ready) {
		vaDestroyContext(va.dpy, va.ctx);
		vaDestroySurfaces(va.dpy, &va.out, 1);
	}
	va.ready = 0;
}

static int sx4_va_setup(int w, int h)
{
	VAStatus st;
	int major, minor;

	if (va.ready && va.w == w && va.h == h)
		return 0;
	if (!va.dpy) {
		va.fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
		if (va.fd < 0)
			return -1;
		va.dpy = vaGetDisplayDRM(va.fd);
		if (!va.dpy || vaInitialize(va.dpy, &major, &minor) != VA_STATUS_SUCCESS)
			return -1;
		if (vaCreateConfig(va.dpy, VAProfileNone, VAEntrypointVideoProc, NULL, 0, &va.cfg) !=
		    VA_STATUS_SUCCESS)
			return -1;
	}
	sx4_va_drop_output();
	st = vaCreateSurfaces(va.dpy, VA_RT_FORMAT_YUV420, w, h, &va.out, 1, NULL, 0);
	if (st != VA_STATUS_SUCCESS)
		return -1;
	st = vaCreateContext(va.dpy, va.cfg, w, h, VA_PROGRESSIVE, &va.out, 1, &va.ctx);
	if (st != VA_STATUS_SUCCESS) {
		vaDestroySurfaces(va.dpy, &va.out, 1);
		return -1;
	}
	va.w = w;
	va.h = h;
	va.ready = 1;
	return 0;
}

/* "AR24:0x0800000000000062" or "XR24" (linear) */
static int sx4_va_drm_format(const char *s, uint32_t *fourcc, uint64_t *modifier)
{
	if (!s || strlen(s) < 4)
		return -1;
	*fourcc = (uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16 | (uint32_t)s[3] << 24;
	*modifier = s[4] == ':' ? strtoull(s + 5, NULL, 16) : 0;
	return 0;
}

/* the VideoProc result is in an NV12 surface the driver renders into; export and map it once */
static int sx4_va_map_output(void)
{
	VADRMPRIMESurfaceDescriptor d;
	struct stat sb;
	int i;

	memset(&d, 0, sizeof(d));
	if (vaExportSurfaceHandle(va.dpy, va.out, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
				  VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
				  &d) != VA_STATUS_SUCCESS)
		return -1;
	for (i = 1; i < (int)d.num_objects; i++)
		close(d.objects[i].fd);
	if (fstat(d.objects[0].fd, &sb) == 0 && va.map && sb.st_ino == va.ino) {
		close(d.objects[0].fd);
		return 0;
	}
	if (va.map)
		munmap(va.map, va.map_len);
	if (va.map_fd >= 0)
		close(va.map_fd);
	va.map_fd = d.objects[0].fd;
	va.map_len = d.objects[0].size;
	va.map = mmap(NULL, va.map_len, PROT_READ, MAP_SHARED, va.map_fd, 0);
	if (va.map == MAP_FAILED) {
		va.map = NULL;
		return -1;
	}
	va.ino = sb.st_ino;
	va.off[0] = d.layers[0].offset[0];
	va.pitch[0] = d.layers[0].pitch[0];
	va.off[1] = d.layers[0].num_planes > 1 ? d.layers[0].offset[1] : d.layers[1].offset[0];
	va.pitch[1] = d.layers[0].num_planes > 1 ? d.layers[0].pitch[1] : d.layers[1].pitch[0];
	return 0;
}

/* 0: out (w * h * 3 / 2) holds the frame as NV12 with stride w; < 0: not done */
int sx4_va_nv12_from_sample(GstSample *sample, uint8_t *out, size_t out_len, int w, int h)
{
	VADRMPRIMESurfaceDescriptor desc;
	VASurfaceAttrib attribs[2];
	VAProcPipelineParameterBuffer params;
	VASurfaceID in;
	VABufferID buf;
	GstBuffer *gbuf = gst_sample_get_buffer(sample);
	GstCaps *caps = gst_sample_get_caps(sample);
	GstVideoMeta *meta;
	GstMemory *mem;
	struct dma_buf_sync sync;
	uint32_t fourcc;
	uint64_t modifier;
	int fd, y, ok;
	guint i;

	if (va.failed || !gbuf || !caps || out_len < (size_t)w * h * 3 / 2 || (w & 1) || (h & 1))
		return -1;
	if (sx4_va_drm_format(gst_structure_get_string(gst_caps_get_structure(caps, 0), "drm-format"),
			      &fourcc, &modifier))
		return -2;
	mem = gst_buffer_peek_memory(gbuf, 0);
	if (!gst_is_dmabuf_memory(mem))
		return -3;
	fd = gst_dmabuf_memory_get_fd(mem);
	if (sx4_va_setup(w, h)) {
		va.failed = 1;	/* no VA-API VideoProc here: never try again */
		fprintf(stderr, "sx4_va: VideoProc unavailable\n");
		return -4;
	}

	memset(&desc, 0, sizeof(desc));
	desc.fourcc = fourcc;
	desc.width = w;
	desc.height = h;
	desc.num_objects = 1;
	desc.objects[0].fd = fd;
	desc.objects[0].size = (uint32_t)lseek(fd, 0, SEEK_END);
	desc.objects[0].drm_format_modifier = modifier;
	desc.num_layers = 1;
	desc.layers[0].drm_format = fourcc;
	meta = gst_buffer_get_video_meta(gbuf);
	desc.layers[0].num_planes = meta ? meta->n_planes : 1;
	for (i = 0; i < desc.layers[0].num_planes && i < 4; i++) {
		desc.layers[0].object_index[i] = 0;
		desc.layers[0].offset[i] = meta ? (uint32_t)meta->offset[i] + (uint32_t)mem->offset : 0;
		desc.layers[0].pitch[i] = meta ? (uint32_t)meta->stride[i] : (uint32_t)w * 4;
	}
	attribs[0].type = VASurfaceAttribMemoryType;
	attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
	attribs[0].value.type = VAGenericValueTypeInteger;
	attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
	attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
	attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
	attribs[1].value.type = VAGenericValueTypePointer;
	attribs[1].value.value.p = &desc;
	if (vaCreateSurfaces(va.dpy, VA_RT_FORMAT_RGB32, w, h, &in, 1, attribs, 2) != VA_STATUS_SUCCESS)
		return -5;

	/* BT.601 limited, as libyuv's ARGBToNV12 on the stock path */
	memset(&params, 0, sizeof(params));
	params.surface = in;
	params.surface_color_standard = VAProcColorStandardNone;
	params.output_color_standard = VAProcColorStandardBT601;
	params.filter_flags = VA_FILTER_SCALING_FAST;
	ok = vaBeginPicture(va.dpy, va.ctx, va.out) == VA_STATUS_SUCCESS &&
	     vaCreateBuffer(va.dpy, va.ctx, VAProcPipelineParameterBufferType, sizeof(params), 1,
			    &params, &buf) == VA_STATUS_SUCCESS;
	if (ok) {
		ok = vaRenderPicture(va.dpy, va.ctx, &buf, 1) == VA_STATUS_SUCCESS;
		ok = vaEndPicture(va.dpy, va.ctx) == VA_STATUS_SUCCESS && ok;
		vaDestroyBuffer(va.dpy, buf);
	}
	ok = ok && vaSyncSurface(va.dpy, va.out) == VA_STATUS_SUCCESS;
	vaDestroySurfaces(va.dpy, &in, 1);
	if (!ok || sx4_va_map_output())
		return -6;

	sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
	ioctl(va.map_fd, DMA_BUF_IOCTL_SYNC, &sync);
	for (y = 0; y < h; y++)
		memcpy(out + (size_t)y * w, va.map + va.off[0] + (size_t)y * va.pitch[0], w);
	for (y = 0; y < h / 2; y++)
		memcpy(out + (size_t)w * h + (size_t)y * w, va.map + va.off[1] + (size_t)y * va.pitch[1], w);
	sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
	ioctl(va.map_fd, DMA_BUF_IOCTL_SYNC, &sync);
	return 0;
}
