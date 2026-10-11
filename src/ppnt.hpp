// $PPNT command protocol (docs/COMMAND_PROTOCOL.md): parsing of incoming sentences and the handler that answers
// queries from the hub's state and queues SET commands to the main loop, acknowledging every sentence.
#pragma once

#include "web/hub.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace edge::ppnt {

struct Sentence {
  std::string command;              ///< STATUS, VER, OUT, ...
  std::string kind;                 ///< Q (query), S (set), R (reply)
  std::vector<std::string> fields;  ///< the fields after the kind
};

/// Parses one line (with or without CRLF). nullopt with `error` set on bad framing, checksum or length.
std::optional<Sentence> parse(const std::string& line, std::string* error = nullptr);

/// What the handler needs from the daemon.
struct Context {
  Hub* hub = nullptr;
  std::string version;                              ///< pnt-edge version
  std::string cobra_version;
  std::function<std::string()> config_id;           ///< SHA-256 prefix of the active filter config
  std::function<std::string()> time_source;         ///< gnss | ptp | ntp | free
};

/// Answers one incoming line from `from` (an address for the audit log): zero or more sentences to send back,
/// always ending with an ACK (or an ACK alone for a malformed line).
std::vector<std::string> handle(const std::string& line, const std::string& from, const Context& ctx);

std::string ack(const std::string& command, const std::string& result, const std::string& message = "");

}  // namespace edge::ppnt
