/*  mtn - movie thumbnailer http://moviethumbnail.sourceforge.net/

    Copyright (C) 2007-2008 tuit <tuitfun@yahoo.co.th>, et al.

    based on "Using libavformat and libavcodec" by Martin Böhme:
        http://www.inb.uni-luebeck.de/~boehme/using_libavcodec.html
        http://www.inb.uni-luebeck.de/~boehme/libavcodec_update.html
    and "An ffmpeg and SDL Tutorial":
        http://www.dranger.com/ffmpeg/
    and "Using GD with FFMPeg":
        http://cvs.php.net/viewvc.cgi/gd/playground/ffmpeg/
    and ffplay.c in ffmpeg
        Copyright (c) 2003 Fabrice Bellard
        http://ffmpeg.mplayerhq.hu/
    and gd.c in libGD
        http://cvs.php.net/viewvc.cgi/php-src/ext/gd/libgd/gd.c?revision=1.111&view=markup

    please excuse the mess, i am a very rusty programmer!
    helps, comments or patches are very welcomed. :)

    tested with ffmpeg-r14005 and gd-2.0.34

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

//// enable unicode functions in mingw
//#ifdef WIN32
//    #define UNICODE
//    #define _UNICODE
//#endif

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
//#include <unistd.h>

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"

#include "mtn.h"

/* more global variables */
char logs[1000000];
char *gb_version = "20121218a(j) copyright (c) 2007-2008 tuit, et al.";
params parameters =
        {
            NULL,
            GB_A_RATIO,
            GB_B_BLANK,
            GB_B_BEGIN, // skip this seconds from the beginning
            GB_C_COLUMN,
            GB_C_CUT, // cut movie; <=0 off
            GB_D_EDGE, // edge detection; 0 off; >0 on
            GB_E_EXT,
            GB_E_END, // skip this seconds at the end
            GB_F_FONTNAME,
            COLOR_INFO, // info color
            9, // info font size
            GB_F_FONTNAME, // time stamp fontname
            COLOR_WHITE, // time stamp color
            COLOR_BLACK, // time stamp shadow color
            8, // time stamp font size
            GB_G_GAP,
            GB_H_HEIGHT, // mininum height of each shot; will reduce # of column to meet this height
            GB_I_INFO, // 1 on; 0 off
            GB_I_INDIVIDUAL, // 1 on; 0 off
            GB_J_QUALITY,
            GB_K_BCOLOR, // background color
            GB_L_INFO_LOCATION,
            GB_L_TIME_LOCATION,
            GB_N_NORMAL, // normal priority; 1 normal; 0 lower
            GB_N_SUFFIX, // info text file suffix
            GB_O_SUFFIX,
            GB_O_OUTDIR,
            GB_Q_QUIET, // 1 on; 0 off
            GB_R_ROW, // 0 = as many rows as needed
            GB_S_STEP, // less than 0 = every frame; 0 = step evenly to get column x row
            GB_T_TIME, // 1 on; 0 off
            GB_T_TEXT,
            GB_V_VERBOSE, // 1 on; 0 off
            GB_V_VERBOSE, // 1 on; 0 off
            GB_W_WIDTH, // 0 = column * movie width
            GB_Z_SEEK, // always use seek mode; 1 on; 0 off
            GB_Z_NONSEEK // always use non-seek mode; 1 on; 0 off
        };

/* misc functions */

void format_time(double duration, char *str, char sep)
{
    if (duration < 0) {
        sprintf(str, "N/A");
    } else {
        int hours, mins, secs;
        secs = duration;
        mins = secs / 60;
        secs %= 60;
        hours = mins / 60;
        mins %= 60;
        sprintf(str, "%02d%c%02d%c%02d", hours, sep, mins, sep, secs);
    }
}

char *format_size(int64_t size, char *unit)
{
    static char buf[20]; // FIXME

    if (size < 1024) {
        sprintf(buf, "%"PRId64" %s", size, unit);
    } else if (size < 1024*1024) {
        sprintf(buf, "%.2f Ki%s", size/1024.0, unit);
    } else if (size < 1024*1024*1024) {
        sprintf(buf, "%.2f Mi%s", size/1024.0/1024, unit);
    } else {
        sprintf(buf, "%.2f Gi%s", size/1024.0/1024/1024, unit);
    }
    return buf;
}

/*
return only the file name of the full path
FIXME: wont work in unix if filename has '\\', e.g. path = "hello \\ world";
*/
char *path_2_file(char *path)
{
    int len = strlen(path);
    char *slash = strrchr(path, '/');
    char *backslash = strrchr(path, '\\');
    if (NULL != slash || NULL != backslash) {
        char *last = (slash > backslash) ? slash : backslash;
        if (last - path + 1 < len) { // make sure last char is not '/' or '\\'
            return last + 1;
        }
    }
    return path;
}

/* 
copy n strings to dst
... must be char *
dst must be large enough
*/
char *strcpy_va(char *dst, int n, ...)
{
    va_list ap;
    int pos = 0;
    dst[pos] = '\0';
    va_start(ap, n);
    int i;
    for (i=0; i < n; i++) {
        char *s = va_arg(ap, char *);
        assert(NULL != s);
        int len = strlen(s);
        strncpy(dst + pos, s, len + 1); // for '\0'
        pos += len;
    }
    va_end(ap);
    return dst;
}

/* mtn */

/* 
return pointer to a new cropped image. the original one is freed.
if error, return original and the original stays intact
*/
gdImagePtr crop_image(gdImagePtr ip, int new_width, int new_height)
{
    // cant find GD's crop, so we'll need to create a smaller image
    gdImagePtr new_ip = gdImageCreateTrueColor(new_width, new_height);
    if (NULL == new_ip) {
        //return NULL;
        // return the original should be better
        return ip;
    }
    gdImageCopy(new_ip, ip, 0, 0, 0, 0, new_width, new_height);
    gdImageDestroy(ip);
    return new_ip;
}

/*
returns height, or 0 if error
*/
int image_string_height(char *text, char *font, double size)
{
    int brect[8];

    if (NULL == text || 0 == strlen(text)) {
        return 0;
    }

    char *err = gdImageStringFT(NULL, &brect[0], 0, font, size, 0, 0, 0, text);
    if (NULL != err) {
        return 0;
    }
    return brect[3] - brect[7];
}

/*
position can be:
    1: lower left
    2: lower right
    3: upper right
    4: upper left
returns NULL if success, otherwise returns error message
*/
char *image_string(gdImagePtr ip, char *font, rgb_color color, double size, int position, int gap, char *text, int shadow, rgb_color shadow_color)
{
    int brect[8];

    int gd_color = gdImageColorResolve(ip, color.r, color.g, color.b);
    char *err = gdImageStringFT(NULL, &brect[0], gd_color, font, size, 0, 0, 0, text);
    if (NULL != err) {
        return err;
    }
    /*
    int width = brect[2] - brect[6];
    int height = brect[3] - brect[7];
    */

    int x, y;
    switch (position)
    {
    case 1: // lower left
        x = -brect[0] + gap;
        y = gdImageSY(ip) - brect[1] - gap;
        break;
    case 2: // lower right
        x = gdImageSX(ip) - brect[2] - gap;
        y = gdImageSY(ip) - brect[3] - gap;
        break;
    case 3: // upper right
        x = gdImageSX(ip) - brect[4] - gap;
        y = -brect[5] + gap;
        break;
    case 4: // upper left
        x = -brect[6] + gap;
        y = -brect[7] + gap;
        break;
    default:
        return "image_string's position can only be 1, 2, 3, or 4";
    }

    if (shadow) {
        int shadowx, shadowy;
        switch (position)
        {
        case 1: // lower left
            shadowx = x+1;
            shadowy = y;
            y = y-1;
            break;
        case 2: // lower right
            shadowx = x;
            shadowy = y;
            x = x-1;
            y = y-1;
            break;
        case 3: // upper right
            shadowx = x;
            shadowy = y+1;
            x = x-1;
            break;
        case 4: // upper left
            shadowx = x+1;
            shadowy = y+1;
            break;
        default:
            return "image_string's position can only be 1, 2, 3, or 4";
        }
        int gd_shadow = gdImageColorResolve(ip, shadow_color.r, shadow_color.g, shadow_color.b);
        err = gdImageStringFT(ip, &brect[0], gd_shadow, font, size, 0, shadowx, shadowy, text);
        if (NULL != err) {
            return err;
        }
    }

    return gdImageStringFT(ip, &brect[0], gd_color, font, size, 0, x, y, text);
}

/*
pFrame must be a PIX_FMT_RGB24 frame
*/
void FrameRGB_2_gdImage(AVFrame *pFrame, gdImagePtr ip, int width, int height)
{
    uint8_t *src = pFrame->data[0];
    int x, y;
    for (y = 0; y < height; y++) {
        for (x = 0; x < width * 3; x += 3) {
            gdImageSetPixel(ip, x / 3, y, gdImageColorResolve(ip, src[x], src[x + 1], src[x + 2]));
            //gdImageSetPixel(ip, x/3, y, gdTrueColor(src[x], src[x+1], src[x+2]));
        }
        src += width * 3;
    }
}

/*
*/
void shot_new(shot *psh)
{
    psh->ip = NULL;
    psh->eff_target = AV_NOPTS_VALUE;
    psh->found_pts = AV_NOPTS_VALUE;
    psh->blank = 0;
    int i;
    for (i=0; i<EDGE_PARTS; i++) {
        psh->edge[i] = 1;
    }
}

/* initialize 
*/
void thumb_new(thumbnail *ptn)
{
    ptn->out_ip = NULL;
    ptn->out_filename[0] = '\0';
    ptn->info_filename[0] = '\0';
    ptn->out_saved = 0;
    ptn->width = ptn->height = 0;
    ptn->txt_height = 0;
    ptn->column = ptn->row = 0;
    ptn->step = 0;
    ptn->shot_width = ptn->shot_height = 0;
    ptn->center_gap = 0;
    ptn->idx = -1;

    // dynamic
    ptn->ppts = NULL;
}

/* 
alloc dynamic data; must be called after all required static data is filled in
return -1 if failed
*/
int thumb_alloc_dynamic(thumbnail *ptn)
{
    ptn->ppts = malloc(ptn->column * ptn->row * sizeof(*(ptn->ppts)));
    if (NULL == ptn->ppts) {
        return -1;
    }
    return 0;
}

void thumb_cleanup_dynamic(thumbnail *ptn)
{
    if (NULL != ptn->ppts) {
        free(ptn->ppts);
        ptn->ppts = NULL;
    }
}

/* 
add shot
because ptn->idx is the last index, this function assumes that shots will be added 
in increasing order.
*/
void thumb_add_shot(thumbnail *ptn, gdImagePtr ip, int idx, int64_t pts)
{
    int dstX = idx%ptn->column * (ptn->shot_width+parameters.gb_g_gap) + parameters.gb_g_gap + ptn->center_gap;
    int dstY = idx/ptn->column * (ptn->shot_height+parameters.gb_g_gap) + parameters.gb_g_gap
        + ((3 == parameters.gb_L_info_location || 4 == parameters.gb_L_info_location) ? ptn->txt_height : 0);
    gdImageCopy(ptn->out_ip, ip, dstX, dstY, 0, 0, ptn->shot_width, ptn->shot_height);
    ptn->idx = idx;
    ptn->ppts[idx] = pts;
}

