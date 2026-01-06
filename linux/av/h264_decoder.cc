#include "h264_decoder.h"

#include <memory>
#include <stdexcept>
#include <cstring>
#include <mutex>

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

bool H264Decoder::decode_to_yuv420p(const uint8_t* data,
									size_t size,
									std::vector<uint8_t>& out_yuv,
									int& out_width,
									int& out_height) {
	if (!data || size == 0) return false;
	if (size < 32 || size > 4 * 1024 * 1024) return false; // guard malformed payloads
	bool has_start_code = (size >= 4 && data[0] == 0 && data[1] == 0 && ((data[2] == 0 && data[3] == 1) || data[2] == 1));
	if (!has_start_code) return false; // skip non-Annex B payloads to avoid decoder crashes
	auto* impl = impl_;
	std::lock_guard<std::mutex> lock(impl->mutex);
	av_packet_unref(impl->pkt);
	if (av_new_packet(impl->pkt, static_cast<int>(size)) < 0) return false;
	std::memcpy(impl->pkt->data, data, size);

	int ret = avcodec_send_packet(impl->ctx, impl->pkt);
	if (ret < 0) {
		avcodec_flush_buffers(impl->ctx);
		return false;
	}

	while (true) {
		ret = avcodec_receive_frame(impl->ctx, impl->frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
		if (ret < 0) {
			avcodec_flush_buffers(impl->ctx);
			return false;
		}

		AVFrame* f = impl->frame;
		out_width = f->width;
		out_height = f->height;
		if (out_width <= 0 || out_height <= 0) {
			av_frame_unref(impl->frame);
			return false;
		}
		if (out_width > 8192 || out_height > 4320) return false; // guard against corrupted sizes
		if (!f->data[0] || !f->data[1] || !f->data[2]) return false;
		if (f->linesize[0] <= 0 || f->linesize[1] <= 0 || f->linesize[2] <= 0) return false;

		if (!impl->sws || impl->sws_w != out_width || impl->sws_h != out_height || impl->sws_fmt != static_cast<AVPixelFormat>(f->format)) {
			if (impl->sws) sws_freeContext(impl->sws);
			impl->sws = sws_getContext(
				f->width, f->height, static_cast<AVPixelFormat>(f->format),
				f->width, f->height, AV_PIX_FMT_YUV420P,
				SWS_BILINEAR, nullptr, nullptr, nullptr);
			impl->sws_w = out_width;
			impl->sws_h = out_height;
			impl->sws_fmt = static_cast<AVPixelFormat>(f->format);
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
		return true;
	}
	return false;
}

