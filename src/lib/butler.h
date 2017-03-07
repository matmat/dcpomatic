/*
    Copyright (C) 2016-2017 Carl Hetherington <cth@carlh.net>

    This file is part of DCP-o-matic.

    DCP-o-matic is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    DCP-o-matic is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with DCP-o-matic.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "video_ring_buffers.h"
#include <boost/shared_ptr.hpp>
#include <boost/weak_ptr.hpp>
#include <boost/thread.hpp>
#include <boost/thread/condition.hpp>
#include <boost/signals2.hpp>

class Film;
class Player;
class PlayerVideo;

class Butler : public boost::noncopyable
{
public:
	Butler (boost::weak_ptr<const Film> film, boost::shared_ptr<Player> player);
	~Butler ();

	void seek (DCPTime position, bool accurate);
	std::pair<boost::shared_ptr<PlayerVideo>, DCPTime> get_video ();

private:
	void thread ();
	void video (boost::shared_ptr<PlayerVideo> video, DCPTime time);

	boost::weak_ptr<const Film> _film;
	boost::shared_ptr<Player> _player;
	boost::thread* _thread;

	VideoRingBuffers _video;

	boost::mutex _mutex;
	boost::condition _summon;
	boost::condition _arrived;
	boost::optional<DCPTime> _pending_seek_position;
	bool _pending_seek_accurate;

	bool _finished;

	boost::signals2::scoped_connection _player_video_connection;
};
