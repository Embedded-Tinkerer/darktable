/*
    This file is part of PhotographyLAB, a darktable-based photo editor.
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

// the one translation unit of PhotographyLAB that calls into libdarktable.
// the history and headless-render handling follows src/mcp/dt_bridge.c

#include "plab/plab_engine.h"

#include "common/colorlabels.h"
#include "common/darktable.h"
#include "common/database.h"
#include "common/mipmap_cache.h"
#include "common/ratings.h"
#include "views/view.h"
#include "common/film.h"
#include "common/history.h"
#include "common/illuminants.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/introspection.h"
#include "common/iop_order.h"
#include "common/styles.h"
#include "control/conf.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_module.h"

#ifdef HAVE_AI
#include "common/ai/restore.h"
#include "common/ai/restore_raw_bayer.h"
#include "common/ai/restore_raw_linear.h"
#include "common/ai_models.h"
#include "common/exif.h"
#include "common/grouping.h"
#include "imageio/imageio_dng.h"
#endif

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <stdarg.h>
#include <string.h>

static void _seterr(char **err, const char *fmt, ...)
{
  if(!err) return;
  va_list ap;
  va_start(ap, fmt);
  g_free(*err);
  *err = g_strdup_vprintf(fmt, ap);
  va_end(ap);
}

int plab_init(int argc, char **argv)
{
  // dt_init() keeps pointers into argv for the whole session
  char **args = g_new0(char *, argc + 2);
  args[0] = g_strdup("photographylab");
  for(int i = 0; i < argc; i++) args[i + 1] = g_strdup(argv[i]);
  const int rc = dt_init(argc + 1, args, FALSE, TRUE, NULL);
  // new photos come in unrated, as in Lightroom; darktable gives them a star
  if(rc == 0) dt_conf_set_int("ui_last/import_initial_rating", 0);
  return rc;
}

void plab_shutdown(void)
{
  dt_cleanup();
}

void plab_free(void *p)
{
  g_free(p);
}

int plab_import(const char *path, char **err)
{
  // the library keys images by absolute path
  GFile *gf = g_file_new_for_path(path);
  gchar *canon = g_file_get_path(gf);
  g_object_unref(gf);
  if(!g_file_test(canon, G_FILE_TEST_IS_REGULAR))
  {
    _seterr(err, "'%s' is not a readable file", path);
    g_free(canon);
    return -1;
  }
  dt_imgid_t id = dt_image_get_id_full_path(canon);
  if(!dt_is_valid_imgid(id))
  {
    gchar *dir = g_path_get_dirname(canon);
    dt_film_t film;
    dt_film_init(&film); // holds a mutex that dt_film_new() does not set up
    const dt_filmid_t filmid = dt_film_new(&film, dir);
    dt_film_cleanup(&film);
    g_free(dir);
    if(dt_is_valid_filmid(filmid)) id = dt_image_import(filmid, canon, TRUE, FALSE);
  }
  if(!dt_is_valid_imgid(id)) _seterr(err, "could not import '%s'", path);
  g_free(canon);
  return dt_is_valid_imgid(id) ? id : -1;
}

// ---------------------------------------------------------------------------
// rendering into memory, through an export "format" that keeps the pixels
// (dt_imageio_preview() needs the GUI, see src/mcp/dt_bridge.c)
// ---------------------------------------------------------------------------

typedef struct
{
  dt_imageio_module_data_t head;
  uint8_t *buf;
  uint32_t width, height;
} _mem_fmt_t;

static int _mem_write(dt_imageio_module_data_t *data, const char *filename,
                      const void *in,
                      const dt_colorspaces_color_profile_type_t over_type,
                      const char *over_filename, void *exif, const int exif_len,
                      const dt_imgid_t imgid, const int num, const int total,
                      dt_dev_pixelpipe_t *pipe, const gboolean export_masks)
{
  _mem_fmt_t *d = (_mem_fmt_t *)data;
  if(!in) return 1;
  const size_t bytes = sizeof(uint32_t) * (size_t)data->width * data->height;
  g_free(d->buf);
  d->buf = g_try_malloc(bytes);
  if(!d->buf) return 1;
  memcpy(d->buf, in, bytes);
  d->width = data->width;
  d->height = data->height;
  return 0;
}

static int _mem_bpp(dt_imageio_module_data_t *d) { return 8; }
static int _mem_levels(dt_imageio_module_data_t *d) { return IMAGEIO_RGB | IMAGEIO_INT8; }
static const char *_mem_mime(dt_imageio_module_data_t *d) { return "memory"; }

uint8_t *plab_render(int imgid, int max_w, int max_h, int history_end,
                     int *out_w, int *out_h, char **err)
{
  dt_imageio_module_format_t fmt;
  memset(&fmt, 0, sizeof(fmt));
  fmt.mime = _mem_mime;
  fmt.levels = _mem_levels;
  fmt.bpp = _mem_bpp;
  fmt.write_image = _mem_write;

  _mem_fmt_t dat;
  memset(&dat, 0, sizeof(dat));
  dat.head.max_width = MAX(max_w, 0);
  dat.head.max_height = MAX(max_h, 0);
  dat.head.width = dat.head.max_width;
  dat.head.height = dat.head.max_height;
  dat.head.style_append = TRUE;

  // returns FALSE on success
  const gboolean failed = dt_imageio_export_with_flags(
      imgid, "memory", &fmt, (dt_imageio_module_data_t *)&dat,
      TRUE, TRUE, FALSE /*hq*/, FALSE /*upscale*/, FALSE, 1.0,
      FALSE, NULL, FALSE, FALSE,
      DT_COLORSPACE_SRGB, NULL, DT_INTENT_PERCEPTUAL, NULL, NULL,
      1, 1, NULL, history_end);

  if(failed || !dat.buf)
  {
    g_free(dat.buf);
    _seterr(err, "render failed");
    return NULL;
  }
  *out_w = dat.width;
  *out_h = dat.height;
  return dat.buf;
}

