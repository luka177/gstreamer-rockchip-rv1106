#include <gst/base/gstpushsrc.h>
#include <gst/gst.h>

#include "gst/video/video-info.h"
#include "gstrkmpi_common.h"
#include "gstrkmpiallocator.h"
#include "rk_mpi_vi.h"
#include <rkaiq/common/rk_aiq.h>
#include <rkaiq/common/rk_aiq_types.h>
#include <rkaiq/uAPI2/rk_aiq_user_api2_sysctl.h>
#include <rkaiq/xcore/base/xcam_common.h>

typedef struct _GstRockchipVI {
  GstPushSrc parent;

  rk_aiq_sys_ctx_t *aiq_ctx;
  RK_S32 camera_id;

  VI_PIPE vi_pipe;
  VI_CHN vi_chn;
  VI_CHN_ATTR_S vi_config;

  gint unlocked;
  gint stopping;

  GstRkmpiAllocator *allocator;
} GstRockchipVI;

#define GST_TYPE_ROCKCHIP_VI (gst_rockchip_vi_get_type())
G_DECLARE_FINAL_TYPE(GstRockchipVI, gst_rockchip_vi, GST, ROCKCHIP_VI,
                     GstPushSrc)

struct _GstRockchipVIClass {
  GstPushSrcClass parent_class;
};
G_DEFINE_TYPE(GstRockchipVI, gst_rockchip_vi, GST_TYPE_PUSH_SRC)

static gboolean
gst_rockchip_vi_unlock (GstBaseSrc *src)
{
  GstRockchipVI *self = GST_ROCKCHIP_VI (src);
  GST_ERROR_OBJECT (self, "unlock(): disabling VI channel to unblock capture");

  g_atomic_int_set (&self->unlocked, 1);

  // this hopefully cause RK_MPI_VI_GetChnFrame() to return.
  RK_MPI_VI_DisableChn (self->vi_pipe, self->vi_chn);

  return TRUE;
}

static gboolean
gst_rockchip_vi_unlock_stop (GstBaseSrc *src)
{
  GstRockchipVI *self = GST_ROCKCHIP_VI (src);
  GST_ERROR_OBJECT (self, "unlock_stop(): clearing unlock state");

  g_atomic_int_set (&self->unlocked, 0);

  if (!g_atomic_int_get (&self->stopping)) {
    RK_S32 ret = RK_MPI_VI_EnableChn (self->vi_pipe, self->vi_chn);
    if (ret != RK_SUCCESS)
      GST_WARNING_OBJECT (self, "unlock_stop(): EnableChn failed: %d", ret);
  }

  return TRUE;
}

