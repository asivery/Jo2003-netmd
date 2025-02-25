/* netmdcli.c
 *      Copyright (C) 2017 René Rebe
 *      Copyright (C) 2002, 2003 Marc Britten
 *
 * This file is part of libnetmd.
 *
 * libnetmd is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Libnetmd is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#include <stdio.h>
#include <gcrypt.h>
#include <getopt.h>
#include <json-c/json.h>
#include <json-c/json_object.h>
#include <ctype.h>
#include "netmdcli.h"

/** @brief audio patch type */
typedef enum
{
    apt_no_patch, /**< no patch needed         */
    apt_wave,     /**< wave endianess patch    */
    apt_sp        /**< atrac1 SP padding patch */
} audio_patch_t;

#define NO_ONTHEFLY_CONVERSION 0xf

static json_object *json;
static FILE* json_fd = NULL;

void print_json_disc_info_gui(netmd_device* dev,  netmd_dev_handle* devh, HndMdHdr md);
void print_json_disc_info(netmd_device* dev,  netmd_dev_handle* devh, HndMdHdr md, int shortJson);
void print_disc_info(netmd_dev_handle* devh, HndMdHdr md);
void print_current_track_info(netmd_dev_handle* devh);
void print_syntax();
void import_m3u_playlist(netmd_dev_handle* devh, const char *file);
netmd_error send_track(netmd_dev_handle *devh, const char *filename, const char *in_title, unsigned char onTheFlyConvert);

/* Max line length we support in M3U files... should match MD TOC max */
#define M3U_LINE_MAX	128

/* Min "usable" audio file size (1 frame Atrac LP4)
   = 52 (RIFF/WAVE header Atrac LP) + 8 ("data" + length) + 92 (1 frame LP4) */
#define MIN_WAV_LENGTH 152

#if 0
static void handle_secure_cmd(netmd_dev_handle* devh, int cmdid, int track)
{
    unsigned int player_id;
    unsigned char ekb_head[] = {
        0x01, 0xca, 0xbe, 0x07, 0x2c, 0x4d, 0xa7, 0xae,
        0xf3, 0x6c, 0x8d, 0x73, 0xfa, 0x60, 0x2b, 0xd1};
    unsigned char ekb_body[] = {
        0x0f, 0xf4, 0x7d, 0x45, 0x9c, 0x72, 0xda, 0x81,
        0x85, 0x16, 0x9d, 0x73, 0x49, 0x00, 0xff, 0x6c,
        0x6a, 0xb9, 0x61, 0x6b, 0x03, 0x04, 0xf9, 0xce};
    unsigned char rand_in[8], rand_out[8];
    unsigned char hash8[8];
    unsigned char hash32[32];

    switch (cmdid) {
    case 0x11:
        if (netmd_secure_cmd_11(devh, &player_id) > 0) {
            fprintf(stdout, "Player id = %04d\n", player_id);
        }
        break;
    case 0x12:
        netmd_secure_cmd_12(devh, ekb_head, ekb_body);
        break;
    case 0x20:
        memset(rand_in, 0, sizeof(rand_in));
        if (netmd_secure_cmd_20(devh, rand_in, rand_out) > 0) {
            fprintf(stdout, "Random =\n");
            print_hex(rand_out, sizeof(rand_out));
        }
        break;
    case 0x21:
        netmd_secure_cmd_21(devh);
        break;
    case 0x22:
        memset(hash32, 0, sizeof(hash32));
        netmd_secure_cmd_22(devh, hash32);
        break;
    case 0x23:
        if (netmd_secure_cmd_23(devh, track, hash8) > 0) {
            fprintf(stdout, "Hash id of track %d =\n", track);
            print_hex(hash8, sizeof(hash8));
        }
        break;*/
/*case 0x28: TODO*/
    case 0x40:
        if (netmd_secure_cmd_40(devh, track, hash8) > 0) {
            fprintf(stdout, "Signature of deleted track %d =\n", track);
            print_hex(hash8, sizeof(hash8));
        }
        break;
    case 0x48:
        memset(hash8, 0, sizeof(hash8));
        if (netmd_secure_cmd_48(devh, track, hash8) > 0) {
            fprintf(stdout, "Signature of downloaded track %d =\n", track);
            print_hex(hash8, sizeof(hash8));
        }
        break;
    case 0x80:
        netmd_secure_cmd_80(devh);
        break;
    case 0x81:
        netmd_secure_cmd_81(devh);
        break;
    default:
        fprintf(stderr, "unsupported secure command\n");
        break;
    }
}
#endif

static void send_raw_message(netmd_dev_handle* devh, char *pszRaw)
{
    unsigned char cmd[255], rsp[255];
    unsigned int data;
    char szBuf[4];
    size_t cmdlen;
    int rsplen;

    /* check raw message length */
    if ((strlen(pszRaw) % 2) != 0) {
        printf("Error: invalid length of raw message!\n");
        return;
    }

    /* convert hex message to bin */
    cmdlen = 0;
    while (*pszRaw != 0) {
        szBuf[0] = *pszRaw++;
        szBuf[1] = *pszRaw++;
        szBuf[2] = '\0';
        if (sscanf(szBuf, "%02X", &data) != 1) {
            printf("Error: invalid character at byte %lu ('%s')\n", (unsigned long)cmdlen, szBuf);
            return;
        }
        cmd[cmdlen++] = data & 0xff;
    }

    /* send it */
    rsplen = netmd_exch_message(devh, cmd, cmdlen, rsp);
    if (rsplen < 0) {
        printf("Error: netmd_exch_message failed with %d\n", rsplen);
        return;
    }
}

struct json_object* json_time(const netmd_time *time)
{
    char buffer[12];
    sprintf(buffer, "%02d:%02d:%02d.%02d", (uint8_t)time->hour, time->minute, time->second, time->frame);
    return json_object_new_string(buffer);
}

void print_time(const netmd_time *time)
{
    printf("%02d:%02d:%02d.%02d", time->hour, time->minute, time->second, time->frame);
}

void retailmac(unsigned char *rootkey, unsigned char *hostnonce,
               unsigned char *devnonce, unsigned char *sessionkey)
{
    gcry_cipher_hd_t handle1;
    gcry_cipher_hd_t handle2;

    unsigned char des3_key[24] = { 0 };
    unsigned char iv[8] = { 0 };

    gcry_cipher_open(&handle1, GCRY_CIPHER_DES, GCRY_CIPHER_MODE_ECB, 0);
    gcry_cipher_setkey(handle1, rootkey, 8);
    gcry_cipher_encrypt(handle1, iv, 8, hostnonce, 8);

    memcpy(des3_key, rootkey, 16);
    memcpy(des3_key+16, rootkey, 8);
    gcry_cipher_open(&handle2, GCRY_CIPHER_3DES, GCRY_CIPHER_MODE_CBC, 0);
    gcry_cipher_setkey(handle2, des3_key, 24);
    gcry_cipher_setiv(handle2, iv, 8);
    gcry_cipher_encrypt(handle2, sessionkey, 8, devnonce, 8);

    gcry_cipher_close(handle1);
    gcry_cipher_close(handle2);
}

