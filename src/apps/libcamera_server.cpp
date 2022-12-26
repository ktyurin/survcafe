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
#include <sys/signalfd.h>
#include <sys/stat.h>

#include "core/libcamera_encoder.hpp"
#include "core/libcamera_app.hpp"
#include "output/output.hpp"
#include "output/net_output.hpp"
#include "image/image.hpp"

using namespace std::placeholders;


const int START_VIDEO_SERVER_SIG = SIGRTMIN + 1;
const int STOP_VIDEO_SERVER_SIG = SIGRTMIN + 2;
const int SAVE_IMAGE_SIG = SIGRTMIN + 3;

#define START_VIDEO_SERVER_CMD 1
#define STOP_VIDEO_SERVER_CMD 2
#define SAVE_IMAGE_CMD 3
#define NO_CMD 0

#define VIDEO_SERVER_CONNECTED 1
#define VIDEO_SERVER_WAITING 2
#define IDLE 0

#define SERVER_WAITING_TIMEOUT 600 // 10 minutes


static int g_signal_received;
static void control_signal_handler(int signal_number)
{
	g_signal_received = signal_number;
}


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


int sig2cmd()
{
	int cmd = NO_CMD;
	if (g_signal_received == START_VIDEO_SERVER_SIG)
	{
		cmd = START_VIDEO_SERVER_CMD;
	}
	else if (g_signal_received == STOP_VIDEO_SERVER_SIG)
	{
		cmd = STOP_VIDEO_SERVER_CMD;
	}
	else if (g_signal_received ==  SAVE_IMAGE_SIG)
	{
		cmd =  SAVE_IMAGE_CMD;
	}

	return cmd;
}


// The main even loop for the application.

    
static void event_loop(LibcameraEncoder &app)
{
	VideoOptions const *options = app.GetOptions();

	NetOutput *net_output = NULL;
	app.OpenCamera();
	app.ConfigureVideo(LibcameraEncoder::FLAG_VIDEO_NONE);
	app.StartCamera();

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
		
	int socket_fd = 0;
	
	time_t start_waiting_timestamp = 0;
	int state = 0;
	std::vector<int> commands;
	
	
	for (unsigned int count = 0; ; count++) {
		// Waiting camera frames
		std::queue<LibcameraEncoder::Msg> *queue = app.Wait();
		LibcameraEncoder::Msg msg = std::move(queue->front());
		queue->pop();
		CompletedRequestPtr &completed_request = std::get<CompletedRequestPtr>(msg.payload);
		
		// Handling state
		switch (state) {
			case VIDEO_SERVER_CONNECTED:
			{
				if (net_output->closed())
				{
					commands.push_back(STOP_VIDEO_SERVER_CMD);
				}
				else
				{
					while (!queue->empty())
					{
						LibcameraEncoder::Msg msg = std::move(queue->front());
						queue->pop();
						completed_request = std::get<CompletedRequestPtr>(msg.payload);
						app.EncodeBuffer(completed_request, app.VideoStream());
					}
				}
				break;
			}
			case VIDEO_SERVER_WAITING:
			{
				if (time(NULL) - start_waiting_timestamp > SERVER_WAITING_TIMEOUT)
				{
					commands.push_back(STOP_VIDEO_SERVER_CMD);
				}
				else
				{
					nfds = socket_fd + 1;
					FD_SET(socket_fd, &rfds);
				}
				break;
			}
		}
		
		// Wait for signals and sockets
		int retval = pselect(nfds, &rfds, NULL, NULL, &ts, &sigmask);

		if (retval == -1 && errno == EINTR)  // We have received a signal
		{
			commands.push_back(sig2cmd());
		}
		else if (retval > 0) // We have recevied socket connection
		{
			FD_CLR(socket_fd, &rfds);
			nfds = 0;
			
			socket_fd = net_output->acceptConnection();
			app.SetEncodeOutputReadyCallback(std::bind(&NetOutput::OutputReady, net_output, _1, _2, _3, _4));
			app.StartEncoder();

			state = VIDEO_SERVER_CONNECTED;
		}
		
		// Handling command
		std::vector<int>::iterator iter = commands.begin();
		for (; iter < commands.end(); iter++)
		{
			switch (*iter) {
				case SAVE_IMAGE_CMD:
				{
					save_image(app, completed_request, app.VideoStream(), options->output);
					break;
				}
				case START_VIDEO_SERVER_CMD:
				{
					if (state == IDLE)
					{
						net_output = new NetOutput(options);
						socket_fd = net_output->startServer();
						state = VIDEO_SERVER_WAITING;
						start_waiting_timestamp = time(NULL);
					}
					break;
				}
				case STOP_VIDEO_SERVER_CMD:
				{
					if (state == VIDEO_SERVER_WAITING || state == VIDEO_SERVER_CONNECTED)
					{
						delete net_output;
						if (state == VIDEO_SERVER_WAITING)
						{
							FD_CLR(socket_fd, &rfds);
							nfds = 0;
							close(socket_fd);
						}
						else
						{
							app.StopEncoder();
						}
						state = IDLE;
					}
					break;
				}
			}
		}
		commands.clear();
	}
	
	return;
}

int main(int argc, char *argv[])
{
	try
	{
		LibcameraEncoder app;
		VideoOptions *options = app.GetOptions();
		if (options->Parse(argc, argv))
		{
			if (options->verbose)
				options->Print();

			event_loop(app);
		}
	}
	catch (std::exception const &e)
	{
		std::cerr << "ERROR: *** " << e.what() << " ***" << std::endl;
		return -1;
	}
	return 0;
}
