/*
  Copyright 2008-2013 Kristopher R Beevers.

  Permission is hereby granted, free of charge, to any person
  obtaining a copy of this software and associated documentation files
  (the "Software"), to deal in the Software without restriction,
  including without limitation the rights to use, copy, modify, merge,
  publish, distribute, sublicense, and/or sell copies of the Software,
  and to permit persons to whom the Software is furnished to do so,
  subject to the following conditions:

  The above copyright notice and this permission notice shall be
  included in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
  ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
  CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/

/*
  This file is part of mpro.

  $Id: options.cpp,v 1.11 2006/02/13 20:31:59 beevek Exp $

  Copyright (c) 2006 Kris Beevers
*/

/*!
  \file options.cpp
  \author Kris Beevers (beevek@cs.rpi.edu)
  \version $Id: options.cpp,v 1.11 2006/02/13 20:31:59 beevek Exp $

  \brief Commandline and configuration file options: implementation
  details.  The "official" source of this is in my slam CVS
  repository, from which I stole it for this project.
 */

#include "options.hpp"
#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>

using namespace std;

namespace options
{

  const bool nodump = false;
  const bool dodump = true;

  // global options list
  vector<option *> options;

  // config file commandline options
  char *long_cf = 0;
  char *short_cf = 0;


  struct ltgroup
  {
    bool operator()(const option *o1, const option *o2) const
    {
      assert(o1->group != 0 && o2->group != 0);
      return strcmp(o1->group, o2->group) < 0;
    }
  };
  void sort_by_group()
  {
    sort(options.begin(), options.end(), ltgroup());
  }


  option * find(const char *long_option, const char *short_option)
  {
    for(uint32_t i = 0; i < options.size(); ++i)
      if((long_option && options[i]->longopt &&
	  strcmp(long_option, options[i]->longopt) == 0) ||
	 (short_option && options[i]->shortopt &&
	  strcmp(short_option, options[i]->shortopt) == 0))
	return options[i];
    return 0;
  }


  void dump(ostream &out)
  {
    out << "##" << endl << "## Automatically generated configuration file"
	<< endl << "##" << endl << endl;

    sort_by_group();

    const char *pgrp = "";
    for(uint32_t i = 0; i < options.size(); ++i) {
      if(!options[i]->dump)
	continue;

      if(strcmp(options[i]->group, pgrp) != 0) { // new option group
	out << "##" << endl << "# " << options[i]->group << endl << "##" << endl << endl;
	pgrp = options[i]->group;
      }

      out << "# " << options[i]->longopt << endl;
      // FIXME: find some way to output the type of the option?
      if(options[i]->desc != 0)
	out << "# " << options[i]->desc << endl;
      out << endl << options[i]->longopt << " = ";
      options[i]->write(out);
      out << endl << endl;
    }
  }


  // FIXME: wrap descriptions nicely?
  void print_options(ostream &out)
  {
    sort_by_group();

    out << "Options:" << endl;

    const char *pgrp = "";
    uint32_t i, maxlen = 0, len;

    // find longest longopt + shortopt string
    for(i = 0; i < options.size(); ++i) {
      len = 2 + (options[i]->longopt ? strlen(options[i]->longopt) + 2 : 0);
      len += options[i]->shortopt ? 2 : 0;
      if(len > maxlen)
	maxlen = len;
    }

    for(i = 0; i < options.size(); ++i) {
      if(strcmp(options[i]->group, pgrp) != 0) { // new option group
	out << endl << options[i]->group << ":" << endl;
	pgrp = options[i]->group;
      }

      // commandline flags
      out << "  ";
      if(options[i]->longopt)
	out << "--" << options[i]->longopt;
      if(options[i]->shortopt) {
	if(options[i]->longopt)
	  out << ",";
	out << "-" << options[i]->shortopt;
      }

      if(options[i]->desc != 0) {
	// space so descriptions are aligned
	len = 2 + (options[i]->longopt ? strlen(options[i]->longopt) + 2 : 0);
	len += options[i]->shortopt ? 3 : 0;
	for(uint32_t j = 0; j < maxlen + 2 - len; ++j)
	  out << " ";
	out << options[i]->desc;
      }
      out << endl;
    }
  }