static inline unsigned int leword32(const unsigned char * c)
{
    return (unsigned int)((c[3] << 24U) + (c[2] << 16U) + (c[1] << 8U) + c[0]);
}

static inline unsigned int leword16(const unsigned char * c)
{
    return c[1]*256U+c[0];
}

static size_t wav_data_position(const unsigned char * data, size_t offset, size_t len)
{
    size_t i = offset, pos = 0;

    while (i < len - 4) {
        if(strncmp("data", (const char*)data+i, 4) == 0) {
            pos = i;
            break;
        }
        i += 2;
    }

    return pos;
}

static int audio_supported(const unsigned char * file, size_t fsize, netmd_wireformat * wireformat, unsigned char * diskformat, audio_patch_t * conversion, size_t * channels, size_t * headersize)
{
    if(strncmp("RIFF", (const char*)file, 4) != 0 || strncmp("WAVE", (const char*)file+8, 4) != 0 || strncmp("fmt ", (const char*)file+12, 4) != 0)
    {
        // no wave format, look for preencoded ATRAC1 (SP).
        // I know the test is vague!
        if ((file[1] == 8) && (fsize > 2048))
        {
            *channels   = (file[264] == 2) ? NETMD_CHANNELS_STEREO      : NETMD_CHANNELS_MONO;
            *diskformat = NETMD_DISKFORMAT_LP2;
            *wireformat = NETMD_WIREFORMAT_105KBPS;
            *headersize = 2048;
            *conversion = apt_sp;
            return 1;
        }
        else
            return 0;                                         /* no valid WAV file or fmt chunk missing*/
    }

    if(leword16(file+20) == 1)                                /* PCM */
    {
        *conversion = apt_wave;                               /* needs conversion (byte swapping) for pcm raw data from wav file*/
        *wireformat = NETMD_WIREFORMAT_PCM;
        if(leword32(file+24) != 44100)                        /* sample rate not 44k1*/
            return 0;
        if(leword16(file+34) != 16)                           /* bitrate not 16bit */
            return 0;
        if(leword16(file+22) == 2) {                          /* channels = 2, stereo */
            *channels = NETMD_CHANNELS_STEREO;
            *diskformat = NETMD_DISKFORMAT_SP_STEREO;
        }
        else if(leword16(file+22) == 1) {                     /* channels = 1, mono */
            *channels = NETMD_CHANNELS_MONO;
            *diskformat = NETMD_DISKFORMAT_SP_MONO;
        }
        else
            return 0;
        *headersize = 20 + leword32(file+16);
        return 1;
    }

    if(leword16(file +20) == NETMD_RIFF_FORMAT_TAG_ATRAC3)         /* ATRAC3 */
    {
        *conversion = apt_no_patch;                                /* conversion not needed */
        if(leword32(file+24) != 44100)                             /* sample rate */
            return 0;
        if(leword16(file+32) == NETMD_DATA_BLOCK_SIZE_LP2) {       /* data block size LP2 */
            *wireformat = NETMD_WIREFORMAT_LP2;
            *diskformat = NETMD_DISKFORMAT_LP2;
        }
        else if(leword16(file+32) == NETMD_DATA_BLOCK_SIZE_LP4) {  /* data block size LP4 */
            *wireformat = NETMD_WIREFORMAT_LP4;
            *diskformat = NETMD_DISKFORMAT_LP4;
        }
        else
            return 0;
        *headersize = 20 + leword32(file+16);
        *channels = NETMD_CHANNELS_STEREO;
        return 1;
    }
    return 0;
}

static int check_args(int argc, int min_argc, const char *text)
{
    /* n is the original argc, incl. program name */
    if (argc > min_argc) {
        return 1;
    }
    netmd_log(NETMD_LOG_ERROR, "Error: %s requires at least %d arguments\n", text, min_argc);
    return 0;
}

void print_current_track_info(netmd_dev_handle* devh)
{
    uint16_t track;
    char buffer[256];
    netmd_time time;

    /* TODO: error checking */
    netmd_get_position(devh, &time);
    netmd_get_track(devh, &track);
    netmd_request_title(devh, track, buffer, 256);

    printf("Current track: %s \n", buffer);
    printf("Current playback position: ");
    print_time(&time);
    printf("\n");

}

void print_disc_info(netmd_dev_handle* devh, HndMdHdr md)
{
    uint16_t tc = 0;
    uint16_t i = 0;
    int16_t group = 0, lastgroup = 9858;
    const char* group_name;
    unsigned char bitrate_id;
    unsigned char flags;
    unsigned char channel;
    char *name, buffer[256];
    struct netmd_track time;
    struct netmd_pair const *trprot, *bitrate;

    trprot = bitrate = 0;

    printf("Disc Title: %s\n", md_header_disc_title(md));

    netmd_disc_capacity capacity;
    netmd_get_disc_capacity(devh, &capacity);
    printf("Disc Length: %.02d:%.02d:%.02d.%.03d\n", 
        capacity.total.hour, capacity.total.minute,
        capacity.total.second, capacity.total.frame);

    printf("Time used: %.02d:%.02d:%.02d.%.03d\n", 
        capacity.recorded.hour, capacity.recorded.minute,
        capacity.recorded.second, capacity.recorded.frame);

    printf("Time available: %.02d:%.02d:%.02d.%.03d\n", 
        capacity.available.hour, capacity.available.minute,
        capacity.available.second, capacity.available.frame);

    netmd_request_track_count(devh, &tc);

    for(i = 0; i < tc; i++)
    {
        netmd_request_title(devh, i, buffer, 256);

        group_name = md_header_track_group(md, i + 1, &group);

        if (group != lastgroup)
        {
            lastgroup = group;

            if (group != -1)
            {
                printf(" [ %s ]\n", group_name);
            }
        }

        netmd_request_track_time(devh, i, &time);
        netmd_request_track_flags(devh, i, &flags);
        netmd_request_track_bitrate(devh, i, &bitrate_id, &channel);

        trprot = find_pair(flags, trprot_settings);
        bitrate = find_pair(bitrate_id, bitrates);

        /* Skip 'LP:' prefix... the codec type shows up in the list anyway*/
        if( strncmp( buffer, "LP:", 3 ))
        {
            name = buffer;
        } else {
            name = buffer + 3;
        }

        // Format track time
        char time_buf[9];
        sprintf(time_buf, "%02i:%02i:%02i", time.minute, time.second, time.tenth);

        if (group != -1)
        {
            printf("    ");
        }

        printf("%.2d) %s (%s; %s; %s)\n",
            i + 1, name, time_buf, 
            trprot->name, bitrate->name);
    }
}

static time_t toSec(netmd_time* t)
{
    return (t->hour * 3600) + (t->minute * 60) + t->second;
}

