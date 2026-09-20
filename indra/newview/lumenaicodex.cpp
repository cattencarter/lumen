/**
 * @file lumenaicodex.cpp
 * @brief A WebSocket-over-unix-socket client for OpenAI's Codex app-server.
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
 */

#include "llviewerprecompiledheaders.h"

#include "lumenaicodex.h"

#include "llfile.h"

#include "llbase64.h"
#include "llsdjson.h"
#include "llsdserialize.h"

#if LL_WINDOWS
// The app-server uses a named pipe on Windows rather than a unix socket, and
// that path is not implemented here. Said plainly rather than left to fail
// with a confusing error: Codex is a macOS and Linux option in Lumen today.
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#endif

namespace
{
    /** The user's home, which is NOT gDirUtilp->getOSUserDir(). */
    std::string homeDir()
    {
        const char* h = getenv("HOME");
        if (!h || !*h) h = getenv("USERPROFILE");
        return (h && *h) ? std::string(h) : std::string();
    }
}

std::string LumenAICodex::socketPath()
{
    const std::string home = homeDir();
    if (home.empty()) return std::string();
    const std::string s = gDirUtilp->getDirDelimiter();
    return home + s + ".codex" + s + "app-server-control" + s + "app-server-control.sock";
}

std::string LumenAICodex::cliPath()
{
    const std::string home = homeDir();
    if (home.empty()) return std::string();
    const std::string s = gDirUtilp->getDirDelimiter();
    return home + s + ".codex" + s + "packages" + s + "standalone" + s + "current"
         + s + "bin" + s + "codex";
}

std::vector<std::string> LumenAICodex::enabledPlugins()
{
    std::vector<std::string> out;
    const std::string home = homeDir();
    if (home.empty()) return out;
    const std::string s = gDirUtilp->getDirDelimiter();

    llifstream in((home + s + ".codex" + s + "config.toml").c_str());
    if (!in.is_open()) return out;

    // `[plugins."name@marketplace"]`, and nothing else on the line.
    std::string line;
    while (std::getline(in, line))
    {
        const size_t a = line.find("[plugins.\"");
        if (a == std::string::npos) continue;
        const size_t b = a + 10;
        const size_t c = line.find('"', b);
        if (c == std::string::npos || c == b) continue;
        out.push_back(line.substr(b, c - b));
    }
    return out;
}

bool LumenAICodex::socketPresent() { const std::string p = socketPath(); return !p.empty() && gDirUtilp->fileExists(p); }
bool LumenAICodex::cliInstalled()  { const std::string p = cliPath();    return !p.empty() && gDirUtilp->fileExists(p); }

LumenAICodex::LumenAICodex() : mFd(-1), mUpgraded(false) {}
LumenAICodex::~LumenAICodex() { close(); }

void LumenAICodex::close()
{
#if !LL_WINDOWS
    if (mFd >= 0) ::close(mFd);
#endif
    mFd = -1;
    mIn.clear();
    mUpgraded = false;
}

bool LumenAICodex::connect(std::string& why)
{
#if LL_WINDOWS
    why = "Codex is not available on Windows in this viewer yet.";
    return false;
#else
    close();
    const std::string path = socketPath();
    if (path.empty()) { why = "This system reports no home directory."; return false; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path))
    {
        why = "The Codex socket path is too long for a unix socket.";
        return false;
    }
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    mFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (mFd < 0) { why = "Could not make a socket."; return false; }

    if (::connect(mFd, (struct sockaddr*)&addr, sizeof(addr)) != 0)
    {
        close();
        why = "Codex's background service is not running. Start it with:  "
              "codex app-server daemon start";
        return false;
    }

    if (!handshake(why)) { close(); return false; }

    // Only AFTER the handshake, which is a blocking exchange of a few hundred
    // bytes. Doing it non-blocking would mean a state machine for four lines
    // of HTTP.
    fcntl(mFd, F_SETFL, fcntl(mFd, F_GETFL, 0) | O_NONBLOCK);
    mUpgraded = true;
    return true;
#endif
}

