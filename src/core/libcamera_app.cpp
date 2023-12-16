/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2021, Raspberry Pi (Trading) Ltd.
 *
 * libcamera_app.cpp - base class for libcamera apps.
 */

#include "frame_info.hpp"
#include "libcamera_app.hpp"
#include "options.hpp"

#include <fcntl.h>

#include <sys/ioctl.h>

#include <linux/videodev2.h>

// If we definitely appear to be running the old camera stack, complain and give up.
// Everything else, Pi or not, we let through.

static void check_camera_stack()
{
	int fd = open("/dev/video0", O_RDWR, 0);
	if (fd < 0)
		return;

	v4l2_capability caps;
	int ret = ioctl(fd, VIDIOC_QUERYCAP, &caps);
	close(fd);

	if (ret < 0 || strcmp((char *)caps.driver, "bm2835 mmal"))
		return;

	fprintf(stderr, "ERROR: the system appears to be configured for the legacy camera stack\n");
	exit(-1);
}

static libcamera::PixelFormat mode_to_pixel_format(Mode const &mode)
{
	// The saving grace here is that we can ignore the Bayer order and return anything -
	// our pipeline handler will give us back the order that works, whilst respecting the
	// bit depth and packing. We may get a "stream adjusted" message, which we can ignore.

	static std::vector<std::pair<Mode, libcamera::PixelFormat>> table = {
		{ Mode(0, 0, 8, false), libcamera::formats::SBGGR8 },
		{ Mode(0, 0, 8, true), libcamera::formats::SBGGR8 },
		{ Mode(0, 0, 10, false), libcamera::formats::SBGGR10 },
		{ Mode(0, 0, 10, true), libcamera::formats::SBGGR10_CSI2P },
		{ Mode(0, 0, 12, false), libcamera::formats::SBGGR12 },
		{ Mode(0, 0, 12, true), libcamera::formats::SBGGR12_CSI2P },
	};

	auto it = std::find_if(table.begin(), table.end(), [&mode] (auto &m) { return mode.bit_depth == m.first.bit_depth && mode.packed == m.first.packed; });
	if (it != table.end())
		return it->second;

	return libcamera::formats::SBGGR12_CSI2P;
}

LibcameraApp::LibcameraApp(std::unique_ptr<Options> opts)
	: options_(std::move(opts)), controls_(controls::controls)
{
	check_camera_stack();

	if (!options_)
		options_ = std::make_unique<Options>();
}

LibcameraApp::~LibcameraApp()
{
	if (options_->verbose && !options_->help)
		std::cerr << "Closing Libcamera application"
				  << std::endl;
	StopCamera();
	Teardown();
	CloseCamera();
}

std::string const &LibcameraApp::CameraId() const
{
	return camera_->id();
}

void LibcameraApp::OpenCamera()
{
	if (options_->verbose)
		std::cerr << "Opening camera..." << std::endl;

	camera_manager_ = std::make_unique<CameraManager>();
	int ret = camera_manager_->start();
	if (ret)
		throw std::runtime_error("camera manager failed to start, code " + std::to_string(-ret));

	std::vector<std::shared_ptr<libcamera::Camera>> cameras = camera_manager_->cameras();
	// Do not show USB webcams as these are not supported in libcamera-apps!
	auto rem = std::remove_if(cameras.begin(), cameras.end(),
							  [](auto &cam) { return cam->id().find("/usb") != std::string::npos; });
	cameras.erase(rem, cameras.end());

	if (cameras.size() == 0)
		throw std::runtime_error("no cameras available");
	if (options_->camera >= cameras.size())
		throw std::runtime_error("selected camera is not available");

	std::string const &cam_id = cameras[options_->camera]->id();
	camera_ = camera_manager_->get(cam_id);
	if (!camera_)
		throw std::runtime_error("failed to find camera " + cam_id);

	if (camera_->acquire())
		throw std::runtime_error("failed to acquire camera " + cam_id);
	camera_acquired_ = true;

	if (options_->verbose)
		std::cerr << "Acquired camera " << cam_id << std::endl;
}

