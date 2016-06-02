// Copyright 2016 Peter Jankuliak
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <boost/uuid/uuid_generators.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/adaptor/indirected.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include "club/hub.h"
#include "node.h"
#include "binary/encoder.h"
#include "binary/dynamic_encoder.h"
#include "binary/uuid.h"
#include "serialize/list.h"
#include "message.h"
#include "net/async_exchange.h"
#include "net/protocol_versions.h"
#include "generic/erase_if.h"
#include "stun-client.h"
#include <chrono>
#include <iostream>
#include "get_external_port.h"
#include "connection_graph.h"
#include "dijkstra.h"

using namespace net;
using namespace club;

using std::shared_ptr;
using std::make_shared;
using std::make_pair;
using std::pair;
using std::vector;
using std::move;
using std::set;

using boost::asio::ip::udp;
using boost::adaptors::map_values;
using boost::adaptors::map_keys;
using boost::adaptors::indirected;
using boost::adaptors::reversed;
using boost::system::error_code;

namespace ip = boost::asio::ip;

#include "debug/log.h"

#define USE_LOG 0

#if USE_LOG
#  define IF_USE_LOG(a) a
#  define LOG(...)  log("CLUB: ", _id, " ", __VA_ARGS__)
#  define LOG_(...) log("CLUB: ", _id, " ", __VA_ARGS__)
#else
#  define IF_USE_LOG(a)
#  define LOG(...) do {} while(0)
#  define LOG_(...) log("CLUB: ", _id, " ", __VA_ARGS__)
#endif

// -----------------------------------------------------------------------------
template<class Message>
shared_ptr<vector<char>> encode_message(const Message& msg) {
  binary::dynamic_encoder<char> e;
  e.put(Message::type());
  e.put(msg);
  return make_shared<vector<char>>(e.move_data());
}

shared_ptr<vector<char>> encode_message(const LogMessage& msg) {
  return match( msg
              , [](const Fuse& m)           { return encode_message(m); }
              , [](const PortOffer& m)      { return encode_message(m); }
              , [](const UserData& m)       { return encode_message(m); });
}

// -----------------------------------------------------------------------------
hub::hub(boost::asio::io_service& ios)
  : _io_service(ios)
  , _work(new Work(_io_service))
  , _id(boost::uuids::random_generator()())
  , _log(_id)
  , _time_stamp(0)
  , _was_destroyed(make_shared<bool>(false))
{
  LOG("Created");
  _nodes[_id] = std::unique_ptr<Node>(new Node(this, _id));
  _last_quorum.insert(_id);
  _log.last_commit_op = _id;
  _configs.insert(MessageId(_time_stamp, _id));
}

// -----------------------------------------------------------------------------
void hub::fuse(Socket&& xsocket, const OnFused& on_fused) {
  using boost::asio::buffer;
  using namespace boost::asio::error;

  auto socket = make_shared<Socket>(move(xsocket));

  static const size_t buffer_size = sizeof(NET_PROTOCOL_VERSION) + sizeof(_id);

  binary::dynamic_encoder<char> e(buffer_size);
  e.put(NET_PROTOCOL_VERSION);
  e.put(_id);

  auto tx_bytes = make_shared<Bytes>(e.move_data());
  auto was_destroyed = _was_destroyed;

  LOG("fusing ", _nodes | map_keys);

  async_exchange(*socket, buffer(*tx_bytes), 10000,
      [this, tx_bytes, socket, on_fused, was_destroyed]
      (error_code error, Bytes&& rx_bytes) {

        if (*was_destroyed) return;

        auto fusion_failed = [&](error_code error, const char* /*msg*/) {
          socket->close();
          on_fused(error, uuid());
        };

        if (error) return fusion_failed(error, "socket error");

        binary::decoder d(reinterpret_cast<uint8_t*>(rx_bytes.data()), rx_bytes.size());
        auto his_protocol_version = d.get<decltype(NET_PROTOCOL_VERSION)>();
        auto his_id               = d.get<uuid>();

        if (d.error()) {
          return fusion_failed(connection_refused, "invalid data");
        }

        if (his_protocol_version != NET_PROTOCOL_VERSION) {
          return fusion_failed(no_protocol_option, "protocol michmatch");
        }

        ASSERT(_id != his_id);

        if (_id == his_id) {
          return fusion_failed(already_connected, "sender is myself");
        }

        LOG(socket->local_endpoint().port(), " Exchanged ID with ", his_id);

        auto n = find_node(his_id);

        if (n) {
          n->assign_socket(socket);
        }
        else {
          n = &insert_node(his_id, socket);
        }

        auto sync = construct<Sync>(his_id);
        n->send(encode_message(sync));

        LOG("fused with ", his_id, " since ");

        add_connection(this_node(), his_id, n->address());

        if (destroys_this([&]() { on_fused(error_code(), his_id); })) {
          return;
        }

        commit_what_was_seen_by_everyone();
      });
}

