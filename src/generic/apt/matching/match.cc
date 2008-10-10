// match.cc
//
//   Copyright (C) 2008 Daniel Burrows
//
//   This program is free software; you can redistribute it and/or
//   modify it under the terms of the GNU General Public License as
//   published by the Free Software Foundation; either version 2 of
//   the License, or (at your option) any later version.
//
//   This program is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//   General Public License for more details.
//
//   You should have received a copy of the GNU General Public License
//   along with this program; see the file COPYING.  If not, write to
//   the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
//   Boston, MA 02111-1307, USA.

#include "match.h"

#include <generic/apt/apt.h>
#include <generic/apt/tags.h>
#include <generic/apt/tasks.h>

#include <apt-pkg/pkgrecords.h>
#include <apt-pkg/pkgsystem.h>
#include <apt-pkg/version.h>

#include <cwidget/generic/util/transcode.h>

#include <ept/textsearch/textsearch.h>
#include <xapian/enquire.h>

#include <algorithm>

#include "serialize.h"

using cwidget::util::transcode;
using cwidget::util::ref_ptr;

namespace aptitude
{
  namespace matching
  {
    namespace
    {
      // Information on the Xapian compilation of a top-level term.
      // Note that for correct results in the presence of variable
      // binding constructs, we rely on the fact that those constructs
      // never interact with the Xapian-level search (so it's safe to
      // search once and save the results).
      class xapian_info
      {
      private:
	// Read out the hits in the given MSet into the matched_terms
	// list.
	void record_hits(Xapian::Enquire enq,
			 Xapian::MSet mset);

	// If true, the term was compiled to a Xapian query whose
	// results are stored in matched_packages; only packages in
	// that set could possibly match the pattern.  If false, we'll
	// have to examine all possible packages to find a match.
	bool matched_packages_valid;

	// The actual match set.
	Xapian::MSet xapian_match;

	// The match set resolved to package iterators and sorted for
	// fast access.  (using a flat list instead of std::set
	// because this will be hit a lot during a search and is
	// potentially large)
	std::vector<Xapian::docid> matched_packages;

      public:
	/** \brief Return \b true if this pattern can be used to
	 *  constrain the set of packages to search.
	 */
	bool get_matched_packages_valid() const
	{
	  return matched_packages_valid;
	}

	const Xapian::MSet get_xapian_match() const
	{
	  return xapian_match;
	}

	/** \brief Return the set of packages matched by the top-level
	 *  search (makes no sense if get_matched_packages_valid() is
	 *  false).
	 *
	 *  \todo Should really use a vector that projects through a
	 *  "look up the docid" function.
	 */
	const std::vector<Xapian::docid> &get_matched_packages() const
	{
	  return matched_packages;
	}

	/** \brief Return \b true if the given package might be
	 *  matched by this pattern.
	 */
	bool maybe_contains_package(const pkgCache::PkgIterator &pkg,
				    const ept::textsearch::TextSearch &db) const
	{
	  if(!matched_packages_valid)
	    return true;
	  else
	    return std::binary_search(matched_packages.begin(),
				      matched_packages.end(),
				      db.docidByName(pkg.Name()));
	}

	xapian_info()
	  : matched_packages_valid(false)
	{
	}

	/** \brief Compile a Xapian query from the given
	 *  pattern and execute it.
	 */
	void setup(const Xapian::Database &db,
		   const ref_ptr<pattern> &pattern,
		   bool debug);
      };
    }

    // We could try a fancy scheme where arbitrary values are attached
    // to each pattern and downcast using dynamic_cast, but I opted
    // for just explicitly listing all the possible caches in one
    // place.  This fits better with the architecture of the match
    // language and means that all the caching information is
    // collected in one place.
    class search_cache::implementation : public search_cache
    {
      // \todo this is wrong; it should be indexed by pattern like
      // toplevel_xapian_info.
      std::map<aptitudeDepCache::user_tag,
	       ref_ptr<match> > user_tag_matches;

      ept::textsearch::TextSearch db;

      // Maps "top-level" patterns to their Xapian information (that
      // is, the corresponding query and/or query results).  Term hit
      // lists are stored in matched_terms.  The information for a
      // top-level term is filled in the first time it's encountered.
      std::map<ref_ptr<pattern>, xapian_info> toplevel_xapian_info;

      // Maps each term that has been looked up to a sorted list of
      // the packages it matches.
      std::map<std::string, std::vector<Xapian::docid> > matched_terms;

    public:
      implementation()
      {
      }

      const ept::textsearch::TextSearch &get_db() const
      {
	return db;
      }

      // TODO: this is all wrong and needs to be fixed.
      std::map<aptitudeDepCache::user_tag, ref_ptr<match> > &get_user_tag_matches()
      {
	return user_tag_matches;
      }
			    

      bool term_matches(const pkgCache::PkgIterator &pkg,
			const std::string &term,
			bool debug)
      {
	Xapian::docid pkg_docid(db.docidByName(pkg.Name()));

	const std::map<std::string, std::vector<Xapian::docid> >::iterator
	  found = matched_terms.find(term);

	if(found != matched_terms.end())
	  return std::binary_search(found->second.begin(),
				    found->second.end(),
				    pkg_docid);
	else
	  {
	    if(debug)
	      std::cout << "Retrieving the hits for " << term << std::endl;

	    std::map<std::string, std::vector<Xapian::docid> >::iterator
	      inserted = matched_terms.insert(found, std::make_pair(term, std::vector<Xapian::docid>()));
	    std::vector<Xapian::docid> &matches(inserted->second);

	    // Index the stemmed version of the term too.
	    std::string termStemmed = Xapian::Stem("en")(term);
	    const std::string *terms[2] = { &term, &termStemmed };
	    const int numTerms = sizeof(terms) / sizeof(terms[0]);

	    for(const std::string **termIt = terms; termIt < terms + numTerms; ++termIt)
	      {
		const std::string &currTerm(**termIt);

		Xapian::Database xapian_db(db.db());

		Xapian::PostingIterator
		  postingsBegin = xapian_db.postlist_begin(currTerm),
		  postingsEnd   = xapian_db.postlist_end(currTerm);

		for(Xapian::PostingIterator it = postingsBegin;
		    it != postingsEnd; ++it)
		  matches.push_back(*it);
	      }

	    std::sort(matches.begin(), matches.end());
	    matches.erase(std::unique(matches.begin(), matches.end()),
			  matches.end());

	    if(debug)
	      std::cout << "  (" << matches.size() << " hits)" << std::endl;

	    // Now that the hit list is initialized, we can search it
	    // for a match.
	    return std::binary_search(matches.begin(),
				      matches.end(),
				      pkg_docid);
	  }
      }

      const xapian_info &get_toplevel_xapian_info(const ref_ptr<pattern> &toplevel,
						  bool debug)
      {
	std::map<ref_ptr<pattern>, xapian_info>::iterator found =
	  toplevel_xapian_info.find(toplevel);

	if(found == toplevel_xapian_info.end())
	  {
	    std::map<ref_ptr<pattern>, xapian_info>::iterator inserted =
	      toplevel_xapian_info.insert(found, std::make_pair(toplevel, xapian_info()));
	    xapian_info &rval(inserted->second);
	    rval.setup(db.db(), toplevel, debug);

	    return rval;
	  }
	else
	  return found->second;
      }
    };
 
    search_cache::search_cache()
    {
    }

    ref_ptr<search_cache> search_cache::create()
    {
      return new implementation;
    }

    namespace
    {
      Xapian::Query stem_term(const std::string &term)
      {
	return Xapian::Query(Xapian::Query::OP_OR,
			     Xapian::Query(term),
			     Xapian::Stem("en")(term));
      }

      /** \brief Describes how version-by-version matching is carried
       *  out.
       */
      enum structural_eval_mode
	{
	  /** \brief All the versions in the current pool must match. */
	  structural_eval_all,

	  /** \brief Any one of the versions in the current pool can match. */
	  structural_eval_any
	};

      void print_matchable(std::ostream &out,
			   const matchable &matchable,
			   aptitudeDepCache &cache)
      {
	out << matchable.get_package_iterator(cache).Name();
	if(matchable.get_has_version())
	  {
	    out << " "
		<< matchable.get_version_iterator(cache).VerStr();
	  }
      }

      void print_pool(std::ostream &out,
		      const std::vector<matchable> &pool,
		      aptitudeDepCache &cache)
      {
	out << "{";
	for(std::vector<matchable>::const_iterator it =
	      pool.begin(); it != pool.end(); ++it)
	  {
	    if(it != pool.begin())
	      out << ", ";

	    print_matchable(out, *it, cache);
	  }
	out << "}";
      }

      // The evaluation stack holds references to pools (sorted lists
      // of matchables).
      //
      // NB: this is safe only because references to captured
      // variables can't escape (because you can't, e.g., get a handle
      // to a lambda and return it -- the only naming construct forces
      // the variables to be referred to in the dynamic scope of the
      // construct).  If this weren't the case, we'd need to
      // reference-count the values on the stack -- and if lambdas
      // could end up on the stack themselves, we'd have to fall back
      // to full garbage-collection (e.g., mark-and-sweep).
      typedef std::vector<const std::vector<matchable> *> stack;

      void print_stack(std::ostream &out,
		       const stack &stack,
		       aptitudeDepCache &cache)
      {
	out << "[";
	for(stack::const_reverse_iterator it =
	      stack.rbegin(); it != stack.rend(); ++it)
	  {
	    if(it != stack.rbegin())
	      out << " | ";

	    print_pool(out, **it, cache);
	  }
	out << "]";
      }

      /** \brief Like evaluate_structural, but filters the input using
       *  Xapian first.
       */
      ref_ptr<structural_match> evaluate_toplevel(structural_eval_mode mode,
						  const ref_ptr<pattern> &p,
						  stack &the_stack,
						  const ref_ptr<search_cache::implementation> &search_info,
						  const std::vector<matchable> &pool,
						  aptitudeDepCache &cache,
						  pkgRecords &records,
						  bool debug);