void LibcameraApp::CloseCamera()
{
	if (camera_acquired_)
		camera_->release();
	camera_acquired_ = false;

	camera_.reset();

	camera_manager_.reset();

	if (options_->verbose && !options_->help)
		std::cerr << "Camera closed" << std::endl;
}

void LibcameraApp::ConfigureVideo(unsigned int flags)
{
	if (options_->verbose)
		std::cerr << "Configuring video..." << std::endl;

	bool have_raw_stream = (flags & FLAG_VIDEO_RAW) || options_->mode.bit_depth;
	bool have_lores_stream = options_->lores_width && options_->lores_height;
	std::vector<libcamera::StreamRole> stream_roles = { StreamRole::VideoRecording };
	int lores_index = 1;
	if (have_raw_stream)
	{
		stream_roles.push_back(StreamRole::Raw);
		lores_index = 2;
	}
	if (have_lores_stream)
		stream_roles.push_back(StreamRole::Viewfinder);
	configuration_ = camera_->generateConfiguration(stream_roles);
	if (!configuration_)
		throw std::runtime_error("failed to generate video configuration");

	// Now we get to override any of the default settings from the options_->
	StreamConfiguration &cfg = configuration_->at(0);
	cfg.pixelFormat = libcamera::formats::YUV420;
	cfg.bufferCount = 6; // 6 buffers is better than 4
	if (options_->width)
		cfg.size.width = options_->width;
	if (options_->height)
		cfg.size.height = options_->height;
/*
	if (flags & FLAG_VIDEO_JPEG_COLOURSPACE)
		cfg.colorSpace = libcamera::ColorSpace::Jpeg;
	else if (cfg.size.width >= 1280 || cfg.size.height >= 720)
		cfg.colorSpace = libcamera::ColorSpace::Rec709;
	else
		cfg.colorSpace = libcamera::ColorSpace::Smpte170m;
*/
	// configuration_->transform = options_->transform;

	if (have_raw_stream)
	{
		if (options_->mode.bit_depth)
		{
			configuration_->at(1).size = options_->mode.Size();
			configuration_->at(1).pixelFormat = mode_to_pixel_format(options_->mode);
		}
		else if (!options_->rawfull)
			configuration_->at(1).size = configuration_->at(0).size;
		configuration_->at(1).bufferCount = configuration_->at(0).bufferCount;
	}
	if (have_lores_stream)
	{
		Size lores_size(options_->lores_width, options_->lores_height);
		lores_size.alignDownTo(2, 2);
		if (lores_size.width > configuration_->at(0).size.width ||
			lores_size.height > configuration_->at(0).size.height)
			throw std::runtime_error("Low res image larger than video");
		configuration_->at(lores_index).pixelFormat = libcamera::formats::YUV420;
		configuration_->at(lores_index).size = lores_size;
		configuration_->at(lores_index).bufferCount = configuration_->at(0).bufferCount;
	}
	// configuration_->transform = options_->transform;

	configureDenoise(options_->denoise == "auto" ? "cdn_fast" : options_->denoise);
	setupCapture();

	streams_["video"] = configuration_->at(0).stream();
	if (have_raw_stream)
		streams_["raw"] = configuration_->at(1).stream();
	if (have_lores_stream)
		streams_["lores"] = configuration_->at(lores_index).stream();

	if (options_->verbose)
		std::cerr << "Video setup complete" << std::endl;
}

void LibcameraApp::Teardown()
{
	if (options_->verbose && !options_->help)
		std::cerr << "Tearing down requests, buffers and configuration" << std::endl;

	for (auto &iter : mapped_buffers_)
	{
		// assert(iter.first->planes().size() == iter.second.size());
		// for (unsigned i = 0; i < iter.first->planes().size(); i++)
		for (auto &span : iter.second)
			munmap(span.data(), span.size());
	}
	mapped_buffers_.clear();

	delete allocator_;
	allocator_ = nullptr;

	configuration_.reset();

	frame_buffers_.clear();

	streams_.clear();
}