// -----------------------------------------------------------------------------
void hub::total_order_broadcast(Bytes data) {
  auto msg = make_shared<UserData>(construct_ackable<UserData>(move(data)));

  add_log_entry(*msg, [=](const LogEntry&) {
        on_receive(hub::node{_id}, msg->data);
        });

  auto was_destroyed = _was_destroyed;

  _io_service.post([=]() {
      if (*was_destroyed) return;
      commit_what_was_seen_by_everyone();
      });

  broadcast(*msg);
}

// -----------------------------------------------------------------------------
void hub::add_connection(Node& from, uuid to, ip::address addr) {
  ASSERT(from.peers.count(to) == 0);
  from.peers[to] = Node::Peer({addr});
}

// -----------------------------------------------------------------------------
void hub::on_peer_connected(const Node& node) {
  // TODO
}

// -----------------------------------------------------------------------------
void hub::on_peer_disconnected(const Node& node, std::string reason) {
  auto fuse_msg = construct_ackable<Fuse>(node.id);
  add_log_entry(fuse_msg, [=](const LogEntry& entry) { on_commit_fuse(fuse_msg, entry); });
  broadcast(fuse_msg);
  commit_what_was_seen_by_everyone();
}

// -----------------------------------------------------------------------------
void hub::process(Node&, Ack const& msg) {
  _log.apply_ack(original_poster(msg), msg.data);
}

// -----------------------------------------------------------------------------
void hub::process(Node& op, const Sync& msg) {
  auto fuse_msg = construct_ackable<Fuse>(original_poster(msg));
  
  broadcast(fuse_msg);
  
  add_log_entry(fuse_msg, [=](const LogEntry& entry) {
      on_commit_fuse(fuse_msg, entry);
      });
}

// -----------------------------------------------------------------------------
void hub::process(Node& op, const Fuse& msg) {
  ASSERT(original_poster(msg) != _id);

  add_log_entry(msg, [=](const LogEntry& entry) { on_commit_fuse(msg, entry); });
  _log.apply_ack(original_poster(msg), msg.ack_data);

  auto fuse_entry = _log.find_highest_fuse_entry();

  if (fuse_entry) {
    if (message_id(msg) >= message_id(fuse_entry->message)) {
      broadcast(construct_ack(msg));
      commit_what_was_seen_by_everyone();
    }
  }
  else {
    broadcast(construct_ack(msg));
    commit_what_was_seen_by_everyone();
  }
}

// -----------------------------------------------------------------------------
void hub::process(Node& op, const PortOffer& msg) {
  if (msg.addressor != _id) { return; }
  LOG("Got port offer: ", msg);
  op.set_remote_port( msg.internal_port
                    , msg.external_port);
}

// -----------------------------------------------------------------------------
void hub::process(Node&, const UserData& msg) {
  add_log_entry(msg, [=](const LogEntry&) {
        // Node might have been removed, we don't want to commit that.
        if (!find_node(original_poster(msg))) return;
        on_receive(hub::node{original_poster(msg)}, msg.data);
        });

  _log.apply_ack(original_poster(msg), msg.ack_data);
}

// -----------------------------------------------------------------------------
Graph<uuid> acks_to_graph(const std::map<uuid, AckData>& acks) {
  Graph<uuid> g;

  for (const auto& pair : acks) {
    g.nodes.insert(pair.first);

    for (const auto& peer : pair.second.local_quorum) {
      g.add_edge(pair.first, peer);
    }
  }

  return g;
}

// -----------------------------------------------------------------------------
void hub::recalculate_routers(Graph<uuid>&& graph) {
  Dijkstra dijkstra(_id, move(graph));
  dijkstra.run();

  for (auto node_id : dijkstra.graph.nodes) {
    if (auto node = find_node(node_id)) {
      node->router = dijkstra.first_node_to(node_id);
    }
  }
}