int plab_base_history(int imgid)
{
  const dt_image_t *img = dt_image_cache_get(imgid, 'r');
  const gboolean developed = img && (img->flags & DT_IMAGE_AUTO_PRESETS_APPLIED);
  dt_image_cache_read_release(img);

  // the first pipeline run writes out the modules darktable auto-applies
  if(!developed)
  {
    int w = 0, h = 0;
    g_free(plab_render(imgid, 64, 64, -1, &w, &h, NULL));
  }
  int end = 0;
  dt_image_get_history_end(imgid, &end);
  return end;
}

// ---------------------------------------------------------------------------
// white balance
// ---------------------------------------------------------------------------

static gboolean _as_shot_xy(int imgid, float *x, float *y)
{
  dt_develop_t dev;
  dt_dev_init(&dev, FALSE);
  dt_dev_load_image(&dev, imgid);
  gboolean ok = FALSE;
  // color calibration's defaults hold the as-shot illuminant
  // (channelmixerrgb.c reload_defaults)
  dt_iop_module_t *m = dt_iop_get_module_by_op_priority(dev.iop, "channelmixerrgb", 0);
  if(m && m->so->get_p)
  {
    const float *px = m->so->get_p(m->default_params, "x");
    const float *py = m->so->get_p(m->default_params, "y");
    if(px && py && *px > 0.f && *py > 0.f)
    {
      *x = *px;
      *y = *py;
      ok = TRUE;
    }
  }
  dt_dev_cleanup(&dev);
  return ok;
}

static void _locus(const float t, float *x, float *y)
{
  CCT_to_xy_blackbody(CLAMP(t, 1667.f, 25000.f), x, y);
}

double plab_as_shot_temperature(int imgid)
{
  float x = 0.f, y = 0.f;
  if(!_as_shot_xy(imgid, &x, &y)) return 0.0;
  return xy_to_CCT(x, y);
}

void plab_white_balance_xy(int imgid, double kelvin, double tint,
                           double *x, double *y)
{
  float x0 = 0.3457f, y0 = 0.3585f; // D50 when the image gives nothing
  _as_shot_xy(imgid, &x0, &y0);
  float lx0, ly0, lx, ly;
  _locus(xy_to_CCT(x0, y0), &lx0, &ly0);
  _locus(kelvin, &lx, &ly);
  // a greener illuminant is compensated toward magenta, hence +y for +tint
  *x = lx + (x0 - lx0);
  *y = ly + (y0 - ly0) + 0.0002 * tint;
}

// ---------------------------------------------------------------------------
// edits
// ---------------------------------------------------------------------------

