#include "pptx.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <zip.h>

/* 9525 EMU per editor pixel: 960px -> 9144000 EMU (10in), 720px -> 6858000 (7.5in) */
#define EMU_PER_PX 9525.0

/* ---- dynamic string builder ---- */
typedef struct { char *buf; size_t len, cap; } Sb;

static void sb_init(Sb *s) {
    s->cap = 1024; s->len = 0;
    s->buf = malloc(s->cap);
    s->buf[0] = '\0';
}
static void sb_ensure(Sb *s, size_t add) {
    if (s->len + add + 1 > s->cap) {
        while (s->len + add + 1 > s->cap) s->cap *= 2;
        s->buf = realloc(s->buf, s->cap);
    }
}
static void sb_cat(Sb *s, const char *t) {
    size_t n = strlen(t);
    sb_ensure(s, n);
    memcpy(s->buf + s->len, t, n);
    s->len += n;
    s->buf[s->len] = '\0';
}
static void sb_catf(Sb *s, const char *fmt, ...) {
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    sb_cat(s, tmp);
}
/* XML-escape UTF-8 text into the builder */
static void sb_cat_xml(Sb *s, const char *t) {
    if (!t) return;
    for (; *t; t++) {
        switch (*t) {
            case '&': sb_cat(s, "&amp;"); break;
            case '<': sb_cat(s, "&lt;"); break;
            case '>': sb_cat(s, "&gt;"); break;
            case '"': sb_cat(s, "&quot;"); break;
            case '\'': sb_cat(s, "&apos;"); break;
            default: {
                char c[2] = { *t, 0 };
                sb_cat(s, c);
            }
        }
    }
}

/* ---- model construction ---- */
PptxPresentation* pptx_create(const char *title) {
    PptxPresentation *pres = malloc(sizeof(PptxPresentation));
    pres->title = malloc(strlen(title) + 1);
    strcpy(pres->title, title);
    pres->slides = NULL;
    pres->slide_count = 0;
    return pres;
}

void pptx_add_slide(PptxPresentation *pres, uint32_t bg_color) {
    pres->slides = realloc(pres->slides, (pres->slide_count + 1) * sizeof(PptxSlide*));
    PptxSlide *slide = malloc(sizeof(PptxSlide));
    slide->text_boxes = NULL; slide->text_count = 0;
    slide->shapes = NULL;     slide->shape_count = 0;
    slide->images = NULL;     slide->image_count = 0;
    slide->bg_color = bg_color;
    pres->slides[pres->slide_count++] = slide;
}

void pptx_add_text(PptxPresentation *pres, int slide_idx, const char *text,
                   float x, float y, float w, float h,
                   int font_size, uint32_t color,
                   int bold, int italic, int bullet, int align) {
    if (slide_idx < 0 || slide_idx >= pres->slide_count) return;
    PptxSlide *slide = pres->slides[slide_idx];
    slide->text_boxes = realloc(slide->text_boxes, (slide->text_count + 1) * sizeof(TextBox*));
    TextBox *tb = malloc(sizeof(TextBox));
    tb->text = malloc(strlen(text) + 1);
    strcpy(tb->text, text);
    tb->x = x; tb->y = y; tb->width = w; tb->height = h;
    tb->font_size = font_size; tb->color = color;
    tb->bold = bold; tb->italic = italic; tb->bullet = bullet; tb->align = align;
    slide->text_boxes[slide->text_count++] = tb;
}

void pptx_add_shape(PptxPresentation *pres, int slide_idx, PptxShapeType kind,
                    float x, float y, float w, float h,
                    uint32_t fill, uint32_t line_color, int filled) {
    if (slide_idx < 0 || slide_idx >= pres->slide_count) return;
    PptxSlide *slide = pres->slides[slide_idx];
    slide->shapes = realloc(slide->shapes, (slide->shape_count + 1) * sizeof(Shape*));
    Shape *sh = malloc(sizeof(Shape));
    sh->kind = kind; sh->x = x; sh->y = y; sh->width = w; sh->height = h;
    sh->fill = fill; sh->line_color = line_color; sh->filled = filled;
    slide->shapes[slide->shape_count++] = sh;
}

void pptx_add_image(PptxPresentation *pres, int slide_idx,
                    const unsigned char *data, size_t size, const char *ext,
                    float x, float y, float w, float h) {
    if (slide_idx < 0 || slide_idx >= pres->slide_count) return;
    if (!data || size == 0) return;
    PptxSlide *slide = pres->slides[slide_idx];
    slide->images = realloc(slide->images, (slide->image_count + 1) * sizeof(Image*));
    Image *img = malloc(sizeof(Image));
    img->data = malloc(size);
    memcpy(img->data, data, size);
    img->size = size;
    strncpy(img->ext, ext, sizeof(img->ext) - 1);
    img->ext[sizeof(img->ext) - 1] = '\0';
    img->x = x; img->y = y; img->width = w; img->height = h;
    slide->images[slide->image_count++] = img;
}

