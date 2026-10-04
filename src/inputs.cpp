#include "inputs.hpp"

#include <pntos/cobra/transport/LcmConversions.hpp>
#include <pntos/cobra/transport/LcmLog.hpp>
#include <pntos/cobra/utils/aspn.hpp>
#include <pntos/cobra/utils/logging.hpp>

#include <chrono>
#include <iostream>

namespace edge {

using pntos::api::LoggingLevel;

// ----------------------------------------------------------------------------- LogInput

LogInput::LogInput(pntos::cobra::Filter& filter, const InputConfig& config, std::set<std::string> channels)
    : filter_(filter), cfg_(config), channels_(std::move(channels)) {}

void LogInput::start() {
  stop_ = false;
  thread_ = std::thread([this] { run(); });
}

void LogInput::stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
}

void LogInput::run() {
  do {
    pntos::cobra::lcm::LcmLogReader reader(cfg_.path);
    const auto wall0 = std::chrono::steady_clock::now();
    std::optional<std::int64_t> t0;
    while (!stop_) {
      auto ev = reader.next();
      if (!ev) break;
      if (!channels_.empty() && !channels_.count(ev->channel)) continue;
      std::shared_ptr<pntos::api::AspnBase> msg;
      try {
        msg = pntos::cobra::lcm::decode(ev->data);
      } catch (const std::exception&) {
        msg = nullptr;
      }
      if (!msg) {
        count_failure();
        continue;
      }
      if (cfg_.speed > 0) {
        if (!t0) t0 = ev->timestamp_us;
        std::this_thread::sleep_until(wall0 + std::chrono::microseconds(static_cast<std::int64_t>((ev->timestamp_us - *t0) / cfg_.speed)));
      }
      filter_.push(pntos::api::Message(msg, ev->channel));
      auto tov = pntos::cobra::utils::time_of_validity(*msg);
      count(ev->channel, tov ? tov->elapsed_nsec : 0);
    }
  } while (cfg_.loop && !stop_);
  finished_ = true;
}

// ----------------------------------------------------------------------------- LcmUdpInput

class LcmUdpInput::ForwardingMediator final : public pntos::api::Mediator {
 public:
  ForwardingMediator(LcmUdpInput& owner, pntos::cobra::Filter& filter) : owner_(owner), filter_(filter) {}
  std::vector<std::string> filter_description_list() const override { return {}; }
  std::optional<std::vector<std::optional<pntos::api::Message>>> request_solutions(const std::vector<pntos::api::Timestamp>& t,
                                                                                   const std::optional<std::string>&) override {
    std::vector<std::optional<pntos::api::Message>> out;
    for (auto ts : t) out.push_back(filter_.solution(ts));
    return out;
  }
  void process_pntos_message(const pntos::api::Message& m) override {
    filter_.push(m);
    auto tov = m.wrapped_message ? pntos::cobra::utils::time_of_validity(*m.wrapped_message) : std::nullopt;
    owner_.count(m.source_identifier, tov ? tov->elapsed_nsec : 0);
  }
  void broadcast_aspn_message(const pntos::api::Message&, const std::optional<std::string>&, const std::optional<std::string>&) override {}
  void log_message(LoggingLevel level, const std::string& message) override {
    pntos::cobra::utils::print_message(level, "LcmUdpInput", message);
  }
  pntos::api::Registry& registry() override { return filter_.registry(); }

 private:
  LcmUdpInput& owner_;
  pntos::cobra::Filter& filter_;
};

LcmUdpInput::LcmUdpInput(pntos::cobra::Filter& filter, const InputConfig& config) : filter_(filter), cfg_(config) {}

LcmUdpInput::~LcmUdpInput() { stop(); }

void LcmUdpInput::start() {
  mediator_ = std::make_unique<ForwardingMediator>(*this, filter_);
  // The transport reads its config from the registry: write it there first.
  pntos::cobra::LcmTransportConfig tc;
  tc.group_ = "config/pnt_edge_lcm_input";
  tc.url = cfg_.url;
  tc.subscribe_to = cfg_.subscribe_to;
  tc.to_registry(*mediator_);
  transport_ = std::make_unique<pntos::cobra::LcmUdpTransportPlugin>("pnt-edge LCM input", tc.group_);
  transport_->init_plugin(std::nullopt, mediator_.get());
  transport_->start_listening();
}

void LcmUdpInput::stop() {
  if (transport_) {
    transport_->stop_listening();
    transport_.reset();
  }
}

std::unique_ptr<Input> make_input(pntos::cobra::Filter& filter, const InputConfig& config, std::set<std::string> default_channels) {
  if (config.type == "lcm_udp") return std::make_unique<LcmUdpInput>(filter, config);
  std::set<std::string> channels(config.channels.begin(), config.channels.end());
  if (channels.empty()) channels = std::move(default_channels);
  return std::make_unique<LogInput>(filter, config, std::move(channels));
}

}  // namespace edge