static gboolean gst_rockchip_vi_set_caps(GstBaseSrc *src, GstCaps *caps) {
  GstRockchipVI *self = GST_ROCKCHIP_VI(src);

  gboolean success = false;
  GST_OBJECT_LOCK(src);
  const GstStructure *caps_structure = gst_caps_get_structure(caps, 0);
  if (gst_structure_has_name(caps_structure, "video/x-raw")) {
    GstVideoInfo *vinf = gst_video_info_new_from_caps(caps);
    if (vinf) {
      u32 videoWidth = GST_VIDEO_INFO_WIDTH(vinf);
      u32 videoHeight = GST_VIDEO_INFO_HEIGHT(vinf);

      VI_CHN_ATTR_S *vichn_attr = &self->vi_config;
      memset(vichn_attr, 0, sizeof(*vichn_attr));
      vichn_attr->stIspOpt.u32BufCount = 2;
      vichn_attr->stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
      vichn_attr->enPixelFormat = RK_FMT_YUV420SP;
      vichn_attr->stIspOpt.stMaxSize.u32Width = videoWidth;
      vichn_attr->stIspOpt.stMaxSize.u32Height = videoHeight;
      vichn_attr->stSize.u32Width = videoWidth;
      vichn_attr->stSize.u32Height = videoHeight;
      vichn_attr->enCompressMode = COMPRESS_MODE_NONE; 
      vichn_attr->u32Depth = 2;
      // FIXME: this is wrong
      vichn_attr->stFrameRate.s32DstFrameRate = -1;
      vichn_attr->stFrameRate.s32SrcFrameRate = -1;
      vichn_attr->enVideoFormat =
          VIDEO_FORMAT_TILE_16x8; // FIXME: now negotiate this

      success = true;
    }
  }
  GST_OBJECT_UNLOCK(src);

  if (success) {
    // FIXME: move dis down
    GstRockchipVI *self = GST_ROCKCHIP_VI(src);
    self->camera_id = self->vi_chn = self->vi_pipe = 0; // FIXME

    rk_aiq_static_info_t aiq_static_info;
    rk_aiq_uapi2_sysctl_enumStaticMetasByPhyId(self->camera_id,
                                               &aiq_static_info);

    self->aiq_ctx =
        rk_aiq_uapi2_sysctl_init(aiq_static_info.sensor_info.sensor_name,
                                 "/oem/usr/share/iqfiles", NULL, NULL);
    // FIXME: dis is rotated 90deg
    rk_frame_fmt_t *preferred_fmt = &aiq_static_info.sensor_info.support_fmt[0];
    GST_INFO_OBJECT(self, "using sensor \"%s\", width=%d, height=%d, fps=%d",
                    aiq_static_info.sensor_info.sensor_name,
                    preferred_fmt->width, preferred_fmt->height,
                    preferred_fmt->fps);
    rk_aiq_uapi2_sysctl_prepare(self->aiq_ctx, preferred_fmt->width,
                                preferred_fmt->height, preferred_fmt->hdr_mode);
    rk_aiq_uapi2_sysctl_start(self->aiq_ctx);

    // Initialize the MPI system
    if (!gst_rkmpi_init()) {
      return FALSE;
    }

    // Set VI attributes and enable device
    VI_DEV_ATTR_S videv_attr;
    VI_DEV_BIND_PIPE_S stBindPipe;
	  memset(&stBindPipe, 0, sizeof(stBindPipe));
    memset(&videv_attr, 0, sizeof(videv_attr));
    RK_MPI_VI_GetDevAttr(self->camera_id, &videv_attr);
    videv_attr.enIntfMode = VI_MODE_MIPI_YUV420_NORMAL;
    RK_MPI_VI_SetDevAttr(self->camera_id, &videv_attr);
    int ret = RK_MPI_VI_GetDevIsEnable(self->camera_id);
	  if (ret != RK_SUCCESS) {
		    ret = RK_MPI_VI_EnableDev(self->camera_id);
		    if (ret != RK_SUCCESS) {
			    printf("RK_MPI_VI_EnableDev %x\n", ret);
			  return -1;
		  }
		  stBindPipe.u32Num = 1;
		  stBindPipe.PipeId[0] = self->camera_id;
		  ret = RK_MPI_VI_SetDevBindPipe(self->camera_id, &stBindPipe);
		  if (ret != RK_SUCCESS) {
			  printf("RK_MPI_VI_SetDevBindPipe %x\n", ret);
			  return FALSE;
		  }
	  } else {
		  printf("RK_MPI_VI_EnableDev already\n");
	  }

    // Setup VI Channel
    RK_MPI_VI_SetChnAttr(self->vi_pipe, self->vi_chn, &self->vi_config);
    RK_MPI_VI_EnableChn(self->vi_pipe, self->vi_chn);
  }

  return success;
}

static gboolean gst_rockchip_vi_start(GstBaseSrc *src) { return TRUE; }

static gboolean gst_rockchip_vi_stop(GstBaseSrc *src) {
  GstRockchipVI *self = GST_ROCKCHIP_VI(src);
  g_atomic_int_set(&self->stopping, 1);
   {
    const gint64 deadline = g_get_monotonic_time() + 1000 * G_TIME_SPAN_MILLISECOND;
    gint left = gst_rkmpi_allocator_vi_outstanding(self->allocator);
    while (g_get_monotonic_time() < deadline) {
      if (left == 0)
        break;
      g_usleep(2000);
    }
    if (left != 0) {
      GST_WARNING_OBJECT(self, "stopping with %d VI frame(s) still outstanding",
                         left);
    }
  }
  RK_MPI_VI_DisableChn(self->vi_pipe, self->vi_chn);
  RK_MPI_VI_DisableDev(self->camera_id);
  rk_aiq_uapi2_sysctl_stop(self->aiq_ctx, false);
  rk_aiq_uapi2_sysctl_deinit(self->aiq_ctx);
  gst_rkmpi_exit();
  return TRUE;
}

