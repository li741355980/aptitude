// aptitude_resolver.cc
//
//   Copyright (C) 2005, 2008 Daniel Burrows
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

#include "aptitude_resolver.h"

#include "config_signal.h"

aptitude_resolver::aptitude_resolver(int step_score,
				     int broken_score,
				     int unfixed_soft_score,
				     int infinity,
				     int max_successors,
				     int resolution_score,
				     aptitudeDepCache *cache)
  :generic_problem_resolver<aptitude_universe>(step_score, broken_score, unfixed_soft_score, infinity, max_successors, resolution_score, aptitude_universe(cache))
{
  set_remove_stupid(aptcfg->FindB(PACKAGE "::ProblemResolver::Remove-Stupid-Pairs", true));

  for(pkgCache::PkgIterator i = cache->PkgBegin(); !i.end(); ++i)
    {
      pkgDepCache::StateCache &s((*cache)[i]);

      if(!s.Keep())
	keep_all_solution.put(package(i, cache),
			      action(version(i, i.CurrentVer(), cache),
				     dep(), false, 0));
    }

  if(aptcfg->FindB(PACKAGE "::ProblemResolver::Discard-Null-Solution", true) &&
     !keep_all_solution.empty())
    add_conflict(keep_all_solution);
}

imm::map<aptitude_resolver::package, aptitude_resolver::action>
aptitude_resolver::get_keep_all_solution() const
{
  return keep_all_solution;
}

bool aptitude_resolver::is_break_hold(const aptitude_resolver::version &v) const
{
  const aptitude_resolver::package p(v.get_package());
  const aptitudeDepCache::aptitude_state &state=get_universe().get_cache()->get_ext_state(p.get_pkg());

  return
    !p.get_pkg().CurrentVer().end() &&
    v != p.current_version() &&
    (state.selection_state == pkgCache::State::Hold ||
     (!v.get_ver().end() && state.forbidver == v.get_ver().VerStr()));
}

namespace
{
  bool version_provides(const pkgCache::VerIterator &ver,
			const pkgCache::PkgIterator &pkg_name)
  {
    for(pkgCache::PrvIterator prv = ver.ProvidesList();
	!prv.end(); ++prv)
      if(prv.ParentPkg() == pkg_name)
	return true;

    return false;
  }
}

