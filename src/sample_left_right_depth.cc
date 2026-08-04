#include <iostream>
#include <uvgrtp/lib.hh>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <fp16.h>
#include <happly.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

constexpr uint16_t HTTP_PORT = 47001;                 // Hardcoded HTTP port (do not change)
constexpr uint16_t RTP_PORT = 47002;                  // Default RTP port (can be anything, just make sure it's allowed by firewall and
                                                      // doesn't clash with other programs)

constexpr int IMG_W = 800;                            // Output image width
constexpr int IMG_H = 600;                            // Output image height
                                                      // Supported dimensions are [400x300], [800x600]

constexpr int FPS = 30;                               // Desired explore3D capture FPS. Can be 5, 10, 15, 30, 40, 50, 60. Packet receive rate
                                                      // is throttled by depth process speed, which is around 15-30 depending on depth mode.

constexpr int NTP_HEADER_SIZE = sizeof(int) * 3;      // Network protocol header size

constexpr int DISP_SIZE = IMG_W * IMG_H * 2;          // Single channel 16-bit IEEE floats (disparity)
constexpr int IMG_SIZE = IMG_W * IMG_H * 3;           // 3-channel 8-bit-per-channel RGB image (rectified and undistorted)
constexpr size_t BUF_SIZE = 2 * IMG_SIZE + DISP_SIZE; // Left image + Right image + Left disparity

std::atomic<bool> should_stay_connected{true};
std::atomic<bool> should_poll{true};
std::string svc_address, calib_file, dwvo_file, left_bus_id, right_bus_id, input_type;
float fx_calib, cx_calib, cy_calib;
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

    if (res && res->status != 200) {
        std::cerr << "Failed to do GET request at /health: status " << res->status << std::endl;
    }
}

/**
 * Writes the given disparity and RGB buffers to a PLY. Better performance can be achieved via multi-threading or
 * GPU usage.
 */
void write_to_ply(const std::string& filename, uint16_t* disp, unsigned char* rgb) {
    constexpr int CALIB_W = 1600;    // Image width at calibration
    constexpr int CALIB_H = 1200;    // Image height at calibration
    constexpr float scale = 100;     // Scale factor (m -> cm)
    constexpr float baseline = 0.1;  // Baseline of expore3D (m)
    constexpr float min_depth = 0;   // Minimum depth (cm)
    constexpr float max_depth = 1000; // Maximum depth (cm)

    std::vector<std::array<double, 3>> vertex_positions;
    std::vector<std::array<double, 3>> vertex_colors;

    float fx = fx_calib / (CALIB_W / (float)IMG_W);
    float cx = cx_calib / (CALIB_W / (float)IMG_W);
    float cy = cy_calib / (CALIB_H / (float)IMG_H);

    for (int v = 0; v < IMG_H; ++v) {
        for (int u = 0; u < IMG_W; ++u) {
            float disparity = fp16_ieee_to_fp32_value(disp[v * IMG_W + u]);
            if (disparity < 0.0001f) continue;

            float t = scale * baseline / disparity;
            float z = -t * fx;
            float x = t * (u - cx);
            float y = t * -(v - cy);
            if (abs(z) < min_depth || abs(z) > max_depth) {
                continue;
            }
            vertex_positions.push_back({x, y, z});

            int idx = (v * IMG_W + u) * 3;
            float r = rgb[idx + 0] / 255.0f;
            float g = rgb[idx + 1] / 255.0f;
            float b = rgb[idx + 2] / 255.0f;
            vertex_colors.push_back({r, g, b});
        }
    }

    try {
        happly::PLYData ply;
        ply.addVertexPositions(vertex_positions);
        ply.addVertexColors(vertex_colors);
        ply.write(filename, happly::DataFormat::Binary);
        std::cout << "Wrote PLY." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
    }
}

json list_calibrations(httplib::Client& client) {
    auto res = client.Get("/list_calibrations");
    if (!res || res->status != 200) {
        std::cerr << "Failed to list calibrations." << std::endl;
        exit(1);
    }
    json body = json::parse(res->body);
    if (!body.is_array() || body.empty()) {
        std::cerr << "No calibration files detected. Is ~/RTPSender/calibrations empty on the SVC?" << std::endl;
        exit(1);
    }
    std::cout << body.dump(2) << std::endl;
    return body;
}

void list_dwvos(httplib::Client& client) {
    auto res = client.Get("/recordings");
    if (!res || res->status != 200) {
        std::cerr << "Failed to list recordings." << std::endl;
        exit(1);
    }
    json body = json::parse(res->body);
    std::cout << body.dump(2) << std::endl;
}

void list_devices(httplib::Client& client) {
    auto res = client.Get("/list_devices");
    if (!res || res->status != 200) {
        std::cerr << "Failed to list devices." << std::endl;
        exit(1);
    }
    json body = json::parse(res->body);
    std::cout << body.dump(2) << std::endl;
}

