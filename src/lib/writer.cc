/*
    Copyright (C) 2012-2015 Carl Hetherington <cth@carlh.net>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#include "writer.h"
#include "compose.hpp"
#include "film.h"
#include "ratio.h"
#include "log.h"
#include "dcp_video.h"
#include "dcp_content_type.h"
#include "audio_mapping.h"
#include "config.h"
#include "job.h"
#include "cross.h"
#include "audio_buffers.h"
#include "md5_digester.h"
#include "data.h"
#include "version.h"
#include "font.h"
#include "util.h"
#include "reel_writer.h"
#include <dcp/cpl.h>
#include <boost/foreach.hpp>
#include <fstream>
#include <cerrno>
#include <iostream>

#include "i18n.h"

#define LOG_GENERAL(...) _film->log()->log (String::compose (__VA_ARGS__), LogEntry::TYPE_GENERAL);
#define LOG_GENERAL_NC(...) _film->log()->log (__VA_ARGS__, LogEntry::TYPE_GENERAL);
#define LOG_DEBUG_ENCODE(...) _film->log()->log (String::compose (__VA_ARGS__), LogEntry::TYPE_DEBUG_ENCODE);
#define LOG_TIMING(...) _film->log()->log (String::compose (__VA_ARGS__), LogEntry::TYPE_TIMING);
#define LOG_WARNING_NC(...) _film->log()->log (__VA_ARGS__, LogEntry::TYPE_WARNING);
#define LOG_WARNING(...) _film->log()->log (String::compose (__VA_ARGS__), LogEntry::TYPE_WARNING);
#define LOG_ERROR(...) _film->log()->log (String::compose (__VA_ARGS__), LogEntry::TYPE_ERROR);

/* OS X strikes again */
#undef set_key

using std::make_pair;
using std::pair;
using std::string;
using std::list;
using std::cout;
using boost::shared_ptr;
using boost::weak_ptr;
using boost::dynamic_pointer_cast;

Writer::Writer (shared_ptr<const Film> film, weak_ptr<Job> j)
	: _film (film)
	, _job (j)
	, _thread (0)
	, _finish (false)
	, _queued_full_in_memory (0)
	, _maximum_frames_in_memory (0)
	, _full_written (0)
	, _fake_written (0)
	, _repeat_written (0)
	, _pushed_to_disk (0)
{
	/* Remove any old DCP */
	boost::filesystem::remove_all (_film->dir (_film->dcp_name ()));

	shared_ptr<Job> job = _job.lock ();
	DCPOMATIC_ASSERT (job);

	BOOST_FOREACH (DCPTimePeriod p, _film->reels ()) {
		_reels.push_back (ReelWriter (film, p, job));
	}

	/* We can keep track of the current audio and subtitle reels easily because audio
	   and subs arrive to the Writer in sequence.  This is not so for video.
	*/
	_audio_reel = _reels.begin ();
	_subtitle_reel = _reels.begin ();

	/* Check that the signer is OK if we need one */
	if (_film->is_signed() && !Config::instance()->signer_chain()->valid ()) {
		throw InvalidSignerError ();
	}

	job->sub (_("Encoding image data"));
}

void
Writer::start ()
{
	_thread = new boost::thread (boost::bind (&Writer::thread, this));
}

Writer::~Writer ()
{
	terminate_thread (false);
}

/** Pass a video frame to the writer for writing to disk at some point.
 *  This method can be called with frames out of order.
 *  @param encoded JPEG2000-encoded data.
 *  @param frame Frame index within the DCP.
 *  @param eyes Eyes that this frame image is for.
 */
void
Writer::write (Data encoded, Frame frame, Eyes eyes)
{
	boost::mutex::scoped_lock lock (_state_mutex);

	while (_queued_full_in_memory > _maximum_frames_in_memory) {
		/* The queue is too big; wait until that is sorted out */
		_full_condition.wait (lock);
	}

	QueueItem qi;
	qi.type = QueueItem::FULL;
	qi.encoded = encoded;
	qi.reel = video_reel (frame);
	qi.frame = frame - _reels[qi.reel].start ();

	if (_film->three_d() && eyes == EYES_BOTH) {
		/* 2D material in a 3D DCP; fake the 3D */
		qi.eyes = EYES_LEFT;
		_queue.push_back (qi);
		++_queued_full_in_memory;
		qi.eyes = EYES_RIGHT;
		_queue.push_back (qi);
		++_queued_full_in_memory;
	} else {
		qi.eyes = eyes;
		_queue.push_back (qi);
		++_queued_full_in_memory;
	}

	/* Now there's something to do: wake anything wait()ing on _empty_condition */
	_empty_condition.notify_all ();
}