void aptitude_resolver::add_full_replacement_score(const pkgCache::VerIterator &src,
						   const pkgCache::PkgIterator &real_target,
						   const pkgCache::VerIterator &provider,
						   int full_replacement_score,
						   int undo_full_replacement_score)
{
  pkgCache::PkgIterator src_pkg = src.ParentPkg();
  bool src_installed = (src_pkg->CurrentState != pkgCache::State::NotInstalled &&
			src_pkg->CurrentState != pkgCache::State::ConfigFiles) &&
    src_pkg.CurrentVer() == src;

  pkgCache::PkgIterator target = !provider.end() ? provider.ParentPkg() : real_target;

  bool target_installed = (target->CurrentState != pkgCache::State::NotInstalled &&
			   target->CurrentState != pkgCache::State::ConfigFiles);

  pkgDepCache * const cache = get_universe().get_cache();

  if(!target_installed)
    {
      // If the target isn't installed and the source isn't installed,
      // do nothing.
      if(!src_installed)
	return;
      else
	{
	  // Penalize installing any version of the target package
	  // (that provides the name) if the source is being removed.
	  for(pkgCache::VerIterator target_ver = target.VersionList();
	      !target_ver.end(); ++target_ver)
	    {
	      // If we're working through a Provides, only apply the
	      // penalty to versions of the target that provide the
	      // name in question.
	      if(!provider.end())
		{
		  if(!version_provides(target_ver, real_target))
		    continue;
		}

	      // Penalize removing the source and installing the target.
	      //
	      // It's important that we go down this branch at most
	      // once per package (since we only do it if the source
	      // *version* is installed).
	      imm::set<aptitude_universe::version> s;

	      s.insert(aptitude_universe::version(src.ParentPkg(),
						  pkgCache::VerIterator(*cache),
						  cache));
	      s.insert(aptitude_universe::version(target,
						  target_ver,
						  cache));

	      add_joint_score(s, undo_full_replacement_score);
	    }
	}
    }
  else
    {
      // The target *is* installed.  Favor removing it in favor of
      // installing the source.
      //
      // If the source is already installed, just add a bonus to the
      // target's removal.
      //
      // It's important that we go down this branch at most once
      // per package (since we only do it if the source *version*
      // is installed).
      if(src_installed)
	{
	  add_version_score(aptitude_universe::version(target,
						       pkgCache::VerIterator(*cache),
						       cache),
			    full_replacement_score);

	  // If we are working through a provides, find all versions
	  // that don't provide the package being replaced and apply
	  // the same score to them as to removal.
	  if(!provider.end())
	    {
	      for(pkgCache::VerIterator target_ver = target.VersionList();
		  !target_ver.end(); ++target_ver)
		{
		  if(version_provides(target_ver, real_target))
		    add_version_score(aptitude_universe::version(target,
								 target_ver,
								 cache),
				      full_replacement_score);
		}
	    }
	}
      else
	{
	  {
	    imm::set<aptitude_universe::version> s;

	    s.insert(aptitude_universe::version(src.ParentPkg(),
						src,
						cache));
	    s.insert(aptitude_universe::version(target,
						pkgCache::VerIterator(*cache),
						cache));

	    add_joint_score(s, full_replacement_score);
	  }

	  // If we are working through a provides, find all versions
	  // that don't provide the package being replaced and apply
	  // the same score to them as to removal.
	  if(!provider.end())
	    {
	      for(pkgCache::VerIterator target_ver = target.VersionList();
		  !target_ver.end(); ++target_ver)
		{
		  if(version_provides(target_ver, real_target))
		    {
		      imm::set<aptitude_universe::version> s;

		      s.insert(aptitude_universe::version(src.ParentPkg(),
							  src,
							  cache));
		      s.insert(aptitude_universe::version(target,
							  target_ver,
							  cache));

		      add_joint_score(s, full_replacement_score);
		    }
		}
	    }
	}
    }
}

