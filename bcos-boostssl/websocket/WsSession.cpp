/*
 *  Copyright (C) 2021 FISCO BCOS.
 *  SPDX-License-Identifier: Apache-2.0
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  m_limitations under the License.
 *
 * @file WsSession.cpp
 * @author: octopus
 * @date 2021-07-08
 */

#include <bcos-boostssl/websocket/WsError.h>
#include <bcos-boostssl/websocket/WsSession.h>
#include <bcos-framework/interfaces/protocol/CommonError.h>
#include <bcos-framework/libutilities/DataConvertUtility.h>
#include <bcos-framework/libutilities/Log.h>
#include <bcos-framework/libutilities/ThreadPool.h>
#include <boost/beast/websocket/rfc6455.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <boost/core/ignore_unused.hpp>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

using namespace bcos;
using namespace bcos::boostssl;
using namespace bcos::boostssl::ws;

void WsSession::drop(uint32_t _reason)
{
    WEBSOCKET_SESSION(INFO) << LOG_BADGE("drop") << LOG_KV("reason", _reason)
                            << LOG_KV("endpoint", m_endPoint) << LOG_KV("session", this);

    m_isDrop = true;
    disconnect();
    auto self = std::weak_ptr<WsSession>(shared_from_this());
    m_threadPool->enqueue([self]() {
        auto session = self.lock();
        if (session)
        {
            session->disconnectHandler()(nullptr, session);
        }
    });
}

void WsSession::disconnect()
{
    if (m_stream)
    {
        m_stream->close();
        m_stream = nullptr;
    }

    WEBSOCKET_SESSION(INFO) << LOG_BADGE("disconnect") << LOG_DESC("disconnect the session")
                            << LOG_KV("endpoint", m_endPoint) << LOG_KV("session", this);
}

void WsSession::ping()
{
    try
    {
        if (m_stream)
        {
            m_stream->ping();
        }
    }
    catch (const std::exception& _e)
    {
        WEBSOCKET_SESSION(ERROR) << LOG_BADGE("ping") << LOG_KV("endpoint", m_endPoint)
                                 << LOG_KV("session", this)
                                 << LOG_KV("what", std::string(_e.what()));
        drop(WsError::PingError);
    }
}

void WsSession::pong()
{
    try
    {
        if (m_stream)
        {
            m_stream->pong();
        }
    }
    catch (const std::exception& _e)
    {
        WEBSOCKET_SESSION(ERROR) << LOG_BADGE("pong") << LOG_KV("endpoint", m_endPoint)
                                 << LOG_KV("session", this)
                                 << LOG_KV("what", std::string(_e.what()));
        drop(WsError::PongError);
    }
}

// TODO: init ping/pong

// void WsSession::initPingPoing()
// {
//     auto s = std::weak_ptr<WsSession>(shared_from_this());
//     auto endPoint = m_endPoint;
//     // callback for ping/pong
//     m_wsStream.control_callback([s, endPoint](auto&& _kind, auto&& _payload) {
//         auto session = s.lock();
//         if (!session)
//         {
//             return;
//         }

//         if (_kind == boost::beast::websocket::frame_type::ping)
//         {  // ping message
//             session->pong();
//             WEBSOCKET_SESSION(TRACE) << LOG_DESC("receive ping") << LOG_KV("endPoint", endPoint)
//                                      << LOG_KV("payload", _payload);
//         }
//         else if (_kind == boost::beast::websocket::frame_type::pong)
//         {  // pong message
//             WEBSOCKET_SESSION(TRACE) << LOG_DESC("receive pong") << LOG_KV("endPoint", endPoint)
//                                      << LOG_KV("payload", _payload);
//         }
//     });
// }

// start WsSession as client
void WsSession::startAsClient()
{
    if (m_connectHandler)
    {
        auto session = shared_from_this();
        m_connectHandler(nullptr, session);
    }

    // register ping/pong callback
    // initPingPoing();

    // read message
    asyncRead();

    WEBSOCKET_SESSION(INFO) << LOG_BADGE("startAsClient")
                            << LOG_DESC("websocket handshake successfully")
                            << LOG_KV("endPoint", m_endPoint) << LOG_KV("session", this);
}

