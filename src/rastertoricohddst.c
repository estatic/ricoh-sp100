/*
 * rastertoricohddst - CUPS raster filter for Ricoh DDST GDI printers
 * (Aficio SP 100 / SP 111 / SP 112 / SP 204 families).
 *
 * Reads CUPS raster (1 bit per pixel, black = 1) and writes the PJL + JBIG
 * stream these printers expect. Everything happens in memory: no temporary
 * files, no external tools, and pages are sent as soon as they are encoded.
 *
 * Usage (as called by CUPS):
 *   rastertoricohddst job-id user title copies options [file]
 *
 * Based on the protocol reverse-engineered in pstoricohddst-gdi by
 * Serge V Shistarev (madlynx) and contributors.
 *
 * License: GPL-2.0-or-later
 */

#include <cups/cups.h>
#include <cups/raster.h>
#include <jbig.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define EOL "\r\n"

/* Divisor for the DOTCOUNT value reported to the printer (toner estimate).
 * Matches the behaviour of the last maintained shell filter, which divided
 * the black-pixel count by 100. */
#define DOTCOUNT_DIVISOR 100

static volatile sig_atomic_t canceled = 0;

static void on_term(int sig)
{
    (void)sig;
    canceled = 1;
}

/* Growable buffer that receives the JBIG bitstream. */
struct buf {
    unsigned char *data;
    size_t len, cap;
};

static void buf_append(unsigned char *start, size_t len, void *ctx)
{
    struct buf *b = ctx;

    if (b->len + len > b->cap) {
        size_t cap = b->cap ? b->cap : 256 * 1024;
        while (cap < b->len + len)
            cap *= 2;
        unsigned char *p = realloc(b->data, cap);
        if (!p) {
            fputs("ERROR: Out of memory while compressing page\n", stderr);
            exit(1);
        }
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, start, len);
    b->len += len;
}

/* Paper names understood by the printer, matched by size in points. */
static const struct {
    const char *name;
    int w, h;
} paper_sizes[] = {
    { "A4", 595, 842 },
    { "A5", 420, 595 },
    { "A6", 297, 420 },
    { "LETTER", 612, 792 },
    { "LEGAL", 612, 1008 },
    { "B5", 516, 729 },
    { "B6", 363, 516 },
    { "MONARCH", 279, 540 },
};

static void paper_name(const cups_page_header2_t *h, const char *fallback,
                       char *out, size_t outlen)
{
    const size_t n = sizeof(paper_sizes) / sizeof(paper_sizes[0]);
    const char *src = h->cupsPageSizeName[0] ? h->cupsPageSizeName : fallback;

    if (src && !strncasecmp(src, "Env", 3))
        src += 3;  /* EnvMonarch -> MONARCH */
    for (size_t i = 0; src && i < n; i++) {
        if (!strcasecmp(src, paper_sizes[i].name)) {
            snprintf(out, outlen, "%s", paper_sizes[i].name);
            return;
        }
    }

    for (size_t i = 0; i < n; i++) {
        if (abs((int)h->PageSize[0] - paper_sizes[i].w) <= 2 &&
            abs((int)h->PageSize[1] - paper_sizes[i].h) <= 2) {
            snprintf(out, outlen, "%s", paper_sizes[i].name);
            return;
        }
    }

    /* Unknown size: send the name upper-cased, like the old filter. */
    size_t i;
    for (i = 0; src && src[i] && i + 1 < outlen; i++)
        out[i] = (src[i] >= 'a' && src[i] <= 'z') ? src[i] - 32 : src[i];
    out[i] = '\0';
    if (!out[0])
        snprintf(out, outlen, "A4");
}

/* Keep PJL string values printable ASCII so they cannot break the stream. */
static void pjl_clean(const char *in, char *out, size_t outlen)
{
    size_t j = 0;

    for (; in && *in && j + 1 < outlen; in++) {
        unsigned char c = (unsigned char)*in;
        out[j++] = (c >= 0x20 && c < 0x7f && c != '"') ? (char)c : '_';
    }
    out[j] = '\0';
    if (!out[0])
        snprintf(out, outlen, "Document");
}

static size_t count_black(const unsigned char *p, size_t n)
{
    size_t dots = 0;

    for (size_t i = 0; i < n; i++)
        dots += (size_t)__builtin_popcount(p[i]);
    return dots;
}

