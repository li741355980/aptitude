// cmdline_prompt.h                     -*-c++-*-
//
//  Copyright 2004 Daniel Burrows

#ifndef CMDLINE_PROMPT_H
#define CMDLINE_PROMPT_H

#include "cmdline_common.h"

#include "terminal.h"

#include <boost/shared_ptr.hpp>
#include <cwidget/generic/util/exception.h>

/** \file cmdline_prompt.h
 */

class pkgPolicy;

/** Thrown when we get EOF on stdin.  Should never be thrown
 *  to the cwidget::toplevel.
 */
class StdinEOFException : public cwidget::util::Exception
{
public:
  std::string errmsg() const;
};

/** The main preview-prompt-adjust-preview loop for the command-line
 *  interface.  Displays a preview of what will happen, then allows
 *  the user to confirm, cancel, or perform a number of other actions.
 *  to_install and friends are meant to be the sets the user
 *  explicitly selected (so the prompt can be displayed only when
 *  extra stuff is added or removed).
 *
 *  \param as_upgrade currently ignored; meant to control how the
 *                    preview and prompting are handled.
 *  \param to_install a set of packages to install.
 *  \param to_hold a set of packages to hold.
 *  \param to_remove a set of packages to remove.
 *  \param to_purge a set of packages to purge.
 *  \param showvers \b true to show version information in the preview.
 *  \param showdeps \b true to show dependency information in the preview.
 *  \param showsize \b true to show size information in the preview.
 *  \param showwhy  \b true to show the root cause of automatic
 *                  installs/removals.
 *  \param verbose the current verbosity level
 *  \param assume_yes if \b true, assume the user entered "yes"
 *                    at the prompt.
 *  \param force_no_change if \b true, try extra-hard to preserve
 *                         the user's explicit requests (as
 *                         specified in to_install et al)
 *  \param policy a policy object, used to look up version priorities.
 *  \param arch_only if \b true, when the user asks to have build-dependencies
 *  for a package installed, only the architecture-dependent dependencies
 *  will be considered.
 *  \param term   The terminal on which to display prompts and receive input.
 *
 *  \throws StdinEOFException
 */
bool cmdline_do_prompt(bool as_upgrade,
		       pkgset &to_install,
		       pkgset &to_hold,
		       pkgset &to_remove,
		       pkgset &to_purge,
		       bool showvers,
		       bool showdeps,
		       bool showsize,
		       bool showwhy,
		       bool always_prompt,
		       int verbose,
		       bool assume_yes,
		       bool force_no_change,
		       pkgPolicy &policy,
		       bool arch_only,
                       const boost::shared_ptr<aptitude::cmdline::terminal> &term);

bool cmdline_show_preview(bool as_upgrade, pkgset &to_install,
			  pkgset &to_hold, pkgset &to_remove,
			  bool showvers, bool showdeps,
			  bool showsize, bool showwhy,
			  int verbose,
                          const boost::shared_ptr<aptitude::cmdline::terminal> &term);

/** Prompt for a single line of input from the user.
 *
 *  \param prompt a message to display before reading input.
 *  \return the text the user entered
 *
 *  \throws StdinEOFException
 */
string prompt_string(const string &prompt);


#endif // CMDLINE_PROMPT_H
