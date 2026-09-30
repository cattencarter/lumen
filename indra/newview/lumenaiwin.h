/**
 * @file lumenaiwin.h
 * @brief What starting Codex, Claude Code and Mistral Vibe needs on Windows.
 *
 * Copyright (C) 2026 Catten Carter
 * Based on Phoenix Firestorm and on the Second Life Viewer.
 * Licensed under the GNU Lesser General Public License, version 2.1.
 */

#ifndef LUMEN_AIWIN_H
#define LUMEN_AIWIN_H

#include <cctype>
#include <string>

namespace LumenAIWin
{
    /**
     * One argument, escaped for the Windows command line LLProcess builds.
     *
     * LLProcess hands its arguments to APR, and APR 1.7.5 on Windows
     * (threadproc/win32/proc.c) wraps an argument in quotes when it holds any
     * whitespace and escapes NOTHING: a quotation mark inside it ends the
     * argument early, and a backslash before the closing quote swallows it.
     * Seen in the Windows VM, 2026-09-30: Claude Code's `--mcp-config` JSON
     * arrived with every quotation mark gone. So each argument is escaped here
     * by the rules the programs' own C runtime reads it back with -- a quote
     * as \", the backslashes before one doubled, and trailing backslashes
     * doubled when APR is about to add the closing quote -- and APR's quotes
     * are left to APR. Identity everywhere else.
     */
    inline std::string arg(const std::string& s)
    {
#if LL_WINDOWS
        bool wrapped = s.empty();
        for (char c : s)
        {
            if (isspace((unsigned char)c)) { wrapped = true; break; }
        }
        std::string out;
        size_t slashes = 0;
        for (char c : s)
        {
            if (c == '\\') { ++slashes; continue; }
            if (c == '"')
            {
                out.append(slashes * 2 + 1, '\\');
                out.push_back('"');
            }
            else
            {
                out.append(slashes, '\\');
                out.push_back(c);
            }
            slashes = 0;
        }
        out.append(wrapped ? slashes * 2 : slashes, '\\');
        return out;
#else
        return s;
#endif
    }

    /** A program's file name as it is on this platform: "codex" -> "codex.exe". */
    inline std::string exe(const std::string& name)
    {
#if LL_WINDOWS
        return name + ".exe";
#else
        return name;
#endif
    }

    /** Windows PowerShell, by its full path: a viewer is not given the user's PATH. */
    inline std::string powershell()
    {
        const char* root = getenv("SystemRoot");
        const std::string windows = (root && *root) ? std::string(root) : std::string("C:\\Windows");
        return windows + "\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    }

    /** cmd.exe, by its full path, for the one launch that needs a redirection. */
    inline std::string cmd()
    {
        const char* root = getenv("SystemRoot");
        const std::string windows = (root && *root) ? std::string(root) : std::string("C:\\Windows");
        return windows + "\\System32\\cmd.exe";
    }
}

#endif // LUMEN_AIWIN_H