static gboolean _module_in_history(dt_develop_t *dev, dt_iop_module_t *m)
{
  int i = 0;
  for(GList *h = dev->history; h && i < dev->history_end; h = g_list_next(h), i++)
    if(((dt_dev_history_item_t *)h->data)->module == m) return TRUE;
  return FALSE;
}

// the n-th (from 1) instance of op that the base history does not use,
// created when there are fewer
static dt_iop_module_t *_fresh_instance(dt_develop_t *dev, const char *op, int n)
{
  dt_iop_module_t *base = NULL;
  int found = 0;
  for(GList *l = dev->iop; l; l = g_list_next(l))
  {
    dt_iop_module_t *m = l->data;
    if(strcmp(m->op, op)) continue;
    if(!base || m->multi_priority < base->multi_priority) base = m;
  }
  if(!base) return NULL;

  // in multi_priority order, so a layer keeps its instance across renders
  for(int prio = 0; prio < 1000; prio++)
  {
    dt_iop_module_t *m = dt_iop_get_module_by_op_priority(dev->iop, op, prio);
    if(!m || _module_in_history(dev, m)) continue;
    if(++found == n) return m;
  }
  dt_iop_module_t *m = NULL;
  while(found < n)
  {
    m = dt_dev_module_duplicate_ext(dev, base, TRUE);
    if(!m) return NULL;
    found++;
  }
  return m;
}

static gboolean _place_after(dt_develop_t *dev, dt_iop_module_t *mod, const char *ops)
{
  gchar **names = g_strsplit(ops, ",", -1);
  dt_iop_module_t *target = NULL;
  for(int i = 0; names[i] && !target; i++)
  {
    dt_iop_module_t *t = dt_iop_get_module_by_op_priority(dev->iop, names[i], -1);
    if(t && t->enabled) target = t;
  }
  g_strfreev(names);
  if(!target || target == mod) return TRUE;
  if(mod->iop_order > target->iop_order) return TRUE;
  if(!dt_ioppr_check_can_move_after_iop(dev->iop, mod, target)) return FALSE;
  return dt_ioppr_move_iop_after(dev, mod, target);
}

static gboolean _set_field(dt_iop_module_t *mod, const plab_field_t *f, char **err)
{
  dt_introspection_field_t *fi = mod->so->get_f ? mod->so->get_f(f->name) : NULL;
  void *p = fi ? mod->so->get_p(mod->params, f->name) : NULL;
  if(!p)
  {
    _seterr(err, "module '%s' has no field '%s'", mod->op, f->name);
    return FALSE;
  }
  switch(fi->header.type)
  {
    case DT_INTROSPECTION_TYPE_FLOAT:
    {
      float *v = p;
      const float val = f->relative ? *v + f->value : f->value;
      *v = CLAMP(val, fi->Float.Min, fi->Float.Max);
      return TRUE;
    }
    case DT_INTROSPECTION_TYPE_INT:
      *(int *)p = f->relative ? *(int *)p + (int)f->value : (int)f->value;
      return TRUE;
    case DT_INTROSPECTION_TYPE_BOOL:
      *(gboolean *)p = f->value != 0.0;
      return TRUE;
    case DT_INTROSPECTION_TYPE_ENUM:
      if(f->str)
      {
        for(dt_introspection_type_enum_tuple_t *e = fi->Enum.values; e && e->name; e++)
          if(!strcmp(e->name, f->str))
          {
            *(int *)p = e->value;
            return TRUE;
          }
        _seterr(err, "'%s' is not a value of %s.%s", f->str, mod->op, f->name);
        return FALSE;
      }
      *(int *)p = (int)f->value;
      return TRUE;
    case DT_INTROSPECTION_TYPE_ARRAY:
      if(f->str && fi->Array.type == DT_INTROSPECTION_TYPE_CHAR)
      {
        g_strlcpy(p, f->str, fi->header.size);
        return TRUE;
      }
      // fall through
    default:
      _seterr(err, "field '%s' of '%s' cannot be set", f->name, mod->op);
      return FALSE;
  }
}