void print_json_disc_info_gui(netmd_device* dev,  netmd_dev_handle* devh, HndMdHdr md)
{
    // Construct JSON object
    json = json_object_new_object();
    json_object_object_add(json, "title",   json_object_new_string(md_header_disc_title(md)));
    json_object_object_add(json, "otf_enc", json_object_new_int(dev->otf_conv));
    json_object_object_add(json, "device",  json_object_new_string(dev->model));
    json_object_object_add(json, "sp_upload",  json_object_new_int(netmd_dev_supports_sp_upload(devh)));

    uint16_t tc = 0;
    if (netmd_request_track_count(devh, &tc) == 0)
    {
        json_object_object_add(json, "trk_count",  json_object_new_int(tc));
    }

    uint8_t disc_flags = 0;
    if (netmd_request_disc_flags(devh, &disc_flags) == 0)
    {
        char hex[5] = {'\0'};
        snprintf(hex, 5, "0x%.02x", disc_flags);
        json_object_object_add(json, "disc_flags", json_object_new_string(hex));
    }

    netmd_disc_capacity capacity;
    netmd_get_disc_capacity(devh, &capacity);
    json_object_object_add(json, "t_used", json_object_new_int(toSec(&capacity.recorded)));
    json_object_object_add(json, "t_total", json_object_new_int(toSec(&capacity.total)));
    json_object_object_add(json, "t_free", json_object_new_int(toSec(&capacity.available)));
    
    MDGroups* pGroups = md_header_groups(md);
    if (pGroups != NULL)
    {
        json_object* groups = json_object_new_array();
        for (int i = 0; i < pGroups->mCount; i++)
        {
            if (pGroups->mpGroups[i].mFirst > 0)
            {
                int first = pGroups->mpGroups[i].mFirst;
                int last  = (pGroups->mpGroups[i].mLast == -1) ? first : pGroups->mpGroups[i].mLast;
                json_object* group = json_object_new_object();
                json_object_object_add(group, "name", json_object_new_string(pGroups->mpGroups[i].mpName));
                json_object_object_add(group, "first", json_object_new_int(first));
                json_object_object_add(group, "last", json_object_new_int(last));
                json_object_array_add(groups, group);
            }
        }
        json_object_object_add(json, "groups", groups);
        md_header_free_groups(&pGroups);
    }

    json_object* tracks = json_object_new_array();
    unsigned char bitrate_id;
    unsigned char flags;
    unsigned char channel;
    char *name, buffer[256];
    struct netmd_track time;
    struct netmd_pair const *trprot, *bitrate;

    trprot = bitrate = 0;

    for(uint16_t i = 0; i < tc; i++)
    {
        json_object* track = json_object_new_object();
        netmd_request_title(devh, i, buffer, 256);
        netmd_request_track_time(devh, i, &time);
        netmd_request_track_flags(devh, i, &flags);
        netmd_request_track_bitrate(devh, i, &bitrate_id, &channel);

        trprot = find_pair(flags, trprot_settings);
        bitrate = find_pair(bitrate_id, bitrates);

        /* Skip 'LP:' prefix... the codec type shows up in the list anyway*/
        if( strncmp( buffer, "LP:", 3 ))
        {
            name = buffer;
        } else {
            name = buffer + 3;
        }

        // Format track time
        char time_buf[9];
        sprintf(time_buf, "%02i:%02i:%02i", time.minute, time.second, time.tenth);

        // Create JSON track object and add to array
        json_object_object_add(track, "no",         json_object_new_int(i));
        json_object_object_add(track, "protect",    json_object_new_string(trprot->name));
        json_object_object_add(track, "bitrate",    json_object_new_string(bitrate->name));
        json_object_object_add(track, "time",       json_object_new_string(time_buf));
        json_object_object_add(track, "name",       json_object_new_string(name));

        json_object_array_add(tracks, track);
    }
    json_object_object_add(json, "tracks", tracks);

    if (json_fd == NULL)
        json_fd = stdout;

    int expflags = JSON_C_TO_STRING_PRETTY;

#ifdef JSON_C_TO_STRING_NOSLASHESCAPE
    expflags |= JSON_C_TO_STRING_NOSLASHESCAPE;
#endif

    fputs(json_object_to_json_string_ext(json, expflags), json_fd);
    fflush(json_fd);

    // Clean up JSON object
    json_object_put(json);
}

void print_json_disc_info(netmd_device* dev, netmd_dev_handle* devh, HndMdHdr md, int shortJson)
{
    // Construct JSON object
    json = json_object_new_object();
    json_object_object_add(json, "raw-header", json_object_new_string(md_header_to_string(md)));
    json_object_object_add(json, "device",  json_object_new_string(dev->model));
    json_object_object_add(json, "title",   json_object_new_string(md_header_disc_title(md)));
    json_object_object_add(json, "otf_enc",  json_object_new_int(dev->otf_conv));
    json_object_object_add(json, "sp_upload",  json_object_new_int(netmd_dev_supports_sp_upload(devh)));

    uint16_t tc = 0;
    if (netmd_request_track_count(devh, &tc) == 0)
    {
        json_object_object_add(json, "trk_count",  json_object_new_int(tc));
    }

    netmd_disc_capacity capacity;
    netmd_get_disc_capacity(devh, &capacity);

    if (shortJson)
    {
        json_object_object_add(json, "t_used", json_object_new_int(toSec(&capacity.recorded)));
        json_object_object_add(json, "t_total", json_object_new_int(toSec(&capacity.total)));
        json_object_object_add(json, "t_free", json_object_new_int(toSec(&capacity.available)));
    }
    else
    {
        uint16_t i = 0;
        int16_t group = 0, lastgroup = 9858;
        const char* group_name;
        char* last_group_name = NULL;
        unsigned char bitrate_id;
        unsigned char flags;
        unsigned char channel;
        char *name, buffer[256];
        struct netmd_track time;
        struct netmd_pair const *trprot, *bitrate;

        trprot = bitrate = 0;

        json_object_object_add(json, "recordedTime", json_time(&capacity.recorded));
        json_object_object_add(json, "totalTime", json_time(&capacity.total));
        json_object_object_add(json, "availableTime", json_time(&capacity.available));

        json_object* tracks = json_object_new_array();
        json_object* groups = json_object_new_array();
        json_object* jgroup = NULL;

        for(i = 0; i < tc; i++)
        {
            netmd_request_title(devh, i, buffer, 256);

            group_name = md_header_track_group(md, i + 1, &group);

            if (group != lastgroup)
            {
                lastgroup = group;

                if (jgroup != NULL)
                {
                    json_object* gobj = json_object_new_object();
                    json_object_object_add(gobj, "name"  , json_object_new_string(last_group_name));
                    json_object_object_add(gobj, "tracks", jgroup);
                    json_object_array_add(groups, gobj);
                    free(last_group_name);
                    jgroup = NULL;
                    last_group_name = NULL;
                }

                if (group != -1)
                {
                    last_group_name = strdup(group_name);
                    jgroup = json_object_new_array();
                }
            }

            netmd_request_track_time(devh, i, &time);
            netmd_request_track_flags(devh, i, &flags);
            netmd_request_track_bitrate(devh, i, &bitrate_id, &channel);

            trprot = find_pair(flags, trprot_settings);
            bitrate = find_pair(bitrate_id, bitrates);

            /* Skip 'LP:' prefix... the codec type shows up in the list anyway*/
            if( strncmp( buffer, "LP:", 3 ))
            {
                name = buffer;
            } else {
                name = buffer + 3;
            }

            // Format track time
            char time_buf[9];
            sprintf(time_buf, "%02i:%02i:%02i", time.minute, time.second, time.tenth);

            // Create JSON track object and add to array
            json_object* track = json_object_new_object();
            json_object_object_add(track, "no",         json_object_new_int(i));
            json_object_object_add(track, "protect",    json_object_new_string(trprot->name));
            json_object_object_add(track, "bitrate",    json_object_new_string(bitrate->name));
            json_object_object_add(track, "time",       json_object_new_string(time_buf));
            json_object_object_add(track, "name",       json_object_new_string(name));

            if (jgroup != NULL)
            {
                json_object_array_add(jgroup, track);
            }
            else
            {
                json_object_array_add(tracks, track);
            }
        }

        if (jgroup != NULL)
        {
            json_object* gobj = json_object_new_object();
            json_object_object_add(gobj, "name"  , json_object_new_string(last_group_name));
            json_object_object_add(gobj, "tracks", jgroup);
            json_object_array_add(groups, gobj);
            free(last_group_name);
            jgroup = NULL;
            last_group_name = NULL;
        }

        json_object_object_add(json, "groups", groups);
        json_object_object_add(json, "tracks", tracks);
        
    }

    if (json_fd == NULL)
        json_fd = stdout;

    int expflags = JSON_C_TO_STRING_PRETTY;

#ifdef JSON_C_TO_STRING_NOSLASHESCAPE
    expflags |= JSON_C_TO_STRING_NOSLASHESCAPE;
#endif

    fputs(json_object_to_json_string_ext(json, expflags), json_fd);
    fflush(json_fd);

    // Clean up JSON object
    json_object_put(json);
}