// -----------------------------------------------------------------------------
void hub::on_commit_fuse(const Fuse& msg, const LogEntry& entry) {
  if (!entry.acked_by_quorum()) return;

  set<hub::node> new_ones;

  auto new_graph = acks_to_graph(entry.acks);
  LOG("Commit config: ", entry.message_id(), ": ", new_graph);
  recalculate_routers(move(new_graph));

  _configs.insert(entry.message_id());


  for (auto id : entry.quorum) {
    auto n = find_node(id);

    ASSERT(n);

    if (!n->user_notified) {
      n->user_notified = true;
      ASSERT(id != _id);
      new_ones.insert(id);
    }
  }

  if (!new_ones.empty()) {
    if (destroys_this([&]() { on_insert(move(new_ones)); })) {
      return;
    }
  }

  if (!entry.lost.empty()) {
    set<hub::node> lost;

    for (auto id : entry.lost) {
      lost.insert(hub::node{id});
      _nodes.erase(id);
    }

    if (destroys_this([&]() { on_remove(move(lost)); })) {
      return;
    }
  }
}

// -----------------------------------------------------------------------------
template<class Message> void hub::on_recv(Node& IF_USE_LOG(proxy), Message& msg) {
#if USE_LOG
# define ON_RECV_LOG(...) \
   if (true || Message::type() == port_offer) { \
     LOG("Recv(", proxy.id, "): ", __VA_ARGS__); \
   }
#else
# define ON_RECV_LOG(...) do {} while(0)
#endif // if USE_LOG

  msg.header.visited.insert(_id);

  auto op_id = original_poster(msg);
  auto op = find_node(op_id);

  if (_seen.count(message_id(msg))) {
    ON_RECV_LOG(msg, " (ignored: already seen ", message_id(msg), ")");
    return;
  }

  _seen.insert(message_id(msg));

  _time_stamp = std::max(_time_stamp, msg.header.time_stamp);

  if (!op) {
    ON_RECV_LOG(msg, " (unknown node: creating one)");
    op = &insert_node(op_id);
  }

  // Peers shouldn't broadcast to us back our own messages.
  // This is achieved by the clock synchronization below.
  ASSERT(op_id != _id);

  if (op_id == _id) {
    ON_RECV_LOG(msg, " (ignored: is our own message)");
    return;
  }

  ON_RECV_LOG(msg);

  // Clock synchronization must happen before the broadcast for us to
  // not send it back to the sender.
  //_clock |= time_created(msg);

  // Sync messages are direct between two peers.
  if (Message::type() != sync) {
    // This re-broadcat must happen before we send the ACK so that the
    // receiver always knows what is being acked.
    broadcast(msg);
  }

  if (Message::always_ack()) {
    broadcast(construct_ack(msg));
  }

  if (destroys_this([&]() { process(*op, msg); })) {
    return;
  }

  commit_what_was_seen_by_everyone();
}

// -----------------------------------------------------------------------------
template<class Message>
void hub::parse_message(Node& proxy, binary::decoder& decoder) {
  auto msg = decoder.get<Message>();
  if (decoder.error()) return;
  on_recv<Message>(proxy, msg);
}

// -----------------------------------------------------------------------------
void hub::on_recv_raw(Node& proxy, const Bytes& data) {
  binary::decoder decoder(data.data(), data.size());

  auto msg_type = decoder.get<MessageType>();

  switch (msg_type) {
    case ::club::fuse:    parse_message<Fuse>          (proxy, decoder); break;
    case sync:            parse_message<Sync>          (proxy, decoder); break;
    case port_offer:      parse_message<PortOffer>     (proxy, decoder); break;
    case user_data:       parse_message<UserData>      (proxy, decoder); break;
    case ack:             parse_message<Ack>           (proxy, decoder); break;
    default:              decoder.set_error();
  }

  if (decoder.error()) {
    ASSERT(0 && "Error parsing message");
    proxy.disconnect();
  }
}