      /** \brief Evaluate any regular expression-based pattern.
       *
       *  \param p      The pattern to evaluate.
       *  \param inf    The regular expression to apply.
       *  \param s      The string to test the regular expression against.
       *  \param invert \b true if this match is inverted (i.e., in a NOT
       *                context).  For inverted matches, we only return
       *                a match if the regex does \e not match, and the
       *                match region is the whole string.
       *
       *  \return     A match object corresponding to the regexp,
       *              or \b NULL if the match failed.
       */
      ref_ptr<match> evaluate_regexp(const ref_ptr<pattern> &p,
				     const pattern::regex_info &inf,
				     const char *s,
				     const ref_ptr<search_cache::implementation> &search_info,
				     bool debug)
      {
	// Unfortunately, regexec() seems to require a hard limit to
	// the number of matches that can be returned. :-(
	regmatch_t matches[30];
	const int num_matches = sizeof(matches) / sizeof(regmatch_t);

	bool matched = inf.get_regex_group()->exec(s,
						   matches,
						   num_matches);

	if(matched)
	  {
	    int matches_found = 0;
	    while(matches_found < 30 && matches[matches_found].rm_so >= 0)
	      ++matches_found;

	    return match::make_regexp(p, matches, matches + matches_found);
	  }
	else
	  return NULL;
      }

