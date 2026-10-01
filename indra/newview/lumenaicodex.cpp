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
#include "lltimer.h"      // <Lumen> deadlines on the handshake and on send
#include "llprocess.h"    // <Lumen> startService

#include "llbase64.h"
#include "llsdjson.h"
#include "llsdserialize.h"

#include "lumenaiwin.h"   // <Lumen>

#if LL_WINDOWS
// <Lumen> On Windows Lumen does not use Codex's daemon at all. It starts its
// own `codex app-server --listen ws://127.0.0.1:PORT` and speaks the same
// WebSocket to it over a loopback TCP socket (Findings 43: the daemon refuses
// an elevated session, and its control socket is one more thing to find).
#include "llwin32headers.h"   // winsock2.h, after windows.h as it must be
#include <ws2tcpip.h>
typedef int sock_io_t;
#define LUMEN_FD(fd)            ((SOCKET)(fd))
#define LUMEN_SOCK_ERR()        WSAGetLastError()
#define LUMEN_SOCK_WOULDBLOCK(e) ((e) == WSAEWOULDBLOCK)
#define LUMEN_SOCK_INTR(e)      ((e) == WSAEINTR)
#else
#include <sys/socket.h>
#include <sys/select.h>   // <Lumen> bounded waits on the handshake and on send
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
typedef ssize_t sock_io_t;
#define LUMEN_FD(fd)            ((int)(fd))
#define LUMEN_SOCK_ERR()        errno
#define LUMEN_SOCK_WOULDBLOCK(e) ((e) == EAGAIN || (e) == EWOULDBLOCK)
#define LUMEN_SOCK_INTR(e)      ((e) == EINTR)
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

    // <Lumen> A socket, closed, and made non-blocking, on either platform.
    void closeSocket(intptr_t fd)
    {
#if LL_WINDOWS
        closesocket((SOCKET)fd);
#else
        ::close((int)fd);
#endif
    }

    void setNonBlocking(intptr_t fd)
    {
#if LL_WINDOWS
        u_long on = 1;
        ioctlsocket((SOCKET)fd, FIONBIO, &on);
#else
        fcntl((int)fd, F_SETFL, fcntl((int)fd, F_GETFL, 0) | O_NONBLOCK);
#endif
    }

    /** Wait up to `ms` for the socket to be readable (or writable). */
    int waitFor(intptr_t fd, bool write, int ms)
    {
        fd_set set;
        FD_ZERO(&set);
#if LL_WINDOWS
        FD_SET((SOCKET)fd, &set);
#else
        FD_SET((int)fd, &set);
#endif
        struct timeval wait = { ms / 1000, (ms % 1000) * 1000 };   // select refuses a microsecond count past a second
        return ::select((int)fd + 1, write ? NULL : &set, write ? &set : NULL, NULL, &wait);
    }

#if LL_WINDOWS
    // Lumen's own app-server: the process, and the port it was told to use.
    LLProcessPtr sServer;
    U16          sPort = 0;

    bool winsockReady()
    {
        static const bool ok = []() { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
        return ok;
    }

    /** A port nothing on this machine is using, from the system itself. */
    U16 freeLoopbackPort()
    {
        if (!winsockReady()) return 0;
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) return 0;
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        U16 port = 0;
        int len = (int)sizeof(a);
        if (bind(s, (sockaddr*)&a, (int)sizeof(a)) == 0 && getsockname(s, (sockaddr*)&a, &len) == 0)
        {
            port = ntohs(a.sin_port);
        }
        closesocket(s);
        return port;
    }

    /**
     * A TCP connection to 127.0.0.1:port, non-blocking, within `ms`; -1 when
     * nobody answered. A refused loopback connect on Windows is RETRIED by the
     * system for a second or two, so the wait is bounded rather than trusted.
     */
    intptr_t connectLoopback(U16 port, int ms)
    {
        if (!winsockReady() || port == 0) return -1;
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) return -1;
        setNonBlocking((intptr_t)s);
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        if (::connect(s, (sockaddr*)&a, (int)sizeof(a)) != 0 && WSAGetLastError() != WSAEWOULDBLOCK)
        {
            closesocket(s);
            return -1;
        }
        if (waitFor((intptr_t)s, true, ms) <= 0)
        {
            closesocket(s);
            return -1;
        }
        int err = 0;
        int len = (int)sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
        if (err != 0)
        {
            closesocket(s);
            return -1;
        }
        return (intptr_t)s;
    }
#endif
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
         + s + "bin" + s + LumenAIWin::exe("codex");   // <Lumen> codex.exe on Windows
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