#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif
#ifndef MAX
#define MAX(a,b) ((a)<(b)?(b):(a))
#endif
/*
perform convolution on pFrame and store result in ip
pFrame must be a PIX_FMT_RGB24 frame
ip must be of the same size as pFrame
begin = upper left, end = lower right
filter should be a 2-dimensional but since we cant pass it without knowning the size, we'll use 1 dimension
modified from:
http://cvs.php.net/viewvc.cgi/php-src/ext/gd/libgd/gd.c?revision=1.111&view=markup
*/
void FrameRGB_convolution(AVFrame *pFrame, int width, int height, 
    float *filter, int filter_size, float filter_div, float offset,
    gdImagePtr ip, int xbegin, int ybegin, int xend, int yend)
{

    int x, y, i, j;
    float new_r, new_g, new_b;
    uint8_t *src = pFrame->data[0];

    for (y=ybegin; y<=yend; y++) {
        for(x=xbegin; x<=xend; x++) {
            new_r = new_g = new_b = 0;
            //float grey = 0;

            for (j=0; j<filter_size; j++) {
                int yv = MIN(MAX(y - filter_size/2 + j, 0), height - 1);
                for (i=0; i<filter_size; i++) {
                    int xv = MIN(MAX(x - filter_size/2 + i, 0), width - 1);
                    int pos = yv*width*3 + xv*3;
                    new_r += src[pos]   * filter[j * filter_size + i];
                    new_g += src[pos+1] * filter[j * filter_size + i];
                    new_b += src[pos+2] * filter[j * filter_size + i];
                    //grey += (src[pos] + src[pos+1] + src[pos+2])/3 * filter[j * filter_size + i];
                }
            }

            new_r = (new_r/filter_div)+offset;
            new_g = (new_g/filter_div)+offset;
            new_b = (new_b/filter_div)+offset;
            //grey = (grey/filter_div)+offset;

            new_r = (new_r > 255.0f)? 255.0f : ((new_r < 0.0f)? 0.0f:new_r);
            new_g = (new_g > 255.0f)? 255.0f : ((new_g < 0.0f)? 0.0f:new_g);
            new_b = (new_b > 255.0f)? 255.0f : ((new_b < 0.0f)? 0.0f:new_b);
            //grey = (grey > 255.0f)? 255.0f : ((grey < 0.0f)? 0.0f:grey);

            gdImageSetPixel(ip, x, y, gdImageColorResolve(ip, (int)new_r, (int)new_g, (int)new_b));
            //gdImageSetPixel(ip, x, y, gdTrueColor((int)new_r, (int)new_g, (int)new_b));
            //gdImageSetPixel(ip, x, y, gdTrueColor((int)grey, (int)grey, (int)grey));
        }
    }
}

/* begin = upper left, end = lower right
*/
float cmp_edge(gdImagePtr ip, int xbegin, int ybegin, int xend, int yend)
{
#define CMP_EDGE 208
    int count = 0;
    int i, j;
    for (j = ybegin; j <= yend; j++) {
        for (i = xbegin; i <= xend; i++) {
            int pixel = gdImageGetPixel(ip, i, j);
            if (gdImageRed(ip, pixel) >= CMP_EDGE 
                && gdImageGreen(ip, pixel) >= CMP_EDGE
                && gdImageBlue(ip, pixel) >= CMP_EDGE) {
                count++;
            }
        }
    }
    return (float)count / (yend - ybegin + 1) / (xend - xbegin + 1);
}

int is_edge(float *edge, float edge_found)
{
    if (parameters.gb_V) { // DEBUG
        return 1;
    }
    int count = 0;
    int i;
    for (i = 0; i < EDGE_PARTS; i++) {
        if (edge[i] >= edge_found) {
            count++;
        }
    }
    if (count >= 2) {
        return count;
    }
    return 0;
}

/*
pFrame must be an PIX_FMT_RGB24 frame
http://student.kuleuven.be/~m0216922/CG/
http://www.pages.drexel.edu/~weg22/edge.html
http://student.kuleuven.be/~m0216922/CG/filtering.html
http://cvs.php.net/viewvc.cgi/php-src/ext/gd/libgd/gd.c?revision=1.111&view=markup
*/
gdImagePtr detect_edge(AVFrame *pFrame, int width, int height, float *edge, float edge_found)
{
    static float filter[] = {
         0,-1, 0,
        -1, 4,-1,
         0,-1, 0
    };
#define FILTER_SIZE 3 // 3x3
#define FILTER_DIV 1
#define OFFSET 128
    static int init_filter = 0; // FIXME
    if (0 == init_filter) {
        init_filter = 1;
        filter[1] = -parameters.gb_D_edge/4.0f;
        filter[3] = -parameters.gb_D_edge/4.0f;
        filter[4] =  parameters.gb_D_edge;
        filter[5] = -parameters.gb_D_edge/4.0f;
        filter[7] = -parameters.gb_D_edge/4.0f;
    }

    gdImagePtr ip = gdImageCreateTrueColor(width, height);
    if (NULL == ip) {
        av_log(NULL, AV_LOG_ERROR, "  gdImageCreateTrueColor failed\n");
        return NULL;
    }
    if (parameters.gb_v_verbose > 0) {
        FrameRGB_2_gdImage(pFrame, ip, width, height);
    }

    int i;
    for (i = 0; i < EDGE_PARTS; i++) {
        edge[i] = 1;
    }

    // check 6 parts to speed this up & to improve correctness
    int y_size = height/10;
    int ya = y_size*2;
    int yb = y_size*4;
    int yc = y_size*6;
    int x_crop = width/8;

    // only find edge if neccessary
    int parts[EDGE_PARTS][4] = {
        //xbegin, ybegin, xend, yend
        {x_crop, ya, width/2, ya+y_size},
        {width/2+1, ya+y_size, width-x_crop, ya+2*y_size},
        {x_crop, yb, width/2, yb+y_size},
        {width/2+1, yb+y_size, width-x_crop, yb+2*y_size},
        {x_crop, yc, width/2, yc+y_size},
        {width/2+1, yc+y_size, width-x_crop, yc+2*y_size},
    };
    int count = 0;
    for (i = 0; i < EDGE_PARTS && count < 2; i++) {
        FrameRGB_convolution(pFrame, width, height, filter, FILTER_SIZE, FILTER_DIV, OFFSET, 
            ip, parts[i][0], parts[i][1], parts[i][2], parts[i][3]);
        edge[i] = cmp_edge(ip, parts[i][0], parts[i][1], parts[i][2], parts[i][3]);
        if (edge[i] >= edge_found) {
            count++;
        }
    }
    return ip;
}

/* av_pkt_dump_log()?? */
void dump_packet(AVPacket *p, AVStream * ps)
{
    /* from av_read_frame()
    pkt->pts, pkt->dts and pkt->duration are always set to correct values in 
    AVStream.timebase units (and guessed if the format cannot provided them). 
    pkt->pts can be AV_NOPTS_VALUE if the video format has B frames, so it is 
    better to rely on pkt->dts if you do not decompress the payload.
    */
    //av_log(NULL, AV_LOG_VERBOSE, "***dump_packet: pos:%"PRId64"\n", p->pos);
    //av_log(NULL, AV_LOG_VERBOSE, "pts tb: %"PRId64", dts tb: %"PRId64", duration tb: %d\n",
    //    p->pts, p->dts, p->duration);
    av_log(NULL, AV_LOG_VERBOSE, "pts s: %.2f, dts s: %.2f, duration s: %.2f\n",
        p->pts * av_q2d(ps->time_base), p->dts * av_q2d(ps->time_base), 
        p->duration * av_q2d(ps->time_base)); // pts can be AV_NOPTS_VALUE 
}

void dump_codec_context(AVCodecContext * p)
{
    av_log(NULL, AV_LOG_VERBOSE, "***dump_codec_context %s, time_base: %d / %d\n", p->codec_name, 
        p->time_base.num, p->time_base.den);
    av_log(NULL, AV_LOG_VERBOSE, "frame_number: %d, width: %d, height: %d, sample_aspect_ratio %d/%d%s\n",
        p->frame_number, p->width, p->height, p->sample_aspect_ratio.num, p->sample_aspect_ratio.den,
        (0 == p->sample_aspect_ratio.num) ? "" : "**a**");
}

void dump_index_entries(AVStream * p)
{
    // index_entries are only used if the format does not support seeking natively 
    int i;
    double diff = 0;
    for (i=0; i < p->nb_index_entries; i++) { 
        AVIndexEntry *e = p->index_entries + i;
        double prev_ts = 0, cur_ts = 0;
        cur_ts = e->timestamp * av_q2d(p->time_base);
        //assert(cur_ts > 0);
        diff += cur_ts - prev_ts;
        if (i < 20) { // show only first 20
            //av_log(NULL, AV_LOG_VERBOSE, "    i: %d, pos: %"PRId64", timestamp tb: %"PRId64", timestamp s: %.2f, flags: %d, size: %d, min_distance: %d\n",
             //   i, e->pos, e->timestamp, e->timestamp * av_q2d(p->time_base), e->flags, e->size, e->min_distance);
        }
        prev_ts = cur_ts;
    }
    av_log(NULL, AV_LOG_VERBOSE, "  *** nb_index_entries: %d, avg. timestamp s diff: %.2f\n", p->nb_index_entries, diff / p->nb_index_entries);
}

void dump_stream(AVStream * p)
{
    av_log(NULL, AV_LOG_VERBOSE, "***dump_stream, time_base: %d / %d\n", 
        p->time_base.num, p->time_base.den);
    //av_log(NULL, AV_LOG_VERBOSE, "cur_dts tb?: %"PRId64", start_time tb: %"PRId64", duration tb: %"PRId64", nb_frames: %"PRId64"\n",
        //p->cur_dts, p->start_time, p->duration, p->nb_frames);
    // get funny results here. use format_context's.
    av_log(NULL, AV_LOG_VERBOSE, "cur_dts s?: %.2f, start_time s: %.2f, duration s: %.2f\n",
        p->cur_dts * av_q2d(p->time_base), p->start_time * av_q2d(p->time_base), 
        p->duration * av_q2d(p->time_base)); // duration can be AV_NOPTS_VALUE 
    // field pts in AVStream is for encoding
}

/*
set scale source width & height (scaled_w and scaled_h)
*/
void calc_scale_src(int width, int height, AVRational ratio, int *scaled_w, int *scaled_h)
{
    // mplayer dont downscale horizontally. however, we'll always scale
    // horizontally, up or down, which is the same as mpc's display and 
    // vlc's snapshot. this should make square pixel for both pal & ntsc.
    *scaled_w = width;
    *scaled_h = height;
    if (0 != ratio.num) { // ratio is defined
        assert(ratio.den != 0);
        *scaled_w = av_q2d(ratio) * width + 0.5; // round nearest
    }
}

