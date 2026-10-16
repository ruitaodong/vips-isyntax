/* vips-isyntax: a libvips loader module for Philips iSyntax whole-slide images, using libisyntax.
 *
 * Adds the operation "isyntaxload". Once installed in the libvips module directory
 * (e.g. /usr/lib/x86_64-linux-gnu/vips-modules-8.15/), .isyntax files open like any other format:
 *
 *   vips copy slide.isyntax out.v                 # full resolution
 *   vips copy "slide.isyntax[level=2]" out.v      # a lower-resolution level
 *   vips isyntaxload slide.isyntax out.v --level 1
 *   vips isyntaxload slide.isyntax label.png --associated label   # or macro
 *   pyvips.Image.new_from_file("slide.isyntax", level=0)
 *
 * Output: 3-band uchar sRGB-tagged pixels as stored by the scanner (no colour conversion),
 * xres/yres from the slide's MPP, the embedded ICC profile as "icc-profile-data", and
 * slide metadata as "isyntax.*" fields.
 *
 * Threading: libisyntax holds its cache lock for the whole tile decode, so decoding of one
 * slide is serialised; vips threads still overlap everything downstream (colour, resize, save).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gmodule.h>
#include <vips/vips.h>

#include "libisyntax.h"

#define HAVE_UNTRUSTED (VIPS_MAJOR_VERSION > 8 || (VIPS_MAJOR_VERSION == 8 && VIPS_MINOR_VERSION >= 13))

/* ---- one open slide: file handle + tile cache, owned by an image ---- */

typedef struct {
	char *filename;
	int level;
	isyntax_t *isyntax;
	isyntax_cache_t *cache;
	GMutex lock;
	int width, height;
} ReadIsyntax;

static void
read_free(ReadIsyntax *read)
{
	if (!read)
		return;
	if (read->cache)
		libisyntax_cache_destroy(read->cache);
	if (read->isyntax)
		libisyntax_close(read->isyntax);
	g_mutex_clear(&read->lock);
	g_free(read->filename);
	g_free(read);
}

static void
read_close_cb(VipsImage *image, ReadIsyntax *read)
{
	read_free(read);
}

/* Open the slide and validate the level. with_cache: also set up tile decoding. */
static ReadIsyntax *
read_new(const char *filename, int level, gboolean with_cache)
{
	ReadIsyntax *read = g_new0(ReadIsyntax, 1);
	const isyntax_image_t *wsi;
	const isyntax_level_t *lvl;
	int n_levels;

	g_mutex_init(&read->lock);
	read->filename = g_strdup(filename);
	read->level = level;

	if (libisyntax_open(filename, 0, &read->isyntax) != LIBISYNTAX_OK) {
		vips_error("isyntaxload", "unable to open \"%s\" as iSyntax", filename);
		read_free(read);
		return NULL;
	}
	wsi = libisyntax_get_wsi_image(read->isyntax);
	n_levels = wsi ? libisyntax_image_get_level_count(wsi) : 0;
	if (n_levels <= 0) {
		vips_error("isyntaxload", "\"%s\": no whole-slide image", filename);
		read_free(read);
		return NULL;
	}
	if (level < 0 || level >= n_levels) {
		vips_error("isyntaxload", "level %d out of range, slide has levels 0..%d", level, n_levels - 1);
		read_free(read);
		return NULL;
	}
	lvl = libisyntax_image_get_level(wsi, level);
	read->width = libisyntax_level_get_width(lvl);
	read->height = libisyntax_level_get_height(lvl);
	if (read->width <= 0 || read->height <= 0) {
		vips_error("isyntaxload", "\"%s\": level %d has no pixels", filename, level);
		read_free(read);
		return NULL;
	}

	if (with_cache) {
		/* cache size in tiles; libisyntax's own examples use 2000 */
		if (libisyntax_cache_create("vips-isyntax", 2000, &read->cache) != LIBISYNTAX_OK ||
			libisyntax_cache_inject(read->cache, read->isyntax) != LIBISYNTAX_OK) {
			vips_error("isyntaxload", "\"%s\": unable to create tile cache", filename);
			read_free(read);
			return NULL;
		}
	}
	return read;
}

static int
free_blob_cb(void *mem, void *client)
{
	g_free(mem);
	return 0;
}

static void
set_string(VipsImage *out, const char *name, const char *value)
{
	if (value && value[0])
		vips_image_set_string(out, name, value);
}

