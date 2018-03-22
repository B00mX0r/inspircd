/*
 * InspIRCd -- Internet Relay Chat Daemon
 *
 *   Copyright (C) 2009 Daniel De Graaf <danieldg@inspircd.org>
 *   Copyright (C) 2008 Thomas Stagner <aquanight@inspircd.org>
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

enum
{
	// From RFC 1459.
	RPL_WHOISUSER = 311,
	RPL_WHOISOPERATOR = 313,
	RPL_WHOISIDLE = 317,
	RPL_WHOISCHANNELS = 319,

	// From UnrealIRCd.
	RPL_WHOISHOST = 378,
	RPL_WHOISMODES = 379,

	// InspIRCd-specific.
	RPL_CHANNELSMSG = 651
};

class WhoisContextImpl : public Whois::Context
{
	Events::ModuleEventProvider& lineevprov;

 public:
	WhoisContextImpl(LocalUser* src, User* targ, Events::ModuleEventProvider& evprov)
		: Whois::Context(src, targ)
		, lineevprov(evprov)
	{
	}

	using Whois::Context::SendLine;
	void SendLine(Numeric::Numeric& numeric) CXX11_OVERRIDE;
};

void WhoisContextImpl::SendLine(Numeric::Numeric& numeric)
{
	ModResult MOD_RESULT;
	FIRST_MOD_RESULT_CUSTOM(lineevprov, Whois::LineEventListener, OnWhoisLine, MOD_RESULT, (*this, numeric));

	if (MOD_RESULT != MOD_RES_DENY)
		source->WriteNumeric(numeric);
}

/** Handle /WHOIS.
 */
class CommandWhois : public SplitCommand
{
	ChanModeReference secretmode;
	ChanModeReference privatemode;
	UserModeReference snomaskmode;
	Events::ModuleEventProvider evprov;
	Events::ModuleEventProvider lineevprov;

	void DoWhois(LocalUser* user, User* dest, time_t signon, unsigned long idle);
	void SendChanList(WhoisContextImpl& whois);

 public:
	/** Constructor for whois.
	 */
	CommandWhois(Module* parent)
		: SplitCommand(parent, "WHOIS", 1)
		, secretmode(parent, "secret")
		, privatemode(parent, "private")
		, snomaskmode(parent, "snomask")
		, evprov(parent, "event/whois")
		, lineevprov(parent, "event/whoisline")
	{
		Penalty = 2;
		syntax = "<nick>{,<nick>}";
	}

	/** Handle command.
	 * @param parameters The parameters to the command
	 * @param user The user issuing the command
	 * @return A value from CmdResult to indicate command success or failure.
	 */
	CmdResult HandleLocal(const std::vector<std::string>& parameters, LocalUser* user) CXX11_OVERRIDE;
	CmdResult HandleRemote(const std::vector<std::string>& parameters, RemoteUser* target) CXX11_OVERRIDE;
};

class WhoisNumericSink
{
	WhoisContextImpl& whois;
 public:
	WhoisNumericSink(WhoisContextImpl& whoisref)
		: whois(whoisref)
	{
	}

	void operator()(Numeric::Numeric& numeric) const
	{
		whois.SendLine(numeric);
	}
};

class WhoisChanListNumericBuilder : public Numeric::GenericBuilder<' ', false, WhoisNumericSink>
{
 public:
	WhoisChanListNumericBuilder(WhoisContextImpl& whois)
		: Numeric::GenericBuilder<' ', false, WhoisNumericSink>(WhoisNumericSink(whois), RPL_WHOISCHANNELS, false, whois.GetSource()->nick.size() + whois.GetTarget()->nick.size() + 1)
	{
		GetNumeric().push(whois.GetTarget()->nick).push(std::string());
	}
};

class WhoisChanList
{
	WhoisChanListNumericBuilder num;
	WhoisChanListNumericBuilder spynum;
	std::string prefixstr;

	void AddMember(Membership* memb, WhoisChanListNumericBuilder& out)
	{
		prefixstr.clear();
		const char prefix = memb->GetPrefixChar();
		if (prefix)
			prefixstr.push_back(prefix);
		out.Add(prefixstr, memb->chan->name);
	}

 public:
	const ServerConfig::OperSpyWhoisState spywhois;
	WhoisChanList(WhoisContextImpl& whois)
		: num(whois)
		, spynum(whois)
		, spywhois((whois.GetSource()->HasPrivPermission("users/auspex") && ServerInstance->Config->OperSpyWhois) ? ServerConfig::SPYWHOIS_SPLITMSG : ServerConfig::SPYWHOIS_NONE)
	{
	}

	void AddVisible(Membership* memb)
	{
		AddMember(memb, num);
	}

	void AddHidden(Membership* memb)
	{
		AddMember(memb, spynum);
	}

	void Flush(WhoisContextImpl& whois)
	{
		num.Flush();
		if (!spynum.IsEmpty())
			whois.SendLine(RPL_CHANNELSMSG, "is on private/secret channels:");
		spynum.Flush();
	}
};

void CommandWhois::SendChanList(WhoisContextImpl& whois)
{
	WhoisChanList chanlist(whois);

	User* const target = whois.GetTarget();
	for (User::ChanList::iterator i = target->chans.begin(); i != target->chans.end(); ++i)
	{
		Membership* memb = *i;
		Channel* c = memb->chan;
		/* If neither +p nor +s is set, or the channel contains the user
		 * and the user is not running whois on themselves, it is not
		 # a spy channel
		 */
		if (!c->IsModeSet(privatemode) && !c->IsModeSet(secretmode))
			chanlist.AddVisible(memb);
		else if (c->HasUser(whois.GetSource()) || chanlist.spywhois == ServerConfig::SPYWHOIS_SPLITMSG)
			chanlist.AddHidden(memb);
	}

	chanlist.Flush(whois);
}

