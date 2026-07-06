#include <iostream>
#include <uvgrtp/lib.hh>
#include <httplib.h>
#include <nlohmann/json.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

constexpr char SVC_ADDRESS[] = "dwe-jetson-11.local"; // Replace this with the actual address of your SVC
constexpr uint16_t HTTP_PORT = 47001;                 // Hardcoded HTTP port (do not change)
constexpr uint16_t RTP_PORT = 47002;                  // Default RTP port (can be anything, just make sure it's allowed by firewall and
                                                      // doesn't clash with other programs)
constexpr int IMG_W = 800;
constexpr int IMG_H = 600;
constexpr int NTP_HEADER_SIZE = sizeof(int) * 3;      // Network protocol header size

constexpr int DISP_SIZE = IMG_W * IMG_H * 2;          // Single channel 16-bit IEEE floats (disparity)
constexpr int IMG_SIZE = IMG_W * IMG_H * 3;           // 3-channel 8-bit-per-channel RGB image (rectified and undistorted)
constexpr size_t BUF_SIZE = IMG_SIZE + DISP_SIZE;     // Left image + Left disparity

std::atomic<bool> should_stay_connected{true};
std::atomic<bool> should_poll{true};
using namespace nlohmann;

/**
 * Sets up an HTTP event stream on the /health endpoint of the RTPServer. This is used to keep track of client connection
 * status via ping-pong packets so that the server can shut down the RTP stream upon disconnect. Some other data is also
 * shared such as DWVO recording length.
 */
void subscribe(const std::string &addr) {
    auto client = httplib::Client(addr);
    auto res = client.Get("/health", [&](const char *data, size_t len) {
        try {
            json body = json::parse(std::string(data, len));
            // Uncomment this line to view all received event stream data:
            // std::cout << body.dump(2) << std::endl;
        } catch (const std::exception &e) {
            std::cerr << "Failed to parse health response: " << e.what() << std::endl;
        }

        // HTTPLIB API: Returns true to keep the event stream open, false to close it.
        return should_stay_connected.load();
    });

    if (!res) {
        std::cerr << "Connection requested timed out." << std::endl;
    } else if (res->status != 200) {
        std::cerr << "Failed to do GET request at /health: status " << res->status << std::endl;
    }
}


int main() {
    std::signal(SIGINT, [](int) {
        should_stay_connected = false;
        should_poll = false;
    });
    std::signal(SIGTERM, [](int) {
        should_stay_connected = false;
        should_poll = false;
    });

    // Set up uvgRTP receiver
    uvgrtp::context ctx;
    uvgrtp::session *sess = ctx.create_session("0.0.0.0");
    uvgrtp::media_stream *receiver = sess->create_stream(RTP_PORT, RTP_FORMAT_GENERIC,
                                                         RCE_RECEIVE_ONLY | RCE_FRAGMENT_GENERIC);

    // Set up HTTP client
    std::string addr = std::string(SVC_ADDRESS) + ":" + std::to_string(HTTP_PORT);
    auto client = httplib::Client(addr);

    // Register this address as a client in the remote server
    std::thread subscribe_thread(subscribe, addr);

    json params;
    params["calibration"]["filename"] = "e3d2001.dwecal";
    params["network_protocol"] = "DEPTH_ONLY";
    params["depth_mode"]["frame_width"] = IMG_W;
    params["depth_mode"]["frame_height"] = IMG_H;
    params["depth_mode"]["name"] = "SGM";
    params["input_dwvo"] = "2001.dwvo";
    params["rtp_port"] = RTP_PORT;
    params["resolution"] = "UXGA";
    params["input_type"] = "DWVO";
    params["swap_inputs"] = false;
    params["fps"] = 30;

    // POST to RTPSender /start endpoint. This starts the RTP stream with given parameters.
    auto res = client.Post("/start", params.dump(), "application/json");
    if (!res) {
        std::cerr << "Connection refused. Is the RTPSender process running on the target SVC?" << std::endl;
        goto cleanup;
    }
    if (res->status != 200) {
        std::cerr << "POST error: " << res->body << std::endl;
        return 1;
    }

    // Main application loop
    std::cout << "Waiting for incoming packets." << std::endl;
    while (should_poll) {
        auto frm = receiver->pull_frame(5000);
        size_t expected_size = NTP_HEADER_SIZE + BUF_SIZE;
        if (frm->payload_len != expected_size) {
            std::cerr << "Received invalid frame of size " << frm->payload_len << ", expected " << expected_size <<
                    std::endl;
        } else {
            if (stbi_write_png("left.png", IMG_W, IMG_H, 3, frm->payload + NTP_HEADER_SIZE, IMG_W * 3) == 0) {
                std::cerr << "Failed to save image." << std::endl;
                return 1;
            }
            std::cout << "Wrote to png." << std::endl;
        }
        (void) uvgrtp::frame::dealloc_frame(frm);
    }
cleanup:
    std::cout << "Cleaning up resources... ";
    subscribe_thread.join();
    std::cout << " done." << std::endl;
    return 0;
}
