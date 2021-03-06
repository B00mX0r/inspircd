/*
 * InspIRCd -- Internet Relay Chat Daemon
 *
 *   Copyright (C) 2007-2008 Robin Burchell <robin+git@viroteck.net>
 *   Copyright (C) 2007 Dennis Friis <peavey@inspircd.org>
 *   Copyright (C) 2004, 2006 Craig Edwards <craigedwards@brainbox.cc>
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
#include "modules/exemption.h"

class ModuleNoCTCP : public Module
{
	CheckExemption::EventProvider exemptionprov;
	SimpleChannelModeHandler nc;
	SimpleUserModeHandler ncu;

 public:
	ModuleNoCTCP()
		: exemptionprov(this)
		, nc(this, "noctcp", 'C')
		, ncu(this, "u_noctcp", 'T')
	{
	}

	Version GetVersion() override
	{
		return Version("Provides user mode +T and channel mode +C to block CTCPs", VF_VENDOR);
	}

	ModResult OnUserPreMessage(User* user, const MessageTarget& target, MessageDetails& details) override
	{
		if (!IS_LOCAL(user))
			return MOD_RES_PASSTHRU;

		std::string ctcpname;
		if (!details.IsCTCP(ctcpname) || irc::equals(ctcpname, "ACTION"))
			return MOD_RES_PASSTHRU;

		if (target.type == MessageTarget::TYPE_CHANNEL)
		{
			Channel* c = target.Get<Channel>();
			ModResult res = CheckExemption::Call(exemptionprov, user, c, "noctcp");
			if (res == MOD_RES_ALLOW)
				return MOD_RES_PASSTHRU;

			if (!c->GetExtBanStatus(user, 'C').check(!c->IsModeSet(nc)))
			{
				user->WriteNumeric(ERR_CANNOTSENDTOCHAN, c->name, "Can't send CTCP to channel (+C set)");
				return MOD_RES_DENY;
			}
		}
		else if (target.type == MessageTarget::TYPE_USER)
		{
			User* u = target.Get<User>();
			if (u->IsModeSet(ncu))
			{
				user->WriteNumeric(ERR_CANTSENDTOUSER, u->nick, "Can't send CTCP to user (+T set)");
				return MOD_RES_PASSTHRU;
			}
		}
		return MOD_RES_PASSTHRU;
	}

	void On005Numeric(std::map<std::string, std::string>& tokens) override
	{
		tokens["EXTBAN"].push_back('C');
	}
};

MODULE_INIT(ModuleNoCTCP)
