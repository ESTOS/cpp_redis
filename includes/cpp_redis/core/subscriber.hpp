// The MIT License (MIT)
//
// Copyright (c) 2015-2017 Simon Ninon <simon.ninon@gmail.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>

#include <cpp_redis/core/sentinel.hpp>
#include <cpp_redis/core/types.hpp>
#include <cpp_redis/network/redis_connection.hpp>
#include <cpp_redis/network/tcp_client_iface.hpp>

namespace cpp_redis {

/**
 * The cpp_redis::subscriber is meant to be used for PUB/SUB communication with the Redis server.
 * Please do not use cpp_redis::client to subscribe to some Redis channels as:
 *  * the behavior is undefined
 *  * cpp_redis::client is not meant for that
 *
 */
	class subscriber {
	public:
#ifndef __CPP_REDIS_USE_CUSTOM_TCP_CLIENT

/**
 * ctor
 *
 */
			subscriber();

#endif /* __CPP_REDIS_USE_CUSTOM_TCP_CLIENT */

/**
 * custom ctor to specify custom tcp_client
 *
 * @param tcp_client tcp client to be used for network communications
 *
 */
			explicit subscriber(const std::shared_ptr<network::tcp_client_iface> &tcp_client);

/**
 * dtor
 *
 */
			~subscriber();

/**
 * copy ctor
 *
 */
			subscriber(const subscriber &) = delete;

/**
 * assignment operator
 *
 */
			subscriber &operator=(const subscriber &) = delete;

	public:
/**
 * @brief Connect to redis server
 * @param host host to be connected to
 * @param port port to be connected to
 * @param connect_callback connect handler to be called on connect events (may be null)
 * @param timeout_ms maximum time to connect
 * @param max_reconnects maximum attempts of reconnection if connection dropped
 * @param reconnect_interval_ms time between two attempts of reconnection
 * @param use_encryption enables TLS when set to true
 *
 */
			void connect(
					const std::string &host = "127.0.0.1",
					std::size_t port = 6379,
					const connect_callback_t &connect_callback = nullptr,
					std::uint32_t timeout_ms = 0,
					std::int32_t max_reconnects = 0,
					std::uint32_t reconnect_interval_ms = 0,
					bool use_encryption = false);

/**
 * @brief Connect to redis server
 * @param name sentinel name
 * @param connect_callback connect handler to be called on connect events (may be null)
 * @param timeout_ms maximum time to connect
 * @param max_reconnects maximum attempts of reconnection if connection dropped
 * @param reconnect_interval_ms time between two attempts of reconnection
 * @param use_encryption enables TLS when set to true
 *
 */
			void connect(
					const std::string &name,
					const connect_callback_t &connect_callback = nullptr,
					std::uint32_t timeout_ms = 0,
					std::int32_t max_reconnects = 0,
					std::uint32_t reconnect_interval_ms = 0,
					bool use_encryption = false);

/**
 * @brief determines client connectivity
 * @return whether we are connected to the redis server
 *
 */
			bool is_connected() const;

/**
 * @brief disconnect from redis server
 * @param wait_for_removal when set to true, disconnect blocks until the underlying TCP client has been effectively removed from the io_service and that all the underlying callbacks have completed.
 *
 */
			void disconnect(bool wait_for_removal = false);

/**
 * @brief determines if reconnect is in progress
 * @return whether an attempt to reconnect is in progress
 *
 */
			bool is_reconnecting() const;

/**
 * @brief stop any reconnect in progress
 *
 */
			void cancel_reconnect();

