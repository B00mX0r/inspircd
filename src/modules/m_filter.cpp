/*
 * InspIRCd -- Internet Relay Chat Daemon
 *
 *   Copyright (C) 2009 Daniel De Graaf <danieldg@inspircd.org>
 *   Copyright (C) 2004, 2008 Craig Edwards <craigedwards@brainbox.cc>
 *   Copyright (C) 2007 Dennis Friis <peavey@inspircd.org>
 *   Copyright (C) 2007 Robin Burchell <robin+git@viroteck.net>
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


#include "inspircd.h"
#include "xline.h"
#include "modules/regex.h"
#include "modules/server.h"
#include "modules/shun.h"
#include "modules/stats.h"

enum FilterFlags
{
	FLAG_PART = 2,
	FLAG_QUIT = 4,
	FLAG_PRIVMSG = 8,
	FLAG_NOTICE = 16
};

enum FilterAction
{
	FA_GLINE,
	FA_ZLINE,
	FA_WARN,
	FA_BLOCK,
	FA_SILENT,
	FA_KILL,
	FA_SHUN,
	FA_NONE
};

class FilterResult
{
 public:
	Regex* regex;
	std::string freeform;
	std::string reason;
	FilterAction action;
	unsigned long duration;
	bool from_config;

	bool flag_no_opers;
	bool flag_part_message;
	bool flag_quit_message;
	bool flag_privmsg;
	bool flag_notice;
	bool flag_strip_color;

	FilterResult(dynamic_reference<RegexFactory>& RegexEngine, const std::string& free, const std::string& rea, FilterAction act, unsigned long gt, const std::string& fla, bool cfg)
		: freeform(free)
		, reason(rea)
		, action(act)
		, duration(gt)
		, from_config(cfg)
	{
		if (!RegexEngine)
			throw ModuleException("Regex module implementing '"+RegexEngine.GetProvider()+"' is not loaded!");
		regex = RegexEngine->Create(free);
		this->FillFlags(fla);
	}

	char FillFlags(const std::string &fl)
	{
		flag_no_opers = flag_part_message = flag_quit_message = flag_privmsg =
			flag_notice = flag_strip_color = false;

		for (std::string::const_iterator n = fl.begin(); n != fl.end(); ++n)
		{
			switch (*n)
			{
				case 'o':
					flag_no_opers = true;
				break;
				case 'P':
					flag_part_message = true;
				break;
				case 'q':
					flag_quit_message = true;
				break;
				case 'p':
					flag_privmsg = true;
				break;
				case 'n':
					flag_notice = true;
				break;
				case 'c':
					flag_strip_color = true;
				break;
				case '*':
					flag_no_opers = flag_part_message = flag_quit_message =
						flag_privmsg = flag_notice = flag_strip_color = true;
				break;
				default:
					return *n;
				break;
			}
		}
		return 0;
	}

	std::string GetFlags()
	{
		std::string flags;
		if (flag_no_opers)
			flags.push_back('o');
		if (flag_part_message)
			flags.push_back('P');
		if (flag_quit_message)
			flags.push_back('q');
		if (flag_privmsg)
			flags.push_back('p');
		if (flag_notice)
			flags.push_back('n');

		/* Order is important here, 'c' must be the last char in the string as it is unsupported
		 * on < 2.0.10, and the logic in FillFlags() stops parsing when it ecounters an unknown
		 * character.
		 */
		if (flag_strip_color)
			flags.push_back('c');

		if (flags.empty())
			flags.push_back('-');

		return flags;
	}

	FilterResult()
	{
	}
};

class CommandFilter : public Command
{
 public:
	CommandFilter(Module* f)
		: Command(f, "FILTER", 1, 5)
	{
		flags_needed = 'o';
		this->syntax = "<filter-definition> <action> <flags> [<duration>] :<reason>";
	}
	CmdResult Handle(User* user, const Params& parameters) override;

	RouteDescriptor GetRouting(User* user, const Params& parameters) override
	{
		return ROUTE_BROADCAST;
	}
};

class ModuleFilter : public Module, public ServerEventListener, public Stats::EventListener
{
	typedef insp::flat_set<std::string, irc::insensitive_swo> ExemptTargetSet;

	bool initing;
	bool notifyuser;
	RegexFactory* factory;
	void FreeFilters();

