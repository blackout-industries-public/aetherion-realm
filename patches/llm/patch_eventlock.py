#!/usr/bin/env python3
"""Serialize the random-bot event cache for multithreaded map updates.

eventCache is a bare unordered_map written by SetEventValue and read through
FindEvent - and RandomBotUpdateAction reaches both from map threads. With
MapUpdate.Threads = 1 that was accidentally serialized; with more threads a rehash
during a concurrent read is a crash. A recursive mutex guards every touch point;
the getters copy values out under the lock so no pointer escapes unguarded.
"""
import sys

path = sys.argv[1]
src = open(path).read()

if "sEventCacheMx" in src:
    print("event-cache lock already applied")
    sys.exit(0)

INC = '#include "PartyAssembler.h"\n'
assert src.count(INC) == 1, "include anchor (run after patch_processbot)"
src = src.replace(INC, INC + '''
#include <mutex>

namespace
{
    // Guards RandomPlayerbotMgr::eventCache. Recursive because the getters lock and
    // then call FindEvent, which locks again.
    std::recursive_mutex sEventCacheMx;
}
''', 1)

FIND = """CachedEvent* RandomPlayerbotMgr::FindEvent(uint32 bot, std::string const& event)
{
    BotEventCache& cache = eventCache[bot];"""
assert src.count(FIND) == 1, "FindEvent anchor"
src = src.replace(FIND, """CachedEvent* RandomPlayerbotMgr::FindEvent(uint32 bot, std::string const& event)
{
    std::lock_guard<std::recursive_mutex> lock(sEventCacheMx);
    BotEventCache& cache = eventCache[bot];""", 1)

GETV = """uint32 RandomPlayerbotMgr::GetEventValue(uint32 bot, std::string const& event)
{
    if (CachedEvent* e = FindEvent(bot, event))
        return e->value;

    return 0;
}"""
assert src.count(GETV) == 1, "GetEventValue anchor"
src = src.replace(GETV, """uint32 RandomPlayerbotMgr::GetEventValue(uint32 bot, std::string const& event)
{
    std::lock_guard<std::recursive_mutex> lock(sEventCacheMx);
    if (CachedEvent* e = FindEvent(bot, event))
        return e->value;

    return 0;
}""", 1)

GETD = """std::string RandomPlayerbotMgr::GetEventData(uint32 bot, std::string const& event)
{
    if (CachedEvent* e = FindEvent(bot, event))
        return e->data;

    return "";
}"""
assert src.count(GETD) == 1, "GetEventData anchor"
src = src.replace(GETD, """std::string RandomPlayerbotMgr::GetEventData(uint32 bot, std::string const& event)
{
    std::lock_guard<std::recursive_mutex> lock(sEventCacheMx);
    if (CachedEvent* e = FindEvent(bot, event))
        return e->data;

    return "";
}""", 1)

SETV = """    // Update in-memory cache
    BotEventCache& cache = eventCache[bot];"""
assert src.count(SETV) == 1, "SetEventValue cache anchor"
src = src.replace(SETV, """    // Update in-memory cache
    std::lock_guard<std::recursive_mutex> lock(sEventCacheMx);
    BotEventCache& cache = eventCache[bot];""", 1)

CLR = "        sRandomPlayerbotMgr.eventCache.clear();"
assert src.count(CLR) == 1, "clear anchor"
src = src.replace(CLR, """        {
            std::lock_guard<std::recursive_mutex> lock(sEventCacheMx);
            sRandomPlayerbotMgr.eventCache.clear();
        }""", 1)

ERS = "    eventCache.erase(botId);"
assert src.count(ERS) == 1, "erase anchor"
src = src.replace(ERS, """    {
        std::lock_guard<std::recursive_mutex> lock(sEventCacheMx);
        eventCache.erase(botId);
    }""", 1)

open(path, "w").write(src)
print("patched RandomPlayerbotMgr.cpp (event cache serialized for map threads)")
