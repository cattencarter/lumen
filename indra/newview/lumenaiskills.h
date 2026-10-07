/**
 * @file lumenaiskills.h
 * @brief Skills: a routine taught once and carried out on request, step by checked step.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Lumen Viewer Source Code
 * Copyright (C) 2026, Catten Carter
 * Based on the Phoenix Firestorm Viewer, Copyright (C) 2026,
 * The Phoenix Firestorm Project, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 *
 * Some things in Second Life are a routine of many small steps -- find two
 * herbs, click a HUD, press a menu button, rez the herbs, pick up what
 * appears. A model can do each step, but doing them reliably, the same way
 * every time, is not what a model is for. A skill keeps the steps fixed: the
 * model decides WHEN to use one and fills in what changes (which sickness),
 * and the viewer carries the steps out top to bottom, checking each before
 * the next and stopping with a reason when one fails.
 *
 * A skill is a notecard in `#Lumen/#Skills`, so it follows the person to any
 * computer and survives a reinstall. Inside: a first line `Lumen skill 1`,
 * then JSON. The person never needs to read it -- they teach a skill by
 * doing it once -- but a card broken by a hand edit is refused with the
 * reason, and the last version that worked is used instead.
 *
 * Each skill is offered to the model as a tool of its own, `skill_<name>`,
 * described in the person's own words. Running one goes through the same
 * front door as every other tool, so the action log, the replay of a
 * repeated call and the refusals all apply to every step it takes. The
 * viewer's per-act questions are not asked for the steps: the skill as a
 * whole is what the person approved, and it can ask once before it starts.
 *
 * There are deliberately no steps that give, pay or delete, and a script's
 * request for permissions is never answered by a skill.
 */
#ifndef LUMEN_AISKILLS_H
#define LUMEN_AISKILLS_H

#include "llevents.h"
#include "llsingleton.h"
#include "llsd.h"
#include "lluuid.h"
#include "v2math.h"
#include "v3dmath.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "llnotificationptr.h"

class LLInventoryObserver;
class LLViewerObject;

class LumenAISkills : public LLSingleton<LumenAISkills>
{
    LLSINGLETON(LumenAISkills);
    ~LumenAISkills();

public:
    struct Input
    {
        std::string name;                 //< as the steps refer to it: {sickness}
        std::string about;                //< for the model: what to ask for
        bool        required = true;
        std::vector<std::string> choices; //< empty: anything
    };

    struct Skill
    {
        std::string tool;                 //< "skill_brew_a_potion"
        std::string name;                 //< "Brew a potion", the person's own words
        std::string about;                //< when to use it, in the person's words
        std::string summary;              //< what it does, in plain words, for the questions
        std::string trigger;              //< a phrase that runs it with no model at all
        std::vector<std::string> examples;
        std::vector<Input> inputs;
        LLSD        tables;               //< name -> { key -> value }
        LLSD        steps;                //< checked when the card was read
        bool        ask_first = true;     //< a one-line question before every run
        LLSD        card;                 //< the card as read, for changing it by chat
        LLUUID      item_id, asset_id, creator_id;
        LLUUID      card_asset_id;        //< what the card holds now, which may be a refused version
        std::string card_name;
        std::string problem;              //< the card on the server was refused; this is an older one
    };

    /** After login: read every card in #Lumen/#Skills, then follow the folder for changes. */
    void startLoading();

    static bool isSkillTool(const std::string& name);
    /** #Lumen/#Skills, or null when there is none yet. */
    LLUUID folderId() const { return skillsFolder(); }
    const Skill* find(const std::string& tool) const;
    /** By its tool name or by the name the user gave it. */
    const Skill* findAny(const std::string& tool_or_name) const;
    /** One MCP tool per skill, for tools/list. */
    LLSD toolDescriptors() const;
    std::vector<std::string> toolNames() const;
    /** Every card read, the ones not yet looked at on this computer too: for the Skills window. */
    const std::vector<Skill>& all() const { return mSkills; }
    const LLSD& problems() const { return mProblems; }   //< card name -> why it was refused
    /** What is there and what is wrong with any card, for the model and the person. */
    LLSD describe() const;
    /** A line typed into the Assistant that is exactly a skill's trigger: its tool, else empty. */
    std::string toolForTrigger(const std::string& typed) const;

    /** Empty when the inputs will do; otherwise what to ask the person, for the model. */
    std::string checkInputs(const Skill& skill, const LLSD& inputs) const;
    /**
     * A card this viewer did not write itself, through save_skill: what its
     * steps do is shown and asked about once per version before it runs, and
     * until then it is not one of the model's tools.
     */
    bool needsTrust(const Skill& skill) const;
    void trust(const Skill& skill);
    /** A card's new version, just written by save_skill after the user said yes. */
    void noteWritten(const LLUUID& asset_id);
    /**
     * What the skill touches, a line for each kind -- what it rezzes, accepts,
     * clicks and presses, sits on, takes, calls -- read by the viewer from the
     * steps' own fields, for the questions that guard a skill.
     */
    static std::string questionText(const Skill& skill);
    /** Every step, as the viewer reads it, numbered: for the Assistant, beside the question. */
    static std::string stepsText(const Skill& skill);

