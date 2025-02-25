#include <chrono>
#include <thread>

#include <QEventLoop>

#include "catch2/catch.hpp"
#include "common/util.h"
#include "tools/replay/replay.h"
#include "tools/replay/util.h"

const std::string TEST_RLOG_URL = "https://commadataci.blob.core.windows.net/openpilotci/0c94aa1e1296d7c6/2021-05-05--19-48-37/0/rlog.bz2";
const std::string TEST_RLOG_CHECKSUM = "5b966d4bb21a100a8c4e59195faeb741b975ccbe268211765efd1763d892bfb3";

const int TEST_REPLAY_SEGMENTS = std::getenv("TEST_REPLAY_SEGMENTS") ? atoi(std::getenv("TEST_REPLAY_SEGMENTS")) : 1;

bool download_to_file(const std::string &url, const std::string &local_file, int chunk_size = 5 * 1024 * 1024, int retries = 3) {
  do {
    if (httpDownload(url, local_file, chunk_size)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  } while (--retries >= 0);
  return false;
}

TEST_CASE("LogReader") {
  SECTION("corrupt log") {
    FileReader reader(true);
    std::string corrupt_content = reader.read(TEST_RLOG_URL);
    corrupt_content.resize(corrupt_content.length() / 2);
    corrupt_content = decompressBZ2(corrupt_content);
    LogReader log;
    REQUIRE(log.load(corrupt_content.data(), corrupt_content.size()));
    REQUIRE(log.events.size() > 0);
  }
}

void read_segment(int n, const SegmentFile &segment_file, uint32_t flags) {
  std::mutex mutex;
  std::condition_variable cv;
  Segment segment(n, segment_file, flags, {}, [&](int, bool) {
    REQUIRE(segment.isLoaded() == true);
    REQUIRE(segment.log != nullptr);
    REQUIRE(segment.frames[RoadCam] != nullptr);
    if (flags & REPLAY_FLAG_DCAM) {
      REQUIRE(segment.frames[DriverCam] != nullptr);
    }
    if (flags & REPLAY_FLAG_ECAM) {
      REQUIRE(segment.frames[WideRoadCam] != nullptr);
    }

    // test LogReader & FrameReader
    REQUIRE(segment.log->events.size() > 0);
    REQUIRE(std::is_sorted(segment.log->events.begin(), segment.log->events.end()));

    for (auto cam : ALL_CAMERAS) {
      auto &fr = segment.frames[cam];
      if (!fr) continue;

      if (cam == RoadCam || cam == WideRoadCam) {
        REQUIRE(fr->getFrameCount() == 1200);
      }
      auto [nv12_width, nv12_height, nv12_buffer_size] = get_nv12_info(fr->width, fr->height);
      VisionBuf buf;
      buf.allocate(nv12_buffer_size);
      buf.init_yuv(fr->width, fr->height, nv12_width, nv12_width * nv12_height);
      // sequence get 100 frames
      for (int i = 0; i < 100; ++i) {
        REQUIRE(fr->get(i, &buf));
      }
    }
    cv.notify_one();
  });

  std::unique_lock lock(mutex);
  cv.wait(lock);
}

std::string download_demo_route() {
  static std::string data_dir;

  if (data_dir == "") {
    char tmp_path[] = "/tmp/root_XXXXXX";
    data_dir = mkdtemp(tmp_path);

    Route remote_route(DEMO_ROUTE);
    assert(remote_route.load());

    // Create a local route from remote for testing
    const std::string route_name = std::string(DEMO_ROUTE).substr(17);
    for (int i = 0; i < 2; ++i) {
      std::string log_path = util::string_format("%s/%s--%d/", data_dir.c_str(), route_name.c_str(), i);
      util::create_directories(log_path, 0755);
      REQUIRE(download_to_file(remote_route.at(i).rlog, log_path + "rlog.bz2"));
      REQUIRE(download_to_file(remote_route.at(i).qcamera, log_path + "qcamera.ts"));
    }
  }

  return data_dir;
}
