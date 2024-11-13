#include "backend.h"
#include "bitmap.h"
#include "image.h"
#include "log.h"
#include "source.h"
#include "source_private.h"

#include <fcntl.h>
#include <nsgif.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct private {
  int current_frame;
  nsgif_t *gif;
  void *data;
  size_t len;
};

static nsgif_bitmap_t* bitmap_create(int width, int height)
{
  const size_t bytes_per_pixel = 4;
  return calloc(width * height, bytes_per_pixel);
}

static void bitmap_destroy(void *bitmap)
{
  free(bitmap);
}

static unsigned char* bitmap_get_buffer(void *bitmap)
{
  return bitmap;
}

static void bitmap_set_opaque(void *bitmap, bool opaque)
{
  (void)bitmap;
  (void)opaque;
}

static bool bitmap_test_opaque(void *bitmap)
{
  (void)bitmap;
  return false;
}

static void bitmap_mark_modified(void *bitmap)
{
  (void)bitmap;
}

static nsgif_bitmap_cb_vt bitmap_callbacks = {
  bitmap_create,
  bitmap_destroy,
  bitmap_get_buffer,
  bitmap_set_opaque,
  bitmap_test_opaque,
  bitmap_mark_modified
};


static void free_private(void *raw_private)
{
  if (!raw_private) {
    return;
  }

  struct private *private = raw_private;
  nsgif_destroy(private->gif);
  munmap(private->data, private->len);
  free(private);
}

static void push_current_image(struct private *private,
    struct imv_image **image, int *frametime, void *gif_frame_data)
{
  const nsgif_info_t *gif_info = nsgif_get_info(private->gif);
  const nsgif_frame_info_t *frame_info = nsgif_get_frame_info(private->gif, private->current_frame);

  struct imv_bitmap *bmp = malloc(sizeof *bmp);
  bmp->width = gif_info->width;
  bmp->height = gif_info->height;
  bmp->format = IMV_ABGR;
  size_t len = 4 * bmp->width * bmp->height;
  bmp->data = malloc(len);
  memcpy(bmp->data, gif_frame_data, len);

  *image = imv_image_create_from_bitmap(bmp);
  *frametime = frame_info->delay * 10.0;
}

static void first_frame(void *raw_private, struct imv_image **image, int *frametime)
{
  *image = NULL;
  *frametime = 0;

  struct private *private = raw_private;
  private->current_frame = 0;

  void *gif_frame_data;
  nsgif_error code = nsgif_frame_decode(private->gif, private->current_frame, &gif_frame_data);
  if (code != NSGIF_OK) {
    imv_log(IMV_DEBUG, "libnsgif: failed to decode first frame\n");
    return;
  }

  push_current_image(private, image, frametime, gif_frame_data);
}

static void next_frame(void *raw_private, struct imv_image **image, int *frametime)
{
  *image = NULL;
  *frametime = 0;

  struct private *private = raw_private;

  private->current_frame++;

  const nsgif_info_t *gif_info = nsgif_get_info(private->gif);
  private->current_frame %= gif_info->frame_count;

  void *gif_frame_data;
  nsgif_error code = nsgif_frame_decode(private->gif, private->current_frame, &gif_frame_data);
  if (code != NSGIF_OK) {
    imv_log(IMV_DEBUG, "libnsgif: failed to decode a frame\n");
    return;
  }

  push_current_image(private, image, frametime, gif_frame_data);
}

static const struct imv_source_vtable vtable = {
  .load_first_frame = first_frame,
  .load_next_frame = next_frame,
  .free = free_private
};

static enum backend_result open_memory(void *data, size_t len, struct imv_source **src)
{
  struct private *private = calloc(1, sizeof *private);

  nsgif_error code;

  code = nsgif_create(&bitmap_callbacks, NSGIF_BITMAP_FMT_R8G8B8A8, &private->gif);
  if (code != NSGIF_OK) {
    nsgif_destroy(private->gif);
    free(private);
    imv_log(IMV_DEBUG, "libnsgif: unsupported file\n");
    return BACKEND_UNSUPPORTED;
  }

  code = nsgif_data_scan(private->gif, len, data);
  if (code != NSGIF_OK) {
    nsgif_destroy(private->gif);
    free(private);
    imv_log(IMV_DEBUG, "libsngif: unsupported file\n");
    return BACKEND_UNSUPPORTED;
  }

  nsgif_data_complete(private->gif);

  *src = imv_source_create(&vtable, private);
  return BACKEND_SUCCESS;
}

static enum backend_result open_path(const char *path, struct imv_source **src)
{
  imv_log(IMV_DEBUG, "libnsgif: open_path(%s)\n", path);

  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    return BACKEND_BAD_PATH;
  }

  off_t len = lseek(fd, 0, SEEK_END);
  if (len < 0) {
    close(fd);
    return BACKEND_BAD_PATH;
  }

  void *data = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (data == MAP_FAILED || !data) {
    return BACKEND_BAD_PATH;
  }

  struct private *private = calloc(1, sizeof *private);
  private->data = data;
  private->len = len;
  nsgif_error code;

  code = nsgif_create(&bitmap_callbacks, NSGIF_BITMAP_FMT_R8G8B8A8, &private->gif);
  if (code != NSGIF_OK) {
    nsgif_destroy(private->gif);
    munmap(private->data, private->len);
    free(private);
    imv_log(IMV_DEBUG, "libnsgif: unsupported file\n");
    return BACKEND_UNSUPPORTED;
  }

  code = nsgif_data_scan(private->gif, len, data);
  if (code != NSGIF_OK) {
    nsgif_destroy(private->gif);
    munmap(private->data, private->len);
    free(private);
    imv_log(IMV_DEBUG, "libsngif: unsupported file\n");
    return BACKEND_UNSUPPORTED;
  }

  nsgif_data_complete(private->gif);

  const nsgif_info_t *gif_info = nsgif_get_info(private->gif);

  imv_log(IMV_DEBUG, "libnsgif: num_frames=%d\n", gif_info->frame_count);
  imv_log(IMV_DEBUG, "libnsgif: width=%d\n", gif_info->width);
  imv_log(IMV_DEBUG, "libnsgif: height=%d\n", gif_info->height);

  *src = imv_source_create(&vtable, private);
  return BACKEND_SUCCESS;
}

const struct imv_backend imv_backend_libnsgif = {
  .name = "libnsgif",
  .description = "Tiny GIF decoding library from the NetSurf project",
  .website = "https://www.netsurf-browser.org/projects/libnsgif/",
  .license = "MIT",
  .open_path = &open_path,
  .open_memory = &open_memory,
};
