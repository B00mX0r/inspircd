/*
 * InspIRCd -- Internet Relay Chat Daemon
 *
 *   Copyright (C) 2013 Peter Powell <petpow@saberuk.com>
 *   Copyright (C) 2012 ChrisTX <chris@rev-crew.info>
 *
 * This file is part of InspIRCd.  InspIRCd is free software: you can
 * redistribute it and/or modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/// $CompilerFlags: find_compiler_flags("re2" "")
/// $LinkerFlags: find_linker_flags("re2" "-lre2")

/// $PackageInfo: require_system("darwin") pkg-config re2
/// $PackageInfo: require_system("debian" "8.0") libre2-dev pkg-config
/// $PackageInfo: require_system("ubuntu" "15.10") libre2-dev pkg-config


#include "inspircd.h"
#include "modules/regex.h"

// Fix warnings about shadowing on GCC.
#if defined __GNUC__
# pragma GCC diagnostic ignored "-Wshadow"
#endif

#include <re2/re2.h>

class RE2Regex : public Regex
{
	RE2 regexcl;

 public:
	RE2Regex(const std::string& rx) : Regex(rx), regexcl(rx, RE2::Quiet)
	{
		if (!regexcl.ok())
		{
			throw RegexException(rx, regexcl.error());
		}
	}

	bool Matches(const std::string& text) override
	{
		return RE2::FullMatch(text, regexcl);
	}
};

class RE2Factory : public RegexFactory
{
 public:
	RE2Factory(Module* m) : RegexFactory(m, "regex/re2") { }
	Regex* Create(const std::string& expr) override
	{
		return new RE2Regex(expr);
	}
};

class ModuleRegexRE2 : public Module
{
	RE2Factory ref;

 public:
	ModuleRegexRE2() : ref(this)
	{
	}

	Version GetVersion() override
	{
		return Version("Regex Provider Module for RE2", VF_VENDOR);
	}
};

MODULE_INIT(ModuleRegexRE2)
