#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <glib.h>

#include "nbis/include/lfs.h"
#include "nbis/include/bozorth.h"

static unsigned char *read_pgm(const char *filename, int *w, int *h)
{
    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open %s\n", filename);
        return NULL;
    }
    char magic[16];
    if (fscanf(f, "%15s", magic) != 1 || strcmp(magic, "P5") != 0) {
        fprintf(stderr, "Invalid PGM header in %s\n", filename);
        fclose(f);
        return NULL;
    }
    int maxval;
    if (fscanf(f, "%d %d %d", w, h, &maxval) != 3) {
        fprintf(stderr, "Invalid PGM dims in %s\n", filename);
        fclose(f);
        return NULL;
    }
    fgetc(f); // skip trailing whitespace
    unsigned char *buf = malloc((*w) * (*h));
    if (fread(buf, 1, (*w) * (*h), f) != (size_t)((*w) * (*h))) {
        fprintf(stderr, "Short read in %s\n", filename);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    return buf;
}

static int is_sensor_artifact(int x, int y)
{
    static const int artifacts[][2] = {
        {41, 114}, {42, 115},
        {53, 78},  {52, 78},
        {56, 90},
        {98, 74},
        {104, 70},
        {48, 132},
        {68, 140}
    };
    for (size_t i = 0; i < sizeof(artifacts)/sizeof(artifacts[0]); i++) {
        int dx = x - artifacts[i][0];
        int dy = y - artifacts[i][1];
        if (dx*dx + dy*dy <= 9) // within 3 pixels
            return 1;
    }
    return 0;
}

static int g_margin = 0;

static struct xyt_struct *extract_xyt(const char *filename, int do_filter, int do_invert)
{
    int w, h;
    unsigned char *img = read_pgm(filename, &w, &h);
    if (!img) return NULL;

    if (do_invert) {
        for (int i = 0; i < w * h; i++) {
            img[i] = 0xff - img[i];
        }
    }

    MINUTIAE *minutiae = NULL;
    int *quality_map = NULL, *direction_map = NULL;
    int *low_contrast_map = NULL, *low_flow_map = NULL, *high_curve_map = NULL;
    int map_w, map_h;
    unsigned char *binarized = NULL;
    int bw, bh, bd;

    LFSPARMS lfsparms = g_lfsparms_V2;
    lfsparms.remove_perimeter_pts = 0;

    int r = get_minutiae (&minutiae, &quality_map, &direction_map,
                          &low_contrast_map, &low_flow_map, &high_curve_map,
                          &map_w, &map_h, &binarized, &bw, &bh, &bd,
                          img, w, h, 8, 0.0, &lfsparms);

    free(img);
    if (direction_map) g_free(direction_map);
    if (low_contrast_map) g_free(low_contrast_map);
    if (low_flow_map) g_free(low_flow_map);
    if (high_curve_map) g_free(high_curve_map);
    if (quality_map) g_free(quality_map);
    if (binarized) g_free(binarized);

    if (r || !minutiae || minutiae->num == 0) {
        if (minutiae) free_minutiae(minutiae);
        fprintf(stderr, "[%s] No minutiae found (code: %d)\n", filename, r);
        return NULL;
    }

    struct xyt_struct *xyt = g_new0(struct xyt_struct, 1);
    struct minutiae_struct c[MAX_FILE_MINUTIAE];
    int nmin = minutiae->num < MAX_BOZORTH_MINUTIAE ? minutiae->num : MAX_BOZORTH_MINUTIAE;

    int valid_count = 0;
    for (int i = 0; i < nmin; i++) {
        MINUTIA *m = minutiae->list[i];
        int x, y, theta;
        lfs2nist_minutia_XYT(&x, &y, &theta, m, bw, bh);
        if (theta > 180) theta -= 360;

        int rel = (int) round(m->reliability * 100.0);
        if (do_filter && rel <= 5)
            continue;
        if (is_sensor_artifact(x, y))
            continue;
        if (g_margin > 0) {
            if (x < g_margin || x >= bw - g_margin || y < g_margin || y >= bh - g_margin)
                continue;
        }

        c[valid_count].col[0] = x;
        c[valid_count].col[1] = y;
        c[valid_count].col[2] = theta;
        c[valid_count].col[3] = rel;
        valid_count++;
    }
    qsort(c, valid_count, sizeof(struct minutiae_struct), sort_x_y);

    for (int i = 0; i < valid_count; i++) {
        xyt->xcol[i] = c[i].col[0];
        xyt->ycol[i] = c[i].col[1];
        xyt->thetacol[i] = c[i].col[2];
    }
    xyt->nrows = valid_count;
    free_minutiae(minutiae);
    return xyt;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: %s <probe.pgm> <gallery.pgm> [--filter] [--margin N]\n", argv[0]);
        return 1;
    }

    int do_invert = 0;
    int do_filter = 0;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--invert") == 0) do_invert = 1;
        if (strcmp(argv[i], "--filter") == 0) do_filter = 1;
        if (strcmp(argv[i], "--margin") == 0 && i + 1 < argc) g_margin = atoi(argv[++i]);
    }
    struct xyt_struct *probe = extract_xyt(argv[1], do_filter, do_invert);
    struct xyt_struct *gallery = extract_xyt(argv[2], do_filter, do_invert);

    if (!probe || !gallery) {
        printf("Failed to extract minutiae from one or both images\n");
        return 1;
    }

    int probe_len = bozorth_probe_init(probe);
    int score = bozorth_to_gallery(probe_len, probe, gallery);

    printf("Probe: %s (%d minutiae)\n", argv[1], probe->nrows);
    for (int i = 0; i < probe->nrows; i++) {
        printf("  P[%2d]: x=%3d y=%3d theta=%4d\n", i, probe->xcol[i], probe->ycol[i], probe->thetacol[i]);
    }
    printf("Gallery: %s (%d minutiae)\n", argv[2], gallery->nrows);
    for (int i = 0; i < gallery->nrows; i++) {
        printf("  G[%2d]: x=%3d y=%3d theta=%4d\n", i, gallery->xcol[i], gallery->ycol[i], gallery->thetacol[i]);
    }
    printf("Bozorth Score: %d\n", score);

    g_free(probe);
    g_free(gallery);
    return 0;
}
