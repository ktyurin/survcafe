/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020, Raspberry Pi (Trading) Ltd.
 *
 * libcamera_vid.cpp - libcamera video record app.
 */

#include <chrono>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <thread>
#include <sys/signalfd.h>
#include <sys/stat.h>

#include "core/libcamera_encoder.hpp"
#include "core/libcamera_app.hpp"
#include "image/image.hpp"
#include "libcamera_server.hpp"

using namespace std::placeholders;
using namespace std;


const int START_VIDEO_SERVER_SIG = SIGRTMIN + 1;
const int STOP_VIDEO_SERVER_SIG = SIGRTMIN + 2;
const int SAVE_IMAGE_SIG = SIGRTMIN + 3;


#define SERVER_WAITING_TIMEOUT 600 // 10 minutes


void save_image(LibcameraEncoder &app, CompletedRequestPtr &payload, libcamera::Stream *stream,	
					   std::string const &filename)
{
	StreamInfo info = app.GetStreamInfo(stream);
	
	const std::vector<libcamera::Span<uint8_t>> mem = app.Mmap(payload->buffers[stream]);

	if ("jpg")
	{
		jpeg_save(mem, info, payload->metadata, filename, app.CameraId());
	}
	else if ("png")
	{
		//png_save(mem, info, filename, options);
	}
}


CameraManager::CameraManager(int argc, char* argv[])
{
    VideoOptions *options = m_app.GetOptions();
    options->Parse(argc, argv);
}


void CameraManager::executeCommand(ServerCommand command)
{
    switch(command)
    {
        case ServerCommand::OPEN_NETWORK_STREAM:
            startNetworkStream();
            break;
        case ServerCommand::CLOSE_NETWORK_STREAM:
            stopNetworkStream();
            break;
        case ServerCommand::CAPTURE_IMAGE:
            //captureImage();
            break;
    }
}


void CameraManager::startNetworkStream()
{
    if (m_state == ServerState::IDLE)
    {
        m_net_output = new NetOutput(m_options);
        m_socket_fd = m_net_output->startServer();
        m_state = ServerState::WAITING_CONNECTION;
        start_waiting_timestamp = time(NULL);
    }
}


void CameraManager::stopNetworkStream()
{
    if (m_state == ServerState::WAITING_CONNECTION || m_state == ServerState::CONNECTED)
    {
        delete m_net_output;
        m_net_output = nullptr;
        if (m_state == ServerState::WAITING_CONNECTION)
        {
            close(m_socket_fd);
        }
        else
        {
            m_app.StopEncoder();
        }
        m_state = ServerState::IDLE;
    }
}


void CameraManager::captureImage(CompletedRequestPtr& frame)
{
    save_image(m_app, frame, m_app.VideoStream(), m_options->output);
}


ServerCommand get_command()
{
	string command {};
    cin >> command;
    if (command == "start_video_server")
    {
        return ServerCommand::OPEN_NETWORK_STREAM;
    }
    else if (command == "stop_video_server")
    {
        return ServerCommand::CLOSE_NETWORK_STREAM;
    }
    else if (command == "capture_image")
    {
        return ServerCommand::CAPTURE_IMAGE;
    }
}


static int g_signal_received;
static void control_signal_handler(int signal_number)
{
	g_signal_received = signal_number;
}


void CameraManager::start()
{
    m_app.OpenCamera();
	m_app.ConfigureVideo(LibcameraEncoder::FLAG_VIDEO_NONE);
	m_app.StartCamera();

    m_serving_thread = make_unique<thread>(&CameraManager::serve_forever, this);
}


void CameraManager::stop()
{
    m_stop_request = true;
    m_serving_thread->join();
    m_serving_thread.reset();

    m_app.CloseCamera();
    m_app.CloseCamera();
}

// The main event loop for the application.

    
void CameraManager::serve_forever()
{
	int nfds = 0;
	fd_set rfds;
	sigset_t sigmask;
    struct timespec ts;
    
    signal(SIGPIPE, SIG_IGN);
       
    signal(SIGRTMIN+1, control_signal_handler);
    signal(SIGRTMIN+2, control_signal_handler);
    signal(SIGRTMIN+3, control_signal_handler);
        
    FD_ZERO(&rfds);
    sigemptyset(&sigmask);
    
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000000 / 8;
		
	time_t start_waiting_timestamp = 0;
	std::vector<ServerCommand> commands;
	
	
	while (!m_stop_request)
    {
		// Waiting camera frames
		std::queue<LibcameraEncoder::Msg> *queue = m_app.Wait();

        while (!queue->empty())
        {
            LibcameraEncoder::Msg msg = std::move(queue->front());
            queue->pop();
            CompletedRequestPtr &completed_request = std::get<CompletedRequestPtr>(msg.payload);
            if (m_state == ServerState::CONNECTED)
            {
                m_app.EncodeBuffer(completed_request, m_app.VideoStream());
            }
        }

		// Handling state
		switch (m_state) 
        {
			case ServerState::CONNECTED:
				if (m_net_output->closed())
				{
					commands.push_back(ServerCommand::CLOSE_NETWORK_STREAM);
				}
                break;
			case ServerState::WAITING_CONNECTION:
				if (time(NULL) - start_waiting_timestamp > SERVER_WAITING_TIMEOUT)
				{
					commands.push_back(ServerCommand::CLOSE_NETWORK_STREAM);
				}
				else
				{

				}
				break;
		}

        if (m_net_output)
        {
            nfds = m_socket_fd + 1;
            FD_SET(m_socket_fd, &rfds);
        }
        else
        {
            nfds = 0;
            FD_CLR(m_socket_fd, &rfds);
        }
		
		// Wait for signals and sockets
		int retval = pselect(nfds, &rfds, NULL, NULL, &ts, &sigmask);

		if (retval == -1 && errno == EINTR)  // We have received a signal but we don't handle signals
		{
		}
		else if (retval > 0) // We have recevied socket connection
		{
			FD_CLR(m_socket_fd, &rfds);
			nfds = 0;
			
			m_socket_fd = m_net_output->acceptConnection();
			m_app.SetEncodeOutputReadyCallback(std::bind(&NetOutput::OutputReady, m_net_output, _1, _2, _3, _4));
			m_app.StartEncoder();

			m_state = ServerState::CONNECTED;
		}
		
		// Handling command
		std::vector<ServerCommand>::iterator iter = commands.begin();
		for (; iter < commands.end(); iter++)
		{
            executeCommand(*iter);
		}
		commands.clear();
	}
}


int main(int argc, char *argv[])
{
    CameraManager cm { argc, argv };
	try
	{
        cm.start();
        while (true)
        {
            ServerCommand command = get_command();
            cm.executeCommand(command);
        }
	}
	catch (std::exception const &e)
	{
		std::cerr << "ERROR: *** " << e.what() << " ***" << std::endl;
		return -1;
	}
}
