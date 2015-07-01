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

#include "audio_analysis.h"
#include "audio_buffers.h"
#include "analyse_audio_job.h"
#include "compose.hpp"
#include "film.h"
#include "player.h"
#include "playlist.h"
#include <boost/foreach.hpp>

#include "i18n.h"

using std::string;
using std::max;
using std::min;
using std::cout;
using boost::shared_ptr;
using boost::dynamic_pointer_cast;

int const AnalyseAudioJob::_num_points = 1024;

AnalyseAudioJob::AnalyseAudioJob (shared_ptr<const Film> film, shared_ptr<const Playlist> playlist)
	: Job (film)
	, _playlist (playlist)
	, _done (0)
	, _samples_per_point (1)
	, _overall_peak (0)
	, _overall_peak_frame (0)
{

}

string
AnalyseAudioJob::name () const
{
	return _("Analyse audio");
}

string
AnalyseAudioJob::json_name () const
{
	return N_("analyse_audio");
}

void
AnalyseAudioJob::run ()
{
	shared_ptr<Player> player (new Player (_film, _playlist));
	player->set_ignore_video ();

	int64_t const len = _playlist->length().frames (_film->audio_frame_rate());
	_samples_per_point = max (int64_t (1), len / _num_points);

	_current.resize (_film->audio_channels ());
	_analysis.reset (new AudioAnalysis (_film->audio_channels ()));

	bool has_any_audio = false;
	BOOST_FOREACH (shared_ptr<Content> c, _playlist->content ()) {
		if (dynamic_pointer_cast<AudioContent> (c)) {
			has_any_audio = true;
		}
	}

	if (has_any_audio) {
		_done = 0;
		DCPTime const block = DCPTime::from_seconds (1.0 / 8);
		for (DCPTime t; t < _film->length(); t += block) {
			analyse (player->get_audio (t, block, false));
			set_progress (t.seconds() / _film->length().seconds());
		}
	}

	_analysis->set_peak (_overall_peak, DCPTime::from_frames (_overall_peak_frame, _film->audio_frame_rate ()));
	_analysis->write (_film->audio_analysis_path (_playlist));

	set_progress (1);
	set_state (FINISHED_OK);
}

void
AnalyseAudioJob::analyse (shared_ptr<const AudioBuffers> b)
{
	for (int i = 0; i < b->frames(); ++i) {
		for (int j = 0; j < b->channels(); ++j) {
			float s = b->data(j)[i];
			if (fabsf (s) < 10e-7) {
				/* SafeStringStream can't serialise and recover inf or -inf, so prevent such
				   values by replacing with this (140dB down) */
				s = 10e-7;
			}
			_current[j][AudioPoint::RMS] += pow (s, 2);
			_current[j][AudioPoint::PEAK] = max (_current[j][AudioPoint::PEAK], fabsf (s));

			float const as = fabs (s);

			_current[j][AudioPoint::PEAK] = max (_current[j][AudioPoint::PEAK], as);

			if (as > _overall_peak) {
				_overall_peak = as;
				_overall_peak_frame = _done + i;
			}

			if ((_done % _samples_per_point) == 0) {
				_current[j][AudioPoint::RMS] = sqrt (_current[j][AudioPoint::RMS] / _samples_per_point);
				_analysis->add_point (j, _current[j]);

				_current[j] = AudioPoint ();
			}
		}

		++_done;
	}
}
