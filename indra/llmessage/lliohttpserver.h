/**
 * @file lliohttpserver.h
 * @brief Declaration of function for creating an HTTP wire server
 * @see LLIOServerSocket, LLPumpIO
 *
 * $LicenseInfo:firstyear=2005&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLIOHTTPSERVER_H
#define LL_LLIOHTTPSERVER_H

#include "llchainio.h"
#include "llhttpnode.h"

class LLPumpIO;

class LLIOHTTPServer
{
public:
    typedef void (*timing_callback_t)(const char* hashed_name, F32 time, void* data);

    static LLHTTPNode& create(apr_pool_t* pool, LLPumpIO& pump, U16 port);
    /**< Creates an HTTP wire server on the pump for the given TCP port.
     *
     *   Returns the root node of the new server.  Add LLHTTPNode instances
     *   to this root.
     *
     *   NOTE: this overload binds to every interface (APR_ANYADDR) and calls
     *   LL_ERRS if the socket cannot be opened, which terminates the process.
     *   Prefer createSafe() for anything that should be reachable only from
     *   this machine, or that must not take the viewer down when a port is
     *   already in use.
     *
     *   Nodes that return NULL for getProtocolHandler(), will use the
     *   default handler that interprets HTTP on the wire and converts
     *   it into calls to get(), put(), post(), del() with appropriate
     *   LLSD arguments and results.
     *
     *   To have nodes that implement some other wire protocol (XML-RPC
     *   for example), use the helper templates below.
     */

    // <Lumen> A server that binds where it is told and survives failure.
    static LLHTTPNode* createSafe(apr_pool_t* pool, LLPumpIO& pump, U16 port,
                                  const char* bind_address);
    /**< Same as create(), with two differences that matter for a server
     *   embedded in the viewer rather than in a test harness.
     *
     *   It binds to `bind_address` instead of every interface. Pass
     *   "127.0.0.1" for an endpoint that must not be reachable from the
     *   network.
     *
     *   It returns NULL when the socket cannot be opened, rather than calling
     *   LL_ERRS and terminating. A port already in use is an ordinary
     *   condition for an optional feature and must not end the session.
     *
     *   Returns the root node, owned by the pump as in create(), or NULL.
     */
    // </Lumen>

    static void createPipe(LLPumpIO::chain_t& chain,
            const LLHTTPNode& root, const LLSD& ctx);
    /**< Create a pipe on the chain that handles HTTP requests.
     *   The requests are served by the node tree given at root.
     *
     *   This is primarily useful for unit testing.
     */

    static void setTimingCallback(timing_callback_t callback, void* data);
    /**< Register a callback function that will be called every time
    *    a GET, PUT, POST, or DELETE is handled.
    *
    * This is used to time the LLHTTPNode handler code, which often hits
    * the database or does other, slow operations. JC
    */
};

/* @name Helper Templates
 *
 * These templates make it easy to create nodes that use thier own protocol
 * handlers rather than the default.  Typically, you subclass LLIOPipe to
 * implement the protocol, and then add a node using the templates:
 *
 * rootNode->addNode("thing", new LLHTTPNodeForPipe<LLThingPipe>);
 *
 * The templates are:
 *
 *  LLChainIOFactoryForPipe
 *      - a simple factory that builds instances of a pipe
 *
 *  LLHTTPNodeForFacotry
 *      - a HTTP node that uses a factory as the protocol handler
 *
 *  LLHTTPNodeForPipe
 *      - a HTTP node that uses a simple factory based on a pipe
 */
//@{

template<class Pipe>
class LLChainIOFactoryForPipe : public LLChainIOFactory
{
public:
    virtual bool build(LLPumpIO::chain_t& chain, LLSD context) const
    {
        chain.push_back(LLIOPipe::ptr_t(new Pipe));
        return true;
    }
};

template<class Factory>
class LLHTTPNodeForFactory : public LLHTTPNode
{
public:
    const LLChainIOFactory* getProtocolHandler() const
        { return &mProtocolHandler; }

private:
    Factory mProtocolHandler;
};

//@}


template<class Pipe>
class LLHTTPNodeForPipe : public LLHTTPNodeForFactory<
                                    LLChainIOFactoryForPipe<Pipe> >
{
};


#endif // LL_LLIOHTTPSERVER_H

