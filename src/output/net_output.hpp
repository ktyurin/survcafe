/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020, Raspberry Pi (Trading) Ltd.
 *
 * net_output.hpp - send output over network.
 */

#pragma once

#include <vector>

#include <netinet/in.h>

#include "output.hpp"


class NetOutput : public Output
{
public:
	NetOutput(VideoOptions const *options);
	~NetOutput();
	int acceptConnection();
	int startServer();
	void stopServer();
	bool closed() { return closed_; }
	in_port_t get_port() { return ephemeral_port; }


protected:
	void outputBuffer(void *mem, size_t size, int64_t timestamp_us, uint32_t flags) override;

private:
	std::vector<int> connections_;
	int listen_fd;
	std::string address;
	in_port_t port;
	in_port_t ephemeral_port;
	bool closed_;
	sockaddr_in server_saddr;
};