/*
modified from libavformat's dump_format
*/
void get_stream_info_type(AVFormatContext *ic, enum AVMediaType type, char *buf, AVRational sample_aspect_ratio)
{
    char sub_buf[1024] = ""; // FIXME
    unsigned int i;
    for(i=0; i<ic->nb_streams; i++) {
        char codec_buf[256];
        int flags = ic->iformat->flags;
        AVStream *st = ic->streams[i];
        AVDictionaryEntry *language = av_dict_get(ic->metadata, "language", NULL, 0);

        if (type != st->codec->codec_type) {
            continue;
        }

        if (AVMEDIA_TYPE_SUBTITLE == st->codec->codec_type) {
            if (language != NULL) {
                sprintf(sub_buf + strlen(sub_buf), "%s ", language->value);
            } else {
                // FIXME: ignore for now; language seem to be missing in .vob files
                //sprintf(sub_buf + strlen(sub_buf), "? ");
            }
            continue;
        }

        if (parameters.gb_v_verbose > 0) {
            sprintf(buf + strlen(buf), "Stream %d", i);
            if (flags & AVFMT_SHOW_IDS) {
                sprintf(buf + strlen(buf), "[0x%x]", st->id);
            }
            /*
            int g = ff_gcd(st->time_base.num, st->time_base.den);
            sprintf(buf + strlen(buf), ", %d/%d", st->time_base.num/g, st->time_base.den/g);
            */
            sprintf(buf + strlen(buf), ": ");
        }

        avcodec_string(codec_buf, sizeof(codec_buf), st->codec, 0);
        // remove [PAR DAR] from string, it's not very useful.
        char *begin = NULL, *end = NULL;
        if ((begin=strstr(codec_buf, " [PAR")) != NULL 
            && (end=strchr(begin, ']')) != NULL) {
            while (*++end != '\0') {
                *begin++ = *end;
            }
            *begin = '\0';
        }
        sprintf(buf + strlen(buf), codec_buf);

        if (st->codec->codec_type == AVMEDIA_TYPE_VIDEO){
            if (st->r_frame_rate.den && st->r_frame_rate.num)
                sprintf(buf + strlen(buf), ", %5.2f fps(r)", av_q2d(st->r_frame_rate));
            //else if(st->time_base.den && st->time_base.num)
            //  sprintf(buf + strlen(buf), ", %5.2f fps(m)", 1/av_q2d(st->time_base));
            else
                sprintf(buf + strlen(buf), ", %5.2f fps(c)", 1/av_q2d(st->codec->time_base));

            // show aspect ratio
            int scaled_src_width, scaled_src_height;
            calc_scale_src(st->codec->width, st->codec->height, sample_aspect_ratio,
                &scaled_src_width, &scaled_src_height);
            if (scaled_src_width != st->codec->width || scaled_src_height != st->codec->height) {
                sprintf(buf + strlen(buf), " => %dx%d", scaled_src_width, scaled_src_height);
            }
        }
        if (language != NULL) {
            sprintf(buf + strlen(buf), " (%s)", language->value);
        }
        sprintf(buf + strlen(buf), NEWLINE);
    }

    if (0 < strlen(sub_buf)) {
        sprintf(buf + strlen(buf), "Subtitles: %s\n", sub_buf);
    }
}

/*
modified from libavformat's dump_format
*/
char *get_stream_info(AVFormatContext *ic, char *url, int strip_path, AVRational sample_aspect_ratio)
{
    static char buf[4096]; // FIXME: this is also used for all text at the top
    int duration = -1;

    char *file_name = url;
    if (1 == strip_path) {
        file_name = path_2_file(url);
    }

    sprintf(buf, "File: %s", file_name);
    //sprintf(buf + strlen(buf), " (%s)", ic->iformat->name);
    sprintf(buf + strlen(buf), "%sSize: %"PRId64" bytes (%s)", NEWLINE, avio_size(ic->pb), format_size(avio_size(ic->pb), "B"));
    if (ic->duration != AV_NOPTS_VALUE) { // FIXME: gcc warning: comparison between signed and unsigned
        int hours, mins, secs;
        duration = secs = ic->duration / AV_TIME_BASE;
        mins = secs / 60;
        secs %= 60;
        hours = mins / 60;
        mins %= 60;
        sprintf(buf + strlen(buf), ", duration: %02d:%02d:%02d", hours, mins, secs);
    } else {
        sprintf(buf + strlen(buf), ", duration: N/A");
    }
    /*
    if (ic->start_time != AV_NOPTS_VALUE) {
        int secs, us;
        secs = ic->start_time / AV_TIME_BASE;
        us = ic->start_time % AV_TIME_BASE;
        sprintf(buf + strlen(buf), ", start: %d.%06d", secs, (int)av_rescale(us, 1000000, AV_TIME_BASE));
    }
    */

    // some formats, eg. flv, dont seem to support bit_rate, so we'll prefer to 
    // calculate from duration.
    // is this ok? probably not ok with .vob files when duration is wrong. DEBUG
    if (duration > 0) {
        sprintf(buf + strlen(buf), ", avg.bitrate: %.0f kb/s%s", (double) avio_size(ic->pb) * 8 / duration / 1000, NEWLINE);
    } else if (ic->bit_rate) {
        sprintf(buf + strlen(buf), ", bitrate: %d kb/s%s", ic->bit_rate / 1000, NEWLINE);
    } else {
        sprintf(buf + strlen(buf), ", bitrate: N/A%s", NEWLINE);
    }

    get_stream_info_type(ic, AVMEDIA_TYPE_AUDIO, buf, sample_aspect_ratio);
    get_stream_info_type(ic, AVMEDIA_TYPE_VIDEO, buf, sample_aspect_ratio);
    get_stream_info_type(ic, AVMEDIA_TYPE_SUBTITLE, buf, sample_aspect_ratio);
    // CODEC_TYPE_DATA FIXME: what is this type?
    // CODEC_TYPE_NB FIXME: what is this type?

    //strfmon(buf + strlen(buf), 100, "strfmon: %!i\n", avio_size(ic->pb));
    return buf;
}

void dump_format_context(AVFormatContext *p, int __attribute__((unused)) index, char *url, int __attribute__((unused)) is_output)
{
    //av_log(NULL, AV_LOG_ERROR, "\n");
    av_log(NULL, AV_LOG_VERBOSE, "***dump_format_context, name: %s, long_name: %s\n", 
        p->iformat->name, p->iformat->long_name);
    //dump_format(p, index, url, is_output);

    // dont show scaling info at this time because we dont have the proper sample_aspect_ratio
    av_log(NULL, LOG_INFO, get_stream_info(p, url, 0, GB_A_RATIO)); 

    //av_log(NULL, AV_LOG_VERBOSE, "start_time av: %"PRId64", duration av: %"PRId64", file_size: %"PRId64"\n",
    //    p->start_time, p->duration, avio_size(p->pb));
    av_log(NULL, AV_LOG_VERBOSE, "start_time s: %.2f, duration s: %.2f\n",
        (double) p->start_time / AV_TIME_BASE, (double) p->duration / AV_TIME_BASE);

    AVDictionaryEntry* track = av_dict_get(p->metadata, "track", NULL, 0);
    AVDictionaryEntry* title = av_dict_get(p->metadata, "title", NULL, 0);
    AVDictionaryEntry* author = av_dict_get(p->metadata, "author", NULL, 0);
    AVDictionaryEntry* copyright = av_dict_get(p->metadata, "copyright", NULL, 0);
    AVDictionaryEntry* comment = av_dict_get(p->metadata, "comment", NULL, 0);
    AVDictionaryEntry* album = av_dict_get(p->metadata, "album", NULL, 0);
    AVDictionaryEntry* year = av_dict_get(p->metadata, "year", NULL, 0);
    AVDictionaryEntry* genre = av_dict_get(p->metadata, "genre", NULL, 0);

    if (track != NULL)
        av_log(NULL, LOG_INFO, "  Track: %s\n", track->value);
    if (title != NULL)
        av_log(NULL, LOG_INFO, "  Title: %s\n", title->value);
    if (author != NULL)
        av_log(NULL, LOG_INFO, "  Author: %s\n", author->value);
    if (copyright != NULL)
        av_log(NULL, LOG_INFO, "  Copyright: %s\n", copyright->value);
    if (comment != NULL)
        av_log(NULL, LOG_INFO, "  Comment: %s\n", comment->value);
    if (album != NULL)
        av_log(NULL, LOG_INFO, "  Album: %s\n", album->value);
    if (year != NULL)
        av_log(NULL, LOG_INFO, "  Year: %s\n", year->value);
    if (genre != NULL)
        av_log(NULL, LOG_INFO, "  Genre: %s\n", genre->value);
}

/*
*/
double uint8_cmp(uint8_t *pa, uint8_t *pb, uint8_t *pc, int n)
{
    int i, same = 0;
    for (i=0; i<n; i++) {
        int diffab = pa[i] - pb[i];
        int diffac = pa[i] - pc[i];
        int diffbc = pb[i] - pb[i];

        if ((diffab > -20) && (diffab < 20) &&
            (diffac > -20) && (diffac < 20) &&
            (diffbc > -20) && (diffbc < 20)) {
            same++;
        }
    }
    return (double)same / n;
}

/*
return sameness of the frame; 1 means the frame is the same in all directions, i.e. blank
pFrame must be an PIX_FMT_RGB24 frame
*/
double blank_frame(AVFrame *pFrame, int width, int height)
{
    uint8_t *src = pFrame->data[0];
    int hor_size = height/11 * width * 3;
    uint8_t *pa = src+hor_size*2;
    uint8_t *pb = src+hor_size*5;
    uint8_t *pc = src+hor_size*8;
    double same = .4*uint8_cmp(pa, pb, pc, hor_size);
    int ver_size = hor_size/3;
    same += .6/3*uint8_cmp(pa, pa + ver_size, pa + ver_size*2, ver_size);
    same += .6/3*uint8_cmp(pb, pb + ver_size, pb + ver_size*2, ver_size);
    same += .6/3*uint8_cmp(pc, pc + ver_size, pc + ver_size*2, ver_size);
    return same;
}

/* global */
uint64_t gb_video_pkt_pts = AV_NOPTS_VALUE;
/* These are called whenever we allocate a frame
 * buffer. We use this to store the global_pts in
 * a frame at the time it is allocated.
 */
int our_get_buffer(struct AVCodecContext *c, AVFrame *pic) {
  int ret = avcodec_default_get_buffer(c, pic);
  uint64_t *pts = av_malloc(sizeof(uint64_t));
  *pts = gb_video_pkt_pts;
  pic->opaque = pts;
  //av_log(NULL, AV_LOG_VERBOSE, "*coping gb_video_pkt_pts: %"PRId64" to opaque\n", gb_video_pkt_pts);
  return ret;
}

void our_release_buffer(struct AVCodecContext *c, AVFrame *pic) {
  if(pic) av_freep(&pic->opaque);
  avcodec_default_release_buffer(c, pic);
}

