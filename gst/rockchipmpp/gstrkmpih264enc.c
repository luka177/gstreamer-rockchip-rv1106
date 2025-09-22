// License: GPLv3
#include <gst/gst.h>
#include <gst/video/gstvideoencoder.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gstrkmpi_common.h"
#include "gstrkmpiallocator.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_mmz.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"

static uint64_t monotonic_micros() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)(ts.tv_sec * 1000 * 1000) + (ts.tv_nsec / 1000);
}

#define GST_CAT_DEFAULT gstrkpmpih264
GST_DEBUG_CATEGORY(GST_CAT_DEFAULT);

typedef struct _GstRKMPIH264Enc GstRKMPIH264Enc;
typedef struct _GstRKMPIH264EncClass GstRKMPIH264EncClass;

#define chnId 0

// We need to do this dance because g_async_queue_push cannot accept NULL
// pointers
struct QueuedGstFrame {
  GstVideoCodecFrame *frame_or_null;
  uint32_t u32SeqNo;
};

static struct QueuedGstFrame *queued_gst_frame_new(GstVideoCodecFrame *frame,
                                                   uint32_t seqno) {
  struct QueuedGstFrame *ret = g_malloc(sizeof(struct QueuedGstFrame));
  ret->frame_or_null = frame;
  ret->u32SeqNo = seqno;
  return ret;
}

struct _GstRKMPIH264Enc {
  // Gstreamer
  GstVideoEncoder parent;
  GstVideoCodecState *state;
  GstVideoInfo info;

  // config
  guint bitrate_kbps;
  guint gop_count;

  // What seqno is that of the next buffer?
  _Atomic uint32_t input_frame_counter;
  /// Type: QueuedGstFrame
  GAsyncQueue *gstframe_queue;
  //Stopping
  gint stopping;
};

struct _GstRKMPIH264EncClass {
  GstVideoEncoderClass parent_class;
};

enum {
  PROP_0,
  PROP_BITRATE,
  PROP_GOP,
  N_PROPERTIES
};

#define RK_ALIGN(x, a) (((x) + (a)-1) & ~((a)-1))
#define RK_ALIGN_2(x) RK_ALIGN(x, 2)

static GParamSpec *obj_properties[N_PROPERTIES];

G_DEFINE_TYPE(GstRKMPIH264Enc, gst_rkmpi_h264enc, GST_TYPE_VIDEO_ENCODER)
#define GST_TYPE_RKMPIH264ENC (gst_rkmpi_h264enc_get_type())
#define GST_RKMPIH264ENC(obj)                                                  \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_RKMPIH264ENC, GstRKMPIH264Enc))
#define GST_RKMPIH264ENC_CLASS(klass)                                          \
  (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_RKMPIH264ENC,                     \
                           GstRKMPIH264EncClass))
#define GST_IS_RKMPIH264ENC(obj)                                               \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_RKMPIH264ENC))
#define GST_IS_RKMPIH264ENC_CLASS(klass)                                       \
  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_RKMPIH264ENC))
#define GST_RKMPIH264ENC_GET_CLASS(obj)                                        \
  (G_TYPE_INSTANCE_GET_CLASS((obj), GST_TYPE_RKMPIH264ENC,                     \
                             GstRKMPIH264EncClass))

#define GST_RK_ALIST_XMACRO(X, XLAST)                                          \
  X(NV12, RK_FMT_YUV420SP)                                                     \
  X(NV12_10LE32, RK_FMT_YUV420SP_10BIT)                                        \
  X(NV16, RK_FMT_YUV422SP)                                                     \
  X(P010_10LE, RK_FMT_YUV422SP_10BIT)                                          \
  X(I420, RK_FMT_YUV420P)                                                      \
  X(YV12, RK_FMT_YUV420P_VU)                                                   \
  X(NV21, RK_FMT_YUV420SP_VU)                                                  \
  X(Y42B, RK_FMT_YUV422P)                                                      \
  X(VYUY, RK_FMT_YUV422SP_VU)                                                  \
  X(YUY2, RK_FMT_YUV422_YUYV)                                                  \
  X(UYVY, RK_FMT_YUV422_UYVY)                                                  \
  X(GRAY8, RK_FMT_YUV400SP)                                                    \
  X(Y444, RK_FMT_YUV444)                                                       \
  X(RGB16, RK_FMT_RGB565)                                                      \
  X(BGR16, RK_FMT_BGR565)                                                      \
  X(RGB15, RK_FMT_RGB555)                                                      \
  X(BGR15, RK_FMT_BGR555)                                                      \
  X(RGB, RK_FMT_RGB888)                                                        \
  X(BGR, RK_FMT_BGR888)                                                        \
  X(ARGB, RK_FMT_ARGB8888)                                                     \
  X(ABGR, RK_FMT_ABGR8888)                                                     \
  X(BGRA, RK_FMT_BGRA8888)                                                     \
  X(RGBA, RK_FMT_RGBA8888)                                                     \
  X(YVYU, RK_FMT_YUV422_YVYU)                                                  \
  X(VYUY, RK_FMT_YUV422_VYUY)                                                  \
  X(NV16, RK_FMT_YUV422SP)                                                     \
  X(NV61, RK_FMT_YUV422SP_VU)                                                  \
  X(NV24, RK_FMT_YUV444SP)                                                     \
  X(RGB16, RK_FMT_RGB565)                                                      \
  XLAST(BGR16, RK_FMT_BGR565)