void LibcameraApp::StartCamera()
{
	// This makes all the Request objects that we shall need.
	makeRequests();

	// Build a list of initial controls that we must set in the camera before starting it.
	// We don't overwrite anything the application may have set before calling us.
	if (!controls_.contains(controls::ScalerCrop.id()) && options_->roi_width != 0 && options_->roi_height != 0)
	{
		Rectangle sensor_area = *(camera_->properties().get(properties::ScalerCropMaximum));
		int x = options_->roi_x * sensor_area.width;
		int y = options_->roi_y * sensor_area.height;
		int w = options_->roi_width * sensor_area.width;
		int h = options_->roi_height * sensor_area.height;
		Rectangle crop(x, y, w, h);
		crop.translateBy(sensor_area.topLeft());
		if (options_->verbose)
			std::cerr << "Using crop " << crop.toString() << std::endl;
		controls_.set(controls::ScalerCrop, crop);
	}

	// Framerate is a bit weird. If it was set programmatically, we go with that, but
	// otherwise it applies only to preview/video modes. For stills capture we set it
	// as long as possible so that we get whatever the exposure profile wants.
	if (!controls_.contains(controls::FrameDurationLimits.id()))
	{
		if (StillStream())
			controls_.set(controls::FrameDurationLimits, { INT64_C(100), INT64_C(1000000000) });
		else if (options_->framerate > 0)
		{
			int64_t frame_time = 1000000 / options_->framerate; // in us
			controls_.set(controls::FrameDurationLimits, { frame_time, frame_time });
		}
	}

	if (!controls_.contains(controls::ExposureTime.id()) && options_->shutter)
		controls_.set(controls::ExposureTime.id(), options_->shutter);
	if (!controls_.contains(controls::AnalogueGain.id()) && options_->gain)
		controls_.set(controls::AnalogueGain.id(), options_->gain);
	if (!controls_.contains(controls::AeMeteringMode.id()))
		controls_.set(controls::AeMeteringMode.id(), options_->metering_index);
	if (!controls_.contains(controls::AeExposureMode.id()))
		controls_.set(controls::AeExposureMode.id(), options_->exposure_index);
	if (!controls_.contains(controls::ExposureValue.id()))
		controls_.set(controls::ExposureValue.id(), options_->ev);
	if (!controls_.contains(controls::AwbMode.id()))
		controls_.set(controls::AwbMode.id(), options_->awb_index);
	if (!controls_.contains(controls::ColourGains.id()) && options_->awb_gain_r && options_->awb_gain_b)
		controls_.set(controls::ColourGains, { options_->awb_gain_r, options_->awb_gain_b });
	if (!controls_.contains(controls::Brightness.id()))
		controls_.set(controls::Brightness.id(), options_->brightness);
	if (!controls_.contains(controls::Contrast.id()))
		controls_.set(controls::Contrast.id(), options_->contrast);
	if (!controls_.contains(controls::Saturation.id()))
		controls_.set(controls::Saturation.id(), options_->saturation);
	if (!controls_.contains(controls::Sharpness.id()))
		controls_.set(controls::Sharpness.id(), options_->sharpness);

	if (camera_->start(&controls_))
		throw std::runtime_error("failed to start camera");
	controls_.clear();
	camera_started_ = true;
	last_timestamp_ = 0;

	camera_->requestCompleted.connect(this, &LibcameraApp::requestComplete);

	for (std::unique_ptr<Request> &request : requests_)
	{
		if (camera_->queueRequest(request.get()) < 0)
			throw std::runtime_error("Failed to queue request");
	}
}

void LibcameraApp::StopCamera()
{
	{
		// We don't want QueueRequest to run asynchronously while we stop the camera.
		std::lock_guard<std::mutex> lock(camera_stop_mutex_);
		if (camera_started_)
		{
			if (camera_->stop())
				throw std::runtime_error("failed to stop camera");

			camera_started_ = false;
		}
	}

	if (camera_)
		camera_->requestCompleted.disconnect(this, &LibcameraApp::requestComplete);

	// An application might be holding a CompletedRequest, so queueRequest will get
	// called to delete it later, but we need to know not to try and re-queue it.
	completed_requests_.clear();

	msg_queue_.Clear();

	requests_.clear();

	controls_.clear(); // no need for mutex here

	if (options_->verbose && !options_->help)
		std::cerr << "Camera stopped!" << std::endl;
}

std::queue<LibcameraApp::Msg> *LibcameraApp::Wait()
{
	return msg_queue_.Wait();
}

