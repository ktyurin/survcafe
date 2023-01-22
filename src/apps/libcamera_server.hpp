#pragma once

#include "output/net_output.hpp"


enum class ServerState
{
    VIDEO_SERVER_CONNECTED,
    VIDEO_SERVER_WAITING,
    IDLE
};


enum class ServerCommand 
{
    START_VIDEO_SERVER,
    STOP_VIDEO_SERVER,
    CAPTURE_IMAGE,
    NOP
};


class CameraManager final
{
    public:
        CameraManager(int argc, char* argv[]);
        void executeCommand(ServerCommand command);
        void start();

    private:
        LibcameraEncoder m_app;
        NetOutput *m_net_output  { nullptr };
        VideoOptions const *m_options { nullptr };

        void startVideoServer();
};