bool LumenAICodex::socketPresent()
{
#if LL_WINDOWS
    // <Lumen> No socket file on Windows: "there" is Lumen's own server
    // running. Cheap, as the once-a-second heartbeat needs; listening() is the
    // connect.
    return sServer && sServer->isRunning();
#else
    const std::string p = socketPath();
    return !p.empty() && gDirUtilp->fileExists(p);
#endif
}
bool LumenAICodex::cliInstalled()  { const std::string p = cliPath();    return !p.empty() && gDirUtilp->fileExists(p); }

std::string LumenAICodex::unavailableHere()
{
    // <Lumen> Works on Windows too since 2026-09-30, through Lumen's own
    // app-server on a loopback port (see the top of this file).
    return std::string();
}

bool LumenAICodex::listening()
{
#if LL_WINDOWS
    // <Lumen> Ours, running, and answering on its port.
    if (!sServer || !sServer->isRunning()) return false;
    const intptr_t fd = connectLoopback(sPort, 50);
    if (fd < 0) return false;
    closeSocket(fd);
    return true;
#else
    if (!socketPresent()) return false;
    const std::string path = socketPath();

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) return false;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    // Non-blocking, so a listener with a full backlog cannot hold the frame
    // loop; that answers EAGAIN or EINPROGRESS, which still means somebody is
    // there. A stale file with nobody behind it answers ECONNREFUSED at once.
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    const int rc = ::connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    const bool up = (rc == 0) || errno == EAGAIN || errno == EINPROGRESS;
    ::close(fd);
    return up;
#endif
}

// <Lumen>
bool LumenAICodex::startService()
{
    static F64 sStarted = -1000.0;
    if (!unavailableHere().empty() || !cliInstalled()) return false;
    const std::string home = homeDir();
    const std::string s = gDirUtilp->getDirDelimiter();
    if (home.empty() || !gDirUtilp->fileExists(home + s + ".codex" + s + "auth.json")) return false;
    if (listening()) return false;

    const F64 now = LLTimer::getTotalSeconds();
    if (now - sStarted < 15.0)  return true;    // ours, still coming up
    if (now - sStarted < 120.0) return false;   // tried, and it did not come up
    sStarted = now;

#if LL_WINDOWS
    // <Lumen> Lumen's own app-server, on a free loopback port, for as long as
    // the viewer runs (autokill). From the home folder, as below. Watched in
    // the Windows VM: `codex app-server --listen ws://127.0.0.1:PORT` listens
    // on localhost only, and serves /readyz.
    sPort = freeLoopbackPort();
    if (sPort == 0)
    {
        LL_WARNS("LumenAICodex") << "Could not find a free local port for Codex." << LL_ENDL;
        return false;
    }
    LLProcess::Params wp;
    wp.executable = cliPath();
    wp.args.add("app-server");
    wp.args.add("--listen");
    wp.args.add(llformat("ws://127.0.0.1:%d", (int)sPort));
    wp.cwd = home;
    wp.autokill = true;
    wp.hidden = true;   // no console window for the whole session
    sServer = LLProcess::create(wp);
    LL_INFOS("LumenAICodex") << (sServer ? "Started Codex's app-server on port "
                                         : "Could not start Codex's app-server on port ")
                             << sPort << LL_ENDL;
    return (bool)sServer;
#else

    // From the home folder, never from the viewer's own: the service keeps
    // the folder it was started in, and the viewer's is inside the app bundle
    // an update replaces (Findings 41). Not autokill: it is meant to outlive
    // this call, and the viewer too.
    LLProcess::Params p;
    p.executable = cliPath();
    p.args.add("app-server");
    p.args.add("daemon");
    p.args.add("start");
    p.cwd = home;
    p.autokill = false;
    LLProcessPtr proc = LLProcess::create(p);
    LL_INFOS("LumenAICodex") << "Codex's background service was not running; "
                             << (proc ? "starting it" : "could not start it") << LL_ENDL;
    return (bool)proc;
#endif
}
// </Lumen>

LumenAICodex::LumenAICodex() : mFd(-1), mUpgraded(false) {}
LumenAICodex::~LumenAICodex() { close(); }

void LumenAICodex::close()
{
    if (mFd >= 0) closeSocket(mFd);   // <Lumen> either platform
    mFd = -1;
    mIn.clear();
    mUpgraded = false;
}