void import_m3u_playlist(netmd_dev_handle* devh, const char *file)
{
    FILE *fp;
    char buffer[M3U_LINE_MAX + 1];
    char *s;
    uint8_t track;
    int discard;

    if( file == NULL )
    {
        printf( "No filename specified\n" );
        print_syntax();
        return;
    }

    if( (fp = fopen( file, "r" )) == NULL )
    {
        printf( "Unable to open file %s: %s\n", file, strerror( errno ));
        return;
    }

    if( ! fgets( buffer, M3U_LINE_MAX, fp )) {
        printf( "File Read error\n" );
        return;
    }
    if( strcmp( buffer, "#EXTM3U\n" )) {
        printf( "Invalid M3U playlist\n" );
        return;
    }

    track = 0;
    discard = 0;
    while( fgets( buffer, M3U_LINE_MAX, fp) != NULL ) {
        /* Chomp newlines */
        s = strchr( buffer, '\n' );
        if( s )
            *s = '\0';

        if( buffer[0] == '#' )
        {
            /* comment, ext3inf etc... we only care about ext3inf */
            if( strncmp( buffer, "#EXTINF:", 8 ))
            {
                printf( "Skip: %s\n", buffer );
            }
            else
            {
                s = strchr( buffer, ',' );
                if( !s )
                {
                    printf( "M3U Syntax error! %s\n", buffer );
                }
                else
                {
                    s++;
                    printf( "Title track %d - %s\n", track, s );
                    netmd_set_title(devh, track, s); /* XXX Handle errors */
                    discard = 1;	/* don't fallback to titling by filename */
                }
            }
        }
        else
        {
            /* Filename line */
            if( discard )
            {
                /* printf( "Discard: %s\n", buffer ); */
                discard = 0;
            }
            else
            {
                /* Try and generate a title from the track name */
                s = strrchr( buffer, '.' ); /* Isolate extension */
                if( s )
                    *s = 0;
                s = strrchr( buffer, '/' ); /* Isolate basename */
                if( !s )
                    s = strrchr( buffer, '\\' ); /* Handle DOS paths? */
                if( !s )
                    s = buffer;
                else
                    s++;

                printf( "Title track %d - %s\n", track, s );
                netmd_set_title(devh, track, s); /* XXX Handle errors */
            }
            track++;
        }
    }
}

