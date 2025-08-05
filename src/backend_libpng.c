#include "backend.h"
#include "bitmap.h"
#include "image.h"
#include "log.h"
#include "source.h"
#include "source_private.h"

#include <stdlib.h>
#include <string.h>

#include <png.h>

struct private {
  FILE *file;
  char *data;
  size_t len, pos;

  png_structp png;
  png_infop info;
};

static void read_end(struct private *private)
{
  if (private->png) // The lifetime of private->info matches private->png
    png_destroy_read_struct(&private->png, &private->info, NULL);
  if (private->file) {
    fclose(private->file);
    private->file = NULL;
  }
}

static void free_private(void *raw_private)
{
  if (!raw_private)
    return;

  struct private *private = raw_private;
  read_end(private);
  free(private);
}

static void load_image(void *raw_private, struct imv_image **image, int *frametime)
{
  *image = NULL;
  *frametime = 0;

  struct private *private = raw_private;
  if (setjmp(png_jmpbuf(private->png)))
    return;

  int width = png_get_image_width(private->png, private->info);
  int height = png_get_image_height(private->png, private->info);
  size_t stride = png_get_rowbytes(private->png, private->info);

  png_bytep raw = malloc(stride * height);

  png_bytepp row_pointers = malloc(sizeof(png_bytep) * height);
  for (int i = 0; i < height; i++)
    row_pointers[i] = raw + i*stride;
  png_read_image(private->png, row_pointers);
  free(row_pointers);

  read_end(private);

  struct imv_bitmap *bmp = malloc(sizeof(struct imv_bitmap));
  bmp->width = width;
  bmp->height = height;
  bmp->format = IMV_ABGR;
  bmp->data = raw;
  *image = imv_image_create_from_bitmap(bmp);
}

static const struct imv_source_vtable vtable = {
  .load_first_frame = load_image,
  .free = free_private
};

#define SIG_SIZE 8

static struct private *init_private()
{
  struct private *private = calloc(1, sizeof(struct private));

  private->png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (!private->png) {
    free(private);
    return NULL;
  }

  /* set max PNG chunk size to 50MB, instead of 8MB default */
  png_set_chunk_malloc_max(private->png, 1024 * 1024 * 50);
  png_set_sig_bytes(private->png, SIG_SIZE);

  private->info = png_create_info_struct(private->png);
  if (!private->info) {
    png_destroy_read_struct(&private->png, NULL, NULL);
    free(private);
    return NULL;
  }

  return private;
}

static int setup_png(png_structp png, png_infop info)
{

  if (setjmp(png_jmpbuf(png)))
    return 0;

  png_read_info(png, info);

  /* Tell libpng to give us a consistent output format */
  png_set_gray_to_rgb(png);
  png_set_filler(png, 0xff, PNG_FILLER_AFTER);
  png_set_strip_16(png);
  png_set_expand(png);
  png_set_packing(png);
  png_read_update_info(png, info);

  imv_log(IMV_DEBUG, "libpng: info width=%d height=%d bit_depth=%d color_type=%d\n",
      png_get_image_width(png, info),
      png_get_image_height(png, info),
      png_get_bit_depth(png, info),
      png_get_color_type(png, info));

  return 1;
}

static enum backend_result open_path(const char *path, struct imv_source **src)
{
  FILE *f = fopen(path, "rb");
  if (!f) {
    return BACKEND_BAD_PATH;
  }

  {
    unsigned char sig[SIG_SIZE];
    if (fread(sig, 1, SIG_SIZE, f) != SIG_SIZE) {
      if (feof(f)) {
        return BACKEND_UNSUPPORTED;
      } else {
        imv_log(IMV_ERROR, "Error reading %s\n", path);
        return BACKEND_BAD_PATH;
      }
    }
    if (png_sig_cmp(sig, 0, SIG_SIZE)) {
      fclose(f);
      return BACKEND_UNSUPPORTED;
    }
  }

  struct private *private = init_private();
  if (!private)
    return BACKEND_UNSUPPORTED;
  private->file = f;

  png_init_io(private->png, private->file);

  if (!setup_png(private->png, private->info)) {
    free_private(private);
    return BACKEND_UNSUPPORTED;
  }

  *src = imv_source_create(&vtable, private);
  return BACKEND_SUCCESS;
}

static void read_memory(png_structp png, png_bytep out, png_size_t size)
{
  struct private *private = png_get_io_ptr(png);
  if (private->len - private->pos < size) {
    imv_log(IMV_ERROR, "incomplete png data\n");
    return;
  }

  memcpy(out, private->data + private->pos, size);
  private->pos += size;
}

static enum backend_result open_memory(void *data, size_t len, struct imv_source **src)
{
  if (png_sig_cmp(data, 0, SIG_SIZE))
    return BACKEND_UNSUPPORTED;

  struct private *private = init_private();
  if (!private)
    return BACKEND_UNSUPPORTED;

  private->data = data + SIG_SIZE;
  private->len = len - SIG_SIZE;
  png_set_read_fn(private->png, private, read_memory);

  if (!setup_png(private->png, private->info)) {
    free_private(private);
    return BACKEND_UNSUPPORTED;
  }

  *src = imv_source_create(&vtable, private);
  return BACKEND_SUCCESS;
}

const struct imv_backend imv_backend_libpng = {
  .name = "libpng",
  .description = "The official PNG reference implementation",
  .website = "http://www.libpng.org/pub/png/libpng.html",
  .license = "The libpng license",
  .open_path = open_path,
  .open_memory = open_memory,
};