/* Header fields and metadata for the chosen level. */
static int
read_header(ReadIsyntax *read, VipsImage *out)
{
	isyntax_t *s = read->isyntax;
	const isyntax_image_t *wsi = libisyntax_get_wsi_image(s);
	const isyntax_level_t *lvl = libisyntax_image_get_level(wsi, read->level);
	int n_levels = libisyntax_image_get_level_count(wsi);
	double mpp_x = libisyntax_level_get_mpp_x(lvl);
	double mpp_y = libisyntax_level_get_mpp_y(lvl);
	double xres = 1.0, yres = 1.0; /* pixels per mm; 1.0 = unknown, the libvips default */
	uint8_t *icc = NULL;
	uint32_t icc_size = 0;
	char name[64];
	int i;

	if (libisyntax_get_is_mpp_known(s) && mpp_x > 0 && mpp_y > 0) {
		xres = 1000.0 / mpp_x;
		yres = 1000.0 / mpp_y;
	}

	vips_image_init_fields(out, read->width, read->height, 3, VIPS_FORMAT_UCHAR,
		VIPS_CODING_NONE, VIPS_INTERPRETATION_sRGB, xres, yres);
	if (vips_image_pipelinev(out, VIPS_DEMAND_STYLE_SMALLTILE, NULL))
		return -1;

	vips_image_set_int(out, "isyntax.level", read->level);
	vips_image_set_int(out, "isyntax.level-count", n_levels);
	vips_image_set_int(out, "isyntax.tile-width", libisyntax_get_tile_width(s));
	vips_image_set_int(out, "isyntax.tile-height", libisyntax_get_tile_height(s));
	if (libisyntax_get_is_mpp_known(s)) {
		vips_image_set_double(out, "isyntax.mpp-x", libisyntax_get_mpp_x(s));
		vips_image_set_double(out, "isyntax.mpp-y", libisyntax_get_mpp_y(s));
	}
	vips_image_set_double(out, "isyntax.level-mpp-x", mpp_x);
	vips_image_set_double(out, "isyntax.level-mpp-y", mpp_y);
	vips_image_set_double(out, "isyntax.downsample", libisyntax_level_get_downsample_factor(lvl));
	for (i = 0; i < n_levels; i++) {
		const isyntax_level_t *l = libisyntax_image_get_level(wsi, i);
		g_snprintf(name, sizeof(name), "isyntax.level[%d].width", i);
		vips_image_set_int(out, name, libisyntax_level_get_width(l));
		g_snprintf(name, sizeof(name), "isyntax.level[%d].height", i);
		vips_image_set_int(out, name, libisyntax_level_get_height(l));
	}
	vips_image_set_string(out, "isyntax.associated-images", "label, macro");
	set_string(out, "isyntax.barcode", libisyntax_get_barcode(s));
	set_string(out, "isyntax.manufacturer", libisyntax_get_manufacturer(s));
	set_string(out, "isyntax.model", libisyntax_get_manufacturers_model_name(s));
	set_string(out, "isyntax.serial-number", libisyntax_get_device_serial_number(s));
	set_string(out, "isyntax.acquisition-datetime", libisyntax_get_acquisition_datetime(s));
	set_string(out, "isyntax.derivation", libisyntax_get_derivation_description(s));
	if (libisyntax_is_lossy_image_compression(s)) {
		set_string(out, "isyntax.compression-method", libisyntax_get_lossy_image_compression_method(s));
		vips_image_set_double(out, "isyntax.compression-ratio", libisyntax_get_lossy_image_compression_ratio(s));
	}

	/* Embedded ICC profile, like tiffload does. Not applied: pixels stay as stored. */
	if (libisyntax_read_icc_profile(s, (isyntax_image_t *) wsi, &icc, &icc_size) == LIBISYNTAX_OK &&
		icc && icc_size > 0) {
		void *copy = g_malloc(icc_size); /* not g_memdup2: needs GLib 2.68, focal has 2.64 */
		memcpy(copy, icc, icc_size);
		vips_image_set_blob(out, VIPS_META_ICC_NAME, free_blob_cb, copy, icc_size);
	}
	free(icc);

	return 0;
}

/* Fill a region: libisyntax decodes RGBA (bytes R,G,B,A), we keep R,G,B. */
static int
read_generate(VipsRegion *out, void *seq, void *a, void *b, gboolean *stop)
{
	ReadIsyntax *read = (ReadIsyntax *) a;
	VipsRect *r = &out->valid;
	size_t n = (size_t) r->width * r->height;
	uint32_t *buf;
	isyntax_error_t err;
	int x, y;

	if (!(buf = g_try_malloc(n * 4))) {
		vips_error("isyntaxload", "out of memory");
		return -1;
	}

	g_mutex_lock(&read->lock);
	err = libisyntax_read_region(read->isyntax, read->cache, read->level,
		r->left, r->top, r->width, r->height, buf, LIBISYNTAX_PIXEL_FORMAT_RGBA);
	g_mutex_unlock(&read->lock);

	if (err != LIBISYNTAX_OK) {
		g_free(buf);
		vips_error("isyntaxload", "\"%s\": read error at %d,%d (%dx%d), level %d",
			read->filename, r->left, r->top, r->width, r->height, read->level);
		return -1;
	}

	for (y = 0; y < r->height; y++) {
		const uint8_t *p = (const uint8_t *) (buf + (size_t) y * r->width);
		VipsPel *q = VIPS_REGION_ADDR(out, r->left, r->top + y);
		for (x = 0; x < r->width; x++) {
			q[0] = p[0];
			q[1] = p[1];
			q[2] = p[2];
			q += 3;
			p += 4;
		}
	}
	g_free(buf);
	return 0;
}