netmd_error send_track(netmd_dev_handle *devh, const char *filename, const char *in_title, unsigned char onTheFlyConvert)
{
    netmd_error error;
    netmd_ekb ekb;
    unsigned char chain[] = { 0x25, 0x45, 0x06, 0x4d, 0xea, 0xca,
        0x14, 0xf9, 0x96, 0xbd, 0xc8, 0xa4,
        0x06, 0xc2, 0x2b, 0x81, 0x49, 0xba,
        0xf0, 0xdf, 0x26, 0x9d, 0xb7, 0x1d,
        0x49, 0xba, 0xf0, 0xdf, 0x26, 0x9d,
        0xb7, 0x1d };
    unsigned char signature[] = { 0xe8, 0xef, 0x73, 0x45, 0x8d, 0x5b,
        0x8b, 0xf8, 0xe8, 0xef, 0x73, 0x45,
        0x8d, 0x5b, 0x8b, 0xf8, 0x38, 0x5b,
        0x49, 0x36, 0x7b, 0x42, 0x0c, 0x58 };
    unsigned char rootkey[] = { 0x13, 0x37, 0x13, 0x37, 0x13, 0x37,
        0x13, 0x37, 0x13, 0x37, 0x13, 0x37,
        0x13, 0x37, 0x13, 0x37 };
    netmd_keychain *keychain;
    netmd_keychain *next;
    size_t done;
    unsigned char hostnonce[8] = { 0 };
    unsigned char devnonce[8] = { 0 };
    unsigned char sessionkey[8] = { 0 };
    unsigned char kek[] = { 0x14, 0xe3, 0x83, 0x4e, 0xe2, 0xd3, 0xcc, 0xa5 };
    unsigned char contentid[] = { 0x01, 0x0F, 0x50, 0x00, 0x00, 0x04,
        0x00, 0x00, 0x00, 0x48, 0xA2, 0x8D,
        0x3E, 0x1A, 0x3B, 0x0C, 0x44, 0xAF,
        0x2f, 0xa0 };
    netmd_track_packets *packets = NULL;
    size_t packet_count = 0;
    size_t packet_length = 0;
    struct stat stat_buf;
    unsigned char *data = NULL;
    size_t data_size;
    FILE *f;

    uint16_t track;
    unsigned char uuid[8] = { 0 };
    unsigned char new_contentid[20] = { 0 };
    char title[256] = { 0 };

    size_t headersize, channels;
    unsigned int frames, override_frames = 0;
    size_t data_position, audio_data_position, audio_data_size, i;
    audio_patch_t audio_patch = apt_no_patch;
    unsigned char * audio_data;
    netmd_wireformat wireformat;
    unsigned char discformat;

    /* read source */
    stat(filename, &stat_buf);
    if ((data_size = (size_t)stat_buf.st_size) < MIN_WAV_LENGTH) {
        netmd_log(NETMD_LOG_ERROR, "audio file too small (corrupt or not supported)\n");
        return NETMD_ERROR;
    }

    netmd_log(NETMD_LOG_VERBOSE, "audio file size : %d bytes\n", data_size);

    /* open audio file */
    if ((data = (unsigned char *)malloc(data_size + 2048)) == NULL) {      // reserve additional mem for padding if needed
        netmd_log(NETMD_LOG_ERROR, "error allocating memory for file input\n");
        return NETMD_ERROR;
    }
    else {
        if (!(f = fopen(filename, "rb"))) {
            netmd_log(NETMD_LOG_ERROR, "cannot open audio file\n");
            free(data);

            return NETMD_ERROR;
        }
    }

    /* copy file to buffer */
    memset(data, 0, data_size + 8);
    if ((fread(data, data_size, 1, f)) < 1) {
        netmd_log(NETMD_LOG_ERROR, "cannot read audio file\n");
        free(data);

        return NETMD_ERROR;
    }
    fclose(f);

    /* check contents */
    if (!audio_supported(data, data_size, &wireformat, &discformat, &audio_patch, &channels, &headersize)) {
        netmd_log(NETMD_LOG_ERROR, "audio file unknown or not supported\n");
        free(data);

        return NETMD_ERROR;
    }
    else
    {
        netmd_log(NETMD_LOG_VERBOSE, "supported audio file detected\n");
        if (audio_patch == apt_sp)
        {
            override_frames = (data_size - 2048) / 212;
            if (netmd_prepare_audio_sp_upload(&data, &data_size) != NETMD_NO_ERROR)
            {
                netmd_log(NETMD_LOG_ERROR, "cannot prepare ATRAC1 audio data for SP transfer!\n");
                free(data);
                return NETMD_ERROR;
            }
            else
            {
                // data returned by prepare function has no header
                audio_data = data;
                audio_data_size = data_size;
                netmd_log(NETMD_LOG_VERBOSE, "prepared audio data size: %d bytes\n", audio_data_size);
            }
        }
        else if ((data_position = wav_data_position(data, headersize, data_size)) == 0)
        {
            netmd_log(NETMD_LOG_ERROR, "cannot locate audio data in file\n");
            free(data);

            return NETMD_ERROR;
        }
        else
        {
            netmd_log(NETMD_LOG_VERBOSE, "data chunk position at %d\n", data_position);
            audio_data_position = data_position + 8;
            audio_data = data + audio_data_position;
            audio_data_size = leword32(data + (data_position + 4));
            netmd_log(NETMD_LOG_VERBOSE, "audio data size read from file :           %d bytes\n", audio_data_size);
            netmd_log(NETMD_LOG_VERBOSE, "audio data size calculated from file size: %d bytes\n", data_size - audio_data_position);
        }
    }

    /* acquire device - needed by Sharp devices, may fail on Sony devices */
    error = netmd_acquire_dev(devh);
    netmd_log(NETMD_LOG_VERBOSE, "netmd_acquire_dev: %s\n", netmd_strerror(error));

    if (audio_patch == apt_sp)
    {
        if (netmd_apply_sp_patch(devh, (channels == NETMD_CHANNELS_STEREO) ? 2 : 1) != NETMD_NO_ERROR)
        {
            netmd_log(NETMD_LOG_ERROR, "Can't patch NetMD device for SP transfer, exiting!\n");
            free(data);
            netmd_undo_sp_patch(devh);
            netmd_release_dev(devh);
            return NETMD_ERROR;
        }
    }

    error = netmd_secure_leave_session(devh);
    netmd_log(NETMD_LOG_VERBOSE, "netmd_secure_leave_session : %s\n", netmd_strerror(error));

    error = netmd_secure_set_track_protection(devh, 0x01);
    netmd_log(NETMD_LOG_VERBOSE, "netmd_secure_set_track_protection : %s\n", netmd_strerror(error));

    error = netmd_secure_enter_session(devh);
    netmd_log(NETMD_LOG_VERBOSE, "netmd_secure_enter_session : %s\n", netmd_strerror(error));

    /* build ekb */
    ekb.id = 0x26422642;
    ekb.depth = 9;
    ekb.signature = malloc(sizeof(signature));
    memcpy(ekb.signature, signature, sizeof(signature));

    /* build ekb key chain */
    ekb.chain = NULL;
    for (done = 0; done < sizeof(chain); done += 16U)
    {
        next = malloc(sizeof(netmd_keychain));
        if (ekb.chain == NULL) {
            ekb.chain = next;
        }
        else {
            keychain->next = next;
        }
        next->next = NULL;

        next->key = malloc(16);
        memcpy(next->key, chain + done, 16);

        keychain = next;
    }

    error = netmd_secure_send_key_data(devh, &ekb);
    netmd_log(NETMD_LOG_VERBOSE, "netmd_secure_send_key_data : %s\n", netmd_strerror(error));

    /* cleanup */
    free(ekb.signature);
    keychain = ekb.chain;
    while (keychain != NULL) {
        next = keychain->next;
        free(keychain->key);
        free(keychain);
        keychain = next;
    }

    /* exchange nonces */
    gcry_create_nonce(hostnonce, sizeof(hostnonce));
    error = netmd_secure_session_key_exchange(devh, hostnonce, devnonce);
    netmd_log(NETMD_LOG_VERBOSE, "netmd_secure_session_key_exchange : %s\n", netmd_strerror(error));

    /* calculate session key */
    retailmac(rootkey, hostnonce, devnonce, sessionkey);

    error = netmd_secure_setup_download(devh, contentid, kek, sessionkey);
    netmd_log(NETMD_LOG_VERBOSE, "netmd_secure_setup_download : %s\n", netmd_strerror(error));

    /* conversion (byte swapping) for pcm raw data from wav file if needed */
    if (audio_patch == apt_wave)
    {
        for (i = 0; i < audio_data_size; i += 2)
        {
            unsigned char first = audio_data[i];
            audio_data[i] = audio_data[i + 1];
            audio_data[i + 1] = first;
        }
    }

    /* number of frames will be calculated by netmd_prepare_packets() depending on the wire format and channels */
    error = netmd_prepare_packets(audio_data, audio_data_size, &packets, &packet_count, &frames, channels, &packet_length, kek, wireformat);
    netmd_log(NETMD_LOG_VERBOSE, "netmd_prepare_packets : %s\n", netmd_strerror(error));

    if ((discformat == NETMD_DISKFORMAT_SP_STEREO) && (onTheFlyConvert != NO_ONTHEFLY_CONVERSION))
    {
        discformat = onTheFlyConvert;
    }

    if(override_frames)
        frames = override_frames;

    /* send to device */
    error = netmd_secure_send_track(devh, wireformat,
        discformat,
        frames, packets,
        packet_length, sessionkey,
        &track, uuid, new_contentid);
    netmd_log(NETMD_LOG_VERBOSE, "netmd_secure_send_track : %s\n", netmd_strerror(error));

    /* cleanup */
    netmd_cleanup_packets(&packets);
    free(data);
    audio_data = NULL;

    if (error == NETMD_NO_ERROR) {
        char *titlep = title;

        /* set title, use either user-specified title or filename */
        if (in_title != NULL)
            strncpy(title, in_title, sizeof(title) - 1);
        else {
            strncpy(title, filename, sizeof(title) - 1);

            /* eliminate file extension */
            char *ext_dot = strrchr(title, '.');
            if (ext_dot != NULL)
                *ext_dot = '\0';

            /* eliminate path */
            char *title_slash = strrchr(title, '/');
            if (title_slash != NULL)
                titlep = title_slash + 1;
        }

        netmd_log(NETMD_LOG_VERBOSE, "New Track: %d\n", track);
        netmd_cache_toc(devh);
        netmd_set_title(devh, track, titlep);
        netmd_sync_toc(devh);

        /* commit track */
        error = netmd_secure_commit_track(devh, track, sessionkey);
        if (error == NETMD_NO_ERROR)
            netmd_log(NETMD_LOG_VERBOSE, "netmd_secure_commit_track : %s\n", netmd_strerror(error));
        else
            netmd_log(NETMD_LOG_ERROR, "netmd_secure_commit_track failed : %s\n", netmd_strerror(error));
    }
    else {
        netmd_log(NETMD_LOG_ERROR, "netmd_secure_send_track failed : %s\n", netmd_strerror(error));
    }

    /* forget key */
    netmd_error cleanup_error = netmd_secure_session_key_forget(devh);
    netmd_log(NETMD_LOG_VERBOSE, "netmd_secure_session_key_forget : %s\n", netmd_strerror(cleanup_error));

    /* leave session */
    cleanup_error = netmd_secure_leave_session(devh);
    netmd_log(NETMD_LOG_VERBOSE, "netmd_secure_leave_session : %s\n", netmd_strerror(cleanup_error));

    if (audio_patch == apt_sp)
    {
        netmd_undo_sp_patch(devh);
    }

    /* release device - needed by Sharp devices, may fail on Sony devices */
    cleanup_error = netmd_release_dev(devh);
    netmd_log(NETMD_LOG_VERBOSE, "netmd_release_dev : %s\n", netmd_strerror(cleanup_error));

    return error; /* return error code from the "business logic" */
}