#if !LL_WINDOWS
bool LumenAICodex::handshake(std::string& why)
{
    // A random 16-byte key, as the protocol requires. The server's
    // Sec-WebSocket-Accept is deliberately NOT verified: it defends against a
    // confused cache or proxy, and there is neither between two processes on
    // one machine talking over a file.
    U8 raw[16];
    for (S32 i = 0; i < 16; ++i) raw[i] = (U8)(ll_rand() & 0xFF);
    const std::string key = LLBase64::encode(raw, sizeof(raw));

    const std::string req =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";

    if (::send(mFd, req.data(), req.size(), 0) != (ssize_t)req.size())
    {
        why = "Could not send the handshake to Codex.";
        return false;
    }

    std::string head;
    char buf[1024];
    for (S32 tries = 0; tries < 200 && head.find("\r\n\r\n") == std::string::npos; ++tries)
    {
        const ssize_t n = ::recv(mFd, buf, sizeof(buf), 0);
        if (n > 0) head.append(buf, n);
        else if (n == 0) { why = "Codex closed the connection during the handshake."; return false; }
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        {
            why = "Codex refused the connection.";
            return false;
        }
    }
    if (head.find(" 101 ") == std::string::npos)
    {
        why = "Codex did not accept the connection: " + head.substr(0, head.find("\r\n"));
        return false;
    }
    const size_t end = head.find("\r\n\r\n");
    mIn = head.substr(end + 4);      // anything already past the headers is a frame
    return true;
}

bool LumenAICodex::send(const LLSD& message)
{
    if (mFd < 0) return false;

    const std::string text = boost::json::serialize(LlsdToJson(message));

    // A client frame MUST be masked. The mask is four random bytes and the
    // payload is xored with them, repeating.
    U8 mask[4];
    for (S32 i = 0; i < 4; ++i) mask[i] = (U8)(ll_rand() & 0xFF);

    std::string frame;
    frame.push_back((char)0x81);                 // FIN + text
    const size_t n = text.size();
    if (n < 126)
    {
        frame.push_back((char)(0x80 | n));
    }
    else if (n < 65536)
    {
        frame.push_back((char)(0x80 | 126));
        frame.push_back((char)((n >> 8) & 0xFF));
        frame.push_back((char)(n & 0xFF));
    }
    else
    {
        frame.push_back((char)(0x80 | 127));
        for (S32 i = 7; i >= 0; --i) frame.push_back((char)((n >> (i * 8)) & 0xFF));
    }
    frame.append((const char*)mask, 4);
    for (size_t i = 0; i < n; ++i) frame.push_back((char)(text[i] ^ mask[i % 4]));

    size_t sent = 0;
    while (sent < frame.size())
    {
        const ssize_t w = ::send(mFd, frame.data() + sent, frame.size() - sent, 0);
        if (w > 0) { sent += w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        return false;
    }
    return true;
}

bool LumenAICodex::drain()
{
    char buf[16384];
    for (;;)
    {
        const ssize_t n = ::recv(mFd, buf, sizeof(buf), 0);
        if (n > 0) { mIn.append(buf, n); continue; }
        if (n == 0) { close(); return false; }          // server hung up
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        if (errno == EINTR) continue;
        close();
        return false;
    }
}

bool LumenAICodex::frame(std::string& payload)
{
    // Server frames are never masked, which keeps this short.
    for (;;)
    {
        if (mIn.size() < 2) return false;
        const U8 b0 = (U8)mIn[0], b1 = (U8)mIn[1];
        size_t len = b1 & 0x7F, off = 2;
        if (len == 126)
        {
            if (mIn.size() < 4) return false;
            len = ((U8)mIn[2] << 8) | (U8)mIn[3];
            off = 4;
        }
        else if (len == 127)
        {
            if (mIn.size() < 10) return false;
            len = 0;
            for (S32 i = 0; i < 8; ++i) len = (len << 8) | (U8)mIn[2 + i];
            off = 10;
        }
        if (mIn.size() < off + len) return false;

        const std::string body = mIn.substr(off, len);
        mIn.erase(0, off + len);
        const U8 op = b0 & 0x0F;
        if (op == 0x1) { payload = body; return true; }   // text
        if (op == 0x8) { close(); return false; }         // close
        // ping, pong and continuations are skipped; the next frame is read.
    }
}

bool LumenAICodex::poll(LLSD& out)
{
    if (mFd < 0) return false;
    if (!drain()) return false;

    std::string text;
    if (!frame(text)) return false;

    boost::json::error_code ec;
    const boost::json::value v = boost::json::parse(text, ec);
    if (ec) return false;
    out = LlsdFromJson(v);
    return true;
}
#else
bool LumenAICodex::handshake(std::string&) { return false; }
bool LumenAICodex::send(const LLSD&)       { return false; }
bool LumenAICodex::drain()                 { return false; }
bool LumenAICodex::frame(std::string&)     { return false; }
bool LumenAICodex::poll(LLSD&)             { return false; }
#endif