bool
Writer::can_repeat (Frame frame) const
{
	return frame > _reels[video_reel(frame)].start();
}

/** Repeat the last frame that was written to a reel as a new frame.
 *  @param frame Frame index within the DCP of the new (repeated) frame.
 *  @param eyes Eyes that this repeated frame image is for.
 */
void
Writer::repeat (Frame frame, Eyes eyes)
{
	boost::mutex::scoped_lock lock (_state_mutex);

	while (_queued_full_in_memory > _maximum_frames_in_memory) {
		/* The queue is too big; wait until that is sorted out */
		_full_condition.wait (lock);
	}

	QueueItem qi;
	qi.type = QueueItem::REPEAT;
	qi.reel = video_reel (frame);
	qi.frame = frame - _reels[qi.reel].start ();
	if (_film->three_d() && eyes == EYES_BOTH) {
		qi.eyes = EYES_LEFT;
		_queue.push_back (qi);
		qi.eyes = EYES_RIGHT;
		_queue.push_back (qi);
	} else {
		qi.eyes = eyes;
		_queue.push_back (qi);
	}

	/* Now there's something to do: wake anything wait()ing on _empty_condition */
	_empty_condition.notify_all ();
}

void
Writer::fake_write (Frame frame, Eyes eyes)
{
	boost::mutex::scoped_lock lock (_state_mutex);

	while (_queued_full_in_memory > _maximum_frames_in_memory) {
		/* The queue is too big; wait until that is sorted out */
		_full_condition.wait (lock);
	}

	size_t const reel = video_reel (frame);
	Frame const reel_frame = frame - _reels[reel].start ();

	FILE* file = fopen_boost (_film->info_file(_reels[reel].period()), "rb");
	if (!file) {
		throw ReadFileError (_film->info_file(_reels[reel].period()));
	}
	dcp::FrameInfo info = _reels[reel].read_frame_info (file, reel_frame, eyes);
	fclose (file);

	QueueItem qi;
	qi.type = QueueItem::FAKE;
	qi.size = info.size;
	qi.reel = reel;
	qi.frame = reel_frame;
	if (_film->three_d() && eyes == EYES_BOTH) {
		qi.eyes = EYES_LEFT;
		_queue.push_back (qi);
		qi.eyes = EYES_RIGHT;
		_queue.push_back (qi);
	} else {
		qi.eyes = eyes;
		_queue.push_back (qi);
	}

	/* Now there's something to do: wake anything wait()ing on _empty_condition */
	_empty_condition.notify_all ();
}

/** Write one video frame's worth of audio frames to the DCP.
 *  @param audio Audio data or 0 if there is no audio to be written here (i.e. it is referenced).
 *  This method is not thread safe.
 */
void
Writer::write (shared_ptr<const AudioBuffers> audio)
{
	if (_audio_reel == _reels.end ()) {
		/* This audio is off the end of the last reel; ignore it */
		return;
	}

	_audio_reel->write (audio);

	/* written is in video frames, not audio frames */
	if (_audio_reel->total_written_audio_frames() >= _audio_reel->period().duration().frames_floor (_film->video_frame_rate())) {
		++_audio_reel;
	}
}

/** This must be called from Writer::thread() with an appropriate lock held */
bool
Writer::have_sequenced_image_at_queue_head ()
{
	if (_queue.empty ()) {
		return false;
	}

	_queue.sort ();

	QueueItem const & f = _queue.front();
	ReelWriter const & reel = _reels[f.reel];

	/* The queue should contain only EYES_LEFT/EYES_RIGHT pairs or EYES_BOTH */

	if (f.eyes == EYES_BOTH) {
		/* 2D */
		return f.frame == (reel.last_written_video_frame() + 1);
	}

	/* 3D */

	if (reel.last_written_eyes() == EYES_LEFT && f.frame == reel.last_written_video_frame() && f.eyes == EYES_RIGHT) {
		return true;
	}

	if (reel.last_written_eyes() == EYES_RIGHT && f.frame == (reel.last_written_video_frame() + 1) && f.eyes == EYES_LEFT) {
		return true;
	}

	return false;
}