/* ---- label / macro ("associated") images: small JPEGs, decoded in one go ---- */

/* Decode the named associated image to RGBA. Caller frees *pixels with free(). */
static int
read_associated(const char *filename, const char *name, int *w, int *h, uint32_t **pixels)
{
	isyntax_t *s;
	isyntax_error_t err;
	int32_t ww = 0, hh = 0;

	*pixels = NULL;
	if (strcmp(name, "label") != 0 && strcmp(name, "macro") != 0) {
		vips_error("isyntaxload", "unknown associated image \"%s\", use \"label\" or \"macro\"", name);
		return -1;
	}
	if (libisyntax_open(filename, 0, &s) != LIBISYNTAX_OK) {
		vips_error("isyntaxload", "unable to open \"%s\" as iSyntax", filename);
		return -1;
	}
	err = strcmp(name, "label") == 0
		? libisyntax_read_label_image(s, &ww, &hh, pixels, LIBISYNTAX_PIXEL_FORMAT_RGBA)
		: libisyntax_read_macro_image(s, &ww, &hh, pixels, LIBISYNTAX_PIXEL_FORMAT_RGBA);
	libisyntax_close(s);
	if (err != LIBISYNTAX_OK || !*pixels || ww <= 0 || hh <= 0) {
		free(*pixels);
		*pixels = NULL;
		vips_error("isyntaxload", "\"%s\": no readable %s image", filename, name);
		return -1;
	}
	*w = ww;
	*h = hh;
	return 0;
}

/* ---- the VipsForeignLoad subclass ---- */

typedef struct _VipsForeignLoadIsyntax {
	VipsForeignLoad parent_object;
	char *filename;
	int level;
	char *associated;
} VipsForeignLoadIsyntax;

typedef VipsForeignLoadClass VipsForeignLoadIsyntaxClass;

G_DEFINE_TYPE(VipsForeignLoadIsyntax, vips_foreign_load_isyntax, VIPS_TYPE_FOREIGN_LOAD);

static VipsForeignFlags
vips_foreign_load_isyntax_get_flags_filename(const char *filename)
{
	return VIPS_FOREIGN_PARTIAL;
}

static VipsForeignFlags
vips_foreign_load_isyntax_get_flags(VipsForeignLoad *load)
{
	return VIPS_FOREIGN_PARTIAL;
}

/* iSyntax files start with an XML header: <DataObject ObjectType="DPUfsImport"> */
static gboolean
vips_foreign_load_isyntax_is_a(const char *filename)
{
	char buf[512];
	size_t n;
	FILE *fp;

	if (!(fp = fopen(filename, "rb")))
		return FALSE;
	n = fread(buf, 1, sizeof(buf) - 1, fp);
	fclose(fp);
	buf[n] = '\0';
	return strstr(buf, "<DataObject") != NULL && strstr(buf, "DPUfsImport") != NULL;
}

static int
vips_foreign_load_isyntax_header(VipsForeignLoad *load)
{
	VipsForeignLoadIsyntax *isyntax = (VipsForeignLoadIsyntax *) load;
	ReadIsyntax *read;
	int result;

	if (isyntax->associated) {
		uint32_t *px;
		int w, h;

		if (read_associated(isyntax->filename, isyntax->associated, &w, &h, &px))
			return -1;
		free(px);
		vips_image_init_fields(load->out, w, h, 3, VIPS_FORMAT_UCHAR,
			VIPS_CODING_NONE, VIPS_INTERPRETATION_sRGB, 1.0, 1.0);
		if (vips_image_pipelinev(load->out, VIPS_DEMAND_STYLE_ANY, NULL))
			return -1;
		vips_image_set_string(load->out, "isyntax.associated", isyntax->associated);
		VIPS_SETSTR(load->out->filename, isyntax->filename);
		return 0;
	}

	if (!(read = read_new(isyntax->filename, isyntax->level, FALSE)))
		return -1;
	result = read_header(read, load->out);
	read_free(read);
	if (!result)
		VIPS_SETSTR(load->out->filename, isyntax->filename);
	return result;
}