void LibcameraApp::queueRequest(CompletedRequest *completed_request)
{
	BufferMap buffers(std::move(completed_request->buffers));

	Request *request = completed_request->request;
	delete completed_request;
	assert(request);

	// This function may run asynchronously so needs protection from the
	// camera stopping at the same time.
	std::lock_guard<std::mutex> stop_lock(camera_stop_mutex_);
	if (!camera_started_)
		return;

	// An application could be holding a CompletedRequest while it stops and re-starts
	// the camera, after which we don't want to queue another request now.
	{
		std::lock_guard<std::mutex> lock(completed_requests_mutex_);
		auto it = completed_requests_.find(completed_request);
		if (it == completed_requests_.end())
			return;
		completed_requests_.erase(it);
	}

	for (auto const &p : buffers)
	{
		if (request->addBuffer(p.first, p.second) < 0)
			throw std::runtime_error("failed to add buffer to request in QueueRequest");
	}

	{
		std::lock_guard<std::mutex> lock(control_mutex_);
		request->controls() = std::move(controls_);
	}

	if (camera_->queueRequest(request) < 0)
		throw std::runtime_error("failed to queue request");
}

void LibcameraApp::PostMessage(MsgType &t, MsgPayload &p)
{
	msg_queue_.Post(Msg(t, std::move(p)));
}

libcamera::Stream *LibcameraApp::GetStream(std::string const &name, StreamInfo *info) const
{
	auto it = streams_.find(name);
	if (it == streams_.end())
		return nullptr;
	if (info)
		*info = GetStreamInfo(it->second);
	return it->second;
}

libcamera::Stream *LibcameraApp::ViewfinderStream(StreamInfo *info) const
{
	return GetStream("viewfinder", info);
}

libcamera::Stream *LibcameraApp::StillStream(StreamInfo *info) const
{
	return GetStream("still", info);
}

libcamera::Stream *LibcameraApp::RawStream(StreamInfo *info) const
{
	return GetStream("raw", info);
}

libcamera::Stream *LibcameraApp::VideoStream(StreamInfo *info) const
{
	return GetStream("video", info);
}

libcamera::Stream *LibcameraApp::LoresStream(StreamInfo *info) const
{
	return GetStream("lores", info);
}

libcamera::Stream *LibcameraApp::GetMainStream() const
{
	for (auto &p : streams_)
	{
		if (p.first == "viewfinder" || p.first == "still" || p.first == "video")
			return p.second;
	}

	return nullptr;
}

std::vector<libcamera::Span<uint8_t>> LibcameraApp::Mmap(FrameBuffer *buffer) const
{
	auto item = mapped_buffers_.find(buffer);
	if (item == mapped_buffers_.end())
	{
		return {};
	}
	return item->second;
}

void LibcameraApp::SetControls(ControlList &controls)
{
	std::lock_guard<std::mutex> lock(control_mutex_);
	controls_ = std::move(controls);
}

StreamInfo LibcameraApp::GetStreamInfo(Stream const *stream) const
{
	StreamConfiguration const &cfg = stream->configuration();
	StreamInfo info;
	info.width = cfg.size.width;
	info.height = cfg.size.height;
	info.stride = cfg.stride;
	info.pixel_format = stream->configuration().pixelFormat;
	// info.colour_space = stream->configuration().colorSpace;
	return info;
}