static int write_all(const void *data, size_t len)
{
    return fwrite(data, 1, len, stdout) == len ? 0 : -1;
}

int main(int argc, char *argv[])
{
    if (argc < 6 || argc > 7) {
        fprintf(stderr, "Usage: %s job-id user title copies options [file]\n",
                argv[0]);
        return 1;
    }

    int fd = 0;
    if (argc == 7 && (fd = open(argv[6], O_RDONLY)) < 0) {
        fprintf(stderr, "ERROR: Unable to open raster file \"%s\": %s\n",
                argv[6], strerror(errno));
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    cups_option_t *options = NULL;
    int num_options = cupsParseOptions(argv[5], 0, &options);
    const char *opt_size = cupsGetOption("PageSize", num_options, options);
    const char *opt_slot = cupsGetOption("InputSlot", num_options, options);
    const char *opt_type = cupsGetOption("MediaType", num_options, options);

    char user[64], title[64], slot[32], mtype[32];
    pjl_clean(argv[2], user, sizeof(user));
    pjl_clean(argv[3], title, sizeof(title));
    pjl_clean(opt_slot && strcasecmp(opt_slot, "Auto") ? opt_slot : "TRAY1",
              slot, sizeof(slot));
    pjl_clean(opt_type && strcasecmp(opt_type, "Auto") ? opt_type
                                                       : "PLAINRECYCLE",
              mtype, sizeof(mtype));

    char stamp[32];
    time_t now = time(NULL);
    strftime(stamp, sizeof(stamp), "%Y/%m/%d %H:%M:%S", localtime(&now));

    cups_raster_t *ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    cups_page_header2_t h;
    unsigned char *bitmap = NULL, *row = NULL;
    size_t bitmap_cap = 0, row_cap = 0;
    struct buf jbig = { 0 };
    int page = 0, status = 0;

    while (!canceled && cupsRasterReadHeader2(ras, &h)) {
        if (h.cupsBitsPerPixel != 1) {
            fprintf(stderr, "ERROR: Unsupported raster format (%u bits per "
                    "pixel); the PPD must request 1-bit black output\n",
                    h.cupsBitsPerPixel);
            status = 1;
            break;
        }
        if (h.cupsWidth == 0 || h.cupsHeight == 0)
            continue;

        page++;
        fprintf(stderr, "INFO: Processing page %d\n", page);

        /* gstoraster crops the raster to the PPD's imageable area, but the
         * printer expects a bitmap of the whole sheet (as Ghostscript's
         * pbmraw device produced for the old filter). Place the raster on a
         * full-page canvas at the position of the imageable area. */
        unsigned xres = h.HWResolution[0] ? h.HWResolution[0] : 600;
        unsigned yres = h.HWResolution[1] ? h.HWResolution[1] : 600;
        unsigned page_w = (unsigned)(h.PageSize[0] * xres / 72.0 + 0.5);
        unsigned page_h = (unsigned)(h.PageSize[1] * yres / 72.0 + 0.5);
        unsigned ox = (unsigned)(h.ImagingBoundingBox[0] * xres / 72.0 + 0.5);
        unsigned oy = h.PageSize[1] > h.ImagingBoundingBox[3] && h.ImagingBoundingBox[3]
                          ? (unsigned)((h.PageSize[1] - h.ImagingBoundingBox[3]) * yres / 72.0 + 0.5)
                          : 0;
        if (page_w < h.cupsWidth || page_h < h.cupsHeight) {
            page_w = h.cupsWidth;
            page_h = h.cupsHeight;
        }
        if (ox + h.cupsWidth > page_w)
            ox = page_w - h.cupsWidth;
        if (oy + h.cupsHeight > page_h)
            oy = page_h - h.cupsHeight;

        size_t bpl = (page_w + 7) / 8;
        size_t size = bpl * page_h;
        if (size > bitmap_cap) {
            free(bitmap);
            if (!(bitmap = malloc(size))) {
                fputs("ERROR: Out of memory for page bitmap\n", stderr);
                status = 1;
                break;
            }
            bitmap_cap = size;
        }
        if (h.cupsBytesPerLine > row_cap) {
            free(row);
            if (!(row = malloc(h.cupsBytesPerLine))) {
                fputs("ERROR: Out of memory for raster line\n", stderr);
                status = 1;
                break;
            }
            row_cap = h.cupsBytesPerLine;
        }
        memset(bitmap, 0, size);

        int invert = h.cupsColorSpace == CUPS_CSPACE_W ||
                     h.cupsColorSpace == CUPS_CSPACE_SW;
        size_t src_bytes = (h.cupsWidth + 7) / 8;
        unsigned char tail_mask = h.cupsWidth % 8 ? (unsigned char)(0xff << (8 - h.cupsWidth % 8))
                                                  : 0xff;
        unsigned shift = ox % 8;

        for (unsigned y = 0; y < h.cupsHeight && !canceled; y++) {
            /* A truncated or canceled page is still finished (blank from
             * here on) so the printer is not left in the middle of a job. */
            if (cupsRasterReadPixels(ras, row, h.cupsBytesPerLine) == 0)
                break;
            if (invert)
                for (size_t i = 0; i < src_bytes; i++)
                    row[i] = (unsigned char)~row[i];
            row[src_bytes - 1] &= tail_mask;

            unsigned char *dst = bitmap + (size_t)(oy + y) * bpl + ox / 8;
            if (shift == 0) {
                memcpy(dst, row, src_bytes);
            } else {
                for (size_t i = 0; i < src_bytes; i++) {
                    dst[i] |= (unsigned char)(row[i] >> shift);
                    if (ox / 8 + i + 1 < bpl)
                        dst[i + 1] |= (unsigned char)(row[i] << (8 - shift));
                }
            }
        }

        size_t dots = count_black(bitmap, size) / DOTCOUNT_DIVISOR;

        /* Same parameters as "pbmtojbg -p 72 -o 3 -m 0 -q", which the
         * printer is known to accept. */
        struct jbg_enc_state s;
        unsigned char *planes[1] = { bitmap };
        jbig.len = 0;
        jbg_enc_init(&s, page_w, page_h, 1, planes, buf_append, &jbig);
        jbg_enc_layers(&s, 0);
        jbg_enc_options(&s, JBG_ILEAVE | JBG_SMID, JBG_LRLTWO | JBG_TPBON, 0, 0, -1);
        jbg_enc_out(&s);
        jbg_enc_free(&s);

        if (page == 1)
            printf("\033%%-12345X@PJL" EOL
                   "@PJL SET TIMESTAMP=%s" EOL
                   "@PJL SET FILENAME=%s" EOL
                   "@PJL SET COMPRESS=JBIG" EOL
                   "@PJL SET USERNAME=%s" EOL
                   "@PJL SET COVER=OFF" EOL
                   "@PJL SET HOLD=OFF" EOL,
                   stamp, title, user);

        char paper[64];
        paper_name(&h, opt_size, paper, sizeof(paper));

        printf("@PJL SET PAGESTATUS=START" EOL
               "@PJL SET COPIES=1" EOL
               "@PJL SET MEDIASOURCE=%s" EOL
               "@PJL SET MEDIATYPE=%s" EOL
               "@PJL SET PAPER=%s" EOL
               "@PJL SET PAPERWIDTH=%u" EOL
               "@PJL SET PAPERLENGTH=%u" EOL
               "@PJL SET RESOLUTION=%u" EOL
               "@PJL SET IMAGELEN=%zu" EOL,
               slot, mtype, paper, page_w, page_h, xres, jbig.len);
        if (write_all(jbig.data, jbig.len) < 0) {
            fputs("ERROR: Unable to write print data\n", stderr);
            status = 1;
            break;
        }
        printf("@PJL SET DOTCOUNT=%zu" EOL
               "@PJL SET PAGESTATUS=END" EOL,
               dots);
        fflush(stdout);

        fprintf(stderr, "PAGE: %d 1\n", page);
    }

    if (page > 0) {
        printf("@PJL EOJ" EOL "\033%%-12345X\n");
        fflush(stdout);
    }

    if (canceled)
        fputs("INFO: Job canceled\n", stderr);
    else if (status == 0)
        fprintf(stderr, "INFO: Ready to print (%d page%s)\n", page,
                page == 1 ? "" : "s");

    cupsRasterClose(ras);
    cupsFreeOptions(num_options, options);
    free(bitmap);
    free(row);
    free(jbig.data);
    if (fd)
        close(fd);
    return status;
}