// FIXME: there are some missing entries. Also, some of these might be
// ChatGPT hallucinated
// FIXME: Bayer formats

#define GST_RK_ALIST_E(gst, rk) {GST_VIDEO_FORMAT_##gst, rk},
static struct gst_rkmpi_format {
  GstVideoFormat gst_format;
  PIXEL_FORMAT_E rkmpi_format;
  const char *gst_string;
} GST_RKMPI_FORMAT_ALIST[] = {
    GST_RK_ALIST_XMACRO(GST_RK_ALIST_E, GST_RK_ALIST_E)};

#define GST_RKMPI_H264ENC_SIZE_CAPS                                            \
  "width  = (int) [ 96, MAX ], height = (int) [ 64, MAX ]"
static GstStaticPadTemplate gst_rkmpi_h264enc_src_template =
    GST_STATIC_PAD_TEMPLATE(
        "src", GST_PAD_SRC, GST_PAD_ALWAYS,
        GST_STATIC_CAPS("video/x-h264, " GST_RKMPI_H264ENC_SIZE_CAPS ","
                        "stream-format = (string) { byte-stream }, "
                        "alignment = (string) { au }, "
                        "profile = (string) { baseline, main, high }"));
#define GST_RK_ALIST_CAPS_E(gst, rk) #gst ", "
#define GST_RK_ALIST_CAPS_L(gst, rk) #gst
static GstStaticPadTemplate gst_rkmpih264enc_sink_template =
    GST_STATIC_PAD_TEMPLATE(
        "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
        GST_STATIC_CAPS(
            "video/x-raw, "
            "format = (string) { " GST_RK_ALIST_XMACRO(
                GST_RK_ALIST_CAPS_E,
                GST_RK_ALIST_CAPS_L) " }, " GST_RKMPI_H264ENC_SIZE_CAPS));

static void gst_rkmpi_h264enc_init(GstRKMPIH264Enc *element) {
  GstRKMPIH264Enc *self = GST_RKMPIH264ENC(element);
  self->bitrate_kbps = 4000; // default 4 Mbps
  self->gop_count = 8; // Default GOP
}

static gboolean gst_rkmpi_h264enc_start(GstVideoEncoder *encoder);
static gboolean gst_rkmpi_h264enc_stop(GstVideoEncoder *encoder);
static gboolean gst_rkmpi_h264enc_set_format(GstVideoEncoder *encoder,
                                             GstVideoCodecState *state);
static GstFlowReturn gst_rkmpi_h264enc_finish(GstVideoEncoder *encoder);
static GstFlowReturn gst_rkmpi_h264enc_handle_frame(GstVideoEncoder *self,
                                                    GstVideoCodecFrame *frame);

static gboolean
rkmpi_apply_rc(GstRKMPIH264Enc *self)
{
  VENC_CHN_ATTR_S attr;
  memset(&attr, 0, sizeof(attr));
  if (RK_MPI_VENC_GetChnAttr(chnId, &attr) != RK_SUCCESS)
    return FALSE;

  // Only update fields we support setting at runtime.
  attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
  attr.stRcAttr.stH264Cbr.u32BitRate = self->bitrate_kbps;
  attr.stRcAttr.stH264Cbr.u32Gop     = self->gop_count;
gst_printerrln("hi from rkmpi_apply_rc\n");
  RK_S32 ret = RK_MPI_VENC_SetChnAttr(chnId, &attr);
  return ret == RK_SUCCESS;
}