 public:
	CommandFilter filtcommand;
	dynamic_reference<RegexFactory> RegexEngine;

	std::vector<FilterResult> filters;
	int flags;

	// List of channel names excluded from filtering.
	ExemptTargetSet exemptedchans;

	// List of target nicknames excluded from filtering.
	ExemptTargetSet exemptednicks;

	ModuleFilter();
	void init() override;
	CullResult cull() override;
	ModResult OnUserPreMessage(User* user, const MessageTarget& target, MessageDetails& details) override;
	FilterResult* FilterMatch(User* user, const std::string &text, int flags);
	bool DeleteFilter(const std::string &freeform);
	std::pair<bool, std::string> AddFilter(const std::string& freeform, FilterAction type, const std::string& reason, unsigned long duration, const std::string& flags);
	void ReadConfig(ConfigStatus& status) override;
	Version GetVersion() override;
	std::string EncodeFilter(FilterResult* filter);
	FilterResult DecodeFilter(const std::string &data);
	void OnSyncNetwork(ProtocolInterface::Server& server) override;
	void OnDecodeMetaData(Extensible* target, const std::string &extname, const std::string &extdata) override;
	ModResult OnStats(Stats::Context& stats) override;
	ModResult OnPreCommand(std::string& command, CommandBase::Params& parameters, LocalUser* user, bool validated) override;
	void OnUnloadModule(Module* mod) override;
	bool AppliesToMe(User* user, FilterResult* filter, int flags);
	void ReadFilters();
	static bool StringToFilterAction(const std::string& str, FilterAction& fa);
	static std::string FilterActionToString(FilterAction fa);
};

CmdResult CommandFilter::Handle(User* user, const Params& parameters)
{
	if (parameters.size() == 1)
	{
		/* Deleting a filter */
		Module *me = creator;
		if (static_cast<ModuleFilter *>(me)->DeleteFilter(parameters[0]))
		{
			user->WriteNotice("*** Removed filter '" + parameters[0] + "'");
			ServerInstance->SNO->WriteToSnoMask(IS_LOCAL(user) ? 'f' : 'F', "FILTER: "+user->nick+" removed filter '"+parameters[0]+"'");
			return CMD_SUCCESS;
		}
		else
		{
			user->WriteNotice("*** Filter '" + parameters[0] + "' not found in list, try /stats s.");
			return CMD_FAILURE;
		}
	}
	else
	{
		/* Adding a filter */
		if (parameters.size() >= 4)
		{
			const std::string& freeform = parameters[0];
			FilterAction type;
			const std::string& flags = parameters[2];
			unsigned int reasonindex;
			unsigned long duration = 0;

			if (!ModuleFilter::StringToFilterAction(parameters[1], type))
			{
				if (ServerInstance->XLines->GetFactory("SHUN"))
					user->WriteNotice("*** Invalid filter type '" + parameters[1] + "'. Supported types are 'gline', 'zline', 'none', 'warn', 'block', 'silent', 'kill', and 'shun'.");
				else
					user->WriteNotice("*** Invalid filter type '" + parameters[1] + "'. Supported types are 'gline', 'zline', 'none', 'warn', 'block', 'silent', and 'kill'.");
				return CMD_FAILURE;
			}

			if (type == FA_GLINE || type == FA_ZLINE || type == FA_SHUN)
			{
				if (parameters.size() >= 5)
				{
					if (!InspIRCd::Duration(parameters[3], duration))
					{
						user->WriteNotice("*** Invalid duration for filter");
						return CMD_FAILURE;
					}
					reasonindex = 4;
				}
				else
				{
					user->WriteNotice("*** Not enough parameters: When setting a '" + parameters[1] + "' type filter, a duration must be specified as the third parameter.");
					return CMD_FAILURE;
				}
			}
			else
			{
				reasonindex = 3;
			}

			Module *me = creator;
			std::pair<bool, std::string> result = static_cast<ModuleFilter *>(me)->AddFilter(freeform, type, parameters[reasonindex], duration, flags);
			if (result.first)
			{
				user->WriteNotice("*** Added filter '" + freeform + "', type '" + parameters[1] + "'" +
					(duration ? ", duration " +  parameters[3] : "") + ", flags '" + flags + "', reason: '" +
					parameters[reasonindex] + "'");

				ServerInstance->SNO->WriteToSnoMask(IS_LOCAL(user) ? 'f' : 'F', "FILTER: "+user->nick+" added filter '"+freeform+"', type '"+parameters[1]+"', "+(duration ? "duration "+parameters[3]+", " : "")+"flags '"+flags+"', reason: "+parameters[reasonindex]);

				return CMD_SUCCESS;
			}
			else
			{
				user->WriteNotice("*** Filter '" + freeform + "' could not be added: " + result.second);
				return CMD_FAILURE;
			}
		}
		else
		{
			user->WriteNotice("*** Not enough parameters.");
			return CMD_FAILURE;
		}

	}
}