      // Match an atomic expression against one matchable.
      ref_ptr<match> evaluate_atomic(const ref_ptr<pattern> &p,
				     const matchable &target,
				     stack &the_stack,
				     const ref_ptr<search_cache::implementation> &search_info,
				     aptitudeDepCache &cache,
				     pkgRecords &records,
				     bool debug)
      {
	if(debug)
	  {
	    std::cout << "Matching " << serialize_pattern(p)
		      << " against the target ";
	    print_matchable(std::cout, target, cache);
	    std::cout << " with stack ";
	    print_stack(std::cout, the_stack, cache);
	    std::cout << std::endl;
	  }

	switch(p->get_type())
	  {
	    // Structural matchers:

	  case pattern::all_versions:
	  case pattern::and_tp:
	  case pattern::any_version:
	  case pattern::for_tp:
	  case pattern::narrow:
	  case pattern::not_tp:
	  case pattern::or_tp:
	  case pattern::widen:
	    throw MatchingException("Internal error: evaluate_atomic() invoked on a non-leaf node.");
	    break;

	    // Atomic matchers:
	  case pattern::archive:
	    if(!target.get_has_version())
	      return NULL;

	    {
	      pkgCache::VerIterator ver(target.get_version_iterator(cache));


	      for(pkgCache::VerFileIterator f = ver.FileList(); !f.end(); ++f)
		{
		  pkgCache::PkgFileIterator cur = f.File();

		  if(!cur.end() && cur.Archive())
		    {
		      ref_ptr<match> m = evaluate_regexp(p,
							 p->get_archive_regex_info(),
							 cur.Archive(),
							 search_info,
							 debug);

		      if(m.valid())
			return m;
		    }
		}
	    }

	    return NULL;
	    break;

	  case pattern::action:
	    {
	      bool matches = false;
	      pattern::action_type type = p->get_action_action_type();
	      pkgCache::PkgIterator pkg = target.get_package_iterator(cache);

	      // Install, purge, and remove states all match more than
	      // one find_pkg_state return value.
	      switch(type)
		{
		case pattern::action_install:
		  {
		    pkg_action_state thetype = find_pkg_state(pkg, cache);
		    matches = thetype == pkg_install || thetype == pkg_auto_install;
		  }
		  break;

		case pattern::action_purge:
		  if((cache[pkg].iFlags & pkgDepCache::Purge) == 0)
		    matches = false;
		  else
		    {
		      pkg_action_state thetype = find_pkg_state(pkg, cache);
		      matches = thetype == pkg_remove || thetype == pkg_auto_remove || thetype == pkg_unused_remove;
		    }
		  break;

		case pattern::action_remove:
		  {
		    pkg_action_state thetype = find_pkg_state(pkg, cache);

		    matches = thetype == pkg_remove || thetype == pkg_auto_remove || thetype == pkg_unused_remove;
		  }
		  break;

		case pattern::action_hold:
		  matches = !pkg.CurrentVer().end() && cache.get_ext_state(pkg).selection_state == pkgCache::State::Hold;
		  break;

		  // The rest correspond directly to find_pkg_state() return values.
		case pattern::action_reinstall:
		  matches = find_pkg_state(pkg, cache) == pkg_reinstall;

		case pattern::action_upgrade:
		  matches = find_pkg_state(pkg, cache) == pkg_upgrade;

		case pattern::action_downgrade:
		  matches = find_pkg_state(pkg, cache) == pkg_downgrade;

		case pattern::action_keep:
		  matches = cache[pkg].Keep();
		  break;

		default:
		  throw MatchingException("Internal error: bad action-type flag.");
		}

	      if(matches)
		return match::make_atomic(p);
	      else
		return NULL;
	    }

	    break;

	  case pattern::automatic:
	    {
	      pkgCache::PkgIterator pkg(target.get_package_iterator(cache));

	      if(  (!pkg.CurrentVer().end() || cache[pkg].Install()) &&
		   (cache[pkg].Flags & pkgCache::Flag::Auto)  )
		return match::make_atomic(p);
	      else
		return NULL;
	    }
	    break;

	  case pattern::bind:
	    // If this assert fails, something went wrong internally.
	    {
	      const std::size_t variable_index = p->get_bind_variable_index();
	      eassert(variable_index >= 0 && variable_index < the_stack.size());

	      ref_ptr<structural_match>
		sub_match(evaluate_toplevel(structural_eval_any,
					    p->get_bind_pattern(),
					    the_stack,
					    search_info,
					    *the_stack[variable_index],
					    cache,
					    records,
					    debug));

	      if(sub_match.valid())
		return match::make_with_sub_match(p, sub_match);
	      else
		return NULL;
	    }
	    break;

	  case pattern::broken:
	    if(!target.get_has_version())
	      return NULL;
	    else
	      {
		pkgCache::PkgIterator pkg(target.get_package_iterator(cache));
		aptitudeDepCache::StateCache &state = cache[pkg];

		if(state.NowBroken() || state.InstBroken())
		  return match::make_atomic(p);
		else
		  return NULL;
	      }
	    break;

	  case pattern::broken_type:
	    if(!target.get_has_version())
	      return NULL;
	    else
	      {
		pkgCache::VerIterator ver(target.get_version_iterator(cache));
		pkgCache::DepIterator dep(ver.DependsList());

		while(!dep.end())
		  {
		    while(dep->CompareOp & pkgCache::Dep::Or)
		      ++dep;

		    if(dep->Type == p->get_broken_type_depends_type() &&
		       !(cache[dep] & pkgDepCache::DepGInstall))
		      // Oops, it's broken..
		      return match::make_atomic(p);

		    ++dep;
		  }

		return NULL;
	      }
	    break;

	  case pattern::candidate_version:
	    if(!target.get_has_version())
	      return NULL;
	    else
	      {
		pkgCache::PkgIterator pkg(target.get_package_iterator(cache));
		pkgCache::VerIterator ver(target.get_version_iterator(cache));

		if(ver == cache[pkg].CandidateVerIter(cache))
		  return match::make_atomic(p);
		else
		  return NULL;
	      }
	    break;

	  case pattern::config_files:
	    if(target.get_pkg()->CurrentState == pkgCache::State::ConfigFiles)
	      return match::make_atomic(p);
	    else
	      return NULL;
	    break;

	  case pattern::current_version:
	    if(!target.get_has_version())
	      return NULL;
	    else
	      {
		pkgCache::PkgIterator pkg(target.get_package_iterator(cache));

		pkgCache::VerIterator ver(target.get_version_iterator(cache));

		if(ver == pkg.CurrentVer())
		  return match::make_atomic(p);
		else
		  return NULL;
	      }
	    break;

	  case pattern::depends:
	    if(!target.get_has_version())
	      return NULL;
	    else
	      {
		const pkgCache::VerIterator ver(target.get_version_iterator(cache));

		const pkgCache::Dep::DepType depends_type = p->get_depends_depends_type();
		const bool broken = p->get_depends_broken();

		pkgCache::DepIterator dep = ver.DependsList();
		while(!dep.end())
		  {
		    pkgCache::DepIterator or_group_start = dep;

		    if( (depends_type == dep->Type) ||
			(depends_type == pkgCache::Dep::Depends &&
			 dep->Type == pkgCache::Dep::PreDepends))
		      {
			if(broken)
			  {
			    pkgCache::DepIterator d2(cache, &*dep);
			    while(d2->CompareOp & pkgCache::Dep::Or)
			      ++d2;
			    if(cache[d2] & pkgDepCache::DepGInstall)
			      {
				dep = d2;
				++dep;
				continue;
			      }
			  }

			std::vector<matchable> new_pool;

			// See if a versionless match works.
			while(1)
			  {
			    pkgCache::PkgIterator pkg(dep.TargetPkg());
			    if(pkg.VersionList().end())
			      new_pool.push_back(matchable(pkg));
			    else
			      {
				for(pkgCache::VerIterator i=pkg.VersionList(); !i.end(); i++)
				  if(_system->VS->CheckDep(i.VerStr(), dep->CompareOp, dep.TargetVer()))
				    new_pool.push_back(matchable(pkg, i));
			      }

			    if((dep->CompareOp & pkgCache::Dep::Or) == 0)
			      break;
			    else
			      ++dep;
			  }

			if(!new_pool.empty())
			  {
			    std::sort(new_pool.begin(), new_pool.end());

			    ref_ptr<structural_match> m =
			      evaluate_toplevel(structural_eval_any,
						p->get_depends_pattern(),
						the_stack,
						search_info,
						new_pool,
						cache,
						records,
						debug);

			    // Note: the dependency that we return is
			    // just the head of the OR group.
			    if(m.valid())
			      return match::make_dependency(p, m,
							    or_group_start);
			  }
		      }

		    ++dep;
		  }

		return NULL;
	      }
	    break;

	  case pattern::description:
	    if(!target.get_has_version())
	      return NULL;
	    else
	      {
		pkgCache::VerIterator ver(target.get_version_iterator(cache));
		return evaluate_regexp(p,
				       p->get_description_regex_info(),
				       transcode(get_long_description(ver, &records)).c_str(),
				       search_info,
				       debug);
	      }
	    break;

	  case pattern::essential:
	    {
	      pkgCache::PkgIterator pkg(target.get_package_iterator(cache));

	      if(  ((pkg->Flags & pkgCache::Flag::Essential) == pkgCache::Flag::Essential) ||
		   ((pkg->Flags & pkgCache::Flag::Important) == pkgCache::Flag::Important)  )
		  return match::make_atomic(p);
	      else
		return NULL;
	    }
	    break;

	  case pattern::equal:
	    {
	      const std::size_t variable_index = p->get_equal_stack_position();
	      eassert(variable_index >= 0 && variable_index < the_stack.size());

	      // Search for the incoming package/version in the pool
	      // referenced by this pattern.
	      const std::vector<matchable> &pool(*the_stack[variable_index]);

	      if(std::binary_search(pool.begin(), pool.end(),
				    target))
		return match::make_atomic(p);
	      else
		return NULL;
	    }

	    break;

	  case pattern::false_tp:
	    return NULL;
	    break;

	  case pattern::garbage:
	    if(!target.get_has_version())
	      return NULL;
	    else if(!cache[target.get_package_iterator(cache)].Garbage)
	      return NULL;
	    else
	      return match::make_atomic(p);
	    break;

	  case pattern::install_version:
	    if(target.get_has_version() &&
	       target.get_ver() == cache[target.get_package_iterator(cache)].InstallVer)
	      return match::make_atomic(p);
	    else
	      return NULL;
	    break;

	  case pattern::installed:
	    if(target.get_has_version() &&
	       target.get_version_iterator(cache) == target.get_package_iterator(cache).CurrentVer())
	      return match::make_atomic(p);
	    else
	      return NULL;
	    break;

	  case pattern::maintainer:
	    if(!target.get_has_version())
	      return NULL;
	    else
	      {
		pkgCache::VerIterator ver(target.get_version_iterator(cache));
		pkgRecords::Parser &rec(records.Lookup(ver.FileList()));

		return evaluate_regexp(p,
				       p->get_maintainer_regex_info(),
				       rec.Maintainer().c_str(),
				       search_info,
				       debug);
	      }
	    break;

	  case pattern::name:
	    return evaluate_regexp(p,
				   p->get_name_regex_info(),
				   target.get_package_iterator(cache).Name(),
				   search_info,
				   debug);
	    break;

	  case pattern::new_tp:
	    if(!target.get_has_version())
	      return NULL;
	    else if(!cache.get_ext_state(target.get_package_iterator(cache)).new_package)
	      return NULL;
	    else
	      return match::make_atomic(p);
	    break;

	  case pattern::obsolete:
	    if(pkg_obsolete(target.get_package_iterator(cache)))
	      return match::make_atomic(p);
	    else
	      return NULL;
	    break;

	  case pattern::origin:
	    if(!target.get_has_version())
	      return NULL;
	    {
	      pkgCache::PkgIterator pkg(target.get_package_iterator(cache));
	      pkgCache::VerIterator ver(target.get_version_iterator(cache));

	      for(pkgCache::VerFileIterator f = ver.FileList(); !f.end(); ++f)
		{
		  pkgCache::PkgFileIterator cur = f.File();
		  const char *origin = cur.Origin();

		  if(!cur.end() && origin != NULL)
		    {
		      ref_ptr<match>
			m(evaluate_regexp(p,
					  p->get_origin_regex_info(),
					  origin,
					  search_info,
					  debug));

		      if(m.valid())
			return m;
		    }
		}

	      return NULL;
	    }

	    break;

	  case pattern::priority:
	    if(!target.get_has_version())
	      return NULL;
	    else if(target.get_ver()->Priority != p->get_priority_priority())
	      return NULL;
	    else
	      return match::make_atomic(p);
	    break;

	  case pattern::provides:
	    if(!target.get_has_version())
	      return NULL;
	    else
	      {
		pkgCache::VerIterator ver(target.get_version_iterator(cache));

		std::vector<matchable> new_pool;

		for(pkgCache::PrvIterator prv = ver.ProvidesList();
		    !prv.end(); ++prv)
		  {
		    // Add all versions of each provided package to
		    // the pool.  I chose this because it seems least
		    // surprising (?provides(?description(blah))
		    // should behave as expected), but if versioned
		    // Provides happen this might cause problems with
		    // matching only the versions that are actually
		    // provided.
		    pkgCache::PkgIterator provided_pkg = prv.ParentPkg();
		    if(provided_pkg.VersionList().end())
		      new_pool.push_back(matchable(provided_pkg));
		    else
		      for(pkgCache::VerIterator ver = provided_pkg.VersionList();
			  !ver.end(); ++ver)
			new_pool.push_back(matchable(provided_pkg, ver));

		    ref_ptr<structural_match>
		      m(evaluate_toplevel(structural_eval_any,
					  p->get_provides_pattern(),
					  the_stack,
					  search_info,
					  new_pool,
					  cache,
					  records,
					  debug));

		    if(m.valid())
		      return match::make_provides(p, m, prv);
		  }
	      }

	    return NULL;
	    break;

	  case pattern::reverse_depends:
	    {
	      pkgCache::PkgIterator pkg = target.get_package_iterator(cache);
	      pkgCache::VerIterator ver;
	      if(target.get_has_version())
		ver = target.get_version_iterator(cache);
	      const bool broken = p->get_reverse_depends_broken();
	      pkgCache::Dep::DepType type = p->get_reverse_depends_depends_type();

	      std::vector<matchable> revdep_pool;

	      for(pkgCache::DepIterator d = pkg.RevDependsList();
		  !d.end(); ++d)
		{
		  if(broken)
		    {
		      // Find the corresponding forward dependency and
		      // check whether it's broken.
		      pkgCache::DepIterator d2(cache, &*d);
		      while(d2->CompareOp & pkgCache::Dep::Or)
			++d2;
		      if(cache[d2] & pkgDepCache::DepGInstall)
			continue;
		    }

		  if(  (d->Type == type ||
			(type == pkgCache::Dep::Depends && d->Type == pkgCache::Dep::PreDepends)) &&
		       (!d.TargetVer() || (target.get_has_version() &&
					   _system->VS->CheckDep(ver.VerStr(), d->CompareOp, d.TargetVer())))   )
		    {
		      matchable m(d.ParentPkg(), d.ParentVer());
		      if(revdep_pool.empty())
			revdep_pool.push_back(m);
		      else
			revdep_pool[0] = m;


		      ref_ptr<structural_match>
			rval(evaluate_toplevel(structural_eval_any,
					       p->get_reverse_depends_pattern(),
					       the_stack,
					       search_info,
					       revdep_pool,
					       cache,
					       records,
					       debug));

		      if(rval.valid())
			return match::make_dependency(p, rval, d);
		    }
		}

	      // Check dependencies through virtual packages.
	      if(target.get_has_version())
		{
		  for(pkgCache::PrvIterator prv = ver.ProvidesList();
		      !prv.end(); ++prv)
		    {
		      for(pkgCache::DepIterator d = prv.ParentPkg().RevDependsList();
			  !d.end(); ++d)
			{
			  if(broken)
			    {
			      pkgCache::DepIterator d2(cache, &*d);
			      while(d2->CompareOp & pkgCache::Dep::Or)
				++d2;
			      if(cache[d2] & pkgDepCache::DepGInstall)
				continue;
			    }

			  if(d->Type == type &&
			     (d.TargetVer() == NULL ||
			      (  prv.ProvideVersion() != NULL &&
			         _system->VS->CheckDep(ver.VerStr(), d->CompareOp, d.TargetVer())  )))
			    {
			      matchable m(d.ParentPkg(), d.ParentVer());
			      if(revdep_pool.empty())
				revdep_pool.push_back(m);
			      else
				revdep_pool[0] = m;


			      ref_ptr<structural_match>
				rval(evaluate_toplevel(structural_eval_any,
						       p->get_reverse_depends_pattern(),
						       the_stack,
						       search_info,
						       revdep_pool,
						       cache,
						       records,
						       debug));

			      if(rval.valid())
				return match::make_dependency(p, rval, d);
			    }
			}
		    }
		}

	      return NULL;
	    }
	    break;

	  case pattern::reverse_provides:
	    {
	      std::vector<matchable> revprv_pool;
	      pkgCache::PkgIterator pkg = target.get_package_iterator(cache);

	      // Hm, it would be nice if we could do this in a way that
	      // allowed users to ask to match all the reverse provides.
	      for(pkgCache::PrvIterator prv = pkg.ProvidesList();
		  !prv.end(); ++prv)
		{
		  if(revprv_pool.empty())
		    revprv_pool.push_back(matchable(prv.OwnerPkg(), prv.OwnerVer()));
		  else
		    revprv_pool[0] = matchable(prv.OwnerPkg(), prv.OwnerVer());

		  ref_ptr<structural_match>
		    m(evaluate_toplevel(structural_eval_any,
					p->get_reverse_provides_pattern(),
					the_stack,
					search_info,
					revprv_pool,
					cache,
					records,
					debug));

		  if(m.valid())
		    return match::make_provides(p, m, prv);
		}

	      return NULL;
	    }

	    break;

	  case pattern::section:
	    if(target.get_has_version())
	      {
		pkgCache::VerIterator ver(target.get_version_iterator(cache));
		const char *ver_section = ver.Section();
		if(ver_section != NULL)
		  {
		    ref_ptr<match>
		      m(evaluate_regexp(p,
					p->get_section_regex_info(),
					ver_section,
					search_info,
					debug));

		    if(m.valid())
		      return m;
		  }
	      }

	    {
	      pkgCache::PkgIterator pkg(target.get_package_iterator(cache));
	      const char *pkg_section = pkg.Section();

	      if(pkg_section != NULL)
		return evaluate_regexp(p,
				       p->get_section_regex_info(),
				       pkg_section,
				       search_info,
				       debug);
	      else
		return NULL;
	    }
	    break;

	  case pattern::source_package:
	    {
	      if(!target.get_has_version())
		return NULL;

	      bool checked_real_package = false;

	      pkgCache::PkgIterator pkg(target.get_package_iterator(cache));
	      pkgCache::VerIterator ver(target.get_version_iterator(cache));

	      for(pkgCache::VerFileIterator vf = ver.FileList();
		  !vf.end(); ++vf)
		{
		  pkgRecords::Parser &rec = records.Lookup(vf);

		  if(rec.SourcePkg().empty())
		    {
		      if(!checked_real_package)
			{
			  ref_ptr<match> rval =
			    evaluate_regexp(p,
					    p->get_source_package_regex_info(),
					    pkg.Name(),
					    search_info,
					    debug);

			  if(rval.valid())
			    return rval;
			}
		    }
		  else
		    {
		      ref_ptr<match> rval =
			evaluate_regexp(p,
					p->get_source_package_regex_info(),
					rec.SourcePkg().c_str(),
					search_info,
					debug);

		      if(rval.valid())
			return rval;
		    }
		}

	      return NULL;
	    }
	    break;

	  case pattern::source_version:
	    {
	      if(!target.get_has_version())
		return NULL;

	      bool checked_real_package = false;

	      pkgCache::PkgIterator pkg(target.get_package_iterator(cache));
	      pkgCache::VerIterator ver(target.get_version_iterator(cache));

	      for(pkgCache::VerFileIterator vf = ver.FileList();
		  !vf.end(); ++vf)
		{
		  pkgRecords::Parser &rec = records.Lookup(vf);

		  if(rec.SourceVer().empty())
		    {
		      if(!checked_real_package)
			{
			  ref_ptr<match> rval =
			    evaluate_regexp(p,
					    p->get_source_version_regex_info(),
					    ver.VerStr(),
					    search_info,
					    debug);

			  if(rval.valid())
			    return rval;
			}
		    }
		  else
		    {
		      ref_ptr<match> rval =
			evaluate_regexp(p,
					p->get_source_version_regex_info(),
					rec.SourceVer().c_str(),
					search_info,
					debug);

		      if(rval.valid())
			return rval;
		    }
		}

	      return NULL;
	    }
	    break;

	  case pattern::tag:
	    {
	      pkgCache::PkgIterator pkg(target.get_package_iterator(cache));

#ifdef HAVE_EPT
	      typedef ept::debtags::Tag tag;
	      using aptitude::apt::get_tags;
#endif

#ifdef HAVE_EPT
	      const std::set<tag> realTags(get_tags(pkg));
	      const std::set<tag> * const tags(&realTags);
#else
	      const std::set<tag> * const tags(get_tags(pkg));
#endif

	      if(tags == NULL)
		return false;

	      for(std::set<tag>::const_iterator i=tags->begin(); i!=tags->end(); ++i)
		{
#ifdef HAVE_EPT
		  std::string name(i->fullname());
#else
		  const std::string name = i->str().c_str();
#endif
		  ref_ptr<match> rval =
		    evaluate_regexp(p,
				    p->get_tag_regex_info(),
				    name.c_str(),
				    search_info,
				    debug);

		  if(rval.valid())
		    return rval;
		}

	      return NULL;
	    }
	    break;

	  case pattern::task:
	    {
	      pkgCache::PkgIterator pkg(target.get_package_iterator(cache));

	      std::set<string> *l = get_tasks(pkg);

	      if(!l)
		return NULL;

	      for(std::set<string>::iterator i = l->begin();
		  i != l->end();
		  ++i)
		{
		  ref_ptr<match> m =
		    evaluate_regexp(p,
				    p->get_task_regex_info(),
				    i->c_str(),
				    search_info,
				    debug);

		  if(m.valid())
		    return m;
		}

	      return NULL;
	    }
	    break;

	  case pattern::term:
	    {
	      pkgCache::PkgIterator pkg(target.get_package_iterator(cache));

	      if(search_info->term_matches(pkg, p->get_term_term(), debug))
		return match::make_atomic(p);
	      else
		return NULL;
	    }
	    break;

	  case pattern::true_tp:
	    return match::make_atomic(p);
	    break;

	  case pattern::upgradable:
	    {
	      pkgCache::PkgIterator pkg =
		target.get_package_iterator(cache);

	      if(!pkg.CurrentVer().end() &&
		 cache[pkg].Upgradable())
		return match::make_atomic(p);
	      else
		return NULL;
	    }
	    break;

	  case pattern::user_tag:
	    {
	      pkgCache::PkgIterator pkg =
		target.get_package_iterator(cache);

	      const std::set<aptitudeDepCache::user_tag> &user_tags =
		cache.get_ext_state(pkg).user_tags;

	      for(std::set<aptitudeDepCache::user_tag>::const_iterator it =
		    user_tags.begin(); it != user_tags.end(); ++it)
		{
		  aptitudeDepCache::user_tag tag(*it);

		  // NB: this currently short-circuits (as does, e.g.,
		  // ?task); for highlighting purposes we might want
		  // to return all matches.
		  std::map<aptitudeDepCache::user_tag, ref_ptr<match> > &user_tag_matches(search_info->get_user_tag_matches());
		  std::map<aptitudeDepCache::user_tag, ref_ptr<match> >::const_iterator
		    found = user_tag_matches.find(tag);


		  if(found != user_tag_matches.end() && found->second.valid())
		    return found->second;
		  else if(found == user_tag_matches.end())
		    {
		      ref_ptr<match> rval(evaluate_regexp(p,
							  p->get_user_tag_regex_info(),
							  cache.deref_user_tag(*it).c_str(),
							  search_info,
							  debug));

		      user_tag_matches[tag] = rval;
		      if(rval.valid())
			return rval;
		    }
		}

	      return NULL;
	    }
	    break;

	  case pattern::version:
	    if(!target.get_has_version())
	      return NULL;

	    return evaluate_regexp(p,
				   p->get_version_regex_info(),
				   target.get_version_iterator(cache).VerStr(),
				   search_info,
				   debug);
	    break;

	  case pattern::virtual_tp:
	    if(!target.get_package_iterator(cache).VersionList().end())
	      return NULL;
	    else
	      return match::make_atomic(p);
	    break;

	  default:
	    throw MatchingException("Internal error: unhandled pattern type in evaluate()");
	  }
      }