static gboolean _apply_layer(dt_develop_t *dev, const plab_layer_t *l, char **err)
{
  dt_iop_module_t *mod = l->fresh > 0
    ? _fresh_instance(dev, l->op, l->fresh)
    : dt_iop_get_module_by_op_priority(dev->iop, l->op, 0);
  if(!mod)
  {
    _seterr(err, "module '%s' is not available", l->op);
    return FALSE;
  }

  // a layer nobody switched on, on a module the image does not use, is no edit
  if(!l->enabled && !mod->enabled) return TRUE;

  if(l->after && !_place_after(dev, mod, l->after))
  {
    _seterr(err, "cannot place '%s' after '%s'", l->op, l->after);
    return FALSE;
  }

  for(int i = 0; i < l->n_fields; i++)
    if(!_set_field(mod, &l->fields[i], err)) return FALSE;

  if(l->opacity >= 0.f && mod->blend_params)
  {
    mod->blend_params->mask_mode = l->opacity < 100.f ? DEVELOP_MASK_ENABLED
                                                      : DEVELOP_MASK_DISABLED;
    mod->blend_params->opacity = CLAMP(l->opacity, 0.f, 100.f);
  }
  if(l->label && l->label[0])
  {
    g_strlcpy(mod->multi_name, l->label, sizeof(mod->multi_name));
    mod->multi_name_hand_edited = TRUE;
  }
  mod->enabled = l->enabled;
  dt_dev_add_history_item_ext(dev, mod, l->enabled, TRUE);
  return TRUE;
}

static void _load_truncated(dt_develop_t *dev, int imgid, int base_end)
{
  dt_dev_init(dev, FALSE);
  dt_dev_load_image(dev, imgid);
  // load reads the history but does not replay it into the module params
  const int n = (int)g_list_length(dev->history);
  dt_dev_pop_history_items_ext(dev, CLAMP(base_end, 0, n));
}

int plab_apply(int imgid, int base_end, const plab_layer_t *layers, int n_layers,
               char **err)
{
  dt_develop_t dev;
  _load_truncated(&dev, imgid, base_end);

  gboolean ok = TRUE;
  for(int i = 0; i < n_layers && ok; i++) ok = _apply_layer(&dev, &layers[i], err);

  // drop whatever an earlier apply left above the base, even with no layers
  if(ok)
  {
    GList *tail = g_list_nth(dev.history, dev.history_end);
    if(tail)
    {
      if(tail->prev) tail->prev->next = NULL;
      else dev.history = NULL;
      tail->prev = NULL;
      g_list_free_full(tail, dt_dev_free_history_item);
    }
    dt_dev_write_history_ext(&dev, imgid);
    dt_image_write_sidecar_file(imgid);
    // the library's thumbnail shows the old edit until it is dropped
    dt_mipmap_cache_remove(imgid);
  }
  dt_dev_cleanup(&dev);
  return ok ? 0 : -1;
}

int plab_apply_style(int imgid, int base_end, const char *name, char **err)
{
  if(!dt_styles_exists(name))
  {
    _seterr(err, "no style named '%s'", name);
    return base_end;
  }
  if(plab_apply(imgid, base_end, NULL, 0, err)) return base_end;
  dt_styles_apply_to_image(name, FALSE, FALSE, imgid);
  int end = base_end;
  dt_image_get_history_end(imgid, &end);
  return end;
}

int plab_import_styles(const char *path)
{
  int n = 0;
  if(g_file_test(path, G_FILE_TEST_IS_DIR))
  {
    GDir *dir = g_dir_open(path, 0, NULL);
    const gchar *f;
    while(dir && (f = g_dir_read_name(dir)))
    {
      if(!g_str_has_suffix(f, ".dtstyle")) continue;
      gchar *full = g_build_filename(path, f, NULL);
      dt_styles_import_from_file(full);
      g_free(full);
      n++;
    }
    if(dir) g_dir_close(dir);
  }
  else if(g_file_test(path, G_FILE_TEST_IS_REGULAR))
  {
    dt_styles_import_from_file(path);
    n = 1;
  }
  return n;
}

char *plab_list_styles(void)
{
  GString *s = g_string_new(NULL);
  GList *styles = dt_styles_get_list("");
  for(GList *l = styles; l; l = g_list_next(l))
  {
    dt_style_t *st = l->data;
    if(s->len) g_string_append_c(s, '\n');
    g_string_append(s, st->name);
  }
  g_list_free_full(styles, dt_style_free);
  return g_string_free(s, FALSE);
}

