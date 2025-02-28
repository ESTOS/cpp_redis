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

#include <cpp_redis/core/subscriber.hpp>
#include <cpp_redis/misc/error.hpp>
#include <cpp_redis/misc/logger.hpp>

#include <thread>

namespace cpp_redis {

#ifndef __CPP_REDIS_USE_CUSTOM_TCP_CLIENT
subscriber::subscriber()
: m_reconnecting(false)
, m_cancel(false)
, m_auth_reply_callback(nullptr)
, m_client_setname_reply_callback(nullptr) {
  __CPP_REDIS_LOG(debug, "cpp_redis::subscriber created");
}
#endif /* __CPP_REDIS_USE_CUSTOM_TCP_CLIENT */

subscriber::subscriber(const std::shared_ptr<network::tcp_client_iface>& tcp_client)
: m_client(tcp_client)
, m_sentinel(tcp_client)
, m_reconnecting(false)
, m_cancel(false)
, m_auth_reply_callback(nullptr) {
  __CPP_REDIS_LOG(debug, "cpp_redis::subscriber created");
}

subscriber::~subscriber() {
  //! ensure we stopped reconnection attempts
  if (!m_cancel) {
    cancel_reconnect();
  }

  //! If for some reason sentinel is connected then disconnect now.
  if (m_sentinel.is_connected()) {
    m_sentinel.disconnect(true);
  }

  //! disconnect underlying tcp socket
  if (m_client.is_connected()) {
    m_client.disconnect(true);
  }

  __CPP_REDIS_LOG(debug, "cpp_redis::subscriber destroyed");
}


void
subscriber::connect(
  const std::string& name,
  const connect_callback_t& connect_callback,
  std::uint32_t timeout_ms,
  std::int32_t max_reconnects,
  std::uint32_t reconnect_interval_ms,
  bool use_encryption) {
  //! Save for auto reconnects
  m_master_name = name;

  //! We rely on the sentinel to tell us which redis server is currently the master.
  if (m_sentinel.get_master_addr_by_name(name, m_redis_server, m_redis_port, true)) {
    connect(m_redis_server, m_redis_port, connect_callback, timeout_ms, max_reconnects, reconnect_interval_ms, use_encryption);
  }
  else {
    throw redis_error("cpp_redis::subscriber::connect() could not find master for m_name " + name);
  }
}


void
subscriber::connect(
  const std::string& host, std::size_t port,
  const connect_callback_t& connect_callback,
  std::uint32_t timeout_ms,
  std::int32_t max_reconnects,
  std::uint32_t reconnect_interval_ms,
  bool use_encryption) {
  __CPP_REDIS_LOG(debug, "cpp_redis::subscriber attempts to connect");

  //! Save for auto reconnects
  m_redis_server             = host;
  m_redis_port               = port;
  m_connect_callback         = connect_callback;
  m_max_reconnects           = max_reconnects;
  m_reconnect_interval_ms    = reconnect_interval_ms;
  m_use_encryption           = use_encryption;

  //! notify start
  if (m_connect_callback) {
    m_connect_callback(host, port, connect_state::start);
  }

  auto disconnection_handler = std::bind(&subscriber::connection_disconnection_handler, this, std::placeholders::_1);
  auto receive_handler       = std::bind(&subscriber::connection_receive_handler, this, std::placeholders::_1, std::placeholders::_2);
  m_client.connect(host, port, disconnection_handler, receive_handler, timeout_ms, use_encryption);

  //! notify end
  if (m_connect_callback) {
    m_connect_callback(m_redis_server, m_redis_port, connect_state::ok);
  }

  __CPP_REDIS_LOG(info, "cpp_redis::subscriber connected");
}

void
subscriber::add_sentinel(const std::string& host, std::size_t port, std::uint32_t timeout_ms, bool use_encryption) {
  m_sentinel.add_sentinel(host, port, timeout_ms, use_encryption);
}

const sentinel&
subscriber::get_sentinel() const {
  return m_sentinel;
}

sentinel&
subscriber::get_sentinel() {
  return m_sentinel;
}

void
subscriber::clear_sentinels() {
  m_sentinel.clear_sentinels();
}

void
subscriber::cancel_reconnect() {
  m_cancel = true;
}

subscriber&
subscriber::auth(const std::string& password, const reply_callback_t& reply_callback) {
  __CPP_REDIS_LOG(debug, "cpp_redis::subscriber attempts to authenticate");

  m_password            = password;
  m_auth_reply_callback = reply_callback;

  m_client.send({"AUTH", password});

  __CPP_REDIS_LOG(info, "cpp_redis::subscriber AUTH command sent");

  return *this;
}

subscriber&
subscriber::client_setname(const std::string& name, const reply_callback_t& reply_callback) {
  __CPP_REDIS_LOG(debug, "cpp_redis::subscriber attempts to send CLIENT SETNAME");

  // Retain the name as CLIENT SETNAME can only be sent between the reAUTH and reSUBSCRIBE
  // commands on reconnecting.  This makes it impossible to do reliably in the application
  // layer as opposed to in the subscriber itself for reconnects.  re_client_setname will
  // send the command at reconnect time.
  m_client_name = name;
  m_client_setname_reply_callback = reply_callback;

  m_client.send({"CLIENT", "SETNAME", name});

  __CPP_REDIS_LOG(info, "cpp_redis::subscriber CLIENT SETNAME command sent");

  return *this;
}

void
subscriber::disconnect(bool wait_for_removal) {
  __CPP_REDIS_LOG(debug, "cpp_redis::subscriber attempts to disconnect");
/**
 * close connection
 */
  m_client.disconnect(wait_for_removal);

/**
 * make sure we clear buffer of unanswered callbacks
 */
  clear_ping_callbacks();
  __CPP_REDIS_LOG(info, "cpp_redis::subscriber disconnected");
}

bool
subscriber::is_connected() const {
  return m_client.is_connected();
}

bool
subscriber::is_reconnecting() const {
  return m_reconnecting;
}

subscriber&
subscriber::subscribe(const std::string& channel, const subscribe_callback_t& callback, const acknowledgement_callback_t& acknowledgement_callback) {
  std::lock_guard<std::mutex> lock(m_subscribed_channels_mutex);

  __CPP_REDIS_LOG(debug, "cpp_redis::subscriber attempts to subscribe to channel " + channel);
  unprotected_subscribe(channel, callback, acknowledgement_callback);
  __CPP_REDIS_LOG(info, "cpp_redis::subscriber subscribed to channel " + channel);

  return *this;
}

void
subscriber::unprotected_subscribe(const std::string& channel, const subscribe_callback_t& callback, const acknowledgement_callback_t& acknowledgement_callback) {
  m_subscribed_channels[channel] = {callback, acknowledgement_callback};
  m_client.send({"SUBSCRIBE", channel});
}

subscriber&
subscriber::psubscribe(const std::string& pattern, const subscribe_callback_t& callback, const acknowledgement_callback_t& acknowledgement_callback) {
  std::lock_guard<std::mutex> lock(m_psubscribed_channels_mutex);

  __CPP_REDIS_LOG(debug, "cpp_redis::subscriber attempts to psubscribe to channel " + pattern);
  unprotected_psubscribe(pattern, callback, acknowledgement_callback);
  __CPP_REDIS_LOG(info, "cpp_redis::subscriber psubscribed to channel " + pattern);

  return *this;
}

void
subscriber::unprotected_psubscribe(const std::string& pattern, const subscribe_callback_t& callback, const acknowledgement_callback_t& acknowledgement_callback) {
  m_psubscribed_channels[pattern] = {callback, acknowledgement_callback};
  m_client.send({"PSUBSCRIBE", pattern});
}

subscriber&
subscriber::unsubscribe(const std::string& channel) {
  std::lock_guard<std::mutex> lock(m_subscribed_channels_mutex);

  __CPP_REDIS_LOG(debug, "cpp_redis::subscriber attempts to unsubscribe from channel " + channel);
  auto it = m_subscribed_channels.find(channel);
  if (it == m_subscribed_channels.end()) {
    __CPP_REDIS_LOG(debug, "cpp_redis::subscriber was not subscribed to channel " + channel);
    return *this;
  }

  m_client.send({"UNSUBSCRIBE", channel});
  m_subscribed_channels.erase(it);
  __CPP_REDIS_LOG(info, "cpp_redis::subscriber unsubscribed from channel " + channel);

  return *this;
}

subscriber&
subscriber::punsubscribe(const std::string& pattern) {
  std::lock_guard<std::mutex> lock(m_psubscribed_channels_mutex);

  __CPP_REDIS_LOG(debug, "cpp_redis::subscriber attempts to punsubscribe from channel " + pattern);
  auto it = m_psubscribed_channels.find(pattern);
  if (it == m_psubscribed_channels.end()) {
    __CPP_REDIS_LOG(debug, "cpp_redis::subscriber was not psubscribed to channel " + pattern);
    return *this;
  }

  m_client.send({"PUNSUBSCRIBE", pattern});
  m_psubscribed_channels.erase(it);
  __CPP_REDIS_LOG(info, "cpp_redis::subscriber punsubscribed from channel " + pattern);

  return *this;
}

subscriber&
subscriber::commit() {
  try {
    __CPP_REDIS_LOG(debug, "cpp_redis::subscriber attempts to send pipelined commands");
    m_client.commit();
    __CPP_REDIS_LOG(info, "cpp_redis::subscriber sent pipelined commands");
  }
  catch (const cpp_redis::redis_error&) {
    __CPP_REDIS_LOG(error, "cpp_redis::subscriber could not send pipelined commands");
    throw;
  }

  return *this;
}

subscriber& 
subscriber::ping(const std::string& message, const reply_callback_t& reply_callback) {
  try {
    __CPP_REDIS_LOG(debug, "cpp_redis::subscriber attempts to send ping");
    {
      std::lock_guard<std::mutex> lock(m_ping_callbacks_mutex);
      if (message.empty())
        m_client.send({"PING"});
      else
        m_client.send({"PING", message});
      m_ping_callbacks.push(reply_callback);
    }
  }
  catch (const cpp_redis::redis_error&) {
    __CPP_REDIS_LOG(error, "cpp_redis::subscriber could not send ping");
    throw;
  }
  return *this;
}

void
subscriber::call_acknowledgement_callback(const std::string& channel, const std::map<std::string, callback_holder>& channels, std::mutex& channels_mtx, int64_t nb_chans) {
  std::lock_guard<std::mutex> lock(channels_mtx);

  auto it = channels.find(channel);
  if (it == channels.end())
    return;

  if (it->second.acknowledgement_callback) {
    __CPP_REDIS_LOG(debug, "cpp_redis::subscriber executes acknowledgement callback for channel " + channel);
    it->second.acknowledgement_callback(nb_chans);
  }
}

void
subscriber::handle_acknowledgement_reply(const std::vector<reply>& reply) {
  if (reply.size() != 3)
    return;

  const auto& title    = reply[0];
  const auto& channel  = reply[1];
  const auto& nb_chans = reply[2];

  if (!title.is_string()
      || !channel.is_string()
      || !nb_chans.is_integer())
    return;

  if (title.as_string() == "subscribe")
    call_acknowledgement_callback(channel.as_string(), m_subscribed_channels, m_subscribed_channels_mutex, nb_chans.as_integer());
  else if (title.as_string() == "psubscribe")
    call_acknowledgement_callback(channel.as_string(), m_psubscribed_channels, m_psubscribed_channels_mutex, nb_chans.as_integer());
}

void
subscriber::handle_subscribe_reply(const std::vector<reply>& reply) {
  if (reply.size() != 3)
    return;

  const auto& title   = reply[0];
  const auto& channel = reply[1];
  const auto& message = reply[2];

  if (!title.is_string()
      || !channel.is_string()
      || !message.is_string())
    return;

  if (title.as_string() != "message")
    return;

  std::lock_guard<std::mutex> lock(m_subscribed_channels_mutex);

  auto it = m_subscribed_channels.find(channel.as_string());
  if (it == m_subscribed_channels.end())
    return;

  __CPP_REDIS_LOG(debug, "cpp_redis::subscriber executes subscribe callback for channel " + channel.as_string());
  it->second.subscribe_callback(channel.as_string(), message.as_string());
}

void
subscriber::handle_psubscribe_reply(const std::vector<reply>& reply) {
  if (reply.size() != 4)
    return;

  const auto& title    = reply[0];
  const auto& pchannel = reply[1];
  const auto& channel  = reply[2];
  const auto& message  = reply[3];

  if (!title.is_string()
      || !pchannel.is_string()
      || !channel.is_string()
      || !message.is_string())
    return;

  if (title.as_string() != "pmessage")
    return;

  std::lock_guard<std::mutex> lock(m_psubscribed_channels_mutex);

  auto it = m_psubscribed_channels.find(pchannel.as_string());
  if (it == m_psubscribed_channels.end())
    return;

  __CPP_REDIS_LOG(debug, "cpp_redis::subscriber executes psubscribe callback for channel " + channel.as_string());
  it->second.subscribe_callback(channel.as_string(), message.as_string());
}

void 
subscriber::handle_ping_reply(const cpp_redis::reply& reply) {
  if (!reply.is_array() || reply.as_array().size() != 2)
    return;

  const auto& pong    = reply.as_array()[0];
  const auto& pongmsg = reply.as_array()[1];
  if (!pong.is_string() || !pongmsg.is_string())
    return;

  __CPP_REDIS_LOG(debug, "cpp_redis::subscriber received ping reply " + pong.as_string() + " " + pongmsg.as_string());

  reply_callback_t callback = nullptr;
  {
    std::lock_guard<std::mutex> lock(m_ping_callbacks_mutex);
    //m_callbacks_running += 1;

    if (!m_ping_callbacks.empty()) {
      callback = m_ping_callbacks.front();
      m_ping_callbacks.pop();
    }
  }

  if (callback) {
    __CPP_REDIS_LOG(debug, "cpp_redis::subscriber executes ping reply callback");
    cpp_redis::reply replyCopy = reply;
    callback(replyCopy);
  }

  //{
  //  std::lock_guard<std::mutex> lock(m_ping_callbacks_mutex);
  //  m_callbacks_running -= 1;
  //  m_sync_condvar.notify_all();
  //} 
}

void
subscriber::clear_ping_callbacks() {
  if (m_ping_callbacks.empty()) {
    return;
  }

  /**
   * dequeue callbacks and move them to a local variable
   */
  std::queue<reply_callback_t> callbacks = std::move(m_ping_callbacks);

  //m_callbacks_running += __CPP_REDIS_LENGTH(callbacks.size());

  std::thread t([=]() mutable {
    while (!callbacks.empty()) {
      const auto& callback = callbacks.front();

      if (callback) {
        reply r = {"network failure", reply::string_type::error};
        callback(r);
      }

      //--m_callbacks_running;
      callbacks.pop();
    }

    //m_sync_condvar.notify_all();
  });
  t.detach();
}

void
subscriber::connection_receive_handler(network::redis_connection&, reply& reply) {
  __CPP_REDIS_LOG(info, "cpp_redis::subscriber received reply");

  //! always return an array
  //! otherwise, if auth was defined, this should be the AUTH reply
  //! any other replies from the server are considered as unexpected
  if (!reply.is_array()) {
    if (m_auth_reply_callback) {
      __CPP_REDIS_LOG(debug, "cpp_redis::subscriber executes auth callback");

      m_auth_reply_callback(reply);
      m_auth_reply_callback = nullptr;
    }
    else if (m_client_setname_reply_callback) {
      __CPP_REDIS_LOG(debug, "cpp_redis::subscriber executes client setname callback");

      m_client_setname_reply_callback(reply);
      m_client_setname_reply_callback = nullptr;
    }

    return;
  }

  auto& array = reply.as_array();

  //! Array size of 3 -> SUBSCRIBE if array[2] is a string
  //! Array size of 3 -> AKNOWLEDGEMENT if array[2] is an integer
  //! Array size of 4 -> PSUBSCRIBE
  //! Array size of 2 -> PING if array[0] is "pong"
  //! Otherwise -> unexpected reply
  if (array.size() == 3 && array[2].is_integer())
    handle_acknowledgement_reply(array);
  else if (array.size() == 3 && array[2].is_string())
    handle_subscribe_reply(array);
  else if (array.size() == 4)
    handle_psubscribe_reply(array);
  else if (array.size() == 2 && array[0].is_string() && array[0].as_string() == "pong")
    handle_ping_reply(reply);
}

void
subscriber::connection_disconnection_handler(network::redis_connection&) {
  //! leave right now if we are already dealing with reconnection
  if (is_reconnecting()) {
    return;
  }

  //! initiate reconnection process
  m_reconnecting               = true;
  m_current_reconnect_attempts = 0;

  __CPP_REDIS_LOG(warn, "cpp_redis::subscriber has been disconnected");

  if (m_connect_callback) {
    m_connect_callback(m_redis_server, m_redis_port, connect_state::dropped);
  }

  //! Lock the ping callbacks mutex to prevent more ping commands from beiung issued until our reconnect has completed.
  std::lock_guard<std::mutex> ping_lock_callback(m_ping_callbacks_mutex);
  clear_ping_callbacks();

  //! Lock the callbacks mutex of the base class to prevent more subscriber commands from being issued until our reconnect has completed.
  std::lock_guard<std::mutex> sub_lock_callback(m_subscribed_channels_mutex);
  std::lock_guard<std::mutex> psub_lock_callback(m_psubscribed_channels_mutex);

  while (should_reconnect()) {
    sleep_before_next_reconnect_attempt();
    reconnect();
  }

  if (!is_connected()) {
    clear_subscriptions();

    //! Tell the user we gave up!
    if (m_connect_callback) {
      m_connect_callback(m_redis_server, m_redis_port, connect_state::stopped);
    }
  }

  //! terminate reconnection
  m_reconnecting = false;
}

void
subscriber::clear_subscriptions() {
  m_subscribed_channels.clear();
  m_psubscribed_channels.clear();
}

void
subscriber::sleep_before_next_reconnect_attempt() {
  if (m_reconnect_interval_ms <= 0) {
    return;
  }

  if (m_connect_callback) {
    m_connect_callback(m_redis_server, m_redis_port, connect_state::sleeping);
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(m_reconnect_interval_ms));
}

bool
subscriber::should_reconnect() const {
  return !is_connected() && !m_cancel && (m_max_reconnects == -1 || m_current_reconnect_attempts < m_max_reconnects);
}

void
subscriber::reconnect() {
  //! increase the number of attempts to reconnect
  ++m_current_reconnect_attempts;

  //! We rely on the sentinel to tell us which redis server is currently the master.
  if (!m_master_name.empty() && !m_sentinel.get_master_addr_by_name(m_master_name, m_redis_server, m_redis_port, true)) {
    if (m_connect_callback) {
      m_connect_callback(m_redis_server, m_redis_port, connect_state::lookup_failed);
    }
    return;
  }

  //! Try catch block because the redis subscriber throws an error if connection cannot be made.
  try {
    connect(m_redis_server, m_redis_port, m_connect_callback, m_connect_timeout_ms, m_max_reconnects, m_reconnect_interval_ms, m_use_encryption);
  }
  catch (...) {
  }

  if (!is_connected()) {
    if (m_connect_callback) {
      m_connect_callback(m_redis_server, m_redis_port, connect_state::failed);
    }
    return;
  }

  //! notify end
  if (m_connect_callback) {
    m_connect_callback(m_redis_server, m_redis_port, connect_state::ok);
  }

  __CPP_REDIS_LOG(info, "client reconnected ok");

  re_auth();
  // This is the only window that the Redis server will let us send the CLIENT SETNAME
  // (i.e. between the re_auth and the re_subscriber).  So this needs to be done
  // by the subscriber as opposed to the application layer for reconnects.
  re_client_setname();
  re_subscribe();
  commit();
}

void
subscriber::re_subscribe() {
  std::map<std::string, callback_holder> sub_chans = std::move(m_subscribed_channels);
  for (const auto& chan : sub_chans) {
    unprotected_subscribe(chan.first, chan.second.subscribe_callback, chan.second.acknowledgement_callback);
  }

  std::map<std::string, callback_holder> psub_chans = std::move(m_psubscribed_channels);
  for (const auto& chan : psub_chans) {
    unprotected_psubscribe(chan.first, chan.second.subscribe_callback, chan.second.acknowledgement_callback);
  }
}


void
subscriber::re_auth() {
  if (m_password.empty()) {
    return;
  }

  auth(m_password, [&](cpp_redis::reply& reply) {
    if (reply.is_string() && reply.as_string() == "OK") {
      __CPP_REDIS_LOG(warn, "subscriber successfully re-authenticated");
    }
    else {
      __CPP_REDIS_LOG(warn, std::string("subscriber failed to re-authenticate: " + reply.as_string()).c_str());
    }
  });
}

void
subscriber::re_client_setname(void) {
  if (m_client_name.empty()) {
    return;
  }

  client_setname(m_client_name, [&](cpp_redis::reply& reply) {
    if (reply.is_string() && reply.as_string() == "OK") {
      __CPP_REDIS_LOG(warn, "subscriber successfully re-sent client setname");
    }
    else {
      __CPP_REDIS_LOG(warn, std::string("subscriber failed to re-send client setname: " + reply.as_string()).c_str());
    }
  });
}

} // namespace cpp_redis