      ref_ptr<structural_match> evaluate_structural(structural_eval_mode mode,
						    const ref_ptr<pattern> &p,
						    stack &the_stack,
						    const ref_ptr<search_cache::implementation> &search_info,
						    const std::vector<matchable> &pool,
						    aptitudeDepCache &cache,
						    pkgRecords &records,
						    bool debug)
      {
	if(debug)
	  {
	    std::cout << "Matching " << serialize_pattern(p)
		      << " against the pool ";
	    print_pool(std::cout, pool, cache);
	    std::cout << " with stack ";
	    print_stack(std::cout, the_stack, cache);
	    std::cout << " (mode=";
	    switch(mode)
	      {
	      case structural_eval_all:
		std::cout << "all";
		break;

	      case structural_eval_any:
		std::cout << "any";
		break;
	      }
	    std::cout << ")" << std::endl;
	  }

	switch(p->get_type())
	  {
	    // Structural matchers:

	  case pattern::all_versions:
	    {
	      ref_ptr<structural_match>
		m(evaluate_structural(structural_eval_all,
				      p->get_all_versions_pattern(),
				      the_stack,
				      search_info,
				      pool,
				      cache,
				      records,
				      debug));

	      if(!m.valid())
		return NULL;
	      else
		return structural_match::make_branch(p, &m, (&m) + 1);
	    }
	    break;

	  case pattern::and_tp:
	    {
	      const std::vector<ref_ptr<pattern> > &sub_patterns(p->get_and_patterns());
	      std::vector<ref_ptr<structural_match> > sub_matches;

	      for(std::vector<ref_ptr<pattern> >::const_iterator it =
		    sub_patterns.begin(); it != sub_patterns.end(); ++it)
		{
		  ref_ptr<structural_match> m(evaluate_structural(mode,
								  (*it),
								  the_stack,
								  search_info,
								  pool,
								  cache,
								  records,
								  debug));

		  if(!m.valid())
		    return NULL;

		  sub_matches.push_back(m);
		}

	      return structural_match::make_branch(p, sub_matches.begin(), sub_matches.end());
	    }
	    break;

	  case pattern::any_version:
	    {
	      std::vector<matchable> new_pool;
	      new_pool.push_back(matchable());

	      std::vector<ref_ptr<structural_match> > sub_matches;
	      for(std::vector<matchable>::const_iterator it =
		    pool.begin(); it != pool.end(); ++it)
		{
		  new_pool[0] = *it;

		  ref_ptr<structural_match>
		    m(evaluate_structural(mode,
					  p->get_any_version_pattern(),
					  the_stack,
					  search_info,
					  new_pool,
					  cache,
					  records,
					  debug));

		  if(m.valid())
		    sub_matches.push_back(m);
		}

	      if(sub_matches.empty())
		return NULL;
	      else
		return structural_match::make_branch(p, sub_matches.begin(), sub_matches.end());
	    }
	    break;

	  case pattern::for_tp:
	    {
	      the_stack.push_back(&pool);

	      const ref_ptr<structural_match>
		m(evaluate_structural(mode,
				      p->get_for_pattern(),
				      the_stack,
				      search_info,
				      pool,
				      cache,
				      records,
				      debug));

	      if(m.valid())
		return structural_match::make_branch(p, &m, (&m) + 1);
	      else
		return NULL;
	    }
	    break;

	  case pattern::narrow:
	    // Match each entry in the pool against the filter
	    // separately.  Then match the main pattern against a
	    // pool formed from values that passed the filter.
	    {
	      std::vector<matchable> singleton_pool;
	      std::vector<matchable> new_pool;
	      singleton_pool.push_back(matchable());

	      // \todo we should perhaps store the filter matches in a
	      // separate list.
	      for(std::vector<matchable>::const_iterator it =
		    pool.begin(); it != pool.end(); ++it)
		{
		  singleton_pool[0] = *it;

		  if(evaluate_structural(mode,
					 p->get_narrow_filter(),
					 the_stack,
					 search_info,
					 singleton_pool,
					 cache,
					 records,
					 debug).valid())
		    new_pool.push_back(*it);
		}

	      if(new_pool.empty())
		return NULL;
	      else
		{
		  ref_ptr<structural_match>
		    m(evaluate_structural(mode,
					  p->get_narrow_pattern(),
					  the_stack,
					  search_info,
					  new_pool,
					  cache,
					  records,
					  debug));

		  if(!m.valid())
		    return NULL;
		  else
		    return structural_match::make_branch(p, &m, (&m) + 1);
		}
	    }
	    break;

	  case pattern::not_tp:
	    {
	      ref_ptr<structural_match> m(evaluate_structural(mode,
							      p->get_not_pattern(),
							      the_stack,
							      search_info,
							      pool,
							      cache,
							      records,
							      debug));

	      if(!m.valid())
		// Report a structural match with no sub-parts.  This
		// will lose doubly-negated information.  For now that's
		// just too bad; we can try to recover it later.
		return structural_match::make_branch(p,
						     (ref_ptr<structural_match> *)0,
						     (ref_ptr<structural_match> *)0);
	      else
		return NULL;
	    }

	    break;

	  case pattern::or_tp:
	    {
	      const std::vector<ref_ptr<pattern> > &sub_patterns(p->get_or_patterns());
	      std::vector<ref_ptr<structural_match> > sub_matches;

	      // Note: we do *not* short-circuit, in order to allow
	      // the caller to see as much information as possible
	      // about the match.
	      for(std::vector<ref_ptr<pattern> >::const_iterator it =
		    sub_patterns.begin(); it != sub_patterns.end(); ++it)
		{
		  ref_ptr<structural_match> m(evaluate_structural(mode,
								  (*it),
								  the_stack,
								  search_info,
								  pool,
								  cache,
								  records,
								  debug));

		  if(m.valid())
		    sub_matches.push_back(m);
		}

	      if(sub_matches.empty())
		return NULL;
	      else
		return structural_match::make_branch(p, sub_matches.begin(), sub_matches.end());
	    }
	    break;

	  case pattern::widen:
	    // NB: to make this fast I rely on the sort order of
	    // matchables.
	    //
	    // \todo Perhaps this pattern should be redefined to allow
	    // us to inject arbitrary stuff into the pool?  Right now
	    // it just expands the pool to include all the versions of
	    // each package that it includes.
	    {
	      std::vector<matchable> new_pool;

	      for(std::vector<matchable>::const_iterator it
		    = pool.begin(); it != pool.end(); ++it)
		{
		  // If we've already seen this package it'll be at
		  // the back of the new pool (due to processing
		  // inputs in the pool in sort order).
		  if(!new_pool.empty() &&
		     new_pool.back().get_pkg() == it->get_pkg())
		    continue;

		  // Virtual packages aren't touched by ?widen.
		  if(!it->get_has_version())
		    {
		      new_pool.push_back(*it);
		      continue;
		    }

		  pkgCache::PkgIterator pkg =
		    it->get_package_iterator(cache);
		  if(pkg.VersionList().end())
		    new_pool.push_back(*it);
		  else
		    {
		      for(pkgCache::VerIterator ver = pkg.VersionList();
			  !ver.end(); ++ver)
			{
			  new_pool.push_back(matchable(pkg, ver));
			}
		    }
		}

	      std::sort(new_pool.begin(), new_pool.end());
	      ref_ptr<structural_match>
		m(evaluate_structural(mode,
				      p->get_widen_pattern(),
				      the_stack,
				      search_info,
				      new_pool,
				      cache,
				      records,
				      debug));
	      if(!m.valid())
		return NULL;
	      else
		return structural_match::make_branch(p, &m, (&m) + 1);
	    }
	    break;

	    // Atomic matchers:

	  case pattern::archive:
	  case pattern::action:
	  case pattern::automatic:
	  case pattern::bind:
	  case pattern::broken:
	  case pattern::broken_type:
	  case pattern::candidate_version:
	  case pattern::config_files:
	  case pattern::current_version:
	  case pattern::depends:
	  case pattern::description:
	  case pattern::essential:
	  case pattern::equal:
	  case pattern::false_tp:
	  case pattern::garbage:
	  case pattern::install_version:
	  case pattern::installed:
	  case pattern::maintainer:
	  case pattern::name:
	  case pattern::new_tp:
	  case pattern::obsolete:
	  case pattern::origin:
	  case pattern::priority:
	  case pattern::provides:
	  case pattern::reverse_depends:
	  case pattern::reverse_provides:
	  case pattern::section:
	  case pattern::source_package:
	  case pattern::source_version:
	  case pattern::tag:
	  case pattern::task:
	  case pattern::term:
	  case pattern::true_tp:
	  case pattern::upgradable:
	  case pattern::user_tag:
	  case pattern::version:
	  case pattern::virtual_tp:
	    switch(mode)
	      {
	      case structural_eval_all:
		{
		  std::vector<std::pair<matchable, ref_ptr<match> > > matches;
		  for(std::vector<matchable>::const_iterator it =
			pool.begin(); it != pool.end(); ++it)
		    {
		      cwidget::util::ref_ptr<match> m(evaluate_atomic(p, *it, the_stack, search_info, cache, records, debug));
		      if(!m.valid())
			{
			  if(debug)
			    {
			      std::cout << "Failed to match: ";
			      print_matchable(std::cout, *it, cache);
			      std::cout << std::endl;
			    }
			  return NULL;
			}
		      else
			matches.push_back(std::make_pair(*it, m));
		    }

		  if(matches.size() == 0)
		    return NULL;
		  else
		    return structural_match::make_leaf(p, matches.begin(), matches.end());
		}
		break;

	      case structural_eval_any:
		{
		  std::vector<std::pair<matchable, ref_ptr<match> > > matches;
		  for(std::vector<matchable>::const_iterator it =
			pool.begin(); it != pool.end(); ++it)
		    {
		      cwidget::util::ref_ptr<match> m(evaluate_atomic(p, *it, the_stack, search_info, cache, records, debug));
		      if(m.valid())
			{
			  if(debug)
			    {
			      std::cout << "Matched: ";
			      print_matchable(std::cout, *it, cache);
			      std::cout << std::endl;
			    }
			  // TODO: short-circuit?
			  matches.push_back(std::make_pair(*it, m));
			}
		    }

		  if(matches.size() == 0)
		    return NULL;
		  else
		    return structural_match::make_leaf(p, matches.begin(), matches.end());
		}
		break;

	      default:
		throw MatchingException("Internal error: unhandled structural match mode.");
	      }

	    break;

	  default:
	    throw MatchingException("Internal error: unhandled pattern type in evaluate()");
	  }
      }