int plab_export(int imgid, const char *out_path, int quality, char **err)
{
  return plab_export_sized(imgid, out_path, quality, 0, err);
}

int plab_export_sized(int imgid, const char *out_path, int quality, int max_size,
                      char **err)
{
  const char *dot = strrchr(out_path, '.');
  gchar *ext = g_ascii_strdown(dot ? dot + 1 : "jpg", -1);
  const char *name = !strcmp(ext, "jpg") ? "jpeg" : !strcmp(ext, "tif") ? "tiff" : ext;
  dt_imageio_module_format_t *fmt = dt_imageio_get_format_by_name(name);
  g_free(ext);
  if(!fmt)
  {
    _seterr(err, "no export format for '%s'", out_path);
    return -1;
  }

  gchar *qkey = g_strdup_printf("plugins/imageio/format/%s/quality", fmt->plugin_name);
  if(quality > 0 && dt_conf_key_exists(qkey)) dt_conf_set_int(qkey, quality);
  g_free(qkey);

  dt_imageio_module_data_t *fdata = fmt->get_params(fmt);
  if(!fdata)
  {
    _seterr(err, "export format '%s' failed", fmt->plugin_name);
    return -1;
  }
  // a square bound limits the long edge whichever way the photo is turned
  fdata->max_width = MAX(max_size, 0);
  fdata->max_height = MAX(max_size, 0);
  fdata->style[0] = '\0';
  fdata->style_append = TRUE;

  gchar *parent = g_path_get_dirname(out_path);
  g_mkdir_with_parents(parent, 0755);
  g_free(parent);

  const gboolean failed = dt_imageio_export_with_flags(
      imgid, out_path, fmt, fdata, FALSE, FALSE, TRUE, FALSE, FALSE, 1.0, FALSE, NULL,
      TRUE, FALSE, DT_COLORSPACE_SRGB, NULL, DT_INTENT_PERCEPTUAL, NULL, NULL,
      1, 1, NULL, -1);
  fmt->free_params(fmt, fdata);
  if(failed) _seterr(err, "could not write '%s'", out_path);
  return failed ? -1 : 0;
}

// ---------------------------------------------------------------------------
// library
// ---------------------------------------------------------------------------

static int _import_dir(const char *folder, const gboolean recursive)
{
  GDir *dir = g_dir_open(folder, 0, NULL);
  if(!dir) return 0;
  int n = 0;
  dt_filmid_t filmid = NO_FILMID;
  const gchar *f;
  while((f = g_dir_read_name(dir)))
  {
    if(f[0] == '.') continue;
    gchar *full = g_build_filename(folder, f, NULL);
    if(g_file_test(full, G_FILE_TEST_IS_DIR))
    {
      if(recursive) n += _import_dir(full, recursive);
    }
    else if(dt_supported_image(f))
    {
      if(!dt_is_valid_filmid(filmid))
      {
        dt_film_t film;
        dt_film_init(&film);
        filmid = dt_film_new(&film, folder);
        dt_film_cleanup(&film);
      }
      if(dt_is_valid_filmid(filmid)
         && dt_is_valid_imgid(dt_image_import(filmid, full, FALSE, FALSE)))
        n++;
    }
    g_free(full);
  }
  g_dir_close(dir);
  return n;
}

int plab_import_folder(const char *folder, int recursive)
{
  GFile *gf = g_file_new_for_path(folder);
  gchar *canon = g_file_get_path(gf);
  g_object_unref(gf);
  const int n = g_file_test(canon, G_FILE_TEST_IS_DIR) ? _import_dir(canon, recursive) : -1;
  g_free(canon);
  return n;
}

char *plab_list_folders(void)
{
  GString *s = g_string_new(NULL);
  sqlite3_stmt *st = NULL;
  // clang-format off
  sqlite3_prepare_v2(dt_database_get(darktable.db),
                     "SELECT f.id, f.folder, COUNT(i.id)"
                     " FROM main.film_rolls AS f"
                     " JOIN main.images AS i ON i.film_id = f.id"
                     " GROUP BY f.id ORDER BY f.folder",
                     -1, &st, NULL);
  // clang-format on
  while(st && sqlite3_step(st) == SQLITE_ROW)
    g_string_append_printf(s, "%d\t%s\t%d\n", sqlite3_column_int(st, 0),
                           (const char *)sqlite3_column_text(st, 1),
                           sqlite3_column_int(st, 2));
  sqlite3_finalize(st);
  return g_string_free(s, FALSE);
}

