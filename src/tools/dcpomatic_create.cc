/*
    Copyright (C) 2013-2015 Carl Hetherington <cth@carlh.net>

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

#include "lib/version.h"
#include "lib/film.h"
#include "lib/util.h"
#include "lib/content_factory.h"
#include "lib/job_manager.h"
#include "lib/signal_manager.h"
#include "lib/job.h"
#include "lib/dcp_content_type.h"
#include "lib/ratio.h"
#include "lib/image_content.h"
#include <boost/filesystem.hpp>
#include <getopt.h>
#include <string>
#include <iostream>
#include <cstdlib>
#include <stdexcept>

using std::string;
using std::cout;
using std::cerr;
using std::list;
using std::exception;
using boost::shared_ptr;
using boost::dynamic_pointer_cast;

static void
syntax (string n)
{
	cerr << "Syntax: " << n << " [OPTION] <CONTENT> [<CONTENT> ...]\n"
	     << "  -v, --version                 show DCP-o-matic version\n"
	     << "  -h, --help                    show this help\n"
	     << "  -n, --name <name>             film name\n"
	     << "  -c, --dcp-content-type <type> FTR, SHR, TLR, TST, XSN, RTG, TSR, POL, PSA or ADV\n"
	     << "      --container-ratio <ratio> 119, 133, 137, 138, 166, 178, 185 or 239\n"
	     << "      --content-ratio <ratio>   119, 133, 137, 138, 166, 178, 185 or 239\n"
	     << "  -s, --still-length <n>        number of seconds that still content should last\n"
	     << "      --standard <standard>     SMPTE or interop (default SMPTE)\n"
	     << "      --no-use-isdcf-name       do not use an ISDCF name; use the specified name unmodified\n"
	     << "      --no-sign                 do not sign the DCP\n"
	     << "  -o, --output <dir>            output directory\n";
}

static void
help (string n)
{
	cerr << "Create a film directory (ready for making a DCP) or metadata file from some content files.\n"
	     << "A film directory will be created if -o or --output is specified, otherwise a metadata file\n"
	     << "will be written to stdout.\n";

	syntax (n);
}

class SimpleSignalManager : public SignalManager
{
public:
	/* Do nothing in this method so that UI events happen in our thread
	   when we call SignalManager::ui_idle().
	*/
	void wake_ui () {}
};