      ref_ptr<structural_match> evaluate_toplevel(structural_eval_mode mode,
						  const ref_ptr<pattern> &p,
						  stack &the_stack,
						  const ref_ptr<search_cache::implementation> &search_info,
						  const std::vector<matchable> &pool,
						  aptitudeDepCache &cache,
						  pkgRecords &records,
						  bool debug)
      {
	const xapian_info &xapian_match(search_info->get_toplevel_xapian_info(p, debug));

	// This may be an efficiency problem.
	std::vector<matchable> filtered_pool;
	filtered_pool.reserve(pool.size());
	for(std::vector<matchable>::const_iterator it = pool.begin();
	    it != pool.end(); ++it)
	  {
	    if(xapian_match.maybe_contains_package(it->get_package_iterator(cache), search_info->get_db()))
	      filtered_pool.push_back(*it);
	  }

	return evaluate_structural(mode, p, the_stack, search_info,
				   filtered_pool, cache, records, debug);
      }

      // is_pure_xapian returns "true" if we can identify a Xapian
      // term that matches if AND ONLY IF the pattern matches.  In
      // other words: it contains only structural patterns combined
      // with Boolean operators (and version-manipulation operators,
      // which we ignore).
      bool is_pure_xapian(const ref_ptr<pattern> &p)
      {
	switch(p->get_type())
	  {
	  case pattern::all_versions:
	    return is_pure_xapian(p->get_all_versions_pattern());

	  case pattern::and_tp:
	    {
	      const std::vector<ref_ptr<pattern> > &
		sub_patterns(p->get_and_patterns());

	      if(sub_patterns.size() == 0)
		return true;

	      for(std::vector<ref_ptr<pattern> >::const_iterator it =
		    sub_patterns.begin(); it != sub_patterns.end(); ++it)
		{
		  if(!is_pure_xapian(*it))
		    return false;
		}

	      return true;
	    }

	  case pattern::any_version:
	    return is_pure_xapian(p->get_any_version_pattern());

	  case pattern::for_tp:
	    return is_pure_xapian(p->get_for_pattern());

	  case pattern::narrow:
	    return
	      is_pure_xapian(p->get_narrow_filter()) &&
	      is_pure_xapian(p->get_narrow_pattern());

	  case pattern::not_tp:
	    return is_pure_xapian(p->get_not_pattern());

	  case pattern::or_tp:
	    {
	      const std::vector<ref_ptr<pattern> > &
		sub_patterns(p->get_or_patterns());

	      if(sub_patterns.size() == 0)
		return true;

	      for(std::vector<ref_ptr<pattern> >::const_iterator it =
		    sub_patterns.begin(); it != sub_patterns.end(); ++it)
		{
		  if(!is_pure_xapian(*it))
		    return false;
		}

	      return true;
	    }

	  case pattern::widen:
	    return is_pure_xapian(p->get_widen_pattern());

	  case pattern::term:
	    return true;

	    // Various non-Xapian terms.  All of these return false.

	  case pattern::archive:
	  case pattern::action:
	  case pattern::automatic:
	  case pattern::bind:
	  case pattern::broken:
	  case pattern::broken_type:
	  case pattern::candidate_version:
	  case pattern::config_files:
	  case pattern::current_version:
	  case pattern::depends:
	  case pattern::description:
	  case pattern::essential:
	  case pattern::equal:
	  case pattern::false_tp:
	  case pattern::garbage:
	  case pattern::install_version:
	  case pattern::installed:
	  case pattern::maintainer:
	  case pattern::name:
	  case pattern::new_tp:
	  case pattern::obsolete:
	  case pattern::origin:
	  case pattern::priority:
	  case pattern::provides:
	  case pattern::reverse_depends:
	  case pattern::reverse_provides:
	  case pattern::section:
	  case pattern::source_package:
	  case pattern::source_version:
	  case pattern::tag:
	  case pattern::task:
	  case pattern::true_tp:
	  case pattern::upgradable:
	  case pattern::user_tag:
	  case pattern::version:
	  case pattern::virtual_tp:
	    return false;
	  default:
	    throw MatchingException("Internal error: unhandled pattern type in is_pure_xapian()");
	  }
      }