#define GET_FRAME_TIMEOUT_MS 20

static GstFlowReturn
gst_rockchip_vi_create (GstPushSrc *src, GstBuffer **buf)
{
  GstRockchipVI *self = GST_ROCKCHIP_VI (src);
  GstBaseSrc   *basesrc = GST_BASE_SRC (src);

  for (;;) {
    GstFlowReturn wait = gst_base_src_wait_playing (basesrc);
    if (G_UNLIKELY (wait != GST_FLOW_OK))
      return wait;

    if (G_UNLIKELY (g_atomic_int_get (&self->stopping) ||
                    g_atomic_int_get (&self->unlocked)))
      return GST_FLOW_FLUSHING;

    VIDEO_FRAME_INFO_S frame;
    RK_S32 rc = RK_MPI_VI_GetChnFrame (self->vi_pipe, self->vi_chn,
                                       &frame, GET_FRAME_TIMEOUT_MS);
    if (rc == RK_SUCCESS) {
      GstMemory *mem = gst_rkmpi_allocator_import_viframe (
          self->allocator, self->vi_pipe, self->vi_chn, &frame);
      if (G_UNLIKELY (!mem)) {
        RK_MPI_VI_ReleaseChnFrame (self->vi_pipe, self->vi_chn, &frame);
        return GST_FLOW_ERROR;
      }

      GstBuffer *buffer = gst_buffer_new ();
      gst_buffer_insert_memory (buffer, -1, mem);
      *buf = buffer;
      return GST_FLOW_OK;
    }

    if (G_UNLIKELY (g_atomic_int_get (&self->stopping) ||
                    g_atomic_int_get (&self->unlocked)))
      return GST_FLOW_FLUSHING;
  }
}

static GstStaticPadTemplate gst_rkmpivi_src_template = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw, format=(string)NV12"));

static void gst_rockchip_vi_class_init(GstRockchipVIClass *klass) {
  GstBaseSrcClass *base_src_class = GST_BASE_SRC_CLASS(klass);

  base_src_class->set_caps = GST_DEBUG_FUNCPTR(gst_rockchip_vi_set_caps);
  base_src_class->start = GST_DEBUG_FUNCPTR(gst_rockchip_vi_start);
  base_src_class->stop = GST_DEBUG_FUNCPTR(gst_rockchip_vi_stop);
  base_src_class->unlock = GST_DEBUG_FUNCPTR(gst_rockchip_vi_unlock);
  base_src_class->unlock_stop = GST_DEBUG_FUNCPTR(gst_rockchip_vi_unlock_stop);

  GstPushSrcClass *push_src_class = GST_PUSH_SRC_CLASS(klass);
  push_src_class->create = GST_DEBUG_FUNCPTR(gst_rockchip_vi_create);

  GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
  gst_element_class_set_static_metadata(element_class, "Rockchip Rockit VI src",
                                        "Source/Video",
                                        "Reads frames from RKMPI VI", "Nikita");

  gst_element_class_add_pad_template(
      element_class, gst_static_pad_template_get(&gst_rkmpivi_src_template));
}

static void gst_rockchip_vi_init(GstRockchipVI *self) {
  self->allocator = gst_rkmpi_allocator_new_empty();
   /* we operate in time */
  gst_base_src_set_format (GST_BASE_SRC (self), GST_FORMAT_TIME);
  gst_base_src_set_live (GST_BASE_SRC (self), TRUE);
}

gboolean gst_rkvi_plugin_register(GstPlugin *plugin) {
  return gst_element_register(plugin, "rkvisrc", GST_RANK_NONE,
                              GST_TYPE_ROCKCHIP_VI);
}