bool ModuleFilter::AppliesToMe(User* user, FilterResult* filter, int iflags)
{
	if ((filter->flag_no_opers) && user->IsOper())
		return false;
	if ((iflags & FLAG_PRIVMSG) && (!filter->flag_privmsg))
		return false;
	if ((iflags & FLAG_NOTICE) && (!filter->flag_notice))
		return false;
	if ((iflags & FLAG_QUIT)   && (!filter->flag_quit_message))
		return false;
	if ((iflags & FLAG_PART)   && (!filter->flag_part_message))
		return false;
	return true;
}

ModuleFilter::ModuleFilter()
	: ServerEventListener(this)
	, Stats::EventListener(this)
	, initing(true)
	, filtcommand(this)
	, RegexEngine(this, "regex")
{
}

void ModuleFilter::init()
{
	ServerInstance->SNO->EnableSnomask('f', "FILTER");
}

CullResult ModuleFilter::cull()
{
	FreeFilters();
	return Module::cull();
}

void ModuleFilter::FreeFilters()
{
	for (std::vector<FilterResult>::const_iterator i = filters.begin(); i != filters.end(); ++i)
		delete i->regex;

	filters.clear();
}

ModResult ModuleFilter::OnUserPreMessage(User* user, const MessageTarget& msgtarget, MessageDetails& details)
{
	// Leave remote users and servers alone
	if (!IS_LOCAL(user))
		return MOD_RES_PASSTHRU;

	flags = (details.type == MSG_PRIVMSG) ? FLAG_PRIVMSG : FLAG_NOTICE;

	FilterResult* f = this->FilterMatch(user, details.text, flags);
	if (f)
	{
		std::string target;
		if (msgtarget.type == MessageTarget::TYPE_USER)
		{
			User* t = msgtarget.Get<User>();
			// Check if the target nick is exempted, if yes, ignore this message
			if (exemptednicks.count(t->nick))
				return MOD_RES_PASSTHRU;

			target = t->nick;
		}
		else if (msgtarget.type == MessageTarget::TYPE_CHANNEL)
		{
			Channel* t = msgtarget.Get<Channel>();
			if (exemptedchans.count(t->name))
				return MOD_RES_PASSTHRU;

			target = t->name;
		}
		if (f->action == FA_WARN)
		{
			ServerInstance->SNO->WriteGlobalSno('f', InspIRCd::Format("WARNING: %s's message to %s matched %s (%s)",
				user->nick.c_str(), target.c_str(), f->freeform.c_str(), f->reason.c_str()));
			return MOD_RES_PASSTHRU;
		}
		if (f->action == FA_BLOCK)
		{
			ServerInstance->SNO->WriteGlobalSno('f', InspIRCd::Format("%s had their message to %s filtered as it matched %s (%s)",
				user->nick.c_str(), target.c_str(), f->freeform.c_str(), f->reason.c_str()));
			if (notifyuser)
			{
				if (msgtarget.type == MessageTarget::TYPE_CHANNEL)
					user->WriteNumeric(ERR_CANNOTSENDTOCHAN, target, InspIRCd::Format("Message to channel blocked and opers notified (%s)", f->reason.c_str()));
				else
					user->WriteNotice("Your message to "+target+" was blocked and opers notified: "+f->reason);
			}
			else
				details.echo_original = true;
		}
		else if (f->action == FA_SILENT)
		{
			if (notifyuser)
			{
				if (msgtarget.type == MessageTarget::TYPE_CHANNEL)
					user->WriteNumeric(ERR_CANNOTSENDTOCHAN, target, InspIRCd::Format("Message to channel blocked (%s)", f->reason.c_str()));
				else
					user->WriteNotice("Your message to "+target+" was blocked: "+f->reason);
			}
			else
				details.echo_original = true;
		}
		else if (f->action == FA_KILL)
		{
			ServerInstance->SNO->WriteGlobalSno('f', InspIRCd::Format("%s was killed because their message to %s matched %s (%s)",
				user->nick.c_str(), target.c_str(), f->freeform.c_str(), f->reason.c_str()));
			ServerInstance->Users->QuitUser(user, "Filtered: " + f->reason);
		}
		else if (f->action == FA_SHUN && (ServerInstance->XLines->GetFactory("SHUN")))
		{
			Shun* sh = new Shun(ServerInstance->Time(), f->duration, ServerInstance->Config->ServerName.c_str(), f->reason.c_str(), user->GetIPString());
			ServerInstance->SNO->WriteGlobalSno('f', InspIRCd::Format("%s was shunned because their message to %s matched %s (%s)",
				user->nick.c_str(), target.c_str(), f->freeform.c_str(), f->reason.c_str()));
			if (ServerInstance->XLines->AddLine(sh, NULL))
			{
				ServerInstance->XLines->ApplyLines();
			}
			else
				delete sh;
		}
		else if (f->action == FA_GLINE)
		{
			GLine* gl = new GLine(ServerInstance->Time(), f->duration, ServerInstance->Config->ServerName.c_str(), f->reason.c_str(), "*", user->GetIPString());
			ServerInstance->SNO->WriteGlobalSno('f', InspIRCd::Format("%s was G-lined because their message to %s matched %s (%s)",
				user->nick.c_str(), target.c_str(), f->freeform.c_str(), f->reason.c_str()));
			if (ServerInstance->XLines->AddLine(gl,NULL))
			{
				ServerInstance->XLines->ApplyLines();
			}
			else
				delete gl;
		}
		else if (f->action == FA_ZLINE)
		{
			ZLine* zl = new ZLine(ServerInstance->Time(), f->duration, ServerInstance->Config->ServerName.c_str(), f->reason.c_str(), user->GetIPString());
			ServerInstance->SNO->WriteGlobalSno('f', InspIRCd::Format("%s was Z-lined because their message to %s matched %s (%s)",
				user->nick.c_str(), target.c_str(), f->freeform.c_str(), f->reason.c_str()));
			if (ServerInstance->XLines->AddLine(zl,NULL))
			{
				ServerInstance->XLines->ApplyLines();
			}
			else
				delete zl;
		}

		ServerInstance->Logs->Log(MODNAME, LOG_DEFAULT, user->nick + " had their message filtered, target was " + target + ": " + f->reason + " Action: " + ModuleFilter::FilterActionToString(f->action));
		return MOD_RES_DENY;
	}
	return MOD_RES_PASSTHRU;
}