void print_syntax()
{
    puts("\nNetMD command line tool");
    puts("Usage: netmd [options] command args\n");
    puts("Options:");
    puts("      -v show debug messages");
    puts("      -t enable tracing of USB command and response data");
    puts("      -d [lp2|lp4] ATRAC3 on the fly encoding\n");
    puts("Commands:");
    puts("json - print disc info in json format");
    puts("json_short - print short disc info in json format");
    puts("json_gui - print disc info in json format to be used in GUI");
    puts("disc_info - print disc info in plain text");
    puts("add_group <title> <first group track> <last group track> - add a new group and place a track range");
    puts("rename_disc <string> - sets the disc title w/o touching group infomration");
    puts("rename # <string> - rename track # to <string> track numbers are off by one (ie track 1 is 0)");
    puts("move #1 #2 - make track #1 track #2");
    puts("groupmove #1 #2 - make group #1 start at track #2 !BUGGY!");
    puts("deletegroup #1 - delete a group, but not the tracks in it");
    puts("group #1 #2 - Stick track #1 into group #2");
    puts("retitle #1 <string> - rename group number #1 to <string>");
    puts("play #1 - play track #");
    puts("fforward - start fast forwarding");
    puts("rewind - start rewinding");
    puts("next - starts next track");
    puts("previous - starts previous track");
    puts("restart - restarts current track");
    puts("pause - pause the unit");
    puts("stop - stop the unit");
    puts("delete #1 [#2] - delete track (or tracks in range #1-#2 if #2 given)");
    puts("del_track #1 - delete track and update groups if needed");
    puts("erase [force] - erase the disc (the argument 'force' must be given to actually do it)");
    puts("m3uimport <file> - import song and disc title from a playlist");
    puts("send <file> [<string>] - send WAV format audio file to the device and set title to <string> (optional)");
    puts("      Supported file formats: 16 bit pcm (stereo or mono) @44100Hz or");
    puts("         Atrac LP2/LP4 data stored in a WAV container.");
    puts("      Title defaults to file name if not specified.");
    puts("raw - send raw command (hex)");
    puts("setplaymode (single, repeat, shuffle) - set play mode");
    puts("newgroup <string> - create a new group named <string>");
    puts("settitle <string> - manually set the complete disc title (with group information)");
    puts("settime <track> [<hour>] <minute> <second> [<frame>] - seeks to the given timestamp");
    puts("      (if three values are given, they are minute, second and frame)");
    puts("capacity - shows current minidisc capacity (used, available)");
#if 0  // relevant code at top of file is commented out; leaving this in as reference
    puts("secure #1 #2 - execute secure command #1 on track #2 (where applicable)");
    puts("  --- general ---");
    puts("  0x80 = start secure session");
    puts("  0x11 = get player id");
    puts("  0x12 = send ekb");
    puts("  0x20 = exchange randoms");
    puts("  0x21 = discard randoms");
    puts("  0x81 = end secure session");
    puts("  --- check-out ---");
    puts("  0x22 = submit 32-byte hash");
    puts("  0x28 = prepare download");
    puts("  0x48 = verify downloaded track #");
    puts("  --- check-in ---");
    puts("  0x23 = get hash id for track #");
    puts("  0x40 = secure delete track #");
#endif
    puts("help - show this message\n");
}

void netmd_cli_set_json_fd(FILE* fd)
{
    json_fd = fd;
}

void test_new_utils()
{
    uint8_t *query = NULL;
    size_t   qsz   = 0;
    uint32_t addr  = 0x01020304;
    uint8_t  data[] = {0xde, 0xad, 0xbe, 0xef, 0x4e};

    netmd_query_data_t argv[] = {
        {{.u32 = addr                                     }, sizeof(uint32_t)},
        {{.u8  = 5                                        }, sizeof(uint8_t) },
        {{.pu8 = data                                     }, 5               },
        {{.u16 = netmd_calculate_checksum(data, 5)        }, sizeof(uint16_t)},
    };

    int argc = sizeof(argv) / sizeof(argv[0]);
    query = netmd_format_query("00 1822 ff 00 %<d %b 0000 %* %<w", argv, argc, &qsz);

    if (query != NULL)
    {
        netmd_log_hex(NETMD_LOG_ALL, query, qsz);

        netmd_capture_data_t *capture = NULL;
        int                   cap_sz  = 0;

        netmd_scan_query(query, qsz, "%? 1822 ff %b %w %? %? %b 0000 %*", &capture, &cap_sz);
        netmd_log(NETMD_LOG_ALL, "Scan gives following values:\n");
        if (capture != NULL)
        {
            for(int i = 0; i < cap_sz; i++)
            {
                switch(capture[i].tp)
                {
                case netmd_fmt_byte:
                    netmd_log(NETMD_LOG_ALL, "Cap %d, Byte: 0x%.02x\n", i, (int)capture[i].data.u8);
                    break;
                case netmd_fmt_word:
                    netmd_log(NETMD_LOG_ALL, "Cap %d, Word: 0x%.04x\n", i, capture[i].data.u16);
                    break;
                case netmd_fmt_dword:
                    netmd_log(NETMD_LOG_ALL, "Cap %d, DWord: 0x%.08x\n", i, capture[i].data.u32);
                    break;
                case netmd_fmt_qword:
                    netmd_log(NETMD_LOG_ALL, "Cap %d, QWord: 0x%.016x\n", i, capture[i].data.u64);
                    break;
                case netmd_fmt_barray:
                    netmd_log(NETMD_LOG_ALL, "Cap %d, Byte Array: ", i);
                    netmd_log_hex(NETMD_LOG_ALL, capture[i].data.pu8, capture[i].size);
                    free(capture[i].data.pu8);
                    break;
                default:
                    break;
                }
            }
            free(capture);
        }
        free(query);
    }
}