static int
vips_foreign_load_isyntax_load(VipsForeignLoad *load)
{
	VipsForeignLoadIsyntax *isyntax = (VipsForeignLoadIsyntax *) load;
	VipsImage **t = (VipsImage **) vips_object_local_array(VIPS_OBJECT(load), 2);
	ReadIsyntax *read;
	int tw, th;

	if (isyntax->associated) {
		VipsImage **a = (VipsImage **) vips_object_local_array(VIPS_OBJECT(load), 3);
		uint32_t *px;
		int w, h;

		if (read_associated(isyntax->filename, isyntax->associated, &w, &h, &px))
			return -1;
		a[0] = vips_image_new_from_memory_copy(px, (size_t) w * h * 4, w, h, 4, VIPS_FORMAT_UCHAR);
		free(px);
		if (!a[0] ||
			vips_extract_band(a[0], &a[1], 0, "n", 3, NULL) ||
			vips_copy(a[1], &a[2], "interpretation", VIPS_INTERPRETATION_sRGB, NULL) ||
			vips_image_write(a[2], load->real))
			return -1;
		return 0;
	}

	if (!(read = read_new(isyntax->filename, isyntax->level, TRUE)))
		return -1;
	t[0] = vips_image_new();
	/* the image owns the slide handle and frees it when the image closes */
	g_signal_connect(t[0], "close", G_CALLBACK(read_close_cb), read);

	if (read_header(read, t[0]) ||
		vips_image_generate(t[0], NULL, read_generate, NULL, read, NULL))
		return -1;

	/* Cache decoded tiles, one slide-tile-sized cell at a time, about three rows deep. */
	tw = libisyntax_get_tile_width(read->isyntax);
	th = libisyntax_get_tile_height(read->isyntax);
	if (tw <= 0 || th <= 0)
		tw = th = 512;
	if (vips_tilecache(t[0], &t[1],
			"tile_width", tw,
			"tile_height", th,
			"max_tiles", 3 * (read->width / tw + 1),
			"threaded", TRUE,
			NULL) ||
		vips_image_write(t[1], load->real))
		return -1;

	return 0;
}

static const char *vips_foreign_isyntax_suffs[] = { ".isyntax", NULL };

static void
vips_foreign_load_isyntax_class_init(VipsForeignLoadIsyntaxClass *class)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(class);
	VipsObjectClass *object_class = (VipsObjectClass *) class;
#if HAVE_UNTRUSTED
	VipsOperationClass *operation_class = VIPS_OPERATION_CLASS(class);
#endif
	VipsForeignClass *foreign_class = (VipsForeignClass *) class;
	VipsForeignLoadClass *load_class = (VipsForeignLoadClass *) class;

	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	object_class->nickname = "isyntaxload";
	object_class->description = "load Philips iSyntax with libisyntax";

#if HAVE_UNTRUSTED
	/* third-party parser that hasn't been fuzzed: blocked when VIPS_BLOCK_UNTRUSTED is set */
	operation_class->flags |= VIPS_OPERATION_UNTRUSTED;
#endif

	foreign_class->suffs = vips_foreign_isyntax_suffs;
	foreign_class->priority = 100;

	load_class->get_flags_filename = vips_foreign_load_isyntax_get_flags_filename;
	load_class->get_flags = vips_foreign_load_isyntax_get_flags;
	load_class->is_a = vips_foreign_load_isyntax_is_a;
	load_class->header = vips_foreign_load_isyntax_header;
	load_class->load = vips_foreign_load_isyntax_load;

	VIPS_ARG_STRING(class, "filename", 1,
		"Filename",
		"Filename to load from",
		VIPS_ARGUMENT_REQUIRED_INPUT,
		G_STRUCT_OFFSET(VipsForeignLoadIsyntax, filename),
		NULL);

	VIPS_ARG_INT(class, "level", 20,
		"Level",
		"Load this level (0 = full resolution)",
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET(VipsForeignLoadIsyntax, level),
		0, 100, 0);

	VIPS_ARG_STRING(class, "associated", 21,
		"Associated",
		"Load this associated image instead: \"label\" or \"macro\"",
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET(VipsForeignLoadIsyntax, associated),
		NULL);
}

static void
vips_foreign_load_isyntax_init(VipsForeignLoadIsyntax *isyntax)
{
}

/* ---- module entry point: libvips calls this when it loads the module ---- */

G_MODULE_EXPORT const gchar *
g_module_check_init(GModule *module)
{
	if (libisyntax_init() != LIBISYNTAX_OK)
		return "vips-isyntax: libisyntax_init() failed";
	vips_foreign_load_isyntax_get_type();
	/* GTypes can't be unregistered, so the module must never be unloaded */
	g_module_make_resident(module);
	return NULL;
}