ModResult ModuleFilter::OnPreCommand(std::string& command, CommandBase::Params& parameters, LocalUser* user, bool validated)
{
	if (validated)
	{
		flags = 0;
		bool parting;

		if (command == "QUIT")
		{
			/* QUIT with no reason: nothing to do */
			if (parameters.size() < 1)
				return MOD_RES_PASSTHRU;

			parting = false;
			flags = FLAG_QUIT;
		}
		else if (command == "PART")
		{
			/* PART with no reason: nothing to do */
			if (parameters.size() < 2)
				return MOD_RES_PASSTHRU;

			if (exemptedchans.count(parameters[0]))
				return MOD_RES_PASSTHRU;

			parting = true;
			flags = FLAG_PART;
		}
		else
			/* We're only messing with PART and QUIT */
			return MOD_RES_PASSTHRU;

		FilterResult* f = this->FilterMatch(user, parameters[parting ? 1 : 0], flags);
		if (!f)
			/* PART or QUIT reason doesnt match a filter */
			return MOD_RES_PASSTHRU;

		/* We cant block a part or quit, so instead we change the reason to 'Reason filtered' */
		parameters[parting ? 1 : 0] = "Reason filtered";

		/* We're warning or blocking, OR theyre quitting and its a KILL action
		 * (we cant kill someone whos already quitting, so filter them anyway)
		 */
		if ((f->action == FA_WARN) || (f->action == FA_BLOCK) || (((!parting) && (f->action == FA_KILL))) || (f->action == FA_SILENT))
		{
			return MOD_RES_PASSTHRU;
		}
		else
		{
			/* Are they parting, if so, kill is applicable */
			if ((parting) && (f->action == FA_KILL))
			{
				user->WriteNotice("*** Your PART message was filtered: " + f->reason);
				ServerInstance->Users->QuitUser(user, "Filtered: " + f->reason);
			}
			if (f->action == FA_GLINE)
			{
				/* Note: We G-line *@IP so that if their host doesn't resolve the G-line still applies. */
				GLine* gl = new GLine(ServerInstance->Time(), f->duration, ServerInstance->Config->ServerName.c_str(), f->reason.c_str(), "*", user->GetIPString());
				ServerInstance->SNO->WriteGlobalSno('f', InspIRCd::Format("%s was G-lined because their %s message matched %s (%s)",
					user->nick.c_str(), command.c_str(), f->freeform.c_str(), f->reason.c_str()));

				if (ServerInstance->XLines->AddLine(gl,NULL))
				{
					ServerInstance->XLines->ApplyLines();
				}
				else
					delete gl;
			}
			if (f->action == FA_ZLINE)
			{
				ZLine* zl = new ZLine(ServerInstance->Time(), f->duration, ServerInstance->Config->ServerName.c_str(), f->reason.c_str(), user->GetIPString());
				ServerInstance->SNO->WriteGlobalSno('f', InspIRCd::Format("%s was Z-lined because their %s message matched %s (%s)",
					user->nick.c_str(), command.c_str(), f->freeform.c_str(), f->reason.c_str()));

				if (ServerInstance->XLines->AddLine(zl,NULL))
				{
					ServerInstance->XLines->ApplyLines();
				}
				else
					delete zl;
			}
			else if (f->action == FA_SHUN && (ServerInstance->XLines->GetFactory("SHUN")))
			{
				/* Note: We shun *!*@IP so that if their host doesnt resolve the shun still applies. */
				Shun* sh = new Shun(ServerInstance->Time(), f->duration, ServerInstance->Config->ServerName.c_str(), f->reason.c_str(), user->GetIPString());
				ServerInstance->SNO->WriteGlobalSno('f', InspIRCd::Format("%s was shunned because their %s message matched %s (%s)",
					user->nick.c_str(), command.c_str(), f->freeform.c_str(), f->reason.c_str()));
				if (ServerInstance->XLines->AddLine(sh, NULL))
				{
					ServerInstance->XLines->ApplyLines();
				}
				else
					delete sh;
			}
			return MOD_RES_DENY;
		}
	}
	return MOD_RES_PASSTHRU;
}