/* ---- zip helpers (copy content so the buffer can be freed immediately) ---- */
static void add_str(zip_t *z, const char *name, const char *content) {
    size_t n = strlen(content);
    char *copy = malloc(n);
    if (n) memcpy(copy, content, n);
    zip_source_t *src = zip_source_buffer(z, copy, n, 1); /* freep=1 -> zip frees copy */
    if (!src) { free(copy); return; }
    if (zip_file_add(z, name, src, ZIP_FL_OVERWRITE) < 0) zip_source_free(src);
}
static void add_bin(zip_t *z, const char *name, const unsigned char *data, size_t size) {
    unsigned char *copy = malloc(size ? size : 1);
    if (size) memcpy(copy, data, size);
    zip_source_t *src = zip_source_buffer(z, copy, size, 1);
    if (!src) { free(copy); return; }
    if (zip_file_add(z, name, src, ZIP_FL_OVERWRITE) < 0) zip_source_free(src);
}

/* ---- static package parts ---- */
static const char *THEME1_XML =
"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
"<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" name=\"Office\">"
"<a:themeElements>"
"<a:clrScheme name=\"Office\">"
"<a:dk1><a:sysClr val=\"windowText\" lastClr=\"000000\"/></a:dk1>"
"<a:lt1><a:sysClr val=\"window\" lastClr=\"FFFFFF\"/></a:lt1>"
"<a:dk2><a:srgbClr val=\"44546A\"/></a:dk2>"
"<a:lt2><a:srgbClr val=\"E7E6E6\"/></a:lt2>"
"<a:accent1><a:srgbClr val=\"4472C4\"/></a:accent1>"
"<a:accent2><a:srgbClr val=\"ED7D31\"/></a:accent2>"
"<a:accent3><a:srgbClr val=\"A5A5A5\"/></a:accent3>"
"<a:accent4><a:srgbClr val=\"FFC000\"/></a:accent4>"
"<a:accent5><a:srgbClr val=\"5B9BD5\"/></a:accent5>"
"<a:accent6><a:srgbClr val=\"70AD47\"/></a:accent6>"
"<a:hlink><a:srgbClr val=\"0563C1\"/></a:hlink>"
"<a:folHlink><a:srgbClr val=\"954F72\"/></a:folHlink>"
"</a:clrScheme>"
"<a:fontScheme name=\"Office\">"
"<a:majorFont><a:latin typeface=\"Calibri Light\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:majorFont>"
"<a:minorFont><a:latin typeface=\"Calibri\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:minorFont>"
"</a:fontScheme>"
"<a:fmtScheme name=\"Office\">"
"<a:fillStyleLst>"
"<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
"<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
"<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
"</a:fillStyleLst>"
"<a:lnStyleLst>"
"<a:ln w=\"6350\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln>"
"<a:ln w=\"12700\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln>"
"<a:ln w=\"19050\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln>"
"</a:lnStyleLst>"
"<a:effectStyleLst>"
"<a:effectStyle><a:effectLst/></a:effectStyle>"
"<a:effectStyle><a:effectLst/></a:effectStyle>"
"<a:effectStyle><a:effectLst/></a:effectStyle>"
"</a:effectStyleLst>"
"<a:bgFillStyleLst>"
"<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
"<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
"<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
"</a:bgFillStyleLst>"
"</a:fmtScheme>"
"</a:themeElements>"
"</a:theme>";

static const char *SLIDE_MASTER_XML =
"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
"<p:sldMaster xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
"xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
"xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\">"
"<p:cSld><p:bg><p:bgRef idx=\"1001\"><a:schemeClr val=\"bg1\"/></p:bgRef></p:bg>"
"<p:spTree>"
"<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
"<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/>"
"<a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>"
"</p:spTree></p:cSld>"
"<p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\" accent1=\"accent1\" "
"accent2=\"accent2\" accent3=\"accent3\" accent4=\"accent4\" accent5=\"accent5\" "
"accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/>"
"<p:sldLayoutIdLst><p:sldLayoutId id=\"2147483649\" r:id=\"rId1\"/></p:sldLayoutIdLst>"
"</p:sldMaster>";

