#include <condition_variable>
#include <cstring>
#include <iostream>
#include <uvgrtp/lib.hh>
#include <httplib.h>
#include <nlohmann/json.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

constexpr char SVC_ADDRESS[] = "dwe-jetson-11.local"; // Replace this with the actual address of your SVC
constexpr uint16_t HTTP_PORT = 47001; // Hardcoded HTTP port
constexpr uint16_t RTP_PORT = 47002; // Default RTP port
constexpr int IMG_W = 800;
constexpr int IMG_H = 600;
constexpr int NTP_HEADER_SIZE = sizeof(int) * 3; /* Full header size (npid + metadata) */

constexpr int DISP_SIZE = IMG_W * IMG_H * 2; /* Single channel 16-bit IEEE floats (disparity) */
constexpr int IMG_SIZE = IMG_W * IMG_H * 3;  /* 3-channel 8-bit-per-channel RGB image (rectified and undistorted) */

std::mutex m;
std::condition_variable cv;
bool new_data = false;
void* read_buf;
void* write_buf;

void rtp_receive_hook(void *arg, uvgrtp::frame::rtp_frame *frame);

int main() {
    using namespace nlohmann;

    // Allocate image buffers
    size_t buf_size =  IMG_SIZE * 2 + DISP_SIZE;
    read_buf = malloc(buf_size);
    write_buf = malloc(buf_size);

    // Set up uvgRTP receiver
    uvgrtp::context ctx;
    uvgrtp::session *sess = ctx.create_session("0.0.0.0");
    uvgrtp::media_stream *receiver = sess->create_stream(RTP_PORT, RTP_FORMAT_GENERIC, RCE_RECEIVE_ONLY);
    if (!receiver || receiver->install_receive_hook(nullptr, rtp_receive_hook) != RTP_OK) {
        std::cerr << "Failed to setup RTP receiver." << std::endl;
        return 1;
    }

    // Set up HTTP client
    std::string addr = std::string(SVC_ADDRESS) + ":" + std::to_string(HTTP_PORT);
    auto client = httplib::Client(addr);

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
    params["fps"] = 60;

    auto res = client.Post("/start", params.dump(), "application/json");
    if (res->status != 200) {
        std::cerr << "POST error: " << res->body << std::endl;
        return 1;
    }

    // Main application loop
    std::cout << "Waiting for incoming packets." << std::endl;
    while (true) {
        std::unique_lock lk(m);
        cv.wait(lk, []{return new_data;});
        new_data = false;

        // Save as PNG. Since PNG writing is quite slow we are not guaranteed to capture all received images. To do so we would need to introduce a queue.
        if (stbi_write_png("left.png", IMG_W, IMG_H, 3, read_buf, IMG_W * 3) == 0) {
            std::cerr << "Failed to save image." << std::endl;
            return 1;
        }
        std::cout << "Wrote to png." << std::endl;
    }

    free(read_buf);
    free(write_buf);

    return 0;
}

void rtp_receive_hook(void *arg, uvgrtp::frame::rtp_frame *frame)
{
    std::cout << "Received RTP frame" << std::endl;
    std::lock_guard lk(m);
    // memcpy(left_buf,  frame->payload + NTP_HEADER_SIZE,                IMG_SIZE);
    // memcpy(right_buf, frame->payload + NTP_HEADER_SIZE + IMG_SIZE,     IMG_SIZE);
    // memcpy(disp_buf,  frame->payload + NTP_HEADER_SIZE + IMG_SIZE * 2, DISP_SIZE);
    new_data = true;
    cv.notify_one();

    (void)uvgrtp::frame::dealloc_frame(frame);
}