bool LumenAICodex::connect(std::string& why)
{
#if LL_WINDOWS
    // <Lumen> Lumen's own app-server; the caller starts it and waits when it
    // is not there yet (startService, then listening()).
    close();
    if (!sServer || !sServer->isRunning())
    {
        why = "Codex is not running yet.";
        return false;
    }
    mFd = connectLoopback(sPort, 500);   // on the frame loop, so short
    if (mFd < 0)
    {
        why = "Codex is starting but not answering yet.";
        return false;
    }
    if (!handshake(why)) { close(); return false; }
    mUpgraded = true;
    return true;
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

    if (::connect(LUMEN_FD(mFd), (struct sockaddr*)&addr, sizeof(addr)) != 0)
    {
        close();
        why = "Codex's background service is not running. Start it with:  "
              "codex app-server daemon start";
        return false;
    }

    // <Lumen> Non-blocking BEFORE the handshake, not after. The handshake used
    // to run on a blocking socket, so a daemon that accepted the connection
    // and then said nothing -- busy, wedged, mid-restart -- held the frame
    // loop in recv() for as long as it liked; the "200 tries" cap in
    // handshake() never bounded anything, because the read never returned to
    // count them. With the socket non-blocking the loop there really does
    // poll, and gives up after a few seconds.
    setNonBlocking(mFd);
    if (!handshake(why)) { close(); return false; }
    // </Lumen>
    mUpgraded = true;
    return true;
#endif
}

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

    if (::send(LUMEN_FD(mFd), req.data(), (int)req.size(), 0) != (sock_io_t)req.size())
    {
        why = "Could not send the handshake to Codex.";
        return false;
    }

    std::string head;
    char buf[1024];
    // <Lumen> Wait for readability with a short timeout, up to a deadline,
    // rather than spinning: the socket is non-blocking now (see connect()).
    const F64 deadline = LLTimer::getTotalSeconds() + 5.0;
    while (head.find("\r\n\r\n") == std::string::npos)
    {
        if (LLTimer::getTotalSeconds() > deadline)
        {
            why = "Codex accepted the connection but did not answer within five seconds. "
                  "Try again in a moment; if it keeps happening, quit and reopen Lumen.";
            return false;
        }
        const int ready = waitFor(mFd, false, 50);   // 50 ms
        if (ready < 0 && !LUMEN_SOCK_INTR(LUMEN_SOCK_ERR())) { why = "Codex refused the connection."; return false; }
        if (ready <= 0) continue;

        const sock_io_t n = ::recv(LUMEN_FD(mFd), buf, (int)sizeof(buf), 0);
        if (n > 0) head.append(buf, n);
        else if (n == 0) { why = "Codex closed the connection during the handshake."; return false; }
        else if (!LUMEN_SOCK_WOULDBLOCK(LUMEN_SOCK_ERR()) && !LUMEN_SOCK_INTR(LUMEN_SOCK_ERR()))
        {
            why = "Codex refused the connection.";
            return false;
        }
    }
    // </Lumen>
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
    const F64 deadline = LLTimer::getTotalSeconds() + 5.0;   // <Lumen> bounded
    while (sent < frame.size())
    {
        const sock_io_t w = ::send(LUMEN_FD(mFd), frame.data() + sent, (int)(frame.size() - sent), 0);
        if (w > 0) { sent += w; continue; }
        const int err = LUMEN_SOCK_ERR();
        if (w < 0 && (LUMEN_SOCK_WOULDBLOCK(err) || LUMEN_SOCK_INTR(err)))
        {
            // <Lumen> The socket is non-blocking, so a full kernel buffer used
            // to make this a busy loop at 100% CPU on the frame loop until the
            // daemon drained it. Wait for writability instead, and give up
            // after a few seconds rather than never.
            if (LLTimer::getTotalSeconds() > deadline) return false;
            waitFor(mFd, true, 50);
            continue;
            // </Lumen>
        }
        return false;
    }
    return true;
}

bool LumenAICodex::drain()
{
    char buf[16384];
    for (;;)
    {
        const sock_io_t n = ::recv(LUMEN_FD(mFd), buf, (int)sizeof(buf), 0);
        if (n > 0) { mIn.append(buf, n); continue; }
        if (n == 0) { close(); return false; }          // server hung up
        const int err = LUMEN_SOCK_ERR();
        if (LUMEN_SOCK_WOULDBLOCK(err)) return true;
        if (LUMEN_SOCK_INTR(err)) continue;
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

    boost::system::error_code ec;   // <Lumen> json::error_code is deprecated, an error under MSVC
    const boost::json::value v = boost::json::parse(text, ec);
    if (ec) return false;
    out = LlsdFromJson(v);
    return true;
}