static const char *SLIDE_LAYOUT_XML =
"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
"<p:sldLayout xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
"xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
"xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\" type=\"blank\" preserve=\"1\">"
"<p:cSld name=\"Blank\">"
"<p:spTree>"
"<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
"<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/>"
"<a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>"
"</p:spTree></p:cSld>"
"<p:clrMapOvr><a:overrideClrMapping bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\" "
"accent1=\"accent1\" accent2=\"accent2\" accent3=\"accent3\" accent4=\"accent4\" "
"accent5=\"accent5\" accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/></p:clrMapOvr>"
"</p:sldLayout>";

static const char *SLIDE_MASTER_RELS =
"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
"<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
"<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout\" Target=\"../slideLayouts/slideLayout1.xml\"/>"
"<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme\" Target=\"../theme/theme1.xml\"/>"
"</Relationships>";

static const char *SLIDE_LAYOUT_RELS =
"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
"<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
"<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster\" Target=\"../slideMasters/slideMaster1.xml\"/>"
"</Relationships>";

static const char *PKG_RELS =
"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
"<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
"<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"ppt/presentation.xml\"/>"
"<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/>"
"</Relationships>";

/* ---- dynamic parts ---- */
static void write_content_types(zip_t *z, int slide_count) {
    Sb s; sb_init(&s);
    sb_cat(&s,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Default Extension=\"png\" ContentType=\"image/png\"/>"
        "<Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/>"
        "<Default Extension=\"jpg\" ContentType=\"image/jpeg\"/>"
        "<Override PartName=\"/ppt/presentation.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml\"/>"
        "<Override PartName=\"/ppt/slideMasters/slideMaster1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml\"/>"
        "<Override PartName=\"/ppt/slideLayouts/slideLayout1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml\"/>"
        "<Override PartName=\"/ppt/theme/theme1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.theme+xml\"/>"
        "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>");
    for (int i = 1; i <= slide_count; i++)
        sb_catf(&s, "<Override PartName=\"/ppt/slides/slide%d.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slide+xml\"/>", i);
    sb_cat(&s, "</Types>");
    add_str(z, "[Content_Types].xml", s.buf);
    free(s.buf);
}

static void write_presentation(zip_t *z, int slide_count) {
    Sb s; sb_init(&s);
    sb_cat(&s,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<p:presentation xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
        "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\">");
    /* master rel id = slide_count + 1 */
    sb_catf(&s, "<p:sldMasterIdLst><p:sldMasterId id=\"2147483648\" r:id=\"rId%d\"/></p:sldMasterIdLst>", slide_count + 1);
    sb_cat(&s, "<p:sldIdLst>");
    for (int i = 0; i < slide_count; i++)
        sb_catf(&s, "<p:sldId id=\"%d\" r:id=\"rId%d\"/>", 256 + i, i + 1);
    sb_cat(&s, "</p:sldIdLst>");
    sb_cat(&s, "<p:sldSz cx=\"9144000\" cy=\"6858000\" type=\"screen4x3\"/>");
    sb_cat(&s, "<p:notesSz cx=\"6858000\" cy=\"9144000\"/>");
    sb_cat(&s, "</p:presentation>");
    add_str(z, "ppt/presentation.xml", s.buf);
    free(s.buf);
}

static void write_presentation_rels(zip_t *z, int slide_count) {
    Sb s; sb_init(&s);
    sb_cat(&s,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">");
    for (int i = 0; i < slide_count; i++)
        sb_catf(&s, "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide\" Target=\"slides/slide%d.xml\"/>", i + 1, i + 1);
    sb_catf(&s, "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster\" Target=\"slideMasters/slideMaster1.xml\"/>", slide_count + 1);
    sb_cat(&s, "</Relationships>");
    add_str(z, "ppt/_rels/presentation.xml.rels", s.buf);
    free(s.buf);
}

static void emit_solid_fill(Sb *s, uint32_t color) {
    sb_catf(s, "<a:solidFill><a:srgbClr val=\"%06X\"/></a:solidFill>", color & 0xFFFFFF);
}