      // is_xapian_dependent returns "true" if we can always identify
      // a Xapian term that must match for the pattern to match.
      bool is_xapian_dependent(const ref_ptr<pattern> &p)
      {
	switch(p->get_type())
	  {
	  case pattern::all_versions:
	    return is_xapian_dependent(p->get_all_versions_pattern());

	  case pattern::and_tp:
	    {
	      const std::vector<ref_ptr<pattern> > &
		sub_patterns(p->get_and_patterns());

	      if(sub_patterns.size() == 0)
		return false;

	      // The AND is fine if it has at least one positive
	      // Xapian-dependent term.  NB: since negative terms are
	      // not Xapian-dependent, checking both conditions is
	      // redundant, so we just check that each sub-term is
	      // Xapian-dependent.
	      for(std::vector<ref_ptr<pattern> >::const_iterator it =
		    sub_patterns.begin(); it != sub_patterns.end(); ++it)
		{
		  if(is_xapian_dependent(*it))
		    return true;
		}

	      return false;
	    }

	  case pattern::any_version:
	    return is_xapian_dependent(p->get_any_version_pattern());

	  case pattern::for_tp:
	    return is_xapian_dependent(p->get_for_pattern());

	  case pattern::narrow:
	    return is_xapian_dependent(p->get_narrow_pattern());

	  case pattern::not_tp:
	    return false;

	  case pattern::or_tp:
	    // OR terms are Xapian-dependent if all of their sub-terms
	    // are.
	    {
	      const std::vector<ref_ptr<pattern> > &
		sub_patterns(p->get_or_patterns());

	      if(sub_patterns.size() == 0)
		return false;

	      for(std::vector<ref_ptr<pattern> >::const_iterator it =
		    sub_patterns.begin(); it != sub_patterns.end(); ++it)
		{
		  if(!is_xapian_dependent(*it))
		    return false;
		}

	      return true;
	    }

	  case pattern::widen:
	    return is_xapian_dependent(p->get_widen_pattern());

	  case pattern::term:
	    return true;

	    // Various non-dependent terms.  All of these return
	    // false.  Some have internal matchers, but they are
	    // separate searches.

	  case pattern::archive:
	  case pattern::action:
	  case pattern::automatic:
	  case pattern::bind:
	  case pattern::broken:
	  case pattern::broken_type:
	  case pattern::candidate_version:
	  case pattern::config_files:
	  case pattern::current_version:
	  case pattern::depends:
	  case pattern::description:
	  case pattern::essential:
	  case pattern::equal:
	  case pattern::false_tp:
	  case pattern::garbage:
	  case pattern::install_version:
	  case pattern::installed:
	  case pattern::maintainer:
	  case pattern::name:
	  case pattern::new_tp:
	  case pattern::obsolete:
	  case pattern::origin:
	  case pattern::priority:
	  case pattern::provides:
	  case pattern::reverse_depends:
	  case pattern::reverse_provides:
	  case pattern::section:
	  case pattern::source_package:
	  case pattern::source_version:
	  case pattern::tag:
	  case pattern::task:
	  case pattern::true_tp:
	  case pattern::upgradable:
	  case pattern::user_tag:
	  case pattern::version:
	  case pattern::virtual_tp:
	    return false;
	  default:
	    throw MatchingException("Internal error: unhandled pattern type in is_xapian_dependent()");
	  }
      }

      ref_ptr<pattern> negate_pattern(const ref_ptr<pattern> &p)
      {
	if(p->get_type() == pattern::not_tp)
	  return p->get_not_pattern();
	else
	  return pattern::make_not(p);
      }

      // Adjusts the incoming pattern for the purposes of computing
      // the Xapian query.
      //
      // Specifically, this pushes NOTs out of OR expressions to the
      // top structural level (the top-level of the pattern, or the
      // top-level of a term like ?depends that has a sub-pattern).
      // Also throws away some structural patterns that are irrelevant
      // for Xapian, like all_versions.
      cwidget::util::ref_ptr<pattern> normalize_pattern(const cwidget::util::ref_ptr<pattern> &p)
      {
	switch(p->get_type())
	  {
	  case pattern::all_versions:
	    return normalize_pattern(p->get_all_versions_pattern());

	  case pattern::and_tp:
	    {
	      const std::vector<ref_ptr<pattern> > &
		sub_patterns(p->get_and_patterns());

	      // First, normalize the sub-parts:
	      std::vector<ref_ptr<pattern> >
		normalized_sub_patterns;
	      normalized_sub_patterns.reserve(sub_patterns.size());

	      // The AND is fine if it has at least one positive
	      // Xapian-dependent term.  If it isn't, but it has a
	      // Xapian-dependent term (i.e., all the Xapian terms are
	      // negative), we invert it and turn it into a
	      // Xapian-dependent OR.  If that fails too, it's not
	      // Xapian-dependent.
	      bool has_xapian_dependent_sub_term = false;
	      bool has_positive_xapian_dependent_sub_term = false;
	      for(std::vector<ref_ptr<pattern> >::const_iterator it =
		    sub_patterns.begin(); it != sub_patterns.end(); ++it)
		{
		  ref_ptr<pattern> sub_normalized(normalize_pattern(*it));

		  if(is_xapian_dependent(sub_normalized))
		    {
		      has_xapian_dependent_sub_term = true;
		      if(sub_normalized->get_type() != pattern::not_tp)
			has_positive_xapian_dependent_sub_term = true;
		    }

		  normalized_sub_patterns.push_back(sub_normalized);
		}

	      if(has_positive_xapian_dependent_sub_term)
		return pattern::make_and(normalized_sub_patterns);
	      else if(has_xapian_dependent_sub_term)
		{
		  std::vector<ref_ptr<pattern> > negative_sub_normalized;
		  negative_sub_normalized.reserve(normalized_sub_patterns.size());

		  for(std::vector<ref_ptr<pattern> >::const_iterator it =
			normalized_sub_patterns.begin(); it != normalized_sub_patterns.end(); ++it)
		    negative_sub_normalized.push_back(negate_pattern(*it));

		  return pattern::make_not(pattern::make_or(negative_sub_normalized));
		}
	      else
		// No Xapian-dependent terms at all, so
		// just muddle through.
		return pattern::make_and(normalized_sub_patterns);
	    }

	  case pattern::any_version:
	    return normalize_pattern(p->get_any_version_pattern());

	  case pattern::for_tp:
	    // Don't eliminate the ?for or its captured variables
	    // won't make sense.  (useful for printing purposes)
	    //
	    // However, we need to reach through it and lift the ?not,
	    // if any, out so that it's visible at top-level.
	    {
	      ref_ptr<pattern> sub_normalized =
		normalize_pattern(p->get_for_pattern());

	      // Note: because the normalization lifts ?not terms to
	      // the top level and eliminates double ?nots, we don't
	      // need to handle double ?nots here.
	      if(sub_normalized->get_type() == pattern::not_tp)
		return pattern::make_not(pattern::make_for(p->get_for_variable_name(),
							   sub_normalized->get_not_pattern()));
	      else
		return pattern::make_for(p->get_for_variable_name(),
					 sub_normalized);
	    }

	  case pattern::narrow:
	    return normalize_pattern(p->get_narrow_pattern());

	  case pattern::not_tp:
	    {
	      ref_ptr<pattern> sub_normalized =
		normalize_pattern(p->get_not_pattern());

	      // Eliminate double ?nots.
	      if(sub_normalized->get_type() == pattern::not_tp)
		return sub_normalized->get_not_pattern();
	      else
		return pattern::make_not(sub_normalized);
	    }

	  case pattern::or_tp:
	    {
	      const std::vector<ref_ptr<pattern> > &
		sub_patterns(p->get_or_patterns());

	      std::vector<ref_ptr<pattern> >
		normalized_sub_patterns;
	      normalized_sub_patterns.reserve(sub_patterns.size());

	      bool has_negative_xapian_dependent_sub_term = false;
	      for(std::vector<ref_ptr<pattern> >::const_iterator it =
		    sub_patterns.begin(); it != sub_patterns.end(); ++it)
		{
		  ref_ptr<pattern> sub_normalized(normalize_pattern(*it));

		  if(is_xapian_dependent(sub_normalized) &&
		     sub_normalized->get_type() == pattern::not_tp)
		    has_negative_xapian_dependent_sub_term = true;

		  normalized_sub_patterns.push_back(sub_normalized);
		}

	      if(has_negative_xapian_dependent_sub_term)
		{
		  std::vector<ref_ptr<pattern> > negative_sub_normalized;
		  for(std::vector<ref_ptr<pattern> >::const_iterator it =
		      normalized_sub_patterns.begin(); it != normalized_sub_patterns.end(); ++it)
		    negative_sub_normalized.push_back(negate_pattern(*it));

		  return pattern::make_not(pattern::make_and(negative_sub_normalized));
		}
	      else
		return pattern::make_or(normalized_sub_patterns);
	    }

	  case pattern::widen:
	    return normalize_pattern(p->get_widen_pattern());

	    // Various non-Xapian patterns, along with ?term.
	  case pattern::archive:
	  case pattern::action:
	  case pattern::automatic:
	  case pattern::bind:
	  case pattern::broken:
	  case pattern::broken_type:
	  case pattern::candidate_version:
	  case pattern::config_files:
	  case pattern::current_version:
	  case pattern::depends:
	  case pattern::description:
	  case pattern::essential:
	  case pattern::equal:
	  case pattern::false_tp:
	  case pattern::garbage:
	  case pattern::install_version:
	  case pattern::installed:
	  case pattern::maintainer:
	  case pattern::name:
	  case pattern::new_tp:
	  case pattern::obsolete:
	  case pattern::origin:
	  case pattern::priority:
	  case pattern::provides:
	  case pattern::reverse_depends:
	  case pattern::reverse_provides:
	  case pattern::section:
	  case pattern::source_package:
	  case pattern::source_version:
	  case pattern::tag:
	  case pattern::task:
	  case pattern::term:
	  case pattern::true_tp:
	  case pattern::upgradable:
	  case pattern::user_tag:
	  case pattern::version:
	  case pattern::virtual_tp:
	    return p;
	  default:
	    throw MatchingException("Internal error: unhandled pattern type in normalize_pattern()");
	  }
      }

