/*****************************************************************************
 * rawvideo.c: Pseudo video decoder/packetizer for raw video data
 *****************************************************************************
 * Copyright (C) 2001, 2002 VideoLAN
 * $Id: rawvideo.c,v 1.13 2004/02/27 14:02:05 fenrir Exp $
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <vlc/vlc.h>
#include <vlc/decoder.h>
#include <vlc/vout.h>

/*****************************************************************************
 * decoder_sys_t : raw video decoder descriptor
 *****************************************************************************/
struct decoder_sys_t
{
    /* Module mode */
    vlc_bool_t b_packetizer;

    /*
     * Input properties
     */
    int i_raw_size;

    /*
     * Common properties
     */
    mtime_t i_pts;

};

/****************************************************************************
 * Local prototypes
 ****************************************************************************/
static int  OpenDecoder   ( vlc_object_t * );
static int  OpenPacketizer( vlc_object_t * );
static void CloseDecoder  ( vlc_object_t * );

static void *DecodeBlock  ( decoder_t *, block_t ** );

static picture_t *DecodeFrame( decoder_t *, block_t * );
static block_t   *SendFrame  ( decoder_t *, block_t * );

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
vlc_module_begin();
    set_description( _("Pseudo raw video decoder") );
    set_capability( "decoder", 50 );
    set_callbacks( OpenDecoder, CloseDecoder );

    add_submodule();
    set_description( _("Pseudo raw video packetizer") );
    set_capability( "packetizer", 100 );
    set_callbacks( OpenPacketizer, CloseDecoder );
vlc_module_end();

/*****************************************************************************
 * OpenDecoder: probe the decoder and return score
 *****************************************************************************/
static int OpenDecoder( vlc_object_t *p_this )
{
    decoder_t *p_dec = (decoder_t*)p_this;
    decoder_sys_t *p_sys;

    switch( p_dec->fmt_in.i_codec )
    {
        /* Planar YUV */
        case VLC_FOURCC('I','4','4','4'):
        case VLC_FOURCC('I','4','2','2'):
        case VLC_FOURCC('I','4','2','0'):
        case VLC_FOURCC('Y','V','1','2'):
        case VLC_FOURCC('I','Y','U','V'):
        case VLC_FOURCC('I','4','1','1'):
        case VLC_FOURCC('I','4','1','0'):
        case VLC_FOURCC('Y','V','U','9'):

        /* Packed YUV */
        case VLC_FOURCC('Y','U','Y','2'):
        case VLC_FOURCC('U','Y','V','Y'):

        /* RGB */
        case VLC_FOURCC('R','V','3','2'):
        case VLC_FOURCC('R','V','2','4'):
        case VLC_FOURCC('R','V','1','6'):
        case VLC_FOURCC('R','V','1','5'):
            break;

        default:
            return VLC_EGENERIC;
    }

    /* Allocate the memory needed to store the decoder's structure */
    if( ( p_dec->p_sys = p_sys =
          (decoder_sys_t *)malloc(sizeof(decoder_sys_t)) ) == NULL )
    {
        msg_Err( p_dec, "out of memory" );
        return VLC_EGENERIC;
    }
    /* Misc init */
    p_dec->p_sys->b_packetizer = VLC_FALSE;
    p_sys->i_pts = 0;

    if( p_dec->fmt_in.video.i_width <= 0 ||
        p_dec->fmt_in.video.i_height <= 0 )
    {
        msg_Err( p_dec, "invalid display size %dx%d",
                 p_dec->fmt_in.video.i_width, p_dec->fmt_in.video.i_height );
        return VLC_EGENERIC;
    }

    /* Find out p_vdec->i_raw_size */
    vout_InitFormat( &p_dec->fmt_out.video, p_dec->fmt_in.i_codec,
                     p_dec->fmt_in.video.i_width,
                     p_dec->fmt_in.video.i_height,
                     p_dec->fmt_in.video.i_aspect );
    p_sys->i_raw_size = p_dec->fmt_out.video.i_bits_per_pixel *
        p_dec->fmt_out.video.i_width * p_dec->fmt_out.video.i_height / 8;

    /* Set output properties */
    p_dec->fmt_out.i_cat = VIDEO_ES;
    p_dec->fmt_out.i_codec = p_dec->fmt_in.i_codec;
    //if( !p_dec->fmt_in.video.i_aspect )
    {
        p_dec->fmt_out.video.i_aspect = VOUT_ASPECT_FACTOR *
            p_dec->fmt_out.video.i_width / p_dec->fmt_out.video.i_height;
    }

    /* Set callbacks */
    p_dec->pf_decode_video = (picture_t *(*)(decoder_t *, block_t **))
        DecodeBlock;
    p_dec->pf_packetize    = (block_t *(*)(decoder_t *, block_t **))
        DecodeBlock;

    return VLC_SUCCESS;
}