static void write_slide(zip_t *z, PptxSlide *slide, int slide_num) {
    Sb s; sb_init(&s);
    sb_catf(&s,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<p:sld xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
        "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\">"
        "<p:cSld><p:bg><p:bgPr><a:solidFill><a:srgbClr val=\"%06X\"/></a:solidFill>"
        "<a:effectLst/></p:bgPr></p:bg>"
        "<p:spTree>"
        "<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
        "<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/>"
        "<a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>",
        slide->bg_color & 0xFFFFFF);

    int id = 2; /* shape ids start at 2 */

    /* shapes */
    for (int i = 0; i < slide->shape_count; i++) {
        Shape *sh = slide->shapes[i];
        const char *geom = (sh->kind == SHAPE_ELLIPSE) ? "ellipse"
                         : (sh->kind == SHAPE_LINE)    ? "line"
                         : "rect";
        long ex = (long)(sh->x * EMU_PER_PX);
        long ey = (long)(sh->y * EMU_PER_PX);
        long ecx = (long)(sh->width * EMU_PER_PX);
        long ecy = (long)(sh->height * EMU_PER_PX);
        if (ecx < 1) ecx = 1;
        if (ecy < 1) ecy = 1;
        sb_catf(&s,
            "<p:sp><p:nvSpPr><p:cNvPr id=\"%d\" name=\"Shape %d\"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>"
            "<p:spPr><a:xfrm><a:off x=\"%ld\" y=\"%ld\"/><a:ext cx=\"%ld\" cy=\"%ld\"/></a:xfrm>"
            "<a:prstGeom prst=\"%s\"><a:avLst/></a:prstGeom>",
            id, id, ex, ey, ecx, ecy, geom);
        if (sh->kind == SHAPE_LINE || !sh->filled) {
            sb_cat(&s, "<a:noFill/>");
        } else {
            emit_solid_fill(&s, sh->fill);
        }
        sb_catf(&s, "<a:ln w=\"19050\"><a:solidFill><a:srgbClr val=\"%06X\"/></a:solidFill></a:ln>",
                sh->line_color & 0xFFFFFF);
        sb_cat(&s, "</p:spPr><p:txBody><a:bodyPr/><a:lstStyle/><a:p/></p:txBody></p:sp>");
        id++;
    }

    /* text boxes */
    for (int i = 0; i < slide->text_count; i++) {
        TextBox *tb = slide->text_boxes[i];
        long ex = (long)(tb->x * EMU_PER_PX);
        long ey = (long)(tb->y * EMU_PER_PX);
        long ecx = (long)(tb->width * EMU_PER_PX);
        long ecy = (long)(tb->height * EMU_PER_PX);
        if (ecx < 1) ecx = 914400;
        if (ecy < 1) ecy = 457200;
        const char *algn = (tb->align == 1) ? "ctr" : (tb->align == 2) ? "r" : "l";
        sb_catf(&s,
            "<p:sp><p:nvSpPr><p:cNvPr id=\"%d\" name=\"Text %d\"/>"
            "<p:cNvSpPr txBox=\"1\"/><p:nvPr/></p:nvSpPr>"
            "<p:spPr><a:xfrm><a:off x=\"%ld\" y=\"%ld\"/><a:ext cx=\"%ld\" cy=\"%ld\"/></a:xfrm>"
            "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom><a:noFill/></p:spPr>"
            "<p:txBody><a:bodyPr wrap=\"square\"><a:normAutofit/></a:bodyPr><a:lstStyle/>",
            id, id, ex, ey, ecx, ecy);

        /* split text into paragraphs on '\n' */
        const char *p = tb->text;
        while (1) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            sb_catf(&s, "<a:p><a:pPr algn=\"%s\">", algn);
            if (tb->bullet)
                sb_cat(&s, "<a:buFont typeface=\"Arial\"/><a:buChar char=\"\xE2\x80\xA2\"/>");
            else
                sb_cat(&s, "<a:buNone/>");
            sb_cat(&s, "</a:pPr>");
            sb_catf(&s, "<a:r><a:rPr lang=\"en-US\" sz=\"%d\" b=\"%d\" i=\"%d\" dirty=\"0\">",
                    tb->font_size * 100, tb->bold ? 1 : 0, tb->italic ? 1 : 0);
            emit_solid_fill(&s, tb->color);
            sb_cat(&s, "</a:rPr><a:t>");
            /* escape just this line */
            char *line = malloc(len + 1);
            memcpy(line, p, len);
            line[len] = '\0';
            sb_cat_xml(&s, line);
            free(line);
            sb_cat(&s, "</a:t></a:r></a:p>");
            if (!nl) break;
            p = nl + 1;
        }
        sb_cat(&s, "</p:txBody></p:sp>");
        id++;
    }

    /* images (r:embed rId = 2 + image index; layout is rId1) */
    for (int i = 0; i < slide->image_count; i++) {
        Image *img = slide->images[i];
        long ex = (long)(img->x * EMU_PER_PX);
        long ey = (long)(img->y * EMU_PER_PX);
        long ecx = (long)(img->width * EMU_PER_PX);
        long ecy = (long)(img->height * EMU_PER_PX);
        if (ecx < 1) ecx = 914400;
        if (ecy < 1) ecy = 914400;
        sb_catf(&s,
            "<p:pic><p:nvPicPr><p:cNvPr id=\"%d\" name=\"Picture %d\"/>"
            "<p:cNvPicPr><a:picLocks noChangeAspect=\"1\"/></p:cNvPicPr><p:nvPr/></p:nvPicPr>"
            "<p:blipFill><a:blip r:embed=\"rId%d\"/><a:stretch><a:fillRect/></a:stretch></p:blipFill>"
            "<p:spPr><a:xfrm><a:off x=\"%ld\" y=\"%ld\"/><a:ext cx=\"%ld\" cy=\"%ld\"/></a:xfrm>"
            "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>",
            id, id, i + 2, ex, ey, ecx, ecy);
        id++;
    }

    sb_cat(&s, "</p:spTree></p:cSld><p:clrMapOvr><a:overrideClrMapping bg1=\"lt1\" tx1=\"dk1\" "
               "bg2=\"lt2\" tx2=\"dk2\" accent1=\"accent1\" accent2=\"accent2\" accent3=\"accent3\" "
               "accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\" hlink=\"hlink\" "
               "folHlink=\"folHlink\"/></p:clrMapOvr></p:sld>");

    char filename[256];
    snprintf(filename, sizeof(filename), "ppt/slides/slide%d.xml", slide_num);
    add_str(z, filename, s.buf);
    free(s.buf);

    /* per-slide rels + media */
    Sb r; sb_init(&r);
    sb_cat(&r,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout\" Target=\"../slideLayouts/slideLayout1.xml\"/>");
    for (int i = 0; i < slide->image_count; i++) {
        Image *img = slide->images[i];
        char media[256];
        snprintf(media, sizeof(media), "ppt/media/image_s%d_%d.%s", slide_num, i, img->ext);
        add_bin(z, media, img->data, img->size);
        sb_catf(&r, "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../media/image_s%d_%d.%s\"/>",
                i + 2, slide_num, i, img->ext);
    }
    sb_cat(&r, "</Relationships>");
    char relname[256];
    snprintf(relname, sizeof(relname), "ppt/slides/_rels/slide%d.xml.rels", slide_num);
    add_str(z, relname, r.buf);
    free(r.buf);
}