      // Note: the incoming expression must be Xapian-normalized as
      // with normalize_pattern().
      //
      // Builds a query for the Xapian-relevant part of the search.
      //
      // Analyzes the incoming expression in an attempt to prove that
      // any matching expression will match at least one Xapian term.
      // If this can be proven, builds a Xapian query that
      // overapproximates the match set.  Otherwise, bails and returns
      // the empty query.
      //
      // Returns the new query, or an empty query if there's no
      // Xapian-dependence.
      Xapian::Query build_xapian_query(const cwidget::util::ref_ptr<pattern> &p)
      {
	switch(p->get_type())
	  {
	  case pattern::all_versions:
	    return build_xapian_query(p->get_all_versions_pattern());

	  case pattern::and_tp:
	    {
	      // We make ?and right-associative:
	      // ?and(a, b, c) => a AND (b AND c).
	      //
	      // Hm, maybe this should be the other way around?  That
	      // would make handling AND_NOT and AND_MAYBE cleaner.

	      const std::vector<ref_ptr<pattern> > &sub_patterns =
		p->get_and_patterns();

	      Xapian::Query and_not_tail;
	      // First build all the negative terms for insertion into
	      // an AND_NOT.  (of course, to collect them under a
	      // single ?not, we need to make them use OR and we need
	      // to drop the leading ?not)
	      for(std::vector<ref_ptr<pattern> >::const_reverse_iterator it =
		    sub_patterns.rbegin(); it != sub_patterns.rend(); ++it)
		{
		  // If the sub-pattern has non-Xapian pieces, we need
		  // to just throw it away (which results in the
		  // overestimate we promise) because the
		  // transformation will be an overestimate, so
		  // negating it produces an UNDERestimate and the
		  // world explodes.  This shouldn't affect scores or
		  // our ability to index, so I think it's just fine.
		  if((*it)->get_type() == pattern::not_tp &&
		     is_pure_xapian((*it)->get_not_pattern()))
		    {
		      Xapian::Query q(build_xapian_query((*it)->get_not_pattern()));

		      // Ignore empty queries; they signify that the
		      // entry should be pruned from the generated
		      // tree.
		      if(!q.empty())
			{
			  if(and_not_tail.empty())
			    and_not_tail = q;
			  else
			    and_not_tail = Xapian::Query(Xapian::Query::OP_OR,
							 q,
							 and_not_tail);
			}
		    }
		}

	      Xapian::Query and_maybe_tail;
	      // Build in positive, non-Xapian-dependent terms.  These
	      // are added to the expression using AND_MAYBE to make
	      // sure that the terms are indexed and their scores
	      // considered, but they don't constrain the search
	      // (because the Xapian query might be false when the
	      // aptitude one is true).  This would be wrong inside a
	      // NOT, but we avoid that with the check above for pure
	      // Xapian expressions.
	      for(std::vector<ref_ptr<pattern> >::const_reverse_iterator it =
		    sub_patterns.rbegin(); it != sub_patterns.rend(); ++it)
		{
		  if((*it)->get_type() != pattern::not_tp &&
		     !is_xapian_dependent(*it))
		    {
		      Xapian::Query q(build_xapian_query(*it));

		      if(and_maybe_tail.empty())
			and_maybe_tail = q;
		      else
			and_maybe_tail = Xapian::Query(Xapian::Query::OP_AND_MAYBE,
						       q,
						       and_maybe_tail);
		    }
		}

	      Xapian::Query and_tail;
	      // Now build in the positive, Xapian-dependent terms.
	      for(std::vector<ref_ptr<pattern> >::const_reverse_iterator it =
		    sub_patterns.rbegin(); it != sub_patterns.rend(); ++it)
		{
		  if((*it)->get_type() != pattern::not_tp &&
		     is_xapian_dependent(*it))
		    {
		      Xapian::Query q(build_xapian_query(*it));

		      if(!q.empty())
			{
			  if(and_tail.empty())
			    and_tail = q;
			  else
			    and_tail = Xapian::Query(Xapian::Query::OP_AND,
						     q,
						     and_tail);
			}
		    }
		}

	      if(and_tail.empty())
		return and_tail;
	      else
		{
		  // If we have negative and independent terms, we
		  // don't want this:
		  //
		  //   (a AND NOT (b AND MAYBE c))
		  //
		  // or this:
		  //
		  //   (a AND MAYBE (c AND NOT b))
		  //
		  // What we want is this:
		  //
		  //   (a AND NOT b) AND MAYBE c
		  const bool and_not_empty = and_not_tail.empty();
		  const bool and_maybe_empty = and_maybe_tail.empty();

		  if(and_not_empty && and_maybe_empty)
		    return and_tail;
		  else if(!and_not_empty && and_maybe_empty)
		    return Xapian::Query(Xapian::Query::OP_AND_NOT,
					 and_tail,
					 and_not_tail);
		  else if(and_not_empty && !and_maybe_empty)
		    return Xapian::Query(Xapian::Query::OP_AND_MAYBE,
					 and_tail,
					 and_maybe_tail);
		  else // if(!and_not_empty && !and_maybe_empty)
		    return Xapian::Query(Xapian::Query::OP_AND_MAYBE,
					 Xapian::Query(Xapian::Query::OP_AND_NOT,
						       and_tail,
						       and_not_tail),
					 and_maybe_tail);
		}
	    }

	  case pattern::any_version:
	    return build_xapian_query(p->get_any_version_pattern());

	  case pattern::for_tp:
	    return build_xapian_query(p->get_for_pattern());

	  case pattern::narrow:
	    return build_xapian_query(p->get_narrow_pattern());

	  case pattern::not_tp:
	    // D'oh!  Should I assert-fail here?
	    //
	    // "not" is handled very specially; normalization should
	    // mean it only occurs inside "and" or at the top level.
	    // So I assume that this is a top-level expression; if
	    // "not" occurs at the top-level, we just can't do
	    // anything.
	    return Xapian::Query();

	  case pattern::or_tp:
	    {
	      const std::vector<ref_ptr<pattern> > &sub_patterns =
		p->get_or_patterns();

	      Xapian::Query tail;

	      for(std::vector<ref_ptr<pattern> >::const_reverse_iterator it =
		    sub_patterns.rbegin(); it != sub_patterns.rend(); ++it)
		{
		  if((*it)->get_type() != pattern::not_tp)
		    {
		      Xapian::Query q(build_xapian_query(*it));

		      if(!q.empty())
			{
			  if(tail.empty())
			    tail = q;
			  else
			    tail = Xapian::Query(Xapian::Query::OP_OR,
						 q,
						 tail);
			}
		    }
		}

	      return tail;
	    }

	  case pattern::widen:
	    return build_xapian_query(p->get_widen_pattern());

	  case pattern::term:
	    // We try stemming everything as if it were English.
	    //
	    // TODO: guess which language to use for stemming somehow
	    // (how? the locale isn't reliable; we care about the
	    // language of the package descriptions).
	    return stem_term(p->get_term_term());

	  case pattern::archive:
	  case pattern::action:
	  case pattern::automatic:
	  case pattern::bind:
	  case pattern::broken:
	  case pattern::broken_type:
	  case pattern::candidate_version:
	  case pattern::config_files:
	  case pattern::current_version:
	  case pattern::depends:
	  case pattern::description:
	  case pattern::essential:
	  case pattern::equal:
	  case pattern::false_tp:
	  case pattern::garbage:
	  case pattern::install_version:
	  case pattern::installed:
	  case pattern::maintainer:
	  case pattern::name:
	  case pattern::new_tp:
	  case pattern::obsolete:
	  case pattern::origin:
	  case pattern::priority:
	  case pattern::provides:
	  case pattern::reverse_depends:
	  case pattern::reverse_provides:
	  case pattern::section:
	  case pattern::source_package:
	  case pattern::source_version:
	  case pattern::tag:
	  case pattern::task:
	  case pattern::true_tp:
	  case pattern::upgradable:
	  case pattern::user_tag:
	  case pattern::version:
	  case pattern::virtual_tp:
	    return Xapian::Query();
	  default:
	    throw MatchingException("Internal error: unhandled pattern type in build_xapian_query()");
	  }
      }
    }

    /** \brief Get all the terms used inside ?term in the given
     *  pattern.
     *
     *  \param p   The pattern to analyze.
     *
     *  \param output  A set into which the terms will be
     *                 placed;
     */
    void get_terms(const ref_ptr<pattern> &p,
		   std::set<std::string> &output)
    {
      switch(p->get_type())
	{
	  // Structural patterns:
	case pattern::all_versions:
	  get_terms(p->get_all_versions_pattern(),
		    output);
	  break;

	case pattern::and_tp:
	  for(std::vector<ref_ptr<pattern> >::const_iterator it =
		p->get_and_patterns().begin();
	      it != p->get_and_patterns().end(); ++it)
	    get_terms(*it, output);
	  break;

	case pattern::any_version:
	  get_terms(p->get_any_version_pattern(),
		    output);
	  break;

	case pattern::for_tp:
	  get_terms(p->get_for_pattern(),
		    output);
	  break;

	case pattern::narrow:
	  get_terms(p->get_narrow_filter(),
		    output);
	  get_terms(p->get_narrow_pattern(),
		    output);
	  break;

	case pattern::not_tp:
	  get_terms(p->get_not_pattern(),
		    output);
	  break;

	case pattern::or_tp:
	  for(std::vector<ref_ptr<pattern> >::const_iterator it =
		p->get_or_patterns().begin();
	      it != p->get_or_patterns().end(); ++it)
	    get_terms(*it, output);
	  break;

	case pattern::widen:
	  get_terms(p->get_widen_pattern(),
		    output);
	  break;

	  // Atomic patterns with sub-patterns:
	case pattern::bind:
	  get_terms(p->get_bind_pattern(),
		    output);
	  break;

	case pattern::depends:
	  get_terms(p->get_depends_pattern(),
		    output);
	  break;

	case pattern::provides:
	  get_terms(p->get_provides_pattern(),
		    output);
	  break;

	case pattern::reverse_depends:
	  get_terms(p->get_reverse_depends_pattern(),
		    output);
	  break;

	case pattern::reverse_provides:
	  get_terms(p->get_reverse_provides_pattern(),
		    output);
	  break;

	  // Terms themselves:
	case pattern::term:
	  output.insert(p->get_term_term());
	  break;

	  // Atomic patterns with no sub-patterns:
	case pattern::archive:
	case pattern::action:
	case pattern::automatic:
	case pattern::broken:
	case pattern::broken_type:
	case pattern::candidate_version:
	case pattern::config_files:
	case pattern::current_version:
	case pattern::description:
	case pattern::essential:
	case pattern::equal:
	case pattern::false_tp:
	case pattern::garbage:
	case pattern::install_version:
	case pattern::installed:
	case pattern::maintainer:
	case pattern::name:
	case pattern::new_tp:
	case pattern::obsolete:
	case pattern::origin:
	case pattern::priority:
	case pattern::section:
	case pattern::source_package:
	case pattern::source_version:
	case pattern::tag:
	case pattern::task:
	case pattern::true_tp:
	case pattern::upgradable:
	case pattern::user_tag:
	case pattern::version:
	case pattern::virtual_tp:
	  // Nothing to do; they won't have sub-terms.
	  break;

	default:
	  throw MatchingException(std::string("Internal error: unhandled pattern type in ") + __func__);
	}
    }