void
Writer::thread ()
try
{
	while (true)
	{
		boost::mutex::scoped_lock lock (_state_mutex);

		while (true) {

			if (_finish || _queued_full_in_memory > _maximum_frames_in_memory || have_sequenced_image_at_queue_head ()) {
				/* We've got something to do: go and do it */
				break;
			}

			/* Nothing to do: wait until something happens which may indicate that we do */
			LOG_TIMING (N_("writer-sleep queue=%1"), _queue.size());
			_empty_condition.wait (lock);
			LOG_TIMING (N_("writer-wake queue=%1"), _queue.size());
		}

		if (_finish && _queue.empty()) {
			return;
		}

		/* We stop here if we have been asked to finish, and if either the queue
		   is empty or we do not have a sequenced image at its head (if this is the
		   case we will never terminate as no new frames will be sent once
		   _finish is true).
		*/
		if (_finish && (!have_sequenced_image_at_queue_head() || _queue.empty())) {
			/* (Hopefully temporarily) log anything that was not written */
			if (!_queue.empty() && !have_sequenced_image_at_queue_head()) {
				LOG_WARNING (N_("Finishing writer with a left-over queue of %1:"), _queue.size());
				for (list<QueueItem>::const_iterator i = _queue.begin(); i != _queue.end(); ++i) {
					if (i->type == QueueItem::FULL) {
						LOG_WARNING (N_("- type FULL, frame %1, eyes %2"), i->frame, i->eyes);
					} else {
						LOG_WARNING (N_("- type FAKE, size %1, frame %2, eyes %3"), i->size, i->frame, i->eyes);
					}
				}
			}
			return;
		}

		/* Write any frames that we can write; i.e. those that are in sequence. */
		while (have_sequenced_image_at_queue_head ()) {
			QueueItem qi = _queue.front ();
			_queue.pop_front ();
			if (qi.type == QueueItem::FULL && qi.encoded) {
				--_queued_full_in_memory;
			}

			lock.unlock ();

			ReelWriter& reel = _reels[qi.reel];

			switch (qi.type) {
			case QueueItem::FULL:
				LOG_DEBUG_ENCODE (N_("Writer FULL-writes %1 (%2)"), qi.frame, qi.eyes);
				if (!qi.encoded) {
					qi.encoded = Data (_film->j2c_path (qi.reel, qi.frame, qi.eyes, false));
				}
				reel.write (qi.encoded, qi.frame, qi.eyes);
				++_full_written;
				break;
			case QueueItem::FAKE:
				LOG_DEBUG_ENCODE (N_("Writer FAKE-writes %1"), qi.frame);
				reel.fake_write (qi.frame, qi.eyes, qi.size);
				++_fake_written;
				break;
			case QueueItem::REPEAT:
				LOG_DEBUG_ENCODE (N_("Writer REPEAT-writes %1"), qi.frame);
				reel.repeat_write (qi.frame, qi.eyes);
				++_repeat_written;
				break;
			}

			lock.lock ();

			shared_ptr<Job> job = _job.lock ();
			DCPOMATIC_ASSERT (job);
			int64_t total = _film->length().frames_round (_film->video_frame_rate ());
			if (_film->three_d ()) {
				/* _full_written and so on are incremented for each eye, so we need to double the total
				   frames to get the correct progress.
				*/
				total *= 2;
			}
			if (total) {
				job->set_progress (float (_full_written + _fake_written + _repeat_written) / total);
			}
		}

		while (_queued_full_in_memory > _maximum_frames_in_memory) {
			/* Too many frames in memory which can't yet be written to the stream.
			   Write some FULL frames to disk.
			*/

			/* Find one from the back of the queue */
			_queue.sort ();
			list<QueueItem>::reverse_iterator i = _queue.rbegin ();
			while (i != _queue.rend() && (i->type != QueueItem::FULL || !i->encoded)) {
				++i;
			}

			DCPOMATIC_ASSERT (i != _queue.rend());
			++_pushed_to_disk;
			lock.unlock ();

			/* i is valid here, even though we don't hold a lock on the mutex,
			   since list iterators are unaffected by insertion and only this
			   thread could erase the last item in the list.
			*/

			LOG_GENERAL ("Writer full; pushes %1 to disk", i->frame);

			i->encoded->write_via_temp (
				_film->j2c_path (i->reel, i->frame, i->eyes, true),
				_film->j2c_path (i->reel, i->frame, i->eyes, false)
				);

			lock.lock ();
			i->encoded.reset ();
			--_queued_full_in_memory;
		}

		/* The queue has probably just gone down a bit; notify anything wait()ing on _full_condition */
		_full_condition.notify_all ();
	}
}
catch (...)
{
	store_current ();
}