// start WsSession as server
void WsSession::startAsServer(bcos::boostssl::http::HttpRequest _httpRequest)
{
    // register ping/pong callback
    // initPingPoing();

    // // set websocket params
    // m_wsStream.set_option(
    //     boost::beast::websocket::stream_base::timeout::suggested(boost::beast::role_type::server));

    WEBSOCKET_SESSION(INFO) << LOG_BADGE("startAsServer") << LOG_DESC("start websocket handshake")
                            << LOG_KV("endPoint", m_endPoint) << LOG_KV("session", this);

    auto session = shared_from_this();
    m_stream->asyncHandshake(_httpRequest,
        std::bind(&WsSession::onHandshake, shared_from_this(), std::placeholders::_1));
}

void WsSession::onHandshake(boost::beast::error_code _ec)
{
    if (_ec)
    {
        WEBSOCKET_SESSION(ERROR) << LOG_BADGE("onHandshake") << LOG_KV("error", _ec.message());
        return drop(WsError::AcceptError);
    }

    if (connectHandler())
    {
        connectHandler()(nullptr, shared_from_this());
    }

    asyncRead();

    WEBSOCKET_SESSION(INFO) << LOG_BADGE("onHandshake")
                            << LOG_DESC("websocket handshake successfully")
                            << LOG_KV("endPoint", endPoint()) << LOG_KV("session", this);
}

void WsSession::onReadPacket(boost::beast::flat_buffer& _buffer)
{
    auto data = boost::asio::buffer_cast<bcos::byte*>(boost::beast::buffers_front(_buffer.data()));
    auto size = boost::asio::buffer_size(m_buffer.data());

    auto message = m_messageFactory->buildMessage();
    if (message->decode(data, size) < 0)
    {  // invalid packet, stop this session ?
        WEBSOCKET_SESSION(ERROR) << LOG_BADGE("onReadPacket") << LOG_DESC("decode packet error")
                                 << LOG_KV("endpoint", endPoint()) << LOG_KV("session", this)
                                 << LOG_KV("data", *toHexString(data, data + size));
        return drop(WsError::PacketError);
    }

    _buffer.consume(_buffer.size());

    auto session = shared_from_this();
    auto seq = std::string(message->seq()->begin(), message->seq()->end());
    auto self = std::weak_ptr<WsSession>(session);
    auto callback = getAndRemoveRespCallback(seq);

    // task enqueue
    m_threadPool->enqueue([message, self, callback]() {
        auto session = self.lock();
        if (!session)
        {
            return;
        }
        if (callback)
        {
            if (callback->timer)
            {
                callback->timer->cancel();
            }

            callback->respCallBack(nullptr, message, session);
        }
        else
        {
            session->recvMessageHandler()(message, session);
        }
    });
}

void WsSession::asyncRead()
{
    try
    {
        m_stream->asyncRead(m_buffer, std::bind(&WsSession::onRead, shared_from_this(),
                                          std::placeholders::_1, std::placeholders::_2));
    }
    catch (const std::exception& _e)
    {
        WEBSOCKET_SESSION(ERROR) << LOG_BADGE("asyncRead") << LOG_DESC("exception")
                                 << LOG_KV("endpoint", endPoint()) << LOG_KV("session", this)
                                 << LOG_KV("what", std::string(_e.what()));
        drop(WsError::ReadError);
    }
}

void WsSession::onRead(boost::system::error_code _ec, std::size_t)
{
    if (_ec)
    {
        WEBSOCKET_SESSION(ERROR) << LOG_BADGE("asyncRead") << LOG_KV("error", _ec.message())
                                 << LOG_KV("endpoint", endPoint()) << LOG_KV("session", this);

        return drop(WsError::ReadError);
    }

    onReadPacket(buffer());
    asyncRead();
}