static void gst_rkmpi_h264enc_set_property(GObject *object,
                                           guint prop_id,
                                           const GValue *value,
                                           GParamSpec *pspec) {
  GstRKMPIH264Enc *self = GST_RKMPIH264ENC(object);

  switch (prop_id) {
  case PROP_BITRATE:
    self->bitrate_kbps = g_value_get_uint(value);
    break;
  case PROP_GOP:
    self->gop_count = g_value_get_uint(value);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }
gst_printerrln("gst_rkmpi_h264enc_set_property: got new prop\n");
  if (self->state && !g_atomic_int_get(&self->stopping)) {
    if (!rkmpi_apply_rc(self)) {
      GST_WARNING_OBJECT(self, "Failed to apply RC change at runtime "
                              "(bitrate=%u kbps, gop=%u)",
                              self->bitrate_kbps, self->gop_count);
    } else {
      GST_INFO_OBJECT(self, "Applied RC change: bitrate=%u kbps, gop=%u",
                      self->bitrate_kbps, self->gop_count);
    }
  }
}

static void gst_rkmpi_h264enc_get_property(GObject *object,
                                           guint prop_id,
                                           GValue *value,
                                           GParamSpec *pspec) {
  GstRKMPIH264Enc *self = GST_RKMPIH264ENC(object);

  switch (prop_id) {
  case PROP_BITRATE:
    g_value_set_uint(value, self->bitrate_kbps);
    break;
  case PROP_GOP:
    g_value_set_uint(value, self->gop_count);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }
}

static gboolean gst_rkmpi_h264enc_flush (GstVideoEncoder *enc) {
   gst_printerrln("flush enter\n");
  GstRKMPIH264Enc *self = GST_RKMPIH264ENC (enc);
  g_atomic_int_set (&self->stopping, 1);

  RK_MPI_VENC_StopRecvFrame (chnId);

  g_async_queue_push (self->gstframe_queue, queued_gst_frame_new (NULL, (uint32_t)-1));
     gst_printerrln("flush exit\n");
  return TRUE;
}

static void gst_rkmpi_h264enc_class_init(GstRKMPIH264EncClass *klass) {
  GstVideoEncoderClass *video_encoder = GST_VIDEO_ENCODER_CLASS(klass);
  video_encoder->start = gst_rkmpi_h264enc_start;
  video_encoder->stop = gst_rkmpi_h264enc_stop;
  video_encoder->finish =
      gst_rkmpi_h264enc_finish; // FIXME: maybe implement flush?
  video_encoder->set_format = gst_rkmpi_h264enc_set_format;
  video_encoder->handle_frame = gst_rkmpi_h264enc_handle_frame;
  video_encoder->flush = gst_rkmpi_h264enc_flush;

  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  gobject_class->set_property = gst_rkmpi_h264enc_set_property;
  gobject_class->get_property = gst_rkmpi_h264enc_get_property;

  obj_properties[PROP_BITRATE] =
      g_param_spec_uint("bitrate",
                        "Bitrate (kbps)",
                        "Target bitrate in kilobits per second for H.264 CBR",
                        1,
                        200000,
                        4000,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING);

  obj_properties[PROP_GOP] =
      g_param_spec_uint("gop",
                        "gop",
                        "Target GOP",
                        1,
                        1024,
                        8,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING);
  g_object_class_install_properties(gobject_class, N_PROPERTIES, obj_properties);

  GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
  gst_element_class_add_pad_template(
      element_class,
      gst_static_pad_template_get(&gst_rkmpi_h264enc_src_template));
  gst_element_class_add_pad_template(
      element_class,
      gst_static_pad_template_get(&gst_rkmpih264enc_sink_template));
  gst_element_class_set_static_metadata(
      element_class, "Rockchip Rockit H264 Encoder", "Codec/Encoder/Video",
      "Encode video streams via Rockchip rockit/RKMPI",
      "Nikita <nikblos@outlook.com>");
}

static void gstvideocodecframe_unref2(void *frame) {
  // We can push null
  if (frame) {
    gst_video_codec_frame_unref((GstVideoCodecFrame *)frame);
  }
}