// -----------------------------------------------------------------------------
void hub::commit_what_was_seen_by_everyone() {
  const LogEntry* last_committed_fuse = nullptr;

  for (auto& e : _log | reversed | map_values) {
    if (e.message_type() == ::club::fuse && e.acked_by_quorum()) {
      last_committed_fuse = &e;
      for (auto id : _last_quorum) {
        if (e.quorum.count(id) == 0) {
          const_cast<LogEntry&>(e).lost.insert(id);
        }
      }
      _last_quorum = e.quorum;
      break;
    }
  }

#if USE_LOG
  {
    LOG("Checking what can be commited");
    LOG("    Last committed: ", str(_log.last_committed));
    LOG("    Last committed fuse: ", str(_log.last_fuse_commit));
    LOG("    Last quorum: ", str(_last_quorum));
    LOG("    Entries:");
    for (const auto& e : _log | map_values) {
      LOG("      ", e);
    }
  }
#endif

  auto entry_j = _log.begin();

  auto was_destroyed = _was_destroyed;


  for (auto entry_i = entry_j; entry_i != _log.end(); entry_i = entry_j) {
    entry_j = next(entry_i);

    auto& entry = entry_i->second;

    //------------------------------------------------------
    bool passable = false;

    if (entry.message_type() == ::club::fuse) {
      passable = last_committed_fuse
              && entry.message_id() <= last_committed_fuse->message_id();
    }
    else {
      passable = entry.acked_by_quorum(_last_quorum);
    }

    if (!passable) break;

    //------------------------------------------------------
    if (!entry.predecessors.empty()) {
      auto i = entry.predecessors.rbegin();

      for (; i != entry.predecessors.rend(); ++i) {
        if (i->first == _log.last_committed) break;
        if (_configs.count(config_id(entry.message)) == 0) continue;
        //if (_log.excluded_predecessors.count(i->first)) continue;
        break;
      }

      if (i != entry.predecessors.rend()) {
        LOG("    Predecessor: ", str(*i));
        if (i->first != _log.last_committed && i->first > _log.last_fuse_commit) {
          //if (!(i->first >>= _log.last_committed)) {
            LOG("    entry.predecessor != _log.last_committed "
               , i->first, " != ", _log.last_committed);
            break;
          //}
        }
      }
    }

    //------------------------------------------------------
    if (&entry_i->second == last_committed_fuse) {
      last_committed_fuse = nullptr;
    }

    LOG("    Committing: ", entry);
    auto e = move(entry_i->second);
    _log.erase(entry_i);

    if (e.message_type() == ::club::fuse) {
      _log.last_fuse_commit = message_id(e.message);
    }

    _log.last_committed = message_id(e.message);
    _log.last_commit_op = original_poster(e.message);

    e.on_commit(e);

    if (*was_destroyed) return;
  }
}

// -----------------------------------------------------------------------------
hub::~hub() {
  _work.reset();
  *_was_destroyed = true;
}

// -----------------------------------------------------------------------------
template<class Message, class OnCommit /* void (const LogEntry&) */>
void hub::add_log_entry(const Message& message, OnCommit&& on_commit) {
  LOG("Adding entry for message: ", message);

  if(message_id(message) <= _log.last_committed) {
    if (Message::type() != ::club::fuse) {
      LOG("!!! message_id(message) should be > than _log.last_committed");
      LOG("!!! message_id(message) = ", message_id(message));
      LOG("!!! _log.last_committed   = ", _log.last_committed);
      ASSERT(0);
    }
  }

  _log.insert_entry(LogEntry(message, move(on_commit)));
}

//------------------------------------------------------------------------------
template<class Message, class... Args>
Message hub::construct(Args&&... args) {
  ASSERT(!_configs.empty());
  // TODO: The _id argument in `visited` member is redundant.
  return Message( Header{ _id
                        , ++_time_stamp
                        , *_configs.rbegin()
                        , boost::container::flat_set<uuid>{_id}
                        }
                , std::forward<Args>(args)...);
}

//------------------------------------------------------------------------------
template<class Message, class... Args>
Message hub::construct_ackable(Args&&... args) {
  ASSERT(!_configs.empty());

  ++_time_stamp;

  auto m_id = MessageId(_time_stamp, _id);

  const auto& predecessor_id = _log.get_predecessor_time(m_id);

  AckData ack_data { move(m_id)
                   , move(predecessor_id)
                   , local_quorum() };

  // We don't receive our own message back, so need to apply it manually.
  _log.apply_ack(_id, ack_data);

  // TODO: m_id here is redundant, can be calculated from header.
  // TODO: The _id argument in `visited` member is redundant.
  return Message( Header{ _id
                        , _time_stamp
                        , *_configs.rbegin()
                        , boost::container::flat_set<uuid>{_id}
                        }
                , move(ack_data)
                , std::forward<Args>(args)...);
}

// -----------------------------------------------------------------------------
template<class Msg>
Ack hub::construct_ack(const Msg& msg) {
  const auto& predecessor_id = _log.get_predecessor_time(message_id(msg));

  auto ack = construct<Ack>
             ( message_id(msg)
             , predecessor_id
             , local_quorum());

  // We don't receive our own message back, so need to apply it manually.
  _log.apply_ack(_id, ack.data);
  return ack;
}

//------------------------------------------------------------------------------
template<class Message> void hub::broadcast(const Message& msg) {
  LOG("Broadcasting: ", msg);

  auto data = encode_message(msg);

  for (auto& node : _nodes | map_values | indirected) {
    if (node.id == _id) continue;
    if (!node.is_connected()) {
      LOG("  skipped: ", node.id, " (not connected)");
      continue;
    }

    bool already_visited = msg.header.visited.count(node.id) != 0;

    if (already_visited) {
      continue;
    }

    // If we're sending a message to the OP there is probably something
    // wrong with his clock.
    ASSERT(original_poster(msg) != node.id &&
           "Why are we sending the message back?");

    node.send(data);
  }
}