void ModuleFilter::ReadConfig(ConfigStatus& status)
{
	ConfigTagList tags = ServerInstance->Config->ConfTags("exemptfromfilter");
	exemptedchans.clear();
	exemptednicks.clear();

	for (ConfigIter i = tags.first; i != tags.second; ++i)
	{
		ConfigTag* tag = i->second;

		const std::string target = tag->getString("target");
		if (!target.empty())
		{
			if (target[0] == '#')
				exemptedchans.insert(target);
			else
				exemptednicks.insert(target);
		}
	}

	ConfigTag* tag = ServerInstance->Config->ConfValue("filteropts");
	std::string newrxengine = tag->getString("engine");
	notifyuser = tag->getBool("notifyuser", true);

	factory = RegexEngine ? (RegexEngine.operator->()) : NULL;

	if (newrxengine.empty())
		RegexEngine.SetProvider("regex");
	else
		RegexEngine.SetProvider("regex/" + newrxengine);

	if (!RegexEngine)
	{
		if (newrxengine.empty())
			ServerInstance->SNO->WriteGlobalSno('f', "WARNING: No regex engine loaded - Filter functionality disabled until this is corrected.");
		else
			ServerInstance->SNO->WriteGlobalSno('f', "WARNING: Regex engine '%s' is not loaded - Filter functionality disabled until this is corrected.", newrxengine.c_str());

		initing = false;
		FreeFilters();
		return;
	}

	if ((!initing) && (RegexEngine.operator->() != factory))
	{
		ServerInstance->SNO->WriteGlobalSno('f', "Dumping all filters due to regex engine change");
		FreeFilters();
	}

	initing = false;
	ReadFilters();
}