/*
set first to 1 when calling this for the first time of a file
return >0 if can read packet(s) & decode a frame, *pPts is set to packet's pts
return  0 if end of file
return <0 if error
*/
int read_and_decode(AVFormatContext *pFormatCtx, int video_index, 
    AVCodecContext *pCodecCtx, AVFrame *pFrame, int64_t *pPts, int key_only, int first)
{
    //double pts = -99999;
    AVPacket packet;
    AVStream *pStream = pFormatCtx->streams[video_index];
    int decoded_frame = 0;
    static int run = 0; // # of times read_and_decode has been called for a file
    static double avg_decoded_frame = 0; // average # of decoded frame
    static int skip_non_key = 0;

    if (first) {
        // reset stats
        run = 0;
        avg_decoded_frame = 0;
        skip_non_key = 0;
    }

    int got_picture;
    int pkt_without_pic = 0; // # of video packet read without getting a picture
    //for (got_picture = 0; 0 == got_picture; av_free_packet(&packet)) {
    // keep decoding until we get a key frame
    for (got_picture = 0; 0 == got_picture 
        //|| (1 == key_only && !(1 == pFrame->key_frame && FF_I_TYPE == pFrame->pict_type)); // same as version 0.61
        || (1 == key_only && !(1 == pFrame->key_frame || AV_PICTURE_TYPE_I == pFrame->pict_type)); // same as version 2.42
        //|| (1 == key_only && 1 != pFrame->key_frame); // is there a reason why not use this? t_warhawk_review_gt_h264.mov (svq3) seems to set only pict_type
        av_free_packet(&packet)) {

        if (0 != av_read_frame(pFormatCtx, &packet)) {
            if (pFormatCtx->pb->error) {
                return -1;
            }
            return 0;
        }

        // Is this a packet from the video stream?
        if (packet.stream_index != video_index) {
            continue;
        }
        
        // skip all non-key packet? would this work? // FIXME
        // this seems to slow down nike files. why?
        // so we'll use it only when a key frame is difficult to find.
        // hope this wont break anything. :)
        // this seems to help a lot for files with vorbis audio
        if (1 == skip_non_key && 1 == key_only && !(packet.flags & AV_PKT_FLAG_KEY)) {
            continue;
        }
        
        dump_packet(&packet, pStream);
        //dump_codec_context(pCodecCtx);

        // Save global pts to be stored in pFrame in first call
        //av_log(NULL, AV_LOG_VERBOSE, "*saving gb_video_pkt_pts: %"PRId64"\n", packet.pts);
        gb_video_pkt_pts = packet.pts;

        // Decode video frame
        avcodec_decode_video2(pCodecCtx, pFrame, &got_picture, &packet);
        // error is ignored. perhaps packets read are not enough.
        av_log(NULL, AV_LOG_VERBOSE, "*avcodec_decode_video2: got_picture: %d, key_frame: %d, pict_type: %d\n", got_picture, pFrame->key_frame, pFrame->pict_type);

        // FIXME: with some .dat files, got_picture is never set, why??

        if (0 == got_picture) {
            pkt_without_pic++;
            if (0 == pkt_without_pic%50) {
                av_log(NULL, LOG_INFO, "  no picture in %d packets\n", pkt_without_pic);
            }
            if (1000 == pkt_without_pic) { // is 1000 enough? // FIXME
                av_log(NULL, AV_LOG_ERROR, "  * avcodec_decode_video2 couldn't decode picture\n");
                av_free_packet(&packet);
                return -1;
            }
        } else {
            pkt_without_pic = 0;
            decoded_frame++;
            // some codecs, e.g avisyth, dont seem to set key_frame
            if (1 == key_only && 0 == decoded_frame%200) {
                av_log(NULL, LOG_INFO, "  a key frame is not found in %d frames\n", decoded_frame);
            }
            if (1 == key_only && 400 == decoded_frame) {
                // is there a way to know when a frame has no missing pieces 
                // even though it's not a key frame?? // FIXME
                av_log(NULL, LOG_INFO, "  * using a non-key frame; file problem? ffmpeg's codec problem?\n");
                break;
            }
        }

        // WTH?? why copy pts from dts?
        /*
        if(AV_NOPTS_VALUE == packet.dts 
            && NULL != pFrame->opaque 
            && AV_NOPTS_VALUE != *(uint64_t *) pFrame->opaque) {
            pts = *(uint64_t *)pFrame->opaque;
        } else if(packet.dts != AV_NOPTS_VALUE) {
            pts = packet.dts;
        } else {
            pts = 0;
        }
        pts *= av_q2d(pStream->time_base);
        //av_log(NULL, AV_LOG_VERBOSE, "*after avcodec_decode_video pts: %.2f\n", pts);
        */
    }
    av_free_packet(&packet);

    // stats & enable skipping of non key packets
    run++;
    avg_decoded_frame = (avg_decoded_frame*(run-1) + decoded_frame) / run;
    //av_log(NULL, LOG_INFO, "  decoded frames: %d, avg. decoded frames: %.2f, pict_type: %d\n", 
    //    decoded_frame, avg_decoded_frame, pFrame->pict_type); // DEBUG
    if (0 == skip_non_key && run >= 3 && avg_decoded_frame > 30) {
        skip_non_key = 1;
        av_log(NULL, LOG_INFO, "  skipping non key packets for this file\n");
    }

    av_log(NULL, AV_LOG_VERBOSE, "*****got picture, repeat_pict: %d%s, key_frame: %d, pict_type: %d\n", pFrame->repeat_pict,
        (pFrame->repeat_pict > 0) ? "**r**" : "", pFrame->key_frame, pFrame->pict_type);
    if(NULL != pFrame->opaque && AV_NOPTS_VALUE != *(uint64_t *) pFrame->opaque) {
        //av_log(NULL, AV_LOG_VERBOSE, "*pts: %.2f, value in opaque: %"PRId64"\n", pts, *(uint64_t *) pFrame->opaque);
        //av_log(NULL, AV_LOG_VERBOSE, "*value in opaque: %"PRId64"\n", *(uint64_t *) pFrame->opaque);
    }
    dump_stream(pStream);
    dump_codec_context(pCodecCtx);
    *pPts = packet.pts;
    return 1;
}

/* calculate timestamp to display to users
*/
double calc_time(int64_t timestamp, AVRational time_base, double start_time)
{
    // for files with start_time > 0, we need to subtract the start_time 
    // from timestamp. this should match the time display by MPC & VLC. 
    // however, for .vob files of dvds, after subtracting start_time
    // each file will start with timestamp 0 instead of continuing from the previous file.

    // for unknown start_time, we ignore it
    //if (start_time < 0) {
    //    return av_rescale(timestamp, time_base.num, time_base.den);
    //} else {
        return av_rescale(timestamp, time_base.num, time_base.den) - start_time;
    //}
}

/*
return the duration. guess when unknown.
must be called after codec has been opened
*/
double guess_duration(AVFormatContext *pFormatCtx, int index, 
    AVCodecContext __attribute__((unused)) *pCodecCtx, AVFrame __attribute__((unused)) *pFrame)
{
    double duration = (double) pFormatCtx->duration / AV_TIME_BASE; // can be incorrect for .vob files
    if (duration > 0) {
        return duration;
    }

    AVStream *pStream = pFormatCtx->streams[index];
    double guess;

    // if stream bitrate is known we'll interpolate from file size.
    // pFormatCtx->start_time would be incorrect for .vob file with multiple titles.
    // pStream->start_time doesn't work either. so we'll need to disable timestamping.
    assert(NULL != pStream && NULL != pStream->codec);
    if (pStream->codec->bit_rate > 0 && avio_size(pFormatCtx->pb) > 0) {
        guess = 0.9 * avio_size(pFormatCtx->pb) / (pStream->codec->bit_rate / 8);
        if (guess > 0) {
            av_log(NULL, AV_LOG_ERROR, "  ** duration is unknown: %.2f; guessing: %.2f s from bit_rate\n", duration, guess);
            return guess;
        }
    }

    return -1;
    
    // the following doesn't work.
    /*
    // we'll guess the duration by seeking to near the end of the file and
    // decode a frame. the timestamp of that frame is the guess.
    // things get more complicated for dvd's .vob files. each .vob file
    // can contain more than 1 title. and each title will have its own pts.
    // for example, 90% of a .vob might be for title 1 and the last 10%
    // might be for title 2; seeking to near the end will end up getting 
    // title 2's pts. this problem cannot be solved if we just look at the
    // .vob files. need to process other info outside .vob files too.
    // as a result, the following will probably never work.
    // .vob files weirdness will make our assumption to seek by byte incorrect too.
    if (pFormatCtx->file_size <= 0) {
        return -1;
    }
    int64_t byte_pos = 0.9 * pFormatCtx->file_size;
    int ret = av_seek_frame(pFormatCtx, index, byte_pos, AVSEEK_FLAG_BYTE);
    if (ret < 0) { // failed
        return -1;
    }
    avcodec_flush_buffers(pCodecCtx);
    int64_t pts;
    ret = read_and_decode(pFormatCtx, index, pCodecCtx, pFrame, &pts, 0); // FIXME: key or not?
    if (ret <= 0) { // end of file or error
        av_log(NULL, AV_LOG_VERBOSE, "  read_and_decode during guessing duration failed\n");
        return -1;
    }
    double start_time = (double) pFormatCtx->start_time / AV_TIME_BASE; // FIXME: can be unknown?
    guess = calc_time(pts, pStream->time_base, start_time);
    if (guess <= 0) {
        return -1;
    }
    av_log(NULL, AV_LOG_ERROR, "  ** duration is unknown: %.2f; guessing: %.2f s.\n", duration, guess);

    // seek back to 0 & flush buffer; FIXME: is 0 correct?
    av_seek_frame(pFormatCtx, index, 0, AVSEEK_FLAG_BYTE); // ignore errors
    avcodec_flush_buffers(pCodecCtx);

    return guess;
    */
}

/*
try hard to seek
assume flags can be either 0 or AVSEEK_FLAG_BACKWARD
*/
int really_seek(AVFormatContext *pFormatCtx, int index, int64_t timestamp, int flags, double duration)
{
    assert(flags == 0 || flags == AVSEEK_FLAG_BACKWARD);
    int ret;

    /* first try av_seek_frame */
    ret = av_seek_frame(pFormatCtx, index, timestamp, flags);
    if (ret >= 0) { // success
        return ret;
    }

    /* then we try seeking to any (non key) frame AVSEEK_FLAG_ANY */
    ret = av_seek_frame(pFormatCtx, index, timestamp, flags | AVSEEK_FLAG_ANY);
    if (ret >= 0) { // success
        //av_log(NULL, LOG_INFO, "AVSEEK_FLAG_ANY: timestamp: %"PRId64"\n", timestamp); // DEBUG
        return ret;
    }

    /* and then we try seeking by byte (AVSEEK_FLAG_BYTE) */
    // here we assume that the whole file has duration seconds.
    // so we'll interpolate accordingly.
    AVStream *pStream = pFormatCtx->streams[index];
    int64_t duration_tb = duration / av_q2d(pStream->time_base); // in time_base unit
    double start_time = (double) pFormatCtx->start_time / AV_TIME_BASE; // in seconds
    // if start_time is negative, we ignore it; FIXME: is this ok?
    if (start_time < 0) {
        start_time = 0;
    }

    // normally when seeking by timestamp we add start_time to timestamp 
    // before seeking, but seeking by byte we need to subtract the added start_time
    timestamp -= start_time / av_q2d(pStream->time_base);
    if (avio_size(pFormatCtx->pb) <= 0) {
        return -1;
    }
    if (duration > 0) {
        int64_t byte_pos = av_rescale(timestamp, avio_size(pFormatCtx->pb), duration_tb);
        //av_log(NULL, LOG_INFO, "AVSEEK_FLAG_BYTE: byte_pos: %"PRId64", timestamp: %"PRId64", file_size: %"PRId64", duration_tb: %"PRId64"\n", byte_pos, timestamp, avio_size(pFormatCtx->pb), duration_tb);
        return av_seek_frame(pFormatCtx, index, byte_pos, AVSEEK_FLAG_BYTE);
    }

    return -1;
}