void aptitude_resolver::add_action_scores(int preserve_score, int auto_score,
					  int remove_score, int keep_score,
					  int install_score, int upgrade_score,
					  int non_default_score, int essential_remove,
					  int full_replacement_score,
					  int undo_full_replacement_score,
					  int break_hold_score,
					  bool allow_break_holds_and_forbids)
{
  // Should I stick with APT iterators instead?  This is a bit more
  // convenient, though..
  for(aptitude_universe::package_iterator pi = get_universe().packages_begin();
      !pi.end(); ++pi)
    {
      const aptitude_universe::package &p=*pi;
      aptitudeDepCache::aptitude_state &state=get_universe().get_cache()->get_ext_state(p.get_pkg());
      pkgDepCache::StateCache &apt_state = (*get_universe().get_cache())[p.get_pkg()];

      // Packages are considered "manual" if either they were manually
      // installed, or if they are currently installed and were
      // manually removed.
      //
      // There is NO PENALTY for any change to a non-manual package's
      // state, other than the usual priority-based and non-default
      // version weighting.
      bool manual = ((!p.current_version().get_ver().end()) && (apt_state.Flags & pkgCache::Flag::Auto)) ||
	(p.current_version().get_ver().end() && (p.get_pkg().CurrentVer().end() || state.remove_reason == aptitudeDepCache::manual));

      for(aptitude_universe::package::version_iterator vi=p.versions_begin(); !vi.end(); ++vi)
	{
	  aptitude_universe::version v=*vi;

	  // Remember, the "current version" is the InstVer.
	  if(v==p.current_version())
	    {
	      if(manual)
		add_version_score(v, preserve_score);
	      else
		add_version_score(v, auto_score);
	    }
	  // Ok, if this version is selected it'll be a change.
	  else if(v.get_ver()==p.get_pkg().CurrentVer())
	    {
	      if(manual)
		add_version_score(v, keep_score);
	    }
	  else if(v.get_ver().end())
	    {
	      if(manual)
		add_version_score(v, remove_score);
	    }
	  else if(v.get_ver()==(*get_universe().get_cache())[p.get_pkg()].CandidateVerIter(*get_universe().get_cache()))
	    {
	      if(manual)
		{
		  // Could try harder not to break holds.
		  if(p.get_pkg().CurrentVer().end())
		    add_version_score(v, install_score);
		  else
		    add_version_score(v, upgrade_score);
		}
	    }
	  else
	    // We know that:
	    //  - this version wasn't requested by the user
	    //  - it's not the current version
	    //  - it's not the candidate version
	    //  - it's not a removal
	    //  - it follows that this is a non-default version.
	    add_version_score(v, non_default_score);

	  // This logic is slightly duplicated in resolver_manger.cc,
	  // but it's not trivial to merge.
	  if(is_break_hold(v))
	    {
	      add_version_score(v, break_hold_score);
	      if(!allow_break_holds_and_forbids)
		reject_version(v);
	    }

	  // In addition, add the essential-removal score:
	  if((p.get_pkg()->Flags & pkgCache::Flag::Essential) &&
	     v.get_ver().end())
	    add_version_score(v, essential_remove);

	  // Look for a conflicts/provides/replaces.
	  if(!v.get_ver().end())
	    {
	      std::set<pkgCache::PkgIterator> replaced_packages;
	      for(pkgCache::DepIterator dep = v.get_ver().DependsList();
		  !dep.end(); ++dep)
		{
		  if((dep->Type & ~pkgCache::Dep::Or) == pkgCache::Dep::Replaces &&
		     aptitude::apt::is_full_replacement(dep))
		    {
		      pkgCache::PkgIterator target = dep.TargetPkg();
		      // First replace the literal package the dep
		      // names.
		      if(replaced_packages.find(target) == replaced_packages.end())
			{
			  replaced_packages.insert(target);
			  add_full_replacement_score(v.get_ver(),
						     target,
						     pkgCache::VerIterator(*get_universe().get_cache()),
						     full_replacement_score,
						     undo_full_replacement_score);
			}

		      // Now find the providers and replace them.  NB:
		      // providers are versions, not packages; how do
		      // I handle that?  Add scores to each version
		      // not providing the given name?
		      for(pkgCache::PrvIterator prv = target.ProvidesList();
			  !prv.end(); ++prv)
			{
			  pkgCache::VerIterator provider = prv.OwnerVer();

			  if(replaced_packages.find(provider.ParentPkg()) == replaced_packages.end())
			    {
			      replaced_packages.insert(provider.ParentPkg());
			      add_full_replacement_score(v.get_ver(),
							 target,
							 provider,
							 full_replacement_score,
							 undo_full_replacement_score);
			    }
			}
		    }
		}
	    }
	}
    }
}

void aptitude_resolver::add_priority_scores(int important,
					    int required,
					    int standard,
					    int optional,
					    int extra)
{
  for(aptitude_universe::package_iterator pi = get_universe().packages_begin();
      !pi.end(); ++pi)
    for(aptitude_universe::package::version_iterator vi=(*pi).versions_begin(); !vi.end(); ++vi)
      {
	if(vi.get_ver().end())
	  continue;

	int score_tweak=0;
	switch(vi.get_ver()->Priority)
	  {
	  case pkgCache::State::Important:
	    score_tweak=important;
	    break;
	  case pkgCache::State::Required:
	    score_tweak=required;
	    break;
	  case pkgCache::State::Standard:
	    score_tweak=standard;
	    break;
	  case pkgCache::State::Optional:
	    score_tweak=optional;
	    break;
	  case pkgCache::State::Extra:
	    score_tweak=extra;
	    break;
	  default:
	    // ??????
	    break;
	  }

	add_version_score(*vi, score_tweak);
      }
}