Version ModuleFilter::GetVersion()
{
	return Version("Text (spam) filtering", VF_VENDOR | VF_COMMON, RegexEngine ? RegexEngine->name : "");
}

std::string ModuleFilter::EncodeFilter(FilterResult* filter)
{
	std::ostringstream stream;
	std::string x = filter->freeform;

	/* Hax to allow spaces in the freeform without changing the design of the irc protocol */
	for (std::string::iterator n = x.begin(); n != x.end(); n++)
		if (*n == ' ')
			*n = '\7';

	stream << x << " " << FilterActionToString(filter->action) << " " << filter->GetFlags() << " " << filter->duration << " :" << filter->reason;
	return stream.str();
}

FilterResult ModuleFilter::DecodeFilter(const std::string &data)
{
	std::string filteraction;
	FilterResult res;
	irc::tokenstream tokens(data);
	tokens.GetMiddle(res.freeform);
	tokens.GetMiddle(filteraction);
	if (!StringToFilterAction(filteraction, res.action))
		throw ModuleException("Invalid action: " + filteraction);

	std::string filterflags;
	tokens.GetMiddle(filterflags);
	char c = res.FillFlags(filterflags);
	if (c != 0)
		throw ModuleException("Invalid flag: '" + std::string(1, c) + "'");

	std::string duration;
	tokens.GetMiddle(duration);
	res.duration = ConvToNum<unsigned long>(duration);

	tokens.GetTrailing(res.reason);

	/* Hax to allow spaces in the freeform without changing the design of the irc protocol */
	for (std::string::iterator n = res.freeform.begin(); n != res.freeform.end(); n++)
		if (*n == '\7')
			*n = ' ';

	return res;
}

void ModuleFilter::OnSyncNetwork(ProtocolInterface::Server& server)
{
	for (std::vector<FilterResult>::iterator i = filters.begin(); i != filters.end(); ++i)
	{
		FilterResult& filter = *i;
		if (filter.from_config)
			continue;

		server.SendMetaData("filter", EncodeFilter(&filter));
	}
}

void ModuleFilter::OnDecodeMetaData(Extensible* target, const std::string &extname, const std::string &extdata)
{
	if ((target == NULL) && (extname == "filter"))
	{
		try
		{
			FilterResult data = DecodeFilter(extdata);
			this->AddFilter(data.freeform, data.action, data.reason, data.duration, data.GetFlags());
		}
		catch (ModuleException& e)
		{
			ServerInstance->Logs->Log(MODNAME, LOG_DEBUG, "Error when unserializing filter: " + e.GetReason());
		}
	}
}

FilterResult* ModuleFilter::FilterMatch(User* user, const std::string &text, int flgs)
{
	static std::string stripped_text;
	stripped_text.clear();

	for (std::vector<FilterResult>::iterator i = filters.begin(); i != filters.end(); ++i)
	{
		FilterResult* filter = &*i;

		/* Skip ones that dont apply to us */
		if (!AppliesToMe(user, filter, flgs))
			continue;

		if ((filter->flag_strip_color) && (stripped_text.empty()))
		{
			stripped_text = text;
			InspIRCd::StripColor(stripped_text);
		}

		if (filter->regex->Matches(filter->flag_strip_color ? stripped_text : text))
			return filter;
	}
	return NULL;
}

bool ModuleFilter::DeleteFilter(const std::string &freeform)
{
	for (std::vector<FilterResult>::iterator i = filters.begin(); i != filters.end(); i++)
	{
		if (i->freeform == freeform)
		{
			delete i->regex;
			filters.erase(i);
			return true;
		}
	}
	return false;
}

std::pair<bool, std::string> ModuleFilter::AddFilter(const std::string& freeform, FilterAction type, const std::string& reason, unsigned long duration, const std::string& flgs)
{
	for (std::vector<FilterResult>::iterator i = filters.begin(); i != filters.end(); i++)
	{
		if (i->freeform == freeform)
		{
			return std::make_pair(false, "Filter already exists");
		}
	}

	try
	{
		filters.push_back(FilterResult(RegexEngine, freeform, reason, type, duration, flgs, false));
	}
	catch (ModuleException &e)
	{
		ServerInstance->Logs->Log(MODNAME, LOG_DEFAULT, "Error in regular expression '%s': %s", freeform.c_str(), e.GetReason().c_str());
		return std::make_pair(false, e.GetReason());
	}
	return std::make_pair(true, "");
}