  void set_cf_options(const char *long_option, const char *short_option)
  {
    long_cf = long_option ? strdup(long_option) : 0;
    short_cf = short_option ? strdup(short_option) : 0;
    if(long_cf)
      add(long_cf, short_cf, "Specify configuration file", 0, string(""), nodump);
  }


  inline void skip_rest_of_line(istream &in)
  {
    in.ignore(100000000, '\n');
  }

  void skip_white_comment(istream &in)
  {
    char c;
    while(!in.eof()) {
      in >> c;
      if(c == '#') // comment
	skip_rest_of_line(in);
      else if(!isspace(c)) {
	in.putback(c);
	break;
      }
    }
  }

  bool read_file(istream &in)
  {
    string optname, discard;
    char equal;
    while(!in.eof()) {
      skip_white_comment(in);

      in >> optname; // read option name
      if(in.eof() || in.bad())
	return true;
      option *opt = find(optname.c_str());
      if(!opt) {
	cerr << "Warning: ignoring unknown option " << optname << endl;
	skip_rest_of_line(in);
	continue;
      }

      skip_white_comment(in);
      in >> equal;   // read equal sign
      if(equal != '=') {
	cerr << "Syntax error in configuration file (no '=')" << endl;
	return false;
      }

      opt->read(in);
      if(in.bad()) {
	cerr << "Error reading value for option " << optname << endl;
	return false;
      }
      skip_rest_of_line(in);
    }
    return true;
  }


  int parse_cmdline(int argc, char **argv)
  {
    // first set up options list for getopt_long
    typedef struct ::option loption;
    loption *long_opts = new loption[options.size() + 1];
    memset(long_opts, 0, sizeof(loption) * (options.size() + 1));
    char *short_opts = new char[2 * options.size() + 1];
    memset(short_opts, 0, sizeof(char) * (2 * options.size() + 1));

    uint32_t i;
    for(i = 0; i < options.size(); ++i) {
      long_opts[i].name = options[i]->longopt;
      long_opts[i].has_arg = options[i]->is_boolean ? 0 : 1;
      long_opts[i].flag = 0;
      if(options[i]->shortopt != 0) {
	long_opts[i].val = options[i]->shortopt[0];
	strcat(short_opts, options[i]->shortopt);
	if(!options[i]->is_boolean)
	  strcat(short_opts, ":");
      } else
	long_opts[i].val = 256 + i; // ensure no overlap with short options
    }

    // now process the commandline
    int c, option_idx;
    while(1) {
      c = getopt_long(argc, argv, short_opts, long_opts, &option_idx);
      if(c < 0)
	break; // done

      if(c == '?') { // unknown option
	optind = -1;
	break;
      }

      // find the option and handle it
      for(i = 0; i < options.size(); ++i)
	if(c == int(256 + i) || (options[i]->shortopt != 0 && c == options[i]->shortopt[0]))
	  break;

      assert(i < options.size());
      if(options[i]->is_boolean)
	(do_cast<option_t<bool> *>(options[i]))->value = true; // set flag
      else {
	// read in option
	stringstream ss(optarg, stringstream::in);
	options[i]->read(ss);
	if(ss.fail()) {
	  cerr << "Error reading value for option --" << *options[i]->longopt;
	  if(options[i]->shortopt)
	    cerr << " (-" << options[i]->shortopt << ")";
	  cerr << endl;
	  optind = -1; // error reading option
	  break;
	}

	// if one of long_cf or short_cf is specified on the
	// commandline, call read_file with the argument
	if((long_cf && options[i]->longopt &&
	    strcmp(options[i]->longopt, long_cf) == 0) ||
	   (short_cf && options[i]->shortopt &&
	    strcmp(options[i]->shortopt, short_cf) == 0))
	  {
	    string * filename = &((do_cast<option_t<string> *>(options[i]))->value);
	    ifstream conf(filename->c_str());
	    if(!conf || !read_file(conf)) {
	      cerr << "Error reading " << options[i]->longopt << endl;
	      optind = -1; // error reading configuration file
	      break;
	    }
	  }
      }
    }

    delete [] long_opts;
    delete [] short_opts;
    return optind;
  }

};