static void write_core(zip_t *z, const char *title) {
    Sb s; sb_init(&s);
    sb_cat(&s,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>");
    sb_cat_xml(&s, title);
    sb_cat(&s, "</dc:title></cp:coreProperties>");
    add_str(z, "docProps/core.xml", s.buf);
    free(s.buf);
}

int pptx_save(PptxPresentation *pres, const char *filename) {
    int err = 0;
    zip_t *z = zip_open(filename, ZIP_CREATE | ZIP_TRUNCATE, &err);
    if (!z) return 0;

    int n = pres->slide_count;
    if (n < 1) n = 1; /* presentation must reference at least one slide relationship set */

    add_str(z, "_rels/.rels", PKG_RELS);
    write_content_types(z, pres->slide_count);
    write_core(z, pres->title);
    write_presentation(z, pres->slide_count);
    write_presentation_rels(z, pres->slide_count);
    add_str(z, "ppt/theme/theme1.xml", THEME1_XML);
    add_str(z, "ppt/slideMasters/slideMaster1.xml", SLIDE_MASTER_XML);
    add_str(z, "ppt/slideMasters/_rels/slideMaster1.xml.rels", SLIDE_MASTER_RELS);
    add_str(z, "ppt/slideLayouts/slideLayout1.xml", SLIDE_LAYOUT_XML);
    add_str(z, "ppt/slideLayouts/_rels/slideLayout1.xml.rels", SLIDE_LAYOUT_RELS);

    for (int i = 0; i < pres->slide_count; i++)
        write_slide(z, pres->slides[i], i + 1);

    if (zip_close(z) < 0) {
        zip_discard(z);
        return 0;
    }
    return 1;
}

void pptx_free(PptxPresentation *pres) {
    if (!pres) return;
    for (int i = 0; i < pres->slide_count; i++) {
        PptxSlide *sl = pres->slides[i];
        for (int j = 0; j < sl->text_count; j++) {
            free(sl->text_boxes[j]->text);
            free(sl->text_boxes[j]);
        }
        free(sl->text_boxes);
        for (int j = 0; j < sl->shape_count; j++)
            free(sl->shapes[j]);
        free(sl->shapes);
        for (int j = 0; j < sl->image_count; j++) {
            free(sl->images[j]->data);
            free(sl->images[j]);
        }
        free(sl->images);
        free(sl);
    }
    free(pres->slides);
    free(pres->title);
    free(pres);
}