void WsSession::onWritePacket()
{
    std::unique_lock lock(x_queue);
    // remove the front ele from the queue, it has been sent
    m_queue.erase(m_queue.begin());

    // send the next message if any
    if (!m_queue.empty())
    {
        asyncWrite();
    }
}

void WsSession::asyncWrite()
{
    try
    {
        // Note: add one simple way to monitor message sending latency
        m_stream->asyncWrite(*m_queue.front(), std::bind(&WsSession::onWrite, shared_from_this(),
                                                   std::placeholders::_1, std::placeholders::_2));
    }
    catch (const std::exception& _e)
    {
        WEBSOCKET_SESSION(ERROR) << LOG_BADGE("asyncWrite")
                                 << LOG_DESC("async_write exception occur")
                                 << LOG_KV("session", this) << LOG_KV("endpoint", endPoint())
                                 << LOG_KV("what", std::string(_e.what()));
        drop(WsError::WriteError);
    }
}

void WsSession::onWrite(boost::beast::error_code _ec, std::size_t)
{
    if (_ec)
    {
        WEBSOCKET_SESSION(ERROR) << LOG_BADGE("asyncWrite") << LOG_KV("message", _ec.message())
                                 << LOG_KV("endpoint", endPoint()) << LOG_KV("session", this);
        return drop(WsError::WriteError);
    }

    onWritePacket();
}

/**
 * @brief: send message with callback
 * @param _msg: message to be send
 * @param _options: options
 * @param _respCallback: callback
 * @return void:
 */
void WsSession::asyncSendMessage(
    std::shared_ptr<WsMessage> _msg, Options _options, RespCallBack _respFunc)
{
    auto seq = std::string(_msg->seq()->begin(), _msg->seq()->end());
    auto buffer = std::make_shared<bcos::bytes>();
    _msg->encode(*buffer);

    if (_respFunc)
    {  // callback
        auto callback = std::make_shared<CallBack>();
        callback->respCallBack = _respFunc;
        auto timeout = _options.timeout > 0 ? _options.timeout : m_sendMsgTimeout;
        if (timeout > 0)
        {
            // create new timer to handle timeout
            auto timer = std::make_shared<boost::asio::deadline_timer>(
                *m_ioc, boost::posix_time::milliseconds(timeout));

            callback->timer = timer;
            auto self = std::weak_ptr<WsSession>(shared_from_this());
            timer->async_wait([self, seq](const boost::system::error_code& e) {
                auto session = self.lock();
                if (session)
                {
                    session->onRespTimeout(e, seq);
                }
            });
        }

        addRespCallback(seq, callback);
    }

    {
        std::unique_lock lock(x_queue);
        auto isEmpty = m_queue.empty();
        // data to be sent is always enqueue first
        m_queue.push_back(buffer);

        // no writing, send it
        if (isEmpty)
        {
            // we are not currently writing, so send this immediately
            asyncWrite();
        }
    }
}

void WsSession::addRespCallback(const std::string& _seq, CallBack::Ptr _callback)
{
    std::unique_lock lock(x_callback);
    m_callbacks[_seq] = _callback;
}

WsSession::CallBack::Ptr WsSession::getAndRemoveRespCallback(const std::string& _seq, bool _remove)
{
    CallBack::Ptr callback = nullptr;
    {
        std::shared_lock lock(x_callback);
        auto it = m_callbacks.find(_seq);
        if (it != m_callbacks.end())
        {
            callback = it->second;
            if (_remove)
            {
                m_callbacks.erase(it);
            }
        }
    }

    return callback;
}

void WsSession::onRespTimeout(const boost::system::error_code& _error, const std::string& _seq)
{
    if (_error)
    {
        return;
    }

    auto callback = getAndRemoveRespCallback(_seq);
    if (!callback)
    {
        return;
    }

    WEBSOCKET_SESSION(WARNING) << LOG_BADGE("onRespTimeout") << LOG_KV("seq", _seq);

    auto error =
        std::make_shared<Error>(WsError::TimeOut, "waiting for message response timed out");
    m_threadPool->enqueue([callback, error]() { callback->respCallBack(error, nullptr, nullptr); });
}