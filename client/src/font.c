#include <string.h>
#include <assert.h>

#include <wayland-util.h>

#include "font.h"
#include "util.h"

#define REPLACEMENT_CHAR 0x0000fffd

static uint32_t utf8_to_utf32(char const **utf8)
{
	static unsigned char const tail_len[256] = {
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
		3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0
	};
	unsigned char tail, c = *(*utf8)++;
	uint32_t r = 0;

	if(c < 0x80)
		return c;

	tail = tail_len[c];

	/* middle of a character or tail too long */
	if(!tail) {
		while((c & 0xc0) == 0x80)
			c = *(*utf8)++;
		return REPLACEMENT_CHAR;
	}

	/* remove length specification bits */
	r = c & 0x3f >> tail;

	while(tail--) {
		c = *(*utf8)++;
		if((c & 0xc0) != 0x80)
			return --(*utf8), REPLACEMENT_CHAR;
		r = (r << 6) + (c & 0x3f);
	}
	return r;
}

static FT_Library ftlib;
static FcConfig *config;

struct font {
	int ref;
	cairo_scaled_font_t *font;
	cairo_font_face_t *face;
	FT_Face ftface;
};

struct text {
	struct font *font;
	struct wl_array glyphs;
	unsigned num;
};

struct font *font_create(char const *family, int size)
{
	FcPattern *match, *pattern = FcPatternCreate();
	FcResult match_result;

	cairo_user_data_key_t const key;
	cairo_matrix_t scale;
	cairo_matrix_t ident;
	cairo_font_options_t *options;

	struct font *font;
	unsigned char *file;
	int index;

	if(!pattern)
		goto err_pattern;
	if(!FcPatternAddString(pattern, FC_FAMILY, (unsigned char *)family))
		goto err_match;
	if(!FcConfigSubstitute(config, pattern, FcMatchPattern))
		goto err_match;
	FcDefaultSubstitute(pattern);
	match = FcFontMatch(config, pattern, &match_result);
	if(match_result != FcResultMatch)
		goto err_match;

	if(FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch)
		goto err_match;
	if(FcPatternGetInteger(match, FC_INDEX, 0, &index) != FcResultMatch)
		goto err_match;

	font = malloc(sizeof *font);
	if(!font)
		goto err_match;

	/* There is no need to manually select a cmap because freetype
	 * will use unicode by default and even emulate unicode if it's
	 * missing */
	if(FT_New_Face(ftlib, (char *)file, index, &font->ftface))
		goto err_ftface;

	font->face = cairo_ft_font_face_create_for_ft_face(font->ftface, 0);
	if(cairo_font_face_set_user_data(font->face, &key, font->ftface,
					 (cairo_destroy_func_t)FT_Done_Face)) {
		cairo_font_face_destroy(font->face);
		FT_Done_Face(font->ftface);
		goto err_ftface;
	}

	cairo_matrix_init_scale(&scale, size, size);
	cairo_matrix_init_identity(&ident);
	options = cairo_font_options_create();
	font->font = cairo_scaled_font_create(font->face, &scale, &ident,
					      options);
	cairo_font_options_destroy(options);
	if(cairo_scaled_font_status(font->font))
		goto err_crfont;

	FcPatternDestroy(match);
	FcPatternDestroy(pattern);

	font->ref = 1;
	return font;

err_crfont:
	cairo_scaled_font_destroy(font->font);
	cairo_font_face_destroy(font->face);
err_ftface:
	free(font);
err_match:
	/* I don't know if FcFontMatch can fail and still return a
	 * pattern, docs don't seem to say. */
	if(match)
		FcPatternDestroy(match);
	FcPatternDestroy(pattern);
err_pattern:
	return NULL;
}

void font_ref(struct font *font)
{
	font->ref++;
}

void font_unref(struct font *font)
{
	if(--font->ref)
		return;

	cairo_scaled_font_destroy(font->font);
	cairo_font_face_destroy(font->face);
	free(font);
}

struct text *text_create(struct font *font, char const *str)
{
	struct text *text = xmalloc(sizeof(struct text));
	FT_GlyphSlot slot = font->ftface->glyph;
	FT_UInt index;
	double x = 0, y = 0;
	uint32_t c;
	cairo_glyph_t *p;

	font_ref(font);
	text->font = font;
	text->num = 0;
	wl_array_init(&text->glyphs);

	while((c = utf8_to_utf32(&str))) {
		index = FT_Get_Char_Index(font->ftface, c);

		if(FT_Load_Glyph(font->ftface, index, FT_LOAD_DEFAULT)) {
			fprintf(stderr, "FT_Load_Char error\n");
			continue;
		}
		p = wl_array_add(&text->glyphs, sizeof(cairo_glyph_t));
		*p = (cairo_glyph_t){ index, x, y };

		++text->num;
		x += slot->advance.x >> 6;
		y += slot->advance.y >> 6;
	}

	return text;
}

void text_destroy(struct text *text)
{
	font_unref(text->font);
	wl_array_release(&text->glyphs);
	free(text);
}

void text_extents(struct text *text, unsigned *width, unsigned *height)
{
	cairo_text_extents_t ext;

	cairo_scaled_font_glyph_extents(text->font->font,
					text->glyphs.data,
					text->num,
					&ext);
	*width = ext.width;
	*height = ext.height;
}

void text_draw(struct text *text, cairo_t *cr)
{
	cairo_set_scaled_font(cr, text->font->font);
	cairo_show_glyphs(cr, text->glyphs.data, text->num);
}

int font_setup(void)
{
	if(FT_Init_FreeType(&ftlib))
		return -1;

	config = FcInitLoadConfigAndFonts();
	return 0;
}

int font_finish(void)
{
	FcConfigDestroy(config);
	if(FT_Done_FreeType(ftlib))
		return -1;
	return 0;
}
