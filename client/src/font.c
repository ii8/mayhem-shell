#include <string.h>
#include <assert.h>

#include "font.h"
#include "util.h"

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
	cairo_glyph_t *glyphs;
	int num;
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
	int n;

	font_ref(font);
	text->font = font;
	text->num = strlen(str);
	text->glyphs = xmalloc(text->num * sizeof(cairo_glyph_t));

	for(n = 0; *str; str++, n++) {
		index = FT_Get_Char_Index(font->ftface, *str);

		if(FT_Load_Glyph(font->ftface, index, FT_LOAD_DEFAULT)) {
			fprintf(stderr, "FT_Load_Char error\n");
			continue;
		}
		text->glyphs[n] = (cairo_glyph_t){ index, x, y };

		x += slot->advance.x >> 6;
		y += slot->advance.y >> 6;
	}

	assert(text->num == n);
	return text;
}

void text_destroy(struct text *text)
{
	font_unref(text->font);
	free(text->glyphs);
	free(text);
}

void text_extents(struct text *text, int *width, int *height)
{
	cairo_text_extents_t ext;

	cairo_scaled_font_glyph_extents(text->font->font,
					text->glyphs,
					text->num,
					&ext);
	*width = ext.width;
	*height = ext.height;
}

void text_draw(struct text *text, cairo_t *cr)
{
	cairo_set_scaled_font(cr, text->font->font);
	cairo_show_glyphs(cr, text->glyphs, text->num);
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