// -----------------------------------------------------------------------------
boost::container::flat_set<uuid> hub::local_quorum() const {
  size_t size = 1;

  for (auto& node : _nodes | map_values | indirected) {
    if (node.id == _id) continue;
    if (node.is_connected()) {
      ++size;
    }
  }

  boost::container::flat_set<uuid> lc;
  lc.reserve(size);
  lc.insert(_id);

  for (auto& node : _nodes | map_values | indirected) {
    if (node.id == _id) continue;
    if (node.is_connected()) {
      lc.insert(node.id);
    }
  }

  return lc;
}

// -----------------------------------------------------------------------------
void hub::broadcast_port_offer_to(Node& node, Address addr) {
  auto was_destroyed = _was_destroyed;

  auto node_id = node.id;

  if (addr.is_loopback()) {
    // If the remote address in on our PC, then there is no need
    // to send him our external address.
    // TODO: Remove code duplication.
    // TODO: Similar optimization when the node is on local LAN.
    _io_service.post([=]{
          if (*was_destroyed) return;

          auto node = find_node(node_id);

          if (!node || node->is_connected()) return;

          udp::socket udp_socket(_io_service, udp::endpoint(udp::v4(), 0));
          uint16_t internal_port = udp_socket.local_endpoint().port();
          uint16_t external_port = 0;

          auto socket = make_shared<Socket>(std::move(udp_socket));
          node->set_remote_address(move(socket), addr);

          broadcast(construct<PortOffer>( node_id
                                        , internal_port
                                        , external_port));
        });
    return;
  }

  _stun_requests.emplace_back(nullptr);
  auto iter = std::prev(_stun_requests.end());

  iter->reset(new GetExternalPort(_io_service
                                 , std::chrono::seconds(2)
                                 , [=] ( error_code error
                                       , udp::socket udp_socket
                                       , Endpoint reflexive_ep) {
          if (*was_destroyed) return;

          _stun_requests.erase(iter);

          auto node = find_node(node_id);

          if (!node || node->is_connected()) return;

          uint16_t internal_port = udp_socket.local_endpoint().port();
          uint16_t external_port = reflexive_ep.port();

          auto socket = make_shared<Socket>(std::move(udp_socket));
          node->set_remote_address(move(socket), addr);

          broadcast(construct<PortOffer>( node_id
                                        , internal_port
                                        , external_port));
        }));
}

// -----------------------------------------------------------------------------
template<class F>
bool hub::destroys_this(F f) {
  auto was_destroyed = _was_destroyed;
  f();
  return *was_destroyed;
}

// -----------------------------------------------------------------------------
hub::Address hub::find_address_to(uuid id) const {
  ConnectionGraph g;

  for (const auto& node : _nodes | map_values | indirected) {
    if (node.id == _id) continue;
    auto addr = node.address();
    if (!addr.is_unspecified()) {
      g.add_connection(_id, node.id, addr);
    }

    for (const auto& peer_id_info : node.peers) {
      auto peer_id   = peer_id_info.first;
      auto peer_addr = peer_id_info.second.address;
      g.add_connection(node.id, peer_id, peer_addr);
    }
  }

  return g.find_address(_id, id);
}

// -----------------------------------------------------------------------------
inline Node& hub::this_node() {
  // TODO: We can store this node instead of searching the rb-tree
  //       each time.
  return *_nodes[_id];
}

// -----------------------------------------------------------------------------
inline
Node& hub::insert_node(uuid id) {
  auto node = std::unique_ptr<Node>(new Node(this, id));
  auto ret  = node.get();

  _nodes.insert(std::make_pair(id, move(node)));

  return *ret;
}

// -----------------------------------------------------------------------------
inline
Node& hub::insert_node(uuid id, shared_ptr<Socket> socket) {
  auto node = std::unique_ptr<Node>(new Node(this, id, move(socket)));
  auto ret  = node.get();

  _nodes.insert(std::make_pair(id, move(node)));

  return *ret;
}

// -----------------------------------------------------------------------------
inline
Node* hub::find_node(uuid id) {
  auto i = _nodes.find(id);
  if (i == _nodes.end()) return nullptr;
  return i->second.get();
}

inline
const Node* hub::find_node(uuid id) const {
  auto i = _nodes.find(id);
  if (i == _nodes.end()) return nullptr;
  return i->second.get();
}

// -----------------------------------------------------------------------------