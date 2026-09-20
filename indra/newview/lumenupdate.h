/**
 * @file lumenupdate.h
 * @brief Tell the user when a newer Lumen exists. Nothing more than that.
 *
 * Copyright (C) 2026 Catten Carter
 * Based on Phoenix Firestorm and on the Second Life Viewer.
 * Licensed under the GNU Lesser General Public License, version 2.1.
 *
 * **This is deliberately the smallest thing that answers the question**, which
 * the author put plainly: *"so we can update viewers if we find a bad fault"*.
 * It reads the newest release from GitHub, compares it with this build, and
 * says so. It does not download anything, does not replace anything, and
 * cannot stop an old viewer being used.
 *
 * **Why not the updater that is already in the tree.** Velopack is there and
 * would do the whole job -- but it is `#if LL_VELOPACK && LL_WINDOWS`, its
 * macOS support does not fit a drag-installed .dmg, and an updater that
 * replaces the binary is the highest-consequence code in a viewer: wrong once
 * and everybody is broken at the same moment. That is a poor trade for a
 * spare-time proof of concept.
 *
 * **And it is the same shape as the thing we removed.** FSData contacted
 * Firestorm's servers on every launch and carried `BlockedReleases` -- a lever
 * they held over a viewer they did not build. This points a similar mechanism
 * at ourselves, so three properties are load-bearing and must stay true:
 *
 *   - it only ever TELLS. No blocking, no downgrade, no settings changed.
 *     That was the actual objection to theirs, not the network call.
 *   - it is switchable off, `LumenUpdateCheck`.
 *   - it asks GitHub and nobody else, so there is no server of ours that
 *     could be pointed somewhere unpleasant later.
 */

#ifndef LUMEN_UPDATE_H
#define LUMEN_UPDATE_H

#include <string>

namespace LumenUpdate
{
    /** Start the check. Returns at once; the work is a coroutine. Safe to
     *  call when offline, when GitHub is down, and when rate-limited: every
     *  one of those is silence rather than a dialogue. */
    void checkWhenLoggedIn();

    /** Is `candidate` a later release than `current`? Both "0.1.0" or
     *  "v0.1.0". Exposed only so it can be reasoned about: an unparseable
     *  string is never "newer", so a malformed tag cannot nag anybody. */
    bool isNewer(const std::string& candidate, const std::string& current);
}

#endif // LUMEN_UPDATE_H