int run_me(int argc, char* argv[])
{
    netmd_dev_handle* devh;
    HndMdHdr md = NULL;
    netmd_device *device_list, *netmd;
    long unsigned int i = 0;
    long unsigned int j = 0;
    char name[16];
    uint16_t track, playmode;
    netmd_time time;
    netmd_error error;
    FILE *f;
    int exit_code = 0;
    unsigned char onTheFlyConvert = NO_ONTHEFLY_CONVERSION;

    /* by default, log only errors */
    netmd_set_log_level(NETMD_LOG_ERROR);

    /* parse parameters */
    {
        int c;
        opterr = 0;
        optind = 1;

        while ((c = getopt (argc, argv, "tvd:Y")) != -1)
        {
            switch (c)
            {
            case 'Y':
                test_new_utils();
                return 0;
                break;
            case 't':
                netmd_set_log_level(NETMD_LOG_ALL);
                break;
            case 'v':
                netmd_set_log_level(NETMD_LOG_VERBOSE);
                break;
            case 'd':
                if (!strcmp(optarg, "lp2"))
                {
                    onTheFlyConvert = NETMD_DISKFORMAT_LP2;
                }
                else if (!strcmp(optarg, "lp4"))
                {
                    onTheFlyConvert = NETMD_DISKFORMAT_LP4;
                }
                break;
            case '?':
                if (optopt == 'd')
                {
                    netmd_log(NETMD_LOG_ERROR, "Option -%c requires an argument.\n", optopt);
                }
                else if (isprint (optopt))
                {
                    netmd_log(NETMD_LOG_ERROR, "Unknown option `-%c'.\n", optopt);
                }
                else
                {
                    netmd_log(NETMD_LOG_ERROR, "Unknown option character `\\x%x'.\n", optopt);
                }
            // fall through
            default:
                return 1;
            }
        }
    }

    /* update argv and argc after parsing options */
    argv = &argv[optind - 1];
    argc -= (optind - 1);

    /* don't require device init to show help */
    if ((argc == 1) || ((argc > 1 && strcmp("help", argv[1]) == 0)))
    {
        print_syntax();
        return 0;
    }

    error = netmd_init(&device_list, NULL);
    if (error != NETMD_NO_ERROR) {
        printf("Error initializing netmd\n%s\n", netmd_strerror(error));
        return 1;
    }

    if (device_list == NULL) {
        puts("Found no NetMD device(s).");
        return 1;
    }

    /* pick first available device */
    netmd = device_list;

    error = netmd_open(netmd, &devh);
    if(error != NETMD_NO_ERROR)
    {
        printf("Error opening netmd\n%s\n", netmd_strerror(error));
        return 1;
    }

    error = netmd_get_devname(devh, name, 16);
    if (error != NETMD_NO_ERROR)
    {
        printf("Could not get device name\n%s\n", netmd_strerror(error));
        return 1;
    }

    netmd_initialize_disc_info(devh, &md);

    /* parse commands */
    if(argc > 1)
    {
        if(strcmp("json", argv[1]) == 0)
        {
            print_json_disc_info(netmd, devh, md, 0);
        }
        else if(strcmp("json_short", argv[1]) == 0)
        {
            print_json_disc_info(netmd, devh, md, 1);
        }
        else if(strcmp("json_gui", argv[1]) == 0)
        {
            print_json_disc_info_gui(netmd, devh, md);
        }
        else if(strcmp("disc_info", argv[1]) == 0)
        {
            print_disc_info(devh, md);  
        }
        else if(strcmp("rename", argv[1]) == 0)
        {
            if (!check_args(argc, 3, "rename")) return -1;
            i = strtoul(argv[2], NULL, 10);
            netmd_cache_toc(devh);
            netmd_set_title(devh, i & 0xffff, argv[3]);
            netmd_sync_toc(devh);
        }
        else if(strcmp("move", argv[1]) == 0)
        {
            if (!check_args(argc, 3, "move")) return -1;
            i = strtoul(argv[2], NULL, 10);
            j = strtoul(argv[3], NULL, 10);
            netmd_move_track(devh, i & 0xffff, j & 0xffff);
        }
        else if(strcmp("write", argv[1]) == 0)
        {
            // Probably non-functional for most use cases
            if (!check_args(argc, 2, "write")) return -1;
            if(netmd_write_track(devh, argv[2]) < 0)
            {
                fprintf(stderr, "Error writing track %i\n", errno);
            }
        }
        else if(strcmp("newgroup", argv[1]) == 0)
        {
            if (!check_args(argc, 2, "newgroup")) return -1;
            netmd_create_group(devh, md, argv[2], -1, -1);
        }
        else if(strcmp("settitle", argv[1]) == 0)
        {
            if (!check_args(argc, 2, "settitle")) return -1;
            // netmd_cache_toc(devh);
            netmd_set_disc_title(devh, argv[2], strlen(argv[2]));
            // netmd_sync_toc(devh);
        }
        else if(strcmp("add_group", argv[1]) == 0)
        {
            if (!check_args(argc, 4, "add_group")) return -1;
            i = strtoul(argv[3], NULL, 10);
            j = strtoul(argv[4], NULL, 10);
            if (md_header_add_group(md, argv[2], i, j) > 0)
            {
                netmd_write_disc_header(devh, md);
            }
        }
        else if(strcmp("rename_disc", argv[1]) == 0)
        {
            if (!check_args(argc, 2, "rename_disc")) return -1;
            if (md_header_set_disc_title(md, argv[2]) == 0)
            {
                netmd_write_disc_header(devh, md);
            }
        }
        else if(strcmp("group", argv[1]) == 0)
        {
            if (!check_args(argc, 3, "group")) return -1;
            i = strtoul(argv[2], NULL, 10);
            j = strtoul(argv[3], NULL, 10);
            if(!netmd_put_track_in_group(devh, md, i & 0xffff, j & 0xffff))
            {
                printf("Something screwy happened\n");
            }
        }
        else if(strcmp("retitle", argv[1]) == 0)
        {
            if (!check_args(argc, 3, "retitle")) return -1;
            i = strtoul(argv[2], NULL, 10);
            netmd_set_group_title(devh, md, (unsigned int) i, argv[3]);
        }
        else if(strcmp("play", argv[1]) == 0)
        {
            if( argc > 2 ) {
                i = strtoul(argv[2],NULL, 10);
                netmd_set_track( devh, i & 0xffff );
            }
            netmd_play(devh);
        }
        else if(strcmp("stop", argv[1]) == 0)
        {
            netmd_stop(devh);
        }
        else if(strcmp("pause", argv[1]) == 0)
        {
            netmd_pause(devh);
        }
        else if(strcmp("fforward", argv[1]) == 0)
        {
            netmd_fast_forward(devh);
        }
        else if(strcmp("rewind", argv[1]) == 0)
        {
            netmd_rewind(devh);
        }
        else if(strcmp("next", argv[1]) == 0)
        {
            netmd_track_next(devh);
        }
        else if(strcmp("previous", argv[1]) == 0)
        {
            netmd_track_previous(devh);
        }
        else if(strcmp("restart", argv[1]) == 0)
        {
            netmd_track_restart(devh);
        }
        else if(strcmp("settime", argv[1]) == 0)
        {
            if (!check_args(argc, 4, "settime")) return -1;
            track = strtoul(argv[2], (char **) NULL, 10) & 0xffff;
            if (argc > 6)
            {
                time.hour = strtoul(argv[3], (char **) NULL, 10) & 0xffff;
                time.minute = strtoul(argv[4], (char **) NULL, 10) & 0xff;
                time.second = strtoul(argv[5], (char **) NULL, 10) & 0xff;
                time.frame = strtoul(argv[6], (char **) NULL, 10) & 0xff;
            }
            else
            {
                time.hour = 0;
                time.minute = strtoul(argv[3], (char **) NULL, 10) & 0xff;
                time.second = strtoul(argv[4], (char **) NULL, 10) & 0xff;
                if (argc > 5)
                {
                    time.frame = strtoul(argv[5], (char **) NULL, 10) & 0xff;;
                }
                else
                {
                    time.frame = 0;
                }
            }

            netmd_set_time(devh, track, &time);
        }
        else if(strcmp("m3uimport", argv[1]) == 0)
        {
            if (!check_args(argc, 2, "m3uimport")) return -1;
            import_m3u_playlist(devh, argv[2]);
        }
        else if(strcmp("del_track", argv[1]) == 0)
        {
            if (!check_args(argc, 2, "del_track")) return -1;
            i = strtoul(argv[2], NULL, 10);
            uint16_t tc = 0;
            netmd_request_track_count(devh, &tc);

            if (i < tc)
            {
                netmd_cache_toc(devh);
                netmd_delete_track(devh, i);
                netmd_wait_for_sync(devh);
                netmd_sync_toc(devh);

                if (md_header_del_track(md, i + 1) == 0)
                {
                    netmd_write_disc_header(devh, md);
                }
            }
            else
            {
                netmd_log(NETMD_LOG_ERROR, "del_track: invalid track number %d\n", i);
            }
        }
        else if(strcmp("delete", argv[1]) == 0)
        {
            if (!check_args(argc, 2, "delete")) return -1;
            i = strtoul(argv[2], NULL, 10);
            if (argc > 3)
                j = strtoul(argv[3], NULL, 10);
            else
                j = i;

            if (j < i || j >= 0xffff || i >= 0xffff) {
                netmd_log(NETMD_LOG_ERROR, "delete: invalid track number\n");
                exit_code = 1;
            }
            else {
                netmd_cache_toc(devh);

                uint16_t track = j;

                while (track >= i)
                {
                    netmd_log(NETMD_LOG_VERBOSE, "delete: removing track %d\n", track);

                    netmd_delete_track(devh, track);
                    netmd_wait_for_sync(devh);

                    if (track == 0)
                    {
                        break;
                    }
                    track --;
                }

                netmd_sync_toc(devh);
            }
        }
        else if(strcmp("erase", argv[1]) == 0)
        {
          if (!check_args(argc, 2, "erase")) return -1;

          if (strcmp("force", argv[2]) != 0) {
            netmd_log(NETMD_LOG_ERROR, "erase: 'force' must be given as argument to proceed\n");
            exit_code = 1;
          } else {
            netmd_log(NETMD_LOG_VERBOSE, "erase: executing erase\n");
            netmd_erase_disc(devh);
          }
        }
        else if(strcmp("deletegroup", argv[1]) == 0)
        {
            if (!check_args(argc, 2, "deletegroup")) return -1;
            i = strtoul(argv[2], NULL, 10);
            netmd_delete_group(devh, md, i & 0xffff);
        }
        else if(strcmp("status", argv[1]) == 0) {
            print_current_track_info(devh);
        }
        else if (strcmp("raw", argv[1]) == 0) {
            if (!check_args(argc, 2, "raw")) return -1;
            send_raw_message(devh, argv[2]);
        }
        else if (strcmp("setplaymode", argv[1]) == 0) {
            playmode = 0;
            int i;
            for (i = 2; i < argc; i++) {
                if (strcmp(argv[i], "single") == 0) {
                    playmode |= NETMD_PLAYMODE_SINGLE;
                }
                else if (strcmp(argv[i], "repeat") == 0) {
                    playmode |= NETMD_PLAYMODE_REPEAT;
                }
                else if (strcmp(argv[i], "shuffle") == 0) {
                    playmode |= NETMD_PLAYMODE_SHUFFLE;
                }
            }
            printf("%x\n", playmode);
            netmd_set_playmode(devh, playmode);
        }
        else if (strcmp("capacity", argv[1]) == 0) {
            netmd_disc_capacity capacity;
            netmd_get_disc_capacity(devh, &capacity);

            printf("Recorded:  ");
            print_time(&capacity.recorded);
            printf("\nTotal:     ");
            print_time(&capacity.total);
            printf("\nAvailable: ");
            print_time(&capacity.available);
            printf("\n");
        }
        else if (strcmp("recv", argv[1]) == 0) {
            if (!check_args(argc, 3, "recv")) return -1;
            i = strtoul(argv[2], NULL, 10);
            f = fopen(argv[3], "wb");
            netmd_secure_recv_track(devh, i & 0xffff, f);
            fclose(f);
        }
        else if (strcmp("send", argv[1]) == 0) {
            if (!check_args(argc, 2, "send")) return -1;

            const char *filename = argv[2];
            char *title = NULL;
            if (argc > 3)
                title = argv[3];

            exit_code = send_track(devh, filename, title, onTheFlyConvert) == NETMD_NO_ERROR ? 0 : 1;
        } else if (strcmp("leave", argv[1]) == 0) {
          error = netmd_secure_leave_session(devh);
          netmd_log(NETMD_LOG_VERBOSE, "netmd_secure_leave_session : %s\n", netmd_strerror(error));
        }
        else {
            netmd_log(NETMD_LOG_ERROR, "Unknown command '%s'; use 'help' for list of commands\n", argv[1]);
            exit_code = 1;
        }
    }

    free_md_header(&md);
    netmd_close(devh);
    netmd_clean(&device_list);

    return exit_code;
}

void netmd_cli_set_log_fd(FILE* fd)
{
    netmd_log_set_fd(fd);
}

#ifndef NO_PROGRAM
int main(int argc, char* argv[])
{
    return run_me(argc, argv);
}
#endif /* NO_PROGRAM */