static int OpenPacketizer( vlc_object_t *p_this )
{
    decoder_t *p_dec = (decoder_t*)p_this;

    int i_ret = OpenDecoder( p_this );

    if( i_ret == VLC_SUCCESS ) p_dec->p_sys->b_packetizer = VLC_TRUE;

    return i_ret;
}

/****************************************************************************
 * DecodeBlock: the whole thing
 ****************************************************************************
 * This function must be fed with complete frames.
 ****************************************************************************/
static void *DecodeBlock( decoder_t *p_dec, block_t **pp_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    block_t *p_block;
    void *p_buf;

    if( !pp_block || !*pp_block ) return NULL;

    p_block = *pp_block;

    if( !p_sys->i_pts && !p_block->i_pts && !p_block->i_dts )
    {
        /* We've just started the stream, wait for the first PTS. */
        block_Release( p_block );
        return NULL;
    }

    /* Date management */
    if( p_block->i_pts > 0 || p_block->i_dts > 0 )
    {
        if( p_block->i_pts > 0 ) p_sys->i_pts = p_block->i_pts;
        else if( p_block->i_dts > 0 ) p_sys->i_pts = p_block->i_dts;
    }

    if( p_block->i_buffer < p_sys->i_raw_size )
    {
        msg_Warn( p_dec, "invalid frame size (%d < %d)",
                  p_block->i_buffer, p_sys->i_raw_size );

        block_Release( p_block );
        return NULL;
    }

    if( p_sys->b_packetizer )
    {
        p_buf = SendFrame( p_dec, p_block );
    }
    else
    {
        p_buf = DecodeFrame( p_dec, p_block );
    }

    /* Date management: 1 frame per packet */
    p_sys->i_pts += ( I64C(1000000) * 1.0 / 25 /*FIXME*/ );
    *pp_block = NULL;

    return p_buf;
}

/*****************************************************************************
 * FillPicture:
 *****************************************************************************/
static void FillPicture( decoder_t *p_dec, block_t *p_block, picture_t *p_pic )
{
    uint8_t *p_src, *p_dst;
    int i_src, i_plane, i_line, i_width;

    p_src  = p_block->p_buffer;
    i_src = p_block->i_buffer;

    for( i_plane = 0; i_plane < p_pic->i_planes; i_plane++ )
    {
        p_dst = p_pic->p[i_plane].p_pixels;
        i_width = p_pic->p[i_plane].i_visible_pitch;

        for( i_line = 0; i_line < p_pic->p[i_plane].i_lines; i_line++ )
        {
            p_dec->p_vlc->pf_memcpy( p_dst, p_src, i_width );
            p_src += i_width;
            p_dst += p_pic->p[i_plane].i_pitch;
        }
    }
}

/*****************************************************************************
 * DecodeFrame: decodes a video frame.
 *****************************************************************************/
static picture_t *DecodeFrame( decoder_t *p_dec, block_t *p_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    picture_t *p_pic;

    /* Get a new picture */
    p_pic = p_dec->pf_vout_buffer_new( p_dec );
    if( !p_pic )
    {
        block_Release( p_block );
        return NULL;
    }

    FillPicture( p_dec, p_block, p_pic );

    p_pic->date = p_sys->i_pts;

    block_Release( p_block );
    return p_pic;
}

/*****************************************************************************
 * SendFrame: send a video frame to the stream output.
 *****************************************************************************/
static block_t *SendFrame( decoder_t *p_dec, block_t *p_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    p_block->i_dts = p_block->i_pts = p_sys->i_pts;

    return p_block;
}

/*****************************************************************************
 * CloseDecoder: decoder destruction
 *****************************************************************************/
static void CloseDecoder( vlc_object_t *p_this )
{
    decoder_t *p_dec = (decoder_t*)p_this;
    free( p_dec->p_sys );
}