void CommandWhois::DoWhois(LocalUser* user, User* dest, time_t signon, unsigned long idle)
{
	WhoisContextImpl whois(user, dest, lineevprov);

	whois.SendLine(RPL_WHOISUSER, dest->ident, dest->GetDisplayedHost(), '*', dest->fullname);
	if (whois.IsSelfWhois() || user->HasPrivPermission("users/auspex"))
	{
		whois.SendLine(RPL_WHOISHOST, InspIRCd::Format("is connecting from %s@%s %s", dest->ident.c_str(), dest->GetRealHost().c_str(), dest->GetIPString().c_str()));
	}

	SendChanList(whois);

	if (!whois.IsSelfWhois() && !ServerInstance->Config->HideServer.empty() && !user->HasPrivPermission("servers/auspex"))
	{
		whois.SendLine(RPL_WHOISSERVER, ServerInstance->Config->HideServer, ServerInstance->Config->Network);
	}
	else
	{
		whois.SendLine(RPL_WHOISSERVER, dest->server->GetName(), dest->server->GetDesc());
	}

	if (dest->IsAway())
	{
		whois.SendLine(RPL_AWAY, dest->awaymsg);
	}

	if (dest->IsOper())
	{
		if (ServerInstance->Config->GenericOper)
			whois.SendLine(RPL_WHOISOPERATOR, "is an IRC operator");
		else
			whois.SendLine(RPL_WHOISOPERATOR, InspIRCd::Format("is %s %s on %s", (strchr("AEIOUaeiou",dest->oper->name[0]) ? "an" : "a"), dest->oper->name.c_str(), ServerInstance->Config->Network.c_str()));
	}

	if (whois.IsSelfWhois() || user->HasPrivPermission("users/auspex"))
	{
		if (dest->IsModeSet(snomaskmode))
		{
			whois.SendLine(RPL_WHOISMODES, InspIRCd::Format("is using modes %s %s", dest->GetModeLetters().c_str(), snomaskmode->GetUserParameter(dest).c_str()));
		}
		else
		{
			whois.SendLine(RPL_WHOISMODES, InspIRCd::Format("is using modes %s", dest->GetModeLetters().c_str()));
		}
	}

	FOREACH_MOD_CUSTOM(evprov, Whois::EventListener, OnWhois, (whois));

	/*
	 * We only send these if we've been provided them. That is, if hideserver is turned off, and user is local, or
	 * if remote whois is queried, too. This is to keep the user hidden, and also since you can't reliably tell remote time. -- w00t
	 */
	if ((idle) || (signon))
	{
		whois.SendLine(RPL_WHOISIDLE, idle, signon, "seconds idle, signon time");
	}

	whois.SendLine(RPL_ENDOFWHOIS, "End of /WHOIS list.");
}

CmdResult CommandWhois::HandleRemote(const std::vector<std::string>& parameters, RemoteUser* target)
{
	if (parameters.size() < 2)
		return CMD_FAILURE;

	User* user = ServerInstance->FindUUID(parameters[0]);
	if (!user)
		return CMD_FAILURE;

	// User doing the whois must be on this server
	LocalUser* localuser = IS_LOCAL(user);
	if (!localuser)
		return CMD_FAILURE;

	unsigned long idle = ConvToNum<unsigned long>(parameters.back());
	DoWhois(localuser, target, target->signon, idle);

	return CMD_SUCCESS;
}

CmdResult CommandWhois::HandleLocal(const std::vector<std::string>& parameters, LocalUser* user)
{
	User *dest;
	unsigned int userindex = 0;
	unsigned long idle = 0;
	time_t signon = 0;

	if (CommandParser::LoopCall(user, this, parameters, 0))
		return CMD_SUCCESS;

	/*
	 * If 2 paramters are specified (/whois nick nick), ignore the first one like spanningtree
	 * does, and use the second one, otherwise, use the only paramter. -- djGrrr
	 */
	if (parameters.size() > 1)
		userindex = 1;

	dest = ServerInstance->FindNickOnly(parameters[userindex]);

	if ((dest) && (dest->registered == REG_ALL))
	{
		/*
		 * Okay. Umpteenth attempt at doing this, so let's re-comment...
		 * For local users (/w localuser), we show idletime if hideserver is disabled
		 * For local users (/w localuser localuser), we always show idletime, hence parameters.size() > 1 check.
		 * For remote users (/w remoteuser), we do NOT show idletime
		 * For remote users (/w remoteuser remoteuser), spanningtree will handle calling do_whois, so we can ignore this case.
		 * Thanks to djGrrr for not being impatient while I have a crap day coding. :p -- w00t
		 */
		LocalUser* localuser = IS_LOCAL(dest);
		if (localuser && (ServerInstance->Config->HideServer.empty() || parameters.size() > 1))
		{
			idle = labs((long)((localuser->idle_lastmsg)-ServerInstance->Time()));
			signon = dest->signon;
		}

		DoWhois(user,dest,signon,idle);
	}
	else
	{
		/* no such nick/channel */
		user->WriteNumeric(Numerics::NoSuchNick(!parameters[userindex].empty() ? parameters[userindex] : "*"));
		user->WriteNumeric(RPL_ENDOFWHOIS, (!parameters[userindex].empty() ? parameters[userindex] : "*"), "End of /WHOIS list.");
		return CMD_FAILURE;
	}

	return CMD_SUCCESS;
}

COMMAND_INIT(CommandWhois)