static gboolean gst_rkmpi_h264enc_start(GstVideoEncoder *encoder) {
  GstRKMPIH264Enc *self = GST_RKMPIH264ENC(encoder);

  if (!gst_rkmpi_init())
    return FALSE;

  gst_video_info_init(&self->info);
  // FIXME: is this type of cast legal?
  self->gstframe_queue = g_async_queue_new_full(gstvideocodecframe_unref2);
  g_atomic_int_set(&self->stopping, 0);
  // FIXME: NULLCHECK queue alloc

  return TRUE;
}

static gboolean gst_gst2rkmpi_format(PIXEL_FORMAT_E *outFormat,
                                     GstVideoFormat inFormat) {
  for (int i = 0; i < COUNTOF(GST_RKMPI_FORMAT_ALIST); i++) {
    if (GST_RKMPI_FORMAT_ALIST[i].gst_format == inFormat) {
      *outFormat = GST_RKMPI_FORMAT_ALIST[i].rkmpi_format;
      return TRUE;
    }
  }
  gst_printerrln("Cannot handle video format '%d' (RKMPI does not support it)",
                 inFormat);
  return FALSE;
}

static gboolean gst_rkmpi_enc_set_src_caps(GstVideoEncoder *encoder,
                                           const char *media_type) {
  GstRKMPIH264Enc *self = GST_RKMPIH264ENC(encoder);
  GstVideoInfo *info = &self->info;
  GstVideoCodecState *output_state;

  GstCaps *caps = gst_caps_new_empty_simple(media_type);
  gst_caps_set_simple(caps, "stream-format", G_TYPE_STRING, "byte-stream",
                      "alignment", G_TYPE_STRING, "au", "width", G_TYPE_INT,
                      GST_VIDEO_INFO_WIDTH(info), "height", G_TYPE_INT,
                      GST_VIDEO_INFO_HEIGHT(info), NULL);

  GST_DEBUG_OBJECT(self, "output caps: %" GST_PTR_FORMAT, caps);

  output_state = gst_video_encoder_set_output_state(encoder, caps, self->state);

  GST_VIDEO_INFO_WIDTH(&output_state->info) = GST_VIDEO_INFO_WIDTH(info);
  GST_VIDEO_INFO_HEIGHT(&output_state->info) = GST_VIDEO_INFO_HEIGHT(info);
  gst_video_codec_state_unref(output_state);

  return gst_video_encoder_negotiate(encoder);
}

static GstPadProbeReturn
sink_event_probe (GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
  GstRKMPIH264Enc *self = GST_RKMPIH264ENC (user_data);

  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_EVENT_UPSTREAM) {
    GstEvent *ev = GST_PAD_PROBE_INFO_EVENT (info);
    if (GST_EVENT_TYPE (ev) == GST_EVENT_FLUSH_START) {
      /* make the worker exit promptly */
      g_atomic_int_set (&self->stopping, 1);
      RK_MPI_VENC_StopRecvFrame (chnId); /* let GetStream time out */
      g_async_queue_push (self->gstframe_queue,
                          queued_gst_frame_new (NULL, (uint32_t)-1));
    }
  }
  return GST_PAD_PROBE_OK;
}

#define GETSTREAM_TIMEOUT_MS 20
#define QUEUE_TIMEOUT_US     (20 * 1000)

