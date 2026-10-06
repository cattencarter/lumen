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
 * A skill is a notecard in `#Lumen/Skills`, so it follows the person to any
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
        std::string card_name;
        std::string problem;              //< the card on the server was refused; this is an older one
    };

    /** After login: read every card in #Lumen/Skills, then follow the folder for changes. */
    void startLoading();

    static bool isSkillTool(const std::string& name);
    /** #Lumen/Skills, or null when there is none yet. */
    LLUUID folderId() const { return skillsFolder(); }
    const Skill* find(const std::string& tool) const;
    /** One MCP tool per skill, for tools/list. */
    LLSD toolDescriptors() const;
    std::vector<std::string> toolNames() const;
    /** What is there and what is wrong with any card, for the model and the person. */
    LLSD describe() const;
    /** A line typed into the Assistant that is exactly a skill's trigger: its tool, else empty. */
    std::string toolForTrigger(const std::string& typed) const;

    /** Empty when the inputs will do; otherwise what to ask the person, for the model. */
    std::string checkInputs(const Skill& skill, const LLSD& inputs) const;
    /** A card somebody else made: shown and asked once per version before it first runs. */
    bool needsTrust(const Skill& skill) const;
    void trust(const Skill& skill);

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
    /** Start noting what the user does by hand: clicks, menus, objects, items. */
    void startTeaching();
    /** Stop, and hand back what was noted, oldest first, with names filled in. */
    LLSD stopTeaching();
    bool teaching() const { return mTeaching; }
    /** The viewer sending a touch -- the user's own click (lltoolgrab.cpp). */
    static void noteTouch(LLViewerObject* object, const LLVector2& st, const LLVector2& uv, S32 face);

    /** The text a card is written as: the header line, then the JSON, keys in a sensible order. */
    static std::string cardText(const LLSD& card);
    /** The card in #Lumen/Skills that holds the skill of this name, if one does. */
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

    std::vector<Skill> mSkills;
    mutable LLUUID mFolder;      //< #Lumen/Skills, kept while it still is that
    LLSD   mProblems;            //< card name -> why it was refused
    LLSD   mLastGood;            //< item id -> { asset, text }, kept on disk per account
    LLSD   mTrusted;             //< asset id -> true, kept on disk per account
    bool   mLoading = false;
    bool   mReloadWanted = false;
    bool   mLoaded = false;
    LLInventoryObserver* mObserver = nullptr;   //< the folder watcher; the inventory model deletes it at logout
    std::shared_ptr<Run> mRun;   //< the one running, or the last one finished

    bool   mTeaching = false;
    F64    mTeachStarted = 0.0;
    LLSD   mTeachEvents;                     //< what was noted, oldest first
    std::set<LLUUID> mTeachRoots;            //< objects in view when it started, or noted since
    std::map<LLUUID, S32> mTeachMenus;       //< a menu on screen -> its event
    std::map<LLUUID, LLNotificationPtr> mTeachMenuPtrs;
    LLTempBoundListener mTeachMenuListener;
    void noteTeachEvent(LLSD event);
    void pollTeaching();
    friend class LumenSkillsObserver;
};

#endif // LUMEN_AISKILLS_H
