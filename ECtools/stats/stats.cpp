/*
 * Copyright 2005 - 2016 Zarafa and its licensors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <kopano/platform.h>
#include "config.h"

#include <iostream>
#include <string>
#include <mapi.h>
#include <mapiutil.h>
#include <edkmdb.h>

#ifdef HAVE_CURSES_H
#include <curses.h>
#endif

#include <map>
#include <set>
#include <getopt.h>

#include <kopano/CommonUtil.h>
#include <kopano/stringutil.h>
#include <kopano/ECTags.h>
#include <kopano/automapi.hpp>
#include <kopano/ecversion.h>
#include <kopano/memory.hpp>
#include <kopano/charset/convert.h>
#include "MAPIConsoleTable.h"
#include <kopano/ECLogger.h>

using namespace KCHL;
using std::cerr;
using std::cout;
using std::endl;

enum eTableType { INVALID_STATS = -1, SYSTEM_STATS, SESSION_STATS, USER_STATS, COMPANY_STATS, SERVER_STATS, SESSION_TOP, OPTION_HOST, OPTION_USER, OPTION_DUMP };

static const struct option long_options[] = {
		{ "system", 0, NULL, SYSTEM_STATS },
		{ "sessions", 0, NULL, SESSION_STATS },
		{ "users", 0, NULL, USER_STATS },
		{ "company", 0, NULL, COMPANY_STATS },
		{ "servers", 0, NULL, SERVER_STATS },
		{ "top", 0, NULL, SESSION_TOP },
		{ "host", 1, NULL, OPTION_HOST },
		{ "user", 1, NULL, OPTION_USER },
		{ "dump", 0, NULL, OPTION_DUMP },
		{ NULL, 0, NULL, 0 }
};

// sort on something invalid to get the order in which the server added the rows
static constexpr const SizedSSortOrderSet(2, tableSortSystem) =
{ 1, 0, 0,
  {
	  { PR_NULL, TABLE_SORT_DESCEND }
  }
}
;
static constexpr const SizedSSortOrderSet(2, tableSortSession) =
{ 2, 0, 0,
  {
	  { PR_EC_STATS_SESSION_IPADDRESS, TABLE_SORT_ASCEND },
	  { PR_EC_STATS_SESSION_IDLETIME, TABLE_SORT_DESCEND }
  }
};

static constexpr const SizedSSortOrderSet(2, tableSortUser) =
{ 2, 0, 0,
  {
	  { PR_EC_COMPANY_NAME, TABLE_SORT_ASCEND },
	  { PR_EC_USERNAME_A, TABLE_SORT_ASCEND },
  }
};

static constexpr const SizedSSortOrderSet(2, tableSortCompany) =
{ 1, 0, 0,
  {
	  { PR_EC_COMPANY_NAME, TABLE_SORT_ASCEND }
  }
};

static constexpr const SizedSSortOrderSet(2, tableSortServers) =
{ 1, 0, 0,
  {
	  { PR_EC_STATS_SERVER_NAME, TABLE_SORT_ASCEND }
  }
};

static const SSortOrderSet *const sortorders[] = {
	tableSortSystem, tableSortSession,
	tableSortUser, tableSortCompany, tableSortServers
};
static const ULONG ulTableProps[] = {
	PR_EC_STATSTABLE_SYSTEM, PR_EC_STATSTABLE_SESSIONS,
	PR_EC_STATSTABLE_USERS, PR_EC_STATSTABLE_COMPANY, PR_EC_STATSTABLE_SERVERS
};

struct TIMES {
    double dblUser;
    double dblSystem;
    double dblReal;
    unsigned int ulRequests;
};

struct SESSION {
    unsigned long long ullSessionId;
    unsigned long long ullSessionGroupId;
    TIMES times;
    TIMES dtimes;

    unsigned int ulIdle;
    int ulPeerPid;
    bool bLocked;
    
    std::string strUser;
    std::string strIP;
    std::string strBusy;
    std::string strState;
    std::string strPeer;
    std::string strClientVersion;
    std::string strClientApp;
    std::string strClientAppVersion, strClientAppMisc;
    
    bool operator <(const SESSION &b) const
    {
        return this->dtimes.dblReal > b.dtimes.dblReal;
    }
};

static bool sort_sessionid(const SESSION &a, const SESSION &b) { return a.ullSessionId < b.ullSessionId; } // && group < ?
static bool sort_user(const SESSION &a, const SESSION &b) { return a.strUser < b.strUser; }
static bool sort_ippeer(const SESSION &a, const SESSION &b) { return a.strIP < b.strIP || (a.strIP == b.strIP && a.strPeer < b.strPeer); }
static bool sort_version(const SESSION &a, const SESSION &b) { return a.strClientVersion < b.strClientVersion; }
static bool sort_app(const SESSION &a, const SESSION &b) { return a.strClientApp < b.strClientApp; }

typedef bool(*SortFuncPtr)(const SESSION&, const SESSION&);
static SortFuncPtr sortfunc;

static std::string GetString(LPSPropValue lpProps, ULONG cValues,
    ULONG ulPropTag)
{
	auto lpProp = PCpropFindProp(lpProps, cValues, ulPropTag);
    if(lpProp == NULL)
        return "";
        
    switch(PROP_TYPE(ulPropTag)) {
        case PT_STRING8: return lpProp->Value.lpszA;
        case PT_MV_STRING8: {
            std::string s;
            for (ULONG i = 0; i < lpProp->Value.MVszA.cValues; ++i) {
                s+= lpProp->Value.MVszA.lppszA[i];
                s+= " ";
            }
            return s;
        }
        case PT_LONG: {
            char s[80];
            snprintf(s, sizeof(s), "%d", lpProp->Value.ul);
            return s;
        }
    }
    
    return "";
}

static unsigned long long GetLongLong(LPSPropValue lpProps, ULONG cValues,
    ULONG ulPropTag)
{
	auto lpProp = PCpropFindProp(lpProps, cValues, ulPropTag);
    if(lpProp == NULL)
        return -1;
        
    switch(PROP_TYPE(ulPropTag)) {
        case PT_LONG: return lpProp->Value.ul;
        case PT_LONGLONG: return lpProp->Value.li.QuadPart;
    }

    return 0;
}

static double GetDouble(LPSPropValue lpProps, ULONG cValues, ULONG ulPropTag)
{
	auto lpProp = PCpropFindProp(lpProps, cValues, ulPropTag);
    if(lpProp == NULL)
        return 0;
        
    switch(PROP_TYPE(ulPropTag)) {
        case PT_DOUBLE: return lpProp->Value.dbl;
    }
    
    return 0;
}

static void showtop(LPMDB lpStore)
{
#ifdef HAVE_CURSES_H
    HRESULT hr = hrSuccess;
	object_ptr<IMAPITable> lpTable;
    WINDOW *win = NULL;
    std::map<unsigned long long, TIMES> mapLastTimes;
    std::map<std::string, std::string> mapStats;
    std::map<std::string, double> mapDiffStats;
    std::list<SESSION> lstSessions;
    std::set<std::string> setUsers;
    std::set<std::string> setHosts;
    std::map<unsigned long long, unsigned int> mapSessionGroups;
	char date[64];
	time_t now;
    unsigned int ulSessGrp = 1;
    double dblUser, dblSystem;
    int wx,wy;
    int key;
    double dblLast = 0, dblTime = 0;

	// columns in sizes, not literal offsets
	static const unsigned int cols[] = {0, 4, 21, 8, 25, 16, 20, 8, 8, 7, 7, 5};
	unsigned int ofs = 0;
	bool bColumns[] = {false,false,true,true,true,true,true,true,true,true,true,true}; // key 1 through err?
	static constexpr const SortFuncPtr fSort[] = {nullptr, sort_sessionid, sort_version, sort_user, sort_ippeer, sort_app, nullptr}; // key a through g
	bool bReverse = false;
	static constexpr const SizedSPropTagArray(2, sptaSystem) =
		{2, {PR_DISPLAY_NAME_A, PR_EC_STATS_SYSTEM_VALUE}};

    // Init ncurses
    win = initscr(); 
    if(win == NULL) {
        cerr << "ncurses error\n";
        return;
    }

    cbreak(); 
    noecho();
    nonl();
    nodelay(win, TRUE);

    getmaxyx(win, wy, wx);

    while(1) {
        int line = 0;
		werase(win);
		hr = lpStore->OpenProperty(PR_EC_STATSTABLE_SYSTEM, &IID_IMAPITable, 0, 0, &~lpTable);
		if(hr != hrSuccess)
		    goto exit;
		hr = lpTable->SetColumns(sptaSystem, 0);
		if(hr != hrSuccess)
			goto exit;

		rowset_ptr lpsRowSet;
		hr = lpTable->QueryRows(-1, 0, &~lpsRowSet);
        if(hr != hrSuccess)
            goto exit;
            
        dblTime = GetTimeOfDay() - dblLast;
        dblLast = GetTimeOfDay();
            
        for (ULONG i = 0; i < lpsRowSet->cRows; ++i) {
		auto lpName = PCpropFindProp(lpsRowSet->aRow[i].lpProps, lpsRowSet->aRow[i].cValues, PR_DISPLAY_NAME_A);
		auto lpValue = PCpropFindProp(lpsRowSet->aRow[i].lpProps, lpsRowSet->aRow[i].cValues, PR_EC_STATS_SYSTEM_VALUE);
            if(lpName && lpValue) {
                mapDiffStats[lpName->Value.lpszA] = atof(lpValue->Value.lpszA) - atof(mapStats[lpName->Value.lpszA].c_str());
                mapStats[lpName->Value.lpszA] = lpValue->Value.lpszA;
            }
        }
        
        hr = lpStore->OpenProperty(PR_EC_STATSTABLE_SESSIONS, &IID_IMAPITable, 0, 0, &~lpTable);
        if(hr != hrSuccess)
            goto exit;
        hr = lpTable->QueryRows(-1, 0, &~lpsRowSet);
        if(hr != hrSuccess)
            break;
            
        lstSessions.clear();
        mapSessionGroups.clear();
        setUsers.clear();
        dblUser = dblSystem = 0;
        ulSessGrp = 1;
        
        for (ULONG i = 0; i < lpsRowSet->cRows; ++i) {
            SESSION session;

            session.strUser = GetString(lpsRowSet->aRow[i].lpProps, lpsRowSet->aRow[i].cValues, PR_EC_USERNAME_A);
            session.strIP = GetString(lpsRowSet->aRow[i].lpProps, lpsRowSet->aRow[i].cValues, PR_EC_STATS_SESSION_IPADDRESS);
            session.strBusy = GetString(lpsRowSet->aRow[i].lpProps, lpsRowSet->aRow[i].cValues, PR_EC_STATS_SESSION_BUSYSTATES);
            session.strState = GetString(lpsRowSet->aRow[i].lpProps, lpsRowSet->aRow[i].cValues, PR_EC_STATS_SESSION_PROCSTATES);
            session.strClientVersion = GetString(lpsRowSet->aRow[i].lpProps, lpsRowSet->aRow[i].cValues, PR_EC_STATS_SESSION_CLIENT_VERSION);
            session.strClientApp = GetString(lpsRowSet->aRow[i].lpProps, lpsRowSet->aRow[i].cValues, PR_EC_STATS_SESSION_CLIENT_APPLICATION);
            session.strClientAppVersion = GetString(lpsRowSet->aRow[i].lpProps, lpsRowSet->aRow[i].cValues, PR_EC_STATS_SESSION_CLIENT_APPLICATION_VERSION);
            session.strClientAppMisc = GetString(lpsRowSet->aRow[i].lpProps, lpsRowSet->aRow[i].cValues, PR_EC_STATS_SESSION_CLIENT_APPLICATION_MISC);

            session.ulPeerPid = GetLongLong(lpsRowSet->aRow[i].lpProps, lpsRowSet->aRow[i].cValues, PR_EC_STATS_SESSION_PEER_PID);
            session.times.ulRequests = GetLongLong(lpsRowSet->aRow[i].lpProps, lpsRowSet->aRow[i].cValues, PR_EC_STATS_SESSION_REQUESTS);
            session.ullSessionId = GetLongLong(lpsRowSet->aRow[i].lpProps, lpsRowSet->aRow[i].cValues, PR_EC_STATS_SESSION_ID);
            session.ullSessionGroupId = GetLongLong(lpsRowSet->aRow[i].lpProps, lpsRowSet->aRow[i].cValues, PR_EC_STATS_SESSION_GROUP_ID);
			if (session.ulPeerPid != 0)
				session.strPeer = stringify(session.ulPeerPid);

            session.times.dblUser = GetDouble(lpsRowSet->aRow[i].lpProps, lpsRowSet->aRow[i].cValues, PR_EC_STATS_SESSION_CPU_USER);
            session.times.dblSystem = GetDouble(lpsRowSet->aRow[i].lpProps, lpsRowSet->aRow[i].cValues, PR_EC_STATS_SESSION_CPU_SYSTEM);
            session.times.dblReal = GetDouble(lpsRowSet->aRow[i].lpProps, lpsRowSet->aRow[i].cValues, PR_EC_STATS_SESSION_CPU_REAL);

            auto iterTimes = mapLastTimes.find(session.ullSessionId);
            if (iterTimes != mapLastTimes.cend()) {
                session.dtimes.dblUser = (session.times.dblUser - iterTimes->second.dblUser) / dblTime;
                session.dtimes.dblSystem = (session.times.dblSystem - iterTimes->second.dblSystem) / dblTime;
                session.dtimes.dblReal = (session.times.dblReal - iterTimes->second.dblReal) / dblTime;
                
                dblUser += session.dtimes.dblUser;
                dblSystem += session.dtimes.dblSystem;
                
                session.dtimes.ulRequests = session.times.ulRequests - iterTimes->second.ulRequests;
            } else {
                session.dtimes.dblUser = session.dtimes.dblSystem = session.dtimes.dblReal = 0;
            }
            mapLastTimes[session.ullSessionId] = session.times;
			lstSessions.emplace_back(session);
			setUsers.emplace(session.strUser);
			setHosts.emplace(session.strIP);
            if(session.ullSessionGroupId != 0) {
                auto iterSessionGroups = mapSessionGroups.find(session.ullSessionGroupId);
                if (iterSessionGroups == mapSessionGroups.cend())
                    mapSessionGroups[session.ullSessionGroupId] = ulSessGrp++;
            }
        }

		if (sortfunc)
			lstSessions.sort(sortfunc);
		else
			lstSessions.sort();
		if (bReverse)
			lstSessions.reverse();

        wmove(win, 0,0);
		memset(date, 0, 64);
		now = time(NULL);
		strftime(date, 64, "%c", localtime(&now) );
        wprintw(win, "Last update: %s (%.1fs since last)", date, dblTime);
		
        wmove(win, 1,0);
        wprintw(win, "Sess: %d", lstSessions.size());
        wmove(win, 1, 12);
        wprintw(win, "Sess grp: %d", mapSessionGroups.size());
        wmove(win, 1, 30);
        wprintw(win, "Users: %d", setUsers.size());
        wmove(win, 1, 42);
        wprintw(win, "Hosts: %d", setHosts.size());
        wmove(win, 1, 54);
        wprintw(win, "CPU: %d%%", (int)((dblUser+dblSystem)*100));
        wmove(win, 1, 64);
        wprintw(win, "QLen: %s", mapStats["queuelen"].c_str());
        wmove(win, 1, 73);
        wprintw(win, "QAge: %.5s", mapStats["queueage"].c_str());
        wmove(win, 1, 85);
        if(mapDiffStats["soap_request"] > 0)
            wprintw(win, "RTT: %d ms", (int)(mapDiffStats["response_time"] / mapDiffStats["soap_request"]));

        wmove(win, 2, 0);
        wprintw(win, "SQL/s SEL:%5d UPD:%4d INS:%4d DEL:%4d", (int)(mapDiffStats["sql_select"]/dblTime), (int)(mapDiffStats["sql_update"]/dblTime), (int)(mapDiffStats["sql_insert"]/dblTime), (int)(mapDiffStats["sql_delete"]/dblTime));
        wmove(win, 2, 50);
        wprintw(win, "Threads(idle): %s(%s)", mapStats["threads"].c_str(), mapStats["threads_idle"].c_str());
        wmove(win, 2, 75);
        wprintw(win, "MWOPS: %d  MROPS: %d", (int)(mapDiffStats["mwops"]/dblTime), (int)(mapDiffStats["mrops"]/dblTime));
        wmove(win, 2, 102);
        wprintw(win, "SOAP calls: %d", (int)(mapDiffStats["soap_request"]/dblTime));

		ofs = cols[0];
        if (bColumns[0]) { wmove(win, 4, ofs); wprintw(win, "GRP");			ofs += cols[1]; }
        if (bColumns[1]) { wmove(win, 4, ofs); wprintw(win, "SESSIONID");	ofs += cols[2]; }
        if (bColumns[2]) { wmove(win, 4, ofs); wprintw(win, "VERSION");		ofs += cols[3]; }
        if (bColumns[3]) { wmove(win, 4, ofs); wprintw(win, "USERID");		ofs += cols[4]; }
        if (bColumns[4]) { wmove(win, 4, ofs); wprintw(win, "IP/PID");		ofs += cols[5]; }
        if (bColumns[5]) { wmove(win, 4, ofs); wprintw(win, "APP");			ofs += cols[6]; }
        if (bColumns[6]) { wmove(win, 4, ofs); wprintw(win, "TIME");		ofs += cols[7]; }
        if (bColumns[7]) { wmove(win, 4, ofs); wprintw(win, "CPUTIME");		ofs += cols[8]; }
        if (bColumns[8]) { wmove(win, 4, ofs); wprintw(win, "CPU%");		ofs += cols[9]; }
        if (bColumns[9]) { wmove(win, 4, ofs); wprintw(win, "NREQ");		ofs += cols[10]; }
        if (bColumns[10]) { wmove(win, 4, ofs); wprintw(win, "STAT");		ofs += cols[11]; }
        if (bColumns[11]) { wmove(win, 4, ofs); wprintw(win, "TASK"); }
        
	for (const auto &ses : lstSessions) {
		if (ses.dtimes.dblUser + ses.dtimes.dblSystem > 0)
                wattron(win, A_BOLD);
            else
                wattroff(win, A_BOLD);

			ofs = cols[0];
			if (bColumns[0]) {
				wmove(win, 5 + line, ofs);
				if (ses.ullSessionGroupId > 0)
					wprintw(win, "%3d", mapSessionGroups[ses.ullSessionGroupId]);
				ofs += cols[1];
			}
			if (bColumns[1]) {
				wmove(win, 5 + line, ofs);
				wprintw(win, "%19llu", ses.ullSessionId);
				ofs += cols[2];
			}
			if (bColumns[2]) {
				wmove(win, 5 + line, ofs);
				wprintw(win, "%.*s", cols[3] - 1, ses.strClientVersion.c_str());
				ofs += cols[3];
			}
			if (bColumns[3]) {
				wmove(win, 5 + line, ofs);
				// the .24 caps off 24 bytes, not characters, so multi-byte UTF-8 is capped earlier than you might expect
				wprintw(win, "%.24s", ses.strUser.c_str());
				ofs += cols[4];
			}
			if (bColumns[4]) {
				wmove(win, 5 + line, ofs);
				if (ses.ulPeerPid > 0)
					wprintw(win, "%.20s", ses.strPeer.c_str());
				else
					wprintw(win, "%s", ses.strIP.c_str());
				ofs += cols[5];
			}
			if (bColumns[5]) {
				std::string dummy = ses.strClientApp + "/" +
					ses.strClientAppVersion + "/" +
					ses.strClientAppMisc;
				if (dummy.size() >= cols[6])
					dummy = dummy.substr(0, cols[6] - 1);
				wmove(win, 5 + line, ofs);
				wprintw(win, "%.*s", cols[6] - 1, dummy.c_str());
				ofs += cols[6];
			}
			if (bColumns[6]) {
				wmove(win, 5 + line, ofs);
				wprintw(win, "%d:%02d", static_cast<int>(ses.times.dblReal) / 60,
					static_cast<int>(ses.times.dblReal) % 60);
				ofs += cols[7];
			}
			if (bColumns[7]) {
				wmove(win, 5 + line, ofs);
				wprintw(win, "%d:%02d", static_cast<int>(ses.times.dblUser + ses.times.dblSystem) / 60,
					static_cast<int>(ses.times.dblUser + ses.times.dblUser) % 60);
				ofs += cols[8];
			}
			if (bColumns[8]) {
				wmove(win, 5 + line, ofs);
				wprintw(win, "%d", static_cast<int>(ses.dtimes.dblUser * 100.0 + ses.dtimes.dblSystem * 100.0));
				ofs += cols[9];
			}
			if (bColumns[9]) {
				wmove(win, 5 + line, ofs);
				wprintw(win, "%d", static_cast<int>(ses.dtimes.ulRequests));
				ofs += cols[10];
			}
			if (bColumns[10]) {
				wmove(win, 5 + line, ofs);
				wprintw(win, "%s", ses.strState.c_str());
				ofs += cols[11];
			}

			if (bColumns[11]) {
				wmove(win, 5 + line, ofs);
				wprintw(win, "%s", ses.strBusy.c_str());
			}

            ++line;
            
            if(line + 5>= wy)
            	break;
        }
		wattroff(win, A_BOLD);

        wrefresh(win);
        timeout(1000);
        if((key = getch()) != ERR) {
			if (key == 27 || key == 'q')				// escape key
				break;
			if (key == KEY_RESIZE)		// resize action
				getmaxyx(win, wy, wx);
			if (key >= '0' && key <= '9')
				bColumns[key-'1'] = !bColumns[key-'1']; // en/disable columns
			if (key >= 'a' && key <= 'g') {				// 'a' - 'g', sort columns
				if (sortfunc == fSort[key-'a'])
					bReverse = !bReverse;
				else
					bReverse = false;
				sortfunc = fSort[key-'a'];
			}
		}
    }

exit:
    endwin();
#else
	cerr << "Not compiled with ncurses support." << endl;
#endif
}
 
static void dumptable(eTableType eTable, LPMDB lpStore, bool humanreadable)
{
	HRESULT hr = hrSuccess;
	object_ptr<IMAPITable> lpTable;

	hr = lpStore->OpenProperty(ulTableProps[eTable], &IID_IMAPITable, 0, MAPI_DEFERRED_ERRORS, &~lpTable);
	if (hr != hrSuccess) {
		cout << "Unable to open requested statistics table" << endl;
		return;
	}

	if (sortorders[eTable] != NULL)
		hr = lpTable->SortTable(sortorders[eTable], 0);

	if (hr != hrSuccess) {
		cout << "Unable to sort statistics table" << endl;
		return;
	}
	MAPITablePrint(lpTable, humanreadable);
}

static void print_help(const char *name)
{
	cout << "Usage:" << endl;
	cout << name << " <options> [table]" << endl << endl;
	cout << "Supported tables:" << endl;
	cout << "  --system" << "\tGives information about threads, SQL and caches" << endl;
	cout << "  --session" << "\tGives information about sessions and server time spent in SOAP calls" << endl;
	cout << "  --users" << "\tGives information about users, store sizes and quotas" << endl;
	cout << "  --company" << "\tGives information about companies, company sizes and quotas" << endl;
	cout << "  --servers" << "\tGives information about cluster nodes" << endl;
	cout << "  --top" << "\t\tShows top-like information about sessions" << endl;
	cout << "Options:" << endl;
	cout << "  --user, -u <user>" << "\tUse specified username to logon" << endl;
	cout << "  --host, -h <url>" << "\tUse specified url to logon (eg http://127.0.0.1:236/)" << endl;
	cout << "  --dump, -d" << "\t\tPrint output as comma separated fields" << endl;
}

int main(int argc, char *argv[])
{
	HRESULT hr = hrSuccess;
	object_ptr<ECLogger> lpLogger(new ECLogger_File(EC_LOGLEVEL_FATAL, 0, "-", false));
	AutoMAPI mapiinit;
	object_ptr<IMAPISession> lpSession;
	object_ptr<IMsgStore> lpStore;
	eTableType eTable = INVALID_STATS;
	const char *user = NULL;
	const char *pass = NULL;
	const char *host = NULL;
	bool humanreadable(true);

	setlocale(LC_MESSAGES, "");
	setlocale(LC_CTYPE, "");
	
	if(argc < 2) {
		print_help(argv[0]);
		return 1;
	}

	int c;
	while (1) {
		c = getopt_long(argc, argv, "h:u:d", long_options, NULL);
		if(c == -1)
			break;
		switch(c) {
		case '?':
			print_help(argv[0]);
			return 1;
		case OPTION_HOST:
		case 'h':
			host = optarg;
			break;
		case 'u':
			user = optarg;
			break;
		case OPTION_DUMP:
		case 'd':
			humanreadable = false;
			break;
		case SYSTEM_STATS:
		case SESSION_STATS:
		case USER_STATS:
		case COMPANY_STATS:
		case SERVER_STATS:
		case SESSION_TOP:
			eTable = (eTableType)c;
			break;
		}
	}
	if (eTable == INVALID_STATS) {
		print_help(argv[0]);
		return 1;
	}

	hr = mapiinit.Initialize();
	if (hr != hrSuccess) {
		cerr << "Cannot init mapi" << endl;
		return EXIT_FAILURE;
	}
	
	if(user) {
        pass = get_password("Enter password:");
        if(!pass) {
            cout << "Invalid password." << endl;
            return 1;
	    }
	}

	auto strwUsername = convert_to<std::wstring>(user ? user : "SYSTEM");
	auto strwPassword = convert_to<std::wstring>(pass ? pass : "");
	hr = HrOpenECSession(&~lpSession, "stats", PROJECT_VERSION,
	     strwUsername.c_str(), strwPassword.c_str(), host,
	     EC_PROFILE_FLAGS_NO_NOTIFICATIONS | EC_PROFILE_FLAGS_NO_PUBLIC_STORE);
	if (hr != hrSuccess) {
		cout << "Cannot open admin session on host " << (host ? host : "localhost") << ", username " << (user ? user : "SYSTEM") << endl;
		return EXIT_FAILURE;
	}
	hr = HrOpenDefaultStore(lpSession, &~lpStore);
	if (hr != hrSuccess) {
		cout << "Unable to open default store" << endl;
		return EXIT_FAILURE;
	}
	if (eTable == SESSION_TOP)
		showtop(lpStore);
	else
		dumptable(eTable, lpStore, humanreadable);
	return EXIT_SUCCESS;
}
