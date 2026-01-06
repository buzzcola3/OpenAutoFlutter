#include "h264_decoder.h"

#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

struct H264Decoder::Impl {
	const AVCodec* codec = nullptr;
	AVCodecContext* ctx = nullptr;
	AVFrame* frame = nullptr;
	AVPacket* pkt = nullptr;
	SwsContext* sws = nullptr;
	std::mutex mutex;
	int sws_w = 0;
	int sws_h = 0;
	AVPixelFormat sws_fmt = AV_PIX_FMT_NONE;
	std::vector<uint8_t> config_annexb;
	bool have_config = false;
	bool injected_config = false;

	Impl() {
		codec = avcodec_find_decoder(AV_CODEC_ID_H264);
		if (!codec) throw std::runtime_error("H264 codec not found");
		ctx = avcodec_alloc_context3(codec);
		if (!ctx) throw std::runtime_error("Failed to alloc codec context");
		if (avcodec_open2(ctx, codec, nullptr) < 0)
			throw std::runtime_error("Failed to open codec");
		frame = av_frame_alloc();
		pkt = av_packet_alloc();
		if (!frame || !pkt) throw std::runtime_error("Failed to alloc frame/pkt");
	}

	~Impl() {
		if (sws) sws_freeContext(sws);
		if (pkt) av_packet_free(&pkt);
		if (frame) av_frame_free(&frame);
		if (ctx) avcodec_free_context(&ctx);
	}
};

H264Decoder::H264Decoder() : impl_(new Impl()) {}
H264Decoder::~H264Decoder() { delete impl_; }

static std::string hex_head(const uint8_t* data, size_t size, size_t max_bytes = 32) {
	std::ostringstream oss;
	const size_t n = std::min(size, max_bytes);
	for (size_t i = 0; i < n; ++i) {
		if (i) oss << ' ';
		oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
	}
	if (size > max_bytes) oss << " ...";
	return oss.str();
}

static bool is_annexb_only_config(const uint8_t* data, size_t size) {
	// Return true if payload is Annex-B and contains only SPS/PPS NALs (types 7/8)
	if (!data || size < 6) return false;
	size_t i = 0;
	bool saw_nal = false;
	while (i + 4 <= size) {
		// find next start code
		size_t sc = i;
		while (sc + 3 < size && !(data[sc] == 0 && data[sc + 1] == 0 && ((data[sc + 2] == 1) || (data[sc + 2] == 0 && sc + 3 < size && data[sc + 3] == 1)))) {
			++sc;
		}
		if (sc + 3 >= size) break;
		size_t start = sc;
		size_t sc_len = (data[sc + 2] == 1) ? 3 : 4;
		size_t nal_start = start + sc_len;
		size_t next = nal_start;
		while (next + 3 < size && !(data[next] == 0 && data[next + 1] == 0 && ((data[next + 2] == 1) || (data[next + 2] == 0 && next + 3 < size && data[next + 3] == 1)))) {
			++next;
		}
		if (nal_start >= size) break;
		uint8_t nal_type = data[nal_start] & 0x1F;
		if (nal_type != 7 && nal_type != 8) return false;
		saw_nal = true;
		if (next <= start) break;
		i = next;
	}
	return saw_nal;
}

static bool parse_avcc_config(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
	// AVCDecoderConfigurationRecord -> Annex-B SPS/PPS blobs
	if (!data || size < 7) return false;
	if (data[0] != 1) return false; // configurationVersion must be 1
	const uint8_t lengthSizeMinusOne = data[4] & 0x03;
	if (lengthSizeMinusOne > 3) return false;
	size_t offset = 5;
	if (offset >= size) return false;
	const uint8_t numSPS = data[offset] & 0x1F;
	offset++;
	for (uint8_t i = 0; i < numSPS; ++i) {
		if (offset + 2 > size) return false;
		uint16_t nal_len = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
		offset += 2;
		if (nal_len == 0 || offset + nal_len > size) return false;
		out.insert(out.end(), {0x00, 0x00, 0x00, 0x01});
		out.insert(out.end(), data + offset, data + offset + nal_len);
		offset += nal_len;
	}
	if (offset >= size) return false;
	const uint8_t numPPS = data[offset];
	offset++;
	for (uint8_t i = 0; i < numPPS; ++i) {
		if (offset + 2 > size) return false;
		uint16_t nal_len = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
		offset += 2;
		if (nal_len == 0 || offset + nal_len > size) return false;
		out.insert(out.end(), {0x00, 0x00, 0x00, 0x01});
		out.insert(out.end(), data + offset, data + offset + nal_len);
		offset += nal_len;
	}
	return !out.empty();
}