static void gst_rkmpi_buffer_loop (gpointer encoder) {
  GstRKMPIH264Enc *self = GST_RKMPIH264ENC (encoder);
  GstVideoEncoder *ven = GST_VIDEO_ENCODER (encoder);
  GstPad *srcpad  = ven->srcpad;
  GstPad *sinkpad = ven->sinkpad;

  for (;;) {
    if (g_atomic_int_get (&self->stopping) ||
        GST_PAD_IS_FLUSHING (srcpad) ||
        GST_PAD_IS_FLUSHING (sinkpad) ||
        gst_pad_get_task_state (srcpad) != GST_TASK_STARTED)
      goto pause_and_return;

    struct QueuedGstFrame *q =
        g_async_queue_timeout_pop (self->gstframe_queue, QUEUE_TIMEOUT_US);
    if (!q) continue;

    GstVideoCodecFrame *gst_frame = q->frame_or_null;
    uint32_t frame_seqno = q->u32SeqNo;
    g_free (q);
    if (!gst_frame)
      goto pause_and_return;

    VENC_PACK_S pack = {0};
    VENC_STREAM_S st = {0};
    st.pstPack = &pack;
    st.u32PackCount = 1;
    st.u32Seq = frame_seqno;

    gboolean got_pack = FALSE;
    while (!g_atomic_int_get (&self->stopping) &&
           !GST_PAD_IS_FLUSHING (srcpad) &&
           !GST_PAD_IS_FLUSHING (sinkpad) &&
           gst_pad_get_task_state (srcpad) == GST_TASK_STARTED) {
      if (RK_MPI_VENC_GetStream (chnId, &st, GETSTREAM_TIMEOUT_MS) == RK_SUCCESS) {
        got_pack = TRUE;
        break;
      }
    }

    if (!got_pack || GST_PAD_IS_FLUSHING (srcpad) ||
        GST_PAD_IS_FLUSHING (sinkpad) ||
        gst_pad_get_task_state (srcpad) != GST_TASK_STARTED) {
      gst_video_encoder_finish_frame (ven, gst_frame);
      continue;
    }

    if (gst_video_encoder_allocate_output_frame (ven, gst_frame,
                                                 st.pstPack->u32Len) != GST_FLOW_OK) {
      RK_MPI_VENC_ReleaseStream (chnId, &st);
      goto pause_and_return;
    }

    GstMapInfo out;
    if (gst_buffer_map (gst_frame->output_buffer, &out, GST_MAP_WRITE)) {
      void *data = RK_MPI_MB_Handle2VirAddr (st.pstPack->pMbBlk);
      memcpy (out.data, data, st.pstPack->u32Len);
      gst_buffer_unmap (gst_frame->output_buffer, &out);
    }
    RK_MPI_VENC_ReleaseStream (chnId, &st);

    if (gst_video_encoder_finish_frame (ven, gst_frame) != GST_FLOW_OK)
      goto pause_and_return;
  }

pause_and_return:
  gst_pad_pause_task (srcpad);
  return;
}