int
main (int argc, char* argv[])
{
	dcpomatic_setup_path_encoding ();
	dcpomatic_setup ();

	string name;
	DCPContentType const * dcp_content_type = DCPContentType::from_isdcf_name ("TST");
	Ratio const * container_ratio = 0;
	Ratio const * content_ratio = 0;
	int still_length = 10;
	dcp::Standard standard = dcp::SMPTE;
	boost::filesystem::path output;
	bool sign = true;
	bool use_isdcf_name = true;

	int option_index = 0;
	while (true) {
		static struct option long_options[] = {
			{ "version", no_argument, 0, 'v'},
			{ "help", no_argument, 0, 'h'},
			{ "name", required_argument, 0, 'n'},
			{ "dcp-content-type", required_argument, 0, 'c'},
			{ "container-ratio", required_argument, 0, 'A'},
			{ "content-ratio", required_argument, 0, 'B'},
			{ "still-length", required_argument, 0, 's'},
			{ "standard", required_argument, 0, 'C'},
			{ "no-use-isdcf-name", no_argument, 0, 'D'},
			{ "no-sign", no_argument, 0, 'E'},
			{ "output", required_argument, 0, 'o'},
			{ 0, 0, 0, 0}
		};

		int c = getopt_long (argc, argv, "vhn:c:A:B:C:s:o:DE", long_options, &option_index);
		if (c == -1) {
			break;
		}

		switch (c) {
		case 'v':
			cout << "dcpomatic version " << dcpomatic_version << " " << dcpomatic_git_commit << "\n";
			exit (EXIT_SUCCESS);
		case 'h':
			help (argv[0]);
			exit (EXIT_SUCCESS);
		case 'n':
			name = optarg;
			break;
		case 'c':
			dcp_content_type = DCPContentType::from_isdcf_name (optarg);
			if (dcp_content_type == 0) {
				cerr << "Bad DCP content type.\n";
				syntax (argv[0]);
				exit (EXIT_FAILURE);
			}
			break;
		case 'A':
			container_ratio = Ratio::from_id (optarg);
			if (container_ratio == 0) {
				cerr << "Bad container ratio.\n";
				syntax (argv[0]);
				exit (EXIT_FAILURE);
			}
			break;
		case 'B':
			content_ratio = Ratio::from_id (optarg);
			if (content_ratio == 0) {
				cerr << "Bad content ratio " << optarg << ".\n";
				syntax (argv[0]);
				exit (EXIT_FAILURE);
			}
			break;
		case 'C':
			if (strcmp (optarg, "interop") == 0) {
				standard = dcp::INTEROP;
			} else if (strcmp (optarg, "SMPTE") != 0) {
				cerr << "Bad standard " << optarg << ".\n";
				syntax (argv[0]);
				exit (EXIT_FAILURE);
			}
			break;
		case 'D':
			use_isdcf_name = false;
			break;
		case 'E':
			sign = false;
			break;
		case 's':
			still_length = atoi (optarg);
			break;
		case 'o':
			output = optarg;
			break;
		case '?':
			syntax (argv[0]);
			exit (EXIT_FAILURE);
		}
	}

	if (optind > argc) {
		help (argv[0]);
		exit (EXIT_FAILURE);
	}

	if (!content_ratio) {
		cerr << argv[0] << ": missing required option --content-ratio.\n";
		exit (EXIT_FAILURE);
	}

	if (!container_ratio) {
		container_ratio = content_ratio;
	}

	if (optind == argc) {
		cerr << argv[0] << ": no content specified.\n";
		exit (EXIT_FAILURE);
	}

	signal_manager = new SimpleSignalManager ();

	if (name.empty ()) {
		name = boost::filesystem::path (argv[optind]).leaf().string ();
	}

	try {
		shared_ptr<Film> film (new Film (output, false));
		film->set_name (name);

		film->set_container (container_ratio);
		film->set_dcp_content_type (dcp_content_type);
		film->set_interop (standard == dcp::INTEROP);
		film->set_use_isdcf_name (use_isdcf_name);
		film->set_signed (sign);

		for (int i = optind; i < argc; ++i) {
			shared_ptr<Content> c = content_factory (film, argv[i]);
			shared_ptr<VideoContent> vc = dynamic_pointer_cast<VideoContent> (c);
			if (vc) {
				vc->set_scale (VideoContentScale (content_ratio));
			}
			film->examine_and_add_content (c);
		}

		JobManager* jm = JobManager::instance ();

		while (jm->work_to_do ()) {}
		while (signal_manager->ui_idle() > 0) {}

		ContentList content = film->content ();
		for (ContentList::iterator i = content.begin(); i != content.end(); ++i) {
			shared_ptr<ImageContent> ic = dynamic_pointer_cast<ImageContent> (*i);
			if (ic) {
				ic->set_video_length (still_length * 24);
			}
		}

		if (jm->errors ()) {
			list<shared_ptr<Job> > jobs = jm->get ();
			for (list<shared_ptr<Job> >::iterator i = jobs.begin(); i != jobs.end(); ++i) {
				if ((*i)->finished_in_error ()) {
					cerr << (*i)->error_summary () << "\n"
					     << (*i)->error_details () << "\n";
				}
			}
			exit (EXIT_FAILURE);
		}

		if (!output.empty ()) {
			film->write_metadata ();
		} else {
			film->metadata()->write_to_stream_formatted (cout, "UTF-8");
		}
	} catch (exception& e) {
		cerr << argv[0] << ": " << e.what() << "\n";
		exit (EXIT_FAILURE);
	}

	return 0;
}