void
Writer::terminate_thread (bool can_throw)
{
	boost::mutex::scoped_lock lock (_state_mutex);
	if (_thread == 0) {
		return;
	}

	_finish = true;
	_empty_condition.notify_all ();
	_full_condition.notify_all ();
	lock.unlock ();

	DCPOMATIC_ASSERT (_thread->joinable ());
 	_thread->join ();
	if (can_throw) {
		rethrow ();
	}

	delete _thread;
	_thread = 0;
}

void
Writer::finish ()
{
	if (!_thread) {
		return;
	}

	terminate_thread (true);

	BOOST_FOREACH (ReelWriter& i, _reels) {
		i.finish ();
	}

	dcp::DCP dcp (_film->dir (_film->dcp_name()));

	shared_ptr<dcp::CPL> cpl (
		new dcp::CPL (
			_film->dcp_name(),
			_film->dcp_content_type()->libdcp_kind ()
			)
		);

	dcp.add (cpl);

	BOOST_FOREACH (ReelWriter& i, _reels) {

		cpl->add (i.create_reel (_reel_assets, _fonts));

		shared_ptr<Job> job = _job.lock ();
		DCPOMATIC_ASSERT (job);
		i.calculate_digests (job);
	}

	dcp::XMLMetadata meta;
	meta.creator = Config::instance()->dcp_creator ();
	if (meta.creator.empty ()) {
		meta.creator = String::compose ("DCP-o-matic %1 %2", dcpomatic_version, dcpomatic_git_commit);
	}
	meta.issuer = Config::instance()->dcp_issuer ();
	if (meta.issuer.empty ()) {
		meta.issuer = String::compose ("DCP-o-matic %1 %2", dcpomatic_version, dcpomatic_git_commit);
	}
	meta.set_issue_date_now ();

	cpl->set_metadata (meta);

	shared_ptr<const dcp::CertificateChain> signer;
	if (_film->is_signed ()) {
		signer = Config::instance()->signer_chain ();
		/* We did check earlier, but check again here to be on the safe side */
		if (!signer->valid ()) {
			throw InvalidSignerError ();
		}
	}

	dcp.write_xml (_film->interop () ? dcp::INTEROP : dcp::SMPTE, meta, signer);

	LOG_GENERAL (
		N_("Wrote %1 FULL, %2 FAKE, %3 REPEAT, %4 pushed to disk"), _full_written, _fake_written, _repeat_written, _pushed_to_disk
		);
}

/** @param frame Frame index within the whole DCP.
 *  @return true if we can fake-write this frame.
 */
bool
Writer::can_fake_write (Frame frame) const
{
	/* We have to do a proper write of the first frame so that we can set up the JPEG2000
	   parameters in the asset writer.
	*/

	ReelWriter const & reel = _reels[video_reel(frame)];

	/* Make frame relative to the start of the reel */
	frame -= reel.start ();
	return (frame != 0 && frame < reel.first_nonexistant_frame());
}

void
Writer::write (PlayerSubtitles subs)
{
	if (subs.text.empty ()) {
		return;
	}

	if (_subtitle_reel->period().to <= subs.from) {
		++_subtitle_reel;
	}

	_subtitle_reel->write (subs);
}

void
Writer::write (list<shared_ptr<Font> > fonts)
{
	/* Just keep a list of fonts and we'll deal with them in ::finish */
	copy (fonts.begin (), fonts.end (), back_inserter (_fonts));
}

bool
operator< (QueueItem const & a, QueueItem const & b)
{
	if (a.reel != b.reel) {
		return a.reel < b.reel;
	}

	if (a.frame != b.frame) {
		return a.frame < b.frame;
	}

	return static_cast<int> (a.eyes) < static_cast<int> (b.eyes);
}

bool
operator== (QueueItem const & a, QueueItem const & b)
{
	return a.reel == b.reel && a.frame == b.frame && a.eyes == b.eyes;
}

void
Writer::set_encoder_threads (int threads)
{
	_maximum_frames_in_memory = lrint (threads * 1.1);
}

void
Writer::write (ReferencedReelAsset asset)
{
	_reel_assets.push_back (asset);
}

size_t
Writer::video_reel (int frame) const
{
	DCPTime t = DCPTime::from_frames (frame, _film->video_frame_rate ());
	size_t i = 0;
	while (i < _reels.size() && !_reels[i].period().contains (t)) {
		++i;
	}

	DCPOMATIC_ASSERT (i < _reels.size ());
	return i;
}
