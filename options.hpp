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

/*!
  \file options.hpp
  \author Kris Beevers (beevek@cs.rpi.edu)
  \version $Id: options.hpp,v 1.10 2006/02/13 20:32:00 beevek Exp $

  \brief Functions and data structures that manage commandline and
  configuration file options; this is much simpler than boost's
  program_options library but is also a lot easier to use (plus, as of
  now, program_options isn't in the official boost release).  The
  "official" source of this is in my slam CVS repository, from which I
  stole it for this project.
 */


#ifndef _OPTIONS_HPP
#define _OPTIONS_HPP

#include <string.h>
#include <vector>
#include <string>
#include <iostream>
#include <typeinfo>

#ifdef _WIN32
  #define do_cast static_cast
#else
  #define do_cast dynamic_cast
#endif

namespace options
{

  // options have:
  //  - long option name (e.g. "option_name")
  //  - short option name (e.g. "o")
  //  - description
  //  - group (for sorting when printing usage information)
  //  - flag indicating whether the option is a boolean one
  //  - flag indicating if the option should be written to auto-generated conf files
  //  - default value

  struct option; // declared later

  // register an option; if bool, requires no argument, otherwise,
  // requires an argument
  template <class T>
  void add(const char *long_option, const char *short_option = 0,
	   const char *desc = 0, const char *group = 0,
	   T def = T(), bool dump = true);

  // get the value of an option
  template <class T>
  bool get(T &out, const char *long_option, const char *short_option = 0,
	   const char *group = 0);

  // return the value of an option (typically need to call this like
  // quickget<type>("option")
  template <class T>
  T quickget(const char *long_option, const char *short_option = 0, const char *group = 0);


  // set the value of an option
  template <class T>
  bool set(T val, const char *long_option, const char *short_option = 0,
	   const char *group = 0);

  // find an option
  option * find(const char *long_option, const char *short_option = 0);

  // dump all options to a configuration file
  void dump(std::ostream &out);

  extern const bool nodump;
  extern const bool dodump;

  // print options and descriptions (in a format suitable for "usage"
  // information)
  void print_options(std::ostream &out);

  // set the commandline flags for specifying a configuration file to
  // be read; if encountered while parsing the commandline, the
  // specified config file will be read; long is a full option, like
  // "config", short is like "c", which appear on the commandline
  // respectively as "--config <filename>" and "-c <filename"
  void set_cf_options(const char *long_option, const char *short_option);

  // read a configuration file
  bool read_file(std::istream &in);

  // parse the commandline; if one of the options specifies a
  // configuration file, read the file (options already set will be
  // replaced, and commandline options specified after the config file
  // will replace config file options); returns index in argv of the
  // first non-option element (e.g. input file)
  int parse_cmdline(int argc, char **argv);



  //////////////////////////////////////////////////////////////////////

  // implementation details

  struct option
  {
    char *longopt;
    char *shortopt;
    char *desc;
    char *group;
    bool is_boolean;
    bool dump;

    virtual ~option() {}
    virtual std::istream & read(std::istream &in) = 0;
    virtual std::ostream & write(std::ostream &out) const = 0;
    virtual const std::type_info & type() const = 0;
  };

  template <class T, class S>
  inline bool isoftype(S &s)
  {
    try {
      do_cast<T &>(s);
    } catch(std::bad_cast) {
      return false;
    }
    return true;
  }

  template <class T>
  struct option_t : public option
  {
    T value;

    option_t(const char *lo, const char *so, const char *d, const char *g,
	     const T &v, bool dmp = true)
    {
      // make local copies of the strings
      longopt = lo ? strdup(lo) : 0;
      shortopt = so ? strdup(so) : 0;
      if(shortopt && strlen(shortopt) > 1)
	shortopt[1] = 0;                  // shortopt can only be a single character
      group = g ? strdup(g) : strdup(""); // make sure there's always a group
      desc = d ? strdup(d) : 0;
      value = v;                          // set default
      is_boolean = (typeid(T) == typeid(bool)); // set flag
      dump = dmp;
    }

    virtual ~option_t()
    {
      if(longopt != 0) delete [] longopt;
      if(shortopt != 0) delete [] shortopt;
      if(desc != 0) delete [] desc;
      if(group != 0) delete [] group;
    }

    virtual std::istream & read(std::istream &in)
    {
      return in >> value;
    }

    virtual std::ostream & write(std::ostream &out) const
    {
      return out << value;
    }

    virtual const std::type_info & type() const
    {
      return typeid(T);
    }

  };

  // in options.cpp
  extern std::vector<option *> options;

  template <class T>
  void add(const char *long_option, const char *short_option,
	   const char *desc, const char *group,
	   T def, bool dump)
  {
    option *o = find(long_option, short_option);
    if(o) { // option is already there
      if(!isoftype< option_t<T> >(*o))
	throw std::bad_cast(); // it is a different type!
      option_t<T> *ot = do_cast<option_t<T> *>(o);
      if(ot->desc)
	delete [] ot->desc;
      ot->desc = desc ? strdup(desc) : 0;
      ot->value = def;
      ot->dump = dump;
      return;
    }
    options.push_back
      (new option_t<T>(long_option, short_option, desc, group, def, dump));
  }


  template <class T>
  bool get(T &out, const char *long_option, const char *short_option, const char *group)
  {
    option *opt = find(long_option, short_option);
    if(opt && isoftype< option_t<T> >(*opt)) {
      out = (do_cast<option_t<T> *>(opt))->value;
      return true;
    }
    return false; // not found or wrong type
  }


  template <class T>
  T quickget(const char *long_option, const char *short_option, const char *group)
  {
    T out = T();
    get(out, long_option, short_option, group);
    return out; // returns default T() if not found
  }


  template <class T>
  bool set(T val, const char *long_option, const char *short_option,
	   const char *group)
  {
    option *opt = find(long_option, short_option);
    if(opt && isoftype< option_t<T> >(*opt)) {
      (do_cast<option_t<T> *>(opt))->value = val;
      return true;
    }
    return false; // not found or wrong type
  }

};

#endif // _OPTIONS_HPP