static gboolean gst_rkmpi_h264enc_set_format(GstVideoEncoder *encoder,
                                             GstVideoCodecState *state) {
  GstRKMPIH264Enc *self = GST_RKMPIH264ENC(encoder);

  self->state = gst_video_codec_state_ref(state);
  self->info = state->info;
  self->input_frame_counter = 1;

  const RK_U32 width = GST_VIDEO_INFO_WIDTH(&self->info),
               height = GST_VIDEO_INFO_HEIGHT(&self->info);
  const RK_U32 size = GST_VIDEO_INFO_SIZE(&self->info);

  VENC_CHN_ATTR_S stAttr;
  memset(&stAttr, 0, sizeof(VENC_CHN_ATTR_S));
  stAttr.stVencAttr.enType = RK_VIDEO_ID_AVC;
  if (!gst_gst2rkmpi_format(&stAttr.stVencAttr.enPixelFormat,
                            GST_VIDEO_INFO_FORMAT(&self->info)))
    return FALSE;
  stAttr.stVencAttr.u32Profile = H264E_PROFILE_HIGH;
  stAttr.stVencAttr.u32PicWidth = width;
  stAttr.stVencAttr.u32PicHeight = height;
  stAttr.stVencAttr.u32VirWidth = RK_ALIGN_2(width);
  stAttr.stVencAttr.u32VirHeight = RK_ALIGN_2(height);
  stAttr.stVencAttr.u32StreamBufCnt = 8;
  stAttr.stVencAttr.u32BufSize = width * height * 2;
  stAttr.stRcAttr.stH264Cbr.u32SrcFrameRateNum = 60;
  stAttr.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1;
  stAttr.stRcAttr.stH264Cbr.fr32DstFrameRateNum = 60;
  stAttr.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;
  stAttr.stRcAttr.stH264Cbr.u32StatTime = 1;

  stAttr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
  stAttr.stRcAttr.stH264Cbr.u32BitRate = self->bitrate_kbps;
  stAttr.stRcAttr.stH264Cbr.u32Gop = self->gop_count;
  RK_MPI_VENC_CreateChn(chnId, &stAttr);

  VENC_RC_PARAM_S pstRcParam;
  memset(&pstRcParam, 0, sizeof(VENC_RC_PARAM_S));
  pstRcParam.s32FirstFrameStartQp = 28;
  pstRcParam.stParamH264.u32MinQp   = 8;
  pstRcParam.stParamH264.u32MaxQp   = 51;
  pstRcParam.stParamH264.u32MinIQp  = 8;
  pstRcParam.stParamH264.u32MaxIQp  = 51;

  // try to get stable bitrate???
  pstRcParam.stParamH264.u32FrmMinQp   = 16;
  pstRcParam.stParamH264.u32FrmMinIQp  = 14;
  pstRcParam.stParamH264.u32FrmMaxQp   = 36;
  pstRcParam.stParamH264.u32FrmMaxIQp  = 32;
  RK_MPI_VENC_SetRcParam(chnId, &pstRcParam);

 /* VENC_SUPERFRAME_CFG_S stSuperFrameCfg;
  memset(&stSuperFrameCfg, 0, sizeof(stSuperFrameCfg));
  stSuperFrameCfg.enSuperFrmMode = SUPERFRM_DISCARD;
  stSuperFrameCfg.u32SuperIFrmBitsThr = 440 * 1024 * 8;  // 100KByte
  stSuperFrameCfg.u32SuperPFrmBitsThr = 180 * 1024 * 8;  // 20KByte
  stSuperFrameCfg.enRcPriority = VENC_RC_PRIORITY_BITRATE_FIRST;
  RK_MPI_VENC_SetSuperFrameStrategy(stVencCfg.u32ChnId, &stSuperFrameCfg);
*/
  VENC_RECV_PIC_PARAM_S stRecvParam;
  memset(&stRecvParam, 0, sizeof(VENC_RECV_PIC_PARAM_S));
  stRecvParam.s32RecvPicNum = -1;
  RK_MPI_VENC_StartRecvFrame(chnId, &stRecvParam);

  VENC_INTRA_REFRESH_S stIntraRefresh;
  memset(&stIntraRefresh, 0, sizeof(stIntraRefresh));
  stIntraRefresh.bRefreshEnable = RK_TRUE;
  stIntraRefresh.enIntraRefreshMode = INTRA_REFRESH_ROW;
  stIntraRefresh.u32RefreshNum = 10;
  RK_S32 rkret = RK_MPI_VENC_SetIntraRefresh(chnId, &stIntraRefresh);
gst_printerrln("gst_rkmpi_h264enc_set_format: RK_MPI_VENC_SetIntraRefresh: %d\n", rkret);
    gst_pad_add_probe (GST_VIDEO_ENCODER (encoder)->sinkpad,
                     GST_PAD_PROBE_TYPE_EVENT_UPSTREAM,
                     sink_event_probe, self, NULL);


  gst_pad_start_task(encoder->srcpad, gst_rkmpi_buffer_loop, self, NULL);

  return gst_rkmpi_enc_set_src_caps(encoder, "video/x-h264");
}

static GstFlowReturn gst_rkmpi_h264enc_finish(GstVideoEncoder *encoder) {
gst_printerrln("gst_rkmpi_h264enc_finish: Enter\n");
  GstRKMPIH264Enc *self = GST_RKMPIH264ENC(encoder);

  g_atomic_int_set (&self->stopping, 1);
  RK_S32 rkret;
  rkret = RK_MPI_VENC_StopRecvFrame(chnId);
  // Hey, please generate the EOS bitstream
  RK_MPI_ERROR_CHECK(RK_MPI_VENC_StopRecvFrame);

  if (gst_pad_get_task_state(encoder->srcpad) == GST_TASK_STARTED) {
    g_async_queue_push(self->gstframe_queue, queued_gst_frame_new(NULL, -1));
    // NOTE: Wait what, UNLOCK then LOCK??? This looks horrible, but just seems
    // to be how you do things in Gstreamer
    // https://gitlab.freedesktop.org/gstreamer/gst-plugins-good/-/blob/6525abfc63917e3f92e359f68a02cf65c8fda7d8/sys/v4l2/gstv4l2videoenc.c#L271
    GST_VIDEO_ENCODER_STREAM_UNLOCK(encoder);
    if (!gst_pad_stop_task(encoder->srcpad))
      return GST_FLOW_ERROR;
    GST_VIDEO_ENCODER_STREAM_LOCK(encoder);
  }
gst_printerrln("gst_rkmpi_h264enc_finish: Exit\n");
  return GST_FLOW_OK;
}