/*
*/
void make_thumbnail(char *file)
{
    av_log(NULL, AV_LOG_VERBOSE, "make thumbnail: %s\n", file);
    static int nb_file = 0; // FIXME: static
    nb_file++;

    struct timeval tstart;
    gettimeofday(&tstart, NULL);

    int i;
    thumbnail tn; // thumbnail data & info
    thumb_new(&tn);
    // shot sh; // shot info

    shot fill_buffer[parameters.gb_c_column-1]; // skipped shots to fill the last row
    for (i=0; i<parameters.gb_c_column-1; i++) {
        fill_buffer[i].ip = NULL;
    }
    int nb_shots = 0; // # of decoded shots (stat purposes)

    /* these are checked during cleaning up, must be NULL if not used */
    AVFormatContext *pFormatCtx = NULL;
    AVCodecContext *pCodecCtx = NULL;
    AVDictionary *pDict = NULL;
    AVFrame *pFrame = NULL;
    AVFrame *pFrameRGB = NULL;
    uint8_t *rgb_buffer = NULL;
    struct SwsContext *pSwsCtx = NULL;
    tn.out_ip = NULL;
    FILE *out_fp = NULL;
    FILE *info_fp = NULL;
    gdImagePtr ip = NULL;

    int t_timestamp = parameters.gb_t_timestamp; // local timestamp; can be turned off; 0 = off
    int ret;

    av_log(NULL, LOG_INFO, "\n");

    /* check if output file already exists & open output file */
    if (NULL != parameters.gb_O_outdir && strlen(parameters.gb_O_outdir) > 0) {
        strcpy_va(tn.out_filename, 3, parameters.gb_O_outdir, "/", path_2_file(file));
        if (NULL != parameters.gb_N_suffix)
            strcpy_va(tn.info_filename, 3, parameters.gb_O_outdir, "/", path_2_file(file));
    } else {
        strcpy(tn.out_filename, file);
        if (NULL != parameters.gb_N_suffix)
            strcpy(tn.info_filename, file);
    }
    char *suffix = strrchr(tn.out_filename, '.');

#if GB_O_SUFFIX_USE_FULL
    strcat(tn.out_filename, parameters.gb_o_suffix);
#else
    if (NULL == suffix) {
        strcat(tn.out_filename,parameters. gb_o_suffix);
    } else {
        strcpy(suffix, parameters.gb_o_suffix);
    }
#endif

    if (NULL !=parameters. gb_N_suffix) {
        suffix = strrchr(tn.info_filename, '.');
        if (NULL == suffix) {
            strcat(tn.info_filename, parameters.gb_N_suffix);
        } else {
            strcpy(suffix, parameters.gb_N_suffix);
        }
    }

#if defined(WIN32) && defined(_UNICODE)
    wchar_t out_filename_w[FILENAME_MAX];
    UTF8_2_WC(out_filename_w, tn.out_filename, FILENAME_MAX);
    wchar_t info_filename_w[FILENAME_MAX];
    UTF8_2_WC(info_filename_w, tn.info_filename, FILENAME_MAX);
#else
    char *out_filename_w = tn.out_filename;
    char *info_filename_w = tn.info_filename;
#endif
    out_fp = _tfopen(out_filename_w, _TEXT("wb"));
    if (NULL == out_fp) {
        av_log(NULL, AV_LOG_ERROR, "\n%s: creating output image '%s' failed: %s\n", parameters.gb_argv0, tn.out_filename, strerror(errno));
        goto cleanup;
    }
    if (NULL != parameters.gb_N_suffix) {
        info_fp = _tfopen(info_filename_w, _TEXT("wb"));
        if (NULL == info_fp) {
            av_log(NULL, AV_LOG_ERROR, "\n%s: creating info file '%s' failed: %s\n", parameters.gb_argv0, tn.info_filename, strerror(errno));
            goto cleanup;
        }
    }

    // Open video file
    ret = avformat_open_input(&pFormatCtx, file, NULL, NULL);
    if (0 != ret) {
        av_log(NULL, AV_LOG_ERROR, "\n%s: avformat_open_input %s failed: %d\n", parameters.gb_argv0, file, ret);
        goto cleanup;
    }

    // generate pts?? -- from ffplay, not documented
    // it should make av_read_frame() generate pts for unknown value
    assert(NULL != pFormatCtx);
    pFormatCtx->flags |= AVFMT_FLAG_GENPTS;

    // Retrieve stream information
    ret = avformat_find_stream_info(pFormatCtx, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "\n%s: avformat_find_stream_info %s failed: %d\n", parameters.gb_argv0, file, ret);
        goto cleanup;
    }
    dump_format_context(pFormatCtx, nb_file, file, 0);

    // Find the first video stream
    // int av_find_default_stream_index(AVFormatContext *s)
    int video_index = -1;
    for (i = 0; i < pFormatCtx->nb_streams; i++) {
        if (AVMEDIA_TYPE_VIDEO == pFormatCtx->streams[i]->codec->codec_type) {
            video_index = i;
            break;
        }
    }
    if (video_index == -1) {
        av_log(NULL, AV_LOG_ERROR, "  couldn't find a video stream\n");
        goto cleanup;
    }

    AVStream *pStream = pFormatCtx->streams[video_index];
    pCodecCtx = pStream->codec;
    dump_stream(pStream);
    dump_index_entries(pStream);
    dump_codec_context(pCodecCtx);
    av_log(NULL, AV_LOG_VERBOSE, "\n");

    // Find the decoder for the video stream
    AVCodec *pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
    if (pCodec == NULL) {
        av_log(NULL, AV_LOG_ERROR, "  couldn't find a decoder for codec_id: %d\n", pCodecCtx->codec_id);
        goto cleanup;
    }

    // discard frames; is this OK?? // FIXME
    if (parameters.gb_s_step >= 0) {
        // nonkey & bidir cause program crash with some files, e.g. tokyo 275 .
        // codec bugs???
        //pCodecCtx->skip_frame = AVDISCARD_NONKEY; // slower with nike 15-11-07
        //pCodecCtx->skip_frame = AVDISCARD_BIDIR; // this seems to speed things up
        pCodecCtx->skip_frame = AVDISCARD_NONREF; // internal err msg but not crash
    }

    // Open codec
    ret = avcodec_open2(pCodecCtx, pCodec, &pDict);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "  couldn't open codec %s id %d: %d\n", pCodec->name, pCodec->id, ret);
        goto cleanup;
    }
    pCodecCtx->get_buffer = our_get_buffer;
    pCodecCtx->release_buffer = our_release_buffer;

    // Allocate video frame
    pFrame = avcodec_alloc_frame();
    if (pFrame == NULL) {
        av_log(NULL, AV_LOG_ERROR, "  couldn't allocate a video frame\n");
        goto cleanup;
    }

    // keep a copy of sample_aspect_ratio because it might be changed after 
    // decoding a frame, e.g. Dragonball Z 001 (720x480 H264 AAC).mkv
    // is this a codec bug? it seem this value can be in the header or in the stream.
    AVRational sample_aspect_ratio = pCodecCtx->sample_aspect_ratio;

    double duration = (double) pFormatCtx->duration / AV_TIME_BASE; // can be unknown & can be incorrect (e.g. .vob files)
    if (duration <= 0) {
        duration = guess_duration(pFormatCtx, video_index, pCodecCtx, pFrame);
        // have to turn timestamping off because it'll be incorrect
        if (1 == parameters.gb_t_timestamp) { // on
            t_timestamp = 0;
            av_log(NULL, AV_LOG_ERROR, "  turning time stamp off because of duration\n");
        }
    }
    if (duration <= 0) {
        av_log(NULL, AV_LOG_ERROR, "  duration is unknown: %.2f\n", duration);
        goto cleanup;
    }
    int64_t duration_tb = duration / av_q2d(pStream->time_base); // in time_base unit
    double start_time = (double) pFormatCtx->start_time / AV_TIME_BASE; // in seconds
    // VTS_01_2.VOB & beyond from DVD seem to be like this
    //if (start_time > duration) {
        //av_log(NULL, AV_LOG_VERBOSE, "  start_time: %.2f is more than duration: %.2f\n", start_time, duration);
        //goto cleanup;
    //}
    // if start_time is negative, we ignore it; FIXME: is this ok?
    if (start_time < 0) {
        start_time = 0;
    }
    int64_t start_time_tb = start_time * pStream->time_base.den / pStream->time_base.num; // in time_base unit
    //av_log(NULL, AV_LOG_ERROR, "  start_time_tb: %"PRId64"\n", start_time_tb);

    // decode the first frame without seeking.
    // without doing this, avcodec_decode_video wont be able to decode any picture
    // with some files, eg. http://download.pocketmovies.net/movies/3d/twittwit_320x184.mpg
    // bug reported by: swmaherl, jake_o from sourceforge
    // and pCodecCtx->width and pCodecCtx->height might not be correct without this
    // for .flv files. bug reported by: dragonbook 
    int64_t found_pts = -1;
    int64_t first_pts = -1; // pts of first frame
    ret = read_and_decode(pFormatCtx, video_index, pCodecCtx, pFrame, &first_pts, 0, 1);
    if (0 == ret) { // end of file
        goto eof;
    } else if (ret < 0) { // error
        av_log(NULL, AV_LOG_ERROR, "  read_and_decode first failed!\n");
        goto cleanup;
    }
    //av_log(NULL, LOG_INFO, "first_pts: %"PRId64" (%.2f s)\n", first_pts, calc_time(first_pts, pStream->time_base, start_time)); // DEBUG

    // set sample_aspect_ratio
    // assuming sample_y = display_y
    if (parameters.gb_a_ratio.num != 0) { // use cmd line arg if specified
        sample_aspect_ratio.num = (double) pCodecCtx->height * av_q2d(parameters.gb_a_ratio) / pCodecCtx->width * 10000;
        sample_aspect_ratio.den = 10000;
        av_log(NULL, LOG_INFO, "  *** using sample_aspect_ratio: %d/%d because of -a %.4f option\n", sample_aspect_ratio.num, sample_aspect_ratio.den, av_q2d(parameters.gb_a_ratio));
    } else {
        if (sample_aspect_ratio.num != 0 && pCodecCtx->sample_aspect_ratio.num != 0
            && av_q2d(sample_aspect_ratio) != av_q2d(pCodecCtx->sample_aspect_ratio)) {
            av_log(NULL, LOG_INFO, "  *** conflicting sample_aspect_ratio: %.2f vs %.2f: using %.2f\n",
                av_q2d(sample_aspect_ratio), av_q2d(pCodecCtx->sample_aspect_ratio), av_q2d(sample_aspect_ratio));
            av_log(NULL, LOG_INFO, "      to use sample_aspect_ratio %.2f use: -a %.4f option\n",
                av_q2d(pCodecCtx->sample_aspect_ratio), av_q2d(pCodecCtx->sample_aspect_ratio) * pCodecCtx->width / pCodecCtx->height);
            // we'll continue with existing value. is this ok? FIXME
            // this is the same as mpc's and vlc's. 
        }
        if (sample_aspect_ratio.num == 0) { // not defined
            sample_aspect_ratio = pCodecCtx->sample_aspect_ratio;
        }
    }

    /* calc options */
    // FIXME: make sure values are ok when movies are very short or very small
    double net_duration;
    if (parameters.gb_C_cut > 0) {
        net_duration = parameters.gb_C_cut;
        if (net_duration + parameters.gb_B_begin > duration) {
            net_duration = duration - parameters.gb_B_begin;
            av_log(NULL, AV_LOG_ERROR, "  -C %.2f s is too long, using %.2f s.\n", parameters.gb_C_cut, net_duration);
        }
    } else {
        //double net_duration = duration - start_time - gb_B_begin - gb_E_end;
        net_duration = duration - parameters.gb_B_begin - parameters.gb_E_end; // DVD
        if (net_duration <= 0) {
            av_log(NULL, AV_LOG_ERROR, "  duration: %.2f s, net duration after -B & -E is negative: %.2f s.\n", duration, net_duration);
            goto cleanup;
        }
    }

    /* scale according to sample_aspect_ratio. */
    int scaled_src_width, scaled_src_height;
    calc_scale_src(pCodecCtx->width, pCodecCtx->height, sample_aspect_ratio,
        &scaled_src_width, &scaled_src_height);
    if (scaled_src_width != pCodecCtx->width || scaled_src_height != pCodecCtx->height) {
        av_log(NULL, LOG_INFO, "  * scaling input * %dx%d => %dx%d according to sample_aspect_ratio %d/%d\n", 
            pCodecCtx->width, pCodecCtx->height, scaled_src_width, scaled_src_height, 
            sample_aspect_ratio.num, sample_aspect_ratio.den);
    }

    tn.column = parameters.gb_c_column + 1; // will be -1 in the loop
    int seek_mode = 1; // 1 = seek; 0 = non-seek
    tn.step = -99999; // seconds
    tn.row = -99999;
    tn.width = -99999;
    tn.shot_width = -99999;
    tn.shot_height = -99999;

    // reduce # of column until we meet minimum height except when movie is too small
    while (tn.shot_height < parameters.gb_h_height && tn.column > 0 && tn.shot_width != scaled_src_width) {
        tn.column--;
        if (parameters.gb_s_step == 0) { // step evenly to get column x row
            tn.step = net_duration / (tn.column * parameters.gb_r_row + 1);
        } else {
            tn.step = parameters.gb_s_step;
        }
        if (parameters.gb_r_row > 0) {
            tn.row = parameters.gb_r_row;
            // if # of columns is reduced, we should increase # of rows so # of tiles would be almost the same
            // could some users not want this?
        } else { // as many rows as needed
            tn.row = floor(net_duration / tn.column / tn.step + 0.5); // round nearest
        }
        if (tn.row < 1) {
            tn.row = 1;
        }

        // make sure last row is full
        tn.step = net_duration / (tn.column * tn.row + 1);

        int full_width = tn.column * (scaled_src_width + parameters.gb_g_gap) + parameters.gb_g_gap;
        if (parameters.gb_w_width > 0 && parameters.gb_w_width < full_width) {
            tn.width = parameters.gb_w_width;
        } else {
            tn.width = full_width;
        }
        tn.shot_width = floor((tn.width - parameters.gb_g_gap*(tn.column+1)) / (double)tn.column + 0.5); // round nearest
        tn.shot_width -= tn.shot_width%2; // floor to even number
        tn.shot_height = floor((double) scaled_src_height / scaled_src_width * tn.shot_width + 0.5); // round nearest
        tn.shot_height -= tn.shot_height%2; // floor to even number
        tn.center_gap = (tn.width - parameters.gb_g_gap*(tn.column+1) - tn.shot_width * tn.column) / 2.0;
    }
    if (tn.step == 0) {
        av_log(NULL, AV_LOG_ERROR, "  step is zero; movie is too short?\n");
        goto cleanup;
    }
    if (tn.column != parameters.gb_c_column) {
        av_log(NULL, LOG_INFO, "  changing # of column to %d to meet minimum height of %d; see -h option\n", tn.column, parameters.gb_h_height);
    }
    if (parameters.gb_w_width > 0 && parameters.gb_w_width != tn.width) {
        av_log(NULL, LOG_INFO, "  changing width to %d to match movie's size (%dx%d)\n", tn.width, scaled_src_width, tn.column);
    }
    char *all_text = get_stream_info(pFormatCtx, file, 1, sample_aspect_ratio); // FIXME: using function's static buffer
    if (NULL != info_fp) {
        fprintf(info_fp, all_text);
    }
    if (0 == parameters.gb_i_info) { // off
        *all_text = '\0';
    }
    if (NULL != parameters.gb_T_text) {
        all_text = strcat(all_text, parameters.gb_T_text);
        if (NULL != info_fp) {
            fprintf(info_fp, "%s%s", parameters.gb_T_text, NEWLINE);
        }
    }
    tn.txt_height = image_string_height(all_text, parameters.gb_f_fontname,parameters. gb_F_info_font_size) + parameters.gb_g_gap;
    tn.height = tn.shot_height*tn.row + parameters.gb_g_gap*(tn.row+1) + tn.txt_height;
    av_log(NULL, LOG_INFO, "  step: %d s; # tiles: %dx%d, tile size: %dx%d; total size: %dx%d\n", 
        tn.step, tn.column, tn.row, tn.shot_width, tn.shot_height, tn.width, tn.height);

    // jpeg seems to have max size of 65500 pixels
    if (tn.width > 65500 || tn.height > 65500) {
        av_log(NULL, AV_LOG_ERROR, "  jpeg only supports max size of 65500\n");
        goto cleanup;
    }

    int evade_step = MIN(10, tn.step / 14); // seconds to evade blank screen ; max 10 s
    // FIXME: what's the min value? 1?
    if (evade_step <= 0) {
        av_log(NULL, LOG_INFO, "  step is less than 14 s; blank & blur evasion is turned off.\n");
    }

    /* prepare for resize & conversion to PIX_FMT_RGB24 */
    pFrameRGB = avcodec_alloc_frame();
    if (pFrameRGB == NULL) {
        av_log(NULL, AV_LOG_ERROR, "  couldn't allocate a video frame\n");
        goto cleanup;
    }
    int rgb_bufsize = avpicture_get_size(PIX_FMT_RGB24, tn.shot_width, tn.shot_height);
    rgb_buffer = av_malloc(rgb_bufsize);
    if (NULL == rgb_buffer) {
        av_log(NULL, AV_LOG_ERROR, "  av_malloc %d bytes failed\n", rgb_bufsize);
        goto cleanup;
    }
    avpicture_fill((AVPicture *) pFrameRGB, rgb_buffer, PIX_FMT_RGB24,
        tn.shot_width, tn.shot_height);
    pSwsCtx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt,
        tn.shot_width, tn.shot_height, PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);
    if (NULL == pSwsCtx) { // sws_getContext is not documented
        av_log(NULL, AV_LOG_ERROR, "  sws_getContext failed\n");
        goto cleanup;
    }

    /* create the output image */
    tn.out_ip = gdImageCreateTrueColor(tn.width, tn.height);
    if (NULL == tn.out_ip) {
        av_log(NULL, AV_LOG_ERROR, "  gdImageCreateTrueColor failed: width %d, height %d\n", tn.width, tn.height);
        goto cleanup;
    }
    int background = gdImageColorResolve(tn.out_ip, parameters.gb_k_bcolor.r, parameters.gb_k_bcolor.g, parameters.gb_k_bcolor.b); // set backgroud
    gdImageFilledRectangle(tn.out_ip, 0, 0, tn.width, tn.height, background);

    /* add info & text */ // do this early so when font is not found we'll quit early
    if (NULL != all_text && strlen(all_text) > 0) {
        char *str_ret = image_string(tn.out_ip, 
            parameters.gb_f_fontname, parameters.gb_F_info_color, parameters.gb_F_info_font_size,
            parameters.gb_L_info_location, parameters.gb_g_gap, all_text, 0, COLOR_WHITE);
        if (NULL != str_ret) {
            av_log(NULL, AV_LOG_ERROR, "  %s; font problem? see -f option\n", str_ret);
            goto cleanup;
        }
    }

    /* alloc dynamic thumb data */
    if (-1 == thumb_alloc_dynamic(&tn)) {
        av_log(NULL, AV_LOG_ERROR, "  thumb_alloc_dynamic failed\n");
        goto cleanup;
    }

    if (1 == parameters.gb_z_seek) {
        seek_mode = 1;
    }
    if (1 == parameters.gb_Z_nonseek) {
        seek_mode = 0;
        av_log(NULL, LOG_INFO, "  *** using non-seek mode -- slower but more accurate timing.\n");
    }

  restart: ;
    /* decode & fill in the shots */
    if (0 == seek_mode && parameters.gb_B_begin > 10) {
        av_log(NULL, LOG_INFO, "  -B %.2f with non-seek mode will take some time.\n", parameters.gb_B_begin);
    }

    int64_t seek_target, seek_evade = 0; // in time_base unit
    int evade_try = 0; // blank screen evasion index
    double avg_evade_try = 0; // average
    int direction = 0; // seek direction (seek flags)
    seek_target = (tn.step + start_time + parameters.gb_B_begin) / av_q2d(pStream->time_base);
    int idx = 0; // idx = thumb_idx
    int thumb_nb = tn.row * tn.column; // thumb_nb = # of shots we need
    int64_t prevshot_pts = -1; // pts of previous good shot
    int64_t prevfound_pts = -1; // pts of previous decoding
    gdImagePtr edge_ip = NULL; // edge image

    for (idx = 0; idx < thumb_nb; idx++) {

        int64_t eff_target = seek_target + seek_evade; // effective target
        eff_target = MAX(eff_target, start_time_tb); // make sure eff_target > start_time
        char time_tmp[15]; // FIXME
        format_time(calc_time(eff_target, pStream->time_base, start_time), time_tmp, ':');

        /* for some formats, previous seek might over shoot pass this seek_target; is this a bug in libavcodec? */
        if (prevshot_pts > eff_target && 0 == evade_try) {
            av_log(NULL, LOG_INFO, "  skipping shot at %s because of previous seek or evasions\n", time_tmp);
            idx--;
            thumb_nb--;
            goto skip_shot;
        }

        // make sure eff_target > previous found
        eff_target = MAX(eff_target, prevfound_pts+1);
        if (eff_target > duration_tb) { // end of file
            goto eof;
        }
        format_time(calc_time(eff_target, pStream->time_base, start_time), time_tmp, ':');
        //av_log(NULL, AV_LOG_VERBOSE, "\n***eff_target tb: %"PRId64", eff_target s:%.2f (%s), prevshot_pts: %"PRId64"\n",
        //    eff_target, calc_time(eff_target, pStream->time_base, start_time), time_tmp, prevshot_pts);

        /* jump to next shot */
        //struct timeval dstart; // DEBUG
        //gettimeofday(&dstart, NULL); // calendar time; effected by load & io & etc. DEBUG
        if (1 == seek_mode) { // seek mode
            ret = really_seek(pFormatCtx, video_index, eff_target, direction, duration);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "  seeking to to %.2f s failed\n", calc_time(eff_target, pStream->time_base, start_time));
                goto cleanup;
            }
            avcodec_flush_buffers(pCodecCtx);

            ret = read_and_decode(pFormatCtx, video_index, pCodecCtx, pFrame, &found_pts, 1, 0);
            if (0 == ret) { // end of file
                goto eof;
            } else if (ret < 0) { // error
                av_log(NULL, AV_LOG_ERROR, "  read_and_decode failed!\n");
                goto cleanup;
            }
            //av_log(NULL, LOG_INFO, "  found_pts: %"PRId64", eff_target: %"PRId64"\n", found_pts, eff_target); // DEBUG
        } else { // non-seek mode -- we keep decoding until we get to the next shot
            found_pts = 0;
            while (found_pts < eff_target) {
                // we should check if it's taking too long for this loop. FIXME
                ret = read_and_decode(pFormatCtx, video_index, pCodecCtx, pFrame, &found_pts, 0, 0);
                if (0 == ret) { // end of file
                    goto eof;
                } else if (ret < 0) { // error
                    av_log(NULL, AV_LOG_ERROR, "  read_and_decode failed!\n");
                    goto cleanup;
                }
                //av_log(NULL, LOG_INFO, "  found_pts: %"PRId64", eff_target: %"PRId64"\n", found_pts, eff_target); // DEBUG
            }
        }
        //struct timeval dfinish; // DEBUG
        //gettimeofday(&dfinish, NULL); // calendar time; effected by load & io & etc. DEBUG
        //double decode_time = (dfinish.tv_sec + dfinish.tv_usec/1000000.0) - (dstart.tv_sec + dstart.tv_usec/1000000.0);
        double decode_time = 0;

        double found_diff = (found_pts - eff_target) * av_q2d(pStream->time_base);
        //av_log(NULL, LOG_INFO, "  found_diff: %.2f\n", found_diff); // DEBUG
        // if found frame is too far off from target, we'll disable seeking and start over
        if (idx < 5 && 1 == seek_mode && 0 == parameters.gb_z_seek
            // usually movies have key frames every 10 s
            && (tn.step < 15 || found_diff > 15)
            && (found_diff <= -tn.step || found_diff >= tn.step)) {
            
            // compute the approx. time it take for the non-seek mode, if too long print a msg instead
            double shot_dtime;
            if (scaled_src_width > 576*4/3.0) { // HD
                shot_dtime = tn.step * 30 / 30.0;
            } else if (scaled_src_width > 288*4/3.0) { // ~DVD
                shot_dtime = tn.step * 30 / 80.0;
            } else { // small
                shot_dtime = tn.step * 30 / 500.0;
            }
            if (shot_dtime > 2 || shot_dtime * tn.column * tn.row > 120) {
                av_log(NULL, LOG_INFO, "  *** seeking off target %.2f s, increase time step or use non-seek mode.\n", found_diff);
                goto non_seek_too_long;
            }

            // disable seeking and start over
            av_seek_frame(pFormatCtx, video_index, 0, 0);
            avcodec_flush_buffers(pCodecCtx);
            seek_mode = 0;
            av_log(NULL, LOG_INFO, "  *** switching to non-seek mode because seeking was off target by %.2f s.\n", found_diff);
            av_log(NULL, LOG_INFO, "  non-seek mode is slower. increase time step or use -z if you dont want this.\n");
            goto restart;
        }
      non_seek_too_long:

        nb_shots++;
        //av_log(NULL, AV_LOG_VERBOSE, "shot %d: found_: %"PRId64" (%.2fs), eff_: %"PRId64" (%.2fs), dtime: %.3f\n",
        //    idx, found_pts, calc_time(found_pts, pStream->time_base, start_time),
        //    eff_target, calc_time(eff_target, pStream->time_base, start_time), decode_time);
        av_log(NULL, AV_LOG_VERBOSE, "approx. decoded frames/s %.2f\n", tn.step * 30 / decode_time); // DEBUG
        /*
        char debug_filename[2048]; // DEBUG
        sprintf(debug_filename, "%s_decoded%05d.jpg", tn.out_filename, nb_shots - 1);
        save_AVFrame(pFrame, pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt, 
            debug_filename, pCodecCtx->width, pCodecCtx->height);
        */

        /* doesn't work very well -- tends to make evade_step too large
        if (evade_step > 0) {
            // this shot is the same as previous seek -- not good.
            if (found_pts == prevfound_pts) {
                // maybe evade_step is too small
                if (evade_step < 20) { // FIXME
                    evade_step *= 1.2; // FIXME
                }
            }
            // found diffs from target > evade_step
            if (found_diff <= -evade_step || found_diff >= evade_step) {
                // maybe evade_step is too small
                if (evade_step < 20 && found_diff < 20) { // FIXME
                    //evade_step = 1 + ceiling(found_diff); // FIXME
                }
            }
        }
        */

        // got same picture as previous shot, we'll skip it
        if (prevshot_pts == found_pts && 0 == evade_try) {
            av_log(NULL, LOG_INFO, "  skipping shot at %s because got previous shot\n", time_tmp);
            idx--;
            thumb_nb--;
            goto skip_shot;
        }

        /* convert to PIX_FMT_RGB24 & resize */
        sws_scale(pSwsCtx, (const uint8_t * const*)pFrame->data, pFrame->linesize, 0, pCodecCtx->height, 
            pFrameRGB->data, pFrameRGB->linesize);
        /*
        sprintf(debug_filename, "%s_resized%05d.jpg", tn.out_filename, nb_shots - 1); // DEBUG
        save_AVFrame(pFrameRGB, tn.shot_width, tn.shot_height, PIX_FMT_RGB24, 
            debug_filename, tn.shot_width, tn.shot_height);
        */

        /* if blank screen, try again */
        // FIXME: make sure this'll work when step is small
        // FIXME: make sure each shot wont get repeated
        double blank = blank_frame(pFrameRGB, tn.shot_width, tn.shot_height);
        // only do edge when blank detection doesn't work
        float edge[EDGE_PARTS] = {1,1,1,1,1,1}; // FIXME: change this if EDGE_PARTS is changed
        if (evade_step > 0 && blank <= parameters.gb_b_blank && parameters.gb_D_edge > 0) {
            edge_ip = detect_edge(pFrameRGB, tn.shot_width, tn.shot_height, edge, EDGE_FOUND);
        }
        //av_log(NULL, AV_LOG_VERBOSE, "  idx: %d, evade_try: %d, blank: %.2f%s edge: %.3f %.3f %.3f %.3f %.3f %.3f%s\n", 
        //    idx, evade_try, blank, (blank > gb_b_blank) ? "**b**" : "", 
        //    edge[0], edge[1], edge[2], edge[3], edge[4], edge[5], is_edge(edge, EDGE_FOUND) ? "" : "**e**"); // DEBUG
        if (evade_step > 0 && (blank > parameters.gb_b_blank || !is_edge(edge, EDGE_FOUND))) {
            idx--;
            evade_try++;
            // we'll always search forward to support non-seek mode, which cant go backward
            // keep trying until getting close to next step
            seek_evade = evade_step * evade_try / av_q2d(pStream->time_base);
            if (seek_evade < (tn.step - evade_step) / av_q2d(pStream->time_base)) { // FIXME
                //av_log(NULL, AV_LOG_VERBOSE, "  * blank or no edge * try #%d: seeking forward seek_evade: %"PRId64" (%.2f s)\n",
                //    evade_try, seek_evade, seek_evade * av_q2d(pStream->time_base));
                goto continue_cleanup;
            }

            // not found -- skip shot
            char time_tmp[15]; // FIXME
            format_time(calc_time(seek_target, pStream->time_base, start_time), time_tmp, ':');
            av_log(NULL, LOG_INFO, "  * blank %.2f or no edge * skipping shot at %s after %d tries\n", blank, time_tmp, evade_try);
            thumb_nb--; // reduce # shots
            goto skip_shot;
        }

        //
        avg_evade_try = (avg_evade_try * idx + evade_try ) / (idx+1); // DEBUG
        //av_log(NULL, AV_LOG_VERBOSE, "  *** avg_evade_try: %.2f\n", avg_evade_try); // DEBUG

        /* convert to GD image */
        ip = gdImageCreateTrueColor(tn.shot_width, tn.shot_height);
        if (NULL == ip) {
            av_log(NULL, AV_LOG_ERROR, "  gdImageCreateTrueColor failed: width %d, height %d\n", tn.shot_width, tn.shot_height);
            goto cleanup;
        }
        FrameRGB_2_gdImage(pFrameRGB, ip, tn.shot_width, tn.shot_height);

        /* if debugging, save the edge instead */
        if (parameters.gb_v_verbose > 0 && NULL != edge_ip) {
            gdImageDestroy(ip);
            ip = edge_ip;
            edge_ip = NULL;
        }

        /* timestamping */
        // FIXME: this frame might not actually be at the requested position. is pts correct?
        if (1 == t_timestamp) { // on
            char time_str[15]; // FIXME
            format_time(calc_time(found_pts, pStream->time_base, start_time), time_str, ':');
            char *str_ret = image_string(ip, 
                parameters.gb_F_ts_fontname, parameters.gb_F_ts_color, parameters.gb_F_ts_font_size,
                parameters.gb_L_time_location, 0, time_str, 1, parameters.gb_F_ts_shadow);
            if (NULL != str_ret) {
                av_log(NULL, AV_LOG_ERROR, "  %s; font problem? see -f option or -F option\n", str_ret);
                goto cleanup; // LEAK: ip, edge_ip
            }
            /* stamp idx & blank & edge for debugging */
            if (parameters.gb_v_verbose > 0) {
                char idx_str[10]; // FIXME
                sprintf(idx_str, "idx: %d, blank: %.2f\n%.6f  %.6f\n%.6f  %.6f\n%.6f  %.6f", 
                    idx, blank, edge[0], edge[1], edge[2], edge[3], edge[4], edge[5]);
                image_string(ip, parameters.gb_f_fontname, COLOR_WHITE, parameters.gb_F_ts_font_size, 2, 0, idx_str, 1, COLOR_BLACK);
            }
        }

        /* add picture to output image */
        thumb_add_shot(&tn, ip, idx, found_pts);
        gdImageDestroy(ip);
        ip = NULL;

      skip_shot:
        /* step */
        seek_target += tn.step / av_q2d(pStream->time_base);
        
        seek_evade = 0;
        direction = 0;
        evade_try = 0;
        prevshot_pts = found_pts;
        //av_log(NULL, AV_LOG_VERBOSE, "found_pts bottom: %"PRId64"\n", found_pts);
    
      continue_cleanup: // cleaning up before continuing the loop
        prevfound_pts = found_pts;
        if (NULL != edge_ip) {
            gdImageDestroy(edge_ip);
            edge_ip = NULL;
        }
    }
    av_log(NULL, AV_LOG_VERBOSE, "  *** avg_evade_try: %.2f\n", avg_evade_try); // DEBUG

  eof: ;
    /* crop if we dont get enough shots */
    int skipped_rows = tn.row - ceil((double)idx / tn.column);
    if (skipped_rows == tn.row) {
        av_log(NULL, AV_LOG_ERROR, "  all rows're skipped?\n");
        goto cleanup;
    }
    if (0 != skipped_rows) {
        int cropped_height = tn.height - skipped_rows*tn.shot_height;
        gdImagePtr new_out_ip = crop_image(tn.out_ip, tn.width, cropped_height);
        if (new_out_ip != tn.out_ip) {
            tn.out_ip = new_out_ip;
            av_log(NULL, LOG_INFO, "  changing # of tiles to %dx%d because of skipped shots; total size: %dx%d\n", tn.column, tn.row - skipped_rows, tn.width, cropped_height);
        }
    }

    /* fill in the last row if some shots were skipped */

    /* save output image */
    errno = 0;

    if(parameters.gb_O_format == 0)
        gdImageJpeg(tn.out_ip, out_fp, parameters.gb_j_quality);  /* FIXME: how to check if write was successful? */
    //else if (parameters.gb_O_format == 1)
        //gdImageBmp(tn.out_ip, out_fp);
    else if (parameters.gb_O_format == 2)
        gdImagePng(tn.out_ip, out_fp);
    //else if (parameters.gb_O_format == 3)
        //gdImageTiff(tn.out_ip, out_fp);

    if (0 != errno) { // FIXME: this should work?
        av_log(NULL, AV_LOG_ERROR, "  saving output image failed: %s\n", strerror(errno));
        goto cleanup;
    }
    tn.out_saved = 1;

    struct timeval tfinish;
    gettimeofday(&tfinish, NULL); // calendar time; effected by load & io & etc.
    double diff_time = (tfinish.tv_sec + tfinish.tv_usec/1000000.0) - (tstart.tv_sec + tstart.tv_usec/1000000.0);
    // previous version reported # of decoded shots/s; now we report the # of final shots/s
    //av_log(NULL, LOG_INFO, "  avg. %.2f shots/s; output file: %s\n", nb_shots / diff_time, tn.out_filename);
    av_log(NULL, LOG_INFO, "  %.2f s, %.2f shots/s; output: %s\n", 
        diff_time, (tn.idx + 1) / diff_time, tn.out_filename);

  cleanup:
    if (NULL != ip)
        gdImageDestroy(ip);
    if (NULL != tn.out_ip)
        gdImageDestroy(tn.out_ip);

    if (NULL != out_fp) {
        fclose(out_fp);
        if (1 != tn.out_saved) {
            _tunlink(out_filename_w);
        }
    }
    if (NULL != info_fp) {
        fclose(info_fp);
        if (1 != tn.out_saved) {
            _tunlink(info_filename_w);
        }
    }

    if (NULL != pSwsCtx)
        sws_freeContext(pSwsCtx); // do we need to do this?

    // Free the video frame
    if (NULL != rgb_buffer)
        av_free(rgb_buffer);
    if (NULL != pFrameRGB)
        av_free(pFrameRGB);
    if (NULL != pFrame)
        av_free(pFrame);

    av_dict_free(&pDict);
 
    // Close the codec
    if (NULL != pCodecCtx && NULL != pCodecCtx->codec) {
        avcodec_close(pCodecCtx);
    }

    // Close the video file
    if (NULL != pFormatCtx)
        avformat_close_input(&pFormatCtx);

    thumb_cleanup_dynamic(&tn);
    
    av_log(NULL, AV_LOG_VERBOSE, "make_thumbnail: done\n");
}

