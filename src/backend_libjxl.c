#include "backend.h"
#include "bitmap.h"
#include "image.h"
#include "log.h"
#include "source.h"
#include "source_private.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <limits.h>

#include <jxl/decode.h>

#define BACKEND_NB_CHANNELS       4
#define BACKEND_DEFAULT_FRAMETIME 100
#define BACKEND_NB_ALLOC_FRAMES   64

struct jxl_frame {
  void *data;
  int frametime;
};

struct private {
  void *data;
  off_t data_len;
  int owns_data;

  int width;
  int height;
  int is_animation;

  struct jxl_frame *frames;
  int nb_allocd_frames;
  int cur_frame;
  int nb_frames;
};

static void free_private(void *raw_pvt)
{
  if (!raw_pvt)
    return;

  struct private *pvt = raw_pvt;

  if (pvt->owns_data) {
    if (pvt->data)
      munmap(pvt->data, pvt->data_len);
  }

  if (pvt->frames) {
    for (int i = 0; i != pvt->nb_allocd_frames; ++i)
      if (pvt->frames[i].data)
        free(pvt->frames[i].data);

    free(pvt->frames);
  }

  free(pvt);
}

static void push_frame(struct private *pvt, struct imv_image **img, int *frametime)
{
  struct imv_bitmap *bmp = malloc(sizeof *bmp);
  size_t sz = pvt->width * pvt->height * BACKEND_NB_CHANNELS;

  bmp->width = pvt->width;
  bmp->height = pvt->height;
  bmp->format = IMV_ABGR;
  bmp->data = malloc(sz);
  memcpy(bmp->data, pvt->frames[pvt->cur_frame].data, sz);

  *img = imv_image_create_from_bitmap(bmp);
  *frametime = pvt->frames[pvt->cur_frame].frametime;
}