static gboolean gst_rkmpi_h264enc_stop(GstVideoEncoder *encoder) {
  gst_printerrln("gst_rkmpi_h264enc_stop: Enter\n");
  GstRKMPIH264Enc *self = GST_RKMPIH264ENC(encoder);

  g_atomic_int_set(&self->stopping, 1);

  RK_MPI_VENC_StopRecvFrame(chnId);
  // wake the queue pop() if it's blocked
  g_async_queue_push(self->gstframe_queue, queued_gst_frame_new (NULL, (uint32_t)-1));

  // stop the pad task
  GST_VIDEO_ENCODER_STREAM_UNLOCK(encoder);
  gst_pad_stop_task(encoder->srcpad);
  GST_VIDEO_ENCODER_STREAM_LOCK(encoder);

  RK_MPI_VENC_DestroyChn(chnId);

  gst_rkmpi_exit();

  if(self->gstframe_queue) {
    g_async_queue_unref(self->gstframe_queue);
    self->gstframe_queue = NULL;
  }
  gst_video_codec_state_unref(self->state);
  GST_DEBUG_OBJECT(self, "stopped");
  gst_printerrln("gst_rkmpi_h264enc_stop: Exit\n");
  return TRUE;
}

static GstFlowReturn gst_rkmpi_h264enc_handle_frame(GstVideoEncoder *encoder,
                                                    GstVideoCodecFrame *frame) {
  GstRKMPIH264Enc *self = GST_RKMPIH264ENC(encoder);
  GstPad *srcpad = encoder->srcpad;
  GstPad *sinkpad = encoder->sinkpad;

  const RK_S32 SEND_TIMEOUT_MS = 20;
  RK_S32 rkret = 0;

  if (g_atomic_int_get (&self->stopping) ||
      gst_pad_get_task_state (srcpad) != GST_TASK_STARTED || GST_PAD_IS_FLUSHING(srcpad)  || GST_PAD_IS_FLUSHING(sinkpad))
    return GST_FLOW_FLUSHING;

  MB_BLK blk = NULL;
  gboolean was_imported = FALSE;
  if (!(blk = gst_rkmpi_buffer_get_mb(frame->input_buffer))) {
    GST_ELEMENT_ERROR(encoder, STREAM, FAILED, ("Expected an MPI buffer. Add rkmpiupload before this element to convert."), (NULL));
    return GST_FLOW_ERROR;
  }

  RK_U32 width = GST_VIDEO_INFO_WIDTH(&self->info),
         height = GST_VIDEO_INFO_HEIGHT(&self->info);
  VIDEO_FRAME_INFO_S h264_frame;
  memset(&h264_frame, 0, sizeof(VIDEO_FRAME_INFO_S));
  h264_frame.stVFrame.u32Width = width;
  h264_frame.stVFrame.u32Height = height;
  h264_frame.stVFrame.u32VirWidth = width;
  h264_frame.stVFrame.u32VirHeight = height;
  if (!gst_gst2rkmpi_format(&h264_frame.stVFrame.enPixelFormat,
                            GST_VIDEO_INFO_FORMAT(&self->info)))
    return FALSE;
  h264_frame.stVFrame.u32FrameFlag = 0;
  h264_frame.stVFrame.pMbBlk = blk;
  h264_frame.stVFrame.u32TimeRef =
      self->input_frame_counter; // FIXME: gstreamer
  h264_frame.stVFrame.u64PTS = frame->pts;
  h264_frame.stVFrame.enVideoFormat = VIDEO_FORMAT_TILE_16x8;

  for (;;) {
    if (g_atomic_int_get (&self->stopping) ||
        gst_pad_get_task_state (srcpad) != GST_TASK_STARTED || GST_PAD_IS_FLUSHING(srcpad)  || GST_PAD_IS_FLUSHING(sinkpad))
      return GST_FLOW_FLUSHING;

    rkret = RK_MPI_VENC_SendFrame (chnId, &h264_frame, SEND_TIMEOUT_MS);
    if (rkret == RK_SUCCESS) break;
    /* else timeout/EAGAIN → retry while active */
  }

  g_async_queue_push(self->gstframe_queue,
                    queued_gst_frame_new(frame, self->input_frame_counter++));

  return GST_FLOW_OK;
}

gboolean gst_rkmpih264_plugin_register(GstPlugin *plugin) {
  GST_DEBUG_CATEGORY_INIT(gstrkpmpih264, "rkmpih264", 0, "RKMPI H264 Encoder");
  return gst_element_register(plugin, "rkmpih264enc", GST_RANK_PRIMARY,
                              GST_TYPE_RKMPIH264ENC);
}