	public:
/**
 * @brief reply callback called whenever a reply is received, takes as parameter the received reply
 *
 */
			typedef std::function<void(reply &)> reply_callback_t;

/**
 * @brief ability to authenticate on the redis server if necessary
 * this method should not be called repeatedly as the storage of reply_callback is NOT thread safe (only one reply callback is stored for the subscriber client)
 * calling repeatedly auth() is undefined concerning the execution of the associated callbacks
 * @param password password to be used for authentication
 * @param reply_callback callback to be called on auth completion (nullable)
 * @return current instance
 *
 */
			subscriber &auth(const std::string &password, const reply_callback_t &reply_callback = nullptr);

/**
 * @brief Set the label for the connection on the Redis server via the CLIENT SETNAME command.
 * This is useful for monitoring and managing connections on the server side of things.
 * @param name - string to label the connection with on the server side
 * @param reply_callback callback to be called on auth completion (nullable)
 * @return current instance
 *
 */
			subscriber& client_setname(const std::string& name, const reply_callback_t& reply_callback = nullptr);

/**
 * subscribe callback, called whenever a new message is published on a subscribed channel
 * takes as parameter the channel and the message
 *
 */
			typedef std::function<void(const std::string &, const std::string &)> subscribe_callback_t;

/**
 * Subscribes to the given channel and:
 *  * calls acknowledgement_callback once the server has acknowledged about the subscription.
 *  * calls subscribe_callback each time a message is published on this channel.
 * The command is not effectively sent immediately but stored in an internal buffer until commit() is called.
 *
 * @param channel channel to subscribe
 * @param callback callback to be called whenever a message is received for this channel
 * @param acknowledgement_callback callback to be called on subscription completion (nullable)
 * @return current instance
 */
//!
			subscriber &subscribe(const std::string &channel, const subscribe_callback_t &callback,
			                      const acknowledgement_callback_t &acknowledgement_callback = nullptr);

/**
 * PSubscribes to the given channel and:
 *  * calls acknowledgement_callback once the server has acknowledged about the subscription.
 *  * calls subscribe_callback each time a message is published on this channel.
 * The command is not effectively sent immediately but stored in an internal buffer until commit() is called.
 *
 * @param pattern pattern to psubscribe
 * @param callback callback to be called whenever a message is received for this pattern
 * @param acknowledgement_callback callback to be called on subscription completion (nullable)
 * @return current instance
 */
//!
			subscriber &psubscribe(const std::string &pattern, const subscribe_callback_t &callback,
			                       const acknowledgement_callback_t &acknowledgement_callback = nullptr);

/**
 * unsubscribe from the given channel
 * The command is not effectively sent immediately, but stored inside an internal buffer until commit() is called.
 *
 * @param channel channel to unsubscribe from
 * @return current instance
 *
 */
			subscriber &unsubscribe(const std::string &channel);

/**
 * punsubscribe from the given pattern
 * The command is not effectively sent immediately, but stored inside an internal buffer until commit() is called.
 *
 * @param pattern pattern to punsubscribe from
 * @return current instance
 *
 */
			subscriber &punsubscribe(const std::string &pattern);

/**
 * commit pipelined transaction
 * that is, send to the network all commands pipelined by calling send() / subscribe() / ...
 *
 * @return current instance
 *
 */
			subscriber &commit();

/**
 * Sends a ping to the redis server
 * The command is not effectively sent immediately, but stored inside an internal buffer until commit() is called.
 * 
 * @param message (optional) argument for ping to be returnd as second multi-bulk
 * @param reply_callback (optional) callback to be called when the anser to the ping is received
 * @return current instance
 * 
 */
			subscriber& ping(const std::string& message = "", const reply_callback_t& reply_callback = nullptr);

	public:
/**
 * add a sentinel definition. Required for connect() or get_master_addr_by_name() when autoconnect is enabled.
 *
 * @param host sentinel host
 * @param port sentinel port
 * @param timeout_ms maximum time to connect
 *
 */
			void add_sentinel(const std::string &host, std::size_t port, std::uint32_t timeout_ms, bool use_encryption);

/**
 * retrieve sentinel for current client
 *
 * @return sentinel associated to current client
 *
 */
			const sentinel &get_sentinel() const;

/**
 * retrieve sentinel for current client
 * non-const version
 *
 * @return sentinel associated to current client
 *
 */
			sentinel &get_sentinel();

/**
 * clear all existing sentinels.
 *
 */
			void clear_sentinels();

	private:
/**
 * struct to hold callbacks (sub and ack) for a given channel or pattern
 *
 */
			struct callback_holder {
					subscribe_callback_t subscribe_callback;
					acknowledgement_callback_t acknowledgement_callback;
			};

	private:
/**
 * redis connection receive handler, triggered whenever a reply has been read by the redis connection
 *
 * @param connection redis_connection instance
 * @param reply parsed reply
 *
 */
			void connection_receive_handler(network::redis_connection &connection, reply &reply);

/**
 * redis_connection disconnection handler, triggered whenever a disconnection occurred
 *
 * @param connection redis_connection instance
 *
 */
			void connection_disconnection_handler(network::redis_connection &connection);

/**
 * trigger the ack callback for matching channel/pattern
 * check if reply is valid
 *
 * @param reply received reply
 *
 */
			void handle_acknowledgement_reply(const std::vector<reply> &reply);

/**
 * trigger the sub callback for all matching channels/patterns
 * check if reply is valid
 *
 * @param reply received reply
 *
 */
			void handle_subscribe_reply(const std::vector<reply> &reply);

/**
 * trigger the sub callback for all matching channels/patterns
 * check if reply is valid
 *
 * @param reply received reply
 *
 */
			void handle_psubscribe_reply(const std::vector<reply> &reply);