// copied & modified from mingw-runtime-3.13's init.c
typedef struct {
  int newmode;
} _startupinfo;
extern void __wgetmainargs (int *, wchar_t ***, wchar_t ***, int, _startupinfo *);

char *gb_argv[10240]; // FIXME: global & hopefully noone will use more than this

void usage()
{
    av_log(NULL, AV_LOG_ERROR, "\nMovie thumbnailer (mtn) %s\n", gb_version);
    av_log(NULL, AV_LOG_ERROR, "Compiled with : %s %s %s %s GD%s\n\n", LIBAVCODEC_IDENT, LIBAVFORMAT_IDENT, LIBAVUTIL_IDENT, LIBSWSCALE_IDENT, GD_VERSION_STRING);
    av_log(NULL, AV_LOG_ERROR, "Description:\n");
    av_log(NULL, AV_LOG_ERROR, "  Mtn saves thumbnails of specified movie files or directories to jpeg files.\n");
    av_log(NULL, AV_LOG_ERROR, "  For directories, it will recursively search inside for movie files.\n\n");
    av_log(NULL, AV_LOG_ERROR, "Usage:\n  %s [options] file_or_dir1 [file_or_dir2] ... [file_or_dirn]\n\n", parameters.gb_argv0);
    av_log(NULL, AV_LOG_ERROR, "Options: (and default values)\n");
    av_log(NULL, AV_LOG_ERROR, "  -a aspect_ratio : override input file's display aspect ratio\n");
    av_log(NULL, AV_LOG_ERROR, "  -b %.2f : skip if %% blank is higher; 0:skip all 1:skip really blank >1:off\n", GB_B_BLANK);
    av_log(NULL, AV_LOG_ERROR, "  -B %.1f : omit this seconds from the beginning\n", GB_B_BEGIN);
    av_log(NULL, AV_LOG_ERROR, "  -c %d : # of column\n", GB_C_COLUMN);
    av_log(NULL, AV_LOG_ERROR, "  -C %d : cut movie and thumbnails not more than the specified seconds; <=0:off\n", GB_C_CUT);
    //av_log(NULL, AV_LOG_ERROR, "  -d : this option shouldn't be needed anymore\n");
    av_log(NULL, AV_LOG_ERROR, "  -D %d : edge detection; 0:off >0:on; higher detects more; try -D4 -D6 or -D8\n", parameters.gb_D_edge);
    //av_log(NULL, AV_LOG_ERROR, "  -e : to be done\n"); // extension of movie files
    av_log(NULL, AV_LOG_ERROR, "  -E %.1f : omit this seconds at the end\n", GB_E_END);
    av_log(NULL, AV_LOG_ERROR, "  -f %s : font file; use absolute path if not in usual places\n", GB_F_FONTNAME);
    av_log(NULL, AV_LOG_ERROR, "  -F RRGGBB:size[:font:RRGGBB:RRGGBB:size] : font format [time is optional]\n     info_color:info_size[:time_font:time_color:time_shadow:time_size]\n");
    av_log(NULL, AV_LOG_ERROR, "  -g %d : gap between each shot\n", GB_G_GAP);
    av_log(NULL, AV_LOG_ERROR, "  -h %d : minimum height of each shot; will reduce # of column to fit\n", GB_H_HEIGHT);
    av_log(NULL, AV_LOG_ERROR, "  -i : info text off\n");
    av_log(NULL, AV_LOG_ERROR, "  -I : save individual shots too\n");
    av_log(NULL, AV_LOG_ERROR, "  -j %d : jpeg quality\n", GB_J_QUALITY);
    av_log(NULL, AV_LOG_ERROR, "  -k RRGGBB : background color (in hex)\n"); // backgroud color
    av_log(NULL, AV_LOG_ERROR, "  -L info_location[:time_location] : location of text\n     1=lower left, 2=lower right, 3=upper right, 4=upper left\n");
    av_log(NULL, AV_LOG_ERROR, "  -n : run at normal priority\n");
    av_log(NULL, AV_LOG_ERROR, "  -N info_suffix : save info text to a file with suffix\n");
    av_log(NULL, AV_LOG_ERROR, "  -o %s : output suffix\n", GB_O_SUFFIX);
    av_log(NULL, AV_LOG_ERROR, "  -O directory : save output files in the specified directory\n");
    //av_log(NULL, AV_LOG_ERROR, "  -q : to be done\n"); // quiet mode
    av_log(NULL, AV_LOG_ERROR, "  -r %d : # of rows; >0:override -s\n", GB_R_ROW);
    av_log(NULL, AV_LOG_ERROR, "  -s %d : time step between each shot\n", GB_S_STEP);
    av_log(NULL, AV_LOG_ERROR, "  -t : time stamp off\n");
    av_log(NULL, AV_LOG_ERROR, "  -T text : add text above output image\n");
    av_log(NULL, AV_LOG_ERROR, "  -v : verbose mode (debug)\n");
    av_log(NULL, AV_LOG_ERROR, "  -w %d : width of output image; 0:column * movie width\n", GB_W_WIDTH);
    av_log(NULL, AV_LOG_ERROR, "  -W : dont overwrite existing files, i.e. update mode\n");
    av_log(NULL, AV_LOG_ERROR, "  -z : always use seek mode\n");
    av_log(NULL, AV_LOG_ERROR, "  -Z : always use non-seek mode -- slower but more accurate timing\n\n");
    av_log(NULL, AV_LOG_ERROR, "Examples:\n");
    av_log(NULL, AV_LOG_ERROR, "  to save thumbnails to file infile%s with default options:\n    %s infile.avi\n", GB_O_SUFFIX, parameters.gb_argv0);
    av_log(NULL, AV_LOG_ERROR, "  to change time step to 65 seconds & change total width to 900:\n    %s -s 65 -w 900 infile.avi\n", parameters.gb_argv0);
    // as of version 0.60, -s 0 is not needed
    av_log(NULL, AV_LOG_ERROR, "  to step evenly to get 3 columns x 10 rows:\n    %s -c 3 -r 10 infile.avi\n", parameters.gb_argv0);
    av_log(NULL, AV_LOG_ERROR, "  to save output files to writeable directory:\n    %s -O writeable /read/only/dir/infile.avi\n", parameters.gb_argv0);
    av_log(NULL, AV_LOG_ERROR, "  to get 2 columns in original movie size:\n    %s -c 2 -w 0 infile.avi\n", parameters.gb_argv0);
    av_log(NULL, AV_LOG_ERROR, "  to skip uninteresting shots, try:\n    %s -D 6 infile.avi\n", parameters.gb_argv0);
#ifdef WIN32
    av_log(NULL, AV_LOG_ERROR, "\nIn windows, you can run %s from command prompt or drag files/dirs from\n", parameters.gb_argv0);
    av_log(NULL, AV_LOG_ERROR, "windows explorer and drop them on %s. you can change the default options\n", parameters.gb_argv0);
    av_log(NULL, AV_LOG_ERROR, "by creating a shortcut to %s and add options there (right click the\n", parameters.gb_argv0);
    av_log(NULL, AV_LOG_ERROR, "shortcut -> Properties -> Target); then drop files/dirs on the shortcut\n");
    av_log(NULL, AV_LOG_ERROR, "instead.\n");
#else
    av_log(NULL, AV_LOG_ERROR, "\nYou'll probably need to change the truetype font path (-f fontfile).\n");
    av_log(NULL, AV_LOG_ERROR, "the default is set to %s which might not exist in non-windows\n", GB_F_FONTNAME);
    av_log(NULL, AV_LOG_ERROR, "systems. if you dont have a truetype font, you can turn the text off by\n");
    av_log(NULL, AV_LOG_ERROR, "using -i -t.\n");
#endif
    av_log(NULL, AV_LOG_ERROR, "\nMtn comes with ABSOLUTELY NO WARRANTY. this is free software, and you are\n");
    av_log(NULL, AV_LOG_ERROR, "welcome to redistribute it under certain conditions; for details see file\n");
    av_log(NULL, AV_LOG_ERROR, "gpl-2.0.txt.\n");
}

void my_log_callback(void *ptr, int level, const char *fmt, va_list vl)
{
    va_list vl2;
    char line[1024];
    static int print_prefix = 1;

    va_copy(vl2, vl);
    av_log_default_callback(ptr, level, fmt, vl);
    av_log_format_line(ptr, level, fmt, vl2, line, sizeof(line), &print_prefix);
    va_end(vl2);
    sprintf(logs+strlen(logs),line);
}

void process_file()
{
    memset(logs,'\0',sizeof logs);

    setvbuf(stderr, NULL, _IONBF, 0); // turn off buffering in mingw

    // set locale
    __attribute__((unused)) char *locale = setlocale(LC_ALL, "");

    /* init */
    av_register_all();
    // Register all formats and codecs
    av_log_set_level(LOG_INFO);
    // Log Callback
    av_log_set_callback(my_log_callback);
    /* process movie files */
    make_thumbnail(parameters.gb_argv0);
}