bool ModuleFilter::StringToFilterAction(const std::string& str, FilterAction& fa)
{
	if (stdalgo::string::equalsci(str, "gline"))
		fa = FA_GLINE;
	else if (stdalgo::string::equalsci(str, "zline"))
		fa = FA_ZLINE;
	else if (stdalgo::string::equalsci(str, "warn"))
		fa = FA_WARN;
	else if (stdalgo::string::equalsci(str, "block"))
		fa = FA_BLOCK;
	else if (stdalgo::string::equalsci(str, "silent"))
		fa = FA_SILENT;
	else if (stdalgo::string::equalsci(str, "kill"))
		fa = FA_KILL;
	else if (stdalgo::string::equalsci(str, "shun") && (ServerInstance->XLines->GetFactory("SHUN")))
		fa = FA_SHUN;
	else if (stdalgo::string::equalsci(str, "none"))
		fa = FA_NONE;
	else
		return false;

	return true;
}

std::string ModuleFilter::FilterActionToString(FilterAction fa)
{
	switch (fa)
	{
		case FA_GLINE:  return "gline";
		case FA_ZLINE:  return "zline";
		case FA_WARN:   return "warn";
		case FA_BLOCK:  return "block";
		case FA_SILENT: return "silent";
		case FA_KILL:   return "kill";
		case FA_SHUN:   return "shun";
		default:		return "none";
	}
}

void ModuleFilter::ReadFilters()
{
	for (std::vector<FilterResult>::iterator filter = filters.begin(); filter != filters.end(); )
	{
		if (filter->from_config)
		{
			ServerInstance->SNO->WriteGlobalSno('f', "FILTER: removing filter '" + filter->freeform + "' due to config rehash.");
			delete filter->regex;
			filter = filters.erase(filter);
			continue;
		}

		// The filter is not from the config.
		filter++;
	}

	ConfigTagList tags = ServerInstance->Config->ConfTags("keyword");
	for (ConfigIter i = tags.first; i != tags.second; ++i)
	{
		std::string pattern = i->second->getString("pattern");
		std::string reason = i->second->getString("reason");
		std::string action = i->second->getString("action");
		std::string flgs = i->second->getString("flags");
		unsigned long duration = i->second->getDuration("duration", 10*60, 1);
		if (flgs.empty())
			flgs = "*";

		FilterAction fa;
		if (!StringToFilterAction(action, fa))
			fa = FA_NONE;

		try
		{
			filters.push_back(FilterResult(RegexEngine, pattern, reason, fa, duration, flgs, true));
			ServerInstance->Logs->Log(MODNAME, LOG_DEFAULT, "Regular expression %s loaded.", pattern.c_str());
		}
		catch (ModuleException &e)
		{
			ServerInstance->Logs->Log(MODNAME, LOG_DEFAULT, "Error in regular expression '%s': %s", pattern.c_str(), e.GetReason().c_str());
		}
	}
}

ModResult ModuleFilter::OnStats(Stats::Context& stats)
{
	if (stats.GetSymbol() == 's')
	{
		for (std::vector<FilterResult>::iterator i = filters.begin(); i != filters.end(); i++)
		{
			stats.AddRow(223, RegexEngine.GetProvider()+":"+i->freeform+" "+i->GetFlags()+" "+FilterActionToString(i->action)+" "+ConvToStr(i->duration)+" :"+i->reason);
		}
		for (ExemptTargetSet::const_iterator i = exemptedchans.begin(); i != exemptedchans.end(); ++i)
		{
			stats.AddRow(223, "EXEMPT "+(*i));
		}
		for (ExemptTargetSet::const_iterator i = exemptednicks.begin(); i != exemptednicks.end(); ++i)
		{
			stats.AddRow(223, "EXEMPT "+(*i));
		}
	}
	return MOD_RES_PASSTHRU;
}

void ModuleFilter::OnUnloadModule(Module* mod)
{
	// If the regex engine became unavailable or has changed, remove all filters
	if (!RegexEngine)
	{
		FreeFilters();
	}
	else if (RegexEngine.operator->() != factory)
	{
		factory = RegexEngine.operator->();
		FreeFilters();
	}
}

MODULE_INIT(ModuleFilter)
