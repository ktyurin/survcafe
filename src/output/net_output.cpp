/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020, Raspberry Pi (Trading) Ltd.
 *
 * net_output.cpp - send output over network.
 */

#include <arpa/inet.h>
#include <sys/socket.h>

#include "net_output.hpp"

NetOutput::NetOutput(VideoOptions const *options) : Output(options)
{
    char protocol[4];
    int start, end, a, b, c, d;
    if (sscanf(options->server.c_str(), "%3s://%n%d.%d.%d.%d%n:%d", protocol, &start, &a, &b, &c, &d, &end, &port) != 6)
        throw std::runtime_error("bad network address " + options->output);
    if (strcmp(protocol, "tcp") != 0)
    {
        throw std::runtime_error("unrecognised network protocol " + options->output);
    }
    address = options->server.substr(start, end - start);
    closed_ = false;
}


NetOutput::~NetOutput()
{
    stopServer();
    for (auto connection_fd : connections_) {
        close(connection_fd);
    }
}


int NetOutput::startServer() {
    // We are the server.
    this->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
        throw std::runtime_error("unable to open listen socket");

    server_saddr = {};
    server_saddr.sin_family = AF_INET;
    server_saddr.sin_addr.s_addr = INADDR_ANY;
    server_saddr.sin_port = htons(port);

    int enable = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0)
        throw std::runtime_error("failed to setsockopt listen socket");

    if (bind(listen_fd, (struct sockaddr *)&server_saddr, sizeof(server_saddr)) < 0) {
        throw std::runtime_error("failed to bind listen socket");
    }

    ephemeral_port = ntohs(server_saddr.sin_port);

    listen(listen_fd, 1);

    return listen_fd;
}


void NetOutput::stopServer() {
    close(listen_fd);
    for (std::vector<int>::iterator it = connections_.begin(); it < connections_.end(); it++) {
        close(*it);
    }
    connections_.clear();
    closed_ = true;
}


void NetOutput::outputBuffer(void *mem, size_t size, int64_t /*timestamp_us*/, uint32_t /*flags*/)
{
    using namespace std;
    vector<int> closed_fds;
    try
    {
        // std::cerr << "NetOutput: output buffer " << mem << " size " << size << "\n";

        for (auto fd : connections_) {
            size_t max_size = size;
            for (uint8_t *ptr = (uint8_t *)mem; size;)
            {
                size_t bytes_to_send = std::min(size, max_size);
                if (send(fd, ptr, bytes_to_send, 0) < 0) {
                    if (errno == EPIPE) {
                        closed_fds.push_back(fd);
                    } else {
                        std::cerr << "failed to send data on socket" << errno << std::endl;
                    }
                    break;
                }
                ptr += bytes_to_send;
                size -= bytes_to_send;
            }
        }
        // Clean closed sockets
        for (int ix = 0; ix < closed_fds.size(); ix++) {
            for (vector<int>::iterator it = connections_.begin(); it < connections_.end(); it++) {
                if (*it == closed_fds[ix]) {
                    close(*it);
                    connections_.erase(it);
                    break;
                }
            }
        }
    }
    catch (...)
    {
        std::cerr << "ERROR: *** " << std::endl;
    }
}

int NetOutput::acceptConnection()
{
    sockaddr addr;
    socklen_t sockaddr_size = sizeof(addr);

    int fd = accept(listen_fd, (struct sockaddr *)&addr, &sockaddr_size);
    if (fd < 0)
    {
        std::cerr << "ERRNO " << errno << " " << fd << std::endl;
        throw std::runtime_error("accept socket failed");
    }
    connections_.push_back(fd);

    // close(listen_fd);
}