void LibcameraApp::setupCapture()
{
	// First finish setting up the configuration.

	CameraConfiguration::Status validation = configuration_->validate();
	if (validation == CameraConfiguration::Invalid)
		throw std::runtime_error("failed to valid stream configurations");
	else if (validation == CameraConfiguration::Adjusted)
		std::cerr << "Stream configuration adjusted" << std::endl;

	if (camera_->configure(configuration_.get()) < 0)
		throw std::runtime_error("failed to configure streams");
	if (options_->verbose)
		std::cerr << "Camera streams configured" << std::endl;

	if (options_->verbose)
	{
		std::cerr << "Available controls:" << std::endl;
		for (auto const &[id, info] : camera_->controls())
			std::cerr << "    " << id->name() << " : " << info.toString() << std::endl;
	}

	// Next allocate all the buffers we need, mmap them and store them on a free list.

	allocator_ = new FrameBufferAllocator(camera_);
	for (StreamConfiguration &config : *configuration_)
	{
		Stream *stream = config.stream();

		if (allocator_->allocate(stream) < 0)
			throw std::runtime_error("failed to allocate capture buffers");

		for (const std::unique_ptr<FrameBuffer> &buffer : allocator_->buffers(stream))
		{
			// "Single plane" buffers appear as multi-plane here, but we can spot them because then
			// planes all share the same fd. We accumulate them so as to mmap the buffer only once.
			size_t buffer_size = 0;
			for (unsigned i = 0; i < buffer->planes().size(); i++)
			{
				const FrameBuffer::Plane &plane = buffer->planes()[i];
				buffer_size += plane.length;
				if (i == buffer->planes().size() - 1 || plane.fd.get() != buffer->planes()[i + 1].fd.get())
				{
					void *memory = mmap(NULL, buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, plane.fd.get(), 0);
					mapped_buffers_[buffer.get()].push_back(
						libcamera::Span<uint8_t>(static_cast<uint8_t *>(memory), buffer_size));
					buffer_size = 0;
				}
			}
			frame_buffers_[stream].push(buffer.get());
		}
	}
	if (options_->verbose)
		std::cerr << "Buffers allocated and mapped" << std::endl;

	//startPreview();

	// The requests will be made when StartCamera() is called.
}

void LibcameraApp::makeRequests()
{
	auto free_buffers(frame_buffers_);
	while (true)
	{
		for (StreamConfiguration &config : *configuration_)
		{
			Stream *stream = config.stream();
			if (stream == configuration_->at(0).stream())
			{
				if (free_buffers[stream].empty())
				{
					if (options_->verbose)
						std::cerr << "Requests created" << std::endl;
					return;
				}
				std::unique_ptr<Request> request = camera_->createRequest();
				if (!request)
					throw std::runtime_error("failed to make request");
				requests_.push_back(std::move(request));
			}
			else if (free_buffers[stream].empty())
				throw std::runtime_error("concurrent streams need matching numbers of buffers");

			FrameBuffer *buffer = free_buffers[stream].front();
			free_buffers[stream].pop();
			if (requests_.back()->addBuffer(stream, buffer) < 0)
				throw std::runtime_error("failed to add buffer to request");
		}
	}
}

void LibcameraApp::requestComplete(Request *request)
{
	if (request->status() == Request::RequestCancelled)
		return;

	CompletedRequest *r = new CompletedRequest(sequence_++, request);
	CompletedRequestPtr payload(r, [this](CompletedRequest *cr) { this->queueRequest(cr); });
	{
		std::lock_guard<std::mutex> lock(completed_requests_mutex_);
		completed_requests_.insert(r);
	}

	// We calculate the instantaneous framerate in case anyone wants it.
	// Use the sensor timestamp if possible as it ought to be less glitchy than
	// the buffer timestamps.
	uint64_t timestamp = payload->metadata.contains(controls::SensorTimestamp.id())
							? *(payload->metadata.get(controls::SensorTimestamp))
							: payload->buffers.begin()->second->metadata().timestamp;
	if (last_timestamp_ == 0 || last_timestamp_ == timestamp)
		payload->framerate = 0;
	else
		payload->framerate = 1e9 / (timestamp - last_timestamp_);
	last_timestamp_ = timestamp;

	this->msg_queue_.Post(Msg(MsgType::RequestComplete, std::move(payload)));
}

void LibcameraApp::configureDenoise(const std::string &denoise_mode)
{
	using namespace libcamera::controls::draft;

	static const std::map<std::string, NoiseReductionModeEnum> denoise_table = {
		{ "off", NoiseReductionModeOff },
		{ "cdn_off", NoiseReductionModeMinimal },
		{ "cdn_fast", NoiseReductionModeFast },
		{ "cdn_hq", NoiseReductionModeHighQuality }
	};
	NoiseReductionModeEnum denoise;

	auto const mode = denoise_table.find(denoise_mode);
	if (mode == denoise_table.end())
		throw std::runtime_error("Invalid denoise mode " + denoise_mode);
	denoise = mode->second;

	controls_.set(NoiseReductionMode, denoise);
}