static void first_frame(void *raw_pvt, struct imv_image **img, int *frametime)
{
  *img = NULL;
  *frametime = 0;

  imv_log(IMV_DEBUG, "libjxl: first_frame called\n");

  struct private *pvt = raw_pvt;
  JxlDecoder *jxld = JxlDecoderCreate(NULL);
  if (JxlDecoderSubscribeEvents(jxld, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) {
    imv_log(IMV_ERROR, "libjxl: decoder failed to subscribe to events\n");
    goto end;
  }

  JxlBasicInfo info;
  JxlDecoderStatus sts = JxlDecoderSetInput(jxld, pvt->data, pvt->data_len);
  if (sts != JXL_DEC_SUCCESS) {
    imv_log(IMV_ERROR, "libjxl: decoder failed to set input\n");
    goto end;
  }
  JxlDecoderCloseInput(jxld);

  pvt->nb_frames = 0;
  pvt->cur_frame = 0;

  JxlPixelFormat fmt = { BACKEND_NB_CHANNELS, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };
  pvt->nb_allocd_frames = BACKEND_NB_ALLOC_FRAMES;
  pvt->frames = calloc(pvt->nb_allocd_frames, sizeof *pvt->frames);
  do {
    sts = JxlDecoderProcessInput(jxld);

    switch (sts) {
      case JXL_DEC_SUCCESS:
        break;
      case JXL_DEC_ERROR:
        imv_log(IMV_ERROR, "libjxl: decoder error\n");
        goto end;
      case JXL_DEC_NEED_MORE_INPUT:
        imv_log(IMV_ERROR, "libjxl: decoder needs more input\n");
        goto end;
      case JXL_DEC_BASIC_INFO:
        if (JxlDecoderGetBasicInfo(jxld, &info) != JXL_DEC_SUCCESS) {
          imv_log(IMV_ERROR, "libjxl: decoder failed to get basic info\n");
          goto end;
        }
        pvt->width = info.xsize;
        pvt->height = info.ysize;
        pvt->is_animation = info.have_animation == JXL_TRUE ? 1 : 0;
        break;
      case JXL_DEC_NEED_IMAGE_OUT_BUFFER:
        {
          size_t buf_sz;
          if (JxlDecoderImageOutBufferSize(jxld, &fmt, &buf_sz) != JXL_DEC_SUCCESS) {
            imv_log(IMV_ERROR, "libjxl: decoder failed to get output buffer size\n");
            goto end;
          }

          if (pvt->nb_frames == pvt->nb_allocd_frames) {
            pvt->frames = realloc(pvt->frames,
                sizeof(struct jxl_frame) * (pvt->nb_allocd_frames + BACKEND_NB_ALLOC_FRAMES));
            pvt->nb_allocd_frames += BACKEND_NB_ALLOC_FRAMES;
            memset(&pvt->frames[pvt->nb_frames], 0,
                (pvt->nb_allocd_frames * sizeof(struct jxl_frame)) - (pvt->nb_frames * sizeof(struct jxl_frame)));
          }

          struct jxl_frame *cur_frame = &pvt->frames[pvt->nb_frames];
          cur_frame->data = malloc(buf_sz);

          if (pvt->is_animation) {
            if (info.animation.tps_numerator && info.animation.tps_denominator)
              cur_frame->frametime = info.animation.tps_numerator / info.animation.tps_denominator;
            else {
                imv_log(IMV_DEBUG, "libjxl: no frametime info for animation, using default\n");
                cur_frame->frametime = BACKEND_DEFAULT_FRAMETIME;
            }
          } else
            cur_frame->frametime = 0;

          if (JxlDecoderSetImageOutBuffer(jxld, &fmt, cur_frame->data, buf_sz) != JXL_DEC_SUCCESS) {
            imv_log(IMV_ERROR, "libjxl: JxlDecoderSetImageOutBuffer failed\n");
            goto end;
          }
          break;
        }
      case JXL_DEC_FULL_IMAGE:
        ++pvt->nb_frames;
        continue;
      default:
        imv_log(IMV_ERROR, "libjxl: unknown decoder status\n");
        goto end;
    }
  } while (sts != JXL_DEC_SUCCESS);

  push_frame(pvt, img, frametime);

end:
  if (jxld)
    JxlDecoderDestroy(jxld);
}

static void next_frame(void *raw_pvt, struct imv_image **img, int *frametime)
{
  *img = NULL;
  *frametime = 0;

  imv_log(IMV_DEBUG, "libjxl: next_frame called\n");

  struct private *pvt = raw_pvt;

  if (pvt->cur_frame == pvt->nb_frames - 1)
    pvt->cur_frame = 0;
  else
    ++pvt->cur_frame;

  push_frame(pvt, img, frametime);
}

static const struct imv_source_vtable vtable = {
  .load_first_frame = first_frame,
  .load_next_frame = next_frame,
  .free = free_private,
};

static enum backend_result open_memory(void *data, size_t sz, struct imv_source **src)
{
  imv_log(IMV_DEBUG, "libjxl: open_memory called\n");

  struct private *pvt = calloc(1, sizeof *pvt);

  switch (JxlSignatureCheck(data, sz)) {
    case JXL_SIG_NOT_ENOUGH_BYTES:
      imv_log(IMV_DEBUG, "libjxl: not enough bytes to read\n");
    case JXL_SIG_INVALID:
      imv_log(IMV_DEBUG, "libjxl: valid jxl signature not found\n");
      return BACKEND_UNSUPPORTED;
    default:
      pvt->owns_data = 0;
      pvt->data = data;
      pvt->data_len = sz;
      break;
    }

  *src = imv_source_create(&vtable, pvt);

  return BACKEND_SUCCESS;
}

static enum backend_result open_path(const char *path, struct imv_source **src)
{
  imv_log(IMV_DEBUG, "libjxl: open_path(%s)\n", path);

  enum backend_result ret = BACKEND_SUCCESS;
  struct private *pvt = calloc(1, sizeof *pvt);

  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    ret = BACKEND_BAD_PATH;
    goto end;
  }

  pvt->data_len = lseek(fd, 0, SEEK_END);
  if (pvt->data_len < 0) {
    ret = BACKEND_BAD_PATH;
    goto end;
  }
  pvt->data = mmap(NULL, pvt->data_len, PROT_READ, MAP_PRIVATE, fd, 0);
  if (!pvt->data || pvt->data == MAP_FAILED) {
    imv_log(IMV_ERROR, "libjxl: failed to map file into memory\n");
    ret = BACKEND_BAD_PATH;
    goto end;
  }

  switch (JxlSignatureCheck(pvt->data, pvt->data_len)) {
    case JXL_SIG_NOT_ENOUGH_BYTES:
      imv_log(IMV_DEBUG, "libjxl: not enough bytes to read\n");
    case JXL_SIG_INVALID:
      imv_log(IMV_DEBUG, "libjxl: valid jxl signature not found\n");
      munmap(pvt->data, pvt->data_len);
      ret = BACKEND_UNSUPPORTED;
      goto end;
    default:
      pvt->owns_data = 1;
      break;
    }

  *src = imv_source_create(&vtable, pvt);

end:
  if (fd >= 0)
    close(fd);

  return ret;
}

const struct imv_backend imv_backend_libjxl = {
  .name = "libjxl",
  .description = "The official JPEGXL reference implementation",
  .website = "https://jpeg.org/jpegxl/",
  .license = "The Modified BSD License",
  .open_path = &open_path,
  .open_memory = &open_memory,
};

/* vim:set ts=2 sts=2 sw=2 et: */
