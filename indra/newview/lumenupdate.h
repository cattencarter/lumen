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
 * It does not download anything, does not replace anything, and cannot stop
 * an old viewer being used.
 *
 * **Luminous's way, since 0.1.8** (the author, 2026-10-28). Once a session, as
 * the login screen shows -- so the message is there before anybody logs in --
 * one GET of a small JSON file on needfulsl.com (`LumenUpdateURL`):
 *
 *     { "version": "0.1.9",
 *       "url": "https://needfulsl.com/lumen/",
 *       "notes": "What is new, a line or two." }
 *
 * When its version is newer than this one (`LUMEN_VERSION`), a window with
 * the notes, Download -- the url (https only) in the browser -- and Later.
 * It tells again the next day the viewer starts while it is still older,
 * whichever was clicked (`LumenUpdateSeen`, `LumenUpdateToldDay`); a newer
 * version at once. An answer that comes after the login shows in the world.
 * Nothing is sent but the request itself; no answer, or a bad one, is only
 * logged.
 *
 * Before 0.1.8 this asked GitHub's releases API, on the argument that a file
 * of ours could be pointed somewhere unpleasant later. That argument was about
 * FSData, and the three properties that answered it still hold, which is what
 * matters:
 *
 *   - it only ever TELLS. No blocking, no downgrade, no settings changed but
 *     its own two.
 *   - it is switchable off, `LumenUpdateCheck`: no request at all.
 *   - Download only ever opens an https page in the browser.
 *
 * Viewers up to 0.1.7.1 still ask GitHub, so a release goes on GitHub as well.
 *
 * **Why not the updater that is already in the tree.** Velopack is there and
 * would do the whole job -- but it is `#if LL_VELOPACK && LL_WINDOWS`, its
 * macOS support does not fit a drag-installed .dmg, and an updater that
 * replaces the binary is the highest-consequence code in a viewer: wrong once
 * and everybody is broken at the same moment.
 */

#ifndef LUMEN_UPDATE_H
#define LUMEN_UPDATE_H

#include <string>

namespace LumenUpdate
{
    /** Start the check, once a session. Returns at once; the work is a
     *  coroutine. Offline, a missing file or a bad answer is a line in the
     *  log, never a dialogue. */
    void check();

    /** Is `candidate` a later release than `current`? Both "0.1.0" or
     *  "v0.1.0". Exposed only so it can be reasoned about: an unparseable
     *  string is never "newer", so a malformed file cannot nag anybody. */
    bool isNewer(const std::string& candidate, const std::string& current);
}

#endif // LUMEN_UPDATE_H