    /**
     * Start a run, or say how the one already running for these same inputs is
     * getting on, or hand back how it ended. `running` in the result means call
     * again with the same arguments.
     */
    LLSD run(const Skill& skill, const LLSD& inputs);
    /** Whether a run for these inputs has started and not been collected. */
    bool hasRun(const Skill& skill, const LLSD& inputs) const;
    /** Clear in the Assistant window, or logging out: stop between steps. */
    void stopAll();

    /** True while the runner is making one of its own calls. */
    static bool stepRunning();

    // ---- teaching --------------------------------------------------------
    /** Start noting what the user does by hand: clicks, menus, objects, items.
        Empty, or why it cannot start now (a skill is running). */
    std::string startTeaching();
    /** Stop, and hand back what was noted, oldest first, with names filled in -- once. */
    LLSD stopTeaching();
    bool teaching() const { return mTeaching; }
    /** The viewer sending a touch -- the user's own click (lltoolgrab.cpp). */
    static void noteTouch(LLViewerObject* object, const LLVector2& st, const LLVector2& uv, S32 face);
    /**
     * Held by the endpoint while it carries out a request (lumenaictl.cpp,
     * handleRequest): a touch it sends or a menu it answers in that time is
     * the assistant's, and teaching does not note it as the user's.
     */
    struct EndpointActing
    {
        EndpointActing();
        ~EndpointActing();
        EndpointActing(const EndpointActing&) = delete;
        EndpointActing& operator=(const EndpointActing&) = delete;
    private:
        std::string mCoro;   //< the coroutine the request runs on
    };

    /** The text a card is written as: the header line, then the JSON, keys in a sensible order. */
    static std::string cardText(const LLSD& card);
    /** The card in #Lumen/#Skills that holds the skill of this name, if one does. */
    LLUUID cardFor(const std::string& name) const;

    /** The card's text read into a skill; false with `error` saying what is wrong, in plain words. */
    static bool parse(const std::string& text, Skill& out, std::string& error);
    /** What the skill does, one short line per step, in everyday words. */
    static std::string plainSummary(const Skill& skill);

    struct Run;   // lumenaiskills.cpp

private:
    void loadNow();
    void watchFolder();
    void scheduleReload();
    LLUUID skillsFolder() const;
    void readLastGood();
    void writeLastGood() const;
    void readTrusted();
    void writeTrusted() const;
    void readOwn();
    void writeOwn() const;

    std::vector<Skill> mSkills;
    mutable LLUUID mFolder;      //< #Lumen/#Skills, kept while it still is that
    LLSD   mProblems;            //< card name -> why it was refused
    LLSD   mLastGood;            //< item id -> { asset, text }, kept on disk per account
    LLSD   mTrusted;             //< asset id -> true, kept on disk per account
    LLSD   mOwn;                 //< asset id -> true: cards save_skill wrote, kept on disk per account
    bool   mLoading = false;
    bool   mReloadWanted = false;
    bool   mLoaded = false;
    LLInventoryObserver* mObserver = nullptr;   //< the folder watcher; the inventory model deletes it at logout
    std::shared_ptr<Run> mRun;   //< the one running, or the last one finished

    bool   mTeaching = false;
    F64    mTeachStarted = 0.0;              //< 0 once what was noted has been handed back
    F64    mTeachStopped = 0.0;              //< when watching ended
    std::string mTeachEnded;                 //< "" when teach_stop ended it; "time" or "cleared"
    LLSD   mTeachEvents;                     //< what was noted, oldest first
    S32    mTeachNext = 0;                   //< the number the last event got
    bool   mTeachTruncated = false;          //< something was left out to keep it short
    std::set<LLUUID> mTeachRoots;            //< objects in view when it started, or noted since
    LLUUID mTeachSeat;                       //< what the user sat on when last looked, or null
    LLVector3d mTeachLastPos;                //< where the avatar was at the last look round
    F64    mTeachSettleUntil = 0.0;          //< after a teleport or a long jump, what appears streamed in
    std::map<LLUUID, S32> mTeachMenus;       //< a menu on screen -> its event's number
    std::map<LLUUID, LLNotificationPtr> mTeachMenuPtrs;
    LLTempBoundListener mTeachMenuListener;
    void noteTeachEvent(LLSD event);
    void pollTeaching();
    void noteArrivals();
    void endTeaching(const std::string& how);
    friend class LumenSkillsObserver;
};

#endif // LUMEN_AISKILLS_H