char *plab_list_images(int film_id, int min_rating, int rejected_only)
{
  GString *s = g_string_new(NULL);
  sqlite3_stmt *st = NULL;
  // clang-format off
  sqlite3_prepare_v2(dt_database_get(darktable.db),
                     "SELECT i.id, f.folder || '/' || i.filename, i.flags & 7,"
                     "       (SELECT IFNULL(SUM(1 << c.color), 0)"
                     "          FROM main.color_labels AS c WHERE c.imgid = i.id),"
                     "       i.history_end > 0"
                     " FROM main.images AS i"
                     " JOIN main.film_rolls AS f ON f.id = i.film_id"
                     " WHERE (?1 < 0 OR i.film_id = ?1)"
                     "   AND ((?3 = 1 AND i.flags & 7 = 6)"
                     "        OR (?3 = 0 AND i.flags & 7 != 6 AND i.flags & 7 >= ?2))"
                     " ORDER BY i.datetime_taken, i.filename",
                     -1, &st, NULL);
  // clang-format on
  if(st)
  {
    sqlite3_bind_int(st, 1, film_id);
    sqlite3_bind_int(st, 2, min_rating);
    sqlite3_bind_int(st, 3, rejected_only ? 1 : 0);
  }
  while(st && sqlite3_step(st) == SQLITE_ROW)
    g_string_append_printf(s, "%d\t%s\t%d\t%d\t%d\n", sqlite3_column_int(st, 0),
                           (const char *)sqlite3_column_text(st, 1),
                           sqlite3_column_int(st, 2), sqlite3_column_int(st, 3),
                           sqlite3_column_int(st, 4));
  sqlite3_finalize(st);
  return g_string_free(s, FALSE);
}

void plab_set_rating(int imgid, int rating)
{
  dt_ratings_apply_on_image(imgid, CLAMP(rating, 0, DT_VIEW_REJECT), FALSE, FALSE, FALSE);
  dt_image_write_sidecar_file(imgid);
}

void plab_toggle_color_label(int imgid, int color)
{
  GList *l = g_list_prepend(NULL, GINT_TO_POINTER(imgid));
  dt_colorlabels_toggle_label_on_list(l, color, FALSE);
  g_list_free(l);
  dt_image_write_sidecar_file(imgid);
}

uint8_t *plab_thumbnail(int imgid, int max_w, int max_h, int *out_w, int *out_h)
{
  const dt_mipmap_size_t size = dt_mipmap_cache_get_matching_size(max_w, max_h);
  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(&buf, imgid, size, DT_MIPMAP_BLOCKING, 'r');
  uint8_t *out = NULL;
  if(buf.buf && buf.width > 0 && buf.height > 0)
  {
    const size_t bytes = (size_t)buf.width * buf.height * 4;
    out = g_try_malloc(bytes);
    if(out)
    {
      memcpy(out, buf.buf, bytes);
      *out_w = buf.width;
      *out_h = buf.height;
    }
  }
  dt_mipmap_cache_release(&buf);
  return out;
}

