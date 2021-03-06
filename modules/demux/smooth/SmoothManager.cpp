/*
 * SmoothManager.cpp
 *****************************************************************************
 * Copyright © 2015 - VideoLAN and VLC authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_fixups.h>
#include <cinttypes>

#include "SmoothManager.hpp"

#include "../adaptive/tools/Retrieve.hpp"
#include "playlist/Parser.hpp"
#include "../adaptive/xml/DOMParser.h"
#include <vlc_stream.h>
#include <vlc_demux.h>
#include <vlc_charset.h>
#include <time.h>

using namespace adaptive;
using namespace adaptive::logic;
using namespace smooth;
using namespace smooth::playlist;

SmoothManager::SmoothManager(demux_t *demux_, Manifest *playlist,
                       AbstractStreamFactory *factory,
                       AbstractAdaptationLogic::LogicType type) :
             PlaylistManager(demux_, playlist, factory, type)
{
}

SmoothManager::~SmoothManager()
{
}

Manifest * SmoothManager::fetchManifest()
{
    std::string playlisturl(p_demux->psz_access);
    playlisturl.append("://");
    playlisturl.append(p_demux->psz_location);

    block_t *p_block = Retrieve::HTTP(VLC_OBJECT(p_demux), playlisturl);
    if(!p_block)
        return NULL;

    stream_t *memorystream = stream_MemoryNew(p_demux, p_block->p_buffer, p_block->i_buffer, true);
    if(!memorystream)
    {
        block_Release(p_block);
        return NULL;
    }

    xml::DOMParser parser(memorystream);
    if(!parser.parse(true))
    {
        stream_Delete(memorystream);
        block_Release(p_block);
        return NULL;
    }

    Manifest *manifest = NULL;

    ManifestParser *manifestParser = new (std::nothrow) ManifestParser(parser.getRootNode(), VLC_OBJECT(p_demux),
                                                                       memorystream, playlisturl);
    if(manifestParser)
    {
        manifest = manifestParser->parse();
        delete manifestParser;
    }

    stream_Delete(memorystream);
    block_Release(p_block);

    return manifest;
}

bool SmoothManager::updatePlaylist()
{
    return updatePlaylist(false);
}

void SmoothManager::scheduleNextUpdate()
{
    time_t now = time(NULL);

    mtime_t minbuffer = 0;
    std::vector<AbstractStream *>::const_iterator it;
    for(it=streams.begin(); it!=streams.end(); ++it)
    {
        const AbstractStream *st = *it;
        const mtime_t m = st->getMinAheadTime();
        if(m > 0 && (m < minbuffer || minbuffer == 0))
            minbuffer = m;
    }

    minbuffer /= 2;

    if(playlist->minUpdatePeriod.Get() > minbuffer)
        minbuffer = playlist->minUpdatePeriod.Get();

    if(minbuffer < 5 * CLOCK_FREQ)
        minbuffer = 5 * CLOCK_FREQ;

    nextPlaylistupdate = now + minbuffer / CLOCK_FREQ;

    msg_Dbg(p_demux, "Updated playlist, next update in %" PRId64 "s", (mtime_t) nextPlaylistupdate - now );
}

bool SmoothManager::needsUpdate() const
{
    if(nextPlaylistupdate && time(NULL) < nextPlaylistupdate)
        return false;

    return PlaylistManager::needsUpdate();
}

bool SmoothManager::updatePlaylist(bool forcemanifest)
{
    /* FIXME: do update from manifest after resuming from pause */

    /* Timelines updates should be inlined in tfrf atoms.
       We'll just care about pruning live timeline then. */

    if(forcemanifest && nextPlaylistupdate)
    {
        Manifest *newManifest = fetchManifest();
        if(newManifest)
        {
            playlist->mergeWith(newManifest, 0);
            delete newManifest;

#ifdef NDEBUG
            playlist->debug();
#endif
        }
        else return false;
    }

    pruneLiveStream();

    return true;
}

bool SmoothManager::reactivateStream(AbstractStream *stream)
{
    if(playlist->isLive())
        updatePlaylist(true);
    return PlaylistManager::reactivateStream(stream);
}

bool SmoothManager::isSmoothStreaming(xml::Node *root)
{
    return root->getName() == "SmoothStreamingMedia";
}

bool SmoothManager::mimeMatched(const std::string &mime)
{
    return (mime == "application/vnd.ms-sstr+xml");
}
