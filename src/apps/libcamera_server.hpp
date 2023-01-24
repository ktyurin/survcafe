#pragma once

#include <atomic>
#include <thread>

#include "output/net_output.hpp"


enum class ServerState
{
    CONNECTED,
    WAITING_CONNECTION,
    IDLE
};


enum class ServerCommand 
{
    OPEN_NETWORK_STREAM,
    CLOSE_NETWORK_STREAM,
    CAPTURE_IMAGE,
    NOP
};


class CameraManager final
{
    public:
        CameraManager(int argc, char* argv[]);
        void executeCommand(ServerCommand command);
        void serve_forever();
        void start();
        void stop();

    private:
        LibcameraEncoder m_app;
        std::unique_ptr<std::thread> m_serving_thread;
        std::atomic<ServerState> m_state {ServerState::IDLE};
        NetOutput *m_net_output  {nullptr};
        VideoOptions *m_options {nullptr};
        time_t m_start_waiting_timestamp {};
        int m_socket_fd {};
        std::atomic_bool m_stop_request {false};

        void startNetworkStream();
        void stopNetworkStream();
        void captureImage(CompletedRequestPtr& frame);
};