    /** \brief Return all the "obscured" terms in the
     *  given pattern.
     *
     *  Obscured terms are terms hidden inside constructs like ?bind
     *  and ?depends that we can't generate a sensible Xapian query
     *  for.
     *
     *  \param p   The pattern to analyze
     *  \param output   The set into which the obscured terms
     *                  will be placed.
     *  \param top_level_obscured   Normally false; if true,
     *                              terms at the top level
     *                              will be treated as if
     *                              they are obscured.
     */
    void find_obscured_terms(const ref_ptr<pattern> &p,
			     std::set<std::string> &output,
			     bool top_level_obscured)
    {
      switch(p->get_type())
	{
	  // Structural patterns:
	case pattern::all_versions:
	  find_obscured_terms(p->get_all_versions_pattern(),
			      output,
			      top_level_obscured);
	  break;

	case pattern::and_tp:
	  for(std::vector<ref_ptr<pattern> >::const_iterator it =
		p->get_and_patterns().begin();
	      it != p->get_and_patterns().end(); ++it)
	    find_obscured_terms(*it, output,
				top_level_obscured);
	  break;

	case pattern::any_version:
	  find_obscured_terms(p->get_any_version_pattern(),
			      output,
			      top_level_obscured);
	  break;

	case pattern::for_tp:
	  find_obscured_terms(p->get_for_pattern(),
			      output,
			      top_level_obscured);
	  break;

	case pattern::narrow:
	  find_obscured_terms(p->get_narrow_filter(),
			      output,
			      top_level_obscured);
	  find_obscured_terms(p->get_narrow_pattern(),
			      output,
			      top_level_obscured);
	  break;

	case pattern::not_tp:
	  find_obscured_terms(p->get_not_pattern(),
			      output,
			      top_level_obscured);
	  break;

	case pattern::or_tp:
	  for(std::vector<ref_ptr<pattern> >::const_iterator it =
		p->get_or_patterns().begin();
	      it != p->get_or_patterns().end(); ++it)
	    find_obscured_terms(*it, output,
				top_level_obscured);
	  break;

	case pattern::widen:
	  find_obscured_terms(p->get_widen_pattern(),
			      output,
			      top_level_obscured);
	  break;

	  // Atomic patterns with sub-patterns:
	case pattern::bind:
	  find_obscured_terms(p->get_bind_pattern(),
			      output,
			      true);
	  break;

	case pattern::depends:
	  find_obscured_terms(p->get_depends_pattern(),
			      output,
			      true);
	  break;

	case pattern::provides:
	  find_obscured_terms(p->get_provides_pattern(),
			      output,
			      true);
	  break;

	case pattern::reverse_depends:
	  find_obscured_terms(p->get_reverse_depends_pattern(),
			      output,
			      true);
	  break;

	case pattern::reverse_provides:
	  find_obscured_terms(p->get_reverse_provides_pattern(),
			      output,
			      true);
	  break;

	  // Terms themselves:
	case pattern::term:
	  if(top_level_obscured)
	    output.insert(p->get_term_term());
	  break;

	  // Atomic patterns with no sub-patterns:
	case pattern::archive:
	case pattern::action:
	case pattern::automatic:
	case pattern::broken:
	case pattern::broken_type:
	case pattern::candidate_version:
	case pattern::config_files:
	case pattern::current_version:
	case pattern::description:
	case pattern::essential:
	case pattern::equal:
	case pattern::false_tp:
	case pattern::garbage:
	case pattern::install_version:
	case pattern::installed:
	case pattern::maintainer:
	case pattern::name:
	case pattern::new_tp:
	case pattern::obsolete:
	case pattern::origin:
	case pattern::priority:
	case pattern::section:
	case pattern::source_package:
	case pattern::source_version:
	case pattern::tag:
	case pattern::task:
	case pattern::true_tp:
	case pattern::upgradable:
	case pattern::user_tag:
	case pattern::version:
	case pattern::virtual_tp:
	  break;

	default:
	  throw MatchingException(std::string("Internal error: unhandled pattern type in ") + __func__);
	}
    }

    ref_ptr<structural_match>
    get_match(const ref_ptr<pattern> &p,
	      const pkgCache::PkgIterator &pkg,
	      const pkgCache::VerIterator &ver,
	      const cwidget::util::ref_ptr<search_cache> &search_info,
	      aptitudeDepCache &cache,
	      pkgRecords &records,
	      bool debug)
    {
      eassert(p.valid());
      eassert(search_info.valid());

      std::vector<matchable> initial_pool;

      if(pkg.VersionList().end())
	initial_pool.push_back(matchable(pkg));
      else if(ver.end())
	{
	  for(pkgCache::VerIterator ver2 = pkg.VersionList();
	      !ver2.end(); ++ver2)
	    {
	      initial_pool.push_back(matchable(pkg, ver2));
	    }
	}
      else
	{
	  eassert(ver.ParentPkg() == pkg);

	  initial_pool.push_back(matchable(pkg, ver));
	}

      std::sort(initial_pool.begin(), initial_pool.end());

      stack st;
      st.push_back(&initial_pool);

      ref_ptr<search_cache::implementation> search_info_imp =
	search_info.dyn_downcast<search_cache::implementation>();
      eassert(search_info_imp.valid());

      return evaluate_structural(structural_eval_any,
				 p,
				 st,
				 search_info_imp,
				 initial_pool,
				 cache,
				 records,
				 debug);
    }

    ref_ptr<structural_match>
    get_match(const ref_ptr<pattern> &p,
	      const pkgCache::PkgIterator &pkg,
	      const cwidget::util::ref_ptr<search_cache> &search_info,
	      aptitudeDepCache &cache,
	      pkgRecords &records,
	      bool debug)
    {
      return get_match(p, pkg,
		       pkgCache::VerIterator(cache),
		       search_info, cache, records, debug);
    }

    void xapian_info::setup(const Xapian::Database &db,
			    const ref_ptr<pattern> &p,
			    bool debug)
    {
      matched_packages_valid = false;

      if(debug)
	std::cout << "Finding Xapian hits for " << serialize_pattern(p) << std::endl;

      ref_ptr<pattern> normalized(normalize_pattern(p));

      if(debug)
	std::cout << "Pattern Xapian-normalized to: " << serialize_pattern(normalized)
		  << (is_xapian_dependent(normalized) ? " [Xapian-dependent]" : " [not Xapian-dependent]")
		  << std::endl;


      Xapian::Query q(build_xapian_query(normalized));

      if(q.empty())
	{
	  if(debug)
	    std::cout << "Can't build a Xapian query for this search." << std::endl
		      << "Each incoming package will be tested separately." << std::endl;
	}
      else
	{
	  if(debug)
	    std::cout << "Xapian query built: " << q.get_description() << std::endl;

	  Xapian::Enquire enq(db);
	  enq.set_query(q);

	  xapian_match = enq.get_mset(0, 100000);

	  if(debug)
	    std::cout << "  (" << xapian_match.size() << " hits)"
		      << std::endl;

	  // Read off the matches and stuff them into the list of
	  // matched packages for future reference.
	  for(Xapian::MSetIterator it = xapian_match.begin();
	      it != xapian_match.end(); ++it)
	    matched_packages.push_back(*it);

	  std::sort(matched_packages.begin(), matched_packages.end());

	  matched_packages_valid = true;
	}
    }

    void search(const ref_ptr<pattern> &p,
		const ref_ptr<search_cache> &search_info,
		std::vector<std::pair<pkgCache::PkgIterator, ref_ptr<structural_match> > > &matches,
		aptitudeDepCache &cache,
		pkgRecords &records,
		bool debug)
    {
      eassert(p.valid());
      eassert(search_info.valid());

      const ref_ptr<search_cache::implementation> info = search_info.dyn_downcast<search_cache::implementation>();
      eassert(info.valid());

      const xapian_info &xapian_results(info->get_toplevel_xapian_info(p, debug));

      if(!xapian_results.get_matched_packages_valid())
	{
	  if(debug)
	    std::cout << "Failed to build a Xapian query for this search." << std::endl
		      << "Falling back to testing each package." << std::endl;

	  for(pkgCache::PkgIterator pkg = cache.PkgBegin();
	      !pkg.end(); ++pkg)
	    {
	      if(pkg.VersionList().end() && pkg.ProvidesList().end())
		continue;

	      // TODO: how do I make sure the sub-patterns are
	      // searched using the right xapian_info?  I could thread
	      // the current top-level or the current xapian_info
	      // through, I suppose.  Or I could use a global list of
	      // term postings and only store match sets on a
	      // per-toplevel basis (that might work, actually?).

	      ref_ptr<structural_match> m(get_match(p, pkg,
						    info,
						    cache,
						    records,
						    debug));

	      if(m.valid())
		matches.push_back(std::make_pair(pkg, m));
	    }
	}
      else
	{
	  Xapian::MSet mset(xapian_results.get_xapian_match());
	  for(Xapian::MSetIterator it = mset.begin();
	      it != mset.end(); ++it)
	    {
	      std::string name(it.get_document().get_data());

	      if(debug)
		std::cout << "HIT: " << name
			  << " (score " << it.get_weight() << ")" << std::endl;

	      pkgCache::PkgIterator pkg(cache.FindPkg(name));
	      if(pkg.end())
		{
		  if(debug)
		    std::cout << "W: unable to find the package " << name
			      << std::endl;
		}
	      else if(!(pkg.VersionList().end() && pkg.ProvidesList().end()))
		{
		  ref_ptr<structural_match> m(get_match(p, pkg,
							info,
							cache,
							records,
							debug));

		  if(m.valid())
		    matches.push_back(std::make_pair(pkg, m));
		}
	    }
	}
    }
  }
}