int main(int argc, char **argv) {

    if (argc < 3) {
        std::cout << "Start USB stream:       " << argv[0] << " <svc-address> --USB <left-bus-id> <right-bus-id> <calibration-filename>" << std::endl;
        std::cout << "Start DWVO stream:      " << argv[0] << " <svc-address> --DWVO <dwvo-filename> <calibration-filename>" << std::endl;
        std::cout << "List calibration files: " << argv[0] << " <svc-address> -lc" << std::endl;
        std::cout << "List DWVO files:        " << argv[0] << " <svc-address> -lv" << std::endl;
        std::cout << "List device bus IDs:    " << argv[0] << " <svc-address> -ld" << std::endl;
        exit(0);
    }
    svc_address = argv[1];
    std::string flag = argv[2];

    // Set up HTTP client
    std::string addr = std::string(svc_address) + ":" + std::to_string(HTTP_PORT);
    auto client = httplib::Client(addr);

    if (flag == "-lc") {
        list_calibrations(client);
        exit(0);
    }

    if (flag == "-lv") {
        list_dwvos(client);
        exit(0);
    }

    if (flag == "-ld") {
        list_devices(client);
        exit(0);
    }

    if (flag == "--USB") {
        if (argc != 6) {
            std::cout << "Missing one or more required inputs for USB streaming." << std::endl;
            exit(0);
        }
        left_bus_id = argv[3];
        right_bus_id = argv[4];

        // Basic validation
        if (left_bus_id == right_bus_id) {
            std::cerr << "Left and right bus IDs must be unique." << std::endl;
            exit(1);
        }

        calib_file = argv[5];
        input_type = "USB";
    } else if (flag == "--DWVO") {
        if (argc != 5) {
            std::cout << "Missing one or more required inputs for DWVO streaming." << std::endl;
            exit(0);
        }
        dwvo_file = argv[3];
        calib_file = argv[4];
        input_type = "DWVO";
    } else {
        std::cout << "Unknown flag: " << flag << std::endl;
        exit(0);
    }

    // Grab calibration intrinsics
    json calib_list = list_calibrations(client);
    auto it = std::find_if(calib_list.begin(), calib_list.end(), [&](const json& obj) {
        if (obj["filename"] == calib_file) {
            fx_calib = obj["intrinsics"]["fx"];
            cx_calib = obj["intrinsics"]["cx"];
            cy_calib = obj["intrinsics"]["cy"];
            return true;
        }
        return false;
    });
    if (it == calib_list.end()) {
        std::cerr << "Specified calibration file not found." << std::endl;
        exit(1);
    }

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

    // Register this address as a client in the remote server
    std::thread subscribe_thread(subscribe, addr);

    json params;
    params["calibration"]["filename"] = calib_file;
    params["network_protocol"] = "SLAM"; // signals to send left image, right image, left disparity
    params["depth_mode"]["frame_width"] = IMG_W;
    params["depth_mode"]["frame_height"] = IMG_H;
    params["depth_mode"]["name"] = "SGM";
    params["input_dwvo"] = dwvo_file;
    params["rtp_port"] = RTP_PORT;
    params["resolution"] = "UXGA";
    params["input_type"] = input_type;
    params["input_left"] = left_bus_id;
    params["input_right"] = right_bus_id;
    params["swap_inputs"] = false;
    params["fps"] = FPS;

    // POST to RTPSender /start endpoint. This starts the RTP stream with given parameters.
    std::cout << "Starting stream..." << std::endl;
    auto res = client.Post("/start", params.dump(), "application/json");
    if (!res) {
        std::cerr << "Connection refused. Is the RTPSender process running on the target SVC?" << std::endl;
        should_stay_connected = false;
        should_poll = false;
    } else if (res->status != 200) {
        std::cerr << "POST error: " << res->body << std::endl;
        should_stay_connected = false;
        should_poll = false;
    }

    // Main application loop
    std::cout << "Waiting for incoming packets. Press [Ctrl+C] to stop." << std::endl;
    while (should_poll) {
        // This loop will not process all received packets as writing to a PLY is very slow. If your
        // goal is to capture all packets, use the uvgRTP receive hook API and a queuing approach.

        // Pull most recently received frame, if any
        auto frm = receiver->pull_frame(5000);
        if (!frm) {
            std::cout << "Frame pull timed out. Trying again..." << std::endl;
            continue;
        }
        size_t expected_size = NTP_HEADER_SIZE + BUF_SIZE;
        if (frm->payload_len != expected_size) {
            std::cerr << "Received invalid frame of size " << frm->payload_len << ", expected " << expected_size <<
                    std::endl;
        } else {
            uint8_t* left_img = frm->payload + NTP_HEADER_SIZE;
            uint8_t* right_img = left_img + IMG_SIZE;
            uint16_t* disp = reinterpret_cast<uint16_t*>(right_img + IMG_SIZE);

            // Write received disparity and RGB to PLY file. Overwrites the last written one if any
            write_to_ply("left.ply", disp, left_img);

            // Write right rectified image to PNG
            stbi_write_png("right.png", IMG_W, IMG_H, 3, right_img, IMG_W * 3);
        }
        (void) uvgrtp::frame::dealloc_frame(frm);
    }
    std::cout << "Cleaning up resources... ";
    subscribe_thread.join();
    std::cout << " done." << std::endl;
    return 0;
}

