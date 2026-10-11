// Serial devices as a measurement input: one reader thread per device (VN-100 IMU, u-blox UBX or NMEA GNSS)
// decoding frames into ASPN messages, a shared reorder buffer that releases them to the filter in time order,
// per-device health for the status model, optional raw recording. Devices reconnect on their own when the port
// disappears (unplug) and reappears.
#pragma once

#include "../inputs.hpp"
#include "clock.hpp"
#include "nmea_gnss.hpp"
#include "port.hpp"
#include "ubx.hpp"
#include "vn100.hpp"

#include <condition_variable>
#include <deque>
#include <fstream>
#include <queue>

namespace edge {

class SerialInput final : public Input {
 public:
  SerialInput(pntos::cobra::Filter& filter, const InputConfig& config);
  ~SerialInput() override;
  void start() override;
  void stop() override;
  bool finished() const override { return false; }
  nlohmann::json devices() const override;
  std::optional<std::int64_t> utc_correction_ns() const override;

  /// Builds the ASPN messages a device would push for decoded content; exposed for the tests.
  static std::shared_ptr<pntos::api::AspnBase> imu_message(std::int64_t tov_ns, const serial::vn100::Sample& s);
  static std::shared_ptr<pntos::api::AspnBase> position_message(std::int64_t tov_ns, double lat_rad, double lon_rad, double alt_m, double sigma_n, double sigma_e,
                                                                double sigma_d);
  static std::shared_ptr<pntos::api::AspnBase> velocity_message(std::int64_t tov_ns, double vn, double ve, double vd, double sigma_h, double sigma_v);

 private:
  struct Device {
    SerialDeviceConfig cfg;
    std::thread thread;
    serial::Port port;
    serial::ClockMapper mapper;       ///< device clock -> host timeline
    serial::ClockMapper utc_mapper;   ///< receiver UTC -> host timeline (host - UTC)
    std::ofstream record;
    // health, guarded by the input's mutex_
    std::string state = "closed";     ///< closed | opening | open | streaming | no data | lost
    std::string detail;
    std::uint64_t bytes = 0, frames = 0, errors = 0;
    std::int64_t last_frame_host_ns = 0;
    double rate_hz = 0;
    std::uint64_t window_frames = 0;
    std::int64_t window_start_ns = 0;
    std::optional<int> leap_seconds;
    std::optional<std::int64_t> clock_offset_ns;
    std::optional<std::int64_t> utc_correction_ns;
  };
  struct Pending {
    std::int64_t tov_ns, arrival_ns;
    std::shared_ptr<pntos::api::AspnBase> message;
    std::string channel;
    bool operator>(const Pending& o) const { return tov_ns > o.tov_ns; }
  };

  void run_device(Device& d);
  void handle_bytes(Device& d, const std::uint8_t* data, std::size_t n, std::int64_t arrival_ns);
  std::int64_t stamp(Device& d, std::optional<std::int64_t> absolute_ns, std::optional<std::int64_t> relative_ns, std::int64_t arrival_ns);
  void enqueue(const Device& d, std::int64_t tov_ns, std::int64_t arrival_ns, std::shared_ptr<pntos::api::AspnBase> m, const std::string& channel);
  void pusher();
  void note_frame(Device& d, std::int64_t arrival_ns);

  pntos::cobra::Filter& filter_;
  InputConfig cfg_;
  std::vector<std::unique_ptr<Device>> devices_;
  // per-device parsers live on the device threads
  std::thread pusher_;
  std::priority_queue<Pending, std::vector<Pending>, std::greater<Pending>> queue_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::atomic<bool> stop_{false};
};

}  // namespace edge