bool H264Decoder::decode_to_yuv420p(const uint8_t* data,
									size_t size,
									std::vector<uint8_t>& out_yuv,
									int& out_width,
									int& out_height) {
	static std::atomic<int> frame_counter{0};
	static std::atomic<int> packet_log_counter{0};
	if (!data || size == 0) {
		std::cout << "[H264Decoder] Reject packet: empty input" << std::endl;
		return false;
	}
	if (size < 5 || size > 4 * 1024 * 1024) {
		std::cout << "[H264Decoder] Reject packet: size=" << size << std::endl;
		return false; // guard malformed payloads
	}
	int pkt_log_id = ++packet_log_counter;
	if (pkt_log_id <= 10) {
		bool start_code = (size >= 4 && data[0] == 0 && data[1] == 0 && ((data[2] == 0 && data[3] == 1) || data[2] == 1));
		std::cout << "[H264Decoder] Packet " << pkt_log_id << " size=" << size
			<< " startCode=" << (start_code ? "yes" : "no")
			<< " head=" << hex_head(data, size, 32) << std::endl;
	}
	bool has_start_code = (size >= 4 && data[0] == 0 && data[1] == 0 && ((data[2] == 0 && data[3] == 1) || data[2] == 1));
	std::vector<uint8_t> avcc_to_annexb;
	const uint8_t* payload = data;
	size_t payload_size = size;
	if (!has_start_code) {
		// Treat a small timestamp-0 codec config (AVCC) specially: stash SPS/PPS and skip decode
		std::vector<uint8_t> config;
		if (parse_avcc_config(data, size, config)) {
			std::lock_guard<std::mutex> lock(impl_->mutex);
			impl_->config_annexb = std::move(config);
			impl_->have_config = true;
			impl_->injected_config = false;
			std::cout << "[H264Decoder] Stored AVC configuration (" << size << " bytes) head=" << hex_head(data, size, 32) << std::endl;
			return false;
		}

		// Handle AVCC (length-prefixed) by converting to Annex-B start codes
		size_t offset = 0;
		avcc_to_annexb.clear();
		while (offset + 4 <= size) {
			uint32_t nal_len = (static_cast<uint32_t>(data[offset]) << 24) |
							(static_cast<uint32_t>(data[offset + 1]) << 16) |
							(static_cast<uint32_t>(data[offset + 2]) << 8) |
							(static_cast<uint32_t>(data[offset + 3]));
			offset += 4;
			if (nal_len == 0 || offset + nal_len > size) {
				std::cout << "[H264Decoder] Reject packet: invalid AVCC length at offset=" << (offset - 4)
						<< " len=" << nal_len << " size=" << size << std::endl;
				return false;
			}
			avcc_to_annexb.insert(avcc_to_annexb.end(), {0x00, 0x00, 0x00, 0x01});
			avcc_to_annexb.insert(avcc_to_annexb.end(), data + offset, data + offset + nal_len);
			offset += nal_len;
		}
		if (offset != size) {
			std::cout << "[H264Decoder] Reject packet: trailing bytes after AVCC parse offset=" << offset << " size=" << size << std::endl;
			return false;
		}
		if (avcc_to_annexb.empty()) {
			std::cout << "[H264Decoder] Reject packet: missing Annex B start code and AVCC conversion failed" << std::endl;
			return false;
		}
		payload = avcc_to_annexb.data();
		payload_size = avcc_to_annexb.size();
	} else {
		// If Annex-B and contains only SPS/PPS, treat as configuration and skip decode
		if (is_annexb_only_config(payload, payload_size)) {
			std::lock_guard<std::mutex> lock(impl_->mutex);
			impl_->config_annexb.assign(payload, payload + payload_size);
			impl_->have_config = true;
			impl_->injected_config = false;
			std::cout << "[H264Decoder] Stored Annex-B SPS/PPS (" << payload_size << " bytes) head=" << hex_head(payload, payload_size, 32) << std::endl;
			return false;
		}
	}
	auto* impl = impl_;
	std::lock_guard<std::mutex> lock(impl->mutex);
	av_packet_unref(impl->pkt);
	// Prepend stored SPS/PPS config once before first decode if we saw an AVC config packet
	std::vector<uint8_t> with_config;
	const uint8_t* final_payload = payload;
	size_t final_size = payload_size;
	if (impl->have_config && !impl->injected_config) {
		with_config.reserve(impl->config_annexb.size() + payload_size);
		with_config.insert(with_config.end(), impl->config_annexb.begin(), impl->config_annexb.end());
		with_config.insert(with_config.end(), payload, payload + payload_size);
		final_payload = with_config.data();
		final_size = with_config.size();
		impl->injected_config = true;
		std::cout << "[H264Decoder] Injected stored SPS/PPS before first frame" << std::endl;
	}
	if (av_new_packet(impl->pkt, static_cast<int>(final_size)) < 0) {
		std::cout << "[H264Decoder] Failed to allocate packet of size " << final_size << std::endl;
		return false;
	}
	std::memcpy(impl->pkt->data, final_payload, final_size);

	int ret = avcodec_send_packet(impl->ctx, impl->pkt);
	if (ret < 0) {
		std::cout << "[H264Decoder] avcodec_send_packet failed: " << ret << std::endl;
		avcodec_flush_buffers(impl->ctx);
		return false;
	}

	while (true) {
		ret = avcodec_receive_frame(impl->ctx, impl->frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
		if (ret < 0) {
			std::cout << "[H264Decoder] avcodec_receive_frame failed: " << ret << std::endl;
			avcodec_flush_buffers(impl->ctx);
			return false;
		}

		AVFrame* f = impl->frame;
		out_width = f->width;
		out_height = f->height;
		if (out_width <= 0 || out_height <= 0) {
			std::cout << "[H264Decoder] Invalid frame dimensions: " << out_width << "x" << out_height << std::endl;
			av_frame_unref(impl->frame);
			return false;
		}
		if (out_width > 8192 || out_height > 4320) {
			std::cout << "[H264Decoder] Frame too large: " << out_width << "x" << out_height << std::endl;
			return false; // guard against corrupted sizes
		}
		if (!f->data[0] || !f->data[1] || !f->data[2]) {
			std::cout << "[H264Decoder] Missing plane data" << std::endl;
			return false;
		}
		if (f->linesize[0] <= 0 || f->linesize[1] <= 0 || f->linesize[2] <= 0) {
			std::cout << "[H264Decoder] Invalid linesize" << std::endl;
			return false;
		}

		if (!impl->sws || impl->sws_w != out_width || impl->sws_h != out_height || impl->sws_fmt != static_cast<AVPixelFormat>(f->format)) {
			if (impl->sws) sws_freeContext(impl->sws);
			impl->sws = sws_getContext(
				f->width, f->height, static_cast<AVPixelFormat>(f->format),
				f->width, f->height, AV_PIX_FMT_YUV420P,
				SWS_BILINEAR, nullptr, nullptr, nullptr);
			impl->sws_w = out_width;
			impl->sws_h = out_height;
			impl->sws_fmt = static_cast<AVPixelFormat>(f->format);
			std::cout << "[H264Decoder] Recreated SWS context for " << out_width << "x" << out_height << " fmt=" << f->format << std::endl;
		}
		if (!impl->sws) return false;

		const int y_size = out_width * out_height;
		const int uv_w = (out_width + 1) / 2;
		const int uv_h = (out_height + 1) / 2;
		const int uv_size = uv_w * uv_h;
		out_yuv.resize(y_size + uv_size * 2);

		uint8_t* dst_data[4] = {
			out_yuv.data(),                         // Y
			out_yuv.data() + y_size,                // U
			out_yuv.data() + y_size + uv_size,      // V
			nullptr
		};
		int dst_linesize[4] = {
			out_width,
			uv_w,
			uv_w,
			0
		};

		sws_scale(impl->sws, f->data, f->linesize, 0, out_height, dst_data, dst_linesize);
		av_frame_unref(impl->frame);
		int count = ++frame_counter;
		if (count <= 5 || count % 60 == 0) {
			std::cout << "[H264Decoder] Decoded frame " << out_width << "x" << out_height << " (" << count << ")" << std::endl;
		}
		return true;
	}
	return false;
}