			void handle_ping_reply(const cpp_redis::reply& reply);

/**
* reset the queue of pending callbacks
*
*/
            void clear_ping_callbacks();


                        /**
 * find channel or pattern that is associated to the reply and call its ack callback
 *
 * @param channel channel or pattern that caused the issuance of this reply
 * @param channels list of channels or patterns to be searched for the received channel
 * @param channels_mtx channels or patterns mtx to be locked for race condition
 * @param nb_chans redis server ack reply
 *
 */
			void
			call_acknowledgement_callback(const std::string &channel, const std::map<std::string, callback_holder> &channels,
			                              std::mutex &channels_mtx, int64_t nb_chans);

	private:
/**
 * reconnect to the previously connected host
 * automatically re authenticate and resubscribe to subscribed channel in case of success
 *
 */
			void reconnect();

/**
 * re authenticate to redis server based on previously used password
 *
 */
			void re_auth();

/**
 * re send CLIENT SETNAME to redis server based on previously used name
 *
 */
			void re_client_setname(void);

/**
 * resubscribe (sub and psub) to previously subscribed channels/patterns
 *
 */
			void re_subscribe();

/**
 * @return whether a reconnection attempt should be performed
 *
 */
			bool should_reconnect() const;

/**
 * sleep between two reconnect attempts if necessary
 *
 */
			void sleep_before_next_reconnect_attempt();

/**
 * clear all subscriptions (dirty way, no unsub/punsub commands send: mostly used for cleaning in disconnection condition)
 *
 */
			void clear_subscriptions();

	private:
/**
 * unprotected sub
 * same as subscribe, but without any mutex lock
 *
 * @param channel channel to subscribe
 * @param callback callback to be called whenever a message is received for this channel
 * @param acknowledgement_callback callback to be called on subscription completion (nullable)
 *
 */
			void unprotected_subscribe(const std::string &channel, const subscribe_callback_t &callback,
			                           const acknowledgement_callback_t &acknowledgement_callback);

/**
 * unprotected psub
 * same as psubscribe, but without any mutex lock
 *
 * @param pattern pattern to psubscribe
 * @param callback callback to be called whenever a message is received for this pattern
 * @param acknowledgement_callback callback to be called on subscription completion (nullable)
 *
 */
			void unprotected_psubscribe(const std::string &pattern, const subscribe_callback_t &callback,
			                            const acknowledgement_callback_t &acknowledgement_callback);

	private:
/**
 * server we are connected to
 *
 */
			std::string m_redis_server;
/**
 * port we are connected to
 *
 */
			std::size_t m_redis_port = 0;
/**
 * master name (if we are using sentinel) we are connected to
 *
 */
			std::string m_master_name;
/**
 * password used to authenticate
 *
 */
			std::string m_password;

/**
 * name to use with CLIENT SETNAME
 *
 */
			std::string m_client_name;

/**
 * tcp client for redis connection
 *
 */
			network::redis_connection m_client;

/**
 * redis sentinel
 *
 */
			cpp_redis::sentinel m_sentinel;

/**
 * max time to connect
 *
 */
			std::uint32_t m_connect_timeout_ms = 0;
/**
 * max number of reconnection attempts
 *
 */
			std::int32_t m_max_reconnects = 0;
/**
 * current number of attempts to reconnect
 *
 */
			std::int32_t m_current_reconnect_attempts = 0;
/**
 * time between two reconnection attempts
 *
 */
			std::uint32_t m_reconnect_interval_ms = 0;
 /**
 * use encryption
 */
			bool m_use_encryption = false;

/**
 * reconnection status
 *
 */
			std::atomic_bool m_reconnecting;
/**
 * to force cancel reconnection
 *
 */
			std::atomic_bool m_cancel;

/**
 * subscribed channels and their associated channels
 *
 */
			std::map<std::string, callback_holder> m_subscribed_channels;
/**
 * psubscribed channels and their associated channels
 *
 */
			std::map<std::string, callback_holder> m_psubscribed_channels;

/**
 * connect handler
 *
 */
			connect_callback_t m_connect_callback;

/**
* ping callbacks
* 
*/
			std::queue<reply_callback_t> m_ping_callbacks;

/**
*  ping callbacks thread safety
*
*/
            std::mutex m_ping_callbacks_mutex;


/**
 * sub chans thread safety
 *
 */
			std::mutex m_psubscribed_channels_mutex;
/**
 * psub chans thread safety
 *
 */
			std::mutex m_subscribed_channels_mutex;

/**
 * auth reply callback
 *
 */
			reply_callback_t m_auth_reply_callback;

/**
 * client setname reply callback
 *
 */
			reply_callback_t m_client_setname_reply_callback;
	};

} // namespace cpp_redis
