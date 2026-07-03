#ifndef PPTX_H
#define PPTX_H

#include <stdint.h>
#include <stddef.h>

/* Coordinates are given in editor pixels (slide is 960x720 px = 10in x 7.5in). */

typedef enum {
    SHAPE_RECT = 0,
    SHAPE_ELLIPSE = 1,
    SHAPE_LINE = 2
} PptxShapeType;

typedef struct {
    char *text;                 /* plain UTF-8, lines separated by '\n' */
    float x, y, width, height;
    int font_size;              /* points */
    uint32_t color;             /* 0xRRGGBB */
    int bold, italic, bullet;
    int align;                  /* 0 left, 1 center, 2 right */
} TextBox;

typedef struct {
    PptxShapeType kind;
    float x, y, width, height;
    uint32_t fill;              /* 0xRRGGBB */
    uint32_t line_color;        /* 0xRRGGBB */
    int filled;                 /* 0 = no fill (line) */
} Shape;

typedef struct {
    unsigned char *data;        /* owned raw image bytes */
    size_t size;
    char ext[8];                /* "png" or "jpeg" */
    float x, y, width, height;
} Image;

typedef struct {
    TextBox **text_boxes;
    int text_count;
    Shape **shapes;
    int shape_count;
    Image **images;
    int image_count;
    uint32_t bg_color;          /* 0xRRGGBB */
} PptxSlide;

typedef struct {
    PptxSlide **slides;
    int slide_count;
    char *title;
} PptxPresentation;

PptxPresentation* pptx_create(const char *title);
void pptx_add_slide(PptxPresentation *pres, uint32_t bg_color);
void pptx_add_text(PptxPresentation *pres, int slide_idx, const char *text,
                   float x, float y, float w, float h,
                   int font_size, uint32_t color,
                   int bold, int italic, int bullet, int align);
void pptx_add_shape(PptxPresentation *pres, int slide_idx, PptxShapeType kind,
                    float x, float y, float w, float h,
                    uint32_t fill, uint32_t line_color, int filled);
/* Takes ownership of a copy of data (data itself is not freed by pptx). */
void pptx_add_image(PptxPresentation *pres, int slide_idx,
                    const unsigned char *data, size_t size, const char *ext,
                    float x, float y, float w, float h);
int pptx_save(PptxPresentation *pres, const char *filename);
void pptx_free(PptxPresentation *pres);

#endif