void plab_set_lut_folder(const char *folder)
{
  dt_conf_set_string("plugins/darkroom/lut3d/def_path", folder);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

// ---------------------------------------------------------------------------
// AI denoise: the raw path of src/libs/neural_restore.c without its GUI. the
// sensor data goes in and a DNG comes out at the same stage, so darktable's
// whole pipeline then runs on cleaner data
// ---------------------------------------------------------------------------

#define PLAB_RAWDENOISE_MODEL "rawdenoise-nind"

#ifdef HAVE_AI

int plab_ai_built(void)
{
  return 1;
}

int plab_ai_model_ready(void)
{
  if(!dt_ai_registry_is_enabled()) return 0;
  dt_ai_model_t *m = dt_ai_models_get_by_id(PLAB_RAWDENOISE_MODEL);
  const int ok = m && (m->status == DT_AI_MODEL_DOWNLOADED
                       || m->status == DT_AI_MODEL_UPDATE_AVAILABLE);
  dt_ai_model_free(m);
  return ok;
}

typedef struct _progress_t
{
  void (*cb)(double, void *);
  void *data;
} _progress_t;

static void _download_progress(const char *model_id, const double progress, gpointer user_data)
{
  const _progress_t *p = user_data;
  if(p->cb) p->cb(progress, p->data);
}

int plab_ai_prepare(void (*progress)(double fraction, void *data), void *data, char **err)
{
  if(!darktable.ai_registry)
  {
    _seterr(err, "darktable's AI engine did not start");
    return -1;
  }
  // as the AI preferences tab does when the switch is turned on
  dt_conf_set_bool("plugins/ai/enabled", TRUE);
  dt_ai_registry_set_enabled(TRUE);
  dt_ai_models_init_lazy();
  if(plab_ai_model_ready()) return 0;

  _progress_t p = { progress, data };
  char *msg = dt_ai_models_download_sync(PLAB_RAWDENOISE_MODEL, _download_progress, &p, NULL);
  if(msg)
  {
    _seterr(err, "could not download the AI denoise model: %s", msg);
    g_free(msg);
    return -1;
  }
  if(!plab_ai_model_ready())
  {
    _seterr(err, "the AI denoise model is not usable after downloading it");
    return -1;
  }
  return 0;
}

static char *_denoise_target(const char *src)
{
  gchar *dir = g_path_get_dirname(src);
  gchar *base = g_path_get_basename(src);
  gchar *dot = strrchr(base, '.');
  if(dot) *dot = '\0';
  gchar *out = NULL;
  for(int k = 1; !out || g_file_test(out, G_FILE_TEST_EXISTS); k++)
  {
    g_free(out);
    gchar *name = k == 1 ? g_strdup_printf("%s-AI-Denoise.dng", base)
                         : g_strdup_printf("%s-AI-Denoise-%d.dng", base, k);
    out = g_build_filename(dir, name, NULL);
    g_free(name);
  }
  g_free(dir);
  g_free(base);
  return out;
}

static int _denoise_bayer(dt_restore_context_t *ctx, const dt_imgid_t imgid,
                          const dt_image_t *img, const char *src, const char *out,
                          const float strength)
{
  dt_mipmap_buffer_t mbuf;
  dt_mipmap_cache_get(&mbuf, imgid, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');
  if(!mbuf.buf)
  {
    dt_mipmap_cache_release(&mbuf);
    return -1;
  }
  const int width = img->width, height = img->height;
  const size_t npix = (size_t)width * height;
  float *cfa_in = g_try_malloc(npix * sizeof(float));
  uint16_t *cfa_out = g_try_malloc(npix * sizeof(uint16_t));
  int res = -1;
  if(cfa_in && cfa_out)
  {
    if(img->buf_dsc.datatype == TYPE_UINT16)
    {
      const uint16_t *const raw = (const uint16_t *)mbuf.buf;
      for(size_t i = 0; i < npix; i++) cfa_in[i] = (float)raw[i];
      res = 0;
    }
    else if(img->buf_dsc.datatype == TYPE_FLOAT)
    {
      memcpy(cfa_in, mbuf.buf, npix * sizeof(float));
      res = 0;
    }
  }
  dt_mipmap_cache_release(&mbuf);

  if(res == 0)
    res = dt_restore_raw_bayer(ctx, img, cfa_in, width, height, cfa_out, strength, NULL);
  g_free(cfa_in);
  if(res == 0)
  {
    uint8_t *exif = NULL;
    const int exif_len = dt_exif_read_blob(&exif, src, imgid, FALSE, width, height, TRUE);
    res = dt_imageio_dng_write_cfa_bayer(out, cfa_out, width, height, img, exif, exif_len, NULL);
    g_free(exif);
  }
  g_free(cfa_out);
  return res;
}

static int _denoise_linear(dt_restore_context_t *ctx, const dt_imgid_t imgid,
                           const dt_image_t *img, const char *src, const char *out,
                           const float strength)
{
  float *rgb = NULL;
  int w = 0, h = 0;
  int res = dt_restore_raw_linear(ctx, imgid, &rgb, &w, &h, strength, NULL);
  if(res != 0 || !rgb)
  {
    if(rgb) dt_free_align(rgb);
    return -1;
  }
  uint8_t *exif = NULL;
  const int exif_len = dt_exif_read_blob(&exif, src, imgid, FALSE, w, h, TRUE);
  res = dt_imageio_dng_write_linear(out, rgb, w, h, img, exif, exif_len, NULL);
  g_free(exif);
  dt_free_align(rgb);
  return res;
}

int plab_ai_denoise(int imgid, float strength, char **err)
{
  if(!plab_ai_model_ready())
  {
    _seterr(err, "the AI denoise model is not installed");
    return -1;
  }
  // loading the full raw fills in buf_dsc (filters, datatype), which a
  // fresh catalog entry does not have yet
  dt_mipmap_buffer_t warm;
  dt_mipmap_cache_get(&warm, imgid, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');
  const gboolean loaded = warm.buf != NULL;
  dt_mipmap_cache_release(&warm);
  if(!loaded)
  {
    _seterr(err, "could not read the raw data");
    return -1;
  }
  const dt_image_t *cached = dt_image_cache_get(imgid, 'r');
  if(!cached)
  {
    _seterr(err, "image %d is not in the library", imgid);
    return -1;
  }
  const dt_image_t img = *cached;
  dt_image_cache_read_release(cached);

  const dt_restore_sensor_class_t cls = dt_restore_classify_sensor(&img);
  if(cls == DT_RESTORE_SENSOR_CLASS_UNSUPPORTED)
  {
    _seterr(err, "AI denoise works on raw files only");
    return -1;
  }

  char src[PATH_MAX] = { 0 };
  gboolean from_cache = FALSE;
  dt_image_full_path(imgid, src, sizeof(src), &from_cache);

  dt_restore_env_t *env = dt_restore_env_init();
  dt_restore_context_t *ctx = !env ? NULL
    : cls == DT_RESTORE_SENSOR_CLASS_BAYER  ? dt_restore_load_rawdenoise_bayer(env)
    : cls == DT_RESTORE_SENSOR_CLASS_XTRANS ? dt_restore_load_rawdenoise_xtrans(env)
                                            : dt_restore_load_rawdenoise_linear(env);
  if(!ctx)
  {
    dt_restore_env_destroy(env);
    _seterr(err, "could not load the AI denoise model");
    return -1;
  }

  char *out = _denoise_target(src);
  const float s = CLAMP(strength, 0.0f, 1.0f);
  // X-Trans runs through the linear model, as in neural_restore.c
  const int res = cls == DT_RESTORE_SENSOR_CLASS_BAYER
                    ? _denoise_bayer(ctx, imgid, &img, src, out, s)
                    : _denoise_linear(ctx, imgid, &img, src, out, s);
  dt_restore_unref(ctx);
  dt_restore_env_destroy(env);
  if(res != 0)
  {
    g_unlink(out);
    _seterr(err, "AI denoise failed on %s", src);
    g_free(out);
    return -1;
  }
  // stops darktable's auto-applied denoise presets running on it again
  dt_exif_xmp_write_neural_restore(out, "raw-denoise");

  const int newid = plab_import(out, err);
  g_free(out);
  if(newid < 0) return -1;

  const dt_image_t *si = dt_image_cache_get(imgid, 'r');
  const int rating = si ? dt_image_get_xmp_rating(si) : 0;
  const dt_imgid_t group = si && dt_is_valid_imgid(si->group_id) ? si->group_id : imgid;
  dt_image_cache_read_release(si);
  dt_image_t *ni = dt_image_cache_get(newid, 'w');
  dt_image_set_xmp_rating(ni, rating);
  dt_image_cache_write_release(ni, DT_IMAGE_CACHE_SAFE);
  const int labels = dt_colorlabels_get_labels(imgid);
  for(int c = 0; c < DT_COLORLABELS_LAST; c++)
    if(labels & (1 << c)) dt_colorlabels_set_label(newid, c);
  dt_grouping_add_to_group(group, newid);
  return newid;
}

#else

int plab_ai_built(void)
{
  return 0;
}

int plab_ai_model_ready(void)
{
  return 0;
}

int plab_ai_prepare(void (*progress)(double fraction, void *data), void *data, char **err)
{
  _seterr(err, "this build has no AI support");
  return -1;
}

int plab_ai_denoise(int imgid, float strength, char **err)
{
  _seterr(err, "this build has no AI support");
  return -1;
}

#endif
